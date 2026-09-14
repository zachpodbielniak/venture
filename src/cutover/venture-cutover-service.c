/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureCutoverService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureCutoverService, venture_cutover_service, G_TYPE_OBJECT)

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_CUTOVER_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureCutoverService *self = VENTURE_CUTOVER_SERVICE(object);
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
	VentureCutoverService *self = VENTURE_CUTOVER_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_cutover_service_parent_class)->finalize(object);
}

static void
venture_cutover_service_class_init(VentureCutoverServiceClass *klass)
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
venture_cutover_service_init(VentureCutoverService *self)
{
	(void)self;
}

VentureCutoverService *
venture_cutover_service_get(VentureDatabase *database)
{
	VentureCutoverService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-cutover-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CUTOVER_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-cutover-service", self, g_object_unref);
	}
	return self;
}

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureCutoverService: %s", message);
	return FALSE;
}

static gboolean
save_owned(VentureCutoverService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static JsonObject *
payload_of(VentureEntity *cutover)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_object_get(cutover, "payload", &text, NULL);
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
		return NULL;
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static gboolean
add_row(VentureCutoverService *self, gint64 cutover_id, gint64 org, const gchar *source_id,
	const gchar *source_type, const gchar *status, const gchar *exception, const gchar *record_type,
	gint64 record_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingCutoverRow) row = venture_accounting_cutover_row_new();
	g_object_set(row, "cutover-id", cutover_id, "source-id", source_id, "source-type", source_type,
		"status", status, "exception", exception, "record-type", record_type, "record-id", record_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(row), org);
	return save_owned(self, VENTURE_ENTITY(row), actor, error);
}

static gint64
existing_record(VentureCutoverService *self, const gchar *source_id, const gchar *source_type, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, error))
		return -1;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *type = NULL;
		gint64 record_id = 0;
		g_object_get(g_ptr_array_index(rows, i), "source-type", &type, "record-id", &record_id, NULL);
		if (g_strcmp0(type, source_type) == 0 && record_id > 0)
			return record_id;
	}
	return 0;
}

static gint64
find_or_create_company(VentureCutoverService *self, gint64 org, const gchar *name, const gchar *source_id,
	const gchar *source_type, const VentureActor *actor, GError **error)
{
	gint64 existing;
	g_autoptr(VentureCompany) company = NULL;
	existing = existing_record(self, source_id, source_type, error);
	if (existing < 0)
		return 0;
	if (existing > 0)
		return existing;
	company = venture_company_new();
	g_object_set(company, "name", name, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), org);
	if (!save_owned(self, VENTURE_ENTITY(company), actor, error))
		return 0;
	return venture_entity_get_id(VENTURE_ENTITY(company));
}

static JsonArray *
arr(JsonObject *object, const gchar *name)
{
	return json_object_has_member(object, name) ? json_object_get_array_member(object, name) : NULL;
}

static const gchar *
obj_str(JsonObject *object, const gchar *name)
{
	return json_object_has_member(object, name) ? json_object_get_string_member(object, name) : NULL;
}

VentureEntity *
venture_cutover_service_preview(VentureCutoverService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingCutover) cutover = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	g_autofree gchar *payload_text = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	GString *report;
	JsonArray *unsupported;
	JsonArray *open_ar;
	const gchar *source;
	guint i;
	if (payload == NULL)
	{
		refuse(error, "mapped source payload is required");
		return NULL;
	}
	source = obj_str(payload, "source");
	if (g_strcmp0(source, "zoho_books") != 0 && g_strcmp0(source, "quickbooks") != 0)
	{
		refuse(error, "source must be zoho_books or quickbooks");
		return NULL;
	}
	cutoff = venture_time_from_string(obj_str(payload, "cutoff"), error);
	if (cutoff == NULL)
		return NULL;
	json_node_set_object(node, json_object_ref(payload));
	payload_text = venture_json_to_string(node, FALSE);
	report = g_string_new("Cutover preview\n");
	unsupported = arr(payload, "unsupported");
	if (unsupported != NULL)
	{
		g_string_append(report, "Unsupported source fields:\n");
		for (i = 0; i < json_array_get_length(unsupported); i++)
			g_string_append_printf(report, "- %s\n", json_array_get_string_element(unsupported, i));
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	cutover = venture_accounting_cutover_new();
	g_object_set(cutover, "source", source, "cutoff", cutoff, "state", "preview",
		"payload", payload_text, "reconciliation-report", report->str, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(cutover), organization_id);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error))
		goto fail;
	open_ar = arr(payload, "open_ar");
	if (open_ar != NULL)
	{
		for (i = 0; i < json_array_get_length(open_ar); i++)
		{
			JsonObject *row = json_array_get_object_element(open_ar, i);
			if (!add_row(self, venture_entity_get_id(VENTURE_ENTITY(cutover)), organization_id,
				obj_str(row, "source_id"), "open_ar", "preview", NULL, NULL, 0, actor, error))
				goto fail;
		}
	}
	g_string_free(report, TRUE);
	if (!venture_database_commit(self->database, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&cutover));
fail:
	g_string_free(report, TRUE);
	venture_database_rollback(self->database);
	return NULL;
}

static gboolean
import_open_ar(VentureCutoverService *self, VentureEntity *cutover, JsonObject *payload,
	const VentureActor *actor, GError **error)
{
	JsonArray *open_ar = arr(payload, "open_ar");
	JsonArray *customers = arr(payload, "customers");
	gint64 org = venture_entity_get_organization_id(cutover);
	guint i;
	if (open_ar == NULL)
		return TRUE;
	for (i = 0; i < json_array_get_length(open_ar); i++)
	{
		JsonObject *row = json_array_get_object_element(open_ar, i);
		const gchar *source_id = obj_str(row, "source_id");
		const gchar *customer_source = obj_str(row, "customer_source_id");
		const gchar *customer_name = "Imported customer";
		g_autoptr(VentureInvoice) invoice = NULL;
		g_autoptr(VentureInvoiceLine) line = NULL;
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) tax = NULL;
		gint64 customer_id, existing, tax_percent = 0;
		guint c;
		existing = existing_record(self, source_id, "open_ar", error);
		if (existing < 0)
			return FALSE;
		if (existing > 0)
			continue;
		if (customers != NULL)
		{
			for (c = 0; c < json_array_get_length(customers); c++)
			{
				JsonObject *customer = json_array_get_object_element(customers, c);
				if (g_strcmp0(obj_str(customer, "source_id"), customer_source) == 0)
					customer_name = obj_str(customer, "name");
			}
		}
		customer_id = find_or_create_company(self, org, customer_name, customer_source, "customer", actor, error);
		if (customer_id == 0)
			return FALSE;
		net = venture_money_from_string(obj_str(row, "net"), "USD", error);
		tax = venture_money_from_string(obj_str(row, "tax"), "USD", error);
		if (net == NULL)
			return FALSE;
		if (tax != NULL && !venture_money_is_zero(net))
			tax_percent = (venture_money_get_amount(tax) * 100) / venture_money_get_amount(net);
		invoice = venture_invoice_new();
		g_object_set(invoice, "number", obj_str(row, "number"), "company-id", customer_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(invoice), org);
		if (!venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", obj_str(row, "date"), error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
			return FALSE;
		line = venture_invoice_line_new();
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", "Opening balance", "quantity", 1.0, "tax-percent", tax_percent, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(line), org);
		if (!venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", obj_str(row, "net"), error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
			return FALSE;
		g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
			return FALSE;
		if (!add_row(self, venture_entity_get_id(cutover), org, source_id, "open_ar", "imported", NULL,
			"invoice", venture_entity_get_id(VENTURE_ENTITY(invoice)), actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_bank(VentureCutoverService *self, VentureEntity *cutover, JsonObject *payload,
	const VentureActor *actor, GError **error)
{
	JsonArray *banks = arr(payload, "bank_balances");
	gint64 org = venture_entity_get_organization_id(cutover);
	guint i;
	if (banks == NULL)
		return TRUE;
	for (i = 0; i < json_array_get_length(banks); i++)
	{
		JsonObject *row = json_array_get_object_element(banks, i);
		gint64 existing;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(GPtrArray) accounts = NULL;
		g_autoptr(VentureBankAccount) bank = NULL;
		g_autoptr(VentureJournal) header = NULL;
		g_autoptr(VentureJournal) posted = NULL;
		g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) cutoff = NULL;
		gint64 cash_id, equity_id;
		VentureJournalLine *line;
		existing = existing_record(self, obj_str(row, "source_id"), "bank", error);
		if (existing < 0)
			return FALSE;
		if (existing > 0)
			continue;
		amount = venture_money_from_string(obj_str(row, "amount"), "USD", error);
		if (amount == NULL)
			return FALSE;
		g_object_get(cutover, "cutoff", &cutoff, NULL);
		venture_query_set_organization(query, org);
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ,
			obj_str(row, "account_code") != NULL ? obj_str(row, "account_code") : "1000", error))
			return FALSE;
		accounts = venture_database_find(self->database, query, error);
		if (accounts == NULL || accounts->len == 0)
			return refuse(error, "bank account code is missing from the chart");
		cash_id = venture_entity_get_id(g_ptr_array_index(accounts, 0));
		g_clear_object(&query);
		g_clear_pointer(&accounts, g_ptr_array_unref);
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(query, org);
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "3000", error))
			return FALSE;
		accounts = venture_database_find(self->database, query, error);
		if (accounts == NULL || accounts->len == 0)
			return refuse(error, "equity account 3000 is required for opening cash");
		equity_id = venture_entity_get_id(g_ptr_array_index(accounts, 0));
		bank = venture_bank_account_new();
		g_object_set(bank, "name", obj_str(row, "name"), "currency", "USD", "account-id", cash_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(bank), org);
		if (!save_owned(self, VENTURE_ENTITY(bank), actor, error))
			return FALSE;
		header = venture_journal_new();
		g_object_set(header, "organization-id", org, "source-type", "accounting_cutover",
			"source-id", venture_entity_get_id(cutover), "occurred-at", cutoff, "currency", "USD",
			"memo", "Opening bank balance", NULL);
		line = venture_journal_line_new();
		g_object_set(line, "account-id", cash_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", amount, NULL);
		g_ptr_array_add(lines, line);
		line = venture_journal_line_new();
		g_object_set(line, "account-id", equity_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", amount, NULL);
		g_ptr_array_add(lines, line);
		posted = venture_posting_service_post(venture_database_get_posting_service(self->database),
			header, lines, NULL, actor, error);
		if (posted == NULL)
			return FALSE;
		if (!add_row(self, venture_entity_get_id(cutover), org, obj_str(row, "source_id"), "bank",
			"imported", NULL, "bank_account", venture_entity_get_id(VENTURE_ENTITY(bank)), actor, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_cutover_service_import(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonObject) payload = NULL;
	g_autofree gchar *state = NULL;
	g_object_get(cutover, "state", &state, NULL);
	if (g_strcmp0(state, "preview") != 0 && g_strcmp0(state, "imported") != 0)
		return refuse(error, "import requires a previewed batch");
	payload = payload_of(VENTURE_ENTITY(cutover));
	if (payload == NULL)
		return refuse(error, "cutover payload is missing");
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!import_open_ar(self, VENTURE_ENTITY(cutover), payload, actor, error) ||
		!import_bank(self, VENTURE_ENTITY(cutover), payload, actor, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	g_object_set(cutover, "state", "imported", NULL);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error) ||
		!venture_database_commit(self->database, error))
		return FALSE;
	return TRUE;
}

gboolean
venture_cutover_service_reconcile(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autofree gchar *report = NULL;
	g_object_get(cutover, "state", &state, "reconciliation-report", &report, NULL);
	if (g_strcmp0(state, "imported") != 0 && g_strcmp0(state, "reconciled") != 0)
		return refuse(error, "reconcile after import");
	g_object_set(cutover, "state", "reconciled", "reconciliation-report",
		report != NULL ? report : "Opening AR, bank and trial balance accepted", NULL);
	return save_owned(self, VENTURE_ENTITY(cutover), actor, error);
}

gboolean
venture_cutover_service_activate(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_object_get(cutover, "state", &state, NULL);
	if (g_strcmp0(state, "reconciled") != 0)
		return refuse(error, "activate after reconciliation");
	g_object_set(cutover, "state", "active", "activated-at", now, NULL);
	return save_owned(self, VENTURE_ENTITY(cutover), actor, error);
}

gboolean
venture_cutover_service_rollback(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	g_object_get(cutover, "state", &state, NULL);
	if (g_strcmp0(state, "active") == 0)
		return refuse(error, "an activated cutover cannot be rolled back");
	if (!venture_database_begin(self->database, error))
		return FALSE;
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "cutover-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(VENTURE_ENTITY(cutover)), error))
		goto fail;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		goto fail;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *type = NULL;
		gint64 record_id = 0;
		g_object_get(row, "record-type", &type, "record-id", &record_id, NULL);
		if (record_id > 0 && type != NULL && *type != '\0')
		{
			GType gtype = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
			g_autoptr(VentureEntity) record = NULL;
			if (gtype != G_TYPE_INVALID)
			{
				record = venture_database_get(self->database, gtype, record_id, NULL);
				if (record != NULL)
				{
					self->writing = record;
					venture_database_delete(self->database, record, actor, NULL);
					self->writing = NULL;
				}
			}
		}
	}
	g_object_set(cutover, "state", "rolled_back", NULL);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error) ||
		!venture_database_commit(self->database, error))
		return FALSE;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean
cutover_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
cutover_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	VentureCutoverService *service = venture_action_get_data(action);
	g_object_get(action, "name", &name, NULL);
	if (g_strcmp0(name, "import") == 0)
		return venture_cutover_service_import(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "reconcile") == 0)
		return venture_cutover_service_reconcile(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "activate") == 0)
		return venture_cutover_service_activate(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "rollback") == 0)
		return venture_cutover_service_rollback(service, VENTURE_ACCOUNTING_CUTOVER(entity), actor, error) ? g_object_ref(entity) : NULL;
	(void)params;
	return NULL;
}

void
venture_cutover_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	static const gchar *const names[] = { "import", "reconcile", "activate", "rollback" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "accounting_cutover",
			"name", names[i], "label", names[i], "description", "Cutover batch action",
			"stageable", FALSE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		g_autoptr(GError) error = NULL;
		venture_action_registry_register(registry, action, cutover_allowed, cutover_invoke,
			venture_cutover_service_get(database), NULL, &error);
	}
}
