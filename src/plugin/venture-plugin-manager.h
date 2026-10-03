/*
 * venture-plugin-manager.h - Loading plugins and declarative types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every extension is loaded through a runtime (venture-plugin-runtime.h).
 * Four ship:
 *
 *   - native: a compiled .so, loaded with GModule
 *   - crispy: a .c file, compiled on demand and cached by content hash
 *   - declarative: a .yaml venture type, which is data rather than code
 *   - exec: a program run as a subprocess, named by a manifest
 *
 * and a plugin may register more. A bare file is loaded by the runtime that
 * claims its extension; a `*.plugin.yaml` manifest names its runtime and its
 * entry. A file no runtime claims yet is held, and loaded the moment a
 * runtime for it is registered.
 *
 * native and crispy share an entry point. A plugin exports:
 *
 * |[<!-- language="C" -->
 * gboolean venture_plugin_register (VentureContext *context, GError **error);
 * ]|
 *
 * and may optionally export venture_plugin_info() describing itself. Inside
 * that function it can register record types, reports, AI tools, venture
 * types and runtimes -- all of which then behave exactly as built-ins do,
 * because every consumer works from the registries rather than from a
 * hardcoded list.
 *
 * A loaded module is never unloaded. It may have registered a GType, and
 * pulling the code out from under a live type crashes on next use.
 */

#ifndef VENTURE_PLUGIN_MANAGER_H
#define VENTURE_PLUGIN_MANAGER_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <gmodule.h>

G_BEGIN_DECLS

/**
 * VenturePluginRegisterFunc:
 * @context: the wiring, giving access to every registry
 * @error: (out) (optional): return location for a #GError
 *
 * The symbol every plugin must export as `venture_plugin_register`.
 *
 * Returns: %TRUE if the plugin registered successfully
 */
typedef gboolean (*VenturePluginRegisterFunc) (
	VentureContext	 *context,
	GError		**error
);

/**
 * VenturePluginDescribeFunc:
 *
 * The optional symbol `venture_plugin_info`, returning a one-line
 * description of the plugin for the plugin list.
 *
 * Returns: (transfer none): a description
 */
typedef const gchar * (*VenturePluginDescribeFunc) (void);

#define VENTURE_TYPE_PLUGIN_MANAGER (venture_plugin_manager_get_type())

G_DECLARE_FINAL_TYPE(VenturePluginManager, venture_plugin_manager,
                     VENTURE, PLUGIN_MANAGER, GObject)

/**
 * venture_plugin_manager_new:
 * @context: the wiring handed to each plugin
 *
 * Creates a manager with the four built-in runtimes registered. Set it on
 * the context with venture_context_set_plugin_manager() *before* loading
 * anything: a plugin finds the manager -- to read its configuration or add
 * a runtime -- through the context, from inside its own registration.
 *
 * Returns: (transfer full): a new manager
 */
VenturePluginManager *
venture_plugin_manager_new(VentureContext *context);

/**
 * venture_plugin_manager_get_context:
 * @self: a #VenturePluginManager
 *
 * Returns: (transfer none): the wiring plugins register into
 */
VentureContext *
venture_plugin_manager_get_context(VenturePluginManager *self);

/**
 * venture_plugin_manager_add_runtime:
 * @self: a #VenturePluginManager
 * @runtime: the runtime to add
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a runtime, typically from a plugin's own registration. A name
 * already registered, or an extension another runtime claims, is refused;
 * so is `.plugin.yaml`, which belongs to manifests.
 *
 * Every file held for want of a runtime is tried again, in the order it was
 * met -- once the plugin registering this runtime has finished loading,
 * never in the middle of it.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_plugin_manager_add_runtime(
	VenturePluginManager	 *self,
	VenturePluginRuntime	 *runtime,
	GError			**error
);

/**
 * venture_plugin_manager_lookup_runtime:
 * @self: a #VenturePluginManager
 * @name: (nullable): a runtime's name
 *
 * Returns: (transfer none) (nullable): the runtime, or %NULL
 */
VenturePluginRuntime *
venture_plugin_manager_lookup_runtime(
	VenturePluginManager	*self,
	const gchar		*name
);

/**
 * venture_plugin_manager_list_runtimes:
 * @self: a #VenturePluginManager
 *
 * Returns: (transfer full) (array zero-terminated=1): the runtimes' names,
 *   in registration order
 */
gchar **
venture_plugin_manager_list_runtimes(VenturePluginManager *self);

/**
 * venture_plugin_manager_list_deferred:
 * @self: a #VenturePluginManager
 *
 * Returns: (transfer full) (array zero-terminated=1): the files held until
 *   a runtime for them is registered, in the order they were met
 */
gchar **
venture_plugin_manager_list_deferred(VenturePluginManager *self);

/**
 * venture_plugin_manager_get_venture_types:
 * @self: a #VenturePluginManager
 *
 * Returns: (transfer none): the declarative venture types loaded so far
 */
VentureVentureTypeRegistry *
venture_plugin_manager_get_venture_types(VenturePluginManager *self);

/**
 * venture_plugin_manager_load_file:
 * @self: a #VenturePluginManager
 * @path: a manifest, or a file some runtime's extension claims
 * @error: (out) (optional): return location for a #GError
 *
 * Loads one extension. A `*.plugin.yaml` is read as a manifest before any
 * extension is considered; anything else goes to the runtime claiming its
 * extension. A file no runtime claims, or a manifest naming a runtime not
 * registered yet, is refused with %VENTURE_ERROR_PLUGIN *and* held: it
 * loads if such a runtime is added later.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_plugin_manager_load_file(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
);

/**
 * venture_plugin_manager_load_directory:
 * @self: a #VenturePluginManager
 * @path: a directory to scan
 * @error: (out) (optional): return location for a #GError
 *
 * Loads every extension in a directory, in sorted order. A subdirectory is
 * a plugin's own directory: its `*.plugin.yaml` manifests are loaded and
 * nothing else in it is, and nothing deeper is scanned. A plugin that fails
 * to load is reported and skipped: one broken plugin must not stop the
 * server, because the operator still needs the rest of their data. A file
 * no runtime claims is held quietly.
 *
 * A directory that does not exist is not an error.
 *
 * Returns: the number of extensions loaded
 */
guint
venture_plugin_manager_load_directory(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
);

/**
 * venture_plugin_manager_load_configured:
 * @self: a #VenturePluginManager
 * @error: (out) (optional): return location for a #GError
 *
 * Loads everything configuration points at: the built-in plugin directory,
 * the paths in `plugins.paths`, and the venture-type directories.
 *
 * A plugin named in `plugins.required` that fails to load IS fatal, which is
 * the point of that list -- an install that depends on a plugin should not
 * start half-configured and quietly behave differently.
 *
 * Returns: %TRUE if every required plugin loaded
 */
gboolean
venture_plugin_manager_load_configured(
	VenturePluginManager	 *self,
	GError			**error
);

/**
 * venture_plugin_manager_list:
 * @self: a #VenturePluginManager
 *
 * Returns: (transfer full): a JSON array of what is loaded, with each
 *   plugin's name, kind, runtime, path and description, and the kinds it
 *   provides when its manifest lists any
 */
JsonNode *
venture_plugin_manager_list(VenturePluginManager *self);

/**
 * venture_plugin_manager_get_config_text:
 * @self: a #VenturePluginManager
 * @plugin_name: which plugin's configuration
 *
 * The stored YAML, verbatim -- what the configuration editor shows.
 *
 * Returns: (transfer full) (nullable): the YAML text, or %NULL if none is
 *   stored
 */
gchar *
venture_plugin_manager_get_config_text(
	VenturePluginManager	*self,
	const gchar		*plugin_name
);

/**
 * venture_plugin_manager_get_config:
 * @self: a #VenturePluginManager
 * @plugin_name: which plugin's configuration
 *
 * The stored configuration, parsed. This is what a plugin reads: call it
 * once at registration, and again from a handler connected to
 * #VenturePluginManager::config-changed, and the plugin follows the
 * settings page without a restart.
 *
 * Returns: (transfer full) (nullable): the configuration as JSON, or %NULL
 *   if none is stored
 */
JsonNode *
venture_plugin_manager_get_config(
	VenturePluginManager	*self,
	const gchar		*plugin_name
);

/**
 * venture_plugin_manager_set_config:
 * @self: a #VenturePluginManager
 * @plugin_name: which plugin's configuration
 * @yaml: the new configuration, as YAML
 * @actor: (nullable): who changed it, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Validates that @yaml parses, stores it on the plugin's configuration
 * record, and emits #VenturePluginManager::config-changed so a running
 * plugin can pick the change up immediately. Storing YAML that does not
 * parse is refused outright -- a plugin must never be handed garbage it
 * then has to defend against.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_plugin_manager_set_config(
	VenturePluginManager	 *self,
	const gchar		 *plugin_name,
	const gchar		 *yaml,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_plugin_config_get_string:
 * @config: (nullable): a configuration from
 *   venture_plugin_manager_get_config()
 * @key: a top-level key
 * @fallback: (nullable): returned when the key is absent
 *
 * A convenience for plugin authors: one flat string setting, with a
 * default, no JSON spelunking.
 *
 * Returns: (transfer full) (nullable): the value, or a copy of @fallback
 */
gchar *
venture_plugin_config_get_string(
	JsonNode	*config,
	const gchar	*key,
	const gchar	*fallback
);

/**
 * venture_plugin_config_get_double:
 * @config: (nullable): a configuration from
 *   venture_plugin_manager_get_config()
 * @key: a top-level key
 * @fallback: returned when the key is absent or not a number
 *
 * Returns: the value, or @fallback
 */
gdouble
venture_plugin_config_get_double(
	JsonNode	*config,
	const gchar	*key,
	gdouble		 fallback
);

/**
 * venture_plugin_manager_add_exec:
 * @self: a #VenturePluginManager
 * @spec: an exec plugin's program
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a program under its spec's name. Called by the exec runtime's
 * load; a name already registered is refused.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_plugin_manager_add_exec(
	VenturePluginManager	 *self,
	VentureExecSpec		 *spec,
	GError			**error
);

/**
 * venture_plugin_manager_lookup_exec:
 * @self: a #VenturePluginManager
 * @name: (nullable): an exec plugin's name
 *
 * The frozen description of a loaded exec plugin's program. Take a
 * reference to hand it to a worker thread: the spec is read-only and
 * venture_exec_run() touches neither the database nor the configuration.
 *
 * Returns: (transfer none) (nullable): the spec, or %NULL
 */
VentureExecSpec *
venture_plugin_manager_lookup_exec(
	VenturePluginManager	*self,
	const gchar		*name
);

/**
 * venture_plugin_manager_build_exec_request:
 * @self: a #VenturePluginManager
 * @name: an exec plugin's name
 * @command: (nullable): what to ask it to do; %NULL is "run"
 * @params: (nullable): the command's parameters
 *
 * Builds the request an exec plugin reads on its standard input:
 * `plugin`, `command`, `params`, and `settings` -- the plugin's stored
 * configuration, read from the database now. Main thread only, for that
 * reason; a worker is handed the finished object. The run adds `protocol`
 * and `secrets`.
 *
 * Returns: (transfer full): the request
 */
JsonObject *
venture_plugin_manager_build_exec_request(
	VenturePluginManager	*self,
	const gchar		*name,
	const gchar		*command,
	JsonObject		*params
);

/**
 * venture_plugin_manager_run_exec:
 * @self: a #VenturePluginManager
 * @name: an exec plugin's name
 * @command: (nullable): what to ask it to do; %NULL is "run"
 * @params: (nullable): the command's parameters
 * @secrets: (nullable): secrets for this run, on standard input only
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the request and runs the plugin, on the calling thread. Refuses
 * when `plugins.allow_exec` is off *now*, not only when it was off at
 * load, so switching it off stops the programs without a restart.
 *
 * Returns: (transfer full) (nullable): the result; see venture_exec_run()
 */
VentureExecResult *
venture_plugin_manager_run_exec(
	VenturePluginManager	 *self,
	const gchar		 *name,
	const gchar		 *command,
	JsonObject		 *params,
	JsonObject		 *secrets,
	GCancellable		 *cancellable,
	GError			**error
);

/**
 * venture_plugin_manager_get_count:
 * @self: a #VenturePluginManager
 *
 * Returns: how many plugins are loaded
 */
guint
venture_plugin_manager_get_count(VenturePluginManager *self);

G_END_DECLS

#endif /* VENTURE_PLUGIN_MANAGER_H */
