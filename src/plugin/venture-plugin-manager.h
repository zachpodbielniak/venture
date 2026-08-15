/*
 * venture-plugin-manager.h - Loading plugins and declarative types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three kinds of extension, one host:
 *
 *   - a compiled .so, loaded with GModule
 *   - a .c file, compiled on demand by crispy and cached by content hash
 *   - a .yaml venture type, which is data rather than code
 *
 * The first two share an entry point. A plugin exports:
 *
 * |[<!-- language="C" -->
 * gboolean venture_plugin_register (VentureContext *context, GError **error);
 * ]|
 *
 * and may optionally export venture_plugin_info() describing itself. Inside
 * that function it can register record types, reports, AI tools and venture
 * types -- all of which then behave exactly as built-ins do, because every
 * consumer works from the registries rather than from a hardcoded list.
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
 * Returns: (transfer full): a new manager
 */
VenturePluginManager *
venture_plugin_manager_new(VentureContext *context);

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
 * @path: a .so, a .c or a .yaml
 * @error: (out) (optional): return location for a #GError
 *
 * Loads one extension, dispatching on the extension of the filename.
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
 * Loads every extension in a directory. A plugin that fails to load is
 * reported and skipped: one broken plugin must not stop the server, because
 * the operator still needs the rest of their data.
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
 *   plugin's name, kind, path and description
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
 * venture_plugin_manager_get_count:
 * @self: a #VenturePluginManager
 *
 * Returns: how many plugins are loaded
 */
guint
venture_plugin_manager_get_count(VenturePluginManager *self);

G_END_DECLS

#endif /* VENTURE_PLUGIN_MANAGER_H */
