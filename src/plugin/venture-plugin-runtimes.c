/*
 * venture-plugin-runtimes.c - The four runtimes that ship
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * native, crispy and declarative are what the plugin manager did before it
 * had runtimes, moved behind the interface unchanged: a bare .so, .c or
 * .yaml loads exactly as it always has. exec is new.
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * Shared by native and crispy: calling a resolved entry point
 * ========================================================================== */

/*
 * Calls `venture_plugin_register` and takes `venture_plugin_info` as the
 * description. The context is handed over whole: a plugin gets the same
 * registries the core uses, because an extension that could only do less
 * than a built-in would not be worth the mechanism.
 */
static gboolean
runtimes_call_register(
	VentureContext		 *context,
	const gchar		 *path,
	gpointer		  register_symbol,
	gpointer		  describe_symbol,
	VenturePluginRecord	 *record,
	GError			**error
){
	VenturePluginRegisterFunc register_func;
	VenturePluginDescribeFunc describe_func;
	g_autoptr(GError) local_error = NULL;

	register_func = (VenturePluginRegisterFunc)register_symbol;
	describe_func = (VenturePluginDescribeFunc)describe_symbol;

	if (NULL != describe_func)
		venture_plugin_record_set_description(record, describe_func());

	if (!register_func(context, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s refused to register: %s", path,
		            (NULL != local_error) ? local_error->message
		                                  : "no reason given");
		return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * native
 * ========================================================================== */

static gboolean
runtimes_load_native(
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
){
	GModule *module;
	gpointer register_symbol = NULL;
	gpointer describe_symbol = NULL;

	(void)manager;
	(void)manifest;
	(void)user_data;

	venture_plugin_record_set_kind(record, VENTURE_PLUGIN_KIND_NATIVE);

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

	return runtimes_call_register(context, path, register_symbol,
	                              describe_symbol, record, error);
}

VenturePluginRuntime *
venture_plugin_runtime_native_new(void)
{
	static const gchar *const extensions[] = { ".so", NULL };

	return venture_func_plugin_runtime_new("native", extensions,
	                                       runtimes_load_native, NULL, NULL);
}

/* ==========================================================================
 * crispy
 * ========================================================================== */

typedef struct
{
	/* Built on first use: making one finds a compiler, and an install
	 * with no .c plugin should not pay for that. */
	VentureCrispyHost *host;
} CrispyRuntime;

static void
crispy_runtime_free(gpointer data)
{
	CrispyRuntime *runtime;

	runtime = data;

	g_clear_object(&runtime->host);
	g_free(runtime);
}

static gboolean
runtimes_load_crispy(
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
){
	CrispyRuntime *runtime;
	gpointer register_symbol = NULL;
	gpointer describe_symbol = NULL;
	gboolean allow_crispy;

	(void)manager;
	(void)manifest;

	runtime = user_data;
	venture_plugin_record_set_kind(record, VENTURE_PLUGIN_KIND_CRISPY);

	g_object_get(venture_context_get_config(context),
	             "plugins-allow-crispy", &allow_crispy, NULL);

	if (!allow_crispy)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s is a crispy plugin but plugins.allow_crispy is off",
		            path);
		return FALSE;
	}

	if (NULL == runtime->host)
	{
		g_autofree gchar *cache_dir = NULL;

		cache_dir = g_build_filename(
			venture_config_get_state_dir(
				venture_context_get_config(context)),
			"crispy-cache", NULL);

		runtime->host = venture_crispy_host_new(cache_dir, error);

		if (NULL == runtime->host)
			return FALSE;
	}

	if (!venture_crispy_host_lookup(runtime->host, path,
	                                "venture_plugin_register",
	                                &register_symbol, NULL, error))
		return FALSE;

	/* The descriptor is optional, so a failure to find it is not an
	 * error -- the plugin simply has no description. */
	venture_crispy_host_lookup(runtime->host, path, "venture_plugin_info",
	                           &describe_symbol, NULL, NULL);

	return runtimes_call_register(context, path, register_symbol,
	                              describe_symbol, record, error);
}

VenturePluginRuntime *
venture_plugin_runtime_crispy_new(void)
{
	static const gchar *const extensions[] = { ".c", NULL };

	return venture_func_plugin_runtime_new("crispy", extensions,
	                                       runtimes_load_crispy,
	                                       g_new0(CrispyRuntime, 1),
	                                       crispy_runtime_free);
}

/* ==========================================================================
 * declarative
 * ========================================================================== */

static gboolean
runtimes_load_declarative(
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(VentureVentureType) type = NULL;

	(void)manager;
	(void)manifest;
	(void)user_data;

	venture_plugin_record_set_kind(record, VENTURE_PLUGIN_KIND_DECLARATIVE);

	type = venture_venture_type_new_from_file(path, error);

	if (NULL == type)
		return FALSE;

	venture_plugin_record_set_description(record,
		venture_venture_type_get_description(type));

	/*
	 * Into the context's registry, not a private one. Everything that
	 * consumes venture types -- the API, the web UI, validation -- reads
	 * the context, so a second registry would load the file and then have
	 * nobody look at it.
	 */
	venture_venture_type_registry_add(venture_context_get_venture_types(context),
	                                  g_steal_pointer(&type));

	return TRUE;
}

VenturePluginRuntime *
venture_plugin_runtime_declarative_new(void)
{
	static const gchar *const extensions[] = { ".yaml", ".yml", NULL };

	return venture_func_plugin_runtime_new("declarative", extensions,
	                                       runtimes_load_declarative,
	                                       NULL, NULL);
}

/* ==========================================================================
 * exec
 * ========================================================================== */

static const gchar *const exec_section_keys[] = {
	"args", "env", "timeout", "max_output", "max_stderr", NULL
};

/*
 * Reads a whole number from the exec section, as an integer or its digits,
 * within [minimum, maximum]. Absent leaves @out_value alone.
 */
static gboolean
exec_section_number(
	JsonObject	 *section,
	const gchar	 *manifest_path,
	const gchar	 *member,
	gint64		  minimum,
	gint64		  maximum,
	gint64		 *out_value,
	GError		**error
){
	JsonNode *node;
	gint64 value;

	if (!json_object_has_member(section, member))
		return TRUE;

	node = json_object_get_member(section, member);

	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_INT64 == json_node_get_value_type(node)))
	{
		value = json_node_get_int(node);
	}
	else if (!JSON_NODE_HOLDS_VALUE(node) ||
	         (G_TYPE_STRING != json_node_get_value_type(node)) ||
	         !g_ascii_string_to_signed(json_node_get_string(node), 10,
	                                   G_MININT64, G_MAXINT64, &value, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: exec.%s must be a whole number", manifest_path,
		            member);
		return FALSE;
	}

	if ((value < minimum) || (value > maximum))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: exec.%s must be between %" G_GINT64_FORMAT " and %"
		            G_GINT64_FORMAT, manifest_path, member, minimum, maximum);
		return FALSE;
	}

	*out_value = value;

	return TRUE;
}

/*
 * Reads a list of scalars as strings. YAML reads `--flag` as a string and
 * `10` as a number; an argument is text either way, so both are accepted.
 */
static GPtrArray *
exec_section_strings(
	JsonObject	 *section,
	const gchar	 *manifest_path,
	const gchar	 *member,
	guint		  maximum,
	GError		**error
){
	g_autoptr(GPtrArray) values = NULL;
	JsonNode *node;
	JsonArray *array;
	guint i;

	values = g_ptr_array_new_with_free_func(g_free);

	if (!json_object_has_member(section, member))
		return g_steal_pointer(&values);

	node = json_object_get_member(section, member);

	if (JSON_NODE_HOLDS_NULL(node))
		return g_steal_pointer(&values);

	if (!JSON_NODE_HOLDS_ARRAY(node))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: exec.%s must be a list", manifest_path, member);
		return NULL;
	}

	array = json_node_get_array(node);

	if (json_array_get_length(array) > maximum)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: exec.%s has more than %u entries", manifest_path,
		            member, maximum);
		return NULL;
	}

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *element;
		GType value_type;

		element = json_array_get_element(array, i);
		value_type = JSON_NODE_HOLDS_VALUE(element)
			? json_node_get_value_type(element) : G_TYPE_INVALID;

		if (G_TYPE_STRING == value_type)
			g_ptr_array_add(values, g_strdup(json_node_get_string(element)));
		else if (G_TYPE_INT64 == value_type)
			g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT,
			                json_node_get_int(element)));
		else
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s: exec.%s[%u] must be text", manifest_path,
			            member, i);
			return NULL;
		}
	}

	return g_steal_pointer(&values);
}

static gboolean
runtimes_load_exec(
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(VentureExecSpec) spec = NULL;
	g_autoptr(GPtrArray) args = NULL;
	g_autoptr(GPtrArray) env = NULL;
	JsonObject *section;
	const gchar *manifest_path;
	gboolean allow_exec;
	gint64 timeout;
	gint64 max_output;
	gint64 max_stderr;
	guint i;

	(void)user_data;

	venture_plugin_record_set_kind(record, VENTURE_PLUGIN_KIND_EXEC);

	/*
	 * Off by default, and checked first. Turning it on is the operator
	 * saying that the programs in their plugin directories may run as
	 * the server's user; nothing in a manifest can say that for them.
	 */
	g_object_get(venture_context_get_config(context),
	             "plugins-allow-exec", &allow_exec, NULL);

	if (!allow_exec)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s is an exec plugin but plugins.allow_exec is off", path);
		return FALSE;
	}

	if (NULL == manifest)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: an exec plugin is loaded only through a manifest",
		            path);
		return FALSE;
	}

	manifest_path = venture_plugin_manifest_get_path(manifest);

	/* The protocol is declared, never assumed: a program written for a
	 * later revision must not be read under rules it was not written to. */
	if (VENTURE_JSONL_PROTOCOL_VERSION !=
	    venture_plugin_manifest_get_protocol(manifest))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s: an exec plugin must declare \"protocol: %d\", the "
		            "only version this build speaks", manifest_path,
		            VENTURE_JSONL_PROTOCOL_VERSION);
		return FALSE;
	}

	section = venture_plugin_manifest_get_section(manifest, "exec");
	timeout = VENTURE_EXEC_DEFAULT_TIMEOUT_SECONDS;
	max_output = VENTURE_EXEC_DEFAULT_MAX_STDOUT;
	max_stderr = VENTURE_EXEC_DEFAULT_MAX_STDERR;

	if (NULL != section)
	{
		JsonObjectIter iter;
		JsonNode *member;
		const gchar *name;

		/* A misspelt key would otherwise be a setting that silently
		 * does nothing -- a timeout of 60 where 5 was meant. */
		json_object_iter_init(&iter, section);

		while (json_object_iter_next(&iter, &name, &member))
		{
			if (!g_strv_contains(exec_section_keys, name))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
				            "%s: exec.%s is not a setting (known: args, env, "
				            "timeout, max_output, max_stderr)", manifest_path,
				            name);
				return FALSE;
			}
		}

		if (!exec_section_number(section, manifest_path, "timeout", 1,
		                         VENTURE_EXEC_MAX_TIMEOUT_SECONDS, &timeout,
		                         error) ||
		    !exec_section_number(section, manifest_path, "max_output", 1,
		                         VENTURE_EXEC_MAX_MAX_STDOUT, &max_output,
		                         error) ||
		    !exec_section_number(section, manifest_path, "max_stderr", 1,
		                         VENTURE_EXEC_MAX_MAX_STDERR, &max_stderr,
		                         error))
			return FALSE;

		args = exec_section_strings(section, manifest_path, "args",
		                            VENTURE_EXEC_MAX_ARGS, error);

		if (NULL == args)
			return FALSE;

		env = exec_section_strings(section, manifest_path, "env", 64, error);

		if (NULL == env)
			return FALSE;
	}

	spec = venture_exec_spec_new(venture_plugin_manifest_get_name(manifest),
	                             venture_plugin_manifest_get_root(manifest),
	                             path, error);

	if (NULL == spec)
		return FALSE;

	venture_exec_spec_set_timeout(spec, (guint)timeout);
	venture_exec_spec_set_limits(spec, (gsize)max_output, (gsize)max_stderr);

	if (NULL != args)
	{
		g_ptr_array_add(args, NULL);
		venture_exec_spec_set_args(spec, (const gchar *const *)args->pdata);
	}

	for (i = 0; (NULL != env) && (i < env->len); i++)
	{
		if (!venture_exec_spec_pass_env(spec, g_ptr_array_index(env, i),
		                                error))
			return FALSE;
	}

	/* Nothing runs at load: a program is started when something it
	 * provides is used, so a slow or broken one cannot hold up startup. */
	return venture_plugin_manager_add_exec(manager, spec, error);
}

VenturePluginRuntime *
venture_plugin_runtime_exec_new(void)
{
	return venture_func_plugin_runtime_new("exec", NULL, runtimes_load_exec,
	                                       NULL, NULL);
}
