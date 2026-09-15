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

gboolean
venture_cutover_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name;
	VentureCutoverService *self;
	(void)removal;
	if (record == NULL || database == NULL)
		return TRUE;
	name = venture_entity_get_entity_name(record);
	if (g_strcmp0(name, "accounting_cutover") != 0 && g_strcmp0(name, "accounting_cutover_row") != 0)
		return TRUE;
	self = venture_cutover_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	return refuse(error, "cutover batches must be changed through VentureCutoverService");
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
existing_record(VentureCutoverService *self, gint64 org, const gchar *source_id, const gchar *source_type, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	if (source_id == NULL || *source_id == '\0')
		return 0;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, error))
		return -1;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *type = NULL;
		g_autofree gchar *status = NULL;
		gint64 record_id = 0;
		g_object_get(g_ptr_array_index(rows, i), "source-type", &type, "record-id", &record_id,
			"status", &status, NULL);
		if (g_strcmp0(type, source_type) == 0 && record_id > 0 && g_strcmp0(status, "rolled_back") != 0)
			return record_id;
	}
	return 0;
}

static gint64
find_or_create_company(VentureCutoverService *self, gint64 cutover_id, gint64 org, const gchar *name,
	const gchar *source_id, const gchar *source_type, const VentureActor *actor, GError **error)
{
	gint64 existing;
	g_autoptr(VentureCompany) company = NULL;
	existing = existing_record(self, org, source_id, source_type, error);
	if (existing < 0)
		return 0;
	if (existing > 0)
		return existing;
	company = venture_company_new();
	g_object_set(company, "name", name != NULL ? name : "Imported party", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), org);
	if (!save_owned(self, VENTURE_ENTITY(company), actor, error))
		return 0;
	if (source_id != NULL && !add_row(self, cutover_id, org, source_id, source_type, "imported", NULL,
		"company", venture_entity_get_id(VENTURE_ENTITY(company)), actor, error))
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

static gboolean
text_names_currency(const gchar *text)
{
	gsize length;
	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length > 4 && text[length - 4] == ' ' &&
		g_ascii_isalpha(text[length - 3]) && g_ascii_isalpha(text[length - 2]) &&
		g_ascii_isalpha(text[length - 1]))
		return TRUE;
	if (length > 4 && text[3] == ' ' &&
		g_ascii_isalpha(text[0]) && g_ascii_isalpha(text[1]) && g_ascii_isalpha(text[2]))
		return TRUE;
	return FALSE;
}

static VentureMoney *
parse_money(const gchar *text, const gchar *currency, GError **error)
{
	if (text == NULL || *text == '\0')
	{
		refuse(error, "monetary amount is required");
		return NULL;
	}
	if (!text_names_currency(text) && (currency == NULL || *currency == '\0'))
	{
		refuse(error, "amount must include a currency");
		return NULL;
	}
	return venture_money_from_string(text, currency, error);
}

static gboolean
array_nonempty(JsonObject *payload, const gchar *name)
{
	JsonArray *rows = arr(payload, name);
	return rows != NULL && json_array_get_length(rows) > 0;
}

static gboolean
refuse_unimported_sections(JsonObject *payload, GError **error)
{
	if (array_nonempty(payload, "open_ap") || array_nonempty(payload, "credits") ||
		array_nonempty(payload, "assets"))
		return refuse(error, "open_ap, credits and assets are not imported; omit them or list them under unsupported");
	return TRUE;
}

VentureEntity *
venture_cutover_service_preview(VentureCutoverService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingCutover) cutover = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	g_autofree gchar *payload_text = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GString) report = g_string_new("Cutover preview\n");
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
	if (!refuse_unimported_sections(payload, error))
		return NULL;
	cutoff = venture_time_from_string(obj_str(payload, "cutoff"), error);
	if (cutoff == NULL)
		return NULL;
	json_node_set_object(node, json_object_ref(payload));
	payload_text = venture_json_to_string(node, FALSE);
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
	if (!venture_database_commit(self->database, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&cutover));
fail:
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
	gint64 cutover_id = venture_entity_get_id(cutover);
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
		g_autoptr(VentureMoney) zero_tax = NULL;
		gint64 customer_id, existing, tax_percent = 0;
		guint c;
		existing = existing_record(self, org, source_id, "open_ar", error);
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
		customer_id = find_or_create_company(self, cutover_id, org, customer_name, customer_source,
			"customer", actor, error);
		if (customer_id == 0)
			return FALSE;
		{
			const gchar *currency = obj_str(row, "currency");
			if (currency == NULL)
				currency = obj_str(payload, "currency");
			net = parse_money(obj_str(row, "net") != NULL ? obj_str(row, "net") : obj_str(row, "amount"),
				currency, error);
			if (net == NULL)
				return FALSE;
			if (obj_str(row, "tax") != NULL)
				tax = parse_money(obj_str(row, "tax"), currency, error);
			if (obj_str(row, "tax") != NULL && tax == NULL)
				return FALSE;
		}
		if (tax != NULL && !venture_money_is_zero(net) && venture_money_get_amount(tax) * 100 % venture_money_get_amount(net) == 0)
			tax_percent = (venture_money_get_amount(tax) * 100) / venture_money_get_amount(net);
		invoice = venture_invoice_new();
		g_object_set(invoice, "number", obj_str(row, "number"), "company-id", customer_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(invoice), org);
		if (!venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", obj_str(row, "date"), error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
			return FALSE;
		line = venture_invoice_line_new();
		zero_tax = venture_money_new_zero(venture_money_get_currency(net));
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", "Opening balance", "quantity", 1.0, "tax-percent", tax_percent,
			"income-amount", net, "tax-amount", tax != NULL ? tax : zero_tax, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(line), org);
		if (!venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price",
			obj_str(row, "net") != NULL ? obj_str(row, "net") : obj_str(row, "amount"), error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
			return FALSE;
		if (json_object_has_member(row, "tax_exempt") && json_object_get_boolean_member(row, "tax_exempt"))
			g_object_set(invoice, "tax-exempt", TRUE, "tax-exempt-reason", obj_str(row, "tax_exempt_reason"), NULL);
		g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
			return FALSE;
		if (!add_row(self, cutover_id, org, source_id, "open_ar", "imported", NULL,
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
		const gchar *currency;
		gint64 cash_id, equity_id;
		VentureJournalLine *line;
		existing = existing_record(self, org, obj_str(row, "source_id"), "bank", error);
		if (existing < 0)
			return FALSE;
		if (existing > 0)
			continue;
		amount = parse_money(obj_str(row, "amount"),
			obj_str(row, "currency") != NULL ? obj_str(row, "currency") : obj_str(payload, "currency"), error);
		if (amount == NULL)
			return FALSE;
		currency = venture_money_get_currency(amount);
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
		g_object_set(bank, "name", obj_str(row, "name"), "currency", currency, "account-id", cash_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(bank), org);
		if (!save_owned(self, VENTURE_ENTITY(bank), actor, error))
			return FALSE;
		header = venture_journal_new();
		g_object_set(header, "organization-id", org, "source-type", "accounting_cutover",
			"source-id", venture_entity_get_id(cutover), "occurred-at", cutoff, "currency", currency,
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
	if (!refuse_unimported_sections(payload, error))
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!import_open_ar(self, VENTURE_ENTITY(cutover), payload, actor, error) ||
		!import_bank(self, VENTURE_ENTITY(cutover), payload, actor, error))
		goto fail;
	g_object_set(cutover, "state", "imported", NULL);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean
add_money(VentureMoney **total, const VentureMoney *amount, GError **error)
{
	if (amount == NULL)
		return TRUE;
	if (*total == NULL)
	{
		*total = venture_money_copy((VentureMoney *)amount);
		return TRUE;
	}
	{
		VentureMoney *next = venture_money_add(*total, amount, error);
		if (next == NULL)
			return FALSE;
		venture_money_free(*total);
		*total = next;
	}
	return TRUE;
}

static gboolean
payload_sum(JsonArray *rows, const gchar *field, VentureMoney **total, GError **error)
{
	guint i;
	if (rows == NULL)
		return TRUE;
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		g_autoptr(VentureMoney) amount = NULL;
		if (obj_str(row, field) == NULL)
			continue;
		amount = parse_money(obj_str(row, field), NULL, error);
		if (amount == NULL)
			return FALSE;
		if (!add_money(total, amount, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
cutover_rows(VentureCutoverService *self, gint64 cutover_id, GPtrArray **rows, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "cutover-id", VENTURE_FILTER_OP_EQ, cutover_id, error))
		return FALSE;
	*rows = venture_database_find(self->database, query, error);
	return *rows != NULL;
}

static gboolean
issue_amount(VentureCutoverService *self, gint64 invoice_id, VentureMoney **amount, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
	g_autoptr(GPtrArray) events = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error))
		return FALSE;
	events = venture_database_find(self->database, query, error);
	if (events == NULL)
		return FALSE;
	for (i = 0; i < events->len; i++)
	{
		g_autofree gchar *kind = NULL;
		g_autoptr(VentureMoney) total = NULL;
		g_object_get(g_ptr_array_index(events, i), "kind", &kind, "amount", &total, NULL);
		if (g_strcmp0(kind, "issue") == 0)
		{
			*amount = total != NULL ? venture_money_copy(total) : NULL;
			return TRUE;
		}
	}
	*amount = NULL;
	return TRUE;
}

static gboolean
trial_balance(VentureCutoverService *self, gint64 org, GDateTime *cutoff, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	gint64 debit = 0, credit = 0;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	journals = venture_database_find(self->database, query, error);
	if (journals == NULL)
		return FALSE;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(VentureQuery) lines_query = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		VentureJournalState state;
		guint j;
		g_object_get(journal, "state", &state, "occurred-at", &date, NULL);
		if (state != VENTURE_JOURNAL_POSTED && state != VENTURE_JOURNAL_REVERSED)
			continue;
		if (date == NULL || g_date_time_compare(date, cutoff) > 0)
			continue;
		lines_query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		venture_query_set_limit(lines_query, 0);
		if (!venture_query_add_filter_int(lines_query, "journal-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(journal), error))
			return FALSE;
		lines = venture_database_find(self->database, lines_query, error);
		if (lines == NULL)
			return FALSE;
		for (j = 0; j < lines->len; j++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			gint side;
			g_object_get(g_ptr_array_index(lines, j), "side", &side, "book-amount", &amount, NULL);
			if (amount == NULL)
				continue;
			if (side == VENTURE_LEDGER_SIDE_DEBIT)
				debit += venture_money_get_amount(amount);
			else
				credit += venture_money_get_amount(amount);
		}
	}
	if (debit != credit)
		return refuse(error, "trial balance does not tie at the cutoff");
	return TRUE;
}

gboolean
venture_cutover_service_reconcile(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) expected_ar = NULL;
	g_autoptr(VentureMoney) imported_ar = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	g_autoptr(GString) report = g_string_new("Cutover reconciliation\n");
	gint64 org;
	guint i;
	g_object_get(cutover, "state", &state, "cutoff", &cutoff, NULL);
	if (g_strcmp0(state, "imported") != 0 && g_strcmp0(state, "reconciled") != 0)
		return refuse(error, "reconcile after import");
	payload = payload_of(VENTURE_ENTITY(cutover));
	if (payload == NULL)
		return refuse(error, "cutover payload is missing");
	org = venture_entity_get_organization_id(VENTURE_ENTITY(cutover));
	if (!trial_balance(self, org, cutoff, error))
		return FALSE;
	if (!payload_sum(arr(payload, "open_ar"), "amount", &expected_ar, error))
		return FALSE;
	if (expected_ar == NULL && !payload_sum(arr(payload, "open_ar"), "net", &expected_ar, error))
		return FALSE;
	if (!cutover_rows(self, venture_entity_get_id(VENTURE_ENTITY(cutover)), &rows, error))
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *type = NULL;
		g_autofree gchar *source_id = NULL;
		gint64 record_id = 0;
		g_object_get(row, "source-type", &type, "source-id", &source_id, "record-id", &record_id, NULL);
		if (g_strcmp0(type, "open_ar") == 0 && record_id > 0)
		{
			g_autoptr(VentureMoney) amount = NULL;
			if (!issue_amount(self, record_id, &amount, error))
				return FALSE;
			if (!add_money(&imported_ar, amount, error))
				return FALSE;
		}
		if (g_strcmp0(type, "bank") == 0 && record_id > 0)
		{
			g_autoptr(VentureEntity) bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT,
				record_id, error);
			g_autoptr(VentureMoney) cash = NULL;
			g_autoptr(VentureMoney) expected = NULL;
			g_autofree gchar *currency = NULL;
			JsonArray *banks = arr(payload, "bank_balances");
			gint64 account_id = 0;
			guint b;
			if (bank == NULL)
				return FALSE;
			g_object_get(bank, "account-id", &account_id, "currency", &currency, NULL);
			cash = venture_posting_service_account_balance(venture_database_get_posting_service(self->database),
				account_id, org, currency, cutoff, error);
			if (cash == NULL)
				return FALSE;
			if (banks != NULL)
			{
				for (b = 0; b < json_array_get_length(banks); b++)
				{
					JsonObject *src = json_array_get_object_element(banks, b);
					if (g_strcmp0(obj_str(src, "source_id"), source_id) == 0)
					{
						expected = parse_money(obj_str(src, "amount"), currency, error);
						break;
					}
				}
			}
			if (expected == NULL || !venture_money_equal(cash, expected))
				return refuse(error, "opening bank cash does not match the source total");
			g_string_append_printf(report, "Bank cash %" G_GINT64_FORMAT "\n", venture_money_get_amount(cash));
		}
	}
	if (expected_ar != NULL)
	{
		if (imported_ar == NULL || !venture_money_equal(expected_ar, imported_ar))
			return refuse(error, "opening AR does not match issued invoice totals");
		g_string_append_printf(report, "Open AR %" G_GINT64_FORMAT "\n", venture_money_get_amount(expected_ar));
	}
	g_string_append(report, "Trial balance ties at cutoff.\n");
	g_object_set(cutover, "state", "reconciled", "reconciliation-report", report->str, NULL);
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
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GDateTime) cutoff = NULL;
	gint64 org;
	guint i;
	g_object_get(cutover, "state", &state, "cutoff", &cutoff, NULL);
	if (g_strcmp0(state, "active") == 0)
		return refuse(error, "an activated cutover cannot be rolled back");
	org = venture_entity_get_organization_id(VENTURE_ENTITY(cutover));
	if (!venture_database_begin(self->database, error))
		return FALSE;
	journals = venture_posting_service_find_source(venture_database_get_posting_service(self->database),
		"accounting_cutover", venture_entity_get_id(VENTURE_ENTITY(cutover)), org, error);
	if (journals == NULL)
		goto fail;
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(VentureJournal) reversed = NULL;
		VentureJournalState journal_state;
		g_object_get(journal, "state", &journal_state, NULL);
		if (journal_state != VENTURE_JOURNAL_POSTED)
			continue;
		reversed = venture_posting_service_reverse(venture_database_get_posting_service(self->database),
			venture_entity_get_id(journal), cutoff, "Cutover rollback", actor, error);
		if (reversed == NULL)
			goto fail;
	}
	if (!cutover_rows(self, venture_entity_get_id(VENTURE_ENTITY(cutover)), &rows, error))
		goto fail;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *type = NULL;
		gint64 record_id = 0;
		g_object_get(row, "record-type", &type, "record-id", &record_id, NULL);
		if (g_strcmp0(type, "invoice") == 0 && record_id > 0)
		{
			g_autoptr(VentureEntity) invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE,
				record_id, error);
			if (invoice == NULL)
				goto fail;
			if (!venture_settlement_service_transition(venture_settlement_service_get(self->database),
				VENTURE_INVOICE(invoice), "void", cutoff, actor, error))
				goto fail;
		}
		g_object_set(row, "status", "rolled_back", NULL);
		if (!save_owned(self, row, actor, error))
			goto fail;
	}
	g_object_set(cutover, "state", "rolled_back", NULL);
	if (!save_owned(self, VENTURE_ENTITY(cutover), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
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
