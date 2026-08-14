/*
 * venture-serializable.h - Objects that can round-trip through JSON and YAML
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * #VentureEntity implements this generically by walking its GObject
 * properties, which means a new record type gets a REST representation, a
 * YAML export and an AI-visible form for free. The interface exists so that
 * a type with a representation the generic walker cannot infer -- a packed
 * blob, a computed summary, a legacy shape that must stay stable -- can
 * override just the part it needs.
 *
 * Implementations must satisfy one rule: what to_json() writes, from_json()
 * must read back into an equal object. The test suite asserts this for every
 * registered entity type, so an override that breaks the round trip fails
 * the build rather than corrupting an export six months later.
 */

#ifndef VENTURE_SERIALIZABLE_H
#define VENTURE_SERIALIZABLE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_SERIALIZABLE (venture_serializable_get_type())

G_DECLARE_INTERFACE(VentureSerializable, venture_serializable,
                    VENTURE, SERIALIZABLE, GObject)

/**
 * VentureSerializableInterface:
 * @parent_iface: the parent interface
 * @to_json: serialise the object
 * @from_json: populate the object from a serialised form
 * @get_sensitive_fields: list properties that must never leave the process
 *
 * The vtable for #VentureSerializable.
 */
struct _VentureSerializableInterface
{
	GTypeInterface parent_iface;

	/**
	 * to_json: (virtual to_json)
	 * @self: a #VentureSerializable
	 * @include_sensitive: whether to include fields marked sensitive
	 *
	 * Returns: (transfer full): the serialised object
	 */
	JsonNode *(*to_json) (VentureSerializable *self,
	                      gboolean             include_sensitive);

	/**
	 * from_json: (virtual from_json)
	 * @self: a #VentureSerializable
	 * @node: the serialised form
	 * @error: (out) (optional): return location for a #GError
	 *
	 * Returns: %TRUE on success
	 */
	gboolean (*from_json) (VentureSerializable  *self,
	                       JsonNode             *node,
	                       GError              **error);

	/**
	 * get_sensitive_fields: (virtual get_sensitive_fields)
	 * @self: a #VentureSerializable
	 *
	 * Returns: (transfer none) (nullable) (array zero-terminated=1): the
	 *   names of properties that must be withheld from API responses,
	 *   logs, exports and anything the AI can read
	 */
	const gchar * const *(*get_sensitive_fields) (VentureSerializable *self);

	/*< private >*/
	gpointer padding[4];
};

/**
 * venture_serializable_to_json:
 * @self: a #VentureSerializable
 * @include_sensitive: whether to include fields marked sensitive
 *
 * Serialises @self. Pass %FALSE for @include_sensitive unless the
 * destination is the database or an explicit encrypted backup: password
 * hashes, API token secrets and provider keys are all marked sensitive, and
 * this flag is the only thing standing between them and an API response.
 *
 * Returns: (transfer full): the serialised object
 */
JsonNode *
venture_serializable_to_json(
	VentureSerializable	*self,
	gboolean		 include_sensitive
);

/**
 * venture_serializable_from_json:
 * @self: a #VentureSerializable
 * @node: the serialised form
 * @error: (out) (optional): return location for a #GError
 *
 * Populates @self from @node. Members that do not correspond to a property
 * are ignored rather than rejected, so a client sending a slightly newer or
 * older shape still works.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_serializable_from_json(
	VentureSerializable	 *self,
	JsonNode		 *node,
	GError			**error
);

/**
 * venture_serializable_to_json_string:
 * @self: a #VentureSerializable
 * @include_sensitive: whether to include sensitive fields
 * @pretty: whether to indent the output
 *
 * Returns: (transfer full): the serialised object as text
 */
gchar *
venture_serializable_to_json_string(
	VentureSerializable	*self,
	gboolean		 include_sensitive,
	gboolean		 pretty
);

/**
 * venture_serializable_to_yaml:
 * @self: a #VentureSerializable
 * @include_sensitive: whether to include sensitive fields
 *
 * Serialises to YAML, which is the format VENTURE uses for exports, fixture
 * files and anything destined for a notes repository.
 *
 * Returns: (transfer full): the serialised object as YAML text
 */
gchar *
venture_serializable_to_yaml(
	VentureSerializable	*self,
	gboolean		 include_sensitive
);

/**
 * venture_serializable_get_sensitive_fields:
 * @self: a #VentureSerializable
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the
 *   withheld property names
 */
const gchar * const *
venture_serializable_get_sensitive_fields(VentureSerializable *self);

/**
 * venture_serializable_is_sensitive_field:
 * @self: a #VentureSerializable
 * @name: a property name
 *
 * Returns: %TRUE if @name must be withheld
 */
gboolean
venture_serializable_is_sensitive_field(
	VentureSerializable	*self,
	const gchar		*name
);

G_END_DECLS

#endif /* VENTURE_SERIALIZABLE_H */
