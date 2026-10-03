/*
 * venture-plugin-runtime.c - What loads a plugin, and what a plugin offers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <stdlib.h>
#include <string.h>
#include <yaml-glib.h>

/* A manifest is a page of YAML. Anything larger is not one. */
#define VENTURE_PLUGIN_MANIFEST_MAX_BYTES (64 * 1024)

/* The longest plugin name: it becomes a plugin_config record's name, a
 * line on the plugins page and a key in plugins.required. */
#define VENTURE_PLUGIN_NAME_MAX (64)

/* ==========================================================================
 * Manifests
 * ========================================================================== */

struct _VenturePluginManifest
{
	gint		 ref_count;
	gchar		*name;
	gchar		*runtime;
	gchar		*description;
	gchar		*path;
	gchar		*root;
	gchar		*entry;
	gint64		 protocol;
	JsonObject	*object;
};

G_DEFINE_BOXED_TYPE(VenturePluginManifest, venture_plugin_manifest,
                    venture_plugin_manifest_ref, venture_plugin_manifest_unref)

/*
 * A plugin name: lower case letters and digits, then those plus `.`, `_`
 * and `-`. Restricted because the name is stored, shown and matched
 * against plugins.required, and a name with a slash or a space in it would
 * be a path or a sentence by the time it got there.
 */
static gboolean
manifest_name_is_valid(const gchar *name)
{
	gsize length;

	if ((NULL == name) || ('\0' == name[0]))
		return FALSE;

	length = strlen(name);

	if (length > VENTURE_PLUGIN_NAME_MAX)
		return FALSE;

	if (!g_ascii_islower(name[0]) && !g_ascii_isdigit(name[0]))
		return FALSE;

	return strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789._-") == length;
}

/*
 * Reads a top-level string, refusing any other shape. Absent and null both
 * answer NULL with no error.
 */
static gboolean
manifest_string(
	JsonObject	 *object,
	const gchar	 *path,
	const gchar	 *member,
	const gchar	**out_value,
	GError		**error
){
	JsonNode *node;

	*out_value = NULL;

	if (!json_object_has_member(object, member))
		return TRUE;

	node = json_object_get_member(object, member);

	if (JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	if (!JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: \"%s\" must be a string", path, member);
		return FALSE;
	}

	*out_value = json_node_get_string(node);

	return TRUE;
}

VenturePluginManifest *
venture_plugin_manifest_new_from_file(
	const gchar	 *path,
	GError		**error
){
	g_autoptr(VenturePluginManifest) self = NULL;
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(JsonNode) root_node = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *directory = NULL;
	JsonObject *object;
	YamlNode *yaml_root;
	const gchar *name;
	const gchar *runtime;
	const gchar *description;
	const gchar *entry;
	gsize length;

	g_return_val_if_fail(NULL != path, NULL);

	if (!g_file_get_contents(path, &text, &length, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "Cannot read the manifest %s: %s", path,
		            local_error->message);
		return NULL;
	}

	if (length > VENTURE_PLUGIN_MANIFEST_MAX_BYTES)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s is larger than a manifest can be (%u bytes)", path,
		            (guint)VENTURE_PLUGIN_MANIFEST_MAX_BYTES);
		return NULL;
	}

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, text, (gssize)length,
	                                &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s is not valid YAML: %s", path, local_error->message);
		return NULL;
	}

	yaml_root = yaml_parser_get_root(parser);

	if (NULL != yaml_root)
		root_node = yaml_node_to_json_node(yaml_root);

	if ((NULL == root_node) || !JSON_NODE_HOLDS_OBJECT(root_node))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s must be a mapping of keys to values", path);
		return NULL;
	}

	object = json_node_get_object(root_node);

	if (!manifest_string(object, path, "name", &name, error) ||
	    !manifest_string(object, path, "runtime", &runtime, error) ||
	    !manifest_string(object, path, "description", &description, error) ||
	    !manifest_string(object, path, "entry", &entry, error))
		return NULL;

	if (!manifest_name_is_valid(name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: \"name\" is required, and must be lower-case "
		            "letters, digits, '.', '_' or '-' (at most %d)", path,
		            VENTURE_PLUGIN_NAME_MAX);
		return NULL;
	}

	if ((NULL != runtime) && !manifest_name_is_valid(runtime))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: \"runtime\" is not a runtime name", path);
		return NULL;
	}

	if ((NULL == entry) || ('\0' == entry[0]))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: \"entry\" is required: the file this plugin loads, "
		            "relative to the manifest", path);
		return NULL;
	}

	self = g_new0(VenturePluginManifest, 1);
	self->ref_count = 1;
	self->name = g_strdup(name);
	self->runtime = g_strdup(runtime);
	self->description = g_strdup(description);
	self->path = g_strdup(path);
	self->object = json_object_ref(object);

	/* The protocol, as an integer or the digits of one. */
	if (json_object_has_member(object, "protocol"))
	{
		JsonNode *node;
		gboolean understood;

		node = json_object_get_member(object, "protocol");
		understood = FALSE;

		if (JSON_NODE_HOLDS_VALUE(node) &&
		    (G_TYPE_INT64 == json_node_get_value_type(node)))
		{
			self->protocol = json_node_get_int(node);
			understood = TRUE;
		}
		else if (JSON_NODE_HOLDS_VALUE(node) &&
		         (G_TYPE_STRING == json_node_get_value_type(node)))
		{
			understood = g_ascii_string_to_signed(
				json_node_get_string(node), 10, 1, G_MAXINT32,
				&self->protocol, NULL);
		}

		if (!understood)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s: \"protocol\" must be a whole number", path);
			return NULL;
		}

		if (self->protocol < 1)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s: \"protocol\" must be at least 1", path);
			return NULL;
		}
	}

	/* What it provides: a list of objects, each naming its kind. The
	 * kind's own handler judges the rest of the entry. */
	if (json_object_has_member(object, "provides") &&
	    !JSON_NODE_HOLDS_NULL(json_object_get_member(object, "provides")))
	{
		JsonNode *node;
		JsonArray *provides;
		guint i;

		node = json_object_get_member(object, "provides");

		if (!JSON_NODE_HOLDS_ARRAY(node))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s: \"provides\" must be a list", path);
			return NULL;
		}

		provides = json_node_get_array(node);

		for (i = 0; i < json_array_get_length(provides); i++)
		{
			JsonNode *element;
			const gchar *kind;

			element = json_array_get_element(provides, i);

			if (!JSON_NODE_HOLDS_OBJECT(element))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
				            "%s: provides[%u] must be a mapping with a "
				            "\"kind\"", path, i);
				return NULL;
			}

			if (!manifest_string(json_node_get_object(element), path,
			                     "kind", &kind, error))
				return NULL;

			if (!manifest_name_is_valid(kind))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
				            "%s: provides[%u] needs a \"kind\"", path, i);
				return NULL;
			}
		}
	}

	/*
	 * The directory, resolved, and the entry, resolved and held inside it.
	 * Every runtime loads from here, so a manifest cannot point a native
	 * loader at a library elsewhere on the machine any more than it can
	 * point exec at /bin/sh.
	 */
	directory = g_path_get_dirname(path);

	{
		gchar *resolved;

		resolved = realpath(directory, NULL);

		if (NULL == resolved)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s: its directory cannot be resolved", path);
			return NULL;
		}

		self->root = g_strdup(resolved);
		free(resolved);
	}

	self->entry = venture_exec_resolve_within(self->root, entry, &local_error);

	if (NULL == self->entry)
	{
		g_set_error(error, VENTURE_ERROR, local_error->code,
		            "%s: \"entry\" %s", path, local_error->message);
		return NULL;
	}

	return g_steal_pointer(&self);
}

VenturePluginManifest *
venture_plugin_manifest_ref(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	g_atomic_int_inc(&self->ref_count);

	return self;
}

void
venture_plugin_manifest_unref(VenturePluginManifest *self)
{
	if (NULL == self)
		return;

	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_free(self->name);
	g_free(self->runtime);
	g_free(self->description);
	g_free(self->path);
	g_free(self->root);
	g_free(self->entry);
	g_clear_pointer(&self->object, json_object_unref);
	g_free(self);
}

const gchar *
venture_plugin_manifest_get_name(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->name;
}

const gchar *
venture_plugin_manifest_get_runtime(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->runtime;
}

const gchar *
venture_plugin_manifest_get_description(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->description;
}

const gchar *
venture_plugin_manifest_get_path(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->path;
}

const gchar *
venture_plugin_manifest_get_root(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->root;
}

const gchar *
venture_plugin_manifest_get_entry(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->entry;
}

gint64
venture_plugin_manifest_get_protocol(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->protocol;
}

JsonObject *
venture_plugin_manifest_get_object(VenturePluginManifest *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->object;
}

JsonObject *
venture_plugin_manifest_get_section(
	VenturePluginManifest	*self,
	const gchar		*name
){
	JsonNode *node;

	g_return_val_if_fail(NULL != self, NULL);
	g_return_val_if_fail(NULL != name, NULL);

	if (!json_object_has_member(self->object, name))
		return NULL;

	node = json_object_get_member(self->object, name);

	return JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

JsonArray *
venture_plugin_manifest_get_provides(VenturePluginManifest *self)
{
	JsonNode *node;

	g_return_val_if_fail(NULL != self, NULL);

	if (!json_object_has_member(self->object, "provides"))
		return NULL;

	node = json_object_get_member(self->object, "provides");

	return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

/* ==========================================================================
 * The runtime interface
 * ========================================================================== */

G_DEFINE_INTERFACE(VenturePluginRuntime, venture_plugin_runtime, G_TYPE_OBJECT)

static void
venture_plugin_runtime_default_init(VenturePluginRuntimeInterface *iface)
{
	(void)iface;
}

const gchar *
venture_plugin_runtime_get_name(VenturePluginRuntime *self)
{
	VenturePluginRuntimeInterface *iface;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_RUNTIME(self), NULL);

	iface = VENTURE_PLUGIN_RUNTIME_GET_IFACE(self);
	g_return_val_if_fail(NULL != iface->get_name, NULL);

	return iface->get_name(self);
}

const gchar *const *
venture_plugin_runtime_get_extensions(VenturePluginRuntime *self)
{
	static const gchar *const none[] = { NULL };
	VenturePluginRuntimeInterface *iface;
	const gchar *const *extensions;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_RUNTIME(self), none);

	iface = VENTURE_PLUGIN_RUNTIME_GET_IFACE(self);

	if (NULL == iface->get_extensions)
		return none;

	extensions = iface->get_extensions(self);

	return (NULL != extensions) ? extensions : none;
}

gboolean
venture_plugin_runtime_load(
	VenturePluginRuntime	 *self,
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	GError			**error
){
	VenturePluginRuntimeInterface *iface;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_RUNTIME(self), FALSE);
	g_return_val_if_fail(NULL != path, FALSE);
	g_return_val_if_fail(NULL != record, FALSE);

	iface = VENTURE_PLUGIN_RUNTIME_GET_IFACE(self);
	g_return_val_if_fail(NULL != iface->load, FALSE);

	return iface->load(self, manager, context, path, manifest, record, error);
}

/* ==========================================================================
 * A runtime made of a function
 * ========================================================================== */

#define VENTURE_TYPE_FUNC_PLUGIN_RUNTIME (venture_func_plugin_runtime_get_type())

G_DECLARE_FINAL_TYPE(VentureFuncPluginRuntime, venture_func_plugin_runtime,
                     VENTURE, FUNC_PLUGIN_RUNTIME, GObject)

struct _VentureFuncPluginRuntime
{
	GObject parent_instance;

	gchar				*name;
	GStrv				 extensions;
	VenturePluginRuntimeLoadFunc	 load;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
};

static void venture_func_plugin_runtime_iface_init(
	VenturePluginRuntimeInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(VentureFuncPluginRuntime,
                              venture_func_plugin_runtime, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(VENTURE_TYPE_PLUGIN_RUNTIME,
                                  venture_func_plugin_runtime_iface_init))

static void
venture_func_plugin_runtime_finalize(GObject *object)
{
	VentureFuncPluginRuntime *self;

	self = VENTURE_FUNC_PLUGIN_RUNTIME(object);

	if (NULL != self->destroy)
		self->destroy(self->user_data);

	g_free(self->name);
	g_strfreev(self->extensions);

	G_OBJECT_CLASS(venture_func_plugin_runtime_parent_class)->finalize(object);
}

static void
venture_func_plugin_runtime_class_init(VentureFuncPluginRuntimeClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_func_plugin_runtime_finalize;
}

static void
venture_func_plugin_runtime_init(VentureFuncPluginRuntime *self)
{
	(void)self;
}

static const gchar *
func_runtime_get_name(VenturePluginRuntime *runtime)
{
	return VENTURE_FUNC_PLUGIN_RUNTIME(runtime)->name;
}

static const gchar *const *
func_runtime_get_extensions(VenturePluginRuntime *runtime)
{
	return (const gchar *const *)
		VENTURE_FUNC_PLUGIN_RUNTIME(runtime)->extensions;
}

static gboolean
func_runtime_load(
	VenturePluginRuntime	 *runtime,
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	GError			**error
){
	VentureFuncPluginRuntime *self;

	self = VENTURE_FUNC_PLUGIN_RUNTIME(runtime);

	return self->load(manager, context, path, manifest, record,
	                  self->user_data, error);
}

static void
venture_func_plugin_runtime_iface_init(VenturePluginRuntimeInterface *iface)
{
	iface->get_name = func_runtime_get_name;
	iface->get_extensions = func_runtime_get_extensions;
	iface->load = func_runtime_load;
}

VenturePluginRuntime *
venture_func_plugin_runtime_new(
	const gchar			*name,
	const gchar *const		*extensions,
	VenturePluginRuntimeLoadFunc	 load,
	gpointer			 user_data,
	GDestroyNotify			 destroy
){
	VentureFuncPluginRuntime *self;

	g_return_val_if_fail(NULL != name, NULL);
	g_return_val_if_fail(NULL != load, NULL);

	self = g_object_new(VENTURE_TYPE_FUNC_PLUGIN_RUNTIME, NULL);
	self->name = g_strdup(name);
	self->extensions = (NULL != extensions)
		? g_strdupv((gchar **)extensions) : g_new0(gchar *, 1);
	self->load = load;
	self->user_data = user_data;
	self->destroy = destroy;

	return VENTURE_PLUGIN_RUNTIME(self);
}

/* ==========================================================================
 * The provides registry
 * ========================================================================== */

typedef struct
{
	gchar				*kind;
	gchar				*description;
	VenturePluginProvidesFunc	 func;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} ProvidesKind;

static void
provides_kind_free(gpointer data)
{
	ProvidesKind *kind;

	kind = data;

	if (NULL != kind->destroy)
		kind->destroy(kind->user_data);

	g_free(kind->kind);
	g_free(kind->description);
	g_free(kind);
}

struct _VenturePluginProvidesRegistry
{
	GObject parent_instance;

	GHashTable	*kinds;		/* name -> ProvidesKind */
};

G_DEFINE_FINAL_TYPE(VenturePluginProvidesRegistry,
                    venture_plugin_provides_registry, G_TYPE_OBJECT)

static void
venture_plugin_provides_registry_finalize(GObject *object)
{
	VenturePluginProvidesRegistry *self;

	self = VENTURE_PLUGIN_PROVIDES_REGISTRY(object);

	g_clear_pointer(&self->kinds, g_hash_table_unref);

	G_OBJECT_CLASS(venture_plugin_provides_registry_parent_class)
		->finalize(object);
}

static void
venture_plugin_provides_registry_class_init(
	VenturePluginProvidesRegistryClass *klass
){
	G_OBJECT_CLASS(klass)->finalize =
		venture_plugin_provides_registry_finalize;
}

static void
venture_plugin_provides_registry_init(VenturePluginProvidesRegistry *self)
{
	self->kinds = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                                    provides_kind_free);
}

VenturePluginProvidesRegistry *
venture_plugin_provides_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_PLUGIN_PROVIDES_REGISTRY, NULL);
}

gboolean
venture_plugin_provides_registry_add(
	VenturePluginProvidesRegistry	 *self,
	const gchar			 *kind,
	const gchar			 *description,
	VenturePluginProvidesFunc	  func,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
){
	ProvidesKind *entry;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_PROVIDES_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != func, FALSE);

	if (!manifest_name_is_valid(kind))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A provides kind needs a lower-case name");
		return FALSE;
	}

	if (g_hash_table_contains(self->kinds, kind))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "The provides kind \"%s\" is already registered", kind);
		return FALSE;
	}

	entry = g_new0(ProvidesKind, 1);
	entry->kind = g_strdup(kind);
	entry->description = g_strdup(description);
	entry->func = func;
	entry->user_data = user_data;
	entry->destroy = destroy;

	g_hash_table_insert(self->kinds, entry->kind, entry);

	return TRUE;
}

gboolean
venture_plugin_provides_registry_has(
	VenturePluginProvidesRegistry	*self,
	const gchar			*kind
){
	g_return_val_if_fail(VENTURE_IS_PLUGIN_PROVIDES_REGISTRY(self), FALSE);

	return (NULL != kind) && g_hash_table_contains(self->kinds, kind);
}

gchar **
venture_plugin_provides_registry_list(VenturePluginProvidesRegistry *self)
{
	g_autofree gpointer *keys = NULL;
	GPtrArray *names;
	guint count;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_PROVIDES_REGISTRY(self), NULL);

	keys = g_hash_table_get_keys_as_array(self->kinds, &count);
	names = g_ptr_array_new();

	for (i = 0; i < count; i++)
		g_ptr_array_add(names, g_strdup(keys[i]));

	g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(names, FALSE);
}

gboolean
venture_plugin_provides_registry_dispatch(
	VenturePluginProvidesRegistry	 *self,
	VenturePluginManager		 *manager,
	VenturePluginManifest		 *manifest,
	GError				**error
){
	JsonArray *provides;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_PROVIDES_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != manifest, FALSE);

	provides = venture_plugin_manifest_get_provides(manifest);

	if (NULL == provides)
		return TRUE;

	for (i = 0; i < json_array_get_length(provides); i++)
	{
		JsonObject *entry;
		ProvidesKind *kind;
		const gchar *name;
		g_autoptr(GError) local_error = NULL;

		entry = json_array_get_object_element(provides, i);
		name = json_object_get_string_member(entry, "kind");
		kind = g_hash_table_lookup(self->kinds, name);

		if (NULL == kind)
		{
			g_auto(GStrv) known = NULL;
			g_autofree gchar *joined = NULL;

			known = venture_plugin_provides_registry_list(self);
			joined = g_strjoinv(", ", known);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s provides a \"%s\", which nothing in this "
			            "install understands (known: %s). Is the module "
			            "that handles it switched on?",
			            venture_plugin_manifest_get_path(manifest), name,
			            ('\0' != joined[0]) ? joined : "none");
			return FALSE;
		}

		if (!kind->func(manager, manifest, entry, kind->user_data,
		                &local_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s: its %s was refused: %s",
			            venture_plugin_manifest_get_path(manifest), name,
			            (NULL != local_error) ? local_error->message
			                                  : "no reason given");
			return FALSE;
		}
	}

	return TRUE;
}
