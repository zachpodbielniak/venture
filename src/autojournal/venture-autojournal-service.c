/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

struct _VentureAutojournalService {
	GObject parent_instance;
	GWeakRef database;
	GHashTable *saving;
};
static void refund_iface(VenturePostingRuleInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureAutojournalService, venture_autojournal_service, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_POSTING_RULE, refund_iface))

static gboolean
enabled(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "posting_profile") != 0 &&
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "journal") != 0;
}
static gint64
account(VentureDatabase *db, gint64 org, const gchar *code, const gchar *name, VentureAccountKind kind, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) found = NULL;
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	found = venture_database_find_one(db, query, error);
	if (found != NULL) return venture_entity_get_id(found);
	if (error != NULL && *error != NULL) return 0;
	found = VENTURE_ENTITY(venture_account_new());
	g_object_set(found, "organization-id", org, "code", code, "name", name, "kind", kind, "active", TRUE, NULL);
	if (!venture_database_save(db, found, NULL, error)) return 0;
	return venture_entity_get_id(found);
}
VenturePostingProfile *
venture_autojournal_service_profile(VentureAutojournalService *self, gint64 org, GError **error)
{
	static const struct { const gchar *field; const gchar *code; const gchar *name; VentureAccountKind kind; } chart[] = {
		{ "sales-account-id", "4000", "Sales", VENTURE_ACCOUNT_KIND_INCOME },
		{ "fees-account-id", "6000", "Platform fees", VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "tax-account-id", "2100", "Sales tax payable", VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "shipping-income-account-id", "4100", "Shipping income", VENTURE_ACCOUNT_KIND_INCOME },
		{ "shipping-expense-account-id", "6100", "Shipping expense", VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "discounts-account-id", "4200", "Sales discounts", VENTURE_ACCOUNT_KIND_INCOME },
		{ "refunds-account-id", "4300", "Sales refunds", VENTURE_ACCOUNT_KIND_INCOME },
		{ "cogs-account-id", "5000", "Cost of goods sold", VENTURE_ACCOUNT_KIND_EXPENSE },
		{ "inventory-account-id", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET },
		{ "cash-account-id", "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET },
		{ "payable-account-id", "2000", "Accounts payable", VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "default-expense-account-id", "6900", "General expenses", VENTURE_ACCOUNT_KIND_EXPENSE }
	};
	static const struct { const gchar *category; const gchar *code; const gchar *name; } categories[] = {
		{ "ADVERTISING", "6200", "Advertising" }, { "SOFTWARE", "6300", "Software and services" },
		{ "SUPPLIES", "6400", "Supplies" }, { "PROFESSIONAL", "6500", "Professional fees" },
		{ "HOME_OFFICE", "6600", "Home office" }, { "TRAVEL", "6700", "Travel" },
		{ "FEES", "6000", "Platform fees" }, { "SHIPPING", "6100", "Shipping expense" }
	};
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_POSTING_PROFILE);
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(JsonObject) map = json_object_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *json = NULL;
	guint i;
	if (db == NULL || !venture_database_begin(db, error)) return NULL;
	venture_query_set_organization(query, org);
	found = venture_database_find_one(db, query, error);
	if (error != NULL && *error != NULL) goto fail;
	if (found != NULL) goto commit;
	found = VENTURE_ENTITY(venture_posting_profile_new());
	g_object_set(found, "organization-id", org, NULL);
	for (i = 0; i < G_N_ELEMENTS(chart); i++) {
		gint64 id = account(db, org, chart[i].code, chart[i].name, chart[i].kind, error);
		if (id == 0) goto fail;
		g_object_set(found, chart[i].field, id, NULL);
	}
	for (i = 0; i < G_N_ELEMENTS(categories); i++) {
		gint64 id = account(db, org, categories[i].code, categories[i].name, VENTURE_ACCOUNT_KIND_EXPENSE, error);
		if (id == 0) goto fail;
		json_object_set_int_member(map, categories[i].category, id);
	}
	json_node_set_object(node, map);
	json = venture_json_to_string(node, FALSE);
	g_object_set(found, "expense-categories", json, NULL);
	if (!venture_database_save(db, found, NULL, error)) goto fail;
commit:
	if (!venture_database_commit(db, error)) return NULL;
	return VENTURE_POSTING_PROFILE(g_steal_pointer(&found));
fail:
	venture_database_rollback(db);
	return NULL;
}
static gboolean
leg(GPtrArray *rows, VenturePostingProfile *profile, const gchar *field, VentureLedgerSide side,
	const VentureMoney *amount, GError **error)
{
	g_autoptr(VentureMoney) magnitude = NULL;
	VentureJournalLine *row;
	gint64 id;
	if (amount == NULL || amount->amount == 0) return TRUE;
	if (amount->amount == G_MININT64) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Journal amount magnitude overflows");
		return FALSE;
	}
	g_object_get(profile, field, &id, NULL);
	magnitude = venture_money_abs(amount);
	if (amount->amount < 0) side = side == VENTURE_LEDGER_SIDE_DEBIT ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT;
	row = venture_journal_line_new();
	g_object_set(row, "account-id", id, "amount", magnitude, "side", side,
		"organization-id", venture_entity_get_organization_id(VENTURE_ENTITY(profile)), "memo", field, NULL);
	g_ptr_array_add(rows, row);
	return TRUE;
}
static VentureMoney *
amount(VentureEntity *source, const gchar *field, const gchar *currency)
{
	VentureMoney *value = NULL;
	g_object_get(source, field, &value, NULL);
	return value != NULL ? value : venture_money_new_zero(currency);
}
static gboolean
add(VentureMoney **total, const VentureMoney *term, gboolean subtract, GError **error)
{
	VentureMoney *next = subtract ? venture_money_subtract(*total, term, error) : venture_money_add(*total, term, error);
	if (next == NULL) return FALSE;
	venture_money_free(*total); *total = next; return TRUE;
}
static GPtrArray *
refund_lines(VenturePostingRule *rule, VentureDatabase *db, VentureEntity *source, GError **error)
{
	g_autoptr(VenturePostingProfile) profile = NULL;
	g_autoptr(VentureMoney) refund = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	(void)db;
	if (!VENTURE_IS_SALE(source)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Refund posting requires a sale"); return NULL;
	}
	profile = venture_autojournal_service_profile(VENTURE_AUTOJOURNAL_SERVICE(rule), venture_entity_get_organization_id(source), error);
	if (profile == NULL) return NULL;
	refund = amount(source, "refunded", "USD");
	if (!leg(rows, profile, "refunds-account-id", VENTURE_LEDGER_SIDE_DEBIT, refund, error) ||
		!leg(rows, profile, "cash-account-id", VENTURE_LEDGER_SIDE_CREDIT, refund, error)) return NULL;
	return g_steal_pointer(&rows);
}
static const gchar *refund_name(VenturePostingRule *rule) { (void)rule; return "sale_refund"; }
static void refund_iface(VenturePostingRuleInterface *iface) { iface->get_name = refund_name; iface->build_lines = refund_lines; }

G_DECLARE_FINAL_TYPE(AutoRule, auto_rule, AUTO, RULE, GObject)
struct _AutoRule { GObject parent_instance; gchar *name; VenturePostingRule *fallback; };
static void rule_iface(VenturePostingRuleInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(AutoRule, auto_rule, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(VENTURE_TYPE_POSTING_RULE, rule_iface))
static const gchar *rule_name(VenturePostingRule *rule) { return ((AutoRule *)rule)->name; }
static GPtrArray *
rule_lines(VenturePostingRule *rule, VentureDatabase *db, VentureEntity *source, GError **error)
{
	g_autoptr(VenturePostingProfile) profile = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) base = NULL;
	g_autoptr(VentureMoney) cash = NULL;
	const gchar *name = rule_name(rule);
	if (!enabled()) return venture_posting_rule_build_lines(((AutoRule *)rule)->fallback, db, source, error);
	profile = venture_autojournal_service_profile(venture_database_get_autojournal_service(db), venture_entity_get_organization_id(source), error);
	if (profile == NULL) return NULL;
	if (g_str_equal(name, "sale") && VENTURE_IS_SALE(source)) {
		static const struct { const gchar *source; const gchar *account; gboolean debit; } fields[] = {
			{ "fees", "fees-account-id", TRUE }, { "discount", "discounts-account-id", TRUE },
			{ "shipping-cost", "shipping-expense-account-id", TRUE },
			{ "gross", "sales-account-id", FALSE }, { "tax-collected", "tax-account-id", FALSE },
			{ "shipping-collected", "shipping-income-account-id", FALSE }
		};
		g_autoptr(VentureMoney) remitted = NULL;
		gint64 product_id, quantity;
		guint i;
		base = amount(source, "gross", "USD");
		cash = venture_money_new_zero(base->currency);
		for (i = 0; i < G_N_ELEMENTS(fields); i++) {
			g_autoptr(VentureMoney) term = amount(source, fields[i].source, base->currency);
			if (!add(&cash, term, fields[i].debit, error) ||
				!leg(rows, profile, fields[i].account, fields[i].debit ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT, term, error)) return NULL;
		}
		if (!leg(rows, profile, "cash-account-id", VENTURE_LEDGER_SIDE_DEBIT, cash, error)) return NULL;
		remitted = amount(source, "tax-remitted", base->currency);
		/* Validate even a lone remittance's currency against the sale. */
		if (!add(&cash, remitted, TRUE, error) ||
			!leg(rows, profile, "tax-account-id", VENTURE_LEDGER_SIDE_DEBIT, remitted, error) ||
			!leg(rows, profile, "cash-account-id", VENTURE_LEDGER_SIDE_CREDIT, remitted, error)) return NULL;
		g_object_get(source, "product-id", &product_id, "quantity", &quantity, NULL);
		if (product_id > 0 && quantity != 0) {
			g_autoptr(VentureEntity) product = venture_database_get(db, VENTURE_TYPE_PRODUCT, product_id, error);
			g_autoptr(VentureMoney) cost = NULL;
			g_autoptr(VentureMoney) total = NULL;
			if (product == NULL) return NULL;
			g_object_get(product, "cost", &cost, NULL);
			if (cost != NULL) {
				total = venture_money_multiply_rational(cost, quantity, 1, error);
				if (total == NULL || !add(&cash, total, FALSE, error) ||
					!leg(rows, profile, "cogs-account-id", VENTURE_LEDGER_SIDE_DEBIT, total, error) ||
					!leg(rows, profile, "inventory-account-id", VENTURE_LEDGER_SIDE_CREDIT, total, error)) return NULL;
			}
		}
	} else if (g_str_equal(name, "expense") && VENTURE_IS_EXPENSE(source)) {
		g_autofree gchar *category = NULL, *method = NULL, *mapping = NULL, *tax_code = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *map;
		gint64 tax_id, debit = 0;
		g_object_get(source, "category", &category, "payment-method", &method, "tax-category-id", &tax_id, NULL);
		g_object_get(profile, "expense-categories", &mapping, NULL);
		if (mapping == NULL || !json_parser_load_from_data(parser, mapping, -1, error)) return NULL;
		if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expense categories must be a JSON object"); return NULL;
		}
		map = json_node_get_object(json_parser_get_root(parser));
		if (tax_id > 0) {
			g_autoptr(VentureEntity) tax = venture_database_get(db, VENTURE_TYPE_TAX_CATEGORY, tax_id, error);
			if (tax == NULL) return NULL;
			g_object_get(tax, "code", &tax_code, NULL);
		}
		if (tax_code != NULL && json_object_has_member(map, tax_code)) debit = venture_json_object_get_int(map, tax_code, 0);
		if (debit == 0 && category != NULL && json_object_has_member(map, category)) debit = venture_json_object_get_int(map, category, 0);
		if (debit != 0) g_object_set(profile, "default-expense-account-id", debit, NULL);
		base = amount(source, "amount", "USD");
		if (!leg(rows, profile, "default-expense-account-id", VENTURE_LEDGER_SIDE_DEBIT, base, error) ||
			!leg(rows, profile, g_strcmp0(method, "credit") == 0 || g_strcmp0(method, "credit_card") == 0 || g_strcmp0(method, "accounts_payable") == 0 || g_strcmp0(method, "unpaid") == 0 ? "payable-account-id" : "cash-account-id", VENTURE_LEDGER_SIDE_CREDIT, base, error)) return NULL;
	} else {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Wrong source for automatic journal rule"); return NULL;
	}
	/* Zero documents still need balanced evidence for version/backfill tracking. */
	if (rows->len == 0) {
		gint64 id;
		guint i;
		g_object_get(profile, "cash-account-id", &id, NULL);
		for (i = 0; i < 2; i++) {
			VentureJournalLine *row = venture_journal_line_new();
			g_object_set(row, "account-id", id, "amount", base, "side", i == 0 ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT, NULL);
			g_ptr_array_add(rows, row);
		}
	}
	return g_steal_pointer(&rows);
}
static void rule_iface(VenturePostingRuleInterface *iface) { iface->get_name = rule_name; iface->build_lines = rule_lines; }
static void auto_rule_finalize(GObject *object) { AutoRule *self = (AutoRule *)object; g_free(self->name); g_clear_object(&self->fallback); G_OBJECT_CLASS(auto_rule_parent_class)->finalize(object); }
static void auto_rule_class_init(AutoRuleClass *klass) { G_OBJECT_CLASS(klass)->finalize = auto_rule_finalize; }
static void auto_rule_init(AutoRule *self) { (void)self; }

static void service_set(GObject *object, guint id, const GValue *value, GParamSpec *spec) {
	if (id == 1) g_weak_ref_set(&VENTURE_AUTOJOURNAL_SERVICE(object)->database, g_value_get_object(value));
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void service_get(GObject *object, guint id, GValue *value, GParamSpec *spec) {
	if (id == 1) g_value_take_object(value, g_weak_ref_get(&VENTURE_AUTOJOURNAL_SERVICE(object)->database));
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void service_finalize(GObject *object) {
	VentureAutojournalService *self = VENTURE_AUTOJOURNAL_SERVICE(object);
	g_weak_ref_clear(&self->database); g_hash_table_unref(self->saving);
	G_OBJECT_CLASS(venture_autojournal_service_parent_class)->finalize(object);
}
static void venture_autojournal_service_class_init(VentureAutojournalServiceClass *klass) {
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->set_property = service_set; object->get_property = service_get; object->finalize = service_finalize;
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database", "Weak owning database", VENTURE_TYPE_DATABASE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_autojournal_service_init(VentureAutojournalService *self) {
	g_weak_ref_init(&self->database, NULL); self->saving = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
VentureAutojournalService *
venture_database_get_autojournal_service(VentureDatabase *db)
{
	VenturePostingRuleRegistry *registry = venture_posting_service_get_rules(venture_database_get_posting_service(db));
	VenturePostingRule *existing = venture_posting_rule_registry_lookup(registry, "sale_refund");
	VentureAutojournalService *self;
	static const gchar *const names[] = { "sale", "expense" };
	guint i;
	if (existing != NULL && VENTURE_IS_AUTOJOURNAL_SERVICE(existing)) return VENTURE_AUTOJOURNAL_SERVICE(existing);
	self = g_object_new(VENTURE_TYPE_AUTOJOURNAL_SERVICE, "database", db, NULL);
	venture_posting_rule_registry_add(registry, VENTURE_POSTING_RULE(self));
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		AutoRule *rule;
		existing = venture_posting_rule_registry_lookup(registry, names[i]);
		/* A plugin's explicit policy already overrides the built-in. */
		if (existing == NULL || g_strcmp0(G_OBJECT_TYPE_NAME(existing), "VentureBuiltinPostingRule") != 0) continue;
		rule = g_object_new(auto_rule_get_type(), NULL);
		rule->name = g_strdup(names[i]); rule->fallback = g_object_ref(existing);
		venture_posting_rule_registry_add(registry, VENTURE_POSTING_RULE(rule));
	}
	return self;
}
static gboolean
post_refund(VentureAutojournalService *self, VentureEntity *source, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	VenturePostingService *posting = venture_database_get_posting_service(db);
	g_autoptr(VentureMoney) refund = amount(source, "refunded", "USD");
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureJournal) header = NULL, posted = NULL;
	g_autofree gchar *posting_key = NULL;
	g_autoptr(GDateTime) when = NULL;
	VentureEntity *last = NULL;
	guint i;
	/* Incomplete operational sales may have neither money nor an organization. */
	if (refund->amount == 0 && venture_entity_get_organization_id(source) <= 0) return TRUE;
	history = venture_posting_service_find_source(posting, "sale", venture_entity_get_id(source), venture_entity_get_organization_id(source), error);
	if (history == NULL) return FALSE;
	for (i = 0; i < history->len; i++) {
		VentureEntity *journal = g_ptr_array_index(history, i);
		g_autofree gchar *rule = NULL;
		VentureJournalState state;
		g_object_get(journal, "rule-name", &rule, "state", &state, NULL);
		if (g_strcmp0(rule, "sale_refund") == 0 && state == VENTURE_JOURNAL_POSTED) last = journal;
	}
	if (last == NULL && refund->amount == 0) return TRUE;
	g_object_get(source, "refunded-at", &when, NULL);
	if (when == NULL) g_object_get(source, "occurred-at", &when, NULL);
	if (when == NULL) when = g_date_time_ref(venture_entity_get_created_at(source));
	if (last != NULL) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(VentureEntity) row = NULL;
		g_autoptr(VentureMoney) old = NULL;
		g_autoptr(VentureJournal) reversal = NULL;
		VentureLedgerSide side;
		venture_query_set_organization(query, venture_entity_get_organization_id(source));
		venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(last), NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		row = venture_database_find_one(db, query, error);
		if (row == NULL) return FALSE;
		g_object_get(row, "amount", &old, "side", &side, NULL);
		if (side == VENTURE_LEDGER_SIDE_CREDIT) {
			VentureMoney *negative = venture_money_negate(old);
			if (negative == NULL) return FALSE;
			venture_money_free(old); old = negative;
		}
		if (venture_money_equal(old, refund)) return TRUE;
		reversal = venture_posting_service_reverse(posting, venture_entity_get_id(last), when, "Refund correction", actor, error);
		if (reversal == NULL) return FALSE;
	}
	if (refund->amount == 0) return TRUE;
	rows = refund_lines(VENTURE_POSTING_RULE(self), db, source, error);
	if (rows == NULL) return FALSE;
	header = venture_journal_new();
	posting_key = g_strdup_printf("sale:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":sale_refund", venture_entity_get_id(source), venture_entity_get_version(source));
	g_object_set(header, "posting-key", posting_key, "organization-id", venture_entity_get_organization_id(source), "source-type", "sale",
		"source-id", venture_entity_get_id(source), "source-version", venture_entity_get_version(source),
		"rule-name", "sale_refund", "occurred-at", when, "currency", refund->currency, NULL);
	posted = venture_posting_service_post(posting, header, rows, NULL, actor, error);
	return posted != NULL;
}

gboolean
venture_autojournal_save_hook(VentureDatabase *db, VentureEntity *entity, const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureAutojournalService *self;
	g_autoptr(VentureEntity) copy = NULL;
	g_autofree gchar *uuid = NULL;
	*handled = FALSE;
	if ((!VENTURE_IS_SALE(entity) && !VENTURE_IS_EXPENSE(entity)) || !enabled() ||
		venture_receivables_is_projection_write(db, entity)) return TRUE;
	self = venture_database_get_autojournal_service(db);
	uuid = g_strdup(venture_entity_get_uuid(entity));
	if (g_hash_table_contains(self->saving, uuid)) return TRUE;
	*handled = TRUE;
	if (!venture_database_begin(db, error)) return FALSE;
	g_hash_table_add(self->saving, g_strdup(uuid));
	copy = g_object_new(G_OBJECT_TYPE(entity), NULL);
	venture_entity_copy_properties_from(copy, entity, FALSE);
	if (!venture_database_save(db, copy, actor, error) ||
		(VENTURE_IS_SALE(copy) && !post_refund(self, copy, actor, error))) goto fail;
	g_hash_table_remove(self->saving, uuid);
	if (!venture_database_commit(db, error)) return FALSE;
	venture_entity_copy_properties_from(entity, copy, FALSE);
	return TRUE;
fail:
	g_hash_table_remove(self->saving, uuid);
	venture_database_rollback(db);
	return FALSE;
}

static GDateTime *
source_date(VentureEntity *source)
{
	GDateTime *when = NULL;
	g_object_get(source, "occurred-at", &when, NULL);
	return when != NULL ? when : g_date_time_ref(venture_entity_get_created_at(source));
}
static gint
source_order(gconstpointer a, gconstpointer b)
{
	VentureEntity *left = *(VentureEntity *const *)a, *right = *(VentureEntity *const *)b;
	g_autoptr(GDateTime) first = source_date(left), second = source_date(right);
	gint order = g_date_time_compare(first, second);
	if (order != 0) return order;
	order = g_strcmp0(venture_entity_get_entity_name(left), venture_entity_get_entity_name(right));
	if (order != 0) return order;
	return venture_entity_get_id(left) < venture_entity_get_id(right) ? -1 : venture_entity_get_id(left) > venture_entity_get_id(right);
}
GPtrArray *
venture_autojournal_service_unposted(VentureAutojournalService *self, gint64 org, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree GType *types = NULL;
	VenturePostingService *posting;
	guint n, i;
	if (!enabled() || org <= 0) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Autojournal requires an enabled module and an organization"); return NULL;
	}
	posting = venture_database_get_posting_service(db);
	types = venture_entity_registry_list_types(venture_entity_registry_get_default(), &n);
	for (i = 0; i < n; i++) {
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) sources = NULL;
		guint j;
		if (!g_type_is_a(types[i], VENTURE_TYPE_SALE) && !g_type_is_a(types[i], VENTURE_TYPE_EXPENSE)) continue;
		query = venture_query_new(types[i]);
		venture_query_set_organization(query, org); venture_query_set_limit(query, 0);
		sources = venture_database_find(db, query, error);
		if (sources == NULL) return NULL;
		for (j = 0; j < sources->len; j++) {
			VentureEntity *source = g_ptr_array_index(sources, j);
			g_autoptr(GPtrArray) journals = NULL;
			g_autoptr(GError) projection = NULL;
			gboolean posted = FALSE;
			guint k;
			if (VENTURE_IS_SALE(source) && !venture_receivables_check_sale(db, source, &projection)) {
				if (g_error_matches(projection, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED)) continue;
				g_propagate_error(error, g_steal_pointer(&projection)); return NULL;
			}
			journals = venture_posting_service_find_source(posting, venture_entity_get_entity_name(source), venture_entity_get_id(source), org, error);
			if (journals == NULL) return NULL;
			for (k = 0; k < journals->len; k++) {
				gint64 version;
				VentureJournalState state;
				g_autofree gchar *rule = NULL;
				g_object_get(g_ptr_array_index(journals, k), "source-version", &version, "state", &state, "rule-name", &rule, NULL);
				if (version == venture_entity_get_version(source) && state == VENTURE_JOURNAL_POSTED &&
					g_strcmp0(rule, venture_entity_get_entity_name(source)) == 0) posted = TRUE;
			}
			if (!posted) g_ptr_array_add(result, g_object_ref(source));
		}
	}
	g_ptr_array_sort(result, source_order);
	return g_steal_pointer(&result);
}
JsonNode *
venture_autojournal_service_backfill(VentureAutojournalService *self, gint64 org, gboolean dry_run, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = g_weak_ref_get(&self->database);
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(JsonNode) result = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) counts = json_object_new();
	VenturePostingService *posting = venture_database_get_posting_service(db);
	guint i, posted_count = 0, skipped = 0;
	if (!venture_database_begin(db, error)) return NULL;
	sources = venture_autojournal_service_unposted(self, org, error);
	if (sources == NULL) goto fail;
	for (i = 0; i < sources->len; i++) {
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(GPtrArray) history = NULL;
		g_autoptr(GDateTime) when = source_date(source);
		g_autoptr(VentureJournal) posted = NULL;
		guint j;
		g_object_get(source, VENTURE_IS_SALE(source) ? "gross" : "amount", &value, NULL);
		if (value == NULL) { skipped++; continue; }
		history = venture_posting_service_find_source(posting, venture_entity_get_entity_name(source), venture_entity_get_id(source), org, error);
		if (history == NULL) goto fail;
		for (j = 0; j < history->len; j++) {
			VentureEntity *old = g_ptr_array_index(history, j);
			g_autofree gchar *rule = NULL;
			VentureJournalState state;
			g_object_get(old, "rule-name", &rule, "state", &state, NULL);
			if (state == VENTURE_JOURNAL_POSTED && g_strcmp0(rule, venture_entity_get_entity_name(source)) == 0) {
				g_autoptr(VentureJournal) reversal = NULL;
				g_autoptr(GDateTime) old_when = NULL;
				g_object_get(old, "occurred-at", &old_when, NULL);
				reversal = venture_posting_service_reverse(posting, venture_entity_get_id(old),
					g_date_time_compare(when, old_when) < 0 ? old_when : when, "Backfill current source version", actor, error);
				if (reversal == NULL) goto fail;
			}
		}
		posted = venture_posting_service_post_document(posting, venture_entity_get_entity_name(source), source, actor, error);
		if (posted == NULL || (VENTURE_IS_SALE(source) && !post_refund(self, source, actor, error))) goto fail;
		posted_count++;
	}
	json_object_set_int_member(counts, "candidates", sources->len);
	json_object_set_int_member(counts, "posted", posted_count);
	json_object_set_int_member(counts, "skipped", skipped);
	json_object_set_boolean_member(counts, "dry_run", dry_run);
	json_node_set_object(result, counts);
	if (dry_run) venture_database_rollback(db);
	else if (!venture_database_commit(db, error)) return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(db);
	return NULL;
}
