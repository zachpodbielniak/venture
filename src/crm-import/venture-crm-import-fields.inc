/* SPDX-License-Identifier: AGPL-3.0-or-later */

static gboolean track_auxiliary(Run *run, const gchar *object, const gchar *source_id,
	VentureEntity *record, GError **error);

static JsonArray *
custom_definitions(JsonObject *manifest, const gchar *file)
{
	JsonObject *all = obj_obj(manifest, "custom_fields");
	JsonNode *node = all != NULL ? json_object_get_member(all, file) : NULL;
	return node != NULL && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

static const gchar *
table_record_type(Table *table)
{
	const ImportModule *module = table->module;
	if (module != NULL)
	{
		g_autoptr(VentureEntity) record = g_object_new(module->type(), NULL);
		/* Entity names are class-owned strings. */
		return venture_entity_get_entity_name(record);
	}
	if (g_str_equal(table->object, "note")) return "interaction";
	if (g_str_equal(table->object, "task")) return "activity";
	return table->object;
}

static gboolean
validate_custom_tables(VentureCrmImportService *self, gint64 org, JsonObject *manifest,
	GPtrArray *tables, GError **error)
{
	JsonNode *node = json_object_get_member(manifest, "custom_fields");
	JsonObject *all;
	g_autoptr(GList) members = NULL;
	GList *l;
	guint t;
	if (node != NULL && !JSON_NODE_HOLDS_OBJECT(node))
		return refuse(error, "custom_fields must map export modules to definition arrays");
	all = node != NULL ? json_node_get_object(node) : NULL;
	members = all != NULL ? json_object_get_members(all) : NULL;
	if (members != NULL && !venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "accounting_custom_field"))
		return refuse(error, "custom_fields module must be enabled to import field definitions and values");
	for (t = 0; t < tables->len; t++)
	{
		Table *table = g_ptr_array_index(tables, t);
		const gchar *type = table_record_type(table);
		if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), type))
			return refuse(error, "%s needs the module containing %s to be enabled", table->file, type);
	}
	for (l = members; l != NULL; l = l->next)
	{
		JsonNode *definitions_node = json_object_get_member(all, l->data);
		JsonArray *definitions;
		Table *table = NULL;
		guint d;
		for (t = 0; t < tables->len; t++)
		{
			Table *candidate = g_ptr_array_index(tables, t);
			if (g_strcmp0(candidate->file, l->data) == 0) table = candidate;
		}
		if (table == NULL || !JSON_NODE_HOLDS_ARRAY(definitions_node))
			return refuse(error, "custom_fields.%s needs a corresponding export and an array", (const gchar *)l->data);
		if (g_str_equal(table->object, "user"))
			return refuse(error, "exported users are mappings; migration cannot alter login custom fields");
		definitions = json_node_get_array(definitions_node);
		for (d = 0; d < json_array_get_length(definitions); d++)
		{
			JsonNode *definition_node = json_array_get_element(definitions, d);
			JsonObject *definition;
			const gchar *header, *name, *kind;
			g_autoptr(VentureAccountingCustomField) field = venture_accounting_custom_field_new();
			g_autofree gchar *options = NULL;
			gchar **headers = g_ptr_array_index(table->rows, 0);
			guint h;
			gboolean found = FALSE;
			if (!JSON_NODE_HOLDS_OBJECT(definition_node))
				return refuse(error, "custom field definitions must be objects");
			definition = json_node_get_object(definition_node);
			header = obj_str(definition, "header");
			name = obj_str(definition, "name");
			kind = obj_str(definition, "kind");
			if (blank(header) || blank(name) || blank(kind))
				return refuse(error, "custom field definitions need name, header and kind");
			for (h = 0; headers[h] != NULL; h++)
				if (g_ascii_strcasecmp(headers[h], header) == 0) found = TRUE;
			if (!found) return refuse(error, "%s: custom field %s needs column %s", table->file, name, header);
			node = json_object_get_member(definition, "options");
			options = node != NULL ? venture_json_to_string(node, FALSE) : g_strdup("[]");
			venture_entity_set_organization_id(VENTURE_ENTITY(field), org);
			g_object_set(field, "record-type", table_record_type(table), "name", name, "kind", kind,
				"required", venture_json_object_get_bool(definition, "required", FALSE), "options", options, NULL);
			if (!venture_custom_fields_validate(self->database, VENTURE_ENTITY(field), error)) return FALSE;
		}
	}
	return TRUE;
}

static gboolean
prepare_custom_definition(Run *run, JsonObject *definition, const gchar *type, GError **error)
{
	const gchar *name = obj_str(definition, "name");
	const gchar *kind = obj_str(definition, "kind");
	const gchar *header = obj_str(definition, "header");
	gboolean required = venture_json_object_get_bool(definition, "required", FALSE);
	g_autofree gchar *options = NULL;
	g_autofree gchar *key = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUSTOM_FIELD);
	g_autoptr(VentureEntity) field = NULL;
	JsonNode *node = json_object_get_member(definition, "options");
	if (blank(name) || blank(header) || blank(kind))
		return refuse(error, "custom field definitions need name, header and kind");
	options = node != NULL ? venture_json_to_string(node, FALSE) : g_strdup("[]");
	key = g_strdup_printf("%s:%s", type, name);
	venture_query_set_organization(query, run->org);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, "field-key", VENTURE_FILTER_OP_EQ, key, error)) return FALSE;
	field = venture_database_find_one(run->self->database, query, error);
	if (field != NULL)
	{
		g_autofree gchar *old_kind = string_of(field, "kind");
		g_autofree gchar *old_options = string_of(field, "options");
		gboolean old_required = FALSE;
		g_object_get(field, "required", &old_required, NULL);
		if (g_strcmp0(old_kind, kind) != 0 || g_strcmp0(old_options, options) != 0 || old_required != required)
			return refuse(error, "custom field %s conflicts with an existing definition", key);
		if (venture_entity_is_deleted(field))
			return venture_database_restore(run->self->database, field, run->actor, error) &&
				track_auxiliary(run, "custom_field", key, field, error);
		return TRUE;
	}
	if (error != NULL && *error != NULL) return FALSE;
	field = venture_custom_fields_service_define(venture_custom_fields_service_get(run->self->database),
		run->org, type, name, kind, required, options, run->actor, error);
	return field != NULL && track_auxiliary(run, "custom_field", key, field, error);
}

static gboolean
prepare_custom_fields(Run *run, GPtrArray *tables, GError **error)
{
	guint t, d;
	for (t = 0; t < tables->len; t++)
	{
		Table *table = g_ptr_array_index(tables, t);
		JsonArray *definitions = custom_definitions(run->manifest, table->file);
		for (d = 0; definitions != NULL && d < json_array_get_length(definitions); d++)
		{
			JsonNode *node = json_array_get_element(definitions, d);
			JsonObject *definition;
			if (!JSON_NODE_HOLDS_OBJECT(node))
				return refuse(error, "custom_fields.%s must contain definitions", table->file);
			definition = json_node_get_object(node);
			if (!prepare_custom_definition(run, definition, table_record_type(table), error))
				return FALSE;
			if (g_str_equal(table->object, "task") &&
				!prepare_custom_definition(run, definition, "interaction", error))
				return FALSE;
		}
	}
	return TRUE;
}

static gboolean
apply_custom_fields(Run *run, Table *table, gchar **row, VentureEntity *record, GError **error)
{
	JsonArray *definitions = custom_definitions(run->manifest, table->file);
	gchar **headers = g_ptr_array_index(table->rows, 0);
	guint d;
	for (d = 0; definitions != NULL && d < json_array_get_length(definitions); d++)
	{
		JsonObject *definition = json_array_get_object_element(definitions, d);
		const gchar *header = obj_str(definition, "header");
		const gchar *name = obj_str(definition, "name");
		guint h;
		gboolean found = FALSE;
		for (h = 0; headers[h] != NULL && row[h] != NULL; h++)
		{
			if (g_ascii_strcasecmp(headers[h], header) == 0)
			{
				venture_entity_set_attribute(record, name, row[h]);
				found = TRUE;
				break;
			}
		}
		if (!found)
			return refuse(error, "%s: custom field %s needs column %s", table->file, name, header);
	}
	return TRUE;
}

/* The custom-value index belongs to its record. Retiring an imported
 * record must retire its index rows too; retained stage evidence keeps
 * its own attributes and index. The enclosing rollback is transactional. */
static gboolean
remove_custom_values(VentureCrmImportService *self, VentureEntity *record,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) values = NULL;
	guint i;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "custom_field_value"))
		return TRUE;
	query = venture_query_new(VENTURE_TYPE_CUSTOM_FIELD_VALUE);
	venture_query_set_organization(query, venture_entity_get_organization_id(record));
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_string(query, "record-type", VENTURE_FILTER_OP_EQ, venture_entity_get_entity_name(record), error) ||
		!venture_query_add_filter_int(query, "record-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), error))
		return FALSE;
	values = venture_database_find(self->database, query, error);
	if (values == NULL) return FALSE;
	for (i = 0; i < values->len; i++)
		if (!venture_database_delete(self->database, g_ptr_array_index(values, i), actor, error))
			return FALSE;
	return TRUE;
}
