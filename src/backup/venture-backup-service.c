/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureBackupService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureBackupService, venture_backup_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureBackupService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_BACKUP_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureBackupService *self = VENTURE_BACKUP_SERVICE(object);
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
	VentureBackupService *self = VENTURE_BACKUP_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_backup_service_parent_class)->finalize(object);
}

static void
venture_backup_service_class_init(VentureBackupServiceClass *klass)
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
venture_backup_service_init(VentureBackupService *self)
{
	(void)self;
}

VentureBackupService *
venture_backup_service_get(VentureDatabase *database)
{
	VentureBackupService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-backup-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_BACKUP_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-backup-service", self, g_object_unref);
	}
	return self;
}

gboolean
venture_backup_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	VentureBackupService *self;
	(void)removal;
	if (record == NULL || database == NULL || !VENTURE_IS_ACCOUNTING_BACKUP(record))
		return TRUE;
	self = venture_backup_service_get(database);
	if (self->writing == record)
		return TRUE;
	return refuse(error, "accounting backups are owned by VentureBackupService");
}

static gboolean
save_owned(VentureBackupService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static GPtrArray *
all(VentureBackupService *self, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	if (type == G_TYPE_INVALID)
		return g_ptr_array_new();
	query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	return venture_database_find(self->database, query, error);
}

static const gchar *
account_code_of(VentureBackupService *self, gint64 id)
{
	static gchar code[64];
	g_autoptr(VentureEntity) account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, id, NULL);
	g_autofree gchar *value = NULL;
	if (account == NULL)
		return "";
	g_object_get(account, "code", &value, NULL);
	g_strlcpy(code, value ? value : "", sizeof(code));
	return code;
}

static JsonNode *
export_json(VentureBackupService *self, gint64 org, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) journals = all(self, VENTURE_TYPE_JOURNAL, org, error);
	g_autoptr(GPtrArray) invoices = all(self, VENTURE_TYPE_INVOICE, org, error);
	g_autoptr(GPtrArray) accounts = all(self, VENTURE_TYPE_ACCOUNT, org, error);
	g_autoptr(GPtrArray) bills = NULL;
	g_autoptr(GPtrArray) banks = all(self, VENTURE_TYPE_BANK_ACCOUNT, org, error);
	guint i, j;
	if (journals == NULL || invoices == NULL || accounts == NULL)
		return NULL;
	bills = all(self, venture_entity_registry_lookup(venture_entity_registry_get_default(), "vendor_bill"), org, error);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "accounts");
	json_builder_begin_array(builder);
	for (i = 0; i < accounts->len; i++)
	{
		g_autofree gchar *code = NULL;
		g_autofree gchar *name = NULL;
		gint kind = 0;
		g_object_get(g_ptr_array_index(accounts, i), "code", &code, "name", &name, "kind", &kind, NULL);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "code");
		json_builder_add_string_value(builder, code);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, name ? name : code);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_int_value(builder, kind);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "journals");
	json_builder_begin_array(builder);
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		g_autofree gchar *currency = NULL;
		g_autoptr(GDateTime) when = NULL;
		gint state;
		g_autofree gchar *source_type = NULL;
		g_object_get(journal, "currency", &currency, "occurred-at", &when, "state", &state,
			"source-type", &source_type, NULL);
		if (state != VENTURE_JOURNAL_POSTED)
			continue;
		if (g_strcmp0(source_type, "invoice") == 0 || g_strcmp0(source_type, "payment") == 0 ||
			g_strcmp0(source_type, "payment_allocation") == 0 || g_strcmp0(source_type, "refund") == 0)
			continue;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "currency");
		json_builder_add_string_value(builder, currency);
		json_builder_set_member_name(builder, "occurred_at");
		{
			g_autofree gchar *iso = g_date_time_format(when, "%Y-%m-%dT%H:%M:%SZ");
			json_builder_add_string_value(builder, iso);
		}
		json_builder_set_member_name(builder, "lines");
		json_builder_begin_array(builder);
		venture_query_add_filter_int(q, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(journal), NULL);
		venture_query_set_limit(q, 0);
		lines = venture_database_find(self->database, q, error);
		if (lines == NULL)
			return NULL;
		for (j = 0; j < lines->len; j++)
		{
			VentureEntity *line = g_ptr_array_index(lines, j);
			g_autoptr(VentureMoney) amount = NULL;
			gint side;
			gint64 account_id;
			g_autofree gchar *text = NULL;
			g_object_get(line, "account-id", &account_id, "side", &side, "amount", &amount, NULL);
			text = venture_money_to_string(amount);
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "account_code");
			json_builder_add_string_value(builder, account_code_of(self, account_id));
			json_builder_set_member_name(builder, "side");
			json_builder_add_string_value(builder, side == VENTURE_LEDGER_SIDE_DEBIT ? "debit" : "credit");
			json_builder_set_member_name(builder, "amount");
			json_builder_add_string_value(builder, text);
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "invoices");
	json_builder_begin_array(builder);
	for (i = 0; i < invoices->len; i++)
	{
		VentureEntity *invoice = g_ptr_array_index(invoices, i);
		g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(invoice), FALSE);
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		JsonObject *object;
		json_builder_begin_object(builder);
		object = json_node_get_object(node);
		if (json_object_has_member(object, "number"))
		{
			json_builder_set_member_name(builder, "number");
			json_builder_add_string_value(builder, json_object_get_string_member(object, "number"));
		}
		json_builder_set_member_name(builder, "company_id");
		json_builder_add_int_value(builder, json_object_get_int_member(object, "company_id"));
		{
			gint status = 0;
			g_object_get(invoice, "status", &status, NULL);
			json_builder_set_member_name(builder, "status");
			json_builder_add_int_value(builder, status);
		}
		{
			g_autoptr(VentureQuery) eq = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
			g_autoptr(GPtrArray) events = NULL;
			venture_query_add_filter_int(eq, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
			events = venture_database_find(self->database, eq, NULL);
			if (events && events->len > 0)
			{
				g_autoptr(GDateTime) issued = NULL;
				g_object_get(g_ptr_array_index(events, 0), "date", &issued, NULL);
				if (issued != NULL)
				{
					g_autofree gchar *iso = g_date_time_format(issued, "%Y-%m-%dT%H:%M:%SZ");
					json_builder_set_member_name(builder, "issued_at");
					json_builder_add_string_value(builder, iso);
				}
			}
		}
		json_builder_set_member_name(builder, "lines");
		json_builder_begin_array(builder);
		venture_query_add_filter_int(q, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
		venture_query_set_limit(q, 0);
		lines = venture_database_find(self->database, q, error);
		if (lines == NULL)
			return NULL;
		for (j = 0; j < lines->len; j++)
		{
			g_autoptr(JsonNode) line_node = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(lines, j)), FALSE);
			json_builder_add_value(builder, json_node_copy(line_node));
		}
		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "vendor_bills");
	json_builder_begin_array(builder);
	if (bills)
		for (i = 0; i < bills->len; i++)
		{
			g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(bills, i)), FALSE);
			json_builder_add_value(builder, json_node_copy(node));
		}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "bank_accounts");
	json_builder_begin_array(builder);
	if (banks)
		for (i = 0; i < banks->len; i++)
		{
			g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(banks, i)), FALSE);
			json_builder_add_value(builder, json_node_copy(node));
		}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "payments");
	json_builder_begin_array(builder);
	{
		g_autoptr(GPtrArray) payments = all(self, VENTURE_TYPE_PAYMENT, org, error);
		if (payments == NULL) return NULL;
		for (i = 0; i < payments->len; i++)
		{
			VentureEntity *payment = g_ptr_array_index(payments, i);
			g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(payment), FALSE);
			JsonObject *object = json_node_get_object(node);
			gint64 invoice_id = 0;
			g_object_get(payment, "invoice-id", &invoice_id, NULL);
			if (invoice_id > 0)
			{
				g_autoptr(VentureEntity) invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, invoice_id, NULL);
				g_autofree gchar *number = NULL;
				if (invoice != NULL)
					g_object_get(invoice, "number", &number, NULL);
				if (number != NULL)
					json_object_set_string_member(object, "invoice_number", number);
			}
			json_builder_add_value(builder, json_node_copy(node));
		}
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "allocations");
	json_builder_begin_array(builder);
	{
		g_autoptr(GPtrArray) rows = all(self, VENTURE_TYPE_PAYMENT_ALLOCATION, org, error);
		if (rows == NULL) return NULL;
		for (i = 0; i < rows->len; i++)
		{
			g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(rows, i)), FALSE);
			json_builder_add_value(builder, json_node_copy(node));
		}
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "invoice_events");
	json_builder_begin_array(builder);
	{
		g_autoptr(GPtrArray) rows = all(self, VENTURE_TYPE_INVOICE_EVENT, org, error);
		if (rows == NULL) return NULL;
		for (i = 0; i < rows->len; i++)
		{
			g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(rows, i)), FALSE);
			json_builder_add_value(builder, json_node_copy(node));
		}
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, 2);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

static gchar *
export_csv(JsonNode *node)
{
	GString *csv = g_string_new("section,count\n");
	JsonObject *object = json_node_get_object(node);
	static const gchar *const keys[] = { "journals", "invoices", "vendor_bills", "bank_accounts", "payments", "allocations" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(keys); i++)
	{
		JsonArray *array = json_object_get_array_member(object, keys[i]);
		g_string_append_printf(csv, "%s,%u\n", keys[i], array ? json_array_get_length(array) : 0);
	}
	return g_string_free(csv, FALSE);
}

VentureEntity *
venture_backup_service_export(VentureBackupService *self, gint64 organization_id, const gchar *format,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureAccountingBackup) backup = NULL;
	g_autofree gchar *payload = NULL;
	g_autofree gchar *checksum = NULL;
	g_return_val_if_fail(VENTURE_IS_BACKUP_SERVICE(self), NULL);
	node = export_json(self, organization_id, error);
	if (node == NULL)
		return NULL;
	if (format != NULL && g_strcmp0(format, "csv") == 0)
		payload = export_csv(node);
	else
		payload = venture_json_to_string(node, FALSE);
	checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, payload, -1);
	backup = venture_accounting_backup_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(backup), organization_id);
	g_object_set(backup, "format", format && format[0] ? format : "json", "payload", payload,
		"state", "exported", "checksum", checksum, NULL);
	if (!save_owned(self, VENTURE_ENTITY(backup), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&backup));
}

static gint64
account_by_code(VentureBackupService *self, gint64 org, const gchar *code, const VentureActor *actor)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureAccount) account = NULL;
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(self->database, query, NULL);
	if (row != NULL)
		return venture_entity_get_id(row);
	account = venture_account_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(account), org);
	g_object_set(account, "code", code, "name", code, "kind",
		(g_strcmp0(code, "4000") == 0) ? VENTURE_ACCOUNT_KIND_INCOME : VENTURE_ACCOUNT_KIND_ASSET,
		"active", TRUE, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(account), actor, NULL))
	{
		g_autofree gchar *prefixed = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, code);
		g_object_set(account, "code", prefixed, NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(account), actor, NULL))
			return 0;
	}
	return venture_entity_get_id(VENTURE_ENTITY(account));
}


static gboolean
restore_accounts(VentureBackupService *self, gint64 org, JsonArray *accounts, const VentureActor *actor, GError **error)
{
	guint i;
	if (accounts == NULL)
		return TRUE;
	for (i = 0; i < json_array_get_length(accounts); i++)
	{
		JsonObject *row = json_array_get_object_element(accounts, i);
		const gchar *code = json_object_get_string_member(row, "code");
		const gchar *name = json_object_has_member(row, "name") ? json_object_get_string_member(row, "name") : code;
		gint kind = json_object_has_member(row, "kind") ? json_object_get_int_member(row, "kind") : VENTURE_ACCOUNT_KIND_ASSET;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(VentureEntity) existing = NULL;
		g_autoptr(VentureAccount) account = NULL;
		if (code == NULL || code[0] == '\0')
			continue;
		venture_query_set_organization(query, org);
		venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
		existing = venture_database_find_one(self->database, query, NULL);
		if (existing != NULL)
		{
			g_object_set(existing, "kind", kind, "name", name, "active", TRUE, NULL);
			if (!venture_database_save(self->database, existing, actor, error))
				return FALSE;
			continue;
		}
		account = venture_account_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(account), org);
		g_object_set(account, "code", code, "name", name, "kind", kind, "active", TRUE, NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(account), actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
restore_journals(VentureBackupService *self, gint64 org, JsonArray *journals, const VentureActor *actor, GError **error)
{
	guint i, j;
	for (i = 0; journals && i < json_array_get_length(journals); i++)
	{
		JsonObject *row = json_array_get_object_element(journals, i);
		JsonArray *lines = json_object_get_array_member(row, "lines");
		g_autoptr(VentureJournal) journal = venture_journal_new();
		g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureJournal) posted = NULL;
		const gchar *stamp = json_object_get_string_member(row, "occurred_at");
		when = stamp ? g_date_time_new_from_iso8601(stamp, NULL) : venture_time_now();
		g_object_set(journal, "source-type", "organization", "source-id", org, "occurred-at", when,
			"currency", json_object_get_string_member(row, "currency"), "organization-id", org, NULL);
		for (j = 0; lines && j < json_array_get_length(lines); j++)
		{
			JsonObject *line = json_array_get_object_element(lines, j);
			g_autoptr(VentureJournalLine) jl = venture_journal_line_new();
			gint64 account = account_by_code(self, org, json_object_get_string_member(line, "account_code"), actor);
			if (account == 0)
				return refuse(error, "restore is missing a mapped account");
			g_object_set(jl, "account-id", account, "side",
				g_strcmp0(json_object_get_string_member(line, "side"), "debit") == 0 ?
					VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
				"organization-id", org, NULL);
			if (!venture_entity_set_field_from_string(VENTURE_ENTITY(jl), "amount",
				json_object_get_string_member(line, "amount"), error))
				return FALSE;
			g_ptr_array_add(rows, g_steal_pointer(&jl));
		}
		posted = venture_posting_service_post(venture_database_get_posting_service(self->database),
			journal, rows, NULL, actor, error);
		if (posted == NULL)
			return FALSE;
	}
	return TRUE;
}

static gint64
ensure_company(VentureBackupService *self, gint64 org, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureCompany) company = NULL;
	venture_query_set_organization(query, org);
	existing = venture_database_find_one(self->database, query, NULL);
	if (existing != NULL)
		return venture_entity_get_id(existing);
	company = venture_company_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(company), org);
	g_object_set(company, "name", "Restored customer", NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(company), actor, error))
		return 0;
	return venture_entity_get_id(VENTURE_ENTITY(company));
}

static gboolean
restore_invoices(VentureBackupService *self, gint64 org, JsonArray *invoices, const VentureActor *actor, GError **error)
{
	guint i, j;
	gint64 company = ensure_company(self, org, actor, error);
	if (company == 0 && invoices && json_array_get_length(invoices) > 0)
		return FALSE;
	for (i = 0; invoices && i < json_array_get_length(invoices); i++)
	{
		JsonObject *row = json_array_get_object_element(invoices, i);
		JsonArray *lines = json_object_get_array_member(row, "lines");
		g_autoptr(VentureInvoice) invoice = venture_invoice_new();
		const gchar *source_number = json_object_get_string_member(row, "number");
		gint status = json_object_has_member(row, "status") ?
			json_object_get_int_member(row, "status") : VENTURE_INVOICE_STATUS_DRAFT;
		venture_entity_set_organization_id(VENTURE_ENTITY(invoice), org);
		g_object_set(invoice, "number", source_number && source_number[0] ? source_number : "INV-REST",
			"company-id", company, NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
			return FALSE;
		for (j = 0; lines && j < json_array_get_length(lines); j++)
		{
			g_autoptr(VentureInvoiceLine) il = venture_invoice_line_new();
			JsonObject *line = json_array_get_object_element(lines, j);
			JsonNode *price = json_object_get_member(line, "unit_price");
			g_autoptr(VentureMoney) amount = NULL;
			if (price == NULL)
				price = json_object_get_member(line, "unit-price");
			venture_entity_set_organization_id(VENTURE_ENTITY(il), org);
			g_object_set(il, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
				"description", json_object_get_string_member(line, "description"),
				"quantity", json_object_has_member(line, "quantity") ? json_object_get_double_member(line, "quantity") : 1.0,
				NULL);
			if (price != NULL)
			{
				g_autoptr(VentureEntity) organization = venture_database_get(self->database,
					VENTURE_TYPE_ORGANIZATION, org, NULL);
				g_autofree gchar *currency = NULL;
				if (organization != NULL)
					g_object_get(organization, "default-currency", &currency, NULL);
				amount = venture_money_from_json(price, currency, error);
				if (amount == NULL)
					return FALSE;
			}
			if (amount != NULL)
				g_object_set(il, "unit-price", amount, NULL);
			{
				JsonNode *frozen = json_object_get_member(line, "tax_amount");
				if (frozen == NULL)
					frozen = json_object_get_member(line, "tax-amount");
				if (frozen != NULL)
				{
					g_autoptr(VentureMoney) tax = venture_money_from_json(frozen, NULL, NULL);
					if (tax != NULL)
						g_object_set(il, "tax-amount", tax, NULL);
				}
				frozen = json_object_get_member(line, "income_amount");
				if (frozen == NULL)
					frozen = json_object_get_member(line, "income-amount");
				if (frozen != NULL)
				{
					g_autoptr(VentureMoney) income = venture_money_from_json(frozen, NULL, NULL);
					if (income != NULL)
						g_object_set(il, "income-amount", income, NULL);
				}
			}
			if (!venture_database_save(self->database, VENTURE_ENTITY(il), actor, error))
				return FALSE;
		}
		if (status == VENTURE_INVOICE_STATUS_SENT || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID ||
			status == VENTURE_INVOICE_STATUS_PAID)
		{
			g_autoptr(GDateTime) when = NULL;
			const gchar *issued = json_object_has_member(row, "issued_at") ?
				json_object_get_string_member(row, "issued_at") : NULL;
			when = issued ? g_date_time_new_from_iso8601(issued, NULL) : venture_time_now();
			if (!venture_settlement_service_transition(venture_settlement_service_get(self->database),
				invoice, "sent", when, actor, error))
				return FALSE;
		}
	}
	return TRUE;
}

static gint64
invoice_by_number(VentureBackupService *self, gint64 org, const gchar *number)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(VentureEntity) row = NULL;
	if (number == NULL || number[0] == '\0')
		return 0;
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "number", VENTURE_FILTER_OP_EQ, number, NULL);
	row = venture_database_find_one(self->database, query, NULL);
	return row ? venture_entity_get_id(row) : 0;
}

static gboolean
restore_payments(VentureBackupService *self, gint64 org, JsonArray *payments, const VentureActor *actor, GError **error)
{
	guint i;
	gint64 company;
	if (payments == NULL || json_array_get_length(payments) == 0)
		return TRUE;
	company = ensure_company(self, org, actor, error);
	if (company == 0)
		return FALSE;
	for (i = 0; i < json_array_get_length(payments); i++)
	{
		JsonObject *row = json_array_get_object_element(payments, i);
		g_autoptr(VenturePayment) payment = venture_payment_new();
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		JsonNode *amount_node;
		const gchar *invoice_number = json_object_has_member(row, "invoice_number") ?
			json_object_get_string_member(row, "invoice_number") : NULL;
		gint64 invoice_id = 0;
		{
			JsonNode *date_node = json_object_get_member(row, "date");
			if (date_node != NULL && JSON_NODE_HOLDS_VALUE(date_node) &&
				json_node_get_value_type(date_node) == G_TYPE_STRING)
				when = g_date_time_new_from_iso8601(json_node_get_string(date_node), NULL);
		}
		if (when == NULL)
			when = venture_time_now();
		amount_node = json_object_get_member(row, "amount");
		if (amount_node != NULL)
			amount = venture_money_from_json(amount_node, NULL, error);
		if (amount == NULL)
			return FALSE;
		invoice_id = invoice_by_number(self, org, invoice_number);
		if (invoice_id == 0)
			return refuse(error, "restore payment is missing a remapped invoice");
		venture_entity_set_organization_id(VENTURE_ENTITY(payment), org);
		g_object_set(payment, "customer-id", company, "date", when, "amount", amount,
			"method", json_object_has_member(row, "method") ? json_object_get_string_member(row, "method") : "transfer",
			"reference", json_object_has_member(row, "reference") ? json_object_get_string_member(row, "reference") : "restore",
			"invoice-id", invoice_id, NULL);
		if (!venture_settlement_service_apply_payment(venture_settlement_service_get(self->database),
			payment, NULL, actor, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_backup_service_restore(VentureBackupService *self, gint64 organization_id, const gchar *payload,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	JsonObject *root;
	gint64 existing;
	g_return_val_if_fail(VENTURE_IS_BACKUP_SERVICE(self), FALSE);
	if (payload == NULL || payload[0] != '{')
		return refuse(error, "restore requires a JSON accounting pack");
	venture_query_set_organization(query, organization_id);
	existing = venture_database_count(self->database, query, error);
	if (existing < 0)
		return FALSE;
	if (existing > 0)
		return refuse(error, "restore requires an empty organization");
	if (!json_parser_load_from_data(parser, payload, -1, error))
		return FALSE;
	root = json_node_get_object(json_parser_get_root(parser));
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!venture_setup_seed_defaults(self->database, organization_id, actor, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	if (!restore_accounts(self, organization_id, json_object_has_member(root, "accounts") ?
			json_object_get_array_member(root, "accounts") : NULL, actor, error) ||
		!restore_journals(self, organization_id, json_object_get_array_member(root, "journals"), actor, error) ||
		!restore_invoices(self, organization_id, json_object_get_array_member(root, "invoices"), actor, error) ||
		!restore_payments(self, organization_id,
			json_object_has_member(root, "payments") ? json_object_get_array_member(root, "payments") : NULL,
			actor, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	return venture_database_commit(self->database, error);
}

static gboolean
backup_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
backup_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	JsonNode *payload;
	g_object_get(action, "name", &name, NULL);
	payload = params ? g_hash_table_lookup(params, "payload") : NULL;
	if (g_strcmp0(name, "restore") == 0)
		return venture_backup_service_restore(venture_action_get_data(action),
			venture_entity_get_organization_id(entity),
			payload && JSON_NODE_HOLDS_VALUE(payload) ? json_node_get_string(payload) : NULL,
			actor, error) ? g_object_ref(entity) : NULL;
	return NULL;
}

void
venture_backup_actions_register(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	g_ptr_array_add(parameters, venture_field_spec_new("payload", "Pack JSON", VENTURE_FIELD_KIND_TEXT));
	action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "accounting_backup", "name", "restore",
		"label", "Restore", "description", "Restore this pack into an empty organization",
		"parameters", parameters, "stageable", FALSE, "roles", VENTURE_USER_ROLE_ADMIN, NULL);
	venture_action_registry_register(venture_database_get_action_registry(database), action,
		backup_allowed, backup_invoke, venture_backup_service_get(database), NULL, &error);
}
