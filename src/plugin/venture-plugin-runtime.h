/*
 * venture-plugin-runtime.h - What loads a plugin, and what a plugin offers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A runtime turns a file into a loaded plugin. Four ship -- `native`
 * (a .so through GModule), `crispy` (a .c compiled on demand), `declarative`
 * (a .yaml venture type) and `exec` (a program run as a subprocess) -- and
 * a plugin may register more: an embedded interpreter is a runtime that
 * claims an extension.
 *
 * A file is either bare -- its extension picks the runtime -- or named by a
 * manifest, `<name>.plugin.yaml`, which says which runtime loads it and
 * gives that runtime its settings. A manifest is the only way to load an
 * exec plugin, because a program has no extension to recognise it by.
 *
 * A manifest may also list what the plugin *provides*. Each entry names a
 * kind, and the kind's handler -- registered on the context by whatever
 * subsystem understands it -- is handed the entry. The runtime never needs
 * to know what a data-source provider or any later kind is, which is what
 * lets those kinds arrive without touching it.
 */

#ifndef VENTURE_PLUGIN_RUNTIME_H
#define VENTURE_PLUGIN_RUNTIME_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_PLUGIN_MANIFEST_SUFFIX:
 *
 * A file whose name ends in this is a manifest, read before any runtime
 * is asked about its extension -- it is also a .yaml, and the declarative
 * runtime would otherwise take it for a venture type.
 */
#define VENTURE_PLUGIN_MANIFEST_SUFFIX ".plugin.yaml"

/* ==========================================================================
 * The record a load fills in
 * ========================================================================== */

typedef struct _VenturePluginRecord VenturePluginRecord;

/**
 * venture_plugin_record_get_name:
 * @self: a record
 *
 * Returns: (transfer none): the plugin's name -- the manifest's `name`, or
 *   a bare file's basename
 */
const gchar *
venture_plugin_record_get_name(VenturePluginRecord *self);

/**
 * venture_plugin_record_get_path:
 * @self: a record
 *
 * Returns: (transfer none): the manifest, or the bare file
 */
const gchar *
venture_plugin_record_get_path(VenturePluginRecord *self);

/**
 * venture_plugin_record_get_runtime:
 * @self: a record
 *
 * Returns: (transfer none): the name of the runtime that loaded it
 */
const gchar *
venture_plugin_record_get_runtime(VenturePluginRecord *self);

/**
 * venture_plugin_record_get_kind:
 * @self: a record
 *
 * Returns: how the plugin was supplied
 */
VenturePluginKind
venture_plugin_record_get_kind(VenturePluginRecord *self);

/**
 * venture_plugin_record_set_kind:
 * @self: a record
 * @kind: how the plugin was supplied
 *
 * Called by a runtime's load. A runtime that does not call it reports
 * %VENTURE_PLUGIN_KIND_OTHER.
 */
void
venture_plugin_record_set_kind(
	VenturePluginRecord	*self,
	VenturePluginKind	 kind
);

/**
 * venture_plugin_record_get_description:
 * @self: a record
 *
 * Returns: (transfer none) (nullable): a one-line description
 */
const gchar *
venture_plugin_record_get_description(VenturePluginRecord *self);

/**
 * venture_plugin_record_set_description:
 * @self: a record
 * @description: (nullable): a one-line description
 *
 * Called by a runtime's load when the plugin describes itself. A
 * manifest's own `description` is used when the plugin gives none.
 */
void
venture_plugin_record_set_description(
	VenturePluginRecord	*self,
	const gchar		*description
);

/* ==========================================================================
 * Manifests
 * ========================================================================== */

typedef struct _VenturePluginManifest VenturePluginManifest;

#define VENTURE_TYPE_PLUGIN_MANIFEST (venture_plugin_manifest_get_type())

GType venture_plugin_manifest_get_type(void) G_GNUC_CONST;

/**
 * venture_plugin_manifest_new_from_file:
 * @path: a `*.plugin.yaml`
 * @error: (out) (optional): return location for a #GError
 *
 * Reads and checks a manifest: `name` (lower case, digits, `.`, `_`, `-`,
 * up to 64 bytes), `entry` (resolved with realpath() and refused unless it
 * is inside the manifest's directory), and the optional `runtime`,
 * `description`, `protocol` and `provides`. Which other top-level keys are
 * allowed depends on the runtime, so the manager checks those once the
 * runtime is known.
 *
 * Returns: (transfer full) (nullable): the manifest, or %NULL on error
 */
VenturePluginManifest *
venture_plugin_manifest_new_from_file(
	const gchar	 *path,
	GError		**error
);

/**
 * venture_plugin_manifest_ref:
 * @self: a manifest
 *
 * Returns: (transfer full): @self
 */
VenturePluginManifest *
venture_plugin_manifest_ref(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_unref:
 * @self: (transfer full): a manifest
 */
void
venture_plugin_manifest_unref(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_name:
 * @self: a manifest
 *
 * Returns: (transfer none): the plugin's name
 */
const gchar *
venture_plugin_manifest_get_name(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_runtime:
 * @self: a manifest
 *
 * Returns: (transfer none) (nullable): the runtime it asks for, or %NULL
 *   to have the entry's extension pick one
 */
const gchar *
venture_plugin_manifest_get_runtime(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_description:
 * @self: a manifest
 *
 * Returns: (transfer none) (nullable): its description
 */
const gchar *
venture_plugin_manifest_get_description(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_path:
 * @self: a manifest
 *
 * Returns: (transfer none): the manifest file
 */
const gchar *
venture_plugin_manifest_get_path(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_root:
 * @self: a manifest
 *
 * Returns: (transfer none): its directory, resolved with realpath()
 */
const gchar *
venture_plugin_manifest_get_root(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_entry:
 * @self: a manifest
 *
 * Returns: (transfer none): the file it names, resolved with realpath()
 *   and inside venture_plugin_manifest_get_root()
 */
const gchar *
venture_plugin_manifest_get_entry(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_protocol:
 * @self: a manifest
 *
 * Returns: the declared `protocol`, or 0 when it declares none
 */
gint64
venture_plugin_manifest_get_protocol(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_object:
 * @self: a manifest
 *
 * Returns: (transfer none): the whole manifest, as JSON
 */
JsonObject *
venture_plugin_manifest_get_object(VenturePluginManifest *self);

/**
 * venture_plugin_manifest_get_section:
 * @self: a manifest
 * @name: a top-level key, usually the runtime's name
 *
 * Returns: (transfer none) (nullable): that key's object, or %NULL when it
 *   is absent
 */
JsonObject *
venture_plugin_manifest_get_section(
	VenturePluginManifest	*self,
	const gchar		*name
);

/**
 * venture_plugin_manifest_get_provides:
 * @self: a manifest
 *
 * Returns: (transfer none) (nullable): the `provides` list, each element
 *   an object with at least a `kind`
 */
JsonArray *
venture_plugin_manifest_get_provides(VenturePluginManifest *self);

/* ==========================================================================
 * The runtime interface
 * ========================================================================== */

#define VENTURE_TYPE_PLUGIN_RUNTIME (venture_plugin_runtime_get_type())

G_DECLARE_INTERFACE(VenturePluginRuntime, venture_plugin_runtime,
                    VENTURE, PLUGIN_RUNTIME, GObject)

/**
 * VenturePluginRuntimeInterface:
 * @parent_iface: the parent interface
 * @get_name: the runtime's name, as a manifest's `runtime` names it
 * @get_extensions: the bare-file suffixes it loads, such as ".so"; may be
 *   empty, as exec's is
 * @load: loads one plugin; @manifest is %NULL for a bare file, and @path
 *   is the file to load either way
 *
 * A load runs on the main thread, during startup or when a runtime is
 * added later, and may register anything the context offers. It fills
 * @record's kind and description; the manager has already set its name,
 * path and runtime.
 */
struct _VenturePluginRuntimeInterface
{
	GTypeInterface parent_iface;

	const gchar *		(*get_name)		(VenturePluginRuntime	 *self);
	const gchar *const *	(*get_extensions)	(VenturePluginRuntime	 *self);
	gboolean		(*load)			(VenturePluginRuntime	 *self,
							 VenturePluginManager	 *manager,
							 VentureContext		 *context,
							 const gchar		 *path,
							 VenturePluginManifest	 *manifest,
							 VenturePluginRecord	 *record,
							 GError			**error);

	/*< private >*/
	gpointer padding[8];
};

/**
 * venture_plugin_runtime_get_name:
 * @self: a runtime
 *
 * Returns: (transfer none): its name
 */
const gchar *
venture_plugin_runtime_get_name(VenturePluginRuntime *self);

/**
 * venture_plugin_runtime_get_extensions:
 * @self: a runtime
 *
 * Returns: (transfer none) (array zero-terminated=1): the suffixes it loads
 */
const gchar *const *
venture_plugin_runtime_get_extensions(VenturePluginRuntime *self);

/**
 * venture_plugin_runtime_load:
 * @self: a runtime
 * @manager: the manager loading it
 * @context: the wiring the plugin registers into
 * @path: the file to load
 * @manifest: (nullable): the manifest that named it
 * @record: the record to fill
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the plugin loaded
 */
gboolean
venture_plugin_runtime_load(
	VenturePluginRuntime	 *self,
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	GError			**error
);

/**
 * VenturePluginRuntimeLoadFunc:
 * @manager: the manager loading it
 * @context: the wiring
 * @path: the file to load
 * @manifest: (nullable): the manifest that named it
 * @record: the record to fill
 * @user_data: the data given to venture_func_plugin_runtime_new()
 * @error: (out) (optional): return location for a #GError
 *
 * The load of a function-backed runtime.
 *
 * Returns: %TRUE if the plugin loaded
 */
typedef gboolean (*VenturePluginRuntimeLoadFunc) (
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
);

/**
 * venture_func_plugin_runtime_new:
 * @name: the runtime's name, lower case
 * @extensions: (array zero-terminated=1) (nullable): the bare-file
 *   suffixes it loads, each starting with "."
 * @load: what loading does
 * @user_data: (closure): handed to @load
 * @destroy: (nullable): frees @user_data with the runtime
 *
 * A runtime without the GObject boilerplate, for a crispy script or a
 * test.
 *
 * Returns: (transfer full): a new runtime
 */
VenturePluginRuntime *
venture_func_plugin_runtime_new(
	const gchar			*name,
	const gchar *const		*extensions,
	VenturePluginRuntimeLoadFunc	 load,
	gpointer			 user_data,
	GDestroyNotify			 destroy
);

/**
 * venture_plugin_runtime_native_new:
 *
 * The runtime for a compiled `.so`, loaded with GModule and never unloaded.
 *
 * Returns: (transfer full): a new runtime
 */
VenturePluginRuntime *
venture_plugin_runtime_native_new(void);

/**
 * venture_plugin_runtime_crispy_new:
 *
 * The runtime for a `.c`, compiled on demand by crispy and cached by
 * content hash under `<state_dir>/crispy-cache`. Refuses unless
 * `plugins.allow_crispy` is on.
 *
 * Returns: (transfer full): a new runtime
 */
VenturePluginRuntime *
venture_plugin_runtime_crispy_new(void);

/**
 * venture_plugin_runtime_declarative_new:
 *
 * The runtime for a `.yaml` or `.yml` declarative venture type.
 *
 * Returns: (transfer full): a new runtime
 */
VenturePluginRuntime *
venture_plugin_runtime_declarative_new(void);

/**
 * venture_plugin_runtime_exec_new:
 *
 * The runtime for an exec plugin: a program named by a manifest with
 * `runtime: exec` and `protocol: 1`. Loading checks the program and
 * registers it with the manager; nothing runs until something it provides
 * is used. Refuses unless `plugins.allow_exec` is on.
 *
 * Returns: (transfer full): a new runtime
 */
VenturePluginRuntime *
venture_plugin_runtime_exec_new(void);

/* ==========================================================================
 * What a plugin provides
 * ========================================================================== */

/**
 * VenturePluginProvidesFunc:
 * @manager: the manager loading the plugin
 * @manifest: the plugin's manifest
 * @entry: one element of its `provides` list
 * @user_data: the data the kind was registered with
 * @error: (out) (optional): return location for a #GError
 *
 * Takes one `provides` entry and registers whatever it describes. The
 * plugin is already loaded -- an exec plugin's program can be looked up
 * with venture_plugin_manager_lookup_exec() -- and a refusal here fails
 * the plugin's load.
 *
 * Returns: %TRUE if the entry was accepted
 */
typedef gboolean (*VenturePluginProvidesFunc) (
	VenturePluginManager	 *manager,
	VenturePluginManifest	 *manifest,
	JsonObject		 *entry,
	gpointer		  user_data,
	GError			**error
);

/**
 * VenturePluginProvidesValidateFunc:
 * @manager: the manager loading the plugin
 * @manifest: the plugin's manifest
 * @entry: one element of its `provides` list
 * @user_data: the data the kind was registered with
 * @error: (out) (optional): return location for a #GError
 *
 * Judges one entry without registering anything: whether the kind's
 * #VenturePluginProvidesFunc would accept it now. Every entry of a
 * manifest is judged before any is registered, so a plugin whose third
 * entry is malformed registers nothing at all.
 *
 * Returns: %TRUE if the entry would be accepted
 */
typedef gboolean (*VenturePluginProvidesValidateFunc) (
	VenturePluginManager	 *manager,
	VenturePluginManifest	 *manifest,
	JsonObject		 *entry,
	gpointer		  user_data,
	GError			**error
);

/**
 * VenturePluginProvidesRemoveFunc:
 * @manager: the manager loading the plugin
 * @manifest: the plugin's manifest
 * @entry: an entry this kind accepted
 * @user_data: the data the kind was registered with
 *
 * Takes back what accepting @entry registered, when a later entry of the
 * same manifest (or a later step of the load) is refused after all.
 */
typedef void (*VenturePluginProvidesRemoveFunc) (
	VenturePluginManager	*manager,
	VenturePluginManifest	*manifest,
	JsonObject		*entry,
	gpointer		 user_data
);

#define VENTURE_TYPE_PLUGIN_PROVIDES_REGISTRY \
	(venture_plugin_provides_registry_get_type())

G_DECLARE_FINAL_TYPE(VenturePluginProvidesRegistry,
                     venture_plugin_provides_registry,
                     VENTURE, PLUGIN_PROVIDES_REGISTRY, GObject)

/**
 * venture_plugin_provides_registry_new:
 *
 * Returns: (transfer full): an empty registry
 */
VenturePluginProvidesRegistry *
venture_plugin_provides_registry_new(void);

/**
 * venture_plugin_provides_registry_add:
 * @self: a registry
 * @kind: the name a `provides` entry uses, lower case
 * @description: (nullable): a line for the documentation and the list
 * @func: what accepting an entry does
 * @user_data: (closure): handed to @func
 * @destroy: (nullable): frees @user_data with the registry
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a kind. A kind already registered is refused: two handlers for
 * one name would each believe they own the plugins that use it.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_plugin_provides_registry_add(
	VenturePluginProvidesRegistry	 *self,
	const gchar			 *kind,
	const gchar			 *description,
	VenturePluginProvidesFunc	  func,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
);

/**
 * venture_plugin_provides_registry_add_full:
 * @self: a registry
 * @kind: the name a `provides` entry uses, lower case
 * @description: (nullable): a line for the documentation and the list
 * @validate: (nullable): judges an entry without registering it
 * @func: what accepting an entry does
 * @remove: (nullable): takes back what @func registered
 * @user_data: (closure): handed to @validate, @func and @remove
 * @destroy: (nullable): frees @user_data with the registry
 * @error: (out) (optional): return location for a #GError
 *
 * venture_plugin_provides_registry_add() with the two halves that make a
 * manifest all or nothing: @validate runs over every entry before any
 * @func does, and @remove undoes the accepted entries when a later one is
 * refused anyway (two entries naming the same provider, say).
 *
 * Returns: %TRUE on success
 */
gboolean
venture_plugin_provides_registry_add_full(
	VenturePluginProvidesRegistry	 *self,
	const gchar			 *kind,
	const gchar			 *description,
	VenturePluginProvidesValidateFunc validate,
	VenturePluginProvidesFunc	  func,
	VenturePluginProvidesRemoveFunc	  remove,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
);

/**
 * venture_plugin_provides_registry_remove:
 * @self: a registry
 * @kind: a kind
 *
 * Takes a kind back out (a plugin that registered one and then failed to
 * load). What plugins already registered through it stays.
 *
 * Returns: %TRUE when there was one to remove
 */
gboolean
venture_plugin_provides_registry_remove(
	VenturePluginProvidesRegistry	*self,
	const gchar			*kind
);

/**
 * venture_plugin_provides_registry_has:
 * @self: a registry
 * @kind: a kind
 *
 * Returns: %TRUE if @kind is registered
 */
gboolean
venture_plugin_provides_registry_has(
	VenturePluginProvidesRegistry	*self,
	const gchar			*kind
);

/**
 * venture_plugin_provides_registry_list:
 * @self: a registry
 *
 * Returns: (transfer full) (array zero-terminated=1): the kinds, sorted
 */
gchar **
venture_plugin_provides_registry_list(VenturePluginProvidesRegistry *self);

/**
 * venture_plugin_provides_registry_dispatch:
 * @self: a registry
 * @manager: the manager loading the plugin
 * @manifest: the plugin's manifest
 * @error: (out) (optional): return location for a #GError
 *
 * Hands every entry of the manifest's `provides` list to its kind, in
 * order, all or nothing. First every entry is judged -- its kind must be
 * registered (a typo in a manifest must not load a plugin that silently
 * provides nothing) and the kind's validate function must accept it --
 * and only then is any registered. An entry refused while registering
 * takes back the ones before it through their kinds' remove functions.
 *
 * Returns: %TRUE if every entry was accepted
 */
gboolean
venture_plugin_provides_registry_dispatch(
	VenturePluginProvidesRegistry	 *self,
	VenturePluginManager		 *manager,
	VenturePluginManifest		 *manifest,
	GError				**error
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VenturePluginManifest,
                              venture_plugin_manifest_unref)

G_END_DECLS

#endif /* VENTURE_PLUGIN_RUNTIME_H */
