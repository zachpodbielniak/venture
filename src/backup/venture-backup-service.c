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

#include "venture-accounting-snapshot.inc"

static gchar *
export_csv(JsonNode *node)
{
	g_autoptr(GString) csv = g_string_new("type,count\n");
	g_autoptr(GHashTable) counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	JsonArray *records = json_object_get_array_member(json_node_get_object(node), "records");
	g_autoptr(GList) keys = NULL;
	GList *iter;
	guint i;
	for (i = 0; i < json_array_get_length(records); i++)
	{
		const gchar *name = json_object_get_string_member(json_array_get_object_element(records, i), "type");
		guint count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, name));
		g_hash_table_replace(counts, g_strdup(name), GUINT_TO_POINTER(count + 1));
	}
	keys = g_hash_table_get_keys(counts);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
	for (iter = keys; iter != NULL; iter = iter->next)
		g_string_append_printf(csv, "%s,%u\n", (gchar *)iter->data, GPOINTER_TO_UINT(g_hash_table_lookup(counts, iter->data)));
	return g_string_free(g_steal_pointer(&csv), FALSE);
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
	node = snapshot_export(self, organization_id, error);
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
account_by_code(VentureBackupService *self, gint64 org, const gchar *code, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return 0;
	row = venture_database_find_one(self->database, query, error);
	return row != NULL ? venture_entity_get_id(row) : 0;
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
		if (existing == NULL)
		{
			g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, code);
			g_autoptr(VentureQuery) seeded = venture_query_new(VENTURE_TYPE_ACCOUNT);
			venture_query_set_organization(seeded, org);
			venture_query_add_filter_string(seeded, "code", VENTURE_FILTER_OP_EQ, scoped, NULL);
			existing = venture_database_find_one(self->database, seeded, NULL);
			if (existing != NULL)
				g_object_set(existing, "code", code, NULL);
		}
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
			gint64 account = account_by_code(self, org, json_object_get_string_member(line, "account_code"), error);
			if (account == 0)
				return error != NULL && *error != NULL ? FALSE : refuse(error, "restore is missing a mapped account");
			g_object_set(jl, "account-id", account, "side",
				g_strcmp0(json_object_get_string_member(line, "side"), "debit") == 0 ?
					VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT,
				"organization-id", org, NULL);
			g_object_set(jl, "dimension", venture_json_object_get_string(line, "dimension", ""), NULL);
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

/* The old format cannot distinguish missing history from an empty section.
 * Refuse it before seeding or posting anything. Version 4 uses the snapshot importer above; the legacy path
 * is restricted to manual journals. */
static gboolean
legacy_string(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
		json_node_get_value_type(node) == G_TYPE_STRING;
}

static gboolean
restore_preflight(JsonObject *root, GError **error)
{
	static const gchar *const unsupported[] = { "invoices", "vendor_bills", "bank_accounts",
		"payments", "allocations", "invoice_events", "refund", "customer_credit", "company" };
	guint i;
	{
		JsonNode *version = json_object_get_member(root, "version");
		JsonNode *accounts = json_object_get_member(root, "accounts");
		if (version == NULL || !JSON_NODE_HOLDS_VALUE(version) ||
			json_node_get_value_type(version) != G_TYPE_INT64 || json_node_get_int(version) != 3)
			return refuse(error, "legacy accounting packs omit history; use a full database backup");
		if (accounts == NULL || !JSON_NODE_HOLDS_ARRAY(accounts))
			return refuse(error, "accounting pack needs accounts");
		/* Typed JSON getters emit criticals for malformed input. Validate
		 * the complete legacy shape before a transaction can change books. */
		for (i = 0; i < json_array_get_length(json_node_get_array(accounts)); i++)
		{
			JsonNode *entry = json_array_get_element(json_node_get_array(accounts), i);
			JsonObject *account;
			JsonNode *kind;
			if (!JSON_NODE_HOLDS_OBJECT(entry))
				return refuse(error, "invalid legacy account");
			account = json_node_get_object(entry);
			kind = json_object_get_member(account, "kind");
			if (!legacy_string(account, "code") || *json_object_get_string_member(account, "code") == '\0' ||
				(json_object_has_member(account, "name") && !legacy_string(account, "name")) ||
				(kind != NULL && (json_node_get_value_type(kind) != G_TYPE_INT64 ||
				json_node_get_int(kind) < VENTURE_ACCOUNT_KIND_ASSET || json_node_get_int(kind) > VENTURE_ACCOUNT_KIND_EXPENSE)))
				return refuse(error, "invalid legacy account fields");
		}
	}
	for (i = 0; i < G_N_ELEMENTS(unsupported); i++)
	{
		JsonNode *node = json_object_get_member(root, unsupported[i]);
		if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
			return refuse(error, "accounting pack is missing a required array");
		if (json_array_get_length(json_node_get_array(node)) != 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
				"Accounting pack contains %s; export a version 4 snapshot from the source database for document restore", unsupported[i]);
			return FALSE;
		}
	}
	{
		JsonNode *node = json_object_get_member(root, "journals");
		JsonArray *journals;
		if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
			return refuse(error, "accounting pack needs journals");
		journals = json_node_get_array(node);
		for (i = 0; i < json_array_get_length(journals); i++)
		{
			JsonNode *entry = json_array_get_element(journals, i);
			JsonObject *journal;
			JsonNode *lines;
			g_autoptr(GDateTime) date = NULL;
			guint j;
			if (!JSON_NODE_HOLDS_OBJECT(entry) || g_strcmp0(venture_json_object_get_string(
				json_node_get_object(entry), "source_type", ""), "organization") != 0)
				return refuse(error, "document journals require their original document history");
			journal = json_node_get_object(entry);
			lines = json_object_get_member(journal, "lines");
			if (!legacy_string(journal, "currency") || !venture_currency_is_valid(json_object_get_string_member(journal, "currency")) ||
				!legacy_string(journal, "occurred_at") || lines == NULL || !JSON_NODE_HOLDS_ARRAY(lines))
				return refuse(error, "invalid legacy journal fields");
			date = g_date_time_new_from_iso8601(json_object_get_string_member(journal, "occurred_at"), NULL);
			if (date == NULL)
				return refuse(error, "invalid legacy journal date");
			for (j = 0; j < json_array_get_length(json_node_get_array(lines)); j++)
			{
				JsonNode *line = json_array_get_element(json_node_get_array(lines), j);
				JsonObject *object;
				const gchar *side;
				if (!JSON_NODE_HOLDS_OBJECT(line))
					return refuse(error, "invalid legacy journal line");
				object = json_node_get_object(line);
				if (!legacy_string(object, "account_code") || !legacy_string(object, "amount") || !legacy_string(object, "side"))
					return refuse(error, "invalid legacy journal line fields");
				side = json_object_get_string_member(object, "side");
				if (g_strcmp0(side, "debit") != 0 && g_strcmp0(side, "credit") != 0)
					return refuse(error, "invalid legacy journal side");
			}
		}
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
	if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return refuse(error, "accounting pack must be an object");
	root = json_node_get_object(json_parser_get_root(parser));
	if (json_object_has_member(root, "version") &&
		json_node_get_value_type(json_object_get_member(root, "version")) == G_TYPE_INT64 &&
		json_object_get_int_member(root, "version") == 4)
		return snapshot_restore(self, organization_id, root, actor, error);
	if (!restore_preflight(root, error))
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!venture_setup_seed_defaults(self->database, organization_id, actor, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	if (!restore_accounts(self, organization_id, json_object_has_member(root, "accounts") ?
			json_object_get_array_member(root, "accounts") : NULL, actor, error) ||
		!restore_journals(self, organization_id, json_object_get_array_member(root, "journals"), actor, error))
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

gchar *
venture_backup_service_snapshot(VentureBackupService *self, gint64 organization_id, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_return_val_if_fail(VENTURE_IS_BACKUP_SERVICE(self), NULL);
	node = snapshot_export(self, organization_id, error);
	if (node == NULL)
		return NULL;
	return venture_json_to_string(node, FALSE);
}
