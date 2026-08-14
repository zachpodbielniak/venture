/*
 * venture-venture-type.h - Declaratively-defined kinds of venture
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A venture type describes a kind of venture: the extra fields a venture of
 * that kind carries, the metrics worth watching, and the columns a report
 * about it should show.
 *
 * It is a YAML file, not code. That is the point: "I now also sell laser-cut
 * art on Etsy and I care about material cost, listing fee and shop section"
 * should cost a file, not a rebuild.
 *
 * The extra fields are stored in the venture's attribute bag rather than as
 * real columns, because a type can appear and disappear at run time and a
 * column cannot. They are still validated against their declarations, still
 * rendered in the UI, and still described to the AI -- the same
 * #VentureFieldSpec drives all three, exactly as it does for a compiled
 * record type.
 */

#ifndef VENTURE_VENTURE_TYPE_H
#define VENTURE_VENTURE_TYPE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_VENTURE_TYPE (venture_venture_type_get_type())

G_DECLARE_FINAL_TYPE(VentureVentureType, venture_venture_type,
                     VENTURE, VENTURE_TYPE, GObject)

/**
 * venture_venture_type_new:
 * @name: the registry key, e.g. "etsy"
 * @label: (nullable): a human label
 *
 * Returns: (transfer full): a new venture type
 */
VentureVentureType *
venture_venture_type_new(
	const gchar	*name,
	const gchar	*label
);

/**
 * venture_venture_type_new_from_yaml:
 * @yaml: the definition
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a venture type definition:
 *
 * |[
 * name: etsy
 * label: Etsy shop
 * description: A print-on-demand or handmade storefront
 *
 * fields:
 *   shop_name: string
 *   listing_fee:
 *     type: money
 *     label: Listing fee
 *   material:
 *     type: enum
 *     choices: [wood, acrylic, paper]
 *
 * metrics: [revenue, units_sold]
 * report_columns: [shop_name, material]
 * ]|
 *
 * Returns: (transfer full) (nullable): the type, or %NULL on error
 */
VentureVentureType *
venture_venture_type_new_from_yaml(
	const gchar	 *yaml,
	GError		**error
);

/**
 * venture_venture_type_new_from_file:
 * @path: the YAML file to read
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the type, or %NULL on error
 */
VentureVentureType *
venture_venture_type_new_from_file(
	const gchar	 *path,
	GError		**error
);

const gchar *
venture_venture_type_get_name(VentureVentureType *self);

const gchar *
venture_venture_type_get_label(VentureVentureType *self);

const gchar *
venture_venture_type_get_description(VentureVentureType *self);

/**
 * venture_venture_type_get_fields:
 * @self: a #VentureVentureType
 *
 * Returns: (transfer none) (element-type VentureFieldSpec): the extra fields
 *   a venture of this kind carries, in display order
 */
GPtrArray *
venture_venture_type_get_fields(VentureVentureType *self);

/**
 * venture_venture_type_get_metrics:
 * @self: a #VentureVentureType
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): the metric
 *   keys worth watching for this kind
 */
const gchar * const *
venture_venture_type_get_metrics(VentureVentureType *self);

/**
 * venture_venture_type_add_field:
 * @self: a #VentureVentureType
 * @spec: (transfer full): the field to add
 *
 * Adds a field. Used by the YAML parser and available to a plugin that wants
 * to define a type in code.
 */
void
venture_venture_type_add_field(
	VentureVentureType	*self,
	VentureFieldSpec	*spec
);

/**
 * venture_venture_type_validate_venture:
 * @self: a #VentureVentureType
 * @venture: the venture to check
 * @error: (out) (optional): return location for a #GError
 *
 * Checks a venture's attributes against this type's field declarations, so a
 * declared required field is genuinely required and a declared enum really
 * rejects a value outside its choices.
 *
 * Returns: %TRUE if the venture satisfies its type
 */
gboolean
venture_venture_type_validate_venture(
	VentureVentureType	 *self,
	VentureEntity		 *venture,
	GError			**error
);

/**
 * venture_venture_type_to_json:
 * @self: a #VentureVentureType
 *
 * Returns: (transfer full): the type as JSON, for the UI and the AI
 */
JsonNode *
venture_venture_type_to_json(VentureVentureType *self);

/* --- Registry ------------------------------------------------------------ */

#define VENTURE_TYPE_VENTURE_TYPE_REGISTRY \
	(venture_venture_type_registry_get_type())

G_DECLARE_FINAL_TYPE(VentureVentureTypeRegistry, venture_venture_type_registry,
                     VENTURE, VENTURE_TYPE_REGISTRY, GObject)

/**
 * venture_venture_type_registry_new:
 *
 * Returns: (transfer full): an empty registry
 */
VentureVentureTypeRegistry *
venture_venture_type_registry_new(void);

/**
 * venture_venture_type_registry_add:
 * @self: a #VentureVentureTypeRegistry
 * @type: (transfer full): the type to register
 *
 * Registers a venture type, replacing any of the same name.
 */
void
venture_venture_type_registry_add(
	VentureVentureTypeRegistry	*self,
	VentureVentureType		*type
);

/**
 * venture_venture_type_registry_lookup:
 * @self: a #VentureVentureTypeRegistry
 * @name: the type name
 *
 * Returns: (transfer none) (nullable): the type, or %NULL
 */
VentureVentureType *
venture_venture_type_registry_lookup(
	VentureVentureTypeRegistry	*self,
	const gchar			*name
);

/**
 * venture_venture_type_registry_list:
 * @self: a #VentureVentureTypeRegistry
 *
 * Returns: (transfer container) (element-type VentureVentureType): every
 *   registered type, sorted by name
 */
GPtrArray *
venture_venture_type_registry_list(VentureVentureTypeRegistry *self);

/**
 * venture_venture_type_registry_load_directory:
 * @self: a #VentureVentureTypeRegistry
 * @path: a directory of .yaml definitions
 * @error: (out) (optional): return location for a #GError
 *
 * Loads every .yaml file in a directory. A file that fails to parse is
 * reported and skipped rather than aborting the load, so one bad definition
 * does not cost you the others.
 *
 * Returns: the number of types loaded
 */
guint
venture_venture_type_registry_load_directory(
	VentureVentureTypeRegistry	 *self,
	const gchar			 *path,
	GError				**error
);

/**
 * venture_venture_type_registry_to_json:
 * @self: a #VentureVentureTypeRegistry
 *
 * Returns: (transfer full): every type as a JSON array
 */
JsonNode *
venture_venture_type_registry_to_json(VentureVentureTypeRegistry *self);

G_END_DECLS

#endif /* VENTURE_VENTURE_TYPE_H */
