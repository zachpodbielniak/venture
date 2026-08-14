/*
 * venture-field-spec.c - A declarative field definition
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

G_DEFINE_BOXED_TYPE(VentureFieldSpec, venture_field_spec,
                    venture_field_spec_copy, venture_field_spec_free)

/*
 * Turns "material_cost" into "Material cost". Used whenever a declaration
 * gives a field a name but no label, which is most of the time.
 */
static gchar *
venture_field_spec_humanise(const gchar *name)
{
	g_autofree gchar *spaced = NULL;

	if (NULL == name)
		return NULL;

	spaced = g_strdup(name);
	g_strdelimit(spaced, "_-", ' ');

	if ('\0' == spaced[0])
		return g_steal_pointer(&spaced);

	spaced[0] = g_ascii_toupper(spaced[0]);

	return g_steal_pointer(&spaced);
}

VentureFieldSpec *
venture_field_spec_new(
	const gchar		*name,
	const gchar		*label,
	VentureFieldKind	 kind
){
	VentureFieldSpec *self;

	g_return_val_if_fail(NULL != name, NULL);

	self = g_new0(VentureFieldSpec, 1);
	self->name = g_strdup(name);
	self->label = (NULL != label)
		? g_strdup(label)
		: venture_field_spec_humanise(name);
	self->kind = kind;
	self->flags = VENTURE_COLUMN_FLAG_NONE;
	self->max_length = -1;
	self->display_order = 100;
	self->show_in_list = TRUE;

	return self;
}

/*
 * Reads a boolean member that may be absent, defaulting when it is.
 */
static gboolean
venture_field_spec_json_bool(
	JsonObject	*object,
	const gchar	*member,
	gboolean	 fallback
){
	if (!json_object_has_member(object, member))
		return fallback;

	return json_object_get_boolean_member(object, member);
}

VentureFieldSpec *
venture_field_spec_new_from_json(
	const gchar	 *name,
	JsonNode	 *node,
	GError		**error
){
	g_autoptr(VentureFieldSpec) self = NULL;
	JsonObject *object;
	const gchar *type_nick;
	gint kind_value;

	g_return_val_if_fail(NULL != name, NULL);
	g_return_val_if_fail(NULL != node, NULL);

	/* A bare string is shorthand for a field of that type with every
	 * other setting defaulted: `sku: string` rather than a whole block.
	 * Most declarations are that simple and should read that way. */
	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)))
	{
		if (!venture_enum_from_nick(VENTURE_TYPE_FIELD_KIND,
		                            json_node_get_string(node), &kind_value))
		{
			g_autofree gchar *valid = NULL;
			g_auto(GStrv) nicks = NULL;

			/* The shorthand is the commoner spelling, so it gets the
			 * same list of valid types the long form does: whoever is
			 * reading this is mid-edit in a YAML file. */
			nicks = venture_enum_list_nicks(VENTURE_TYPE_FIELD_KIND);
			valid = g_strjoinv(", ", nicks);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			            "Field \"%s\" has unknown type \"%s\". "
			            "Valid types: %s",
			            name, json_node_get_string(node), valid);
			return NULL;
		}

		return venture_field_spec_new(name, NULL,
		                              (VentureFieldKind)kind_value);
	}

	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Field \"%s\" must be a type name or an object", name);
		return NULL;
	}

	object = json_node_get_object(node);

	type_nick = json_object_has_member(object, "type")
		? json_object_get_string_member(object, "type")
		: "string";

	if (!venture_enum_from_nick(VENTURE_TYPE_FIELD_KIND, type_nick,
	                            &kind_value))
	{
		g_autofree gchar *valid = NULL;
		g_auto(GStrv) nicks = NULL;

		nicks = venture_enum_list_nicks(VENTURE_TYPE_FIELD_KIND);
		valid = g_strjoinv(", ", nicks);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Field \"%s\" has unknown type \"%s\". Valid types: %s",
		            name, type_nick, valid);
		return NULL;
	}

	self = venture_field_spec_new(name,
		json_object_has_member(object, "label")
			? json_object_get_string_member(object, "label") : NULL,
		(VentureFieldKind)kind_value);

	if (json_object_has_member(object, "help"))
		self->help = g_strdup(json_object_get_string_member(object, "help"));

	if (json_object_has_member(object, "unit"))
		self->unit = g_strdup(json_object_get_string_member(object, "unit"));

	if (json_object_has_member(object, "pattern"))
		self->pattern = g_strdup(json_object_get_string_member(object, "pattern"));

	if (json_object_has_member(object, "references"))
	{
		self->reference_type =
			g_strdup(json_object_get_string_member(object, "references"));
	}

	if (json_object_has_member(object, "default"))
	{
		JsonNode *default_node;

		/* The default is kept as text and converted lazily, because at
		 * this point the target GType may not exist yet -- a reference
		 * field can point at a type declared later in the same file. */
		default_node = json_object_get_member(object, "default");

		if (JSON_NODE_HOLDS_VALUE(default_node))
		{
			g_autoptr(JsonGenerator) generator = NULL;
			GType value_type;

			value_type = json_node_get_value_type(default_node);

			if (G_TYPE_STRING == value_type)
			{
				self->default_text =
					g_strdup(json_node_get_string(default_node));
			}
			else
			{
				generator = json_generator_new();
				json_generator_set_root(generator, default_node);
				self->default_text =
					json_generator_to_data(generator, NULL);
			}
		}
	}

	self->required = venture_field_spec_json_bool(object, "required", FALSE);

	if (self->required)
		self->flags |= VENTURE_COLUMN_FLAG_NOT_NULL;

	if (venture_field_spec_json_bool(object, "indexed", FALSE))
		self->flags |= VENTURE_COLUMN_FLAG_INDEXED;

	if (venture_field_spec_json_bool(object, "unique", FALSE))
		self->flags |= VENTURE_COLUMN_FLAG_UNIQUE;

	if (venture_field_spec_json_bool(object, "sensitive", FALSE))
		self->flags |= VENTURE_COLUMN_FLAG_SENSITIVE;

	if (venture_field_spec_json_bool(object, "searchable", FALSE))
		self->flags |= VENTURE_COLUMN_FLAG_SEARCHABLE;

	if (venture_field_spec_json_bool(object, "immutable", FALSE))
		self->flags |= VENTURE_COLUMN_FLAG_IMMUTABLE;

	if (venture_field_spec_json_bool(object, "transient", FALSE))
		self->flags |= VENTURE_COLUMN_FLAG_TRANSIENT;

	self->show_in_list = venture_field_spec_json_bool(object, "list", TRUE);

	if (json_object_has_member(object, "order"))
	{
		self->display_order =
			(gint)json_object_get_int_member(object, "order");
	}

	if (json_object_has_member(object, "min"))
	{
		self->has_min = TRUE;
		self->min_value = json_object_get_double_member(object, "min");
	}

	if (json_object_has_member(object, "max"))
	{
		self->has_max = TRUE;
		self->max_value = json_object_get_double_member(object, "max");
	}

	if (json_object_has_member(object, "max_length"))
	{
		self->max_length =
			(gint)json_object_get_int_member(object, "max_length");
	}

	if (json_object_has_member(object, "choices"))
	{
		JsonArray *array;
		g_autoptr(GPtrArray) values = NULL;
		g_autoptr(GPtrArray) labels = NULL;
		guint i;

		array = json_object_get_array_member(object, "choices");
		values = g_ptr_array_new();
		labels = g_ptr_array_new();

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *element;

			element = json_array_get_element(array, i);

			/* A choice is either a bare value or an object with a
			 * separate label, so "wip" can display as "Work in
			 * progress" without the stored value changing. */
			if (JSON_NODE_HOLDS_OBJECT(element))
			{
				JsonObject *choice;
				const gchar *value;

				choice = json_node_get_object(element);
				value = json_object_get_string_member(choice, "value");

				g_ptr_array_add(values, g_strdup(value));
				g_ptr_array_add(labels,
					json_object_has_member(choice, "label")
						? g_strdup(json_object_get_string_member(choice, "label"))
						: venture_field_spec_humanise(value));
			}
			else
			{
				const gchar *value;

				value = json_node_get_string(element);
				g_ptr_array_add(values, g_strdup(value));
				g_ptr_array_add(labels, venture_field_spec_humanise(value));
			}
		}

		g_ptr_array_add(values, NULL);
		g_ptr_array_add(labels, NULL);

		self->choices = (gchar **)g_ptr_array_free(g_steal_pointer(&values), FALSE);
		self->choice_labels = (gchar **)g_ptr_array_free(g_steal_pointer(&labels), FALSE);
	}

	if ((VENTURE_FIELD_KIND_ENUM == self->kind) && (NULL == self->choices))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Field \"%s\" is an enum but declares no choices", name);
		return NULL;
	}

	if ((VENTURE_FIELD_KIND_REFERENCE == self->kind) &&
	    (NULL == self->reference_type))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Field \"%s\" is a reference but declares no "
		            "\"references\" target", name);
		return NULL;
	}

	return g_steal_pointer(&self);
}

VentureFieldSpec *
venture_field_spec_copy(const VentureFieldSpec *self)
{
	VentureFieldSpec *copy;

	if (NULL == self)
		return NULL;

	copy = g_new0(VentureFieldSpec, 1);
	copy->name = g_strdup(self->name);
	copy->label = g_strdup(self->label);
	copy->help = g_strdup(self->help);
	copy->kind = self->kind;
	copy->flags = self->flags;
	copy->required = self->required;
	copy->default_text = g_strdup(self->default_text);
	copy->choices = g_strdupv(self->choices);
	copy->choice_labels = g_strdupv(self->choice_labels);
	copy->reference_type = g_strdup(self->reference_type);
	copy->has_min = self->has_min;
	copy->min_value = self->min_value;
	copy->has_max = self->has_max;
	copy->max_value = self->max_value;
	copy->max_length = self->max_length;
	copy->pattern = g_strdup(self->pattern);
	copy->unit = g_strdup(self->unit);
	copy->display_order = self->display_order;
	copy->show_in_list = self->show_in_list;

	return copy;
}

void
venture_field_spec_free(VentureFieldSpec *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->name, g_free);
	g_clear_pointer(&self->label, g_free);
	g_clear_pointer(&self->help, g_free);
	g_clear_pointer(&self->default_text, g_free);
	g_clear_pointer(&self->reference_type, g_free);
	g_clear_pointer(&self->pattern, g_free);
	g_clear_pointer(&self->unit, g_free);
	g_clear_pointer(&self->choices, g_strfreev);
	g_clear_pointer(&self->choice_labels, g_strfreev);
	g_free(self);
}

/* --- Accessors ----------------------------------------------------------- */

const gchar *
venture_field_spec_get_name(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->name;
}

const gchar *
venture_field_spec_get_label(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return (NULL != self->label) ? self->label : self->name;
}

const gchar *
venture_field_spec_get_help(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->help;
}

VentureFieldKind
venture_field_spec_get_kind(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, VENTURE_FIELD_KIND_STRING);

	return self->kind;
}

VentureColumnFlags
venture_field_spec_get_flags(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, VENTURE_COLUMN_FLAG_NONE);

	return self->flags;
}

gboolean
venture_field_spec_get_required(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, FALSE);

	return self->required;
}

const gchar * const *
venture_field_spec_get_choices(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return (const gchar * const *)self->choices;
}

const gchar *
venture_field_spec_get_reference_type(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->reference_type;
}

const gchar *
venture_field_spec_get_unit(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->unit;
}

gboolean
venture_field_spec_get_show_in_list(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, FALSE);

	return self->show_in_list;
}

gint
venture_field_spec_get_display_order(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->display_order;
}

/* --- Derived representations --------------------------------------------- */

GType
venture_field_spec_get_value_type(const VentureFieldSpec *self)
{
	g_return_val_if_fail(NULL != self, G_TYPE_INVALID);

	switch (self->kind)
	{
	case VENTURE_FIELD_KIND_INTEGER:
		return G_TYPE_INT64;

	case VENTURE_FIELD_KIND_DOUBLE:
		return G_TYPE_DOUBLE;

	case VENTURE_FIELD_KIND_MONEY:
		return VENTURE_TYPE_MONEY;

	case VENTURE_FIELD_KIND_BOOLEAN:
		return G_TYPE_BOOLEAN;

	case VENTURE_FIELD_KIND_DATE:
	case VENTURE_FIELD_KIND_DATETIME:
		return G_TYPE_DATE_TIME;

	/* A reference is stored as the target's integer identifier. Resolving
	 * it to an object is the repository's job, not the property's. */
	case VENTURE_FIELD_KIND_REFERENCE:
		return G_TYPE_INT64;

	/* Enumerations declared in YAML have no registered GEnum, so their
	 * values live as strings and are validated against the choice list. */
	case VENTURE_FIELD_KIND_ENUM:
	case VENTURE_FIELD_KIND_STRING:
	case VENTURE_FIELD_KIND_TEXT:
	case VENTURE_FIELD_KIND_JSON:
	default:
		return G_TYPE_STRING;
	}
}

GParamSpec *
venture_field_spec_create_param_spec(
	const VentureFieldSpec	*self,
	GParamFlags		 param_flags
){
	const gchar *label;
	const gchar *blurb;

	g_return_val_if_fail(NULL != self, NULL);

	label = venture_field_spec_get_label(self);
	blurb = (NULL != self->help) ? self->help : label;

	switch (self->kind)
	{
	case VENTURE_FIELD_KIND_INTEGER:
	case VENTURE_FIELD_KIND_REFERENCE:
		return g_param_spec_int64(self->name, label, blurb,
			self->has_min ? (gint64)self->min_value : G_MININT64,
			self->has_max ? (gint64)self->max_value : G_MAXINT64,
			0, param_flags);

	case VENTURE_FIELD_KIND_DOUBLE:
		return g_param_spec_double(self->name, label, blurb,
			self->has_min ? self->min_value : -G_MAXDOUBLE,
			self->has_max ? self->max_value : G_MAXDOUBLE,
			0.0, param_flags);

	case VENTURE_FIELD_KIND_BOOLEAN:
		return g_param_spec_boolean(self->name, label, blurb,
			FALSE, param_flags);

	case VENTURE_FIELD_KIND_MONEY:
		return g_param_spec_boxed(self->name, label, blurb,
			VENTURE_TYPE_MONEY, param_flags);

	case VENTURE_FIELD_KIND_DATE:
	case VENTURE_FIELD_KIND_DATETIME:
		return g_param_spec_boxed(self->name, label, blurb,
			G_TYPE_DATE_TIME, param_flags);

	default:
		return g_param_spec_string(self->name, label, blurb,
			self->default_text, param_flags);
	}
}

/*
 * Extracts a numeric value from a GValue of any of the numeric types a field
 * might hold, so the bounds check does not need a case per type. Money is
 * compared in major units, which is what a declared min or max means.
 */
static gboolean
venture_field_spec_value_as_double(
	const GValue	*value,
	gdouble		*out_number
){
	if (G_VALUE_HOLDS_INT64(value))
	{
		*out_number = (gdouble)g_value_get_int64(value);
		return TRUE;
	}

	if (G_VALUE_HOLDS_INT(value))
	{
		*out_number = (gdouble)g_value_get_int(value);
		return TRUE;
	}

	if (G_VALUE_HOLDS_DOUBLE(value))
	{
		*out_number = g_value_get_double(value);
		return TRUE;
	}

	if (G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY))
	{
		const VentureMoney *money;

		money = g_value_get_boxed(value);
		*out_number = (NULL != money) ? venture_money_to_double(money) : 0.0;
		return TRUE;
	}

	return FALSE;
}

gboolean
venture_field_spec_validate(
	const VentureFieldSpec	 *self,
	const GValue		 *value,
	GError			**error
){
	const gchar *text;
	gdouble number;

	g_return_val_if_fail(NULL != self, FALSE);

	/* An absent value is only a problem when the field is required. This
	 * is deliberately separate from an empty string, which for a required
	 * field is also a failure -- see below. */
	if (NULL == value)
	{
		if (self->required)
		{
			venture_set_error_validation(error,
				venture_field_spec_get_label(self), "is required");
			return FALSE;
		}

		return TRUE;
	}

	text = G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : NULL;

	if (self->required && G_VALUE_HOLDS_STRING(value) &&
	    ((NULL == text) || ('\0' == text[0])))
	{
		venture_set_error_validation(error,
			venture_field_spec_get_label(self), "is required");
		return FALSE;
	}

	/* Everything below is a constraint on a value that is present; an
	 * absent optional value has already been accepted. */
	if (G_VALUE_HOLDS_STRING(value) && (NULL == text))
		return TRUE;

	if ((NULL != text) && (self->max_length > 0) &&
	    (g_utf8_strlen(text, -1) > self->max_length))
	{
		venture_set_error_validation(error,
			venture_field_spec_get_label(self),
			"must be at most %d characters", self->max_length);
		return FALSE;
	}

	if ((NULL != text) && (NULL != self->pattern) && ('\0' != text[0]))
	{
		g_autoptr(GRegex) regex = NULL;
		g_autoptr(GError) regex_error = NULL;

		regex = g_regex_new(self->pattern, G_REGEX_ANCHORED, 0,
		                    &regex_error);

		if (NULL == regex)
		{
			/* A malformed pattern is a fault in the declaration, not
			 * in the data. Say which, or the operator will spend an
			 * hour looking at the wrong thing. */
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			            "Field \"%s\" declares an invalid pattern: %s",
			            self->name, regex_error->message);
			return FALSE;
		}

		if (!g_regex_match(regex, text, G_REGEX_MATCH_ANCHORED, NULL))
		{
			venture_set_error_validation(error,
				venture_field_spec_get_label(self),
				"is not in the expected format");
			return FALSE;
		}
	}

	if ((VENTURE_FIELD_KIND_ENUM == self->kind) && (NULL != text) &&
	    ('\0' != text[0]) && (NULL != self->choices))
	{
		if (!g_strv_contains((const gchar * const *)self->choices, text))
		{
			g_autofree gchar *valid = NULL;

			valid = g_strjoinv(", ", self->choices);

			venture_set_error_validation(error,
				venture_field_spec_get_label(self),
				"must be one of: %s", valid);
			return FALSE;
		}
	}

	if ((self->has_min || self->has_max) &&
	    venture_field_spec_value_as_double(value, &number))
	{
		if (self->has_min && (number < self->min_value))
		{
			venture_set_error_validation(error,
				venture_field_spec_get_label(self),
				"must be at least %g", self->min_value);
			return FALSE;
		}

		if (self->has_max && (number > self->max_value))
		{
			venture_set_error_validation(error,
				venture_field_spec_get_label(self),
				"must be at most %g", self->max_value);
			return FALSE;
		}
	}

	return TRUE;
}

JsonNode *
venture_field_spec_to_json_schema(const VentureFieldSpec *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	const gchar *json_type;
	g_autoptr(GString) description = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	switch (self->kind)
	{
	case VENTURE_FIELD_KIND_INTEGER:
	case VENTURE_FIELD_KIND_REFERENCE:
		json_type = "integer";
		break;

	case VENTURE_FIELD_KIND_DOUBLE:
		json_type = "number";
		break;

	case VENTURE_FIELD_KIND_BOOLEAN:
		json_type = "boolean";
		break;

	/* Money is presented to the model as a string so it writes "12.34
	 * USD" rather than having to reason about minor units and exponents.
	 * The parser accepts that and converts it exactly. */
	default:
		json_type = "string";
		break;
	}

	description = g_string_new(NULL);
	g_string_append(description, venture_field_spec_get_label(self));

	if (NULL != self->help)
		g_string_append_printf(description, ". %s", self->help);

	if (VENTURE_FIELD_KIND_MONEY == self->kind)
	{
		g_string_append(description,
			". A monetary amount such as \"12.34\" or \"12.34 EUR\"");
	}
	else if (VENTURE_FIELD_KIND_DATE == self->kind)
	{
		g_string_append(description, ". A date in YYYY-MM-DD form");
	}
	else if (VENTURE_FIELD_KIND_DATETIME == self->kind)
	{
		g_string_append(description, ". An ISO 8601 timestamp");
	}
	else if (VENTURE_FIELD_KIND_REFERENCE == self->kind)
	{
		g_string_append_printf(description,
			". The numeric id of a %s record", self->reference_type);
	}

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, json_type);

	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description->str);

	if (NULL != self->choices)
	{
		gsize i;

		json_builder_set_member_name(builder, "enum");
		json_builder_begin_array(builder);

		for (i = 0; NULL != self->choices[i]; i++)
			json_builder_add_string_value(builder, self->choices[i]);

		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

JsonNode *
venture_field_spec_to_json(const VentureFieldSpec *self)
{
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, self->name);

	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, venture_field_spec_get_label(self));

	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_FIELD_KIND, (gint)self->kind));

	json_builder_set_member_name(builder, "required");
	json_builder_add_boolean_value(builder, self->required);

	json_builder_set_member_name(builder, "show_in_list");
	json_builder_add_boolean_value(builder, self->show_in_list);

	json_builder_set_member_name(builder, "order");
	json_builder_add_int_value(builder, (gint64)self->display_order);

	if (NULL != self->help)
	{
		json_builder_set_member_name(builder, "help");
		json_builder_add_string_value(builder, self->help);
	}

	if (NULL != self->unit)
	{
		json_builder_set_member_name(builder, "unit");
		json_builder_add_string_value(builder, self->unit);
	}

	if (NULL != self->reference_type)
	{
		json_builder_set_member_name(builder, "references");
		json_builder_add_string_value(builder, self->reference_type);
	}

	if (NULL != self->default_text)
	{
		json_builder_set_member_name(builder, "default");
		json_builder_add_string_value(builder, self->default_text);
	}

	if (NULL != self->choices)
	{
		gsize i;

		json_builder_set_member_name(builder, "choices");
		json_builder_begin_array(builder);

		for (i = 0; NULL != self->choices[i]; i++)
		{
			json_builder_begin_object(builder);

			json_builder_set_member_name(builder, "value");
			json_builder_add_string_value(builder, self->choices[i]);

			json_builder_set_member_name(builder, "label");
			json_builder_add_string_value(builder,
				(NULL != self->choice_labels) ? self->choice_labels[i]
				                              : self->choices[i]);

			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
	}

	if (self->has_min)
	{
		json_builder_set_member_name(builder, "min");
		json_builder_add_double_value(builder, self->min_value);
	}

	if (self->has_max)
	{
		json_builder_set_member_name(builder, "max");
		json_builder_add_double_value(builder, self->max_value);
	}

	if (self->max_length > 0)
	{
		json_builder_set_member_name(builder, "max_length");
		json_builder_add_int_value(builder, (gint64)self->max_length);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

gint
venture_field_spec_compare_display_order(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureFieldSpec *spec_a;
	const VentureFieldSpec *spec_b;

	spec_a = a;
	spec_b = b;

	if (spec_a->display_order != spec_b->display_order)
		return (spec_a->display_order < spec_b->display_order) ? -1 : 1;

	return g_strcmp0(venture_field_spec_get_label(spec_a),
	                 venture_field_spec_get_label(spec_b));
}
