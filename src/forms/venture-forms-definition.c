/*
 * venture-forms-definition.c - A form's questions, drafted or frozen
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A definition is a form's questions in the order they are asked. While a
 * form is edited it is read from the form_field records; a published
 * version stores it as JSON text and is read from that forever after. The
 * two readings produce the same structure, so the renderer and the
 * validator never know which they were given -- which is the point: what
 * a published version asks cannot drift because someone edited a draft.
 */

#include "venture-forms-private.h"

#include <math.h>
#include <string.h>

/* ==========================================================================
 * Reading records
 * ========================================================================== */

gchar *
venture_forms_get_string(VentureEntity *entity, const gchar *property)
{
	gchar *value = NULL;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);
	return value;
}

gint64
venture_forms_get_int(VentureEntity *entity, const gchar *property)
{
	gint64 value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);
	return value;
}

gdouble
venture_forms_get_double(VentureEntity *entity, const gchar *property)
{
	gdouble value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);
	return value;
}

gboolean
venture_forms_get_bool(VentureEntity *entity, const gchar *property)
{
	gboolean value = FALSE;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);
	return value;
}

/* ==========================================================================
 * Kinds and choices
 * ========================================================================== */

/* The kind's public name, "short_text"; the class uses dashes. */
const gchar *
venture_forms_kind_nick(VentureFormFieldKind kind)
{
	GEnumClass *klass;
	GEnumValue *value;

	klass = g_type_class_peek(VENTURE_TYPE_FORM_FIELD_KIND);
	if (NULL == klass)
		klass = g_type_class_ref(VENTURE_TYPE_FORM_FIELD_KIND);
	value = g_enum_get_value(klass, (gint)kind);
	return (NULL != value) ? value->value_nick : "short_text";
}

static gboolean
forms_kind_from_nick(const gchar *nick, VentureFormFieldKind *kind)
{
	GEnumClass *klass;
	GEnumValue *value;

	klass = g_type_class_peek(VENTURE_TYPE_FORM_FIELD_KIND);
	if (NULL == klass)
		klass = g_type_class_ref(VENTURE_TYPE_FORM_FIELD_KIND);
	value = g_enum_get_value_by_nick(klass, nick);
	if (NULL == value)
		return FALSE;
	*kind = (VentureFormFieldKind)value->value;
	return TRUE;
}

gboolean
venture_forms_kind_has_choices(VentureFormFieldKind kind)
{
	return (VENTURE_FORM_FIELD_SINGLE_CHOICE == kind) ||
	       (VENTURE_FORM_FIELD_MULTIPLE_CHOICE == kind);
}

void
venture_forms_choice_free(gpointer data)
{
	VentureFormsChoice *choice = data;

	g_free(choice->id);
	g_free(choice->label);
	g_free(choice);
}

/* A choice id: what a label becomes when it has no id of its own. */
static gchar *
forms_choice_slug(const gchar *label)
{
	GString *id;
	const gchar *cursor;
	gboolean gap = FALSE;

	id = g_string_new(NULL);
	for (cursor = label; *cursor != '\0'; cursor++)
	{
		if (g_ascii_isalnum(*cursor))
		{
			if (gap && id->len > 0)
				g_string_append_c(id, '_');
			g_string_append_c(id, g_ascii_tolower(*cursor));
			gap = FALSE;
		}
		else
			gap = TRUE;

		if (id->len >= VENTURE_FORMS_KEY_MAX - 4)
			break;
	}
	if (0 == id->len || !g_ascii_isalpha(id->str[0]))
		g_string_prepend(id, "choice_");
	return g_string_free(id, FALSE);
}

gboolean
venture_forms_choice_id_valid(const gchar *id)
{
	const gchar *cursor;

	if (venture_string_is_empty(id) || strlen(id) > VENTURE_FORMS_KEY_MAX)
		return FALSE;
	for (cursor = id; *cursor != '\0'; cursor++)
		if (!(g_ascii_islower(*cursor) || g_ascii_isdigit(*cursor) ||
		      '_' == *cursor || '-' == *cursor))
			return FALSE;
	return TRUE;
}

/*
 * Parses "id | Label" lines into choices. With @assign, a line with no id
 * gets one made from its label, numbered past the ids before it -- the
 * save validator uses this, and writes the result back so the id is fixed
 * from then on. Without, such a line takes the made id unnumbered; only
 * stored lists, which always carry ids, are read that way.
 */
GPtrArray *
venture_forms_choices_parse(const gchar *text, gboolean assign, GError **error)
{
	g_autoptr(GPtrArray) choices = NULL;
	g_auto(GStrv) lines = NULL;
	guint i;

	choices = g_ptr_array_new_with_free_func(venture_forms_choice_free);
	if (venture_string_is_empty(text))
		return g_steal_pointer(&choices);

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; NULL != lines[i]; i++)
	{
		g_autofree gchar *line = g_strstrip(g_strdup(lines[i]));
		VentureFormsChoice *choice;
		gchar *bar;
		guint j;

		if ('\0' == line[0])
			continue;

		choice = g_new0(VentureFormsChoice, 1);
		bar = strchr(line, '|');
		if (NULL != bar)
		{
			*bar = '\0';
			choice->id = g_strstrip(g_strdup(line));
			choice->label = g_strstrip(g_strdup(bar + 1));
		}
		else
		{
			choice->label = g_strdup(line);
			choice->id = forms_choice_slug(line);
			if (assign)
			{
				g_autofree gchar *base = g_strdup(choice->id);
				guint n = 2;

				for (j = 0; j < choices->len; j++)
				{
					VentureFormsChoice *before = g_ptr_array_index(choices, j);

					if (0 == g_strcmp0(before->id, choice->id))
					{
						g_free(choice->id);
						choice->id = g_strdup_printf("%s_%u", base, n++);
						j = (guint)-1;
					}
				}
			}
		}
		g_ptr_array_add(choices, choice);

		if (!venture_forms_choice_id_valid(choice->id))
		{
			venture_set_error_validation(error, "Choices",
				"\"%s\" is not a usable id: lowercase letters, digits, _ and - only",
				choice->id);
			return NULL;
		}
		if ('\0' == choice->label[0])
		{
			venture_set_error_validation(error, "Choices",
				"the choice \"%s\" has no label", choice->id);
			return NULL;
		}
		for (j = 0; j + 1 < choices->len; j++)
		{
			VentureFormsChoice *before = g_ptr_array_index(choices, j);

			if (0 == g_strcmp0(before->id, choice->id))
			{
				venture_set_error_validation(error, "Choices",
					"the id \"%s\" is used twice", choice->id);
				return NULL;
			}
		}
	}
	return g_steal_pointer(&choices);
}

const gchar *
venture_forms_choice_label(GPtrArray *choices, const gchar *id)
{
	guint i;

	for (i = 0; NULL != choices && i < choices->len; i++)
	{
		VentureFormsChoice *choice = g_ptr_array_index(choices, i);

		if (0 == g_strcmp0(choice->id, id))
			return choice->label;
	}
	return NULL;
}

/* The scale a rating question offers: its bounds, or 1 to 5. */
void
venture_forms_rating_bounds(const VentureFormsField *field, gint64 *low, gint64 *high)
{
	if (0 == field->min_value && 0 == field->max_value)
	{
		*low = 1;
		*high = 5;
		return;
	}
	*low = (gint64)ceil(field->min_value);
	*high = (gint64)floor(field->max_value);
}

/* ==========================================================================
 * Definitions
 * ========================================================================== */

void
venture_forms_field_free(gpointer data)
{
	VentureFormsField *field = data;

	g_free(field->key);
	g_free(field->label);
	g_free(field->help);
	g_free(field->placeholder);
	g_free(field->pattern);
	g_free(field->default_value);
	g_free(field->maps_to);
	g_free(field->autocomplete);
	g_clear_pointer(&field->choices, g_ptr_array_unref);
	g_free(field);
}

static gint
forms_record_order(gconstpointer a, gconstpointer b)
{
	VentureEntity *left = *(VentureEntity *const *)a;
	VentureEntity *right = *(VentureEntity *const *)b;
	gint64 lp = venture_forms_get_int(left, "position");
	gint64 rp = venture_forms_get_int(right, "position");

	if (lp != rp)
		return lp < rp ? -1 : 1;
	return venture_entity_get_id(left) < venture_entity_get_id(right) ? -1 :
	       venture_entity_get_id(left) > venture_entity_get_id(right) ? 1 : 0;
}

GPtrArray *
venture_forms_fields(VentureDatabase *database, VentureEntity *form, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	GPtrArray *rows;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	query = venture_query_new(VENTURE_TYPE_FORM_FIELD);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_set_limit(query, 500);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ,
	                             venture_entity_get_id(form), NULL);
	rows = venture_database_find(database, query, error);
	if (NULL != rows)
		g_ptr_array_sort(rows, forms_record_order);
	return rows;
}

GPtrArray *
venture_forms_definition_from_records(VentureDatabase *database, VentureEntity *form, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	GPtrArray *fields;
	guint i;

	rows = venture_forms_fields(database, form, error);
	if (NULL == rows)
		return NULL;
	fields = g_ptr_array_new_with_free_func(venture_forms_field_free);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		VentureFormsField *field = g_new0(VentureFormsField, 1);
		g_autofree gchar *choices = venture_forms_get_string(row, "choices");

		g_object_get(row, "key", &field->key, "label", &field->label, "kind", &field->kind,
		             "required", &field->required, "sensitive", &field->sensitive,
		             "marketing-consent", &field->marketing_consent,
		             "position", &field->position,
		             "help", &field->help, "placeholder", &field->placeholder,
		             "pattern", &field->pattern, "default-value", &field->default_value,
		             "maps-to", &field->maps_to, "autocomplete", &field->autocomplete,
		             "min-value", &field->min_value,
		             "max-value", &field->max_value, "min-length", &field->min_length,
		             "max-length", &field->max_length, NULL);
		field->choices = venture_forms_choices_parse(choices, FALSE, NULL);
		if (NULL == field->choices)
			field->choices = g_ptr_array_new_with_free_func(venture_forms_choice_free);
		g_ptr_array_add(fields, field);
	}
	return fields;
}

/* Members are written only when they say something, so a definition reads
 * like the questions it froze and not like a column dump. */
gchar *
venture_forms_definition_to_json(GPtrArray *fields)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	guint i, j;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "fields");
	json_builder_begin_array(builder);
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, field->key);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, field->label != NULL ? field->label : "");
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, venture_forms_kind_nick(field->kind));
		json_builder_set_member_name(builder, "required");
		json_builder_add_boolean_value(builder, field->required);
		json_builder_set_member_name(builder, "position");
		json_builder_add_int_value(builder, field->position);
		if (field->marketing_consent)
		{
			json_builder_set_member_name(builder, "marketing_consent");
			json_builder_add_boolean_value(builder, TRUE);
		}
		if (field->sensitive)
		{
			json_builder_set_member_name(builder, "sensitive");
			json_builder_add_boolean_value(builder, TRUE);
		}
#define FORMS_STRING(member, name) \
		if (!venture_string_is_empty(field->member)) { \
			json_builder_set_member_name(builder, name); \
			json_builder_add_string_value(builder, field->member); }
		FORMS_STRING(help, "help")
		FORMS_STRING(placeholder, "placeholder")
		FORMS_STRING(pattern, "pattern")
		FORMS_STRING(default_value, "default_value")
		FORMS_STRING(maps_to, "maps_to")
		FORMS_STRING(autocomplete, "autocomplete")
#undef FORMS_STRING
		if (0 != field->min_value)
		{
			json_builder_set_member_name(builder, "min_value");
			json_builder_add_double_value(builder, field->min_value);
		}
		if (0 != field->max_value)
		{
			json_builder_set_member_name(builder, "max_value");
			json_builder_add_double_value(builder, field->max_value);
		}
		if (0 != field->min_length)
		{
			json_builder_set_member_name(builder, "min_length");
			json_builder_add_int_value(builder, field->min_length);
		}
		if (0 != field->max_length)
		{
			json_builder_set_member_name(builder, "max_length");
			json_builder_add_int_value(builder, field->max_length);
		}
		if (NULL != field->choices && field->choices->len > 0)
		{
			json_builder_set_member_name(builder, "choices");
			json_builder_begin_array(builder);
			for (j = 0; j < field->choices->len; j++)
			{
				VentureFormsChoice *choice = g_ptr_array_index(field->choices, j);

				json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "id");
				json_builder_add_string_value(builder, choice->id);
				json_builder_set_member_name(builder, "label");
				json_builder_add_string_value(builder, choice->label);
				json_builder_end_object(builder);
			}
			json_builder_end_array(builder);
		}
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	return json_to_string(root, FALSE);
}

static gchar *
forms_member_string(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);

	if (NULL == node || !JSON_NODE_HOLDS_VALUE(node) || G_TYPE_STRING != json_node_get_value_type(node))
		return NULL;
	return g_strdup(json_node_get_string(node));
}

static gdouble
forms_member_number(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);

	if (NULL == node || !JSON_NODE_HOLDS_VALUE(node))
		return 0;
	if (G_TYPE_INT64 == json_node_get_value_type(node) || G_TYPE_DOUBLE == json_node_get_value_type(node))
		return json_node_get_double(node);
	return 0;
}

GPtrArray *
venture_forms_definition_from_json(const gchar *text, GError **error)
{
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GPtrArray) fields = g_ptr_array_new_with_free_func(venture_forms_field_free);
	JsonArray *array;
	guint i, j;

	root = venture_string_is_empty(text) ? NULL : json_from_string(text, NULL);
	if (NULL == root || !JSON_NODE_HOLDS_OBJECT(root) ||
	    !json_object_has_member(json_node_get_object(root), "fields") ||
	    !JSON_NODE_HOLDS_ARRAY(json_object_get_member(json_node_get_object(root), "fields")))
		goto broken;
	array = json_object_get_array_member(json_node_get_object(root), "fields");
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object;
		VentureFormsField *field;
		g_autofree gchar *kind = NULL;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			goto broken;
		object = json_node_get_object(node);
		field = g_new0(VentureFormsField, 1);
		g_ptr_array_add(fields, field);
		field->key = forms_member_string(object, "key");
		field->label = forms_member_string(object, "label");
		kind = forms_member_string(object, "kind");
		if (NULL == field->key || NULL == kind || !forms_kind_from_nick(kind, &field->kind))
			goto broken;
		field->required = json_object_get_boolean_member_with_default(object, "required", FALSE);
		field->marketing_consent = json_object_get_boolean_member_with_default(object, "marketing_consent", FALSE);
		field->sensitive = json_object_get_boolean_member_with_default(object, "sensitive", FALSE);
		field->position = json_object_get_int_member_with_default(object, "position", 0);
		field->help = forms_member_string(object, "help");
		field->placeholder = forms_member_string(object, "placeholder");
		field->pattern = forms_member_string(object, "pattern");
		field->default_value = forms_member_string(object, "default_value");
		field->maps_to = forms_member_string(object, "maps_to");
		field->autocomplete = forms_member_string(object, "autocomplete");
		field->min_value = forms_member_number(object, "min_value");
		field->max_value = forms_member_number(object, "max_value");
		field->min_length = (gint64)forms_member_number(object, "min_length");
		field->max_length = (gint64)forms_member_number(object, "max_length");
		field->choices = g_ptr_array_new_with_free_func(venture_forms_choice_free);
		if (json_object_has_member(object, "choices"))
		{
			JsonNode *choices = json_object_get_member(object, "choices");

			if (!JSON_NODE_HOLDS_ARRAY(choices))
				goto broken;
			for (j = 0; j < json_array_get_length(json_node_get_array(choices)); j++)
			{
				JsonNode *item = json_array_get_element(json_node_get_array(choices), j);
				VentureFormsChoice *choice;

				if (!JSON_NODE_HOLDS_OBJECT(item))
					goto broken;
				choice = g_new0(VentureFormsChoice, 1);
				choice->id = forms_member_string(json_node_get_object(item), "id");
				choice->label = forms_member_string(json_node_get_object(item), "label");
				g_ptr_array_add(field->choices, choice);
				if (NULL == choice->id || NULL == choice->label)
					goto broken;
			}
		}
	}
	return g_steal_pointer(&fields);

broken:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	                    "This form version's definition cannot be read");
	return NULL;
}

GPtrArray *
venture_forms_definition_for(VentureDatabase *database, VentureEntity *form,
	VentureEntity *version, GError **error)
{
	g_autofree gchar *text = NULL;

	if (NULL == version)
		return venture_forms_definition_from_records(database, form, error);
	text = venture_forms_get_string(version, "definition");
	return venture_forms_definition_from_json(text, error);
}

const VentureFormsField *
venture_forms_definition_find(GPtrArray *fields, const gchar *key)
{
	guint i;

	for (i = 0; NULL != fields && i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);

		if (0 == g_strcmp0(field->key, key))
			return field;
	}
	return NULL;
}

/* ==========================================================================
 * Versions
 * ========================================================================== */

VentureEntity *
venture_forms_version_by_number(VentureDatabase *database, VentureEntity *form,
	gint64 number, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;

	if (number <= 0)
		return NULL;
	query = venture_query_new(VENTURE_TYPE_FORM_VERSION);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_int(query, "number", VENTURE_FILTER_OP_EQ, number, NULL);
	return venture_database_find_one(database, query, error);
}

VentureEntity *
venture_forms_published_version(VentureDatabase *database, VentureEntity *form, GError **error)
{
	gint64 id;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	id = venture_forms_get_int(form, "published-version-id");
	if (id <= 0)
		return NULL;
	return venture_database_get(database, VENTURE_TYPE_FORM_VERSION, id, error);
}
