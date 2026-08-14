/*
 * venture-venture-type.c - Declaratively-defined kinds of venture
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <yaml-glib.h>

#include <string.h>

struct _VentureVentureType
{
	GObject parent_instance;

	gchar		*name;
	gchar		*label;
	gchar		*description;
	GPtrArray	*fields;	/* VentureFieldSpec */
	gchar	       **metrics;
	gchar	       **report_columns;
};

G_DEFINE_FINAL_TYPE(VentureVentureType, venture_venture_type, G_TYPE_OBJECT)

static void
venture_venture_type_finalize(GObject *object)
{
	VentureVentureType *self;

	self = VENTURE_VENTURE_TYPE(object);

	g_clear_pointer(&self->name, g_free);
	g_clear_pointer(&self->label, g_free);
	g_clear_pointer(&self->description, g_free);
	g_clear_pointer(&self->fields, g_ptr_array_unref);
	g_clear_pointer(&self->metrics, g_strfreev);
	g_clear_pointer(&self->report_columns, g_strfreev);

	G_OBJECT_CLASS(venture_venture_type_parent_class)->finalize(object);
}

static void
venture_venture_type_class_init(VentureVentureTypeClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_venture_type_finalize;
}

static void
venture_venture_type_init(VentureVentureType *self)
{
	self->fields = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_field_spec_free);
}

VentureVentureType *
venture_venture_type_new(
	const gchar	*name,
	const gchar	*label
){
	VentureVentureType *self;

	g_return_val_if_fail(NULL != name, NULL);

	self = g_object_new(VENTURE_TYPE_VENTURE_TYPE, NULL);
	self->name = g_strdup(name);
	self->label = g_strdup((NULL != label) ? label : name);

	return self;
}

void
venture_venture_type_add_field(
	VentureVentureType	*self,
	VentureFieldSpec	*spec
){
	g_return_if_fail(VENTURE_IS_VENTURE_TYPE(self));
	g_return_if_fail(NULL != spec);

	g_ptr_array_add(self->fields, spec);
}

/*
 * Reads a list of strings from a JSON member, tolerating both a real array
 * and a single scalar, because a one-element list reads more naturally
 * unbracketed in YAML.
 */
static gchar **
venture_venture_type_read_strv(
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(GPtrArray) values = NULL;
	JsonNode *node;

	if (!json_object_has_member(object, member))
		return NULL;

	node = json_object_get_member(object, member);
	values = g_ptr_array_new();

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array;
		guint i;

		array = json_node_get_array(node);

		for (i = 0; i < json_array_get_length(array); i++)
		{
			const gchar *text;

			text = json_array_get_string_element(array, i);

			if (NULL != text)
				g_ptr_array_add(values, g_strdup(text));
		}
	}
	else if (JSON_NODE_HOLDS_VALUE(node))
	{
		const gchar *text;

		text = json_node_get_string(node);

		if (NULL != text)
			g_ptr_array_add(values, g_strdup(text));
	}

	g_ptr_array_add(values, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&values), FALSE);
}

VentureVentureType *
venture_venture_type_new_from_yaml(
	const gchar	 *yaml,
	GError		**error
){
	g_autoptr(VentureVentureType) self = NULL;
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GError) local_error = NULL;
	YamlNode *yaml_root;
	JsonObject *object;
	const gchar *name;

	g_return_val_if_fail(NULL != yaml, NULL);

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, yaml, -1, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Invalid YAML: %s", local_error->message);
		return NULL;
	}

	yaml_root = yaml_parser_get_root(parser);

	if ((NULL == yaml_root) || yaml_node_is_null(yaml_root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The definition is empty");
		return NULL;
	}

	/* Converting to JSON lets the same field-spec parser serve both a
	 * YAML file and an API request body. */
	root = yaml_node_to_json_node(yaml_root);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "A venture type must be a mapping");
		return NULL;
	}

	object = json_node_get_object(root);
	name = venture_json_object_get_string(object, "name", NULL);

	if (venture_string_is_empty(name))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "A venture type needs a \"name\"");
		return NULL;
	}

	self = venture_venture_type_new(name,
		venture_json_object_get_string(object, "label", NULL));
	self->description = g_strdup(
		venture_json_object_get_string(object, "description", NULL));

	if (json_object_has_member(object, "fields"))
	{
		JsonNode *fields_node;
		JsonObject *fields;
		g_autoptr(GList) members = NULL;
		GList *iter;

		fields_node = json_object_get_member(object, "fields");

		if (!JSON_NODE_HOLDS_OBJECT(fields_node))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			            "The \"fields\" of %s must be a mapping of "
			            "name to definition", name);
			return NULL;
		}

		fields = json_node_get_object(fields_node);
		members = json_object_get_members(fields);

		for (iter = members; NULL != iter; iter = iter->next)
		{
			VentureFieldSpec *spec;

			spec = venture_field_spec_new_from_json(iter->data,
				json_object_get_member(fields, iter->data), error);

			if (NULL == spec)
			{
				g_prefix_error(error, "%s: ", name);
				return NULL;
			}

			venture_venture_type_add_field(self, spec);
		}

		g_ptr_array_sort_values(self->fields,
		                        venture_field_spec_compare_display_order);
	}

	self->metrics = venture_venture_type_read_strv(object, "metrics");
	self->report_columns = venture_venture_type_read_strv(object,
	                                                      "report_columns");

	return g_steal_pointer(&self);
}

VentureVentureType *
venture_venture_type_new_from_file(
	const gchar	 *path,
	GError		**error
){
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) local_error = NULL;
	VentureVentureType *type;

	g_return_val_if_fail(NULL != path, NULL);

	if (!g_file_get_contents(path, &contents, NULL, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Cannot read %s: %s", path, local_error->message);
		return NULL;
	}

	type = venture_venture_type_new_from_yaml(contents, error);

	if (NULL == type)
		g_prefix_error(error, "%s: ", path);

	return type;
}

const gchar *
venture_venture_type_get_name(VentureVentureType *self)
{
	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE(self), NULL);

	return self->name;
}

const gchar *
venture_venture_type_get_label(VentureVentureType *self)
{
	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE(self), NULL);

	return self->label;
}

const gchar *
venture_venture_type_get_description(VentureVentureType *self)
{
	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE(self), NULL);

	return self->description;
}

GPtrArray *
venture_venture_type_get_fields(VentureVentureType *self)
{
	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE(self), NULL);

	return self->fields;
}

const gchar * const *
venture_venture_type_get_metrics(VentureVentureType *self)
{
	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE(self), NULL);

	return (const gchar * const *)self->metrics;
}

gboolean
venture_venture_type_validate_venture(
	VentureVentureType	 *self,
	VentureEntity		 *venture,
	GError			**error
){
	guint i;

	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_ENTITY(venture), FALSE);

	for (i = 0; i < self->fields->len; i++)
	{
		VentureFieldSpec *spec;
		g_auto(GValue) value = G_VALUE_INIT;
		const gchar *stored;

		spec = g_ptr_array_index(self->fields, i);

		/*
		 * A declaratively-defined field lives in the venture's
		 * attribute bag rather than in a column, because a type can
		 * appear and disappear at run time and a column cannot. It is
		 * still validated against its declaration, which is what makes
		 * "required" and "choices" mean something.
		 */
		stored = venture_entity_get_attribute(venture,
			venture_field_spec_get_name(spec));

		g_value_init(&value, G_TYPE_STRING);
		g_value_set_string(&value, stored);

		if (!venture_field_spec_validate(spec, &value, error))
			return FALSE;
	}

	return TRUE;
}

JsonNode *
venture_venture_type_to_json(VentureVentureType *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;
	gsize k;

	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE(self), NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, self->name);

	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, self->label);

	if (NULL != self->description)
	{
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder, self->description);
	}

	json_builder_set_member_name(builder, "fields");
	json_builder_begin_array(builder);

	for (i = 0; i < self->fields->len; i++)
	{
		json_builder_add_value(builder,
			venture_field_spec_to_json(g_ptr_array_index(self->fields, i)));
	}

	json_builder_end_array(builder);

	if (NULL != self->metrics)
	{
		json_builder_set_member_name(builder, "metrics");
		json_builder_begin_array(builder);

		for (k = 0; NULL != self->metrics[k]; k++)
			json_builder_add_string_value(builder, self->metrics[k]);

		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Registry
 * ========================================================================== */

struct _VentureVentureTypeRegistry
{
	GObject parent_instance;

	GHashTable *types;
};

G_DEFINE_FINAL_TYPE(VentureVentureTypeRegistry, venture_venture_type_registry,
                    G_TYPE_OBJECT)

static void
venture_venture_type_registry_finalize(GObject *object)
{
	VentureVentureTypeRegistry *self;

	self = VENTURE_VENTURE_TYPE_REGISTRY(object);

	g_clear_pointer(&self->types, g_hash_table_unref);

	G_OBJECT_CLASS(venture_venture_type_registry_parent_class)->finalize(object);
}

static void
venture_venture_type_registry_class_init(VentureVentureTypeRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_venture_type_registry_finalize;
}

static void
venture_venture_type_registry_init(VentureVentureTypeRegistry *self)
{
	self->types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_object_unref);
}

VentureVentureTypeRegistry *
venture_venture_type_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_VENTURE_TYPE_REGISTRY, NULL);
}

void
venture_venture_type_registry_add(
	VentureVentureTypeRegistry	*self,
	VentureVentureType		*type
){
	g_return_if_fail(VENTURE_IS_VENTURE_TYPE_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_VENTURE_TYPE(type));

	/* Replacing rather than refusing: reloading a definition after an
	 * edit is the normal case, not a conflict. */
	g_hash_table_insert(self->types,
	                    g_strdup(venture_venture_type_get_name(type)), type);
}

VentureVentureType *
venture_venture_type_registry_lookup(
	VentureVentureTypeRegistry	*self,
	const gchar			*name
){
	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE_REGISTRY(self), NULL);

	if (NULL == name)
		return NULL;

	return g_hash_table_lookup(self->types, name);
}

static gint
venture_venture_type_compare(
	gconstpointer	a,
	gconstpointer	b
){
	VentureVentureType * const *type_a = a;
	VentureVentureType * const *type_b = b;

	return g_strcmp0(venture_venture_type_get_name(*type_a),
	                 venture_venture_type_get_name(*type_b));
}

GPtrArray *
venture_venture_type_registry_list(VentureVentureTypeRegistry *self)
{
	GPtrArray *types;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE_REGISTRY(self), NULL);

	types = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->types);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(types, value);

	g_ptr_array_sort(types, venture_venture_type_compare);

	return types;
}

guint
venture_venture_type_registry_load_directory(
	VentureVentureTypeRegistry	 *self,
	const gchar			 *path,
	GError				**error
){
	g_autoptr(GDir) directory = NULL;
	g_autoptr(GError) local_error = NULL;
	const gchar *entry;
	guint loaded;

	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE_REGISTRY(self), 0);
	g_return_val_if_fail(NULL != path, 0);

	directory = g_dir_open(path, 0, &local_error);

	if (NULL == directory)
	{
		/* A configured directory that does not exist is not an error:
		 * an install may simply have no custom types yet. */
		g_debug("venture_venture_type: %s", local_error->message);
		return 0;
	}

	loaded = 0;

	while (NULL != (entry = g_dir_read_name(directory)))
	{
		g_autofree gchar *full_path = NULL;
		g_autoptr(VentureVentureType) type = NULL;
		g_autoptr(GError) parse_error = NULL;

		if (!g_str_has_suffix(entry, ".yaml") &&
		    !g_str_has_suffix(entry, ".yml"))
			continue;

		full_path = g_build_filename(path, entry, NULL);
		type = venture_venture_type_new_from_file(full_path, &parse_error);

		if (NULL == type)
		{
			/* One bad definition must not cost the operator the
			 * others, so it is reported and skipped. */
			g_warning("Skipping venture type: %s", parse_error->message);
			continue;
		}

		venture_venture_type_registry_add(self, g_steal_pointer(&type));
		loaded++;
	}

	return loaded;
}

JsonNode *
venture_venture_type_registry_to_json(VentureVentureTypeRegistry *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) types = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_VENTURE_TYPE_REGISTRY(self), NULL);

	types = venture_venture_type_registry_list(self);
	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < types->len; i++)
	{
		json_builder_add_value(builder,
			venture_venture_type_to_json(g_ptr_array_index(types, i)));
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
