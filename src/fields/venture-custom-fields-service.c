/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <errno.h>

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

/* Custom names never alias a property, including inherited sensitive fields.
 * Both definitions and rendering check this to tolerate older bad rows. */
static gboolean
custom_name_valid(const gchar *record_type, const gchar *name)
{
	GType type;
	GObjectClass *klass;
	gboolean valid;
	const gchar *p;
	if (record_type == NULL || name == NULL ||
		(!g_ascii_isalpha(*name) && *name != '_'))
		return FALSE;
	for (p = name; *p; p++)
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
			return FALSE;
	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), record_type);
	if (type == G_TYPE_INVALID)
		return FALSE;
	klass = g_type_class_ref(type);
	valid = g_object_class_find_property(klass, name) == NULL;
	g_type_class_unref(klass);
	return valid;
}

static gboolean
string_array_valid(JsonNode *node)
{
	guint i;
	JsonArray *array;
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return FALSE;
	array = json_node_get_array(node);
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *item = json_array_get_element(array, i);
		if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING)
			return FALSE;
	}
	return TRUE;
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
	if (!custom_name_valid(record_type, name))
		return refuse(error, "custom field names must not shadow built-in properties");
	if (g_strcmp0(kind, "enum") == 0)
	{
		g_autofree gchar *options = NULL;
		g_autoptr(JsonNode) node = NULL;
		g_object_get(record, "options", &options, NULL);
		node = json_from_string(options ? options : "[]", NULL);
		if (!string_array_valid(node))
			return refuse(error, "enum options must be an array of strings");
	}
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
	if (!string_array_valid(node))
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
		errno = 0;
		g_ascii_strtoll(text, &end, 10);
		if (errno == ERANGE || end == NULL || end == text || *end != '\0')
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
		if (!string_array_valid(node))
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

void
venture_custom_fields_order_specs(VentureDatabase *database, gint64 organization_id,
	const gchar *record_type, GPtrArray *specs)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) layout = NULL;
	g_autofree gchar *order = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GPtrArray) ordered = g_ptr_array_new();
	g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	JsonArray *names;
	guint i, j;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_layout") == G_TYPE_INVALID)
		return;
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_LAYOUT);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "layout-key", VENTURE_FILTER_OP_EQ, record_type, NULL);
	layout = venture_database_find_one(database, query, NULL);
	if (layout == NULL)
		return;
	g_object_get(layout, "field-order", &order, NULL);
	node = json_from_string(order, NULL);
	if (!string_array_valid(node))
		return;
	names = json_node_get_array(node);
	for (i = 0; i < json_array_get_length(names); i++)
		for (j = 0; j < specs->len; j++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(specs, j);
			if (g_strcmp0(spec->name, json_array_get_string_element(names, i)) == 0 &&
				g_hash_table_add(seen, spec))
				g_ptr_array_add(ordered, spec);
		}
	for (i = 0; i < specs->len; i++)
		if (g_hash_table_add(seen, g_ptr_array_index(specs, i)))
			g_ptr_array_add(ordered, g_ptr_array_index(specs, i));
	/* Only reorder pointers. The caller retains the original element
	 * ownership and unspecified fields retain declaration order. */
	for (i = 0; i < specs->len; i++)
		g_ptr_array_index(specs, i) = g_ptr_array_index(ordered, i);
}

GPtrArray *
venture_custom_fields_form_specs(VentureDatabase *database, gint64 organization_id,
	const gchar *record_type, VentureEntity *record, GError **error)
{
	g_autoptr(GPtrArray) fields = NULL;
	GPtrArray *specs;
	guint i;
	if (database == NULL || record_type == NULL)
		return g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_custom_field") == G_TYPE_INVALID)
		return g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	fields = fields_for_type(database, organization_id, record_type, error);
	if (fields == NULL)
		return NULL;
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
		if (!custom_name_valid(record_type, name))
			continue;
		spec = venture_field_spec_new(name, NULL, kind_to_spec(kind));
		spec->required = required;
		if (g_strcmp0(kind, "enum") == 0 && options != NULL)
		{
			g_autoptr(JsonNode) node = json_from_string(options, NULL);
			if (string_array_valid(node))
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
			if (attribute == NULL && stored != NULL)
				venture_entity_set_attribute(record, name, stored);
		}
		g_ptr_array_add(specs, spec);
	}
	venture_custom_fields_order_specs(database, organization_id, record_type, specs);
	return specs;
}

gboolean
venture_custom_fields_validate(VentureDatabase *database, VentureEntity *record, GError **error)
{
	VentureCustomFieldsService *self;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(VentureEntity) previous = NULL;
	const gchar *entity_name;
	guint i;
	if (record == NULL || database == NULL)
		return TRUE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_custom_field") == G_TYPE_INVALID)
		return TRUE;
	self = venture_custom_fields_service_get(database);
	if (own_type(record))
	{
		if (VENTURE_IS_ACCOUNTING_CUSTOM_FIELD(record))
			return prepare_definition(record, error);
		if (VENTURE_IS_ACCOUNTING_LAYOUT(record))
			return prepare_layout(record, error);
		if (self->writing != record)
			return refuse(error, "custom values are derived; update attributes on the owning record");
		return prepare_value(record, error);
	}
	entity_name = venture_entity_get_entity_name(record);
	fields = fields_for_type(database, venture_entity_get_organization_id(record), entity_name, error);
	if (fields == NULL)
		return FALSE;
	if (fields->len > 0 && venture_entity_is_persisted(record))
	{
		previous = venture_database_get(database, G_OBJECT_TYPE(record), venture_entity_get_id(record), error);
		if (previous == NULL)
			return FALSE;
	}
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
		if (!custom_name_valid(entity_name, name))
			continue;
		if (attribute != NULL)
			text = g_strdup(attribute);
		else if (previous != NULL && venture_entity_get_attribute(previous, name) != NULL)
			text = g_strdup("");
		else
			text = stored_value(database, venture_entity_get_organization_id(record),
				entity_name, venture_entity_get_id(record), name, error);
		if (text != NULL && attribute == NULL)
			venture_entity_set_attribute(record, name, text);
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

static gboolean store_value(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, gint64 record_id, const gchar *name, const gchar *value,
	const VentureActor *actor, GError **error);

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
		if (!custom_name_valid(entity_name, name) || attribute == NULL)
			continue;
		if (!store_value(self, venture_entity_get_organization_id(record),
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

static gboolean
store_value(VentureCustomFieldsService *self, gint64 organization_id,
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

gboolean
venture_custom_fields_service_put_value(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, gint64 record_id, const gchar *name, const gchar *value,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	gboolean found = FALSE;
	guint i;
	GType type;
	g_return_val_if_fail(VENTURE_IS_CUSTOM_FIELDS_SERVICE(self), FALSE);
	if (!custom_name_valid(record_type, name))
		return refuse(error, "invalid custom field name or record type");
	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), record_type);
	record = venture_database_get(self->database, type, record_id, error);
	if (record == NULL)
		return FALSE;
	if (venture_entity_get_organization_id(record) != organization_id)
		return refuse(error, "custom value belongs to another organization");
	fields = fields_for_type(self->database, organization_id, record_type, error);
	if (fields == NULL)
		return FALSE;
	for (i = 0; i < fields->len; i++)
	{
		g_autofree gchar *field_name = NULL;
		g_object_get(g_ptr_array_index(fields, i), "name", &field_name, NULL);
		found |= g_strcmp0(field_name, name) == 0;
	}
	if (!found)
		return refuse(error, "custom field is not defined");
	venture_entity_set_attribute(record, name, value ? value : "");
	return venture_database_save(self->database, record, actor, error);
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
