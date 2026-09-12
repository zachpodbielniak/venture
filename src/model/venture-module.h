/*
 * venture-module.h - Modules: the switchable pieces VENTURE is made of
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module is a named group of record types, reports, pages and services
 * that an install turns on or off as one thing: the CRM, invoicing, the
 * knowledge bases, the software factory. Every module names what it
 * requires, and the registry resolves those requirements once at startup
 * so that "invoicing is on but the CRM it bills against is off" is a
 * configuration error reported in a second, never a page that half works.
 *
 * Dependencies are declared as names and resolved by insertion order: a
 * module may only require modules registered before it. That is what makes
 * a cycle impossible by construction rather than merely detected, and it
 * is why the built-in table is written bottom-up and a plugin's module can
 * depend on anything built in but never the other way round.
 *
 * What a disabled module loses is decided in one place, here, and applied
 * to the registries the rest of the system already reads: its record types
 * are hidden from the entity registry, so REST, the UI, the schema, the AI
 * tools, the CLI and the MCP server stop offering them without any of them
 * knowing what a module is; its reports leave the report registry; and its
 * pages and services ask venture_module_registry_is_enabled() before doing
 * anything. Nothing is deleted. A table a disabled module created stays
 * where it is, and turning the module back on shows the same rows.
 */

#ifndef VENTURE_MODULE_H
#define VENTURE_MODULE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VentureModuleInfo:
 * @name: the machine name, `[a-z0-9_-]+`, used in configuration and URLs
 * @label: the human name shown in the UI
 * @description: one sentence on what the module is for
 * @requires: (nullable) (array zero-terminated=1): modules that must be
 *   enabled for this one to work; each must already be registered
 * @suggests: (nullable) (array zero-terminated=1): modules this one is
 *   better with but does not need -- a reference field whose picker is
 *   simply empty without them
 * @entity_types: (nullable) (array zero-terminated=1): the `get_type`
 *   functions of the record types this module owns
 * @reports: (nullable) (array zero-terminated=1): the names of the reports
 *   this module owns
 * @legacy_switch: (nullable): a configuration property that also turns this
 *   module off, kept so `ai.enabled: false` keeps meaning what it always
 *   did
 * @locked: %TRUE if the module cannot be disabled
 *
 * The static description of one module. A plugin declares one of these
 * and hands it to venture_module_registry_add(); the built-in table is a
 * static array of them.
 */
typedef struct _VentureModuleInfo
{
	const gchar		 *name;
	const gchar		 *label;
	const gchar		 *description;
	const gchar *const	 *requires;
	const gchar *const	 *suggests;
	GType			(*const *entity_types) (void);
	const gchar *const	 *reports;
	const gchar		 *legacy_switch;
	gboolean		  locked;
} VentureModuleInfo;

/**
 * VentureModuleOrigin:
 * @VENTURE_MODULE_ORIGIN_BUILTIN: shipped with VENTURE
 * @VENTURE_MODULE_ORIGIN_PLUGIN: declared by a loaded plugin
 *
 * Where a module came from.
 */
typedef enum
{
	VENTURE_MODULE_ORIGIN_BUILTIN = 0,
	VENTURE_MODULE_ORIGIN_PLUGIN
} VentureModuleOrigin;

#define VENTURE_TYPE_MODULE_ORIGIN (venture_module_origin_get_type())

GType
venture_module_origin_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_MODULE (venture_module_get_type())

G_DECLARE_FINAL_TYPE(VentureModule, venture_module, VENTURE, MODULE, GObject)

/**
 * venture_module_get_name:
 * @self: a #VentureModule
 *
 * Returns: (transfer none): the machine name
 */
const gchar *
venture_module_get_name(VentureModule *self);

/**
 * venture_module_get_label:
 * @self: a #VentureModule
 *
 * Returns: (transfer none): the human name
 */
const gchar *
venture_module_get_label(VentureModule *self);

/**
 * venture_module_get_description:
 * @self: a #VentureModule
 *
 * Returns: (transfer none): what the module is for
 */
const gchar *
venture_module_get_description(VentureModule *self);

/**
 * venture_module_get_requires:
 * @self: a #VentureModule
 *
 * Returns: (transfer none) (array zero-terminated=1): the hard dependencies
 */
const gchar *const *
venture_module_get_requires(VentureModule *self);

/**
 * venture_module_get_suggests:
 * @self: a #VentureModule
 *
 * Returns: (transfer none) (array zero-terminated=1): the soft dependencies
 */
const gchar *const *
venture_module_get_suggests(VentureModule *self);

/**
 * venture_module_get_entity_names:
 * @self: a #VentureModule
 *
 * Retrieves the entity names of the record types this module owns, in the
 * order they were declared. Resolved from the types themselves, so the
 * module table never has to spell a name twice.
 *
 * Returns: (transfer none) (array zero-terminated=1): the entity names
 */
const gchar *const *
venture_module_get_entity_names(VentureModule *self);

/**
 * venture_module_get_reports:
 * @self: a #VentureModule
 *
 * Returns: (transfer none) (array zero-terminated=1): the report names
 */
const gchar *const *
venture_module_get_reports(VentureModule *self);

/**
 * venture_module_get_legacy_switch:
 * @self: a #VentureModule
 *
 * Returns: (transfer none) (nullable): the configuration property that
 *   also switches the module, or %NULL
 */
const gchar *
venture_module_get_legacy_switch(VentureModule *self);

/**
 * venture_module_is_locked:
 * @self: a #VentureModule
 *
 * Returns: %TRUE if the module cannot be disabled
 */
gboolean
venture_module_is_locked(VentureModule *self);

/**
 * venture_module_is_enabled:
 * @self: a #VentureModule
 *
 * Retrieves the resolved state. Meaningful only after the registry has been
 * configured; before that every module reads as enabled.
 *
 * Returns: %TRUE if the module is on
 */
gboolean
venture_module_is_enabled(VentureModule *self);

/**
 * venture_module_get_disabled_reason:
 * @self: a #VentureModule
 *
 * Says why a module is off, in words an operator can act on: the setting
 * that turned it off, the legacy switch that did, or the required module
 * that is itself off.
 *
 * Returns: (transfer none) (nullable): the reason, or %NULL when enabled
 */
const gchar *
venture_module_get_disabled_reason(VentureModule *self);

/**
 * venture_module_get_origin:
 * @self: a #VentureModule
 *
 * Returns: where the module came from
 */
VentureModuleOrigin
venture_module_get_origin(VentureModule *self);

/**
 * venture_module_to_json:
 * @self: a #VentureModule
 *
 * Describes the module: name, label, description, dependencies, the record
 * types and reports it owns, whether it is locked, and its resolved state
 * with the reason if it is off.
 *
 * Returns: (transfer full): a JSON object
 */
JsonNode *
venture_module_to_json(VentureModule *self);

#define VENTURE_TYPE_MODULE_REGISTRY (venture_module_registry_get_type())

G_DECLARE_FINAL_TYPE(VentureModuleRegistry, venture_module_registry,
                     VENTURE, MODULE_REGISTRY, GObject)

/**
 * venture_module_registry_new:
 *
 * Creates an empty registry. The context creates one, registers the
 * built-in modules and configures it; a test may build a smaller one.
 *
 * Returns: (transfer full): a new #VentureModuleRegistry
 */
VentureModuleRegistry *
venture_module_registry_new(void);

/**
 * venture_module_registry_register_builtins:
 * @self: a #VentureModuleRegistry
 *
 * Registers every module VENTURE ships with, in dependency order. Calling
 * it twice is harmless.
 */
void
venture_module_registry_register_builtins(VentureModuleRegistry *self);

/**
 * venture_module_registry_add:
 * @self: a #VentureModuleRegistry
 * @info: the module's description; the strings and arrays it points at
 *   must outlive the registry, which for a static table they do
 * @origin: where it came from
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a module. Refused when the name is malformed, already taken,
 * or names a requirement that is not registered yet -- dependencies must
 * be added first, which is what rules a cycle out.
 *
 * If the registry has already been configured, the new module is resolved
 * against that configuration immediately, so a plugin's module reads its
 * switch the same way a built-in one does.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_module_registry_add(
	VentureModuleRegistry		 *self,
	const VentureModuleInfo		 *info,
	VentureModuleOrigin		  origin,
	GError				**error
);

/**
 * venture_module_registry_configure:
 * @self: a #VentureModuleRegistry
 * @config: the configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves every module's state from the configuration: the `modules`
 * section first, then each module's legacy switch, then dependencies.
 *
 * A module whose requirement is off is itself marked off, with a reason
 * naming the requirement, and the call returns %FALSE with an error that
 * says so for every such module at once. The state is still fully
 * resolved on failure -- nothing is left half on -- so a caller may choose
 * to carry on, but the server treats the error as fatal because the
 * operator asked for a combination that cannot work and should be told.
 *
 * Returns: %TRUE if every enabled module has what it needs
 */
gboolean
venture_module_registry_configure(
	VentureModuleRegistry	 *self,
	VentureConfig		 *config,
	GError			**error
);

/**
 * venture_module_registry_check_configured:
 * @self: a #VentureModuleRegistry
 * @config: the configuration
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses a `modules` section that names a module nobody registered. Run
 * after plugins have loaded, because a plugin's module is configured the
 * same way and is not known before then. A misspelt module name would
 * otherwise switch nothing and say nothing.
 *
 * Returns: %TRUE if every configured name is a module
 */
gboolean
venture_module_registry_check_configured(
	VentureModuleRegistry	 *self,
	VentureConfig		 *config,
	GError			**error
);

/**
 * venture_module_registry_apply:
 * @self: a #VentureModuleRegistry
 * @entities: the entity registry to mask
 *
 * Hides the record types of every disabled module from @entities and shows
 * the rest, stamping each with the module that owns it. Every type a module
 * claims is set, enabled or not, so applying a second configuration to the
 * same process-wide registry leaves nothing over from the first.
 */
void
venture_module_registry_apply(
	VentureModuleRegistry	*self,
	VentureEntityRegistry	*entities
);

/**
 * venture_module_registry_lookup:
 * @self: a #VentureModuleRegistry
 * @name: a module name
 *
 * Returns: (transfer none) (nullable): the module, or %NULL if unknown
 */
VentureModule *
venture_module_registry_lookup(
	VentureModuleRegistry	*self,
	const gchar		*name
);

/**
 * venture_module_registry_is_enabled:
 * @self: a #VentureModuleRegistry
 * @name: a module name
 *
 * The question every gated page and service asks. An unknown name is
 * disabled: a route guarded by a module that does not exist should not
 * open just because the check found nothing.
 *
 * Returns: %TRUE if the module exists and is on
 */
gboolean
venture_module_registry_is_enabled(
	VentureModuleRegistry	*self,
	const gchar		*name
);

/**
 * venture_module_registry_list:
 * @self: a #VentureModuleRegistry
 *
 * Lists every module in dependency order, which is also registration
 * order: a module always follows what it requires.
 *
 * Returns: (transfer container) (element-type VentureModule): the modules
 */
GPtrArray *
venture_module_registry_list(VentureModuleRegistry *self);

/**
 * venture_module_registry_list_names:
 * @self: a #VentureModuleRegistry
 *
 * Returns: (transfer full) (array zero-terminated=1): the module names, in
 *   dependency order
 */
gchar **
venture_module_registry_list_names(VentureModuleRegistry *self);

/**
 * venture_module_registry_get_dependents:
 * @self: a #VentureModuleRegistry
 * @name: a module name
 *
 * Lists the modules that require @name, directly. This is what the
 * Modules page shows as "needed by", and what the error for a bad
 * configuration draws on.
 *
 * Returns: (transfer full) (array zero-terminated=1): the dependent names
 */
gchar **
venture_module_registry_get_dependents(
	VentureModuleRegistry	*self,
	const gchar		*name
);

/**
 * venture_module_registry_get_module_for_type:
 * @self: a #VentureModuleRegistry
 * @entity_name: a record type's entity name
 *
 * Returns: (transfer none) (nullable): the module owning that type, or
 *   %NULL for a type no module claims -- a plugin's, usually
 */
VentureModule *
venture_module_registry_get_module_for_type(
	VentureModuleRegistry	*self,
	const gchar		*entity_name
);

/**
 * venture_module_registry_describe:
 * @self: a #VentureModuleRegistry
 *
 * Returns: (transfer full): a JSON array describing every module, in
 *   dependency order
 */
JsonNode *
venture_module_registry_describe(VentureModuleRegistry *self);

/**
 * venture_module_registry_get_builtin_infos:
 * @n_infos: (out): return location for the count
 *
 * Retrieves the static table of built-in modules, for anything that needs
 * to describe them without a registry -- the generated configuration
 * comments, for one.
 *
 * Returns: (transfer none) (array length=n_infos): the table
 */
const VentureModuleInfo *
venture_module_registry_get_builtin_infos(gsize *n_infos);

G_END_DECLS

#endif /* VENTURE_MODULE_H */
