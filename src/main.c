/*
 * main.c - The VENTURE server
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Startup order matters and is deliberate:
 *
 *   configuration -> validation -> database -> migration -> context
 *   -> AI -> web server -> signal handlers -> main loop
 *
 * Validation runs before anything is opened, so a configuration mistake is
 * reported in a second rather than after a database has been created in the
 * wrong place. AI is optional and its absence is never fatal. Signal
 * handlers are installed last so an interrupt during startup exits promptly
 * instead of trying to unwind a half-built server.
 */

#include "venture.h"

#include <glib-unix.h>
#include <stdlib.h>

typedef struct
{
	GMainLoop		*loop;
	VentureWebServer	*server;
	VentureAutomation	*automation;
} VentureServerMain;

/*
 * Handles SIGINT and SIGTERM. Stopping the loop rather than exiting outright
 * lets in-flight requests finish and the database close cleanly, which for
 * SQLite means the write-ahead log is checkpointed rather than left behind.
 */
static gboolean
venture_on_signal(gpointer user_data)
{
	VentureServerMain *application;

	application = user_data;

	g_message("Shutting down");

	if (NULL != application->automation)
		venture_automation_stop(application->automation);

	if (NULL != application->server)
		venture_web_server_stop(application->server);

	g_main_loop_quit(application->loop);

	return G_SOURCE_REMOVE;
}

/*
 * The levels the configured threshold admits. Held in a file-scope variable
 * because a GLogWriterFunc receives no user data of its own.
 */
static GLogLevelFlags venture_log_threshold = G_LOG_LEVEL_MASK;

/*
 * Drops anything below the configured level and hands the rest to GLib's
 * own writer, which already knows about journald and terminal colouring.
 */
static GLogWriterOutput
venture_log_writer(
	GLogLevelFlags	  log_level,
	const GLogField	 *fields,
	gsize		  n_fields,
	gpointer	  user_data
){
	if (0 == (log_level & venture_log_threshold))
		return G_LOG_WRITER_HANDLED;

	return g_log_writer_default(log_level, fields, n_fields, user_data);
}

/*
 * Routes GLib logging according to the configured level, so `logging.level`
 * actually does something rather than being advisory.
 */
static void
venture_configure_logging(VentureConfig *config)
{
	g_autofree gchar *level = NULL;

	g_object_get(config, "logging-level", &level, NULL);

	if (0 == g_strcmp0(level, "debug"))
		venture_log_threshold = G_LOG_LEVEL_MASK;
	else if (0 == g_strcmp0(level, "info"))
		venture_log_threshold = G_LOG_LEVEL_MASK & ~G_LOG_LEVEL_DEBUG;
	else if (0 == g_strcmp0(level, "warning"))
		venture_log_threshold = G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL |
		                        G_LOG_LEVEL_ERROR;
	else
		venture_log_threshold = G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR;

	g_log_set_writer_func(venture_log_writer, NULL, NULL);
	g_log_set_always_fatal(G_LOG_LEVEL_ERROR);
}

int
main(
	int	  argc,
	char	**argv
){
	g_autoptr(GOptionContext) options = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(VenturePluginManager) plugins = NULL;
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;
	VentureServerMain application = { NULL, NULL, NULL };
	g_autofree gchar *config_path = NULL;
	g_autofree gchar *database_uri = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *owner_password = NULL;
	gboolean show_version = FALSE;
	gboolean show_license = FALSE;
	gboolean generate_config = FALSE;
	gboolean generate_c_config = FALSE;
	gboolean migrate_only = FALSE;
	gboolean no_ai = FALSE;
	gboolean no_plugins = FALSE;
	gboolean no_automation = FALSE;
	gboolean list_modules = FALSE;
	g_auto(GStrv) disabled_modules = NULL;
	gint port = 0;

	const GOptionEntry entries[] = {
		{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path,
		  "Read configuration from FILE", "FILE" },
		{ "database", 'd', 0, G_OPTION_ARG_STRING, &database_uri,
		  "Connection URI, overriding the configuration", "URI" },
		{ "state-dir", 0, 0, G_OPTION_ARG_FILENAME, &state_dir,
		  "Directory for the database, caches and automation state", "DIR" },
		{ "port", 'p', 0, G_OPTION_ARG_INT, &port,
		  "Port to listen on", "PORT" },
		{ "owner-password", 0, 0, G_OPTION_ARG_STRING, &owner_password,
		  "Password for the owner account created on first run", "PASSWORD" },
		{ "migrate", 0, 0, G_OPTION_ARG_NONE, &migrate_only,
		  "Apply schema migrations and exit", NULL },
		{ "no-ai", 0, 0, G_OPTION_ARG_NONE, &no_ai,
		  "Start without the AI service", NULL },
		{ "no-plugins", 0, 0, G_OPTION_ARG_NONE, &no_plugins,
		  "Start without loading plugins or venture types", NULL },
		{ "no-automation", 0, 0, G_OPTION_ARG_NONE, &no_automation,
		  "Start without the automation engine", NULL },
		{ "disable-module", 0, 0, G_OPTION_ARG_STRING_ARRAY, &disabled_modules,
		  "Turn a module off for this run (repeatable)", "NAME" },
		{ "list-modules", 0, 0, G_OPTION_ARG_NONE, &list_modules,
		  "Print every module with its resolved state and exit", NULL },
		{ "generate-config", 0, 0, G_OPTION_ARG_NONE, &generate_config,
		  "Print a commented default configuration and exit", NULL },
		{ "generate-c-config", 0, 0, G_OPTION_ARG_NONE, &generate_c_config,
		  "Print a compiled C configuration template and exit", NULL },
		{ "version", 'V', 0, G_OPTION_ARG_NONE, &show_version,
		  "Print the version and exit", NULL },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &show_license,
		  "Print licensing information and exit", NULL },
		{ NULL }
	};

	options = g_option_context_new("- ERP and CRM for a portfolio of ventures");
	g_option_context_add_main_entries(options, entries, NULL);
	g_option_context_set_description(options,
		"Examples:\n"
		"  venture                                  start with the default configuration\n"
		"  venture -c ~/.config/venture/config.yaml start with a specific configuration\n"
		"  venture -d sqlite:///srv/venture.db      use a particular database\n"
		"  venture -p 9000                          listen on another port\n"
		"  venture --migrate                        apply migrations and exit\n"
		"  venture --list-modules                   show which modules are on\n"
		"  venture --disable-module crm             run without a module\n"
		"  venture --generate-config > config.yaml  write a configuration to edit\n"
		"\n"
		"Configuration is layered: built-in defaults, then /etc/venture/config.yaml,\n"
		"then the user's config.yaml, then --config, then a compiled config.c beside\n"
		"it, then VENTURE_* environment variables, then these options.\n"
		"\n"
		"Secrets never live in the configuration file. Every secret setting names an\n"
		"environment variable to read instead.\n");

	if (!g_option_context_parse(options, &argc, &argv, &error))
	{
		g_printerr("%s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_INVALID_ARGUMENT);
	}

	if (show_version)
	{
		g_print("venture %s\n", venture_get_version_string());
		return 0;
	}

	if (show_license)
	{
		g_print("VENTURE %s\n"
		        "Copyright (C) 2026 Zach Podbielniak\n\n"
		        "Licensed under the GNU Affero General Public License, "
		        "version 3 or later.\n"
		        "This is free software: you are free to change and "
		        "redistribute it.\n"
		        "There is NO WARRANTY, to the extent permitted by law.\n\n"
		        "Source: https://gitlab.com/zachpodbielniak/venture\n",
		        venture_get_version_string());
		return 0;
	}

	if (generate_config)
	{
		g_print("%s", venture_config_get_default_yaml());
		return 0;
	}

	if (generate_c_config)
	{
		g_print("%s", venture_config_get_default_c_config());
		return 0;
	}

	config = venture_config_load(config_path, &error);

	if (NULL == config)
	{
		g_printerr("Configuration: %s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_CONFIG);
	}

	/* Command-line options are the last word, applied after every file and
	 * environment variable. */
	if (NULL != database_uri)
		venture_config_set_database_uri(config, database_uri);

	if (NULL != state_dir)
		g_object_set(config, "state-dir", state_dir, NULL);

	if (0 != port)
		g_object_set(config, "server-port", (gint64)port, NULL);

	{
		gsize i;

		for (i = 0; (NULL != disabled_modules) &&
		            (NULL != disabled_modules[i]); i++)
			venture_config_set_module_enabled(config, disabled_modules[i],
			                                  FALSE);
	}

	if (!venture_config_validate(config, &error))
	{
		g_printerr("Configuration: %s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_CONFIG);
	}

	/*
	 * Before the database, so the question "what would this
	 * configuration run" is answered without touching anything. Only the
	 * built-in modules are known here; a plugin's appears once the server
	 * is up, at /api/v1/modules and `venturectl modules`.
	 */
	if (list_modules)
	{
		g_autoptr(VentureModuleRegistry) modules = NULL;
		g_autoptr(GPtrArray) list = NULL;
		guint i;

		modules = venture_module_registry_new();
		venture_module_registry_register_builtins(modules);
		venture_module_registry_configure(modules, config, NULL);
		list = venture_module_registry_list(modules);

		g_print("%-12s %-9s %s\n", "MODULE", "STATE", "REQUIRES");

		for (i = 0; i < list->len; i++)
		{
			VentureModule *module;
			g_autofree gchar *requires = NULL;

			module = g_ptr_array_index(list, i);
			requires = g_strjoinv(", ",
				(gchar **)venture_module_get_requires(module));

			g_print("%-12s %-9s %s%s%s\n", venture_module_get_name(module),
			        venture_module_is_locked(module) ? "locked"
			        : venture_module_is_enabled(module) ? "enabled"
			                                             : "disabled",
			        requires,
			        venture_module_is_enabled(module) ? "" : "  -- ",
			        venture_module_is_enabled(module)
			                ? "" : venture_module_get_disabled_reason(module));
		}

		if (!venture_module_registry_check_configured(modules, config, &error))
		{
			g_printerr("\n%s\n(a plugin may provide it; this list knows "
			           "only the built-in modules)\n", error->message);
			g_clear_error(&error);
		}

		return 0;
	}

	venture_configure_logging(config);

	database = venture_database_new_for_config(config, &error);

	if (NULL == database)
	{
		g_printerr("Database: %s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_DATABASE);
	}

	context = venture_context_new(config, database);

	/*
	 * Plugins load before everything else that reads a registry, because a
	 * plugin may register record types, reports or venture types that the
	 * AI tools and the web routes then need to see.
	 *
	 * Crucially this is also before the migration: a plugin's record type
	 * needs its table, and the migration is what creates it. Loading
	 * plugins afterwards would leave a registered type whose every query
	 * fails on a missing table.
	 */
	if (!no_plugins)
	{
		plugins = venture_plugin_manager_new(context);

		if (!venture_plugin_manager_load_configured(plugins, &error))
		{
			/* Only a plugin listed in plugins.required gets here, and
			 * that list exists precisely so its absence is fatal. */
			g_printerr("Plugins: %s\n", error->message);
			return venture_error_to_exit_code(VENTURE_ERROR_PLUGIN);
		}

		venture_context_set_plugin_manager(context, plugins);

		if (venture_plugin_manager_get_count(plugins) > 0)
		{
			g_message("Loaded %u plugin%s",
			          venture_plugin_manager_get_count(plugins),
			          (1 == venture_plugin_manager_get_count(plugins))
			                  ? "" : "s");
		}
	}

	/*
	 * Every module that will ever exist in this process is registered
	 * now, so a configured name that matches none of them is a typo, and
	 * a typo that switches nothing is the failure this check exists for.
	 */
	if (!venture_module_registry_check_configured(
		venture_context_get_modules(context), config, &error))
	{
		g_printerr("Configuration: %s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_CONFIG);
	}

	{
		g_autoptr(GPtrArray) modules = NULL;
		g_autoptr(GString) off = NULL;
		guint i;
		guint on;

		modules = venture_module_registry_list(
			venture_context_get_modules(context));
		off = g_string_new(NULL);
		on = 0;

		for (i = 0; i < modules->len; i++)
		{
			VentureModule *module;

			module = g_ptr_array_index(modules, i);

			if (venture_module_is_enabled(module))
			{
				on++;
				continue;
			}

			if (off->len > 0)
				g_string_append(off, ", ");

			g_string_append(off, venture_module_get_name(module));
		}

		if (0 == off->len)
			g_message("Modules: all %u enabled", on);
		else
			g_message("Modules: %u enabled; disabled: %s", on, off->str);
	}

	{
		gboolean auto_migrate;

		g_object_get(config, "database-auto-migrate", &auto_migrate, NULL);

		if (auto_migrate || migrate_only)
		{
			if (!venture_database_migrate(database,
				venture_context_get_entity_registry(context), &error))
			{
				g_printerr("Migration: %s\n", error->message);
				return venture_error_to_exit_code(VENTURE_ERROR_MIGRATION);
			}
		}
	}

	if (migrate_only)
	{
		g_print("Schema is up to date.\n");
		return 0;
	}

	if (!no_ai)
	{
		ai = venture_ai_service_new(context, &error);

		if (NULL == ai)
		{
			/*
			 * A missing provider or key is a normal state, not a
			 * failure: the server is useful without AI and the chat
			 * dock explains its absence.
			 */
			g_message("AI is unavailable: %s", error->message);
			g_clear_error(&error);
		}
		else
		{
			venture_context_set_ai_service(context, ai);
		}
	}

	/*
	 * Knowledge bases, built once and shared. Unavailable is a normal
	 * state -- kb.enabled off, or no embedding service reachable -- and
	 * the server is useful without them, so it is a message rather than a
	 * failure to start.
	 */
	{
		g_autoptr(VentureKbService) kb = NULL;

		kb = venture_kb_service_new(context, &error);

		if (NULL == kb)
		{
			g_message("Knowledge bases are unavailable: %s",
			          error->message);
			g_clear_error(&error);
		}
		else
		{
			venture_context_set_kb_service(context, kb);
		}
	}

	if (!no_automation)
	{
		automation = venture_automation_new(context, &error);

		if (NULL == automation)
		{
			/* Disabled in configuration is a normal state, like AI. */
			g_message("Automation is unavailable: %s", error->message);
			g_clear_error(&error);
		}
		else
		{
			venture_context_set_automation(context, automation);

			if (!venture_automation_start(automation, &error))
			{
				/*
				 * A broken rules file must not stop the server: the
				 * operator still needs their data, and a message they
				 * can act on beats a refusal to start.
				 */
				g_warning("Automations not running: %s", error->message);
				g_clear_error(&error);
			}
		}
	}

	{
		g_autoptr(VentureWorkService) work = NULL;

		work = venture_work_service_new(context, &error);

		if (NULL == work)
		{
			/*
			 * Turned off is the default and a normal state, like AI
			 * and automation. Everything else about the forge
			 * integration works without it; this is only the part
			 * that spends money unattended.
			 */
			g_message("Coding runs are unavailable: %s", error->message);
			g_clear_error(&error);
		}
		else
		{
			venture_context_set_work_service(context, work);
		}
	}

	server = venture_web_server_new(context, &error);

	if (NULL == server)
	{
		g_printerr("Server: %s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_FAILED);
	}

	/* Creating the owner account before listening means a fresh install is
	 * reachable on its very first start. */
	{
		g_autoptr(VentureAuth) auth = NULL;
		g_autofree gchar *generated = NULL;

		auth = venture_auth_new(context);
		generated = venture_auth_ensure_owner(auth, owner_password, &error);

		if (NULL != error)
		{
			g_printerr("Cannot create the owner account: %s\n",
			           error->message);
			return venture_error_to_exit_code(VENTURE_ERROR_FAILED);
		}

		if (NULL != generated)
		{
			/* Printed once and never recoverable, which is the point:
			 * the alternative is an account with a known password. */
			g_print("\n  Created the owner account.\n"
			        "  Username: owner\n"
			        "  Password: %s\n\n"
			        "  This is shown once. Save it now.\n\n", generated);
		}
	}

	if (!venture_web_server_start(server, &error))
	{
		g_printerr("%s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_NETWORK);
	}

	application.loop = g_main_loop_new(NULL, FALSE);
	application.server = server;
	application.automation = automation;

	g_unix_signal_add(SIGINT, venture_on_signal, &application);
	g_unix_signal_add(SIGTERM, venture_on_signal, &application);

	g_print("VENTURE %s listening on %s\n", venture_get_version_string(),
	        venture_web_server_get_base_url(server));

	g_main_loop_run(application.loop);
	g_main_loop_unref(application.loop);

	return 0;
}
