/*
 * venture-field-spec.h - A declarative field definition
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureFieldSpec describes one field of a record: what it is called,
 * what type it holds, whether it is required, what it may contain.
 *
 * It exists so that one declaration drives everything downstream. Given a
 * list of field specs, VENTURE can:
 *
 *   - install matching GObject properties on a dynamically-created type
 *   - derive the SQL column, its type, its constraints and its indexes
 *   - render a form input in the web UI, with the right widget and
 *     validation
 *   - build the JSON schema for an AI tool parameter, complete with the
 *     enumeration of permitted values
 *   - validate an incoming REST payload
 *   - render a table column with the right alignment and formatting
 *
 * That is what makes a new venture type -- "I sell laser-cut art on Etsy and
 * I care about material cost, listing fee and shop section" -- a YAML file
 * rather than a code change.
 */

#ifndef VENTURE_FIELD_SPEC_H
#define VENTURE_FIELD_SPEC_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_FIELD_SPEC (venture_field_spec_get_type())

/**
 * VentureFieldSpec:
 *
 * Opaque. Use the accessors.
 */
struct _VentureFieldSpec
{
	/*< private >*/
	gchar			*name;
	gchar			*label;
	gchar			*help;
	VentureFieldKind	 kind;
	VentureColumnFlags	 flags;

	gboolean		 required;
	gchar			*default_text;

	/* For %VENTURE_FIELD_KIND_ENUM: the permitted values and their
	 * labels, parallel arrays terminated by NULL. */
	gchar			**choices;
	gchar			**choice_labels;

	/* For %VENTURE_FIELD_KIND_REFERENCE: the entity type this points at,
	 * as a registered entity name such as "contact". */
	gchar			*reference_type;

	/* Numeric and length bounds. Applied by the validator and emitted as
	 * min/max attributes on the rendered input. */
	gboolean		 has_min;
	gdouble			 min_value;
	gboolean		 has_max;
	gdouble			 max_value;
	gint			 max_length;

	/* An optional anchored regular expression the value must match. */
	gchar			*pattern;

	/* Presentation. */
	gchar			*unit;
	gint			 display_order;
	gboolean		 show_in_list;
};

GType
venture_field_spec_get_type(void) G_GNUC_CONST;

/**
 * venture_field_spec_new:
 * @name: the field name, in lowercase_snake_case
 * @label: (nullable): a human label; derived from @name when %NULL
 * @kind: the field type
 *
 * Returns: (transfer full): a new #VentureFieldSpec
 */
VentureFieldSpec *
venture_field_spec_new(
	const gchar		*name,
	const gchar		*label,
	VentureFieldKind	 kind
);

/**
 * venture_field_spec_new_from_json:
 * @name: the field name
 * @node: a JSON object describing the field, as produced by parsing a
 *   venture-type YAML definition
 * @error: (out) (optional): return location for a #GError
 *
 * Builds a field spec from its declarative description. Recognised members:
 * `type`, `label`, `help`, `required`, `default`, `choices`, `references`,
 * `min`, `max`, `max_length`, `pattern`, `unit`, `indexed`, `unique`,
 * `sensitive`, `searchable`, `immutable`, `order`, `list`.
 *
 * Returns: (transfer full) (nullable): the field spec, or %NULL on error
 */
VentureFieldSpec *
venture_field_spec_new_from_json(
	const gchar	 *name,
	JsonNode	 *node,
	GError		**error
);

/**
 * venture_field_spec_copy:
 * @self: (nullable): a #VentureFieldSpec
 *
 * Returns: (transfer full) (nullable): a deep copy
 */
VentureFieldSpec *
venture_field_spec_copy(const VentureFieldSpec *self);

/**
 * venture_field_spec_free:
 * @self: (nullable): a #VentureFieldSpec
 *
 * Frees @self. Safe to call with %NULL.
 */
void
venture_field_spec_free(VentureFieldSpec *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureFieldSpec, venture_field_spec_free)

/* --- Accessors ----------------------------------------------------------- */

const gchar *
venture_field_spec_get_name(const VentureFieldSpec *self);

const gchar *
venture_field_spec_get_label(const VentureFieldSpec *self);

const gchar *
venture_field_spec_get_help(const VentureFieldSpec *self);

VentureFieldKind
venture_field_spec_get_kind(const VentureFieldSpec *self);

VentureColumnFlags
venture_field_spec_get_flags(const VentureFieldSpec *self);

gboolean
venture_field_spec_get_required(const VentureFieldSpec *self);

/**
 * venture_field_spec_get_choices:
 * @self: a #VentureFieldSpec
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the
 *   permitted values for an enumeration field
 */
const gchar * const *
venture_field_spec_get_choices(const VentureFieldSpec *self);

const gchar *
venture_field_spec_get_reference_type(const VentureFieldSpec *self);

const gchar *
venture_field_spec_get_unit(const VentureFieldSpec *self);

gboolean
venture_field_spec_get_show_in_list(const VentureFieldSpec *self);

gint
venture_field_spec_get_display_order(const VentureFieldSpec *self);

/* --- Derived representations --------------------------------------------- */

/**
 * venture_field_spec_get_value_type:
 * @self: a #VentureFieldSpec
 *
 * Maps the field kind onto the #GType a matching GObject property should
 * hold. Money becomes %VENTURE_TYPE_MONEY, dates become %G_TYPE_DATE_TIME,
 * enumerations become %G_TYPE_STRING because their value set is discovered
 * at run time rather than registered as a GEnum.
 *
 * Returns: the property type
 */
GType
venture_field_spec_get_value_type(const VentureFieldSpec *self);

/**
 * venture_field_spec_create_param_spec:
 * @self: a #VentureFieldSpec
 * @param_flags: the #GParamFlags to install with, usually
 *   %G_PARAM_READWRITE | %G_PARAM_STATIC_STRINGS
 *
 * Builds the #GParamSpec for this field, so a dynamically-created entity
 * class can install it with g_object_class_install_property().
 *
 * Returns: (transfer full): a new #GParamSpec
 */
GParamSpec *
venture_field_spec_create_param_spec(
	const VentureFieldSpec	*self,
	GParamFlags		 param_flags
);

/**
 * venture_field_spec_validate:
 * @self: a #VentureFieldSpec
 * @value: (nullable): the value to check
 * @error: (out) (optional): return location for a #GError
 *
 * Checks a value against the spec: presence when required, numeric bounds,
 * string length, pattern, and membership of the choice list. Raises
 * %VENTURE_ERROR_VALIDATION with a message naming the field.
 *
 * Returns: %TRUE if @value is acceptable
 */
gboolean
venture_field_spec_validate(
	const VentureFieldSpec	 *self,
	const GValue		 *value,
	GError			**error
);

/**
 * venture_field_spec_to_json_schema:
 * @self: a #VentureFieldSpec
 *
 * Renders the field as a JSON Schema property, which is what an AI tool
 * parameter definition needs. Enumerations emit their choice list, so the
 * model is told the valid values rather than guessing them.
 *
 * Returns: (transfer full): a new #JsonNode
 */
JsonNode *
venture_field_spec_to_json_schema(const VentureFieldSpec *self);

/**
 * venture_field_spec_to_json:
 * @self: a #VentureFieldSpec
 *
 * Serialises the full spec, which the web UI uses to render a form and the
 * CLI uses to describe a type.
 *
 * Returns: (transfer full): a new #JsonNode
 */
JsonNode *
venture_field_spec_to_json(const VentureFieldSpec *self);

/**
 * venture_field_spec_compare_display_order:
 * @a: (type VentureFieldSpec): the first spec
 * @b: (type VentureFieldSpec): the second spec
 *
 * Sort function ordering fields by their declared display order, then by
 * label. Suitable for g_ptr_array_sort_values().
 *
 * Returns: a comparison result
 */
gint
venture_field_spec_compare_display_order(
	gconstpointer	a,
	gconstpointer	b
);

G_END_DECLS

#endif /* VENTURE_FIELD_SPEC_H */
