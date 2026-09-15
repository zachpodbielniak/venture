/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct _VentureBankMatchService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureBankMatchService, venture_bank_match_service, G_TYPE_OBJECT)

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureBankMatchService *self = VENTURE_BANK_MATCH_SERVICE(object);
	if (id == 1) g_value_set_object(value, self->database);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureBankMatchService *self = VENTURE_BANK_MATCH_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureBankMatchService *self = VENTURE_BANK_MATCH_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_bank_match_service_parent_class)->finalize(object);
}

static void
venture_bank_match_service_class_init(VentureBankMatchServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_bank_match_service_init(VentureBankMatchService *self)
{
	(void)self;
}

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"VentureBankMatchService: %s", message);
	return FALSE;
}

gboolean
venture_bank_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name = venture_entity_get_entity_name(record);
	VentureBankMatchService *self;
	if (!g_str_has_prefix(name, "bank_") && strcmp(name, "reconciliation"))
		return TRUE;
	self = venture_database_get_bank_match_service(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	if (!strcmp(name, "bank_account") && !removal)
	{
		g_autoptr(VentureEntity) account = NULL;
		g_autofree gchar *currency = NULL;
		gint64 id = 0;
		g_autoptr(VentureEntity) previous = NULL;
		g_autoptr(VentureMoney) supplied = NULL;
		g_autoptr(GDateTime) supplied_date = NULL;
		g_object_get(record, "last-statement-balance", &supplied, "last-statement-date", &supplied_date, NULL);
		if (venture_entity_is_persisted(record))
		{
			g_autoptr(JsonNode) diff = NULL;
			JsonObject *changes;
			previous = venture_database_get(database, VENTURE_TYPE_BANK_ACCOUNT, venture_entity_get_id(record), error);
			if (previous == NULL) return FALSE;
			diff = venture_entity_diff(previous, record);
			changes = json_node_get_object(diff);
			if (json_object_has_member(changes, "last_statement_balance") || json_object_has_member(changes, "last_statement_date") ||
				json_object_has_member(changes, "account_id") || json_object_has_member(changes, "currency") ||
				json_object_has_member(changes, "last_inbox") || json_object_has_member(changes, "last_mapping_preview") ||
				venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(record))
				return refuse(error, "bank identity and statement balances are service-owned");
		}
		else if (supplied != NULL || supplied_date != NULL)
			return refuse(error, "statement balances are derived by import");
		g_object_get(record, "account-id", &id, "currency", &currency, NULL);
		account = venture_database_get(database, VENTURE_TYPE_ACCOUNT, id, error);
		if (account == NULL) return FALSE;
		if (venture_entity_get_organization_id(account) != venture_entity_get_organization_id(record))
			return refuse(error, "ledger account belongs to another organization");
		if (!venture_currency_is_valid(currency))
			return refuse(error, "bank currency is required");
		return TRUE;
	}
	if (!strcmp(name, "bank_connection"))
		return TRUE;
	if (!strcmp(name, "bank_rule") && !removal)
	{
		gboolean enabled = FALSE;
		g_autofree gchar *preview = NULL;
		g_object_get(record, "enabled", &enabled, "last-preview", &preview, NULL);
		if (venture_entity_is_persisted(record))
		{
			g_autoptr(JsonNode) diff = NULL;
			JsonObject *changes;
			g_autoptr(VentureEntity) previous = venture_database_get(database, VENTURE_TYPE_BANK_RULE,
				venture_entity_get_id(record), error);
			if (previous == NULL) return FALSE;
			diff = venture_entity_diff(previous, record);
			changes = json_node_get_object(diff);
			if (json_object_has_member(changes, "last_preview"))
				return refuse(error, "rule preview is service-owned");
		}
		else if (preview != NULL && *preview)
			return refuse(error, "rule preview is service-owned");
		if (enabled && (preview == NULL || *preview == '\0'))
			return refuse(error, "preview historical rows before enabling a rule");
		return TRUE;
	}
	return refuse(error, "statement evidence must be changed through the service");
}

static gint64
number(VentureEntity *record, const gchar *field)
{
	gint64 value = 0;
	g_object_get(record, field, &value, NULL);
	return value;
}

static GPtrArray *
rows(VentureDatabase *db, GType type, gint64 org, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, org);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	return venture_database_find(db, query, error);
}

static VentureEntity *
new_record(GType type, gint64 org)
{
	VentureEntity *record = g_object_new(type, NULL);
	venture_entity_set_organization_id(record, org);
	return record;
}

static gboolean
save_owned(VentureBankMatchService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static const gchar *
option(JsonObject *args, const gchar *name)
{
	JsonNode *node = args != NULL ? json_object_get_member(args, name) : NULL;
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : NULL;
}

static gint64
option_id(JsonObject *args, const gchar *field)
{
	JsonNode *node = args != NULL ? json_object_get_member(args, field) : NULL;
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64
		? json_node_get_int(node) : 0;
}

static GDateTime *
date_parse(const gchar *text)
{
	g_autofree gchar *iso = NULL;
	if (text == NULL) return NULL;
	iso = strlen(text) == 10 ? g_strconcat(text, "T00:00:00Z", NULL) : g_strdup(text);
	return g_date_time_new_from_iso8601(iso, NULL);
}

static gboolean
state_is(VentureEntity *record, const gchar *wanted)
{
	g_autofree gchar *state = NULL;
	g_object_get(record, "state", &state, NULL);
	return g_strcmp0(state, wanted) == 0;
}

static gboolean
unlocked(VentureBankMatchService *self, VentureEntity *transaction, GError **error)
{
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GDateTime) date = NULL;
	guint i;
	found = rows(self->database, VENTURE_TYPE_RECONCILIATION, venture_entity_get_organization_id(transaction),
		"bank-account-id", number(transaction, "bank-account-id"), error);
	if (found == NULL) return FALSE;
	g_object_get(transaction, "date", &date, NULL);
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *rec = g_ptr_array_index(found, i);
		g_autoptr(GDateTime) end = NULL;
		g_object_get(rec, "period-end", &end, NULL);
		if (state_is(rec, "reconciled") && date != NULL && end != NULL && g_date_time_compare(date, end) <= 0)
			return refuse(error, "reconciled transactions and matches are immutable");
	}
	return TRUE;
}

/* An adapter selects only registered cash documents. New vendor payment
 * records participate by the public record name and amount/date metadata. */
VentureMoney *
venture_bank_candidate_amount(VentureEntity *record)
{
	const gchar *name = venture_entity_get_entity_name(record);
	VentureMoney *amount = NULL;
	if (!strcmp(name, "sale")) return venture_sale_get_net(VENTURE_SALE(record), NULL);
	if (strcmp(name, "expense") && strcmp(name, "payment") && strcmp(name, "vendor_bill_payment") &&
		strcmp(name, "bill_payment") && strcmp(name, "refund") && strcmp(name, "processor_payout"))
		return NULL;
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "amount") == NULL)
		return NULL;
	g_object_get(record, "amount", &amount, NULL);
	if (amount != NULL && strcmp(name, "payment") && strcmp(name, "processor_payout"))
	{
		VentureMoney *negative = venture_money_multiply_int(amount, -1, NULL);
		venture_money_free(amount);
		amount = negative;
	}
	return amount;
}

GDateTime *
venture_bank_candidate_date(VentureEntity *record)
{
	GDateTime *date = NULL;
	const gchar *field = g_object_class_find_property(G_OBJECT_GET_CLASS(record), "occurred-at") != NULL
		? "occurred-at" : "date";
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), field) != NULL)
		g_object_get(record, field, &date, NULL);
	return date;
}

static gint
calendar_distance(GDateTime *a, GDateTime *b)
{
	g_autoptr(GDateTime) utc_a = g_date_time_to_utc(a);
	g_autoptr(GDateTime) utc_b = g_date_time_to_utc(b);
	GDate day_a, day_b;
	g_date_clear(&day_a, 1);
	g_date_clear(&day_b, 1);
	g_date_set_dmy(&day_a, g_date_time_get_day_of_month(utc_a), g_date_time_get_month(utc_a), g_date_time_get_year(utc_a));
	g_date_set_dmy(&day_b, g_date_time_get_day_of_month(utc_b), g_date_time_get_month(utc_b), g_date_time_get_year(utc_b));
	return (gint)g_date_get_julian(&day_a) - (gint)g_date_get_julian(&day_b);
}

static gint
match_window(VentureDatabase *db, VentureEntity *transaction)
{
	g_autoptr(VentureEntity) bank = NULL;
	gint64 days = 0;
	bank = venture_database_get(db, VENTURE_TYPE_BANK_ACCOUNT, number(transaction, "bank-account-id"), NULL);
	if (bank != NULL)
		g_object_get(bank, "match-window-days", &days, NULL);
	return days > 0 && days < 366 ? (gint)days : 5;
}

static gboolean
token_match(VentureEntity *record, const gchar *search)
{
	g_autofree gchar *description = NULL;
	g_autofree gchar *reference = NULL;
	g_autofree gchar *needle = NULL;
	if (search == NULL || *search == '\0') return TRUE;
	needle = g_utf8_casefold(search, -1);
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "description") != NULL)
		g_object_get(record, "description", &description, NULL);
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "reference") != NULL)
		g_object_get(record, "reference", &reference, NULL);
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "memo") != NULL && description == NULL)
		g_object_get(record, "memo", &description, NULL);
	{
		g_autofree gchar *hay = g_utf8_casefold(description != NULL ? description : "", -1);
		g_autofree gchar *ref = g_utf8_casefold(reference != NULL ? reference : "", -1);
		return (hay != NULL && strstr(hay, needle) != NULL) || (ref != NULL && strstr(ref, needle) != NULL);
	}
}

static VentureMoney *
journal_bank_amount(VentureDatabase *db, VentureEntity *journal, gint64 ledger_id, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	lines = rows(db, VENTURE_TYPE_JOURNAL_LINE, venture_entity_get_organization_id(journal),
		"journal-id", venture_entity_get_id(journal), error);
	if (lines == NULL) return NULL;
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureMoney) amount = NULL;
		gint side = 0;
		if (number(line, "account-id") != ledger_id) continue;
		g_object_get(line, "amount", &amount, "side", &side, NULL);
		if (amount == NULL) continue;
		if (total == NULL) total = venture_money_new_zero(venture_money_get_currency(amount));
		if (side == VENTURE_LEDGER_SIDE_CREDIT)
		{
			VentureMoney *neg = venture_money_multiply_int(amount, -1, error);
			if (neg == NULL) return NULL;
			g_clear_pointer(&amount, venture_money_free);
			amount = neg;
		}
		{
			VentureMoney *next = venture_money_add(total, amount, error);
			if (next == NULL) return NULL;
			g_clear_pointer(&total, venture_money_free);
			total = next;
		}
	}
	return g_steal_pointer(&total);
}

static VentureMoney *
document_amount(VentureDatabase *db, VentureEntity *record, gint64 ledger_id, GError **error)
{
	if (!strcmp(venture_entity_get_entity_name(record), "journal"))
		return journal_bank_amount(db, record, ledger_id, error);
	(void)error;
	return venture_bank_candidate_amount(record);
}

GPtrArray *
venture_bank_transaction_candidates_search(VentureDatabase *db, VentureEntity *transaction, const gchar *search, GError **error)
{
	g_auto(GStrv) names = venture_entity_registry_list_names(venture_entity_registry_get_default());
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GPtrArray) matches = NULL;
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureEntity) bank = NULL;
	gint64 org = venture_entity_get_organization_id(transaction);
	gint64 ledger = 0;
	gint window;
	guint i, j;
	g_object_get(transaction, "amount", &amount, "date", &date, NULL);
	if (org <= 0 || amount == NULL || date == NULL)
	{
		refuse(error, "candidate search needs organization, amount and date");
		return NULL;
	}
	window = match_window(db, transaction);
	bank = venture_database_get(db, VENTURE_TYPE_BANK_ACCOUNT, number(transaction, "bank-account-id"), error);
	if (bank == NULL) return NULL;
	ledger = number(bank, "account-id");
	matches = rows(db, VENTURE_TYPE_BANK_MATCH, org, NULL, 0, error);
	if (matches == NULL) return NULL;
	for (i = 0; names[i] != NULL; i++)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		g_autoptr(GPtrArray) records = NULL;
		/* Probe the record contract without querying unrelated tables. */
		if (strcmp(names[i], "sale") && strcmp(names[i], "expense") && strcmp(names[i], "payment") &&
			strcmp(names[i], "vendor_bill_payment") && strcmp(names[i], "bill_payment") &&
			strcmp(names[i], "refund") && strcmp(names[i], "processor_payout") &&
			strcmp(names[i], "journal")) continue;
		records = rows(db, type, org, NULL, 0, error);
		if (records == NULL) return NULL;
		for (j = 0; j < records->len; j++)
		{
			VentureEntity *record = g_ptr_array_index(records, j);
			g_autoptr(VentureMoney) value = document_amount(db, record, ledger, error);
			g_autoptr(GDateTime) when = venture_bank_candidate_date(record);
			g_autoptr(VentureMoney) delta = NULL;
			gboolean used = FALSE;
			guint k;
			if (error != NULL && *error != NULL) return NULL;
			if (value == NULL || when == NULL || strcmp(venture_money_get_currency(value), venture_money_get_currency(amount))) continue;
			if (!strcmp(names[i], "journal"))
			{
				g_autofree gchar *source_type = NULL;
				g_object_get(record, "source-type", &source_type, NULL);
				if (source_type != NULL && strcmp(source_type, "organization") && strcmp(source_type, "journal") &&
					strcmp(source_type, "bank_match") && strcmp(source_type, "processor_payout"))
					continue;
			}
			if (calendar_distance(when, date) > window || calendar_distance(when, date) < -window) continue;
			if (!token_match(record, search)) continue;
			delta = venture_money_subtract(value, amount, error);
			if (delta == NULL) return NULL;
			/* Near means at most 100 minor units, never a percentage float. */
			if (venture_money_get_amount(delta) > 100 || venture_money_get_amount(delta) < -100) continue;
			for (k = 0; k < matches->len; k++)
			{
				VentureEntity *match = g_ptr_array_index(matches, k);
				g_autofree gchar *record_type = NULL;
				g_object_get(match, "record-type", &record_type, NULL);
				if (!g_strcmp0(record_type, names[i]) && number(match, "record-id") == venture_entity_get_id(record)) used = TRUE;
			}
			if (!used) g_ptr_array_add(result, g_object_ref(record));
		}
	}
	return g_steal_pointer(&result);
}

GPtrArray *
venture_bank_transaction_candidates(VentureDatabase *db, VentureEntity *transaction, GError **error)
{
	return venture_bank_transaction_candidates_search(db, transaction, NULL, error);
}

static gboolean
post_adjustment(VentureBankMatchService *self, VentureEntity *transaction, VentureEntity *match,
	const gchar *description, gint64 offset, const VentureMoney *contribution, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureEntity) bank = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureMoney) positive = NULL;
	g_autofree gchar *transaction_id = NULL;
	gint64 ledger, org = venture_entity_get_organization_id(transaction);
	gint64 debit, credit;
	if (offset <= 0) return refuse(error, "an adjustment requires an offset account");
	bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, number(transaction, "bank-account-id"), error);
	if (bank == NULL) return FALSE;
	ledger = number(bank, "account-id");
	g_object_get(transaction, "date", &date, NULL);
	positive = venture_money_get_amount(contribution) < 0 ?
		venture_money_multiply_int(contribution, -1, error) : venture_money_copy(contribution);
	if (positive == NULL) return FALSE;
	debit = venture_money_get_amount(contribution) < 0 ? offset : ledger;
	credit = venture_money_get_amount(contribution) < 0 ? ledger : offset;
	transaction_id = g_strdup_printf("banking:adjustment:%s", venture_entity_get_uuid(match));
	{
		VentureLedgerEntry *debit_entry = venture_ledger_entry_new();
		VentureLedgerEntry *credit_entry = venture_ledger_entry_new();
		g_object_set(debit_entry, "transaction-id", transaction_id, "account-id", debit,
			"side", VENTURE_LEDGER_SIDE_DEBIT, "amount", positive, "occurred-at", date,
			"source-type", "bank_match", "source-id", venture_entity_get_id(match), NULL);
		g_object_set(credit_entry, "transaction-id", transaction_id, "account-id", credit,
			"side", VENTURE_LEDGER_SIDE_CREDIT, "amount", positive, "occurred-at", date,
			"source-type", "bank_match", "source-id", venture_entity_get_id(match), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(debit_entry), org);
		venture_entity_set_organization_id(VENTURE_ENTITY(credit_entry), org);
		(void)description;
		g_ptr_array_add(entries, debit_entry);
		g_ptr_array_add(entries, credit_entry);
	}
	return venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
		entries, NULL, actor, error);
}

static gboolean
match_records(VentureBankMatchService *self, VentureEntity *transaction, JsonArray *parts, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) sum = NULL;
	g_autoptr(GPtrArray) pending = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureEntity) bank = NULL;
	guint i;
	gint64 first = 0, org = venture_entity_get_organization_id(transaction);
	gint64 ledger = 0;
	if (!unlocked(self, transaction, error)) return FALSE;
	if (!state_is(transaction, "unmatched") || parts == NULL || json_array_get_length(parts) == 0)
		return refuse(error, "matching requires an unmatched transaction and at least one record");
	g_object_get(transaction, "amount", &amount, NULL);
	sum = venture_money_new_zero(venture_money_get_currency(amount));
	bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, number(transaction, "bank-account-id"), error);
	if (bank == NULL) return FALSE;
	ledger = number(bank, "account-id");
	for (i = 0; i < json_array_get_length(parts); i++)
	{
		JsonNode *node = json_array_get_element(parts, i);
		JsonObject *part;
		const gchar *name;
		GType type;
		g_autoptr(VentureEntity) target = NULL;
		g_autoptr(VentureEntity) match = NULL;
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(VentureMoney) contribution = NULL;
		g_autoptr(VentureMoney) remaining = NULL;
		g_autoptr(VentureMoney) next = NULL;
		g_autoptr(GPtrArray) existing = NULL;
		guint j;
		if (!JSON_NODE_HOLDS_OBJECT(node)) return refuse(error, "match parts must be objects");
		part = json_node_get_object(node);
		name = option(part, "type");
		if (name == NULL) return refuse(error, "match requires type");
		if (!strcmp(name, "adjustment"))
		{
			g_autoptr(GDateTime) cleared = NULL;
			if (option(part, "amount") == NULL) return refuse(error, "an adjustment requires an amount");
			contribution = venture_money_from_string(option(part, "amount"), venture_money_get_currency(amount), error);
			if (contribution == NULL) return FALSE;
			if (strcmp(venture_money_get_currency(contribution), venture_money_get_currency(amount)))
				return refuse(error, "match crosses currencies");
			if (venture_money_is_zero(contribution)) return refuse(error, "an adjustment must change cash");
			next = venture_money_add(sum, contribution, error);
			if (next == NULL) return FALSE;
			g_clear_pointer(&sum, venture_money_free);
			sum = g_steal_pointer(&next);
			match = new_record(VENTURE_TYPE_BANK_MATCH, org);
			g_object_get(transaction, "date", &cleared, NULL);
			g_object_set(match, "transaction-id", venture_entity_get_id(transaction), "record-type", "adjustment",
				"record-id", option_id(part, "account_id") != 0 ? option_id(part, "account_id") : option_id(part, "id"),
				"amount", contribution, "kind", "adjustment",
				"created-by", actor != NULL ? actor->name : "system", "cleared-at", cleared, NULL);
			g_ptr_array_add(pending, g_steal_pointer(&match));
			continue;
		}
		if (!json_object_has_member(part, "id")) return refuse(error, "match requires type and id");
		type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
		if (type == G_TYPE_INVALID) return refuse(error, "match record type is unavailable");
		target = venture_database_get(self->database, type, option_id(part, "id"), error);
		if (target == NULL) return FALSE;
		/* A proposal may outlive its document. Historical get() results do
		 * not authorize creating new reconciliation evidence for deleted rows. */
		if (venture_entity_is_deleted(target)) return refuse(error, "match document has been deleted");
		if (venture_entity_get_organization_id(target) != org) return refuse(error, "match crosses organizations");
		value = document_amount(self->database, target, ledger, error);
		if (value == NULL) return refuse(error, "record is not a supported cash document");
		contribution = option(part, "amount") != NULL ? venture_money_from_string(option(part, "amount"), venture_money_get_currency(value), error) : venture_money_copy(value);
		if (contribution == NULL) return FALSE;
		if (strcmp(venture_money_get_currency(contribution), venture_money_get_currency(value))) return refuse(error, "match crosses currencies");
		if ((venture_money_get_amount(value) >= 0 && (venture_money_get_amount(contribution) <= 0 || venture_money_compare(contribution, value) > 0)) ||
			(venture_money_get_amount(value) < 0 && (venture_money_get_amount(contribution) >= 0 || venture_money_compare(contribution, value) < 0)))
			return refuse(error, "match amount exceeds the document");
		existing = rows(self->database, VENTURE_TYPE_BANK_MATCH, org, "record-id", venture_entity_get_id(target), error);
		if (existing == NULL) return FALSE;
		remaining = venture_money_copy(value);
		for (j = 0; j < existing->len + pending->len; j++)
		{
			VentureEntity *old = j < existing->len ? g_ptr_array_index(existing, j) : g_ptr_array_index(pending, j - existing->len);
			g_autofree gchar *old_type = NULL;
			g_object_get(old, "record-type", &old_type, NULL);
			if (!g_strcmp0(old_type, name) && number(old, "record-id") == venture_entity_get_id(target))
			{
				g_autoptr(VentureMoney) used = NULL;
				VentureMoney *available;
				g_object_get(old, "amount", &used, NULL);
				available = venture_money_subtract(remaining, used, error);
				if (available == NULL) return FALSE;
				g_clear_pointer(&remaining, venture_money_free);
				remaining = available;
			}
		}
		if ((venture_money_get_amount(value) > 0 && venture_money_compare(contribution, remaining) > 0) ||
			(venture_money_get_amount(value) < 0 && venture_money_compare(contribution, remaining) < 0))
			return refuse(error, "match exceeds the document's remaining unmatched amount");
		next = venture_money_add(sum, contribution, error);
		if (next == NULL) return FALSE;
		g_clear_pointer(&sum, venture_money_free);
		sum = g_steal_pointer(&next);
		match = new_record(VENTURE_TYPE_BANK_MATCH, org);
		{
			g_autoptr(GDateTime) cleared = NULL;
			g_object_get(transaction, "date", &cleared, NULL);
			g_object_set(match, "transaction-id", venture_entity_get_id(transaction), "record-type", name,
				"record-id", venture_entity_get_id(target), "amount", contribution,
				"kind", json_array_get_length(parts) > 1 ? "split" : venture_money_equal(value, contribution) ? "exact" : "partial",
				"created-by", actor != NULL ? actor->name : "system", "cleared-at", cleared, NULL);
		}
		g_ptr_array_add(pending, g_steal_pointer(&match));
	}
	if (!venture_money_equal(sum, amount)) return refuse(error, "split amounts must equal the statement transaction");
	for (i = 0; i < pending->len; i++)
	{
		VentureEntity *match = g_ptr_array_index(pending, i);
		g_autofree gchar *kind = NULL;
		g_autoptr(VentureMoney) contribution = NULL;
		if (!save_owned(self, match, actor, error)) return FALSE;
		g_object_get(match, "kind", &kind, "amount", &contribution, NULL);
		if (!g_strcmp0(kind, "adjustment") &&
			!post_adjustment(self, transaction, match, "bank adjustment", number(match, "record-id"), contribution, actor, error))
			return FALSE;
		if (i == 0) first = venture_entity_get_id(match);
	}
	g_object_set(transaction, "state", "matched", "match-id", first, NULL);
	return save_owned(self, transaction, actor, error);
}

static GPtrArray *
parse_csv(const gchar *data, GError **error)
{
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	g_autoptr(GPtrArray) row = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) field = g_string_new(NULL);
	gboolean quoted = FALSE, closed = FALSE;
	const gchar *p;
	for (p = data; ; p++)
	{
		gchar c = *p;
		if (quoted)
		{
			if (c == '\0') { refuse(error, "unterminated CSV quote"); return NULL; }
			if (c == '"' && p[1] == '"') { g_string_append_c(field, '"'); p++; }
			else if (c == '"') { quoted = FALSE; closed = TRUE; }
			else g_string_append_c(field, c);
			continue;
		}
		if (c == '"' && field->len == 0 && !closed) { quoted = TRUE; continue; }
		if (c == ',' || c == '\r' || c == '\n' || c == '\0')
		{
			g_ptr_array_add(row, g_strdup(field->str));
			g_string_truncate(field, 0);
			closed = FALSE;
			if (c != ',')
			{
				if (row->len > 1 || *(gchar *)g_ptr_array_index(row, 0) != '\0')
				{
					g_ptr_array_add(row, NULL);
					g_ptr_array_add(result, g_ptr_array_free(g_steal_pointer(&row), FALSE));
					row = g_ptr_array_new_with_free_func(g_free);
				}
				else g_ptr_array_set_size(row, 0);
				if (c == '\r' && p[1] == '\n') p++;
			}
			if (c == '\0') break;
		}
		else if (closed || c == '"') { refuse(error, "malformed CSV field"); return NULL; }
		else g_string_append_c(field, c);
	}
	return g_steal_pointer(&result);
}

static gchar *
ofx_value(const gchar *block, const gchar *tag)
{
	g_autofree gchar *open = g_strdup_printf("<%s>", tag);
	const gchar *start = strstr(block, open);
	const gchar *end;
	gchar *value;
	if (start == NULL) return NULL;
	start += strlen(open);
	end = start;
	while (*end && *end != '<' && *end != '\r' && *end != '\n') end++;
	value = g_strndup(start, end - start);
	return g_strstrip(value);
}

static gint
column(gchar **header, const gchar *name)
{
	gint i;
	for (i = 0; header[i] != NULL; i++)
		if (!g_strcmp0(header[i], name)) return i;
	return -1;
}

static GDateTime *
bank_date(const gchar *text, const gchar *format)
{
	gint y = 0, m = 0, d = 0;
	gchar tail;
	if (text == NULL) return NULL;
	if (!g_strcmp0(format, "%Y-%m-%d")) return date_parse(text);
	if (!g_strcmp0(format, "%m/%d/%Y") && sscanf(text, "%d/%d/%d%c", &m, &d, &y, &tail) != 3) return NULL;
	if (!g_strcmp0(format, "%d/%m/%Y") && sscanf(text, "%d/%d/%d%c", &d, &m, &y, &tail) != 3) return NULL;
	if (!g_strcmp0(format, "ofx"))
	{
		g_autofree gchar *iso = strlen(text) >= 8 ? g_strdup_printf("%.4s-%.2s-%.2s", text, text + 4, text + 6) : NULL;
		return date_parse(iso);
	}
	if (!g_date_valid_dmy(d, m, y)) return NULL;
	return g_date_time_new_utc(y, m, d, 0, 0, 0);
}

static const gchar *
cell(gchar **line, gint n, gint index)
{
	return index >= 0 && index < n ? line[index] : "";
}

static gchar *
csv_amount_text(gchar **line, gint n, gint amount_col, gint debit_col, gint credit_col, const gchar *locale)
{
	const gchar *raw = cell(line, n, amount_col);
	gchar *text;
	if (raw != NULL && *raw)
		text = g_strdup(raw);
	else
	{
		const gchar *debit = cell(line, n, debit_col);
		const gchar *credit = cell(line, n, credit_col);
		if ((debit == NULL || *debit == '\0') && (credit == NULL || *credit == '\0'))
			return NULL;
		if (credit != NULL && *credit && (debit == NULL || *debit == '\0'))
			text = g_strdup(credit);
		else if (debit != NULL && *debit && (credit == NULL || *credit == '\0'))
			text = g_strconcat("-", debit, NULL);
		else
			return NULL;
	}
	if (locale != NULL && (g_str_has_prefix(locale, "de") || strstr(locale, "DE") != NULL))
		g_strdelimit(text, ",", '.');
	return text;
}

static gboolean
import_line(VentureBankMatchService *self, VentureEntity *bank, VentureEntity *statement,
	const gchar *date_text, const gchar *amount_text, const gchar *description, const gchar *reference,
	const gchar *external, const gchar *format, gboolean invert, gboolean *stored,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *currency = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *derived = NULL;
	g_autoptr(GDateTime) date = bank_date(date_text, format);
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureEntity) transaction = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	g_autoptr(GPtrArray) duplicates = NULL;
	gint64 org = venture_entity_get_organization_id(bank);
	if (stored != NULL) *stored = FALSE;
	g_object_get(bank, "currency", &currency, NULL);
	g_object_get(statement, "period-start", &start, "period-end", &end, NULL);
	if (date == NULL || amount_text == NULL || *amount_text == '\0')
		return refuse(error, "every imported line requires valid date, amount and external ID");
	if (g_date_time_compare(date, start) < 0 || g_date_time_compare(date, end) > 0)
		return refuse(error, "transaction date is outside the statement");
	if (external == NULL || *external == '\0')
	{
		g_autofree gchar *stamp = g_strdup_printf("%s|%s|%s", date_text, amount_text,
			description != NULL ? description : "");
		g_autoptr(VentureQuery) derived_query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
		g_autoptr(GPtrArray) similar = NULL;
		guint n = 1, i;
		venture_query_set_organization(derived_query, org);
		venture_query_set_include_deleted(derived_query, TRUE);
		if (!venture_query_add_filter_int(derived_query, "bank-account-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(bank), error))
			return FALSE;
		similar = venture_database_find(self->database, derived_query, error);
		if (similar == NULL) return FALSE;
		for (i = 0; i < similar->len; i++)
		{
			g_autofree gchar *existing_id = NULL;
			g_object_get(g_ptr_array_index(similar, i), "external-id", &existing_id, NULL);
			if (existing_id != NULL && g_str_has_prefix(existing_id, stamp))
				n++;
		}
		derived = g_strdup_printf("%s|#%u", stamp, n);
		external = derived;
	}
	amount = venture_money_from_string(amount_text, currency, error);
	if (amount == NULL) return FALSE;
	if (strcmp(venture_money_get_currency(amount), currency)) return refuse(error, "statement currency mismatch");
	if (invert)
	{
		VentureMoney *negative = venture_money_multiply_int(amount, -1, error);
		if (negative == NULL) return FALSE;
		g_clear_pointer(&amount, venture_money_free);
		amount = negative;
	}
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", org, venture_entity_get_id(bank), external);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, "external-key", VENTURE_FILTER_OP_EQ, key, error)) return FALSE;
	duplicates = venture_database_find(self->database, query, error);
	if (duplicates == NULL) return FALSE;
	if (duplicates->len != 0)
	{
		/* Same file repeating a FITID is a bad extract; a later overlapping
		 * window reusing that FITID must keep the original row. */
		if (number(g_ptr_array_index(duplicates, 0), "statement-id") == venture_entity_get_id(statement))
			return refuse(error, "external ID already imported for this bank account");
		return TRUE;
	}
	transaction = new_record(VENTURE_TYPE_BANK_TRANSACTION, org);
	g_object_set(transaction, "statement-id", venture_entity_get_id(statement), "bank-account-id", venture_entity_get_id(bank),
		"date", date, "amount", amount, "description", description != NULL && *description ? description : "Bank transaction",
		"reference", reference, "external-id", external, "external-key", key, "state", "unmatched", NULL);
	if (!unlocked(self, transaction, error)) return FALSE;
	if (!save_owned(self, transaction, actor, error)) return FALSE;
	if (stored != NULL) *stored = TRUE;
	return TRUE;
}

static VentureEntity *
import_statement(VentureBankMatchService *self, VentureEntity *bank, JsonObject *args, const VentureActor *actor, GError **error)
{
	const gchar *data = option(args, "data"), *format = option(args, "format");
	g_autoptr(GDateTime) start = date_parse(option(args, "period_start"));
	g_autoptr(GDateTime) end = date_parse(option(args, "period_end"));
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(VentureMoney) opening = NULL, closing = NULL;
	g_autofree gchar *currency = NULL, *hash = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	guint count = 0;
	if (data == NULL || start == NULL || end == NULL || g_date_time_compare(start, end) > 0)
	{
		refuse(error, "import needs data and valid period boundaries"); return NULL;
	}
	if (strlen(option(args, "period_end")) == 10)
	{
		GDateTime *inclusive = g_date_time_add(end, G_TIME_SPAN_DAY - 1);
		g_date_time_unref(end); end = inclusive;
	}
	g_object_get(bank, "currency", &currency, NULL);
	if (option(args, "opening_balance") == NULL || option(args, "closing_balance") == NULL)
	{
		refuse(error, "statement balances are required"); return NULL;
	}
	opening = venture_money_from_string(option(args, "opening_balance"), currency, error);
	if (opening == NULL) return NULL;
	closing = venture_money_from_string(option(args, "closing_balance"), currency, error);
	if (closing == NULL) return NULL;
	if (strcmp(venture_money_get_currency(opening), currency) || strcmp(venture_money_get_currency(closing), currency))
	{
		refuse(error, "statement balances must use bank currency"); return NULL;
	}
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, data, -1);
	statement = new_record(VENTURE_TYPE_BANK_STATEMENT, venture_entity_get_organization_id(bank));
	g_object_set(statement, "bank-account-id", venture_entity_get_id(bank), "period-start", start, "period-end", end,
		"opening-balance", opening, "closing-balance", closing, "source-file-hash", hash, "imported-at", now, NULL);
	if (!save_owned(self, statement, actor, error)) return NULL;
	if (!g_strcmp0(format, "csv"))
	{
		g_autoptr(GPtrArray) parsed = parse_csv(data, error);
		g_autofree gchar *dc = NULL, *ac = NULL, *mc = NULL, *rc = NULL, *ic = NULL, *df = NULL, *sign = NULL;
		g_autofree gchar *debit_name = NULL, *credit_name = NULL, *locale = NULL;
		gint d, a, m, r, x, debit, credit;
		guint i;
		if (parsed == NULL) return NULL;
		g_object_get(bank, "date-column", &dc, "amount-column", &ac, "description-column", &mc,
			"reference-column", &rc, "external-id-column", &ic, "date-format", &df, "sign-convention", &sign,
			"debit-column", &debit_name, "credit-column", &credit_name, "locale", &locale, NULL);
		if (parsed->len < 2) { refuse(error, "CSV requires header and data"); return NULL; }
		d = column(g_ptr_array_index(parsed, 0), dc); a = column(g_ptr_array_index(parsed, 0), ac);
		m = column(g_ptr_array_index(parsed, 0), mc); r = column(g_ptr_array_index(parsed, 0), rc);
		x = column(g_ptr_array_index(parsed, 0), ic);
		debit = column(g_ptr_array_index(parsed, 0), debit_name);
		credit = column(g_ptr_array_index(parsed, 0), credit_name);
		if (d < 0 || (a < 0 && debit < 0 && credit < 0) || (g_strcmp0(sign, "normal") && g_strcmp0(sign, "invert") && sign != NULL && *sign))
		{ refuse(error, "CSV column mapping or sign convention is invalid"); return NULL; }
		if (sign == NULL || *sign == '\0') sign = g_strdup("normal");
		for (i = 1; i < parsed->len; i++)
		{
			gchar **line = g_ptr_array_index(parsed, i);
			gint n = g_strv_length(line);
			g_autofree gchar *amount_text = NULL;
			gboolean stored = FALSE;
			if (d >= n)
			{ refuse(error, "CSV row is missing mapped columns"); return NULL; }
			amount_text = csv_amount_text(line, n, a, debit, credit, locale);
			if (!import_line(self, bank, statement, line[d], amount_text, cell(line, n, m), cell(line, n, r),
				cell(line, n, x), df, !g_strcmp0(sign, "invert"), &stored, actor, error)) return NULL;
			if (stored) count++;
		}
	}
	else if (!g_strcmp0(format, "ofx") || !g_strcmp0(format, "qfx"))
	{
		const gchar *p = data;
		g_autofree gchar *declared_currency = ofx_value(data, "CURDEF");
		if (declared_currency != NULL && g_strcmp0(declared_currency, currency))
		{ refuse(error, "OFX currency differs from the bank account"); return NULL; }
		while ((p = strstr(p, "<STMTTRN>")) != NULL)
		{
			const gchar *end_block = strstr(p, "</STMTTRN>");
			g_autofree gchar *block = NULL, *date = NULL, *amount = NULL, *memo = NULL, *ref = NULL, *id = NULL;
			if (end_block == NULL) { refuse(error, "unterminated OFX transaction"); return NULL; }
			block = g_strndup(p, end_block - p);
			date = ofx_value(block, "DTPOSTED"); amount = ofx_value(block, "TRNAMT");
			memo = ofx_value(block, "MEMO"); ref = ofx_value(block, "CHECKNUM"); id = ofx_value(block, "FITID");
			{
				gboolean stored = FALSE;
				if (!import_line(self, bank, statement, date, amount, memo, ref, id, "ofx", FALSE, &stored, actor, error))
					return NULL;
				if (stored) count++;
			}
			p = end_block + strlen("</STMTTRN>");
		}
	}
	else { refuse(error, "supported formats are csv, ofx and qfx"); return NULL; }
	if (count == 0) { refuse(error, "external ID already imported for this bank account"); return NULL; }
	g_object_set(bank, "last-statement-balance", closing, "last-statement-date", end, NULL);
	if (!save_owned(self, bank, actor, error)) return NULL;
	return g_steal_pointer(&statement);
}

static gboolean
cash_document_source(const gchar *source_type)
{
	return source_type != NULL && (!strcmp(source_type, "expense") || !strcmp(source_type, "payment") ||
		!strcmp(source_type, "sale") || !strcmp(source_type, "vendor_bill_payment") ||
		!strcmp(source_type, "bill_payment") || !strcmp(source_type, "bank_transfer"));
}

static gboolean
cleared_amount(VentureBankMatchService *self, const gchar *source_type, gint64 source_id,
	gint64 org, GDateTime *end, gint64 *cleared, GError **error)
{
	g_autoptr(GPtrArray) matches = NULL;
	guint i;
	*cleared = 0;
	if (source_type == NULL || source_id <= 0)
		return TRUE;
	matches = rows(self->database, VENTURE_TYPE_BANK_MATCH, org, "record-id", source_id, error);
	if (matches == NULL)
		return FALSE;
	for (i = 0; i < matches->len; i++)
	{
		VentureEntity *match = g_ptr_array_index(matches, i);
		g_autofree gchar *type = NULL;
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(match, "record-type", &type, "cleared-at", &when, "amount", &amount, NULL);
		if (g_strcmp0(type, source_type) != 0)
			continue;
		if (when == NULL)
		{
			g_autoptr(VentureEntity) txn = venture_database_get(self->database, VENTURE_TYPE_BANK_TRANSACTION,
				number(match, "transaction-id"), error);
			if (txn == NULL)
				return FALSE;
			g_object_get(txn, "date", &when, NULL);
		}
		if (when != NULL && g_date_time_compare(when, end) <= 0 && amount != NULL)
			*cleared += llabs(venture_money_get_amount(amount));
	}
	return TRUE;
}

static gboolean
outstanding_items(VentureBankMatchService *self, VentureEntity *bank, GDateTime *start, GDateTime *end,
	VentureMoney **checks, VentureMoney **deposits, gchar **evidence, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureMoney) out = NULL;
	g_autoptr(VentureMoney) inn = NULL;
	g_autofree gchar *currency = NULL;
	gint64 account_id, org;
	guint i;
	g_object_get(bank, "currency", &currency, "account-id", &account_id, NULL);
	(void)start;
	org = venture_entity_get_organization_id(bank);
	out = venture_money_new_zero(currency);
	inn = venture_money_new_zero(currency);
	json_builder_begin_array(builder);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	journals = venture_database_find(self->database, query, error);
	if (journals == NULL)
		return FALSE;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(VentureQuery) lines_query = NULL;
		g_autofree gchar *source_type = NULL;
		gint64 source_id;
		gint64 remaining_cleared = 0;
		gint64 reverses_id = 0;
		VentureJournalState state;
		guint j;
		g_object_get(journal, "state", &state, "occurred-at", &date, "source-type", &source_type,
			"source-id", &source_id, "reverses-id", &reverses_id, NULL);
		if (state != VENTURE_JOURNAL_POSTED || reverses_id != 0 || date == NULL)
			continue;
		if (g_date_time_compare(date, end) > 0)
			continue;
		if (!cash_document_source(source_type))
			continue;
		if (!cleared_amount(self, source_type, source_id, org, end, &remaining_cleared, error))
			return FALSE;
		lines_query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		venture_query_set_limit(lines_query, 0);
		venture_query_add_filter_int(lines_query, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(journal), NULL);
		lines = venture_database_find(self->database, lines_query, error);
		if (lines == NULL)
			return FALSE;
		for (j = 0; j < lines->len; j++)
		{
			VentureEntity *line = g_ptr_array_index(lines, j);
			g_autoptr(VentureMoney) amount = NULL;
			gint64 line_account;
			gint side;
			g_object_get(line, "account-id", &line_account, "side", &side, "book-amount", &amount, NULL);
			if (line_account != account_id || amount == NULL || venture_money_is_zero(amount))
				continue;
			{
				gint64 line_amount = venture_money_get_amount(amount);
				gint64 apply = remaining_cleared;
				gint64 remaining;
				g_autoptr(VentureMoney) uncleared = NULL;
				if (apply > line_amount)
					apply = line_amount;
				remaining_cleared -= apply;
				remaining = line_amount - apply;
				if (remaining <= 0)
					continue;
				uncleared = venture_money_new(remaining, venture_money_get_currency(amount),
					venture_money_get_exponent(amount));
				g_clear_pointer(&amount, venture_money_free);
				amount = g_steal_pointer(&uncleared);
			}
			if (side == VENTURE_LEDGER_SIDE_CREDIT)
			{
				VentureMoney *next = venture_money_add(out, amount, error);
				if (next == NULL) return FALSE;
				venture_money_free(out); out = next;
				json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "kind"); json_builder_add_string_value(builder, "outstanding_check");
				json_builder_set_member_name(builder, "journal_id"); json_builder_add_int_value(builder, venture_entity_get_id(journal));
				json_builder_end_object(builder);
			}
			else
			{
				VentureMoney *next = venture_money_add(inn, amount, error);
				if (next == NULL) return FALSE;
				venture_money_free(inn); inn = next;
				json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "kind"); json_builder_add_string_value(builder, "deposit_in_transit");
				json_builder_set_member_name(builder, "journal_id"); json_builder_add_int_value(builder, venture_entity_get_id(journal));
				json_builder_end_object(builder);
			}
		}
	}
	json_builder_end_array(builder);
	node = json_builder_get_root(builder);
	*checks = g_steal_pointer(&out);
	*deposits = g_steal_pointer(&inn);
	*evidence = venture_json_to_string(node, FALSE);
	return TRUE;
}

static VentureEntity *
reconcile(VentureBankMatchService *self, VentureEntity *statement, gboolean finalize_record, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, number(statement, "bank-account-id"), error);
	g_autoptr(GPtrArray) transactions = NULL;
	g_autoptr(GDateTime) end = NULL, start = NULL, now = g_date_time_new_now_utc();
	g_autoptr(VentureMoney) closing = NULL, book = NULL, difference = NULL, checks = NULL, deposits = NULL, adjusted = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *evidence = NULL;
	g_autoptr(VentureEntity) result = NULL;
	gint64 org = venture_entity_get_organization_id(statement);
	guint i;
	if (bank == NULL) return NULL;
	g_object_get(statement, "period-end", &end, "period-start", &start, "closing-balance", &closing, NULL);
	g_object_get(bank, "currency", &currency, NULL);
	transactions = rows(self->database, VENTURE_TYPE_BANK_TRANSACTION, org, "bank-account-id", venture_entity_get_id(bank), error);
	if (transactions == NULL) return NULL;
	for (i = 0; i < transactions->len; i++)
	{
		VentureEntity *transaction = g_ptr_array_index(transactions, i);
		g_autoptr(GDateTime) date = NULL;
		g_object_get(transaction, "date", &date, NULL);
		if (finalize_record && g_date_time_compare(date, end) <= 0 && state_is(transaction, "unmatched"))
		{ refuse(error, "unmatched bank transactions remain through the period end"); return NULL; }
	}
	book = venture_posting_service_account_balance(venture_database_get_posting_service(self->database),
		number(bank, "account-id"), org, currency, end, error);
	if (book == NULL) return NULL;
	if (!outstanding_items(self, bank, start, end, &checks, &deposits, &evidence, error))
		return NULL;
	adjusted = venture_money_add(book, checks, error);
	if (adjusted == NULL) return NULL;
	{
		VentureMoney *next = venture_money_subtract(adjusted, deposits, error);
		if (next == NULL) return NULL;
		g_clear_pointer(&adjusted, venture_money_free);
		adjusted = next;
	}
	difference = venture_money_subtract(closing, book, error);
	if (difference == NULL) return NULL;
	if (finalize_record && !venture_money_equal(closing, adjusted))
	{ refuse(error, "statement and posted book balance differ after outstanding items"); return NULL; }
	result = new_record(VENTURE_TYPE_RECONCILIATION, org);
	g_object_set(result, "bank-account-id", venture_entity_get_id(bank), "period-end", end,
		"statement-balance", closing, "book-balance", book, "difference", difference,
		"outstanding-checks", checks, "deposits-in-transit", deposits, "outstanding-items", evidence,
		"state", finalize_record ? "reconciled" : "open",
		"reconciled-by", finalize_record ? (actor != NULL ? actor->name : "system") : NULL,
		"reconciled-at", finalize_record ? now : NULL, NULL);
	if (!save_owned(self, result, actor, error)) return NULL;
	return g_steal_pointer(&result);
}

static VentureEntity *
reopen_reconciliation(VentureBankMatchService *self, VentureEntity *statement, JsonObject *args,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GDateTime) end = NULL, now = g_date_time_new_now_utc();
	const gchar *reason = option(args, "reason");
	guint i;
	if (reason == NULL || *reason == '\0')
	{ refuse(error, "reopen requires a reason"); return NULL; }
	g_object_get(statement, "period-end", &end, NULL);
	found = rows(self->database, VENTURE_TYPE_RECONCILIATION, venture_entity_get_organization_id(statement),
		"bank-account-id", number(statement, "bank-account-id"), error);
	if (found == NULL) return NULL;
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *rec = g_ptr_array_index(found, i);
		g_autoptr(GDateTime) rec_end = NULL;
		g_object_get(rec, "period-end", &rec_end, NULL);
		if (!state_is(rec, "reconciled") || rec_end == NULL || g_date_time_compare(rec_end, end) != 0)
			continue;
		g_object_set(rec, "state", "reopened", "reopened-by", actor != NULL ? actor->name : "system",
			"reopened-at", now, "reopen-reason", reason, NULL);
		if (!save_owned(self, rec, actor, error)) return NULL;
		return g_object_ref(rec);
	}
	refuse(error, "no reconciled evidence exists for this statement");
	return NULL;
}


static JsonArray *
parts_for(VentureEntity *record)
{
	JsonArray *parts = json_array_new();
	JsonObject *part = json_object_new();
	json_object_set_string_member(part, "type", venture_entity_get_entity_name(record));
	json_object_set_int_member(part, "id", venture_entity_get_id(record));
	json_array_add_object_element(parts, part);
	return parts;
}

static gboolean
auto_match(VentureBankMatchService *self, VentureEntity *statement, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) transactions = rows(self->database, VENTURE_TYPE_BANK_TRANSACTION,
		venture_entity_get_organization_id(statement), "statement-id", venture_entity_get_id(statement), error);
	guint i;
	if (transactions == NULL) return FALSE;
	for (i = 0; i < transactions->len; i++)
	{
		VentureEntity *transaction = g_ptr_array_index(transactions, i);
		g_autoptr(GPtrArray) candidates = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) date = NULL;
		guint j, count = 0;
		VentureEntity *only = NULL;
		if (!state_is(transaction, "unmatched")) continue;
		candidates = venture_bank_transaction_candidates(self->database, transaction, error);
		if (candidates == NULL) return FALSE;
		g_object_get(transaction, "amount", &amount, "date", &date, NULL);
		for (j = 0; j < candidates->len; j++)
		{
			VentureEntity *candidate = g_ptr_array_index(candidates, j);
			g_autoptr(VentureMoney) value = venture_bank_candidate_amount(candidate);
			g_autoptr(GDateTime) when = venture_bank_candidate_date(candidate);
			if (venture_money_equal(value, amount) &&
				calendar_distance(when, date) <= 3 && calendar_distance(when, date) >= -3)
			{ only = candidate; count++; }
		}
		if (count == 1)
		{
			g_autoptr(JsonArray) parts = parts_for(only);
			if (!match_records(self, transaction, parts, actor, error)) return FALSE;
		}
	}
	return TRUE;
}

static gboolean
create_document(VentureBankMatchService *self, VentureEntity *transaction, JsonObject *args, const VentureActor *actor, GError **error)
{
	const gchar *type = option(args, "type");
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureEntity) bank = NULL;
	g_autoptr(VentureMoney) signed_amount = NULL, amount = NULL;
	g_autoptr(VentureMoney) before = NULL, after = NULL, change = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *description = NULL;
	g_autoptr(JsonArray) parts = NULL;
	gint64 org = venture_entity_get_organization_id(transaction);
	if (!unlocked(self, transaction, error)) return FALSE;
	if (!state_is(transaction, "unmatched"))
		return refuse(error, "creation requires an unreconciled unmatched transaction");
	g_object_get(transaction, "amount", &signed_amount, "date", &date, "description", &description, NULL);
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, date, error)) return FALSE;
	bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, number(transaction, "bank-account-id"), error);
	if (bank == NULL) return FALSE;
	before = venture_posting_service_account_balance(venture_database_get_posting_service(self->database),
		number(bank, "account-id"), org, venture_money_get_currency(signed_amount), date, error);
	if (before == NULL) return FALSE;
	if (!g_strcmp0(type, "expense") && venture_money_get_amount(signed_amount) < 0)
	{
		amount = venture_money_multiply_int(signed_amount, -1, error);
		if (amount == NULL) return FALSE;
		record = new_record(VENTURE_TYPE_EXPENSE, org);
		g_object_set(record, "amount", amount, "occurred-at", date, "description", description,
			"cash-account-id", number(bank, "account-id"), NULL);
		if (option(args, "category") != NULL)
			g_object_set(record, "category", option(args, "category"), NULL);
		if (!venture_database_save(self->database, record, actor, error)) return FALSE;
	}
	else if ((!g_strcmp0(type, "receipt") || !g_strcmp0(type, "payment")) && venture_money_get_amount(signed_amount) > 0)
	{
		if (option_id(args, "customer_id") <= 0) return refuse(error, "receipt requires customer_id");
		if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "payment") == G_TYPE_INVALID)
			return refuse(error, "receivables module is disabled");
		record = new_record(VENTURE_TYPE_PAYMENT, org);
		g_object_set(record, "amount", signed_amount, "date", date, "method", "bank",
			"customer-id", option_id(args, "customer_id"), "reference", description, NULL);
		/* The canonical service owns the write permit; use its configured cash
		 * account and restore the property before returning to the caller. */
		{
			VentureSettlementService *canonical = venture_settlement_service_get(self->database);
			gint64 previous_cash = 0;
			gboolean ok;
			g_object_get(canonical, "cash-account-id", &previous_cash, NULL);
			g_object_set(canonical, "cash-account-id", number(bank, "account-id"), NULL);
			ok = venture_settlement_service_apply_payment(canonical, VENTURE_PAYMENT(record), NULL, actor, error);
			g_object_set(canonical, "cash-account-id", previous_cash, NULL);
			if (!ok) return FALSE;
		}
	}
	else return refuse(error, "negative lines create expenses; positive lines create receipts");
	after = venture_posting_service_account_balance(venture_database_get_posting_service(self->database),
		number(bank, "account-id"), org, venture_money_get_currency(signed_amount), date, error);
	if (after == NULL) return FALSE;
	change = venture_money_subtract(after, before, error);
	if (change == NULL) return FALSE;
	if (!venture_money_equal(change, signed_amount))
		return refuse(error, "document posting rule must use this bank account; configure its cash account policy");
	parts = parts_for(record);
	return match_records(self, transaction, parts, actor, error);
}

static gboolean
option_flag(JsonObject *args, const gchar *name)
{
	JsonNode *node = args != NULL ? json_object_get_member(args, name) : NULL;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
	if (json_node_get_value_type(node) == G_TYPE_BOOLEAN) return json_node_get_boolean(node);
	if (json_node_get_value_type(node) == G_TYPE_STRING) return !g_strcmp0(json_node_get_string(node), "true");
	return FALSE;
}

static gboolean
text_has(const gchar *hay, const gchar *needle)
{
	g_autofree gchar *h = NULL, *n = NULL;
	if (needle == NULL || *needle == '\0') return TRUE;
	if (hay == NULL) return FALSE;
	h = g_ascii_strdown(hay, -1);
	n = g_ascii_strdown(needle, -1);
	return strstr(h, n) != NULL;
}

static gint
rule_cmp(gconstpointer a, gconstpointer b)
{
	VentureEntity *left = *(VentureEntity *const *)a, *right = *(VentureEntity *const *)b;
	gint64 pa = number(left, "priority"), pb = number(right, "priority");
	if (pa != pb) return pa < pb ? -1 : 1;
	if (venture_entity_get_id(left) < venture_entity_get_id(right)) return -1;
	if (venture_entity_get_id(left) > venture_entity_get_id(right)) return 1;
	return 0;
}

static gboolean
rule_matches(VentureEntity *rule, VentureEntity *transaction)
{
	g_autofree gchar *merchant = NULL, *contains = NULL, *description = NULL;
	g_autoptr(VentureMoney) amount = NULL, min = NULL, max = NULL;
	gint64 bank_id;
	g_object_get(rule, "merchant", &merchant, "description-contains", &contains,
		"amount-min", &min, "amount-max", &max, "bank-account-id", &bank_id, NULL);
	g_object_get(transaction, "description", &description, "amount", &amount, NULL);
	if (bank_id > 0 && bank_id != number(transaction, "bank-account-id")) return FALSE;
	if (!text_has(description, merchant) || !text_has(description, contains)) return FALSE;
	if (min != NULL && amount != NULL && venture_money_compare(amount, min) < 0) return FALSE;
	if (max != NULL && amount != NULL && venture_money_compare(amount, max) > 0) return FALSE;
	return TRUE;
}

static GPtrArray *
org_rules(VentureBankMatchService *self, gint64 org, GError **error)
{
	GPtrArray *found = rows(self->database, VENTURE_TYPE_BANK_RULE, org, NULL, 0, error);
	if (found != NULL) g_ptr_array_sort(found, rule_cmp);
	return found;
}

static gchar *
json_dump(JsonBuilder *builder)
{
	g_autoptr(JsonNode) node = json_builder_get_root(builder);
	return venture_json_to_string(node, FALSE);
}

static gboolean
suggest_transaction(VentureBankMatchService *self, VentureEntity *transaction, GPtrArray *rules,
	gboolean enabled_only, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) hits = g_ptr_array_new();
	g_autofree gchar *explanation = NULL;
	guint i;
	gint64 first = 0;
	gint confidence = 0;
	const gchar *action = NULL;
	(void)actor;
	if (error != NULL && *error != NULL) return FALSE;
	for (i = 0; i < rules->len; i++)
	{
		VentureEntity *rule = g_ptr_array_index(rules, i);
		gboolean enabled = FALSE;
		g_object_get(rule, "enabled", &enabled, NULL);
		if (enabled_only && !enabled) continue;
		if (rule_matches(rule, transaction))
			g_ptr_array_add(hits, rule);
	}
	if (hits->len == 1)
	{
		g_autofree gchar *name = NULL, *kind = NULL;
		VentureEntity *rule = g_ptr_array_index(hits, 0);
		g_object_get(rule, "name", &name, "action", &kind, NULL);
		first = venture_entity_get_id(rule);
		action = kind != NULL && *kind ? kind : "categorize";
		confidence = 90;
		explanation = g_strdup_printf("Rule '%s' matched merchant, description, amount and account",
			name != NULL ? name : "rule");
	}
	else if (hits->len > 1)
	{
		g_autofree gchar *first_name = NULL, *second_name = NULL;
		g_object_get(g_ptr_array_index(hits, 0), "name", &first_name, NULL);
		g_object_get(g_ptr_array_index(hits, 1), "name", &second_name, NULL);
		confidence = 0;
		action = NULL;
		explanation = g_strdup_printf("ambiguous: '%s' and '%s' both match",
			first_name != NULL ? first_name : "rule", second_name != NULL ? second_name : "rule");
	}
	else
	{
		confidence = 0;
		explanation = g_strdup("no categorization rule matched");
	}
	g_object_set(transaction, "review-confidence", (gint64)confidence, "review-explanation", explanation,
		"suggested-rule-id", first, "suggested-action", action, NULL);
	return save_owned(self, transaction, actor, error);
}

static gboolean
preview_rule(VentureBankMatchService *self, VentureEntity *rule, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) transactions = rows(self->database, VENTURE_TYPE_BANK_TRANSACTION,
		venture_entity_get_organization_id(rule), NULL, 0, error);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *json = NULL;
	guint i, matched = 0;
	if (transactions == NULL) return FALSE;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "matches");
	json_builder_begin_array(builder);
	for (i = 0; i < transactions->len; i++)
	{
		VentureEntity *transaction = g_ptr_array_index(transactions, i);
		g_autofree gchar *description = NULL;
		if (!rule_matches(rule, transaction)) continue;
		g_object_get(transaction, "description", &description, NULL);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "transaction_id");
		json_builder_add_int_value(builder, venture_entity_get_id(transaction));
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder, description != NULL ? description : "");
		json_builder_set_member_name(builder, "confidence");
		json_builder_add_int_value(builder, 90);
		json_builder_end_object(builder);
		matched++;
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, matched);
	json_builder_end_object(builder);
	json = json_dump(builder);
	g_object_set(rule, "last-preview", json, NULL);
	return save_owned(self, rule, actor, error);
}

static gboolean
enable_rule(VentureBankMatchService *self, VentureEntity *rule, const VentureActor *actor, GError **error)
{
	g_autofree gchar *preview = NULL;
	g_object_get(rule, "last-preview", &preview, NULL);
	if (preview == NULL || *preview == '\0')
	{
		if (!preview_rule(self, rule, actor, error)) return FALSE;
	}
	g_object_set(rule, "enabled", TRUE, NULL);
	return save_owned(self, rule, actor, error);
}

static gboolean
create_splits(VentureBankMatchService *self, VentureEntity *transaction, VentureEntity *rule,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *splits = NULL, *description = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(VentureEntity) bank = NULL;
	g_autoptr(VentureMoney) signed_amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(JsonArray) parts = json_array_new();
	JsonArray *array;
	guint i;
	gint64 org = venture_entity_get_organization_id(transaction);
	g_object_get(rule, "splits", &splits, NULL);
	if (splits == NULL || *splits == '\0') return FALSE;
	if (!json_parser_load_from_data(parser, splits, -1, error)) return FALSE;
	if (!JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
		return refuse(error, "rule splits must be a JSON array");
	array = json_node_get_array(json_parser_get_root(parser));
	g_object_get(transaction, "amount", &signed_amount, "date", &date, "description", &description, NULL);
	bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, number(transaction, "bank-account-id"), error);
	if (bank == NULL) return FALSE;
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, date, error)) return FALSE;
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *part = json_array_get_object_element(array, i);
		const gchar *category = option(part, "category");
		const gchar *amount_text = option(part, "amount");
		g_autoptr(VentureEntity) expense = NULL;
		g_autoptr(VentureMoney) part_amount = NULL;
		JsonObject *match;
		if (amount_text == NULL) return refuse(error, "split parts require amount");
		part_amount = venture_money_from_string(amount_text, venture_money_get_currency(signed_amount), error);
		if (part_amount == NULL) return FALSE;
		if (venture_money_get_amount(part_amount) < 0)
		{
			VentureMoney *abs_amount = venture_money_multiply_int(part_amount, -1, error);
			if (abs_amount == NULL) return FALSE;
			g_clear_pointer(&part_amount, venture_money_free);
			part_amount = abs_amount;
		}
		expense = new_record(VENTURE_TYPE_EXPENSE, org);
		g_object_set(expense, "amount", part_amount, "occurred-at", date, "description", description,
			"cash-account-id", number(bank, "account-id"), NULL);
		if (category != NULL) g_object_set(expense, "category", category, NULL);
		if (!venture_database_save(self->database, expense, actor, error)) return FALSE;
		match = json_object_new();
		json_object_set_string_member(match, "type", "expense");
		json_object_set_int_member(match, "id", venture_entity_get_id(expense));
		json_array_add_object_element(parts, match);
	}
	return match_records(self, transaction, parts, actor, error);
}

static gboolean
apply_rule(VentureBankMatchService *self, VentureEntity *transaction, VentureEntity *rule,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *kind = NULL, *category = NULL, *create_type = NULL, *splits = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	g_object_get(rule, "action", &kind, "category", &category, "create-type", &create_type, "splits", &splits, NULL);
	if (!state_is(transaction, "unmatched")) return TRUE;
	if (!g_strcmp0(kind, "match"))
	{
		g_autoptr(GPtrArray) candidates = venture_bank_transaction_candidates(self->database, transaction, error);
		if (candidates == NULL) return FALSE;
		if (candidates->len != 1) return TRUE;
		{
			g_autoptr(JsonArray) parts = parts_for(g_ptr_array_index(candidates, 0));
			return match_records(self, transaction, parts, actor, error);
		}
	}
	if (!g_strcmp0(kind, "split") || (splits != NULL && *splits))
		return create_splits(self, transaction, rule, actor, error);
	json_object_set_string_member(args, "type", create_type != NULL && *create_type ? create_type : "expense");
	if (category != NULL) json_object_set_string_member(args, "category", category);
	return create_document(self, transaction, args, actor, error);
}

static gboolean
review_account(VentureBankMatchService *self, VentureEntity *bank, gboolean apply_unique,
	JsonObject *args, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(bank);
	g_autoptr(GPtrArray) rules = org_rules(self, org, error);
	g_autoptr(GPtrArray) transactions = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *json = NULL;
	JsonArray *ids = args != NULL && json_object_has_member(args, "ids") &&
		JSON_NODE_HOLDS_ARRAY(json_object_get_member(args, "ids")) ?
		json_object_get_array_member(args, "ids") : NULL;
	guint i;
	if (rules == NULL) return FALSE;
	transactions = rows(self->database, VENTURE_TYPE_BANK_TRANSACTION, org, "bank-account-id",
		venture_entity_get_id(bank), error);
	if (transactions == NULL) return FALSE;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "rows");
	json_builder_begin_array(builder);
	for (i = 0; i < transactions->len; i++)
	{
		VentureEntity *transaction = g_ptr_array_index(transactions, i);
		gint64 confidence;
		g_autofree gchar *explanation = NULL;
		gboolean wanted = ids == NULL;
		guint j;
		if (!state_is(transaction, "unmatched")) continue;
		if (ids != NULL)
		{
			for (j = 0; j < json_array_get_length(ids); j++)
				if (json_array_get_int_element(ids, j) == venture_entity_get_id(transaction))
					wanted = TRUE;
		}
		if (!wanted) continue;
		if (!suggest_transaction(self, transaction, rules, TRUE, actor, error)) return FALSE;
		g_object_get(transaction, "review-confidence", &confidence, "review-explanation", &explanation, NULL);
		if (apply_unique && confidence >= 80)
		{
			g_autoptr(VentureEntity) rule = venture_database_get(self->database, VENTURE_TYPE_BANK_RULE,
				number(transaction, "suggested-rule-id"), error);
			if (rule == NULL) return FALSE;
			if (!apply_rule(self, transaction, rule, actor, error)) return FALSE;
		}
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "transaction_id");
		json_builder_add_int_value(builder, venture_entity_get_id(transaction));
		json_builder_set_member_name(builder, "confidence");
		json_builder_add_int_value(builder, confidence);
		json_builder_set_member_name(builder, "explanation");
		json_builder_add_string_value(builder, explanation != NULL ? explanation : "");
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	json = json_dump(builder);
	g_object_set(bank, "last-inbox", json, NULL);
	return save_owned(self, bank, actor, error);
}

static gboolean
unmatch_transaction(VentureBankMatchService *self, VentureEntity *transaction, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) matches = rows(self->database, VENTURE_TYPE_BANK_MATCH,
		venture_entity_get_organization_id(transaction), "transaction-id", venture_entity_get_id(transaction), error);
	guint i;
	if (matches == NULL) return FALSE;
	for (i = 0; i < matches->len; i++)
	{
		VentureEntity *match = g_ptr_array_index(matches, i);
		self->writing = match;
		if (!venture_database_delete(self->database, match, actor, error))
		{
			self->writing = NULL;
			return FALSE;
		}
		self->writing = NULL;
	}
	g_object_set(transaction, "state", "unmatched", "match-id", (gint64)0, "exclusion-reason", NULL, NULL);
	return save_owned(self, transaction, actor, error);
}

static gboolean
reverse_categorization(VentureBankMatchService *self, VentureEntity *transaction,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) matches = NULL;
	g_autoptr(GDateTime) date = NULL;
	gint64 org = venture_entity_get_organization_id(transaction);
	guint i;
	if (!unlocked(self, transaction, error)) return FALSE;
	g_object_get(transaction, "date", &date, NULL);
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, date, error)) return FALSE;
	matches = rows(self->database, VENTURE_TYPE_BANK_MATCH, org, "transaction-id",
		venture_entity_get_id(transaction), error);
	if (matches == NULL) return FALSE;
	for (i = 0; i < matches->len; i++)
	{
		VentureEntity *match = g_ptr_array_index(matches, i);
		g_autofree gchar *record_type = NULL;
		g_autoptr(GPtrArray) journals = NULL;
		guint j;
		g_object_get(match, "record-type", &record_type, NULL);
		if (record_type == NULL) continue;
		journals = venture_posting_service_find_source(venture_database_get_posting_service(self->database),
			record_type, number(match, "record-id"), org, error);
		if (journals == NULL) return FALSE;
		for (j = 0; j < journals->len; j++)
		{
			VentureEntity *journal = g_ptr_array_index(journals, j);
			VentureJournalState state;
			gint64 reverses = 0;
			g_object_get(journal, "state", &state, "reverses-id", &reverses, NULL);
			if (state == VENTURE_JOURNAL_POSTED && reverses == 0)
			{
				g_autoptr(VentureJournal) reversal = venture_posting_service_reverse(
					venture_database_get_posting_service(self->database), venture_entity_get_id(journal),
					date, "Bank categorization reversed", actor, error);
				if (reversal == NULL) return FALSE;
			}
		}
	}
	return unmatch_transaction(self, transaction, actor, error);
}

static gboolean
learn_from(VentureBankMatchService *self, VentureEntity *transaction, JsonObject *args,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *description = NULL, *merchant = NULL;
	g_autoptr(GPtrArray) rules = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	gint64 org = venture_entity_get_organization_id(transaction);
	guint i;
	g_object_get(transaction, "description", &description, NULL);
	merchant = description != NULL ? g_strdup(description) : g_strdup("Bank");
	rules = org_rules(self, org, error);
	if (rules == NULL) return FALSE;
	for (i = 0; i < rules->len; i++)
	{
		VentureEntity *existing = g_ptr_array_index(rules, i);
		g_autofree gchar *existing_merchant = NULL;
		g_object_get(existing, "merchant", &existing_merchant, NULL);
		if (existing_merchant != NULL && text_has(description, existing_merchant))
		{
			g_object_set(existing, "category", option(args, "category"),
				"action", "categorize", "priority", number(existing, "priority") - 1, NULL);
			return save_owned(self, existing, actor, error);
		}
	}
	rule = new_record(VENTURE_TYPE_BANK_RULE, org);
	g_object_set(rule, "name", merchant, "merchant", merchant, "priority", (gint64)1, "enabled", FALSE,
		"action", "categorize", "category", option(args, "category"), "create-type", "expense",
		"bank-account-id", number(transaction, "bank-account-id"),
		"last-preview", "{\"matches\":[],\"count\":0}", NULL);
	return save_owned(self, rule, actor, error);
}

static gboolean
correct_transaction(VentureBankMatchService *self, VentureEntity *transaction, JsonObject *args,
	const VentureActor *actor, GError **error)
{
	if (!state_is(transaction, "unmatched") && !reverse_categorization(self, transaction, actor, error))
		return FALSE;
	if (!create_document(self, transaction, args, actor, error)) return FALSE;
	return learn_from(self, transaction, args, actor, error);
}

static gboolean
map_csv(VentureBankMatchService *self, VentureEntity *bank, JsonObject *args,
	const VentureActor *actor, GError **error)
{
	const gchar *data = option(args, "data");
	g_autoptr(GPtrArray) parsed = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *json = NULL;
	gchar **header;
	guint i;
	if (data == NULL) return refuse(error, "mapping requires CSV data");
	parsed = parse_csv(data, error);
	if (parsed == NULL) return FALSE;
	if (parsed->len < 1) return refuse(error, "CSV requires a header");
	header = g_ptr_array_index(parsed, 0);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "headers");
	json_builder_begin_array(builder);
	for (i = 0; header[i] != NULL; i++)
		json_builder_add_string_value(builder, header[i]);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	json = json_dump(builder);
	if (option_flag(args, "apply"))
	{
		if (option(args, "date_column")) g_object_set(bank, "date-column", option(args, "date_column"), NULL);
		if (option(args, "amount_column")) g_object_set(bank, "amount-column", option(args, "amount_column"), NULL);
		if (option(args, "description_column")) g_object_set(bank, "description-column", option(args, "description_column"), NULL);
		if (option(args, "reference_column")) g_object_set(bank, "reference-column", option(args, "reference_column"), NULL);
		if (option(args, "external_id_column")) g_object_set(bank, "external-id-column", option(args, "external_id_column"), NULL);
		if (option(args, "debit_column")) g_object_set(bank, "debit-column", option(args, "debit_column"), NULL);
		if (option(args, "credit_column")) g_object_set(bank, "credit-column", option(args, "credit_column"), NULL);
		if (option(args, "locale")) g_object_set(bank, "locale", option(args, "locale"), NULL);
		if (option(args, "date_format")) g_object_set(bank, "date-format", option(args, "date_format"), NULL);
	}
	g_object_set(bank, "last-mapping-preview", json, NULL);
	return save_owned(self, bank, actor, error);
}

static gchar *
iso_day(GDateTime *date)
{
	return date != NULL ? g_date_time_format(date, "%Y-%m-%d") : g_strdup("");
}

static gboolean
post_transfer_journal(VentureBankMatchService *self, VentureEntity *transfer, VentureEntity *from_bank,
	VentureEntity *to_bank, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) amount = NULL, fee = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	g_autofree gchar *currency = NULL, *key = NULL, *day = NULL;
	VentureJournalLine *line;
	g_object_get(transfer, "amount", &amount, "fee", &fee, "date", &date, NULL);
	g_object_get(from_bank, "currency", &currency, NULL);
	day = iso_day(date);
	key = g_strdup_printf("bank_transfer:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s:%" G_GINT64_FORMAT,
		venture_entity_get_organization_id(transfer), venture_entity_get_id(from_bank),
		venture_entity_get_id(to_bank), day, venture_money_get_amount(amount));
	g_object_set(header, "organization-id", venture_entity_get_organization_id(transfer),
		"source-type", "bank_transfer", "source-id", venture_entity_get_id(transfer),
		"occurred-at", date, "currency", currency, "memo", "Bank transfer",
		"rule-name", "bank_transfer", "posting-key", key, NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", number(to_bank, "account-id"), "side", VENTURE_LEDGER_SIDE_DEBIT,
		"amount", amount, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", number(from_bank, "account-id"), "side", VENTURE_LEDGER_SIDE_CREDIT,
		"amount", amount, NULL);
	g_ptr_array_add(lines, line);
	if (fee != NULL && !venture_money_is_zero(fee))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(GPtrArray) accounts = NULL;
		venture_query_set_organization(query, venture_entity_get_organization_id(transfer));
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "6900", error))
			return FALSE;
		accounts = venture_database_find(self->database, query, error);
		if (accounts == NULL || accounts->len == 0) return refuse(error, "fee posting requires expense account 6900");
		line = venture_journal_line_new();
		g_object_set(line, "account-id", venture_entity_get_id(g_ptr_array_index(accounts, 0)),
			"side", VENTURE_LEDGER_SIDE_DEBIT, "amount", fee, NULL);
		g_ptr_array_add(lines, line);
		line = venture_journal_line_new();
		g_object_set(line, "account-id", number(from_bank, "account-id"), "side", VENTURE_LEDGER_SIDE_CREDIT,
			"amount", fee, NULL);
		g_ptr_array_add(lines, line);
	}
	posted = venture_posting_service_post(venture_database_get_posting_service(self->database),
		header, lines, NULL, actor, error);
	return posted != NULL;
}

static gboolean
link_transfer_txn(VentureBankMatchService *self, VentureEntity *transaction, VentureEntity *transfer,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) match = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) cleared = NULL;
	if (!unlocked(self, transaction, error)) return FALSE;
	if (!state_is(transaction, "unmatched"))
		return refuse(error, "transfer matching requires unmatched statement lines");
	g_object_get(transaction, "amount", &amount, "date", &cleared, NULL);
	match = new_record(VENTURE_TYPE_BANK_MATCH, venture_entity_get_organization_id(transaction));
	g_object_set(match, "transaction-id", venture_entity_get_id(transaction), "record-type", "bank_transfer",
		"record-id", venture_entity_get_id(transfer), "amount", amount, "kind", "transfer",
		"created-by", actor != NULL ? actor->name : "system", "cleared-at", cleared, NULL);
	if (!save_owned(self, match, actor, error)) return FALSE;
	g_object_set(transaction, "state", "matched", "match-id", venture_entity_get_id(match), NULL);
	return save_owned(self, transaction, actor, error);
}

static gboolean
create_transfer(VentureBankMatchService *self, VentureEntity *from_bank, JsonObject *args,
	const VentureActor *actor, GError **error)
{
	gint64 dest_id = option_id(args, "counterparty_bank_account_id");
	g_autoptr(VentureEntity) to_bank = NULL, transfer = NULL;
	g_autoptr(VentureMoney) amount = NULL, fee = NULL;
	g_autoptr(GDateTime) date = date_parse(option(args, "date"));
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_TRANSFER);
	g_autoptr(GPtrArray) found = NULL;
	g_autofree gchar *currency = NULL, *key = NULL, *day = NULL;
	gint64 org = venture_entity_get_organization_id(from_bank);
	if (dest_id <= 0 || option(args, "amount") == NULL)
		return refuse(error, "transfer requires counterparty_bank_account_id and amount");
	if (date == NULL) date = g_date_time_new_now_utc();
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, date, error)) return FALSE;
	to_bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, dest_id, error);
	if (to_bank == NULL) return FALSE;
	if (venture_entity_get_organization_id(to_bank) != org)
		return refuse(error, "transfer crosses organizations");
	if (venture_entity_get_id(from_bank) == dest_id)
		return refuse(error, "transfer requires two bank accounts");
	g_object_get(from_bank, "currency", &currency, NULL);
	amount = venture_money_from_string(option(args, "amount"), currency, error);
	if (amount == NULL) return FALSE;
	if (option(args, "fee") != NULL)
	{
		fee = venture_money_from_string(option(args, "fee"), currency, error);
		if (fee == NULL) return FALSE;
	}
	day = iso_day(date);
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s:%" G_GINT64_FORMAT,
		org, venture_entity_get_id(from_bank), dest_id, day, venture_money_get_amount(amount));
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_string(query, "transfer-key", VENTURE_FILTER_OP_EQ, key, error))
		return FALSE;
	found = venture_database_find(self->database, query, error);
	if (found == NULL) return FALSE;
	if (found->len != 0) return refuse(error, "transfer already exists");
	transfer = new_record(VENTURE_TYPE_BANK_TRANSFER, org);
	g_object_set(transfer, "from-bank-account-id", venture_entity_get_id(from_bank),
		"to-bank-account-id", dest_id, "amount", amount, "fee", fee, "date", date,
		"cleared-at", date, "memo", option(args, "memo"), "transfer-key", key, "state", "posted", NULL);
	if (!save_owned(self, transfer, actor, error)) return FALSE;
	if (!post_transfer_journal(self, transfer, from_bank, to_bank, actor, error)) return FALSE;
	if (option_id(args, "from_transaction_id") > 0)
	{
		g_autoptr(VentureEntity) txn = venture_database_get(self->database, VENTURE_TYPE_BANK_TRANSACTION,
			option_id(args, "from_transaction_id"), error);
		if (txn == NULL) return FALSE;
		if (!link_transfer_txn(self, txn, transfer, actor, error)) return FALSE;
	}
	if (option_id(args, "to_transaction_id") > 0)
	{
		g_autoptr(VentureEntity) txn = venture_database_get(self->database, VENTURE_TYPE_BANK_TRANSACTION,
			option_id(args, "to_transaction_id"), error);
		if (txn == NULL) return FALSE;
		if (!link_transfer_txn(self, txn, transfer, actor, error)) return FALSE;
	}
	return TRUE;
}


static VentureEntity *
import_feed(VentureBankMatchService *self, VentureEntity *bank, JsonObject *args, const VentureActor *actor, GError **error)
{
	JsonNode *node = args != NULL ? json_object_get_member(args, "transactions") : NULL;
	JsonArray *lines;
	g_autoptr(GDateTime) start = date_parse(option(args, "period_start"));
	g_autoptr(GDateTime) end = date_parse(option(args, "period_end"));
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(VentureMoney) opening = NULL;
	g_autofree gchar *currency = NULL, *hash = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	guint i, count = 0;
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node) || start == NULL || end == NULL)
	{
		refuse(error, "feed import needs transactions and a period");
		return NULL;
	}
	if (option(args, "period_end") != NULL && strlen(option(args, "period_end")) == 10)
	{
		GDateTime *inclusive = g_date_time_add(end, G_TIME_SPAN_DAY - 1);
		g_date_time_unref(end);
		end = inclusive;
	}
	lines = json_node_get_array(node);
	g_object_get(bank, "currency", &currency, NULL);
	opening = venture_money_new_zero(currency);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "bankfeed", -1);
	statement = new_record(VENTURE_TYPE_BANK_STATEMENT, venture_entity_get_organization_id(bank));
	g_object_set(statement, "bank-account-id", venture_entity_get_id(bank), "period-start", start, "period-end", end,
		"opening-balance", opening, "closing-balance", opening, "source-file-hash", hash, "imported-at", now, NULL);
	if (!save_owned(self, statement, actor, error)) return NULL;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *row = json_array_get_object_element(lines, i);
		gboolean stored = FALSE;
		const gchar *date, *amount, *description, *reference, *external;
		if (row == NULL)
		{
			refuse(error, "feed transaction must be an object");
			return NULL;
		}
		date = venture_json_object_get_string(row, "date", NULL);
		amount = venture_json_object_get_string(row, "amount", NULL);
		description = venture_json_object_get_string(row, "description", "Bank transaction");
		reference = venture_json_object_get_string(row, "reference", NULL);
		external = venture_json_object_get_string(row, "external_id", NULL);
		if (!import_line(self, bank, statement, date, amount, description, reference, external, "%Y-%m-%d", FALSE, &stored, actor, error))
			return NULL;
		if (stored) count++;
	}
	if (count == 0)
	{
		self->writing = statement;
		if (!venture_database_delete(self->database, statement, actor, error))
		{
			self->writing = NULL;
			return NULL;
		}
		self->writing = NULL;
		return g_object_ref(bank);
	}
	return g_steal_pointer(&statement);
}

static GType
action_type(const gchar *action)
{
	if (!strcmp(action, "import") || !strcmp(action, "map") || !strcmp(action, "inbox") ||
		!strcmp(action, "bulk") || !strcmp(action, "transfer") || !strcmp(action, "feed"))
		return VENTURE_TYPE_BANK_ACCOUNT;
	if (!strcmp(action, "auto") || !strcmp(action, "reconcile") || !strcmp(action, "reopen"))
		return VENTURE_TYPE_BANK_STATEMENT;
	if (!strcmp(action, "preview") || !strcmp(action, "enable"))
		return VENTURE_TYPE_BANK_RULE;
	return VENTURE_TYPE_BANK_TRANSACTION;
}


VentureEntity *
venture_bank_match_service_execute(VentureBankMatchService *self, const gchar *action, gint64 id,
	JsonObject *args, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) record = NULL, result = NULL;
	GType type;
	gboolean ok = FALSE;
	if (self->database == NULL) { refuse(error, "database is unavailable"); return NULL; }
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_transaction") == G_TYPE_INVALID)
	{ refuse(error, "banking module is disabled"); return NULL; }
	if (action == NULL) { refuse(error, "action is required"); return NULL; }
	type = action_type(action);
	if (!venture_database_begin(self->database, error)) return NULL;
	record = venture_database_get(self->database, type, id, error);
	if (record == NULL) goto finish;
	if (!strcmp(action, "import"))
	{
		result = import_statement(self, record, args, actor, error);
		ok = result != NULL;
	}
	else if (!strcmp(action, "feed"))
	{
		result = import_feed(self, record, args, actor, error);
		ok = result != NULL;
	}
	else if (!strcmp(action, "reconcile"))
	{
		result = reconcile(self, record, g_strcmp0(option(args, "state"), "open") != 0, actor, error);
		ok = result != NULL;
	}
	else if (!strcmp(action, "reopen"))
	{
		result = reopen_reconciliation(self, record, args, actor, error);
		ok = result != NULL;
	}
	else if (!strcmp(action, "auto")) ok = auto_match(self, record, actor, error);
	else if (!strcmp(action, "preview")) ok = preview_rule(self, record, actor, error);
	else if (!strcmp(action, "enable")) ok = enable_rule(self, record, actor, error);
	else if (!strcmp(action, "inbox")) ok = review_account(self, record, FALSE, args, actor, error);
	else if (!strcmp(action, "bulk")) ok = review_account(self, record, TRUE, args, actor, error);
	else if (!strcmp(action, "map")) ok = map_csv(self, record, args, actor, error);
	else if (!strcmp(action, "transfer")) ok = create_transfer(self, record, args, actor, error);
	else if (!unlocked(self, record, error)) goto finish;
	else if (!strcmp(action, "match"))
	{
		JsonNode *parts = args != NULL ? json_object_get_member(args, "parts") : NULL;
		ok = match_records(self, record, parts != NULL && JSON_NODE_HOLDS_ARRAY(parts) ? json_node_get_array(parts) : NULL, actor, error);
	}
	else if (!strcmp(action, "unmatch") || !strcmp(action, "exclude"))
	{
		g_autoptr(GPtrArray) matches = NULL;
		guint i;
		const gchar *reason = option(args, "reason");
		if (!strcmp(action, "exclude") && (reason == NULL || *reason == '\0'))
		{ refuse(error, "exclusion requires a reason"); goto finish; }
		matches = rows(self->database, VENTURE_TYPE_BANK_MATCH, venture_entity_get_organization_id(record), "transaction-id", id, error);
		if (matches == NULL) goto finish;
		for (i = 0; i < matches->len; i++)
		{
			VentureEntity *match = g_ptr_array_index(matches, i);
			self->writing = match;
			ok = venture_database_delete(self->database, match, actor, error);
			self->writing = NULL;
			if (!ok) goto finish;
		}
		g_object_set(record, "state", !strcmp(action, "exclude") ? "excluded" : "unmatched",
			"match-id", (gint64)0, "exclusion-reason", !strcmp(action, "exclude") ? reason : NULL, NULL);
		ok = save_owned(self, record, actor, error);
	}
	else if (!strcmp(action, "create")) ok = create_document(self, record, args, actor, error);
	else if (!strcmp(action, "reverse")) ok = reverse_categorization(self, record, actor, error);
	else if (!strcmp(action, "correct")) ok = correct_transaction(self, record, args, actor, error);
	else refuse(error, "unknown bank action");
finish:
	if (!ok)
	{
		if (error != NULL && *error == NULL) refuse(error, "required record was not found");
		venture_database_rollback(self->database);
		return NULL;
	}
	if (!venture_database_commit(self->database, error)) return NULL;
	return result != NULL ? g_steal_pointer(&result) : g_steal_pointer(&record);
}

static VentureReportResult *
bank_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureEntity) statement = NULL, bank = NULL;
	g_autoptr(GPtrArray) transactions = NULL;
	g_autoptr(VentureMoney) matched = NULL, unmatched = NULL, excluded = NULL;
	g_autoptr(VentureMoney) book = NULL, closing = NULL, difference = NULL, checks = NULL, deposits = NULL;
	g_autofree gchar *evidence = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *currency = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	gint64 id = options != NULL ? venture_json_object_get_int(options, "statement_id", 0) : 0;
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i;
	if (org == 0) org = venture_context_get_default_organization_id(context);
	statement = venture_database_get(db, VENTURE_TYPE_BANK_STATEMENT, id, error);
	if (statement == NULL) return NULL;
	if (venture_entity_get_organization_id(statement) != org)
	{ refuse(error, "statement belongs to another organization"); return NULL; }
	bank = venture_database_get(db, VENTURE_TYPE_BANK_ACCOUNT, number(statement, "bank-account-id"), error);
	if (bank == NULL) return NULL;
	g_object_get(bank, "currency", &currency, NULL);
	g_object_get(statement, "period-end", &end, "closing-balance", &closing, NULL);
	matched = venture_money_new_zero(currency); unmatched = venture_money_new_zero(currency); excluded = venture_money_new_zero(currency);
	transactions = rows(db, VENTURE_TYPE_BANK_TRANSACTION, org, "statement-id", id, error);
	if (transactions == NULL) return NULL;
	for (i = 0; i < transactions->len; i++)
	{
		VentureEntity *transaction = g_ptr_array_index(transactions, i);
		g_autoptr(VentureMoney) amount = NULL;
		VentureMoney **total = state_is(transaction, "matched") ? &matched : state_is(transaction, "excluded") ? &excluded : &unmatched;
		VentureMoney *next;
		g_object_get(transaction, "amount", &amount, NULL);
		next = venture_money_add(*total, amount, error);
		if (next == NULL) return NULL;
		venture_money_free(*total); *total = next;
	}
	book = venture_posting_service_account_balance(venture_database_get_posting_service(db), number(bank, "account-id"), org, currency, end, error);
	if (book == NULL) return NULL;
	difference = venture_money_subtract(closing, book, error);
	if (difference == NULL) return NULL;
	if (!outstanding_items(venture_database_get_bank_match_service(db), bank, NULL, end, &checks, &deposits, &evidence, error))
		return NULL;
	result = venture_report_result_new("Bank reconciliation", period);
	venture_report_result_add_metric(result, venture_metric_new_money("matched", "Matched", matched));
	venture_report_result_add_metric(result, venture_metric_new_money("unmatched", "Unmatched", unmatched));
	venture_report_result_add_metric(result, venture_metric_new_money("excluded", "Excluded", excluded));
	venture_report_result_add_metric(result, venture_metric_new_money("statement", "Statement balance", closing));
	venture_report_result_add_metric(result, venture_metric_new_money("book", "Posted book balance", book));
	venture_report_result_add_metric(result, venture_metric_new_money("outstanding_checks", "Outstanding checks", checks));
	venture_report_result_add_metric(result, venture_metric_new_money("deposits_in_transit", "Deposits in transit", deposits));
	venture_report_result_add_metric(result, venture_metric_new_money("difference", "Difference", difference));
	return g_steal_pointer(&result);
}

void
venture_bank_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"bank_reconciliation", "Bank reconciliation", "Statement evidence and posted balance; requires statement_id.", bank_report)));
}
