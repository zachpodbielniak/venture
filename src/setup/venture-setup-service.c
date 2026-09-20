/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureSetupService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureSetupService, venture_setup_service, G_TYPE_OBJECT)

static const struct {
	const gchar *classification;
	const gchar *code;
	const gchar *name;
	VentureAccountKind kind;
} default_chart[] = {
	{ "cash", "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET },
	{ "receivables", "1100", "Accounts receivable", VENTURE_ACCOUNT_KIND_ASSET },
	{ "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET },
	{ "payables", "2000", "Accounts payable", VENTURE_ACCOUNT_KIND_LIABILITY },
	{ "tax", "2100", "Sales tax payable", VENTURE_ACCOUNT_KIND_LIABILITY },
	{ "deferred", "2200", "Deferred revenue", VENTURE_ACCOUNT_KIND_LIABILITY },
	{ "retained_earnings", "3000", "Owner's equity", VENTURE_ACCOUNT_KIND_EQUITY },
	{ "owner_draws", "3100", "Owner's draw", VENTURE_ACCOUNT_KIND_EQUITY },
	{ "loans", "2500", "Notes payable", VENTURE_ACCOUNT_KIND_LIABILITY },
	{ "income", "4000", "Sales", VENTURE_ACCOUNT_KIND_INCOME },
	{ "expense", "6900", "General expenses", VENTURE_ACCOUNT_KIND_EXPENSE },
	{ "clearing", "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET }
};

static const gchar *const required_classes[] = {
	"cash", "receivables", "payables", "tax", "retained_earnings", "income", "expense", NULL
};

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_SETUP_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureSetupService *self = VENTURE_SETUP_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureSetupService *self = VENTURE_SETUP_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_setup_service_parent_class)->finalize(object);
}

static void
venture_setup_service_class_init(VentureSetupServiceClass *klass)
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
venture_setup_service_init(VentureSetupService *self)
{
	(void)self;
}

VentureSetupService *
venture_setup_service_get(VentureDatabase *database)
{
	VentureSetupService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-setup-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_SETUP_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-setup-service", self, g_object_unref);
	}
	return self;
}

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s", message);
	return FALSE;
}

static gboolean
maps_registered(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"accounting_control_map") != G_TYPE_INVALID;
}

static gboolean
setup_registered(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"accounting_setup") != G_TYPE_INVALID;
}

static gboolean
save_owned(VentureSetupService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static VentureAccountKind
kind_for(const gchar *classification)
{
	if (g_str_equal(classification, "payables") || g_str_equal(classification, "tax") ||
		g_str_equal(classification, "deferred") || g_str_equal(classification, "loans"))
		return VENTURE_ACCOUNT_KIND_LIABILITY;
	if (g_str_equal(classification, "retained_earnings") || g_str_equal(classification, "owner_draws"))
		return VENTURE_ACCOUNT_KIND_EQUITY;
	if (g_str_equal(classification, "income"))
		return VENTURE_ACCOUNT_KIND_INCOME;
	if (g_str_equal(classification, "expense"))
		return VENTURE_ACCOUNT_KIND_EXPENSE;
	return VENTURE_ACCOUNT_KIND_ASSET;
}

static const gchar *
label_for(const gchar *classification)
{
	if (g_str_equal(classification, "cash"))
		return "cash";
	if (g_str_equal(classification, "receivables"))
		return "accounts receivable";
	if (g_str_equal(classification, "payables"))
		return "accounts payable";
	if (g_str_equal(classification, "tax"))
		return "tax payable";
	if (g_str_equal(classification, "retained_earnings"))
		return "retained earnings";
	if (g_str_equal(classification, "clearing"))
		return "clearing";
	if (g_str_equal(classification, "inventory"))
		return "inventory";
	if (g_str_equal(classification, "income"))
		return "income";
	if (g_str_equal(classification, "expense"))
		return "expense";
	return classification;
}

static const gchar *
kind_label(VentureAccountKind kind)
{
	switch (kind)
	{
	case VENTURE_ACCOUNT_KIND_ASSET: return "asset";
	case VENTURE_ACCOUNT_KIND_LIABILITY: return "liability";
	case VENTURE_ACCOUNT_KIND_EQUITY: return "equity";
	case VENTURE_ACCOUNT_KIND_INCOME: return "income";
	case VENTURE_ACCOUNT_KIND_EXPENSE: return "expense";
	default: return "account";
	}
}

static gboolean
validate_mapped_account(VentureDatabase *database, gint64 organization_id,
	gint64 account_id, const gchar *classification, GError **error)
{
	g_autoptr(VentureEntity) account = NULL;
	gboolean active = FALSE;
	gint kind = 0;
	account = venture_database_get(database, VENTURE_TYPE_ACCOUNT, account_id, error);
	if (account == NULL)
		return FALSE;
	g_object_get(account, "active", &active, "kind", &kind, NULL);
	if (!active || kind != (gint)kind_for(classification) ||
		venture_entity_get_organization_id(account) != organization_id)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The %s control account must be an active %s account in this legal entity",
			label_for(classification), kind_label(kind_for(classification)));
		return FALSE;
	}
	return TRUE;
}

static GPtrArray *
load_maps(VentureDatabase *database, gint64 organization_id, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	if (!maps_registered())
		return g_ptr_array_new_with_free_func(g_object_unref);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CONTROL_MAP);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	return venture_database_find(database, query, error);
}

static gboolean
subject_matches(VentureEntity *map, const gchar *subject_type, gint64 subject_id)
{
	g_autofree gchar *actual = NULL;
	gint64 id = 0;
	g_object_get(map, "subject-type", &actual, "subject-id", &id, NULL);
	if (actual == NULL || actual[0] == '\0' || g_str_equal(actual, "organization"))
		return (subject_type == NULL || subject_type[0] == '\0' ||
			g_str_equal(subject_type, "organization")) && subject_id == 0 && id == 0;
	return g_strcmp0(actual, subject_type) == 0 && id == subject_id;
}

static gboolean
effective_ok(VentureEntity *map, GDateTime *as_of)
{
	g_autoptr(GDateTime) from = NULL;
	g_object_get(map, "effective-from", &from, NULL);
	if (from == NULL || as_of == NULL)
		return from == NULL || as_of == NULL || g_date_time_compare(from, as_of) <= 0;
	return g_date_time_compare(from, as_of) <= 0;
}

static gint
effective_rank(VentureEntity *map)
{
	g_autoptr(GDateTime) from = NULL;
	g_object_get(map, "effective-from", &from, NULL);
	return from == NULL ? 0 : 1;
}

static VentureEntity *
pick_map(GPtrArray *maps, const gchar *classification, const gchar *subject_type,
	gint64 subject_id, GDateTime *as_of)
{
	VentureEntity *best = NULL;
	guint i;
	for (i = 0; i < maps->len; i++)
	{
		VentureEntity *map = g_ptr_array_index(maps, i);
		g_autofree gchar *role = NULL;
		g_object_get(map, "classification", &role, NULL);
		if (g_strcmp0(role, classification) != 0 || !subject_matches(map, subject_type, subject_id) ||
			!effective_ok(map, as_of))
			continue;
		if (best == NULL || effective_rank(map) > effective_rank(best))
			best = map;
		else if (best != NULL && effective_rank(map) == effective_rank(best))
		{
			g_autoptr(GDateTime) a = NULL;
			g_autoptr(GDateTime) b = NULL;
			g_object_get(map, "effective-from", &a, NULL);
			g_object_get(best, "effective-from", &b, NULL);
			if (a != NULL && (b == NULL || g_date_time_compare(a, b) > 0))
				best = map;
		}
	}
	return best;
}

gint64
venture_setup_resolve_account(VentureDatabase *database, gint64 organization_id,
	const gchar *classification, const gchar *subject_type, gint64 subject_id,
	GDateTime *as_of, GError **error)
{
	g_autoptr(GPtrArray) maps = NULL;
	VentureEntity *map;
	gint64 account_id = 0;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), 0);
	if (classification == NULL || organization_id <= 0 || !maps_registered())
		return 0;
	maps = load_maps(database, organization_id, error);
	if (maps == NULL)
		return 0;
	map = pick_map(maps, classification, subject_type, subject_id, as_of);
	if (map == NULL && subject_type != NULL && !g_str_equal(subject_type, "organization"))
		map = pick_map(maps, classification, "organization", 0, as_of);
	if (map == NULL)
		return 0;
	g_object_get(map, "account-id", &account_id, NULL);
	if (!validate_mapped_account(database, organization_id, account_id, classification, error))
		return 0;
	return account_id;
}

gboolean
venture_setup_account_classified(VentureDatabase *database, gint64 organization_id,
	gint64 account_id, const gchar *classification, GDateTime *as_of)
{
	g_autoptr(GPtrArray) maps = NULL;
	guint i;
	guint depth;
	if (database == NULL || account_id <= 0 || classification == NULL || !maps_registered())
		return FALSE;
	maps = load_maps(database, organization_id, NULL);
	if (maps == NULL)
		return FALSE;
	for (depth = 0; account_id != 0 && depth < 32; depth++)
	{
		g_autoptr(VentureEntity) account = NULL;
		gint64 parent = 0;
		for (i = 0; i < maps->len; i++)
		{
			VentureEntity *map = g_ptr_array_index(maps, i);
			g_autofree gchar *role = NULL;
			gint64 mapped = 0;
			g_object_get(map, "classification", &role, "account-id", &mapped, NULL);
			if (g_strcmp0(role, classification) == 0 && mapped == account_id &&
				effective_ok(map, as_of))
				return TRUE;
		}
		account = venture_database_get(database, VENTURE_TYPE_ACCOUNT, account_id, NULL);
		if (account == NULL)
			return FALSE;
		g_object_get(account, "parent-id", &parent, NULL);
		account_id = parent;
	}
	return FALSE;
}

static gint64
find_account_code(VentureDatabase *database, gint64 organization_id, const gchar *code, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) found = NULL;
	g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", organization_id, code);
	venture_query_set_organization(query, organization_id);
	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return 0;
	found = venture_database_find_one(database, query, error);
	if (found != NULL)
		return venture_entity_get_id(found);
	if (error != NULL && *error != NULL)
		return 0;
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, organization_id);
	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped, error))
		return 0;
	found = venture_database_find_one(database, query, error);
	return found != NULL ? venture_entity_get_id(found) : 0;
}

static gint64
ensure_account(VentureSetupService *self, gint64 organization_id, const gchar *code,
	const gchar *name, VentureAccountKind kind, const VentureActor *actor, GError **error)
{
	gint64 id = find_account_code(self->database, organization_id, code, error);
	g_autoptr(VentureAccount) account = NULL;
	g_autofree gchar *scoped = NULL;
	if (id > 0 || (error != NULL && *error != NULL))
		return id;
	scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", organization_id, code);
	account = venture_account_new();
	g_object_set(account, "organization-id", organization_id, "code", scoped, "name", name,
		"kind", kind, "active", TRUE, NULL);
	if (!save_owned(self, VENTURE_ENTITY(account), actor, error))
		return 0;
	return venture_entity_get_id(VENTURE_ENTITY(account));
}

static gboolean
upsert_map(VentureSetupService *self, gint64 organization_id, const gchar *classification,
	gint64 account_id, const gchar *subject_type, gint64 subject_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) maps = NULL;
	g_autoptr(VentureAccountingControlMap) created = NULL;
	VentureEntity *existing;
	{
		g_autoptr(GError) local = NULL;
		if (!validate_mapped_account(self->database, organization_id, account_id, classification, &local))
			return TRUE;
	}
	maps = load_maps(self->database, organization_id, error);
	if (maps == NULL)
		return FALSE;
	existing = pick_map(maps, classification, subject_type, subject_id, NULL);
	if (existing != NULL)
		return TRUE;
	created = venture_accounting_control_map_new();
	g_object_set(created, "organization-id", organization_id, "classification", classification,
		"account-id", account_id, "subject-type", subject_type != NULL ? subject_type : "organization",
		"subject-id", subject_id, NULL);
	return save_owned(self, VENTURE_ENTITY(created), actor, error);
}

gboolean
venture_setup_seed_defaults(VentureDatabase *database, gint64 organization_id,
	const VentureActor *actor, GError **error)
{
	VentureSetupService *self;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	if (organization_id <= 0 || !maps_registered())
		return TRUE;
	self = venture_setup_service_get(database);
	for (i = 0; i < G_N_ELEMENTS(default_chart); i++)
	{
		gint64 id = ensure_account(self, organization_id, default_chart[i].code,
			default_chart[i].name, default_chart[i].kind, actor, error);
		if (id == 0)
			return FALSE;
		if (!upsert_map(self, organization_id, default_chart[i].classification, id,
			"organization", 0, actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
count_ok(VentureDatabase *database, GType type, gint64 organization_id, guint *out, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	gint64 n;
	if (type == G_TYPE_INVALID)
	{
		*out = 0;
		return TRUE;
	}
	query = venture_query_new(type);
	venture_query_set_organization(query, organization_id);
	n = venture_database_count(database, query, error);
	if (n < 0)
		return FALSE;
	*out = (guint)n;
	return TRUE;
}

static void
add_step(JsonArray *steps, const gchar *key, const gchar *label, gboolean done, const gchar *detail)
{
	JsonObject *step = json_object_new();
	json_object_set_string_member(step, "key", key);
	json_object_set_string_member(step, "label", label);
	json_object_set_boolean_member(step, "done", done);
	json_object_set_string_member(step, "detail", detail != NULL ? detail : "");
	json_array_add_object_element(steps, step);
}

static gchar *
missing_controls(VentureSetupService *self, gint64 organization_id, GError **error)
{
	GString *missing = g_string_new(NULL);
	gsize i;
	for (i = 0; required_classes[i] != NULL; i++)
	{
		gint64 id = venture_setup_resolve_account(self->database, organization_id,
			required_classes[i], "organization", 0, NULL, error);
		if (error != NULL && *error != NULL)
		{
			g_string_free(missing, TRUE);
			return NULL;
		}
		if (id == 0)
		{
			if (missing->len > 0)
				g_string_append(missing, ", ");
			g_string_append(missing, label_for(required_classes[i]));
		}
	}
	if (missing->len == 0)
	{
		g_string_free(missing, TRUE);
		return NULL;
	}
	return g_string_free(missing, FALSE);
}

JsonNode *
venture_setup_service_checklist(VentureSetupService *self, gint64 organization_id, GError **error)
{
	g_autoptr(JsonArray) steps = json_array_new();
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(GPtrArray) years = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *legal = NULL;
	g_autofree gchar *missing = NULL;
	g_autofree gchar *name = NULL;
	guint taxes = 0, accounts = 0, banks = 0, journals = 0;
	g_return_val_if_fail(VENTURE_IS_SETUP_SERVICE(self), NULL);
	org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	if (org == NULL)
		return NULL;
	g_object_get(org, "legal-name", &legal, "name", &name, "default-currency", &currency, NULL);
	add_step(steps, "legal_entity", "Legal entity",
		(legal != NULL && legal[0] != '\0') || (name != NULL && name[0] != '\0'),
		legal != NULL && legal[0] != '\0' ? legal : name);
	add_step(steps, "book_currency", "Book currency",
		currency != NULL && strlen(currency) == 3, currency);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "fiscal_year") != 0)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_YEAR);
		venture_query_set_organization(query, organization_id);
		years = venture_database_find(self->database, query, error);
		if (years == NULL)
			return NULL;
	}
	add_step(steps, "fiscal_year", "Fiscal year", years != NULL && years->len > 0,
		years != NULL && years->len > 0 ? "Calendar generated" : "No fiscal calendar");
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) setup = NULL;
		g_autofree gchar *basis = NULL;
		if (setup_registered())
		{
			query = venture_query_new(VENTURE_TYPE_ACCOUNTING_SETUP);
			venture_query_set_organization(query, organization_id);
			setup = venture_database_find_one(self->database, query, error);
			if (error != NULL && *error != NULL)
				return NULL;
			if (setup != NULL)
				g_object_get(setup, "basis", &basis, NULL);
		}
		add_step(steps, "basis", "Accounting basis",
			basis != NULL && (g_str_equal(basis, "accrual") || g_str_equal(basis, "cash")),
			basis != NULL ? basis : "Not chosen");
	}
	if (!count_ok(self->database, VENTURE_TYPE_TAX_CATEGORY, organization_id, &taxes, error) ||
		!count_ok(self->database, VENTURE_TYPE_ACCOUNT, organization_id, &accounts, error) ||
		!count_ok(self->database, venture_entity_registry_lookup(venture_entity_registry_get_default(),
			"bank_account"), organization_id, &banks, error) ||
		!count_ok(self->database, VENTURE_TYPE_JOURNAL, organization_id, &journals, error))
		return NULL;
	add_step(steps, "tax_profile", "Tax profile", taxes > 0,
		taxes > 0 ? "Deduction categories ready" : "No tax categories");
	add_step(steps, "chart_template", "Chart of accounts", accounts > 0,
		accounts > 0 ? "Chart in place" : "No accounts");
	add_step(steps, "banks", "Bank accounts", banks > 0,
		banks > 0 ? "Operating bank mapped" : "No bank");
	add_step(steps, "opening_balances", "Opening balances", TRUE,
		journals > 0 ? "Opening evidence posted or reviewed as zero" : "Zero opening, no journal");
	missing = missing_controls(self, organization_id, error);
	if (error != NULL && *error != NULL)
		return NULL;
	add_step(steps, "control_accounts", "Control accounts", missing == NULL,
		missing == NULL ? "Cash, AR, AP, tax, earnings and P&L mapped" : missing);
	{
		JsonNode *node = json_node_new(JSON_NODE_ARRAY);
		json_node_take_array(node, g_steal_pointer(&steps));
		return node;
	}
}

static const gchar *
payload_str(JsonObject *payload, const gchar *key, const gchar *fallback)
{
	const gchar *value = payload != NULL ? venture_json_object_get_string(payload, key, fallback) : fallback;
	return value != NULL ? value : fallback;
}

static VentureEntity *
load_setup(VentureSetupService *self, gint64 organization_id, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	if (!setup_registered())
		return NULL;
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_SETUP);
	venture_query_set_organization(query, organization_id);
	return venture_database_find_one(self->database, query, error);
}

VentureEntity *
venture_setup_service_preview(VentureSetupService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) setup = NULL;
	g_autoptr(JsonNode) checklist = NULL;
	g_autoptr(JsonNode) payload_node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) owned = NULL;
	g_autofree gchar *payload_text = NULL;
	g_autofree gchar *checklist_text = NULL;
	g_return_val_if_fail(VENTURE_IS_SETUP_SERVICE(self), NULL);
	if (organization_id <= 0)
	{
		refuse(error, "Accounting setup needs a legal entity");
		return NULL;
	}
	if (!setup_registered())
	{
		refuse(error, "The setup module is disabled");
		return NULL;
	}
	if (payload == NULL)
	{
		owned = json_object_new();
		payload = owned;
	}
	setup = load_setup(self, organization_id, error);
	if (error != NULL && *error != NULL)
		return NULL;
	if (setup == NULL)
		setup = VENTURE_ENTITY(venture_accounting_setup_new());
	json_node_set_object(payload_node, json_object_ref(payload));
	payload_text = venture_json_to_string(payload_node, FALSE);
	g_object_set(setup, "organization-id", organization_id, "state", "preview",
		"legal-name", payload_str(payload, "legal_name", NULL),
		"book-currency", payload_str(payload, "book_currency", NULL),
		"basis", payload_str(payload, "basis", NULL),
		"tax-profile", payload_str(payload, "tax_profile", "standard"),
		"chart-template", payload_str(payload, "chart_template", "standard"),
		"bank-name", payload_str(payload, "bank_name", NULL),
		"period-length", payload_str(payload, "period_length", "monthly"),
		"payload", payload_text, NULL);
	{
		const gchar *start = payload_str(payload, "fiscal_year_start", NULL);
		const gchar *cash = payload_str(payload, "opening_cash", NULL);
		if (start != NULL && start[0] != '\0')
		{
			g_autoptr(GDateTime) when = venture_time_from_string(start, NULL);
			if (when != NULL)
				g_object_set(setup, "fiscal-year-start", when, NULL);
		}
		if (cash != NULL && cash[0] != '\0')
		{
			g_autoptr(VentureMoney) amount = venture_money_from_string(cash, payload_str(payload, "book_currency", NULL), error);
			if (amount == NULL)
				return NULL;
			g_object_set(setup, "opening-cash", amount, NULL);
		}
	}
	if (!save_owned(self, setup, actor, error))
		return NULL;
	checklist = venture_setup_service_checklist(self, organization_id, error);
	if (checklist == NULL)
		return NULL;
	checklist_text = venture_json_to_string(checklist, FALSE);
	g_object_set(setup, "checklist", checklist_text, NULL);
	if (!save_owned(self, setup, actor, error))
		return NULL;
	return g_steal_pointer(&setup);
}

static JsonObject *
setup_payload(VentureEntity *setup)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_object_get(setup, "payload", &text, NULL);
	if (text == NULL || text[0] == '\0')
		return json_object_new();
	node = venture_json_parse(text, NULL);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return json_object_new();
	return json_object_ref(json_node_get_object(node));
}

static gboolean
ensure_tax_profile(VentureSetupService *self, gint64 organization_id,
	const VentureActor *actor, GError **error)
{
	guint n = 0;
	g_autoptr(VentureTaxCategory) category = NULL;
	if (!count_ok(self->database, VENTURE_TYPE_TAX_CATEGORY, organization_id, &n, error))
		return FALSE;
	if (n > 0)
		return TRUE;
	category = venture_tax_category_new();
	g_object_set(category, "organization-id", organization_id, "name", "Ordinary expense",
		"code", "ORDINARY", "deductibility", VENTURE_DEDUCTIBILITY_FULL, NULL);
	return save_owned(self, VENTURE_ENTITY(category), actor, error);
}

static gboolean
post_opening(VentureSetupService *self, VentureEntity *setup, gint64 cash, gint64 equity,
	const VentureMoney *amount, GDateTime *when, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *transaction = NULL;
	VentureLedgerEntry *debit;
	VentureLedgerEntry *credit;
	if (amount == NULL || venture_money_is_zero(amount))
		return TRUE;
	transaction = g_strdup_printf("setup:opening:%s", venture_entity_get_uuid(setup));
	debit = venture_ledger_entry_new();
	credit = venture_ledger_entry_new();
	g_object_set(debit, "organization-id", venture_entity_get_organization_id(setup),
		"transaction-id", transaction, "account-id", cash, "side", VENTURE_LEDGER_SIDE_DEBIT,
		"amount", amount, "occurred-at", when, "source-type", "accounting_setup",
		"source-id", venture_entity_get_id(setup), NULL);
	g_object_set(credit, "organization-id", venture_entity_get_organization_id(setup),
		"transaction-id", transaction, "account-id", equity, "side", VENTURE_LEDGER_SIDE_CREDIT,
		"amount", amount, "occurred-at", when, "source-type", "accounting_setup",
		"source-id", venture_entity_get_id(setup), NULL);
	g_ptr_array_add(entries, debit);
	g_ptr_array_add(entries, credit);
	return venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
		entries, NULL, actor, error);
}

static gboolean
venture_setup_service_complete_impl(VentureSetupService *self, VentureAccountingSetup *setup,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(JsonNode) checklist = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *checklist_text = NULL;
	g_autofree gchar *missing = NULL;
	gint64 organization_id;
	guint i;
	g_return_val_if_fail(VENTURE_IS_SETUP_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ACCOUNTING_SETUP(setup), FALSE);
	g_object_get(setup, "state", &state, NULL);
	if (g_strcmp0(state, "complete") == 0)
		return TRUE;
	organization_id = venture_entity_get_organization_id(VENTURE_ENTITY(setup));
	payload = setup_payload(VENTURE_ENTITY(setup));
	org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, error);
	if (org == NULL)
		return FALSE;
	original = g_object_new(G_OBJECT_TYPE(setup), NULL);
	venture_entity_copy_properties_from(original, VENTURE_ENTITY(setup), FALSE);
	if (!venture_database_begin(self->database, error))
		return FALSE;
	{
		const gchar *legal = payload_str(payload, "legal_name", NULL);
		const gchar *currency = payload_str(payload, "book_currency", "USD");
		const gchar *basis = payload_str(payload, "basis", "accrual");
		const gchar *bank_name = payload_str(payload, "bank_name", "Operating account");
		const gchar *length_nick = payload_str(payload, "period_length", "monthly");
		const gchar *start_text = payload_str(payload, "fiscal_year_start", "2026-01-01");
		g_autoptr(GDateTime) start = venture_time_from_string(start_text, NULL);
		g_autoptr(VentureMoney) opening = NULL;
		g_autofree gchar *cash_text = NULL;
		gint64 cash = 0, equity = 0;
		if (!venture_currency_is_valid(currency))
		{
			venture_database_rollback(self->database);
			return refuse(error, "Choose a three-letter book currency before opening the books");
		}
		if (g_strcmp0(basis, "accrual") != 0 && g_strcmp0(basis, "cash") != 0)
		{
			venture_database_rollback(self->database);
			return refuse(error, "Choose cash or accrual basis before posting the first invoice");
		}
		if (start == NULL)
		{
			venture_database_rollback(self->database);
			return refuse(error, "A fiscal year needs an opening date");
		}
		if (legal != NULL && legal[0] != '\0')
			g_object_set(org, "legal-name", legal, NULL);
		g_object_set(org, "default-currency", currency, NULL);
		if (!save_owned(self, org, actor, error))
			goto fail;
		for (i = 0; i < G_N_ELEMENTS(default_chart); i++)
		{
			gint64 id = ensure_account(self, organization_id, default_chart[i].code,
				default_chart[i].name, default_chart[i].kind, actor, error);
			if (id == 0 || !upsert_map(self, organization_id, default_chart[i].classification, id,
				"organization", 0, actor, error))
				goto fail;
		}
		if (!ensure_tax_profile(self, organization_id, actor, error))
			goto fail;
		if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "fiscal_year") != 0)
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_YEAR);
			g_autoptr(GPtrArray) years = NULL;
			VenturePeriodLength length = g_str_equal(length_nick, "quarterly") ?
				VENTURE_PERIOD_QUARTERLY : VENTURE_PERIOD_MONTHLY;
			venture_query_set_organization(query, organization_id);
			years = venture_database_find(self->database, query, error);
			if (years == NULL)
				goto fail;
			if (years->len == 0 &&
				venture_period_service_generate(venture_period_service_get(self->database),
					organization_id, "FY", start, length, actor, error) == NULL)
				goto fail;
		}
		cash = venture_setup_resolve_account(self->database, organization_id, "cash",
			"organization", 0, NULL, error);
		equity = venture_setup_resolve_account(self->database, organization_id, "retained_earnings",
			"organization", 0, NULL, error);
		if (cash == 0 || equity == 0)
			goto fail;
		if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_account") != 0)
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_ACCOUNT);
			g_autoptr(GPtrArray) banks = NULL;
			venture_query_set_organization(query, organization_id);
			banks = venture_database_find(self->database, query, error);
			if (banks == NULL)
				goto fail;
			if (banks->len == 0)
			{
				g_autoptr(VentureBankAccount) bank = venture_bank_account_new();
				g_object_set(bank, "organization-id", organization_id, "name", bank_name,
					"account-id", cash, "currency", currency, "date-column", "Date",
					"amount-column", "Amount", "description-column", "Memo",
					"reference-column", "Ref", "external-id-column", "ID",
					"date-format", "%Y-%m-%d", "sign-convention", "normal", NULL);
				if (!save_owned(self, VENTURE_ENTITY(bank), actor, error) ||
					!upsert_map(self, organization_id, "cash", cash, "bank_account",
						venture_entity_get_id(VENTURE_ENTITY(bank)), actor, error))
					goto fail;
			}
			else
				(void)banks;
		}
		g_object_get(setup, "opening-cash", &opening, NULL);
		cash_text = g_strdup(payload_str(payload, "opening_cash", NULL));
		if (opening == NULL && cash_text != NULL && cash_text[0] != '\0')
			opening = venture_money_from_string(cash_text, currency, error);
		if (error != NULL && *error != NULL)
			goto fail;
		if (opening != NULL && !g_str_equal(venture_money_get_currency(opening), currency))
		{
			venture_database_rollback(self->database);
			return refuse(error, "Opening cash must be in the book currency");
		}
		if (!post_opening(self, VENTURE_ENTITY(setup), cash, equity, opening, start, actor, error))
			goto fail;
		g_object_set(setup, "state", "complete", "book-currency", currency, "basis", basis,
			"legal-name", legal, "bank-name", bank_name, "fiscal-year-start", start, NULL);
		if (!save_owned(self, VENTURE_ENTITY(setup), actor, error))
			goto fail;
	}
	missing = missing_controls(self, organization_id, error);
	if (error != NULL && *error != NULL)
		goto fail;
	if (missing != NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Control accounts are not ready: map %s before the first invoice, bill or payment",
			missing);
		goto fail;
	}
	checklist = venture_setup_service_checklist(self, organization_id, error);
	if (checklist == NULL)
		goto fail;
	checklist_text = venture_json_to_string(checklist, FALSE);
	g_object_set(setup, "checklist", checklist_text, NULL);
	if (!save_owned(self, VENTURE_ENTITY(setup), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	/* Restore state and optimistic version too: a rolled-back Complete
	 * must not turn the next attempt into a false successful no-op. */
	venture_entity_copy_properties_from(VENTURE_ENTITY(setup), original, FALSE);
	return FALSE;
}

static gboolean
module_type_on(const gchar *entity_name)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), entity_name) != G_TYPE_INVALID;
}

static gboolean
has_control(VentureDatabase *database, gint64 organization_id, const gchar *role,
	const gchar *code, GError **error)
{
	g_autoptr(GError) local = NULL;
	gint64 id = venture_setup_resolve_account(database, organization_id, role,
		"organization", 0, NULL, &local);
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (id > 0)
		return TRUE;
	id = find_account_code(database, organization_id, code, error);
	return id > 0;
}

static gboolean
require_maps(VentureDatabase *database, VentureEntity *record, const gchar *document,
	const gchar *const *roles, const gchar *const *codes, GError **error)
{
	gint64 org = venture_entity_get_organization_id(record);
	gsize i;
	if (org <= 0 || !maps_registered())
		return TRUE;
	for (i = 0; roles[i] != NULL; i++)
	{
		if (has_control(database, org, roles[i], codes[i], error))
			continue;
		if (error != NULL && *error != NULL)
			return FALSE;
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Cannot record %s until the %s control account is mapped for this legal entity",
			document, label_for(roles[i]));
		return FALSE;
	}
	return TRUE;
}

gboolean
venture_setup_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name;
	VentureSetupService *self;
	if (record == NULL || database == NULL)
		return TRUE;
	name = venture_entity_get_entity_name(record);
	if (g_strcmp0(name, "accounting_setup") == 0)
	{
		self = venture_setup_service_get(database);
		if (self->writing == record)
		{
			self->writing = NULL;
			return TRUE;
		}
		return refuse(error, "Accounting setup must be changed through VentureSetupService");
	}
	if (g_strcmp0(name, "accounting_control_map") == 0)
	{
		gint64 account_id = 0;
		g_autofree gchar *classification = NULL;
		self = venture_setup_service_get(database);
		if (self->writing == record)
		{
			self->writing = NULL;
			return TRUE;
		}
		if (removal)
			return TRUE;
		g_object_get(record, "account-id", &account_id, "classification", &classification, NULL);
		return validate_mapped_account(database, venture_entity_get_organization_id(record),
			account_id, classification, error);
	}
	if (removal)
		return TRUE;
	if (g_strcmp0(name, "invoice") == 0 || g_strcmp0(name, "payment") == 0 ||
		g_strcmp0(name, "vendor_bill") == 0 || g_strcmp0(name, "bill_payment") == 0)
	{
		g_autoptr(VentureEntity) setup = NULL;
		g_autofree gchar *state = NULL;
		static const gchar *const invoice_roles[] = { "cash", "receivables", "income", "tax", NULL };
		static const gchar *const invoice_codes[] = { "1000", "1100", "4000", "2100", NULL };
		static const gchar *const receipt_roles[] = { "cash", "receivables", NULL };
		static const gchar *const receipt_codes[] = { "1000", "1100", NULL };
		static const gchar *const bill_roles[] = { "payables", "expense", NULL };
		static const gchar *const bill_codes[] = { "2000", "6900", NULL };
		static const gchar *const payout_roles[] = { "cash", "payables", NULL };
		static const gchar *const payout_codes[] = { "1000", "2000", NULL };
		if (setup_registered())
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_SETUP);
			venture_query_set_organization(query, venture_entity_get_organization_id(record));
			setup = venture_database_find_one(database, query, error);
			if (error != NULL && *error != NULL)
				return FALSE;
			if (setup != NULL)
				g_object_get(setup, "state", &state, NULL);
		}
		/* A checklist that has been started but not completed must finish
		 * before operational documents post. Legacy entities skip this. */
		if (setup == NULL || g_strcmp0(state, "complete") == 0)
			return TRUE;
		if (g_strcmp0(name, "invoice") == 0)
		{
			if (!module_type_on("invoice"))
				return refuse(error, "Cannot record an invoice because invoicing is disabled");
			return require_maps(database, record, "an invoice", invoice_roles, invoice_codes, error);
		}
		if (g_strcmp0(name, "payment") == 0)
		{
			if (!module_type_on("payment"))
				return refuse(error, "Cannot record a payment because receivables is disabled");
			return require_maps(database, record, "a payment", receipt_roles, receipt_codes, error);
		}
		if (!module_type_on("vendor_bill"))
			return refuse(error, "Cannot record a bill because payables is disabled");
		if (g_strcmp0(name, "bill_payment") == 0)
			return require_maps(database, record, "a payment", payout_roles, payout_codes, error);
		return require_maps(database, record, "a bill", bill_roles, bill_codes, error);
	}
	return TRUE;
}

static gboolean
setup_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
setup_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	VentureSetupService *service = venture_action_get_data(action);
	g_object_get(action, "name", &name, NULL);
	(void)params;
	if (g_strcmp0(name, "complete") == 0)
		return venture_setup_service_complete(service, VENTURE_ACCOUNTING_SETUP(entity), actor, error) ?
			g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "preview") == 0)
	{
		g_autoptr(VentureEntity) result = NULL;
		/* Preview persists the draft and then its checklist; both belong to
		 * this nonposting action even though no accounting consent is needed. */
		if (!venture_database_begin(service->database, error)) return NULL;
		result = venture_setup_service_preview(service, venture_entity_get_organization_id(entity),
			NULL, actor, error);
		if (result == NULL)
		{
			venture_database_rollback(service->database);
			return NULL;
		}
		if (!venture_database_commit(service->database, error)) return NULL;
		return g_steal_pointer(&result);
	}
	return NULL;
}

void
venture_setup_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	static const gchar *const names[] = { "preview", "complete" };
	guint i;
	if (!setup_registered())
		return;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "accounting_setup",
			"name", names[i], "label", names[i], "description", "Accounting setup action",
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		g_autoptr(GError) error = NULL;
		venture_action_registry_register(registry, action, setup_allowed, setup_invoke,
			venture_setup_service_get(database), NULL, &error);
	}
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_setup_service_complete(VentureSetupService *self, VentureAccountingSetup *setup,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(setup), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(db, "setup-complete", VENTURE_ENTITY(setup), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(setup)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_setup_service_complete_impl(self, setup, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}
