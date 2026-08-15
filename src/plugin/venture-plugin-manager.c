/*
 * venture-plugin-manager.c - Loading plugins and declarative types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>
#include <yaml-glib.h>

typedef struct
{
	gchar			*name;
	gchar			*path;
	gchar			*description;
	VenturePluginKind	 kind;
} VenturePluginRecord;

struct _VenturePluginManager
{
	GObject parent_instance;

	VentureContext			*context;
	VentureCrispyHost		*crispy;
	VentureVentureTypeRegistry	*venture_types;

	GPtrArray			*loaded;	/* VenturePluginRecord */
	GHashTable			*seen;		/* path -> NULL */
};

G_DEFINE_FINAL_TYPE(VenturePluginManager, venture_plugin_manager, G_TYPE_OBJECT)

static void
venture_plugin_record_free(gpointer data)
{
	VenturePluginRecord *record;

	record = data;

	g_free(record->name);
	g_free(record->path);
	g_free(record->description);
	g_free(record);
}

static void
venture_plugin_manager_finalize(GObject *object)
{
	VenturePluginManager *self;

	self = VENTURE_PLUGIN_MANAGER(object);

	g_clear_object(&self->context);
	g_clear_object(&self->crispy);
	g_clear_object(&self->venture_types);
	g_clear_pointer(&self->loaded, g_ptr_array_unref);
	g_clear_pointer(&self->seen, g_hash_table_unref);

	G_OBJECT_CLASS(venture_plugin_manager_parent_class)->finalize(object);
}

enum
{
	SIGNAL_CONFIG_CHANGED,
	N_SIGNALS
};

static guint venture_plugin_manager_signals[N_SIGNALS];

static void
venture_plugin_manager_class_init(VenturePluginManagerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_plugin_manager_finalize;

	/**
	 * VenturePluginManager::config-changed:
	 * @self: the manager
	 * @plugin_name: whose configuration changed
	 *
	 * Raised after a plugin's configuration is stored. A plugin that
	 * connects here and re-reads its configuration follows the settings
	 * page live, without a restart -- which is the entire point of
	 * configuration being a record rather than a file read once.
	 */
	venture_plugin_manager_signals[SIGNAL_CONFIG_CHANGED] =
		g_signal_new("config-changed",
		             VENTURE_TYPE_PLUGIN_MANAGER,
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
venture_plugin_manager_init(VenturePluginManager *self)
{
	self->loaded = g_ptr_array_new_with_free_func(venture_plugin_record_free);
	self->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

VenturePluginManager *
venture_plugin_manager_new(VentureContext *context)
{
	VenturePluginManager *self;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	self = g_object_new(VENTURE_TYPE_PLUGIN_MANAGER, NULL);
	self->context = g_object_ref(context);

	/*
	 * Types are loaded into the context's registry, not a private one.
	 * Everything that consumes them -- the API, the web UI, validation --
	 * reads the context, so a second registry here would load the files
	 * and then have nobody look at them.
	 */
	self->venture_types = g_object_ref(
		venture_context_get_venture_types(context));

	return self;
}

VentureVentureTypeRegistry *
venture_plugin_manager_get_venture_types(VenturePluginManager *self)
{
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	return self->venture_types;
}

guint
venture_plugin_manager_get_count(VenturePluginManager *self)
{
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), 0);

	return self->loaded->len;
}

/*
 * Records a successfully loaded extension so the plugin list can report it.
 */
static void
venture_plugin_manager_record(
	VenturePluginManager	*self,
	const gchar		*path,
	VenturePluginKind	 kind,
	const gchar		*description
){
	VenturePluginRecord *record;

	record = g_new0(VenturePluginRecord, 1);
	record->name = g_path_get_basename(path);
	record->path = g_strdup(path);
	record->kind = kind;
	record->description = g_strdup(description);

	g_ptr_array_add(self->loaded, record);
}

/*
 * Calls a resolved entry point and records the result.
 *
 * The context is handed over whole: a plugin gets the same registries the
 * core uses, because an extension that could only do less than a built-in
 * would not be worth the mechanism.
 */
static gboolean
venture_plugin_manager_invoke(
	VenturePluginManager	 *self,
	const gchar		 *path,
	VenturePluginKind	  kind,
	gpointer		  register_symbol,
	gpointer		  describe_symbol,
	GError			**error
){
	VenturePluginRegisterFunc register_func;
	VenturePluginDescribeFunc describe_func;
	const gchar *description = NULL;
	g_autoptr(GError) local_error = NULL;

	register_func = (VenturePluginRegisterFunc)register_symbol;
	describe_func = (VenturePluginDescribeFunc)describe_symbol;

	if (NULL != describe_func)
		description = describe_func();

	if (!register_func(self->context, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s refused to register: %s", path,
		            (NULL != local_error) ? local_error->message
		                                  : "no reason given");
		return FALSE;
	}

	venture_plugin_manager_record(self, path, kind, description);
	g_message("Loaded plugin %s", path);

	return TRUE;
}

static gboolean
venture_plugin_manager_load_native(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	GModule *module;
	gpointer register_symbol = NULL;
	gpointer describe_symbol = NULL;

	/* BIND_LAZY so an unused symbol does not fail the load; deliberately
	 * not BIND_LOCAL, because the plugin must see the executable's
	 * exported venture_* symbols. */
	module = g_module_open(path, G_MODULE_BIND_LAZY);

	if (NULL == module)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "Cannot load %s: %s", path, g_module_error());
		return FALSE;
	}

	if (!g_module_symbol(module, "venture_plugin_register", &register_symbol) ||
	    (NULL == register_symbol))
	{
		g_module_close(module);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s exports no venture_plugin_register()", path);
		return FALSE;
	}

	g_module_symbol(module, "venture_plugin_info", &describe_symbol);

	/* Never unloaded: the plugin may have registered a GType. */
	g_module_make_resident(module);

	return venture_plugin_manager_invoke(self, path,
	                                     VENTURE_PLUGIN_KIND_NATIVE,
	                                     register_symbol, describe_symbol,
	                                     error);
}

static gboolean
venture_plugin_manager_load_crispy(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	gpointer register_symbol = NULL;
	gpointer describe_symbol = NULL;
	gboolean allow_crispy;

	g_object_get(venture_context_get_config(self->context),
	             "plugins-allow-crispy", &allow_crispy, NULL);

	if (!allow_crispy)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s is a crispy plugin but plugins.allow_crispy is off",
		            path);
		return FALSE;
	}

	if (NULL == self->crispy)
	{
		g_autofree gchar *cache_dir = NULL;

		cache_dir = g_build_filename(
			venture_config_get_state_dir(
				venture_context_get_config(self->context)),
			"crispy-cache", NULL);

		self->crispy = venture_crispy_host_new(cache_dir, error);

		if (NULL == self->crispy)
			return FALSE;
	}

	if (!venture_crispy_host_lookup(self->crispy, path,
	                                "venture_plugin_register",
	                                &register_symbol, NULL, error))
		return FALSE;

	/* The descriptor is optional, so a failure to find it is not an
	 * error -- the plugin simply has no description. */
	venture_crispy_host_lookup(self->crispy, path, "venture_plugin_info",
	                           &describe_symbol, NULL, NULL);

	return venture_plugin_manager_invoke(self, path,
	                                     VENTURE_PLUGIN_KIND_CRISPY,
	                                     register_symbol, describe_symbol,
	                                     error);
}

static gboolean
venture_plugin_manager_load_venture_type(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	g_autoptr(VentureVentureType) type = NULL;

	type = venture_venture_type_new_from_file(path, error);

	if (NULL == type)
		return FALSE;

	venture_plugin_manager_record(self, path, VENTURE_PLUGIN_KIND_DECLARATIVE,
	                              venture_venture_type_get_description(type));
	venture_venture_type_registry_add(self->venture_types,
	                                  g_steal_pointer(&type));

	return TRUE;
}

gboolean
venture_plugin_manager_load_file(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);
	g_return_val_if_fail(NULL != path, FALSE);

	/* Loading the same file twice would register its types twice, which
	 * the entity registry would then refuse; skipping is the right
	 * answer when two configured directories overlap. */
	if (g_hash_table_contains(self->seen, path))
		return TRUE;

	g_hash_table_add(self->seen, g_strdup(path));

	if (g_str_has_suffix(path, ".so"))
		return venture_plugin_manager_load_native(self, path, error);

	if (g_str_has_suffix(path, ".c"))
		return venture_plugin_manager_load_crispy(self, path, error);

	if (g_str_has_suffix(path, ".yaml") || g_str_has_suffix(path, ".yml"))
		return venture_plugin_manager_load_venture_type(self, path, error);

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
	            "%s is not a plugin: expected .so, .c, .yaml or .yml", path);

	return FALSE;
}

guint
venture_plugin_manager_load_directory(
	VenturePluginManager	 *self,
	const gchar		 *path,
	GError			**error
){
	g_autoptr(GDir) directory = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	const gchar *entry;
	guint loaded;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), 0);
	g_return_val_if_fail(NULL != path, 0);

	directory = g_dir_open(path, 0, &local_error);

	if (NULL == directory)
	{
		/* A configured directory that does not exist is normal on a
		 * fresh install, not a failure. */
		g_debug("venture_plugin_manager: %s", local_error->message);
		return 0;
	}

	entries = g_ptr_array_new_with_free_func(g_free);

	while (NULL != (entry = g_dir_read_name(directory)))
		g_ptr_array_add(entries, g_strdup(entry));

	/* Sorted, so load order is deterministic and a plugin that depends on
	 * another loading first can rely on naming to arrange it. */
	g_ptr_array_sort_values(entries, (GCompareFunc)g_strcmp0);

	loaded = 0;

	for (i = 0; i < entries->len; i++)
	{
		g_autofree gchar *full_path = NULL;
		g_autoptr(GError) load_error = NULL;
		const gchar *name;

		name = g_ptr_array_index(entries, i);

		if (!g_str_has_suffix(name, ".so") && !g_str_has_suffix(name, ".c") &&
		    !g_str_has_suffix(name, ".yaml") && !g_str_has_suffix(name, ".yml"))
			continue;

		full_path = g_build_filename(path, name, NULL);

		if (!venture_plugin_manager_load_file(self, full_path, &load_error))
		{
			/*
			 * One broken plugin must not stop the server. The
			 * operator still needs their data, and a warning they
			 * can act on beats a refusal to start.
			 */
			g_warning("Skipping plugin: %s", load_error->message);
			continue;
		}

		loaded++;
	}

	return loaded;
}

gboolean
venture_plugin_manager_load_configured(
	VenturePluginManager	 *self,
	GError			**error
){
	VentureConfig *config;
	g_auto(GStrv) paths = NULL;
	g_auto(GStrv) type_paths = NULL;
	g_auto(GStrv) required = NULL;
	gboolean enabled;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);

	config = venture_context_get_config(self->context);
	g_object_get(config,
	             "plugins-enabled", &enabled,
	             "plugins-paths", &paths,
	             "plugins-venture-type-paths", &type_paths,
	             "plugins-required", &required,
	             NULL);

	if (!enabled)
		return TRUE;

	/* The install prefix first, then anything configured, so a
	 * locally-configured plugin can override a packaged one by name. */
	venture_plugin_manager_load_directory(self, VENTURE_PLUGINDIR, NULL);

	/* The build tree, so a freshly-built plugin is picked up without
	 * installing it. */
	{
		const gchar *development_path;

		development_path = g_getenv("VENTURE_PLUGIN_PATH");

		if (NULL != development_path)
		{
			g_auto(GStrv) parts = NULL;

			parts = g_strsplit(development_path, ":", -1);

			for (i = 0; NULL != parts[i]; i++)
				venture_plugin_manager_load_directory(self, parts[i], NULL);
		}
	}

	for (i = 0; (NULL != paths) && (NULL != paths[i]); i++)
		venture_plugin_manager_load_directory(self, paths[i], NULL);

	/* The declarative venture types shipped with the install, then any
	 * configured directory. */
	{
		g_autofree gchar *builtin_types = NULL;

		builtin_types = g_build_filename(VENTURE_DATADIR, "venture-types",
		                                 NULL);
		venture_venture_type_registry_load_directory(self->venture_types,
		                                             builtin_types, NULL);
	}

	{
		const gchar *development_types;

		development_types = g_getenv("VENTURE_VENTURE_TYPE_PATH");

		if (NULL != development_types)
		{
			g_auto(GStrv) parts = NULL;

			/* Colon-separated, like every other *_PATH. */
			parts = g_strsplit(development_types, ":", -1);

			for (i = 0; NULL != parts[i]; i++)
			{
				venture_venture_type_registry_load_directory(
					self->venture_types, parts[i], NULL);
			}
		}
	}

	for (i = 0; (NULL != type_paths) && (NULL != type_paths[i]); i++)
	{
		venture_venture_type_registry_load_directory(self->venture_types,
		                                             type_paths[i], NULL);
	}

	/*
	 * A required plugin that did not load IS fatal. That is the whole
	 * point of the list: an install that depends on a plugin should not
	 * start half-configured and quietly behave differently.
	 */
	for (i = 0; (NULL != required) && (NULL != required[i]); i++)
	{
		gboolean found;
		guint j;

		found = FALSE;

		for (j = 0; j < self->loaded->len; j++)
		{
			const VenturePluginRecord *record;

			record = g_ptr_array_index(self->loaded, j);

			if ((0 == g_strcmp0(record->name, required[i])) ||
			    (0 == g_strcmp0(record->path, required[i])))
			{
				found = TRUE;
				break;
			}
		}

		if (!found)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "The required plugin \"%s\" did not load. Fix it, "
			            "or remove it from plugins.required.", required[i]);
			return FALSE;
		}
	}

	return TRUE;
}

JsonNode *
venture_plugin_manager_list(VenturePluginManager *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < self->loaded->len; i++)
	{
		const VenturePluginRecord *record;

		record = g_ptr_array_index(self->loaded, i);

		json_builder_begin_object(builder);

		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, record->name);

		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder,
			venture_enum_to_nick(VENTURE_TYPE_PLUGIN_KIND,
			                     (gint)record->kind));

		json_builder_set_member_name(builder, "path");
		json_builder_add_string_value(builder, record->path);

		if (NULL != record->description)
		{
			json_builder_set_member_name(builder, "description");
			json_builder_add_string_value(builder, record->description);
		}

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Plugin configuration
 * ========================================================================== */

/*
 * Finds the configuration record for @plugin_name, if one exists.
 *
 * Returns: (transfer full) (nullable): the record, or %NULL
 */
static VentureEntity *
venture_plugin_manager_find_config(
	VenturePluginManager	*self,
	const gchar		*plugin_name
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;

	query = venture_query_new(VENTURE_TYPE_PLUGIN_CONFIG);

	if (!venture_query_add_filter_string(query, "name",
	                                     VENTURE_FILTER_OP_EQ, plugin_name,
	                                     NULL))
		return NULL;

	venture_query_set_limit(query, 1);

	records = venture_database_find(
		venture_context_get_database(self->context), query, NULL);

	if ((NULL == records) || (0 == records->len))
		return NULL;

	return g_object_ref(g_ptr_array_index(records, 0));
}

gchar *
venture_plugin_manager_get_config_text(
	VenturePluginManager	*self,
	const gchar		*plugin_name
){
	g_autoptr(VentureEntity) record = NULL;
	gchar *text;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);
	g_return_val_if_fail(NULL != plugin_name, NULL);

	record = venture_plugin_manager_find_config(self, plugin_name);

	if (NULL == record)
		return NULL;

	g_object_get(record, "config", &text, NULL);

	return text;
}

JsonNode *
venture_plugin_manager_get_config(
	VenturePluginManager	*self,
	const gchar		*plugin_name
){
	g_autofree gchar *text = NULL;
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), NULL);

	text = venture_plugin_manager_get_config_text(self, plugin_name);

	if (venture_string_is_empty(text))
		return NULL;

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, text, -1, NULL))
		return NULL;

	root = yaml_parser_get_root(parser);

	return (NULL != root) ? yaml_node_to_json_node(root) : NULL;
}

gboolean
venture_plugin_manager_set_config(
	VenturePluginManager	 *self,
	const gchar		 *plugin_name,
	const gchar		 *yaml,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(VENTURE_IS_PLUGIN_MANAGER(self), FALSE);
	g_return_val_if_fail(NULL != plugin_name, FALSE);

	if (NULL == yaml)
		yaml = "";

	/*
	 * Refuse YAML that does not parse before anything is stored. The
	 * plugin re-reads on the change signal, and handing a running plugin
	 * settings it cannot parse turns a typo on the settings page into a
	 * runtime failure somewhere far from the typo.
	 */
	if ('\0' != *yaml)
	{
		g_autoptr(YamlParser) parser = NULL;

		parser = yaml_parser_new();

		if (!yaml_parser_load_from_data(parser, yaml, -1, &local_error))
		{
			g_set_error(error, VENTURE_ERROR,
			            VENTURE_ERROR_INVALID_ARGUMENT,
			            "That is not valid YAML: %s",
			            (NULL != local_error)
			                ? local_error->message : "unparseable");
			return FALSE;
		}
	}

	record = venture_plugin_manager_find_config(self, plugin_name);

	if (NULL == record)
	{
		record = VENTURE_ENTITY(venture_plugin_config_new());
		g_object_set(record, "name", plugin_name, NULL);
		venture_entity_set_organization_id(record,
			venture_context_get_default_organization_id(self->context));
	}

	g_object_set(record, "config", yaml, NULL);

	if (!venture_database_save(venture_context_get_database(self->context),
	                           record, actor, error))
		return FALSE;

	g_signal_emit(self,
	              venture_plugin_manager_signals[SIGNAL_CONFIG_CHANGED], 0,
	              plugin_name);

	return TRUE;
}

gchar *
venture_plugin_config_get_string(
	JsonNode	*config,
	const gchar	*key,
	const gchar	*fallback
){
	JsonObject *object;
	JsonNode *member;

	g_return_val_if_fail(NULL != key, NULL);

	if ((NULL == config) || !JSON_NODE_HOLDS_OBJECT(config))
		return g_strdup(fallback);

	object = json_node_get_object(config);

	if (!json_object_has_member(object, key))
		return g_strdup(fallback);

	member = json_object_get_member(object, key);

	if (!JSON_NODE_HOLDS_VALUE(member))
		return g_strdup(fallback);

	return g_strdup(json_node_get_string(member));
}

gdouble
venture_plugin_config_get_double(
	JsonNode	*config,
	const gchar	*key,
	gdouble		 fallback
){
	JsonObject *object;
	JsonNode *member;

	g_return_val_if_fail(NULL != key, fallback);

	if ((NULL == config) || !JSON_NODE_HOLDS_OBJECT(config))
		return fallback;

	object = json_node_get_object(config);

	if (!json_object_has_member(object, key))
		return fallback;

	member = json_object_get_member(object, key);

	if (!JSON_NODE_HOLDS_VALUE(member))
		return fallback;

	return json_node_get_double(member);
}
