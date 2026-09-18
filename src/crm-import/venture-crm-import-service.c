/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

/* A CRM migration is a cutover for the customer side of the house: a
 * mapped manifest, a preview that refuses before writing, an import that
 * is one transaction and records every source id, and a rollback that
 * removes exactly what it created. The accounting cutover is the pattern;
 * this service touches no accounting record. */

struct _VentureCrmImportService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureCrmImportService, venture_crm_import_service, G_TYPE_OBJECT)

static const gchar *const sources[] = { "hubspot", "zoho_crm", "salesforce", NULL };

/* The file keys a manifest may name, and the row object each becomes. */
static const gchar *const objects[][2] = {
	{ "companies", "company" }, { "contacts", "contact" }, { "deals", "deal" },
	{ "notes", "note" }, { "emails", "note" }, { "calls", "note" }, { "meetings", "note" },
	{ "tasks", "task" }, { NULL, NULL }
};

/* Built-in column maps: canonical field, then the vendor's default export
 * header. emails, calls and meetings share the notes map. */
typedef struct
{
	const gchar *source;
	const gchar *object;
	const gchar *const *pairs;
} ColumnDefaults;

static const gchar *const hubspot_companies[] = {
	"id", "Record ID", "name", "Company name", "domain", "Company Domain Name", "website", "Website URL",
	"phone", "Phone Number", "industry", "Industry", "owner", "Company owner", "created_at", "Create Date", NULL
};
static const gchar *const hubspot_contacts[] = {
	"id", "Record ID", "first_name", "First Name", "last_name", "Last Name", "email", "Email",
	"phone", "Phone Number", "company_id", "Associated Company IDs", "title", "Job Title",
	"owner", "Contact owner", "created_at", "Create Date", NULL
};
static const gchar *const hubspot_deals[] = {
	"id", "Record ID", "name", "Deal Name", "amount", "Amount", "currency", "Currency", "stage", "Deal Stage",
	"pipeline", "Pipeline", "close_date", "Close Date", "owner", "Deal owner",
	"company_id", "Associated Company IDs", "contact_id", "Associated Contact IDs", "created_at", "Create Date", NULL
};
static const gchar *const hubspot_notes[] = {
	"id", "Record ID", "kind", "Activity type", "subject", "Title", "body", "Body", "occurred_at", "Activity date",
	"author", "Activity assigned to", "contact_id", "Associated Contact IDs",
	"company_id", "Associated Company IDs", "deal_id", "Associated Deal IDs", NULL
};
static const gchar *const hubspot_tasks[] = {
	"id", "Record ID", "subject", "Task title", "body", "Task body", "due_at", "Due date", "status", "Task status",
	"kind", "Task type", "owner", "Assigned to", "contact_id", "Associated Contact IDs",
	"company_id", "Associated Company IDs", "deal_id", "Associated Deal IDs", "completed_at", "Completion date", NULL
};
static const gchar *const zoho_companies[] = {
	"id", "Record Id", "name", "Account Name", "website", "Website", "phone", "Phone", "industry", "Industry",
	"owner", "Account Owner", "created_at", "Created Time", NULL
};
static const gchar *const zoho_contacts[] = {
	"id", "Record Id", "first_name", "First Name", "last_name", "Last Name", "email", "Email", "phone", "Phone",
	"company_id", "Account Name.id", "title", "Title", "owner", "Contact Owner", "created_at", "Created Time", NULL
};
static const gchar *const zoho_deals[] = {
	"id", "Record Id", "name", "Deal Name", "amount", "Amount", "currency", "Currency", "stage", "Stage",
	"pipeline", "Pipeline", "close_date", "Closing Date", "owner", "Deal Owner",
	"company_id", "Account Name.id", "contact_id", "Contact Name.id", "created_at", "Created Time", NULL
};
static const gchar *const zoho_notes[] = {
	"id", "Record Id", "subject", "Note Title", "body", "Note Content", "occurred_at", "Created Time",
	"author", "Note Owner", "related_id", "Parent Id", NULL
};
static const gchar *const zoho_tasks[] = {
	"id", "Record Id", "subject", "Subject", "body", "Description", "due_at", "Due Date", "status", "Status",
	"owner", "Task Owner", "contact_id", "Contact Name.id", "related_id", "Related To.id",
	"completed_at", "Closed Time", "created_at", "Created Time", NULL
};
static const gchar *const salesforce_companies[] = {
	"id", "Id", "name", "Name", "website", "Website", "phone", "Phone", "industry", "Industry",
	"owner", "OwnerId", "created_at", "CreatedDate", NULL
};
static const gchar *const salesforce_contacts[] = {
	"id", "Id", "first_name", "FirstName", "last_name", "LastName", "email", "Email", "phone", "Phone",
	"company_id", "AccountId", "title", "Title", "owner", "OwnerId", "created_at", "CreatedDate", NULL
};
static const gchar *const salesforce_deals[] = {
	"id", "Id", "name", "Name", "amount", "Amount", "currency", "CurrencyIsoCode", "stage", "StageName",
	"close_date", "CloseDate", "owner", "OwnerId", "company_id", "AccountId", "contact_id", "ContactId",
	"created_at", "CreatedDate", NULL
};
static const gchar *const salesforce_notes[] = {
	"id", "Id", "subject", "Title", "body", "Body", "occurred_at", "CreatedDate", "author", "CreatedById",
	"related_id", "ParentId", NULL
};
static const gchar *const salesforce_tasks[] = {
	"id", "Id", "subject", "Subject", "body", "Description", "due_at", "ActivityDate", "status", "Status",
	"owner", "OwnerId", "contact_id", "WhoId", "related_id", "WhatId", "kind", "TaskSubtype",
	"completed_at", "CompletedDateTime", "created_at", "CreatedDate", NULL
};
static const ColumnDefaults column_defaults[] = {
	{ "hubspot", "companies", hubspot_companies }, { "hubspot", "contacts", hubspot_contacts },
	{ "hubspot", "deals", hubspot_deals }, { "hubspot", "notes", hubspot_notes }, { "hubspot", "tasks", hubspot_tasks },
	{ "zoho_crm", "companies", zoho_companies }, { "zoho_crm", "contacts", zoho_contacts },
	{ "zoho_crm", "deals", zoho_deals }, { "zoho_crm", "notes", zoho_notes }, { "zoho_crm", "tasks", zoho_tasks },
	{ "salesforce", "companies", salesforce_companies }, { "salesforce", "contacts", salesforce_contacts },
	{ "salesforce", "deals", salesforce_deals }, { "salesforce", "notes", salesforce_notes },
	{ "salesforce", "tasks", salesforce_tasks }
};

/* Columns a file must carry for its rows to mean anything. */
static const gchar *const required_columns[][4] = {
	{ "companies", "id", "name", NULL }, { "contacts", "id", NULL, NULL }, { "deals", "id", "name", "stage" },
	{ "notes", "id", NULL, NULL }, { "tasks", "id", "subject", NULL }, { NULL, NULL, NULL, NULL }
};

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_CRM_IMPORT_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureCrmImportService *self = VENTURE_CRM_IMPORT_SERVICE(object);
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
	VentureCrmImportService *self = VENTURE_CRM_IMPORT_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_crm_import_service_parent_class)->finalize(object);
}

static void
venture_crm_import_service_class_init(VentureCrmImportServiceClass *klass)
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
venture_crm_import_service_init(VentureCrmImportService *self)
{
	(void)self;
}

VentureCrmImportService *
venture_crm_import_service_get(VentureDatabase *database)
{
	VentureCrmImportService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-crm-import-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CRM_IMPORT_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-crm-import-service", self, g_object_unref);
	}
	return self;
}

static gboolean
refuse(GError **error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gboolean
refuse(GError **error, const gchar *format, ...)
{
	g_autofree gchar *message = NULL;
	va_list args;
	va_start(args, format);
	message = g_strdup_vprintf(format, args);
	va_end(args);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureCrmImportService: %s", message);
	return FALSE;
}

gboolean
venture_crm_import_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name;
	VentureCrmImportService *self;
	(void)removal;
	if (record == NULL || database == NULL)
		return TRUE;
	name = venture_entity_get_entity_name(record);
	if (g_strcmp0(name, "crm_import") != 0 && g_strcmp0(name, "crm_import_row") != 0)
		return TRUE;
	self = venture_crm_import_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	return refuse(error, "import batches must be changed through VentureCrmImportService");
}

static gboolean
save_owned(VentureCrmImportService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

/* ------------------------------------------------------------------------
 * Manifest and tables
 * ---------------------------------------------------------------------- */

static const gchar *
obj_str(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : NULL;
}

static JsonObject *
obj_obj(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node != NULL && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static gboolean
blank(const gchar *text)
{
	return text == NULL || *text == '\0';
}

JsonObject *
venture_crm_import_default_columns(const gchar *source, const gchar *object)
{
	guint i, k;
	if (g_strcmp0(object, "emails") == 0 || g_strcmp0(object, "calls") == 0 || g_strcmp0(object, "meetings") == 0)
		object = "notes";
	for (i = 0; i < G_N_ELEMENTS(column_defaults); i++)
	{
		JsonObject *map;
		if (g_strcmp0(column_defaults[i].source, source) != 0 || g_strcmp0(column_defaults[i].object, object) != 0)
			continue;
		map = json_object_new();
		for (k = 0; column_defaults[i].pairs[k] != NULL; k += 2)
			json_object_set_string_member(map, column_defaults[i].pairs[k], column_defaults[i].pairs[k + 1]);
		return map;
	}
	return NULL;
}

/* One parsed file: its rows and a canonical-field -> column index map. */
typedef struct
{
	const gchar *file;
	const gchar *object;
	GPtrArray *rows;
	GHashTable *columns;
} Table;

static void
table_free(Table *table)
{
	g_clear_pointer(&table->rows, g_ptr_array_unref);
	g_clear_pointer(&table->columns, g_hash_table_unref);
	g_free(table);
}

static const gchar *
cell(Table *table, gchar **row, const gchar *field)
{
	gpointer index;
	guint i;
	if (!g_hash_table_lookup_extended(table->columns, field, NULL, &index))
		return NULL;
	for (i = 0; row[i] != NULL; i++)
		if (i == (guint)GPOINTER_TO_INT(index))
			return row[i];
	return NULL;
}

/* HubSpot lists several associated ids with a semicolon; the first one is
 * the primary association. */
static gchar *
first_id(const gchar *text)
{
	const gchar *end;
	if (blank(text))
		return NULL;
	end = strchr(text, ';');
	return g_strstrip(end != NULL ? g_strndup(text, (gsize)(end - text)) : g_strdup(text));
}

static gboolean
build_columns(Table *table, const gchar *source, JsonObject *overrides, gchar **headers, GError **error)
{
	g_autoptr(JsonObject) map = venture_crm_import_default_columns(source, table->file);
	g_autoptr(GList) fields = NULL;
	GList *l;
	guint r;
	if (map == NULL)
		return refuse(error, "no column map for %s %s", source, table->file);
	if (overrides != NULL)
	{
		JsonObject *specific = obj_obj(overrides, table->file);
		JsonObject *shared = g_strcmp0(table->object, "note") == 0 ? obj_obj(overrides, "notes") : NULL;
		JsonObject *layers[2];
		guint i;
		layers[0] = shared;
		layers[1] = specific;
		for (i = 0; i < G_N_ELEMENTS(layers); i++)
		{
			g_autoptr(GList) members = layers[i] != NULL ? json_object_get_members(layers[i]) : NULL;
			for (l = members; l != NULL; l = l->next)
			{
				const gchar *header = obj_str(layers[i], l->data);
				if (header == NULL)
					return refuse(error, "columns.%s.%s must name a header", table->file, (const gchar *)l->data);
				json_object_set_string_member(map, l->data, header);
			}
		}
	}
	table->columns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	fields = json_object_get_members(map);
	for (l = fields; l != NULL; l = l->next)
	{
		const gchar *header = obj_str(map, l->data);
		guint h;
		for (h = 0; headers[h] != NULL; h++)
		{
			g_autofree gchar *trimmed = g_strstrip(g_strdup(headers[h]));
			if (g_ascii_strcasecmp(trimmed, header) == 0)
			{
				g_hash_table_insert(table->columns, g_strdup(l->data), GINT_TO_POINTER((gint)h));
				break;
			}
		}
	}
	for (r = 0; required_columns[r][0] != NULL; r++)
	{
		guint k;
		if (g_strcmp0(required_columns[r][0], table->file) != 0 &&
			!(g_strcmp0(required_columns[r][0], "notes") == 0 && g_strcmp0(table->object, "note") == 0))
			continue;
		for (k = 1; k < 4 && required_columns[r][k] != NULL; k++)
			if (!g_hash_table_contains(table->columns, required_columns[r][k]))
				return refuse(error, "%s: column \"%s\" (%s) is missing from the header",
					table->file, obj_str(map, required_columns[r][k]), required_columns[r][k]);
	}
	return TRUE;
}

/* Parses every named CSV and resolves its columns. Returned in manifest
 * order so companies precede the contacts that link to them. */
static GPtrArray *
load_tables(JsonObject *manifest, GError **error)
{
	g_autoptr(GPtrArray) tables = g_ptr_array_new_with_free_func((GDestroyNotify)table_free);
	JsonObject *csv = obj_obj(manifest, "csv");
	const gchar *source = obj_str(manifest, "source");
	guint o;
	if (csv == NULL)
	{
		refuse(error, "manifest needs a csv object mapping companies, contacts, deals, notes and tasks to CSV text");
		return NULL;
	}
	{
		g_autoptr(GList) members = json_object_get_members(csv);
		GList *l;
		for (l = members; l != NULL; l = l->next)
		{
			gboolean known = FALSE;
			for (o = 0; objects[o][0] != NULL; o++)
				if (g_strcmp0(objects[o][0], l->data) == 0)
					known = TRUE;
			if (g_strcmp0(l->data, "attachments") == 0)
			{
				refuse(error, "attachments are unsupported: remove csv.attachments and list it under unsupported");
				return NULL;
			}
			if (!known)
			{
				refuse(error, "unknown object \"%s\": expected companies, contacts, deals, notes, emails, calls, meetings or tasks",
					(const gchar *)l->data);
				return NULL;
			}
		}
	}
	for (o = 0; objects[o][0] != NULL; o++)
	{
		const gchar *text = obj_str(csv, objects[o][0]);
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GError) parse_error = NULL;
		Table *table;
		if (text == NULL)
		{
			if (json_object_has_member(csv, objects[o][0]))
			{
				refuse(error, "csv.%s must be CSV text", objects[o][0]);
				return NULL;
			}
			continue;
		}
		rows = venture_csv_parse(text, &parse_error);
		if (rows == NULL)
		{
			refuse(error, "%s: %s", objects[o][0], parse_error->message);
			return NULL;
		}
		if (rows->len == 0)
		{
			refuse(error, "%s: the CSV has no header row", objects[o][0]);
			return NULL;
		}
		table = g_new0(Table, 1);
		table->file = objects[o][0];
		table->object = objects[o][1];
		table->rows = g_steal_pointer(&rows);
		g_ptr_array_add(tables, table);
		if (!build_columns(table, source, obj_obj(manifest, "columns"), g_ptr_array_index(table->rows, 0), error))
			return NULL;
	}
	return g_steal_pointer(&tables);
}

/* ------------------------------------------------------------------------
 * Stage map and deal validation, run before any write
 * ---------------------------------------------------------------------- */

typedef struct
{
	gint64 stage_id;
	gint64 pipeline_id;
	gint kind;
} StageTarget;

static gboolean
resolve_stage(VentureCrmImportService *self, gint64 org, JsonObject *manifest, const gchar *stage,
	StageTarget *target, GError **error)
{
	JsonObject *map = obj_obj(manifest, "stage_map");
	JsonNode *node = map != NULL ? json_object_get_member(map, stage) : NULL;
	g_autoptr(VentureEntity) entity = NULL;
	gint64 id;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return refuse(error, "stage \"%s\" is not in stage_map; map every source stage to a pipeline stage id before importing", stage);
	id = json_node_get_value_type(node) == G_TYPE_STRING
		? g_ascii_strtoll(json_node_get_string(node), NULL, 10) : json_node_get_int(node);
	entity = id > 0 ? venture_database_get(self->database, VENTURE_TYPE_PIPELINE_STAGE, id, NULL) : NULL;
	if (entity == NULL || venture_entity_is_deleted(entity) || venture_entity_get_organization_id(entity) != org)
		return refuse(error, "stage_map maps \"%s\" to pipeline stage %" G_GINT64_FORMAT ", which does not exist in this organization",
			stage, id);
	target->stage_id = id;
	g_object_get(entity, "pipeline-id", &target->pipeline_id, "kind", &target->kind, NULL);
	return TRUE;
}

static VentureMoney *
deal_amount(JsonObject *manifest, Table *table, gchar **row, const gchar *id, GError **error)
{
	const gchar *amount = cell(table, row, "amount");
	const gchar *currency = cell(table, row, "currency");
	g_autoptr(GError) parse_error = NULL;
	VentureMoney *money;
	if (blank(currency))
		currency = obj_str(manifest, "currency");
	if (blank(amount))
		return venture_money_new_zero(!blank(currency) ? currency : NULL);
	if (blank(currency))
	{
		refuse(error, "deal %s has an amount but no currency; add a currency column or a manifest currency", id);
		return NULL;
	}
	if (strlen(currency) != 3 || !g_ascii_isalpha(currency[0]) || !g_ascii_isalpha(currency[1]) || !g_ascii_isalpha(currency[2]))
	{
		refuse(error, "deal %s: currency \"%s\" is not a three-letter ISO code", id, currency);
		return NULL;
	}
	money = venture_money_from_string(amount, currency, &parse_error);
	if (money == NULL)
		refuse(error, "deal %s: %s", id, parse_error->message);
	return money;
}

static gboolean
validate_deals(VentureCrmImportService *self, gint64 org, JsonObject *manifest, GPtrArray *tables, GError **error)
{
	guint t, r;
	for (t = 0; t < tables->len; t++)
	{
		Table *table = g_ptr_array_index(tables, t);
		if (g_strcmp0(table->object, "deal") != 0)
			continue;
		for (r = 1; r < table->rows->len; r++)
		{
			gchar **row = g_ptr_array_index(table->rows, r);
			const gchar *id = cell(table, row, "id");
			const gchar *stage = cell(table, row, "stage");
			g_autoptr(VentureMoney) money = NULL;
			StageTarget target;
			if (blank(id))
				continue;
			if (blank(stage))
				return refuse(error, "deal %s has no stage", id);
			if (!resolve_stage(self, org, manifest, stage, &target, error))
				return FALSE;
			if (target.kind == 2)
			{
				gint64 reason_id = venture_json_object_get_int(manifest, "loss_reason_id", 0);
				g_autoptr(VentureEntity) reason = reason_id > 0
					? venture_database_get(self->database, VENTURE_TYPE_LOSS_REASON, reason_id, NULL) : NULL;
				gboolean active = FALSE;
				if (reason != NULL)
					g_object_get(reason, "active", &active, NULL);
				if (reason == NULL || !active || venture_entity_get_organization_id(reason) != org)
					return refuse(error, "stage \"%s\" is a lost stage: the manifest needs loss_reason_id naming an active loss reason", stage);
			}
			money = deal_amount(manifest, table, row, id, error);
			if (money == NULL)
				return FALSE;
		}
	}
	return TRUE;
}

/* ------------------------------------------------------------------------
 * Preview
 * ---------------------------------------------------------------------- */

static gboolean
add_row(VentureCrmImportService *self, gint64 import_id, gint64 org, const gchar *object, const gchar *source_id,
	const gchar *status, const gchar *exception, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureCrmImportRow) row = venture_crm_import_row_new();
	g_object_set(row, "import-id", import_id, "source-object", object, "source-id", source_id,
		"status", status, "exception", exception, "created", FALSE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(row), org);
	return save_owned(self, VENTURE_ENTITY(row), actor, error);
}

VentureEntity *
venture_crm_import_service_preview(VentureCrmImportService *self, gint64 organization_id,
	JsonObject *manifest, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureCrmImport) batch = NULL;
	g_autoptr(GPtrArray) tables = NULL;
	g_autoptr(GString) report = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *manifest_text = NULL;
	const gchar *source;
	JsonArray *unsupported;
	guint t, r, i;
	g_return_val_if_fail(VENTURE_IS_CRM_IMPORT_SERVICE(self), NULL);
	if (manifest == NULL)
	{
		refuse(error, "a mapped manifest is required");
		return NULL;
	}
	source = obj_str(manifest, "source");
	if (!g_strv_contains(sources, source != NULL ? source : ""))
	{
		refuse(error, "source must be hubspot, zoho_crm or salesforce");
		return NULL;
	}
	tables = load_tables(manifest, error);
	if (tables == NULL)
		return NULL;
	if (!validate_deals(self, organization_id, manifest, tables, error))
		return NULL;
	report = g_string_new(NULL);
	g_string_append_printf(report, "CRM import preview (%s)\n", source);
	for (t = 0; t < tables->len; t++)
	{
		Table *table = g_ptr_array_index(tables, t);
		g_string_append_printf(report, "- %s: %u rows\n", table->file, table->rows->len - 1);
	}
	g_string_append(report, "Unsupported: attachments");
	unsupported = json_object_has_member(manifest, "unsupported") ? json_object_get_array_member(manifest, "unsupported") : NULL;
	for (i = 0; unsupported != NULL && i < json_array_get_length(unsupported); i++)
		g_string_append_printf(report, ", %s", json_array_get_string_element(unsupported, i));
	g_string_append_c(report, '\n');
	json_node_set_object(node, json_object_ref(manifest));
	manifest_text = venture_json_to_string(node, FALSE);
	if (!venture_database_begin(self->database, error))
		return NULL;
	batch = venture_crm_import_new();
	g_object_set(batch, "source", source, "state", "preview", "manifest", manifest_text, "report", report->str, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(batch), organization_id);
	if (!save_owned(self, VENTURE_ENTITY(batch), actor, error))
		goto fail;
	for (t = 0; t < tables->len; t++)
	{
		Table *table = g_ptr_array_index(tables, t);
		for (r = 1; r < table->rows->len; r++)
		{
			gchar **row = g_ptr_array_index(table->rows, r);
			const gchar *id = cell(table, row, "id");
			g_autofree gchar *placeholder = NULL;
			if (blank(id))
			{
				placeholder = g_strdup_printf("%s#%u", table->file, r);
				if (!add_row(self, venture_entity_get_id(VENTURE_ENTITY(batch)), organization_id, table->object,
					placeholder, "skipped", "row has no source id", actor, error))
					goto fail;
				continue;
			}
			if (!add_row(self, venture_entity_get_id(VENTURE_ENTITY(batch)), organization_id, table->object, id,
				"preview", NULL, actor, error))
				goto fail;
		}
	}
	if (!venture_database_commit(self->database, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&batch));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

/* ------------------------------------------------------------------------
 * Import
 * ---------------------------------------------------------------------- */

/* Everything one import run needs: the batch, its rows keyed by
 * object and source id, and a lookup of source id -> record across this
 * and earlier batches. */
typedef struct
{
	VentureCrmImportService *self;
	VentureEntity *batch;
	JsonObject *manifest;
	gint64 org;
	const gchar *source;
	GHashTable *rows;
	const VentureActor *actor;
} Run;

static gchar *
row_key(const gchar *object, const gchar *source_id)
{
	return g_strdup_printf("%s\x1f%s", object, source_id);
}

static JsonObject *
manifest_of(VentureEntity *batch)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_object_get(batch, "manifest", &text, NULL);
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
		return NULL;
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static GPtrArray *
batch_rows(VentureCrmImportService *self, VentureEntity *batch, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CRM_IMPORT_ROW);
	venture_query_set_organization(query, venture_entity_get_organization_id(batch));
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	if (!venture_query_add_filter_int(query, "import-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(batch), error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static VentureEntity *
run_row(Run *run, const gchar *object, const gchar *source_id)
{
	g_autofree gchar *key = row_key(object, source_id);
	return g_hash_table_lookup(run->rows, key);
}

static gboolean
finish_row(Run *run, VentureEntity *row, const gchar *status, const gchar *exception, const gchar *record_type,
	gint64 record_id, gboolean created, GError **error)
{
	g_object_set(row, "status", status, "exception", exception, "record-type", record_type,
		"record-id", record_id, "created", created, NULL);
	return save_owned(run->self, row, run->actor, error);
}

/* A record already imported for this source id, in this batch or an
 * earlier one that was not rolled back. Zero when none, -1 on error. */
static gint64
imported_record(Run *run, const gchar *object, const gchar *source_id, gchar **record_type, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CRM_IMPORT_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	if (blank(source_id))
		return 0;
	venture_query_set_organization(query, run->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	if (!venture_query_add_filter_string(query, "source-object", VENTURE_FILTER_OP_EQ, object, error) ||
		!venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, error))
		return -1;
	rows = venture_database_find(run->self->database, query, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *status = NULL;
		gint64 record_id = 0;
		g_object_get(g_ptr_array_index(rows, i), "status", &status, "record-id", &record_id, NULL);
		if (record_id > 0 && g_strcmp0(status, "rolled_back") != 0 && g_strcmp0(status, "preview") != 0)
		{
			if (record_type != NULL)
				g_object_get(g_ptr_array_index(rows, i), "record-type", record_type, NULL);
			return record_id;
		}
	}
	return 0;
}

/* The source owner, through the manifest's owner_map when it names them. */
static const gchar *
owner_of(Run *run, const gchar *owner)
{
	JsonObject *map = obj_obj(run->manifest, "owner_map");
	const gchar *mapped = map != NULL && !blank(owner) ? obj_str(map, owner) : NULL;
	return mapped != NULL ? mapped : (blank(owner) ? NULL : owner);
}

static gboolean
parse_time(const gchar *text, GDateTime **out, const gchar *object, const gchar *id, const gchar *field, GError **error)
{
	g_autoptr(GError) parse_error = NULL;
	*out = NULL;
	if (blank(text))
		return TRUE;
	*out = venture_time_from_string(text, &parse_error);
	if (*out == NULL)
		return refuse(error, "%s %s: cannot read %s \"%s\"", object, id, field, text);
	return TRUE;
}

static GPtrArray *
live_records(Run *run, GType type, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, run->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(run->self->database, query, error);
}

static gchar *
string_of(VentureEntity *entity, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(entity, name, &value, NULL);
	return value;
}

static gboolean
same_text(const gchar *a, const gchar *b)
{
	g_autofree gchar *na = g_strstrip(g_utf8_casefold(a != NULL ? a : "", -1));
	g_autofree gchar *nb = g_strstrip(g_utf8_casefold(b != NULL ? b : "", -1));
	return g_str_equal(na, nb);
}

static gchar *
digits_only(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;
	for (p = text != NULL ? text : ""; *p != '\0'; p++)
		if (g_ascii_isdigit(*p))
			g_string_append_c(out, *p);
	return g_string_free(out, FALSE);
}

/* Appends "field differs" when both sides carry a value and they disagree
 * after the leads normalisation. An empty side is not a conflict: the
 * import never fills or clears a field on a matched record. */
static void
note_conflict(GString *conflicts, const gchar *field, const gchar *existing, const gchar *incoming, guint kind)
{
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	if (blank(existing) || blank(incoming))
		return;
	if (kind == 0)
	{
		a = venture_lead_normalize_email(existing);
		b = venture_lead_normalize_email(incoming);
	}
	else if (kind == 1)
	{
		a = digits_only(existing);
		b = digits_only(incoming);
	}
	else if (kind == 2)
	{
		a = venture_lead_normalize_website(existing);
		b = venture_lead_normalize_website(incoming);
	}
	if (a != NULL ? !g_str_equal(a, b) : !same_text(existing, incoming))
	{
		if (conflicts->len > 0)
			g_string_append(conflicts, "; ");
		g_string_append_printf(conflicts, "%s differs (existing \"%s\", source \"%s\")", field, existing, incoming);
	}
}

/* Companies: matched by normalised domain, website or email against live
 * companies, per leads.org. */
static VentureEntity *
match_company(Run *run, const gchar *domain, const gchar *website, const gchar *email, GError **error)
{
	g_autoptr(GPtrArray) companies = live_records(run, VENTURE_TYPE_COMPANY, error);
	g_autofree gchar *want_domain = venture_lead_normalize_website(domain);
	g_autofree gchar *want_site = venture_lead_normalize_website(website);
	g_autofree gchar *want_email = venture_lead_normalize_email(email);
	guint i;
	if (companies == NULL)
		return NULL;
	for (i = 0; i < companies->len; i++)
	{
		VentureEntity *company = g_ptr_array_index(companies, i);
		g_autofree gchar *site = string_of(company, "website");
		g_autofree gchar *mail = string_of(company, "email");
		g_autofree gchar *have_site = venture_lead_normalize_website(site);
		g_autofree gchar *have_email = venture_lead_normalize_email(mail);
		if ((*have_site != '\0' && (g_str_equal(have_site, want_domain) || g_str_equal(have_site, want_site))) ||
			(*have_email != '\0' && g_str_equal(have_email, want_email)))
			return g_object_ref(company);
	}
	return NULL;
}

static VentureEntity *
match_contact(Run *run, const gchar *email, GError **error)
{
	g_autoptr(GPtrArray) contacts = NULL;
	g_autofree gchar *want = venture_lead_normalize_email(email);
	guint i;
	if (*want == '\0')
		return NULL;
	contacts = live_records(run, VENTURE_TYPE_CONTACT, error);
	if (contacts == NULL)
		return NULL;
	for (i = 0; i < contacts->len; i++)
	{
		VentureEntity *contact = g_ptr_array_index(contacts, i);
		g_autofree gchar *mail = string_of(contact, "email");
		g_autofree gchar *have = venture_lead_normalize_email(mail);
		if (g_str_equal(have, want))
			return g_object_ref(contact);
	}
	return NULL;
}

/* Resolves a source id of one object to the record it became, through
 * this batch's rows first and then earlier batches. */
static gint64
resolve_ref(Run *run, const gchar *object, const gchar *source_id, GError **error)
{
	VentureEntity *row;
	g_autofree gchar *id = first_id(source_id);
	gint64 record_id = 0;
	if (id == NULL)
		return 0;
	row = run_row(run, object, id);
	if (row != NULL)
		g_object_get(row, "record-id", &record_id, NULL);
	if (record_id > 0)
		return record_id;
	return imported_record(run, object, id, NULL, error);
}

/* A vendor "parent" or "related to" id names a contact, company or deal
 * without saying which; each object is tried in turn. */
static gboolean
resolve_related(Run *run, const gchar *related, gint64 *contact, gint64 *company, gint64 *deal, GError **error)
{
	gint64 id;
	if (blank(related))
		return TRUE;
	if (*contact == 0)
	{
		id = resolve_ref(run, "contact", related, error);
		if (id < 0) return FALSE;
		if (id > 0) { *contact = id; return TRUE; }
	}
	if (*company == 0)
	{
		id = resolve_ref(run, "company", related, error);
		if (id < 0) return FALSE;
		if (id > 0) { *company = id; return TRUE; }
	}
	if (*deal == 0)
	{
		id = resolve_ref(run, "deal", related, error);
		if (id < 0) return FALSE;
		if (id > 0) { *deal = id; return TRUE; }
	}
	return TRUE;
}

static gboolean
row_pending(VentureEntity *row)
{
	g_autofree gchar *status = string_of(row, "status");
	gint64 record_id = 0;
	g_object_get(row, "record-id", &record_id, NULL);
	return g_strcmp0(status, "preview") == 0 && record_id == 0;
}

static gboolean
import_companies(Run *run, Table *table, GError **error)
{
	guint r;
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, "company", id);
		g_autoptr(VentureEntity) existing = NULL;
		g_autoptr(VentureCompany) company = NULL;
		g_autoptr(GString) conflicts = NULL;
		gint64 earlier;
		if (tracking == NULL || !row_pending(tracking))
			continue;
		earlier = imported_record(run, "company", id, NULL, error);
		if (earlier < 0)
			return FALSE;
		if (earlier > 0)
		{
			if (!finish_row(run, tracking, "matched", NULL, "company", earlier, FALSE, error))
				return FALSE;
			continue;
		}
		existing = match_company(run, cell(table, row, "domain"), cell(table, row, "website"), cell(table, row, "email"), error);
		if (existing == NULL && error != NULL && *error != NULL)
			return FALSE;
		if (existing != NULL)
		{
			g_autofree gchar *name = string_of(existing, "name");
			g_autofree gchar *phone = string_of(existing, "phone");
			g_autofree gchar *industry = string_of(existing, "industry");
			g_autofree gchar *website = string_of(existing, "website");
			conflicts = g_string_new(NULL);
			note_conflict(conflicts, "name", name, cell(table, row, "name"), 3);
			note_conflict(conflicts, "phone", phone, cell(table, row, "phone"), 1);
			note_conflict(conflicts, "industry", industry, cell(table, row, "industry"), 3);
			note_conflict(conflicts, "website", website, cell(table, row, "website"), 2);
			if (!finish_row(run, tracking, conflicts->len > 0 ? "exception" : "matched",
				conflicts->len > 0 ? conflicts->str : NULL, "company", venture_entity_get_id(existing), FALSE, error))
				return FALSE;
			continue;
		}
		company = venture_company_new();
		g_object_set(company, "name", cell(table, row, "name"), "website", cell(table, row, "website"),
			"phone", cell(table, row, "phone"), "industry", cell(table, row, "industry"),
			"email", cell(table, row, "email"), "source", run->source, "external-id", id, "active", TRUE, NULL);
		if (blank(cell(table, row, "website")) && !blank(cell(table, row, "domain")))
			g_object_set(company, "website", cell(table, row, "domain"), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(company), run->org);
		if (!venture_database_save(run->self->database, VENTURE_ENTITY(company), run->actor, error))
			return FALSE;
		if (!finish_row(run, tracking, "imported", NULL, "company", venture_entity_get_id(VENTURE_ENTITY(company)), TRUE, error))
			return FALSE;
	}
	return TRUE;
}

static gchar *
contact_name(Table *table, gchar **row)
{
	const gchar *name = cell(table, row, "name");
	const gchar *first = cell(table, row, "first_name");
	const gchar *last = cell(table, row, "last_name");
	const gchar *email = cell(table, row, "email");
	if (!blank(name))
		return g_strdup(name);
	if (!blank(first) || !blank(last))
	{
		g_autofree gchar *joined = g_strdup_printf("%s %s", first != NULL ? first : "", last != NULL ? last : "");
		return g_strdup(g_strstrip(joined));
	}
	return g_strdup(!blank(email) ? email : "Imported contact");
}

static gboolean
import_contacts(Run *run, Table *table, GError **error)
{
	guint r;
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		const gchar *company_source = cell(table, row, "company_id");
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, "contact", id);
		g_autoptr(VentureEntity) existing = NULL;
		g_autoptr(VentureContact) contact = NULL;
		g_autoptr(GString) conflicts = NULL;
		g_autofree gchar *name = NULL;
		gint64 earlier, company_id;
		if (tracking == NULL || !row_pending(tracking))
			continue;
		earlier = imported_record(run, "contact", id, NULL, error);
		if (earlier < 0)
			return FALSE;
		if (earlier > 0)
		{
			if (!finish_row(run, tracking, "matched", NULL, "contact", earlier, FALSE, error))
				return FALSE;
			continue;
		}
		name = contact_name(table, row);
		existing = match_contact(run, cell(table, row, "email"), error);
		if (existing == NULL && error != NULL && *error != NULL)
			return FALSE;
		if (existing != NULL)
		{
			g_autofree gchar *have_name = string_of(existing, "name");
			g_autofree gchar *phone = string_of(existing, "phone");
			g_autofree gchar *role = string_of(existing, "role");
			conflicts = g_string_new(NULL);
			note_conflict(conflicts, "name", have_name, name, 3);
			note_conflict(conflicts, "phone", phone, cell(table, row, "phone"), 1);
			note_conflict(conflicts, "role", role, cell(table, row, "title"), 3);
			if (!finish_row(run, tracking, conflicts->len > 0 ? "exception" : "matched",
				conflicts->len > 0 ? conflicts->str : NULL, "contact", venture_entity_get_id(existing), FALSE, error))
				return FALSE;
			continue;
		}
		company_id = resolve_ref(run, "company", company_source, error);
		if (company_id < 0)
			return FALSE;
		contact = venture_contact_new();
		g_object_set(contact, "name", name, "email", cell(table, row, "email"), "phone", cell(table, row, "phone"),
			"role", cell(table, row, "title"), "source", run->source, "company-id", company_id, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(contact), run->org);
		if (!venture_database_save(run->self->database, VENTURE_ENTITY(contact), run->actor, error))
			return FALSE;
		if (company_id == 0 && !blank(company_source))
		{
			g_autofree gchar *message = g_strdup_printf("company %s is not in the export or an earlier import; contact created without a company",
				company_source);
			if (!finish_row(run, tracking, "exception", message, "contact", venture_entity_get_id(VENTURE_ENTITY(contact)), TRUE, error))
				return FALSE;
			continue;
		}
		if (!finish_row(run, tracking, "imported", NULL, "contact", venture_entity_get_id(VENTURE_ENTITY(contact)), TRUE, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_deals(Run *run, Table *table, GError **error)
{
	VentureDealService *deals = venture_database_get_deal_service(run->self->database);
	guint r;
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, "deal", id);
		g_autoptr(VentureDeal) deal = NULL;
		g_autoptr(VentureDeal) moved = NULL;
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(GDateTime) close = NULL;
		g_autoptr(GString) missing = NULL;
		g_autofree gchar *note = NULL;
		StageTarget target;
		gint64 earlier, company_id, contact_id;
		if (tracking == NULL || !row_pending(tracking))
			continue;
		earlier = imported_record(run, "deal", id, NULL, error);
		if (earlier < 0)
			return FALSE;
		if (earlier > 0)
		{
			if (!finish_row(run, tracking, "matched", NULL, "deal", earlier, FALSE, error))
				return FALSE;
			continue;
		}
		if (!resolve_stage(run->self, run->org, run->manifest, cell(table, row, "stage"), &target, error))
			return FALSE;
		value = deal_amount(run->manifest, table, row, id, error);
		if (value == NULL)
			return FALSE;
		if (!parse_time(cell(table, row, "close_date"), &close, "deal", id, "close date", error))
			return FALSE;
		company_id = resolve_ref(run, "company", cell(table, row, "company_id"), error);
		if (company_id < 0)
			return FALSE;
		contact_id = resolve_ref(run, "contact", cell(table, row, "contact_id"), error);
		if (contact_id < 0)
			return FALSE;
		missing = g_string_new(NULL);
		if (company_id == 0 && !blank(cell(table, row, "company_id")))
			g_string_append_printf(missing, "company %s not found", cell(table, row, "company_id"));
		if (contact_id == 0 && !blank(cell(table, row, "contact_id")))
			g_string_append_printf(missing, "%scontact %s not found", missing->len > 0 ? "; " : "", cell(table, row, "contact_id"));
		deal = venture_deal_new();
		g_object_set(deal, "name", cell(table, row, "name"), "value", value, "owner", owner_of(run, cell(table, row, "owner")),
			"expected-close-at", close, "source", run->source, "company-id", company_id, "contact-id", contact_id,
			"pipeline-id", target.pipeline_id, NULL);
		if (target.kind == 2)
			g_object_set(deal, "loss-reason-id", venture_json_object_get_int(run->manifest, "loss_reason_id", 0), NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(deal), run->org);
		if (!venture_database_save(run->self->database, VENTURE_ENTITY(deal), run->actor, error))
			return FALSE;
		note = g_strdup_printf("Imported from %s stage \"%s\"", run->source, cell(table, row, "stage"));
		moved = venture_deal_service_move_stage(deals, deal, target.stage_id, note, run->actor, error);
		if (moved == NULL)
			return FALSE;
		if (!finish_row(run, tracking, missing->len > 0 ? "exception" : "imported", missing->len > 0 ? missing->str : NULL,
			"deal", venture_entity_get_id(VENTURE_ENTITY(deal)), TRUE, error))
			return FALSE;
	}
	return TRUE;
}

/* The vendor's activity type word, or the file it came from, decides the
 * kind of history or next action a row becomes. */
static gint
history_kind(const gchar *file, const gchar *word)
{
	g_autofree gchar *lower = g_ascii_strdown(word != NULL ? word : "", -1);
	if (strstr(lower, "email") != NULL || g_strcmp0(file, "emails") == 0)
		return VENTURE_INTERACTION_KIND_EMAIL;
	if (strstr(lower, "call") != NULL || g_strcmp0(file, "calls") == 0)
		return VENTURE_INTERACTION_KIND_CALL;
	if (strstr(lower, "meeting") != NULL || g_strcmp0(file, "meetings") == 0)
		return VENTURE_INTERACTION_KIND_MEETING;
	return VENTURE_INTERACTION_KIND_NOTE;
}

static gint
activity_kind(const gchar *word)
{
	g_autofree gchar *lower = g_ascii_strdown(word != NULL ? word : "", -1);
	if (strstr(lower, "email") != NULL)
		return VENTURE_ACTIVITY_KIND_EMAIL;
	if (strstr(lower, "call") != NULL)
		return VENTURE_ACTIVITY_KIND_CALL;
	if (strstr(lower, "meeting") != NULL)
		return VENTURE_ACTIVITY_KIND_MEETING;
	return VENTURE_ACTIVITY_KIND_TASK;
}

static const gchar *
kind_label(gint kind)
{
	switch (kind)
	{
		case VENTURE_INTERACTION_KIND_EMAIL: return "Email";
		case VENTURE_INTERACTION_KIND_CALL: return "Call";
		case VENTURE_INTERACTION_KIND_MEETING: return "Meeting";
		default: return "Note";
	}
}

/* The interaction record has no author field; the source author is kept
 * as an attribution line so the timeline still says who wrote it. */
static gchar *
attributed_body(Run *run, const gchar *body, const gchar *author)
{
	if (blank(author))
		return g_strdup(body != NULL ? body : "");
	return g_strdup_printf("%s%s\xe2\x80\x94 %s (%s)", body != NULL ? body : "", blank(body) ? "" : "\n\n", author, run->source);
}

static gboolean
resolve_history_refs(Run *run, Table *table, gchar **row, gint64 *contact, gint64 *company, gint64 *deal, GError **error)
{
	*contact = resolve_ref(run, "contact", cell(table, row, "contact_id"), error);
	if (*contact < 0) return FALSE;
	*company = resolve_ref(run, "company", cell(table, row, "company_id"), error);
	if (*company < 0) return FALSE;
	*deal = resolve_ref(run, "deal", cell(table, row, "deal_id"), error);
	if (*deal < 0) return FALSE;
	if (!resolve_related(run, cell(table, row, "related_id"), contact, company, deal, error))
		return FALSE;
	if (*company == 0 && *contact > 0)
	{
		g_autoptr(VentureEntity) person = venture_database_get(run->self->database, VENTURE_TYPE_CONTACT, *contact, NULL);
		if (person != NULL)
			g_object_get(person, "company-id", company, NULL);
	}
	return TRUE;
}

static gboolean
write_interaction(Run *run, VentureEntity *tracking, gint kind, const gchar *subject, const gchar *body,
	const gchar *author, GDateTime *when, gint64 contact, gint64 company, gint64 deal, GError **error)
{
	g_autoptr(VentureInteraction) interaction = venture_interaction_new();
	g_autofree gchar *text = attributed_body(run, body, author);
	g_object_set(interaction, "kind", kind, "subject", !blank(subject) ? subject : kind_label(kind), "body", text,
		"occurred-at", when, "contact-id", contact, "company-id", company, "deal-id", deal, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(interaction), run->org);
	if (!venture_database_save(run->self->database, VENTURE_ENTITY(interaction), run->actor, error))
		return FALSE;
	return finish_row(run, tracking, "imported", NULL, "interaction", venture_entity_get_id(VENTURE_ENTITY(interaction)), TRUE, error);
}

static gboolean
import_notes(Run *run, Table *table, GError **error)
{
	guint r;
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, "note", id);
		g_autoptr(GDateTime) when = NULL;
		gint64 contact, company, deal, earlier;
		if (tracking == NULL || !row_pending(tracking))
			continue;
		earlier = imported_record(run, "note", id, NULL, error);
		if (earlier < 0)
			return FALSE;
		if (earlier > 0)
		{
			if (!finish_row(run, tracking, "matched", NULL, "interaction", earlier, FALSE, error))
				return FALSE;
			continue;
		}
		if (!parse_time(cell(table, row, "occurred_at"), &when, "note", id, "date", error))
			return FALSE;
		if (!resolve_history_refs(run, table, row, &contact, &company, &deal, error))
			return FALSE;
		if (contact == 0 && company == 0 && deal == 0)
		{
			if (!finish_row(run, tracking, "exception", "no contact, company or deal in the export or an earlier import; note not written",
				NULL, 0, FALSE, error))
				return FALSE;
			continue;
		}
		if (!write_interaction(run, tracking, history_kind(table->file, cell(table, row, "kind")), cell(table, row, "subject"),
			cell(table, row, "body"), cell(table, row, "author"), when, contact, company, deal, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
task_completed(const gchar *status)
{
	g_autofree gchar *lower = g_ascii_strdown(status != NULL ? status : "", -1);
	g_strstrip(lower);
	return g_str_equal(lower, "completed") || g_str_equal(lower, "done") || g_str_equal(lower, "closed");
}

static gboolean
import_tasks(Run *run, Table *table, GError **error)
{
	guint r;
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, "task", id);
		g_autoptr(GDateTime) due = NULL;
		g_autoptr(GDateTime) completed = NULL;
		g_autoptr(GDateTime) created = NULL;
		g_autoptr(VentureActivity) activity = NULL;
		g_autofree gchar *type = NULL;
		gint64 contact, company, deal, earlier;
		if (tracking == NULL || !row_pending(tracking))
			continue;
		earlier = imported_record(run, "task", id, &type, error);
		if (earlier < 0)
			return FALSE;
		if (earlier > 0)
		{
			if (!finish_row(run, tracking, "matched", NULL, type, earlier, FALSE, error))
				return FALSE;
			continue;
		}
		if (!parse_time(cell(table, row, "due_at"), &due, "task", id, "due date", error) ||
			!parse_time(cell(table, row, "completed_at"), &completed, "task", id, "completion date", error) ||
			!parse_time(cell(table, row, "created_at"), &created, "task", id, "creation date", error))
			return FALSE;
		if (!resolve_history_refs(run, table, row, &contact, &company, &deal, error))
			return FALSE;
		if (task_completed(cell(table, row, "status")))
		{
			GDateTime *when = completed != NULL ? completed : (due != NULL ? due : created);
			if (contact == 0 && company == 0 && deal == 0)
			{
				if (!finish_row(run, tracking, "exception", "no contact, company or deal in the export or an earlier import; completed task not written",
					NULL, 0, FALSE, error))
					return FALSE;
				continue;
			}
			if (!write_interaction(run, tracking, history_kind(table->file, cell(table, row, "kind")), cell(table, row, "subject"),
				cell(table, row, "body"), cell(table, row, "owner"), when, contact, company, deal, error))
				return FALSE;
			continue;
		}
		activity = venture_activity_new();
		g_object_set(activity, "subject", cell(table, row, "subject"), "body", cell(table, row, "body"),
			"kind", activity_kind(cell(table, row, "kind")), "owner", owner_of(run, cell(table, row, "owner")),
			"due-at", due, "status", VENTURE_ACTIVITY_STATUS_PLANNED,
			"contact-id", contact, "company-id", company, "deal-id", deal, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(activity), run->org);
		if (!venture_database_save(run->self->database, VENTURE_ENTITY(activity), run->actor, error))
			return FALSE;
		if (!finish_row(run, tracking, "imported", NULL, "activity", venture_entity_get_id(VENTURE_ENTITY(activity)), TRUE, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_impl(VentureCrmImportService *self, VentureCrmImport *batch, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonObject) manifest = NULL;
	g_autoptr(GPtrArray) tables = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) index = NULL;
	g_autofree gchar *state = NULL;
	Run run;
	guint i, t;
	g_object_get(batch, "state", &state, NULL);
	if (g_strcmp0(state, "preview") != 0 && g_strcmp0(state, "imported") != 0)
		return refuse(error, "import requires a previewed batch; this one is %s", state != NULL ? state : "unknown");
	manifest = manifest_of(VENTURE_ENTITY(batch));
	if (manifest == NULL)
		return refuse(error, "the batch manifest is missing");
	tables = load_tables(manifest, error);
	if (tables == NULL)
		return FALSE;
	if (!validate_deals(self, venture_entity_get_organization_id(VENTURE_ENTITY(batch)), manifest, tables, error))
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	rows = batch_rows(self, VENTURE_ENTITY(batch), error);
	if (rows == NULL)
		goto fail;
	index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *object = string_of(row, "source-object");
		g_autofree gchar *source_id = string_of(row, "source-id");
		g_hash_table_insert(index, row_key(object, source_id), row);
	}
	run.self = self;
	run.batch = VENTURE_ENTITY(batch);
	run.manifest = manifest;
	run.org = venture_entity_get_organization_id(VENTURE_ENTITY(batch));
	run.source = obj_str(manifest, "source");
	run.rows = index;
	run.actor = actor;
	for (t = 0; t < tables->len; t++)
	{
		Table *table = g_ptr_array_index(tables, t);
		gboolean ok;
		if (g_strcmp0(table->object, "company") == 0)
			ok = import_companies(&run, table, error);
		else if (g_strcmp0(table->object, "contact") == 0)
			ok = import_contacts(&run, table, error);
		else if (g_strcmp0(table->object, "deal") == 0)
			ok = import_deals(&run, table, error);
		else if (g_strcmp0(table->object, "note") == 0)
			ok = import_notes(&run, table, error);
		else
			ok = import_tasks(&run, table, error);
		if (!ok)
			goto fail;
	}
	g_object_set(batch, "state", "imported", NULL);
	if (!save_owned(self, VENTURE_ENTITY(batch), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_crm_import_service_import(VentureCrmImportService *self, VentureCrmImport *batch,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CRM_IMPORT_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_CRM_IMPORT(batch), FALSE);
	if (self->database == NULL)
		return refuse(error, "database is unavailable");
	return import_impl(self, batch, actor, error);
}

gboolean
venture_crm_import_service_activate(VentureCrmImportService *self, VentureCrmImport *batch,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *state = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_return_val_if_fail(VENTURE_IS_CRM_IMPORT_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_CRM_IMPORT(batch), FALSE);
	g_object_get(batch, "state", &state, NULL);
	if (g_strcmp0(state, "active") == 0)
		return TRUE;
	if (g_strcmp0(state, "imported") != 0)
		return refuse(error, "activate requires an imported batch; this one is %s", state != NULL ? state : "unknown");
	now = venture_time_now();
	g_object_set(batch, "state", "active", "activated-at", now, NULL);
	return save_owned(self, VENTURE_ENTITY(batch), actor, error);
}

/* ------------------------------------------------------------------------
 * Rollback
 * ---------------------------------------------------------------------- */

static GType
record_gtype(const gchar *type)
{
	if (g_strcmp0(type, "company") == 0) return VENTURE_TYPE_COMPANY;
	if (g_strcmp0(type, "contact") == 0) return VENTURE_TYPE_CONTACT;
	if (g_strcmp0(type, "deal") == 0) return VENTURE_TYPE_DEAL;
	if (g_strcmp0(type, "interaction") == 0) return VENTURE_TYPE_INTERACTION;
	if (g_strcmp0(type, "activity") == 0) return VENTURE_TYPE_ACTIVITY;
	return G_TYPE_INVALID;
}

static gboolean
rollback_impl(VentureCrmImportService *self, VentureCrmImport *batch, const VentureActor *actor, GError **error)
{
	static const gchar *const order[] = { "activity", "interaction", "deal", "contact", "company" };
	g_autofree gchar *state = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i, o;
	g_object_get(batch, "state", &state, NULL);
	if (g_strcmp0(state, "active") == 0)
		return refuse(error, "an activated import cannot be rolled back");
	if (g_strcmp0(state, "rolled_back") == 0)
		return TRUE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	rows = batch_rows(self, VENTURE_ENTITY(batch), error);
	if (rows == NULL)
		goto fail;
	/* History and next actions first, then the records they hang off. */
	for (o = 0; o < G_N_ELEMENTS(order); o++)
	{
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *type = NULL;
			g_autoptr(VentureEntity) record = NULL;
			gint64 record_id = 0;
			gboolean created = FALSE;
			g_object_get(row, "record-type", &type, "record-id", &record_id, "created", &created, NULL);
			if (!created || record_id <= 0 || g_strcmp0(type, order[o]) != 0)
				continue;
			record = venture_database_get(self->database, record_gtype(type), record_id, error);
			if (record == NULL)
				goto fail;
			if (!venture_entity_is_deleted(record) && !venture_database_delete(self->database, record, actor, error))
				goto fail;
		}
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_object_set(row, "status", "rolled_back", NULL);
		if (!save_owned(self, row, actor, error))
			goto fail;
	}
	g_object_set(batch, "state", "rolled_back", NULL);
	if (!save_owned(self, VENTURE_ENTITY(batch), actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_crm_import_service_rollback(VentureCrmImportService *self, VentureCrmImport *batch,
	const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CRM_IMPORT_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_CRM_IMPORT(batch), FALSE);
	if (self->database == NULL)
		return refuse(error, "database is unavailable");
	return rollback_impl(self, batch, actor, error);
}

/* ------------------------------------------------------------------------
 * Actions
 * ---------------------------------------------------------------------- */

static gboolean
crm_import_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	(void)error;
	return TRUE;
}

static VentureEntity *
crm_import_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	VentureCrmImportService *service = venture_action_get_data(action);
	(void)params;
	g_object_get(action, "name", &name, NULL);
	if (g_strcmp0(name, "import") == 0)
		return venture_crm_import_service_import(service, VENTURE_CRM_IMPORT(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "activate") == 0)
		return venture_crm_import_service_activate(service, VENTURE_CRM_IMPORT(entity), actor, error) ? g_object_ref(entity) : NULL;
	if (g_strcmp0(name, "rollback") == 0)
		return venture_crm_import_service_rollback(service, VENTURE_CRM_IMPORT(entity), actor, error) ? g_object_ref(entity) : NULL;
	return NULL;
}

void
venture_crm_import_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	static const gchar *const names[] = { "import", "activate", "rollback" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "crm_import",
			"name", names[i], "label", names[i], "description", "CRM import batch action",
			"stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		g_autoptr(GError) error = NULL;
		venture_action_registry_register(registry, action, crm_import_allowed, crm_import_invoke,
			venture_crm_import_service_get(database), NULL, &error);
	}
}
