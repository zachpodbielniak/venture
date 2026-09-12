/*
 * venture-entity-registry.h - The catalogue of known record types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything generic in VENTURE dispatches through this registry: the REST
 * router turns /api/v1/<name> into a #GType, the schema builder walks it to
 * emit tables, the AI tool layer enumerates it to describe what can be
 * queried, and venturectl uses it to know which subcommands exist.
 *
 * A plugin registering a type therefore gains all of that at once, and the
 * core has no list of entity types anywhere else that could fall out of step.
 */

#ifndef VENTURE_ENTITY_REGISTRY_H
#define VENTURE_ENTITY_REGISTRY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_ENTITY_REGISTRY (venture_entity_registry_get_type())

G_DECLARE_FINAL_TYPE(VentureEntityRegistry, venture_entity_registry,
                     VENTURE, ENTITY_REGISTRY, GObject)

/**
 * venture_entity_registry_get_default:
 *
 * Retrieves the process-wide registry, creating and populating it with the
 * built-in entity types on first call.
 *
 * Returns: (transfer none): the registry
 */
VentureEntityRegistry *
venture_entity_registry_get_default(void);

/**
 * venture_entity_registry_new:
 *
 * Creates an empty registry. Only the test suite needs this; everything else
 * uses venture_entity_registry_get_default().
 *
 * Returns: (transfer full): a new #VentureEntityRegistry
 */
VentureEntityRegistry *
venture_entity_registry_new(void);

/**
 * venture_entity_registry_register:
 * @self: a #VentureEntityRegistry
 * @entity_type: a #GType deriving from %VENTURE_TYPE_ENTITY
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a record type. The entity name is taken from the type itself, so
 * two types cannot claim the same name; the second registration fails rather
 * than shadowing the first, which would otherwise route half the system to
 * one type and half to the other.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_entity_registry_register(
	VentureEntityRegistry	 *self,
	GType			  entity_type,
	GError			**error
);

/**
 * venture_entity_registry_lookup:
 * @self: a #VentureEntityRegistry
 * @entity_name: the singular machine name, e.g. "sale"
 *
 * Looks a type up by name. The plural form is also accepted, so a caller
 * holding a table name or a REST path segment does not have to singularise
 * it first.
 *
 * Returns: the #GType, or %G_TYPE_INVALID if unknown
 */
GType
venture_entity_registry_lookup(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_entity_registry_lookup_any:
 * @self: a #VentureEntityRegistry
 * @entity_name: the singular or plural machine name
 *
 * Looks a type up whether or not its module has hidden it. For error
 * messages and for the few places -- the module registry, the save-time
 * reference check -- that need to tell "disabled" from "never existed".
 * Everything that offers types to a caller uses venture_entity_registry_lookup().
 *
 * Returns: the #GType, or %G_TYPE_INVALID if never registered
 */
GType
venture_entity_registry_lookup_any(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_entity_registry_is_type_enabled:
 * @self: a #VentureEntityRegistry
 * @entity_name: the singular or plural machine name
 *
 * Returns: %TRUE if the type is registered and not hidden by a module
 */
gboolean
venture_entity_registry_is_type_enabled(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_entity_registry_get_type_module:
 * @self: a #VentureEntityRegistry
 * @entity_name: the singular or plural machine name
 *
 * Returns: (transfer none) (nullable): the name of the module that owns
 *   the type, or %NULL for a type no module claims
 */
const gchar *
venture_entity_registry_get_type_module(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_entity_registry_set_type_module:
 * @self: a #VentureEntityRegistry
 * @entity_name: the singular machine name
 * @module_name: (nullable): the owning module, or %NULL to disown
 * @enabled: whether the type is offered
 *
 * Stamps a type with its module and shows or hides it. Hidden types stay
 * registered -- their #GType, prototype and table are all still there --
 * but every lookup and listing skips them, which is how a disabled module
 * disappears from REST, the UI, the schema, the AI tools and the CLI in
 * one move. Called by venture_module_registry_apply(); a name that is not
 * registered is ignored.
 */
void
venture_entity_registry_set_type_module(
	VentureEntityRegistry	*self,
	const gchar		*entity_name,
	const gchar		*module_name,
	gboolean		 enabled
);

/**
 * venture_entity_registry_set_unknown_type_error:
 * @self: a #VentureEntityRegistry
 * @entity_name: (nullable): the name that was not found
 * @error: (out) (optional): return location for a #GError
 *
 * Sets the error every caller should raise for a type name that did not
 * resolve. It says which of two different things went wrong: the type
 * belongs to a module that is switched off, naming the setting, or no such
 * type exists, listing what does.
 */
void
venture_entity_registry_set_unknown_type_error(
	VentureEntityRegistry	 *self,
	const gchar		 *entity_name,
	GError			**error
);

/**
 * venture_entity_registry_create:
 * @self: a #VentureEntityRegistry
 * @entity_name: the entity name
 * @error: (out) (optional): return location for a #GError
 *
 * Instantiates a record of the named type.
 *
 * Returns: (transfer full) (nullable): a new entity, or %NULL if the name is
 *   unknown
 */
VentureEntity *
venture_entity_registry_create(
	VentureEntityRegistry	 *self,
	const gchar		 *entity_name,
	GError			**error
);

/**
 * venture_entity_registry_list_names:
 * @self: a #VentureEntityRegistry
 *
 * Lists every registered entity name, sorted, so that generated
 * documentation, tool descriptions and CLI help are deterministic.
 *
 * Returns: (transfer full) (array zero-terminated=1): the names
 */
gchar **
venture_entity_registry_list_names(VentureEntityRegistry *self);

/**
 * venture_entity_registry_list_types:
 * @self: a #VentureEntityRegistry
 * @n_types: (out): return location for the count
 *
 * Returns: (transfer container) (array length=n_types): the registered types,
 *   in the same order as venture_entity_registry_list_names()
 */
GType *
venture_entity_registry_list_types(
	VentureEntityRegistry	*self,
	guint			*n_types
);

/**
 * venture_entity_registry_get_prototype:
 * @self: a #VentureEntityRegistry
 * @entity_name: the entity name
 *
 * Retrieves a shared, never-saved instance of the named type. Introspecting
 * a type -- asking for its table name, its field specs, its display
 * behaviour -- requires an instance because those are virtual methods, and
 * constructing a throwaway object for every such question is wasteful when
 * the answer depends only on the class.
 *
 * The returned object must not be modified or saved.
 *
 * Returns: (transfer none) (nullable): the prototype, or %NULL if unknown
 */
VentureEntity *
venture_entity_registry_get_prototype(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_entity_registry_get_table_name:
 * @self: a #VentureEntityRegistry
 * @entity_name: the entity name
 *
 * Returns: (transfer none) (nullable): the database table for the named type
 */
const gchar *
venture_entity_registry_get_table_name(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_entity_registry_describe:
 * @self: a #VentureEntityRegistry
 * @entity_name: the entity name
 *
 * Produces a JSON description of a type: its name, table, and every field
 * with type, label, constraints and choices. This is what
 * `venturectl describe <type>`, the web UI's form renderer and the AI's
 * schema tool all consume.
 *
 * Returns: (transfer full) (nullable): the description
 */
JsonNode *
venture_entity_registry_describe(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_entity_registry_describe_all:
 * @self: a #VentureEntityRegistry
 *
 * Returns: (transfer full): a JSON array describing every registered type
 */
JsonNode *
venture_entity_registry_describe_all(VentureEntityRegistry *self);

/**
 * venture_entity_registry_register_builtins:
 * @self: a #VentureEntityRegistry
 *
 * Registers every entity type that ships with VENTURE. Called automatically
 * by venture_entity_registry_get_default(); exposed so a test can populate a
 * private registry.
 */
void
venture_entity_registry_register_builtins(VentureEntityRegistry *self);

G_END_DECLS

#endif /* VENTURE_ENTITY_REGISTRY_H */
