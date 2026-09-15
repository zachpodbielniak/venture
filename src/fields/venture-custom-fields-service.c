/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureCustomFieldsService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureCustomFieldsService, venture_custom_fields_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureCustomFieldsService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_CUSTOM_FIELDS_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureCustomFieldsService *self = VENTURE_CUSTOM_FIELDS_SERVICE(object);
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
	VentureCustomFieldsService *self = VENTURE_CUSTOM_FIELDS_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_custom_fields_service_parent_class)->finalize(object);
}

static void
venture_custom_fields_service_class_init(VentureCustomFieldsServiceClass *klass)
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
venture_custom_fields_service_init(VentureCustomFieldsService *self)
{
	(void)self;
}

VentureCustomFieldsService *
venture_custom_fields_service_get(VentureDatabase *database)
{
	VentureCustomFieldsService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-custom-fields-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CUSTOM_FIELDS_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-custom-fields-service", self, g_object_unref);
	}
	return self;
}

static gboolean
save_owned(VentureCustomFieldsService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static gboolean
known_kind(const gchar *kind)
{
	return g_strcmp0(kind, "string") == 0 || g_strcmp0(kind, "text") == 0 ||
		g_strcmp0(kind, "integer") == 0 || g_strcmp0(kind, "boolean") == 0 ||
		g_strcmp0(kind, "enum") == 0 || g_strcmp0(kind, "money") == 0 ||
		g_strcmp0(kind, "date") == 0 || g_strcmp0(kind, "datetime") == 0;
}

static gboolean
own_type(VentureEntity *record)
{
	return VENTURE_IS_ACCOUNTING_CUSTOM_FIELD(record) || VENTURE_IS_ACCOUNTING_LAYOUT(record) ||
		VENTURE_IS_CUSTOM_FIELD_VALUE(record);
}

static gchar *
field_key(const gchar *record_type, const gchar *name)
{
	return g_strdup_printf("%s:%s", record_type, name);
}

static gchar *
value_key(const gchar *record_type, gint64 record_id, const gchar *name)
{
	return g_strdup_printf("%s:%" G_GINT64_FORMAT ":%s", record_type, record_id, name);
}

static GPtrArray *
fields_for_type(VentureDatabase *database, gint64 organization_id, const gchar *record_type, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_custom_field") == G_TYPE_INVALID)
		return g_ptr_array_new_with_free_func(g_object_unref);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUSTOM_FIELD);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "record-type", VENTURE_FILTER_OP_EQ, record_type, NULL);
	return venture_database_find(database, query, error);
}

static gchar *
stored_value(VentureDatabase *database, gint64 organization_id, const gchar *record_type,
	gint64 record_id, const gchar *name, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *key = NULL;
	gchar *value = NULL;
	if (record_id <= 0)
		return NULL;
	key = value_key(record_type, record_id, name);
	query = venture_query_new(VENTURE_TYPE_CUSTOM_FIELD_VALUE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "value-key", VENTURE_FILTER_OP_EQ, key, NULL);
	row = venture_database_find_one(database, query, error);
	if (row == NULL)
	{
		g_clear_error(error);
		return NULL;
	}
	g_object_get(row, "value", &value, NULL);
	return value;
}

static gboolean
value_present(const gchar *text)
{
	return text != NULL && text[0] != '\0';
}

static gboolean
prepare_definition(VentureEntity *record, GError **error)
{
	g_autofree gchar *record_type = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *key = NULL;
	g_object_get(record, "record-type", &record_type, "name", &name, "kind", &kind, NULL);
	if (record_type == NULL || record_type[0] == '\0' || name == NULL || name[0] == '\0')
		return refuse(error, "record-type and name are required");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), record_type) == G_TYPE_INVALID)
		return refuse(error, "record-type is not a registered entity");
	if (!known_kind(kind))
		return refuse(error, "kind must be string, text, integer, boolean, enum, money, date or datetime");
	key = field_key(record_type, name);
	g_object_set(record, "field-key", key, NULL);
	return TRUE;
}

static gboolean
prepare_layout(VentureEntity *record, GError **error)
{
	g_autofree gchar *record_type = NULL;
	g_autofree gchar *order = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_object_get(record, "record-type", &record_type, "field-order", &order, NULL);
	if (record_type == NULL || record_type[0] == '\0')
		return refuse(error, "record-type is required");
	if (order == NULL || order[0] == '\0')
		return refuse(error, "field-order JSON is required");
	node = json_from_string(order, error);
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
	{
		g_clear_error(error);
		return refuse(error, "field-order must be a JSON array of field names");
	}
	g_object_set(record, "layout-key", record_type, NULL);
	return TRUE;
}

static gboolean
prepare_value(VentureEntity *record, GError **error)
{
	g_autofree gchar *record_type = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *key = NULL;
	gint64 record_id = 0;
	g_object_get(record, "record-type", &record_type, "name", &name, "record-id", &record_id, NULL);
	if (record_type == NULL || name == NULL || record_id <= 0)
		return refuse(error, "record-type, name and record-id are required");
	key = value_key(record_type, record_id, name);
	g_object_set(record, "value-key", key, NULL);
	return TRUE;
}

static gboolean
check_kind(const gchar *kind, const gchar *options, const gchar *text, const gchar *name, GError **error)
{
	if (g_strcmp0(kind, "integer") == 0)
	{
		gchar *end = NULL;
		g_ascii_strtoll(text, &end, 10);
		if (end == NULL || end == text || *end != '\0')
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCustomFieldsService: %s must be an integer", name);
			return FALSE;
		}
	}
	else if (g_strcmp0(kind, "boolean") == 0)
	{
		if (g_strcmp0(text, "true") != 0 && g_strcmp0(text, "false") != 0 &&
			g_strcmp0(text, "1") != 0 && g_strcmp0(text, "0") != 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCustomFieldsService: %s must be true or false", name);
			return FALSE;
		}
	}
	else if (g_strcmp0(kind, "enum") == 0)
	{
		g_autoptr(JsonNode) node = json_from_string(options ? options : "[]", NULL);
		JsonArray *array;
		gboolean ok = FALSE;
		guint i;
		if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCustomFieldsService: %s has invalid enum options", name);
			return FALSE;
		}
		array = json_node_get_array(node);
		for (i = 0; i < json_array_get_length(array); i++)
		{
			if (g_strcmp0(json_array_get_string_element(array, i), text) == 0)
				ok = TRUE;
		}
		if (!ok)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCustomFieldsService: %s must be one of the declared options", name);
			return FALSE;
		}
	}
	else if (g_strcmp0(kind, "money") == 0)
	{
		g_autoptr(GError) inner = NULL;
		g_autoptr(VentureMoney) money = venture_money_from_string(text, NULL, &inner);
		if (money == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCustomFieldsService: %s must be money", name);
			return FALSE;
		}
	}
	else if (g_strcmp0(kind, "date") == 0 || g_strcmp0(kind, "datetime") == 0)
	{
		g_autoptr(GDateTime) when = g_date_time_new_from_iso8601(text, NULL);
		if (when == NULL && strlen(text) == 10)
		{
			g_autofree gchar *iso = g_strconcat(text, "T00:00:00Z", NULL);
			when = g_date_time_new_from_iso8601(iso, NULL);
		}
		if (when == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCustomFieldsService: %s must be a date", name);
			return FALSE;
		}
	}
	return TRUE;
}

static VentureFieldKind
kind_to_spec(const gchar *kind)
{
	if (g_strcmp0(kind, "text") == 0) return VENTURE_FIELD_KIND_TEXT;
	if (g_strcmp0(kind, "integer") == 0) return VENTURE_FIELD_KIND_INTEGER;
	if (g_strcmp0(kind, "boolean") == 0) return VENTURE_FIELD_KIND_BOOLEAN;
	if (g_strcmp0(kind, "enum") == 0) return VENTURE_FIELD_KIND_ENUM;
	if (g_strcmp0(kind, "money") == 0) return VENTURE_FIELD_KIND_MONEY;
	if (g_strcmp0(kind, "date") == 0) return VENTURE_FIELD_KIND_DATE;
	if (g_strcmp0(kind, "datetime") == 0) return VENTURE_FIELD_KIND_DATETIME;
	return VENTURE_FIELD_KIND_STRING;
}

GPtrArray *
venture_custom_fields_form_specs(VentureDatabase *database, gint64 organization_id,
	const gchar *record_type, VentureEntity *record, GError **error)
{
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(VentureQuery) layout_query = NULL;
	g_autoptr(VentureEntity) layout = NULL;
	GPtrArray *specs;
	GHashTable *by_name;
	guint i;
	if (database == NULL || record_type == NULL)
		return g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_custom_field") == G_TYPE_INVALID)
		return g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	fields = fields_for_type(database, organization_id, record_type, error);
	if (fields == NULL)
		return NULL;
	by_name = g_hash_table_new(g_str_hash, g_str_equal);
	specs = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *kind = NULL;
		g_autofree gchar *options = NULL;
		gboolean required = FALSE;
		VentureFieldSpec *spec;
		g_object_get(field, "name", &name, "kind", &kind, "options", &options, "required", &required, NULL);
		spec = venture_field_spec_new(name, NULL, kind_to_spec(kind));
		spec->required = required;
		if (g_strcmp0(kind, "enum") == 0 && options != NULL)
		{
			g_autoptr(JsonNode) node = json_from_string(options, NULL);
			if (node != NULL && JSON_NODE_HOLDS_ARRAY(node))
			{
				JsonArray *array = json_node_get_array(node);
				GPtrArray *choices = g_ptr_array_new();
				guint j;
				for (j = 0; j < json_array_get_length(array); j++)
					g_ptr_array_add(choices, g_strdup(json_array_get_string_element(array, j)));
				g_ptr_array_add(choices, NULL);
				spec->choices = (gchar **)g_ptr_array_free(choices, FALSE);
			}
		}
		if (record != NULL)
		{
			g_autofree gchar *stored = stored_value(database, organization_id, record_type,
				venture_entity_get_id(record), name, NULL);
			const gchar *attribute = venture_entity_get_attribute(record, name);
			if (!value_present(attribute) && value_present(stored))
				venture_entity_set_attribute(record, name, stored);
		}
		g_hash_table_insert(by_name, spec->name, spec);
		g_ptr_array_add(specs, spec);
	}
	layout_query = venture_query_new(VENTURE_TYPE_ACCOUNTING_LAYOUT);
	venture_query_set_organization(layout_query, organization_id);
	venture_query_add_filter_string(layout_query, "layout-key", VENTURE_FILTER_OP_EQ, record_type, NULL);
	layout = venture_database_find_one(database, layout_query, NULL);
	if (layout != NULL)
	{
		g_autofree gchar *order = NULL;
		g_autoptr(JsonNode) node = NULL;
		g_object_get(layout, "field-order", &order, NULL);
		node = json_from_string(order, NULL);
		if (node != NULL && JSON_NODE_HOLDS_ARRAY(node))
		{
			JsonArray *array = json_node_get_array(node);
			GPtrArray *ordered = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
			GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
			guint j;
			g_ptr_array_set_free_func(specs, NULL);
			for (j = 0; j < json_array_get_length(array); j++)
			{
				const gchar *want = json_array_get_string_element(array, j);
				VentureFieldSpec *spec = want ? g_hash_table_lookup(by_name, want) : NULL;
				if (spec == NULL || g_hash_table_contains(seen, want))
					continue;
				g_hash_table_add(seen, (gpointer)want);
				g_ptr_array_remove(specs, spec);
				g_ptr_array_add(ordered, spec);
			}
			for (j = 0; j < specs->len; j++)
				g_ptr_array_add(ordered, g_ptr_array_index(specs, j));
			g_ptr_array_unref(specs);
			g_hash_table_unref(seen);
			specs = ordered;
		}
	}
	g_hash_table_unref(by_name);
	return specs;
}

gboolean
venture_custom_fields_validate(VentureDatabase *database, VentureEntity *record, GError **error)
{
	VentureCustomFieldsService *self;
	g_autoptr(GPtrArray) fields = NULL;
	const gchar *entity_name;
	guint i;
	if (record == NULL || database == NULL)
		return TRUE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_custom_field") == G_TYPE_INVALID)
		return TRUE;
	self = venture_custom_fields_service_get(database);
	if (own_type(record))
	{
		if (self->writing == record)
			return TRUE;
		if (VENTURE_IS_ACCOUNTING_CUSTOM_FIELD(record))
			return prepare_definition(record, error);
		if (VENTURE_IS_ACCOUNTING_LAYOUT(record))
			return prepare_layout(record, error);
		return prepare_value(record, error);
	}
	entity_name = venture_entity_get_entity_name(record);
	fields = fields_for_type(database, venture_entity_get_organization_id(record), entity_name, error);
	if (fields == NULL)
		return FALSE;
	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *kind = NULL;
		g_autofree gchar *options = NULL;
		g_autofree gchar *stored = NULL;
		g_autofree gchar *text = NULL;
		gboolean required = FALSE;
		const gchar *attribute;
		g_object_get(field, "name", &name, "kind", &kind, "options", &options, "required", &required, NULL);
		attribute = venture_entity_get_attribute(record, name);
		if (value_present(attribute))
			text = g_strdup(attribute);
		else
			text = stored_value(database, venture_entity_get_organization_id(record),
				entity_name, venture_entity_get_id(record), name, error);
		if (required && !value_present(text))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"VentureCustomFieldsService: required custom field %s", name);
			return FALSE;
		}
		if (!value_present(text))
			continue;
		if (!check_kind(kind, options, text, name, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_custom_fields_sync(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, GError **error)
{
	VentureCustomFieldsService *self;
	g_autoptr(GPtrArray) fields = NULL;
	const gchar *entity_name;
	gint64 id;
	guint i;
	if (record == NULL || database == NULL || own_type(record))
		return TRUE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_custom_field") == G_TYPE_INVALID)
		return TRUE;
	self = venture_custom_fields_service_get(database);
	entity_name = venture_entity_get_entity_name(record);
	id = venture_entity_get_id(record);
	if (id <= 0)
		return TRUE;
	fields = fields_for_type(database, venture_entity_get_organization_id(record), entity_name, error);
	if (fields == NULL)
		return FALSE;
	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *name = NULL;
		const gchar *attribute;
		g_object_get(field, "name", &name, NULL);
		attribute = venture_entity_get_attribute(record, name);
		if (!value_present(attribute))
			continue;
		if (!venture_custom_fields_service_put_value(self, venture_entity_get_organization_id(record),
			entity_name, id, name, attribute, actor, error))
			return FALSE;
	}
	return TRUE;
}

VentureEntity *
venture_custom_fields_service_define(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, const gchar *name, const gchar *kind, gboolean required,
	const gchar *options, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingCustomField) field = NULL;
	g_autofree gchar *key = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_return_val_if_fail(VENTURE_IS_CUSTOM_FIELDS_SERVICE(self), NULL);
	key = field_key(record_type, name);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUSTOM_FIELD);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "field-key", VENTURE_FILTER_OP_EQ, key, NULL);
	existing = venture_database_find_one(self->database, query, NULL);
	field = existing ? VENTURE_ACCOUNTING_CUSTOM_FIELD(g_object_ref(existing)) : venture_accounting_custom_field_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(field), organization_id);
	g_object_set(field, "record-type", record_type, "name", name, "kind", kind ? kind : "string",
		"required", required, "options", options ? options : "[]", "field-key", key, NULL);
	if (!save_owned(self, VENTURE_ENTITY(field), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&field));
}

gboolean
venture_custom_fields_service_put_value(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, gint64 record_id, const gchar *name, const gchar *value,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureCustomFieldValue) row = NULL;
	g_autofree gchar *key = NULL;
	g_return_val_if_fail(VENTURE_IS_CUSTOM_FIELDS_SERVICE(self), FALSE);
	key = value_key(record_type, record_id, name);
	query = venture_query_new(VENTURE_TYPE_CUSTOM_FIELD_VALUE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "value-key", VENTURE_FILTER_OP_EQ, key, NULL);
	existing = venture_database_find_one(self->database, query, NULL);
	row = existing ? VENTURE_CUSTOM_FIELD_VALUE(g_object_ref(existing)) : venture_custom_field_value_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(row), organization_id);
	g_object_set(row, "record-type", record_type, "record-id", record_id, "name", name,
		"value", value ? value : "", "value-key", key, NULL);
	return save_owned(self, VENTURE_ENTITY(row), actor, error);
}

VentureEntity *
venture_custom_fields_service_set_layout(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, const gchar *field_order, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureAccountingLayout) layout = NULL;
	g_return_val_if_fail(VENTURE_IS_CUSTOM_FIELDS_SERVICE(self), NULL);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_LAYOUT);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "layout-key", VENTURE_FILTER_OP_EQ, record_type, NULL);
	existing = venture_database_find_one(self->database, query, NULL);
	layout = existing ? VENTURE_ACCOUNTING_LAYOUT(g_object_ref(existing)) : venture_accounting_layout_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(layout), organization_id);
	g_object_set(layout, "record-type", record_type, "field-order", field_order, "layout-key", record_type, NULL);
	if (!save_owned(self, VENTURE_ENTITY(layout), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&layout));
}
