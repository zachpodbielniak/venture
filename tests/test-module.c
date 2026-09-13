/*
 * test-module.c - Modules: switching pieces of VENTURE on and off
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module is a group of record types, reports, pages and services that an
 * install turns on or off as one thing. Everything here is about the two
 * promises that makes: that a disabled module disappears from every
 * surface at once, and that a module whose requirement is off is refused
 * rather than run half-working.
 */

#include <venture.h>

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

/* --- The registry on its own -------------------------------------------- */

/*
 * The built-in table is in dependency order: every requirement precedes
 * the module that names it.
 *
 * What breaks if this regresses: venture_module_registry_add() refuses a
 * requirement that is not registered yet, which is what rules a cycle out
 * by construction. A built-in written out of order would abort every
 * start.
 */
static void
test_module_builtins_are_in_dependency_order(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(GPtrArray) modules = NULL;
	g_autoptr(GHashTable) seen = NULL;
	guint i;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	modules = venture_module_registry_list(registry);
	seen = g_hash_table_new(g_str_hash, g_str_equal);

	g_assert_cmpuint(modules->len, >=, 14);

	for (i = 0; i < modules->len; i++)
	{
		VentureModule *module;
		const gchar *const *requires;
		gsize j;

		module = g_ptr_array_index(modules, i);
		requires = venture_module_get_requires(module);

		for (j = 0; NULL != requires[j]; j++)
			g_assert_true(g_hash_table_contains(seen, requires[j]));

		g_hash_table_add(seen, (gpointer)venture_module_get_name(module));
	}

	/* Registering twice is harmless: a test may build a registry that
	 * a helper has already populated. */
	venture_module_registry_register_builtins(registry);
	g_assert_cmpuint(venture_module_registry_list(registry)->len, ==,
	                 modules->len);
}

/*
 * With nothing configured, everything is on, and core is locked.
 */
static void
test_module_everything_is_on_by_default(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) modules = NULL;
	guint i;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	config = venture_config_new();

	g_assert_true(venture_module_registry_configure(registry, config, &error));
	g_assert_no_error(error);

	modules = venture_module_registry_list(registry);

	for (i = 0; i < modules->len; i++)
	{
		VentureModule *module;

		module = g_ptr_array_index(modules, i);
		if (!g_strcmp0(venture_module_get_name(module), "federation") ||
		    !g_strcmp0(venture_module_get_name(module), "stripe"))
		{
			g_assert_false(venture_module_is_enabled(module));
			continue;
		}
		g_assert_true(venture_module_is_enabled(module));
		g_assert_null(venture_module_get_disabled_reason(module));
	}

	g_assert_true(venture_module_is_locked(
		venture_module_registry_lookup(registry, "core")));
	g_assert_true(venture_module_registry_is_enabled(registry, "crm"));

	/* An unknown module is not on. A route gated by a name that does
	 * not exist must not open because the check found nothing. */
	g_assert_false(venture_module_registry_is_enabled(registry, "nope"));
	g_assert_null(venture_module_registry_lookup(registry, "nope"));
}

/*
 * A module whose requirement is off is refused, naming both.
 *
 * What breaks if this regresses: invoicing with the CRM off is a form
 * whose Bill-to picker is empty and a save that fails on every row. The
 * configuration should be refused in a second, not discovered on the
 * first invoice.
 */
static void
test_module_dependency_conflict_is_refused(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	VentureModule *invoicing;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	config = venture_config_new();
	venture_config_set_module_enabled(config, "crm", FALSE);

	g_assert_false(venture_module_registry_configure(registry, config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "invoicing"));
	g_assert_nonnull(strstr(error->message, "crm"));

	/* Resolved fully on failure: the dependent is off, with a reason
	 * that says why, so a caller carrying on gets a consistent state. */
	invoicing = venture_module_registry_lookup(registry, "invoicing");
	g_assert_false(venture_module_is_enabled(invoicing));
	g_assert_nonnull(strstr(venture_module_get_disabled_reason(invoicing),
	                        "requires crm"));

	g_assert_false(venture_module_registry_is_enabled(registry, "crm"));
	g_assert_cmpstr(venture_module_get_disabled_reason(
		venture_module_registry_lookup(registry, "crm")), ==,
		"modules.crm.enabled is false");

	/* Disabling the dependent too makes the configuration whole. */
	g_clear_error(&error);
	venture_config_set_module_enabled(config, "invoicing", FALSE);
	venture_config_set_module_enabled(config, "receivables", FALSE);
	g_assert_true(venture_module_registry_configure(registry, config, &error));
	g_assert_no_error(error);
	g_assert_true(venture_module_registry_is_enabled(registry, "sales"));
	g_assert_true(venture_module_registry_is_enabled(registry, "finance"));
}

/*
 * The older ai.enabled / kb.enabled / automation.enabled / forge.enabled /
 * plugins.enabled switches keep meaning what they always did, and a module
 * that requires one they turn off follows it.
 *
 * What breaks if this regresses: every existing install that set
 * ai.enabled: false would find the assistant back.
 */
static void
test_module_legacy_switches_still_count(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	config = venture_config_new();
	g_object_set(config, "ai-enabled", FALSE, "forge-enabled", FALSE, NULL);

	/* chat requires ai, so ai.enabled: false alone is a conflict... */
	g_assert_false(venture_module_registry_configure(registry, config, &error));
	g_assert_nonnull(strstr(error->message, "chat"));
	g_assert_cmpstr(venture_module_get_disabled_reason(
		venture_module_registry_lookup(registry, "ai")), ==,
		"ai.enabled is false");
	g_clear_error(&error);

	/* ...and disabling chat with it is not enough either, because the
	 * factory follows the forge. */
	venture_config_set_module_enabled(config, "chat", FALSE);
	g_assert_false(venture_module_registry_configure(registry, config, &error));
	g_assert_nonnull(strstr(error->message, "factory"));
	g_assert_cmpstr(venture_module_get_disabled_reason(
		venture_module_registry_lookup(registry, "forge")), ==,
		"forge.enabled is false");
	g_clear_error(&error);

	venture_config_set_module_enabled(config, "factory", FALSE);
	g_assert_true(venture_module_registry_configure(registry, config, &error));
	g_assert_no_error(error);
	g_assert_false(venture_module_registry_is_enabled(registry, "ai"));
	g_assert_false(venture_module_registry_is_enabled(registry, "chat"));
	g_assert_false(venture_module_registry_is_enabled(registry, "forge"));
	g_assert_false(venture_module_registry_is_enabled(registry, "factory"));
	g_assert_true(venture_module_registry_is_enabled(registry, "tickets"));
}

/*
 * A locked module ignores the switch.
 */
static void
test_module_core_cannot_be_disabled(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	config = venture_config_new();
	venture_config_set_module_enabled(config, "core", FALSE);

	g_assert_true(venture_module_registry_configure(registry, config, &error));
	g_assert_no_error(error);
	g_assert_true(venture_module_registry_is_enabled(registry, "core"));
}

/*
 * A configured name that is no module is an error listing what is.
 *
 * What breaks if this regresses: `modules: {crn: false}` switches nothing
 * and says nothing, which is the most confusing configuration failure
 * there is.
 */
static void
test_module_unknown_name_is_reported(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	config = venture_config_new();

	g_assert_true(venture_module_registry_check_configured(registry, config,
	                                                       &error));

	venture_config_set_module_enabled(config, "crn", FALSE);
	venture_config_set_module_enabled(config, "crm", FALSE);

	g_assert_false(venture_module_registry_check_configured(registry, config,
	                                                        &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "crn"));
	g_assert_nonnull(strstr(error->message, "Known modules"));
	g_assert_nonnull(strstr(error->message, "invoicing"));
}

/*
 * A plugin's module is held to the same rules, and is resolved against the
 * running configuration the moment it is added.
 */
static const gchar *const test_module_requires_crm[] = { "crm", NULL };
static const gchar *const test_module_requires_self[] = { "loop", NULL };
static const gchar *const test_module_requires_missing[] = { "nothing", NULL };
static GType (*const test_module_claims_contact[]) (void) = {
	venture_contact_get_type, NULL
};

static void
test_module_add_enforces_the_rules(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	VentureModuleInfo info;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	config = venture_config_new();
	venture_config_set_module_enabled(config, "acme", FALSE);
	g_assert_true(venture_module_registry_configure(registry, config, NULL));

	memset(&info, 0, sizeof(info));

	/* A malformed name. */
	info.name = "Acme Corp";
	g_assert_false(venture_module_registry_add(registry, &info,
	                                           VENTURE_MODULE_ORIGIN_PLUGIN,
	                                           &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* A taken name. */
	info.name = "crm";
	g_assert_false(venture_module_registry_add(registry, &info,
	                                           VENTURE_MODULE_ORIGIN_PLUGIN,
	                                           &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	/* A requirement nobody registered. */
	info.name = "acme";
	info.requires = test_module_requires_missing;
	g_assert_false(venture_module_registry_add(registry, &info,
	                                           VENTURE_MODULE_ORIGIN_PLUGIN,
	                                           &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* Requiring itself. */
	info.name = "loop";
	info.requires = test_module_requires_self;
	g_assert_false(venture_module_registry_add(registry, &info,
	                                           VENTURE_MODULE_ORIGIN_PLUGIN,
	                                           &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Claiming a type another module owns. */
	info.name = "acme";
	info.requires = test_module_requires_crm;
	info.entity_types = test_module_claims_contact;
	g_assert_false(venture_module_registry_add(registry, &info,
	                                           VENTURE_MODULE_ORIGIN_PLUGIN,
	                                           &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_assert_nonnull(strstr(error->message, "contact"));
	g_clear_error(&error);

	/* A good one: added, and resolved against the configuration that
	 * was already applied, which turned it off. */
	info.entity_types = NULL;
	g_assert_true(venture_module_registry_add(registry, &info,
	                                          VENTURE_MODULE_ORIGIN_PLUGIN,
	                                          &error));
	g_assert_no_error(error);
	g_assert_false(venture_module_registry_is_enabled(registry, "acme"));
	g_assert_cmpint(venture_module_get_origin(
		venture_module_registry_lookup(registry, "acme")), ==,
		VENTURE_MODULE_ORIGIN_PLUGIN);

	/* Its dependency lists it now. */
	{
		g_auto(GStrv) dependents = NULL;

		dependents = venture_module_registry_get_dependents(registry, "crm");
		g_assert_true(g_strv_contains((const gchar *const *)dependents,
		                              "acme"));
		g_assert_true(g_strv_contains((const gchar *const *)dependents,
		                              "invoicing"));
	}

	/* And check_configured knows it, so the switch is no longer a
	 * mystery name. */
	g_assert_true(venture_module_registry_check_configured(registry, config,
	                                                       &error));
	g_assert_no_error(error);
}

/*
 * The JSON description carries everything the Modules page and venturectl
 * show, including who needs each module.
 */
static void
test_module_describe(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonArray *modules;
	gboolean found_crm = FALSE;
	guint i;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	config = venture_config_new();
	venture_config_set_module_enabled(config, "factory", FALSE);
	g_assert_true(venture_module_registry_configure(registry, config, NULL));

	node = venture_module_registry_describe(registry);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(node));
	modules = json_node_get_array(node);

	for (i = 0; i < json_array_get_length(modules); i++)
	{
		JsonObject *module;
		const gchar *name;

		module = json_array_get_object_element(modules, i);
		name = json_object_get_string_member(module, "name");

		if (0 == g_strcmp0(name, "crm"))
		{
			JsonArray *types;
			JsonArray *needed;

			found_crm = TRUE;
			types = json_object_get_array_member(module, "entity_types");
			needed = json_object_get_array_member(module, "required_by");

			g_assert_true(json_object_get_boolean_member(module, "enabled"));
			g_assert_cmpuint(json_array_get_length(types), ==, 4);
			g_assert_cmpstr(json_array_get_string_element(types, 0), ==,
			                "company");
			g_assert_cmpuint(json_array_get_length(needed), >=, 1);
		}

		if (0 == g_strcmp0(name, "factory"))
		{
			g_assert_false(json_object_get_boolean_member(module, "enabled"));
			g_assert_cmpstr(json_object_get_string_member(module,
			                                              "disabled_reason"),
			                ==, "modules.factory.enabled is false");
		}
	}

	g_assert_true(found_crm);
}

/* --- Configuration -------------------------------------------------------- */

/*
 * Both spellings of the modules section are read, and a switch that was
 * never written is unset rather than false.
 */
static void
test_module_config_reads_both_spellings(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) names = NULL;
	gboolean enabled;

	config = venture_config_new();

	g_assert_true(venture_config_apply_yaml_string(config,
		"modules:\n"
		"  crm: false\n"
		"  kb:\n"
		"    enabled: true\n"
		"  forge:\n"
		"    enabled: false\n", &error));
	g_assert_no_error(error);

	g_assert_true(venture_config_get_module_switch(config, "crm", &enabled));
	g_assert_false(enabled);
	g_assert_true(venture_config_get_module_switch(config, "kb", &enabled));
	g_assert_true(enabled);
	g_assert_true(venture_config_get_module_switch(config, "forge", &enabled));
	g_assert_false(enabled);

	/* Unset is not false. */
	g_assert_false(venture_config_get_module_switch(config, "sales", &enabled));

	names = venture_config_list_module_switches(config);
	g_assert_cmpuint(g_strv_length(names), ==, 3);
	g_assert_cmpstr(names[0], ==, "crm");

	venture_config_clear_module_switch(config, "crm");
	g_assert_false(venture_config_get_module_switch(config, "crm", NULL));
}

/*
 * The shipped default configuration carries an (empty) modules section
 * that parses, and a generated file writes the switches back out in a
 * form the parser reads.
 */
static void
test_module_config_round_trips(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(VentureConfig) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	gboolean enabled;

	config = venture_config_new();
	g_assert_true(venture_config_apply_yaml_string(
		config, venture_config_get_default_yaml(), &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(venture_config_get_default_yaml(), "\nmodules:"));

	venture_config_set_module_enabled(config, "outreach", FALSE);
	yaml = venture_config_to_yaml(config, TRUE);
	g_assert_nonnull(strstr(yaml, "modules:"));
	g_assert_nonnull(strstr(yaml, "  outreach:\n    enabled: false"));

	again = venture_config_new();
	g_assert_true(venture_config_apply_yaml_string(again, yaml, &error));
	g_assert_no_error(error);
	g_assert_true(venture_config_get_module_switch(again, "outreach", &enabled));
	g_assert_false(enabled);
}

/*
 * VENTURE_MODULE_<NAME> switches a module from the environment, the way
 * VENTURE_<SECTION>_<KEY> sets a setting, and the settings description
 * lists the switch beside everything else.
 */
static void
test_module_config_environment_and_describe(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(JsonNode) described = NULL;
	JsonArray *settings;
	gboolean enabled;
	gboolean found = FALSE;
	guint i;

	g_setenv("VENTURE_MODULE_OUTREACH", "false", TRUE);

	config = venture_config_new();
	venture_config_apply_environment(config);

	g_unsetenv("VENTURE_MODULE_OUTREACH");

	g_assert_true(venture_config_get_module_switch(config, "outreach",
	                                               &enabled));
	g_assert_false(enabled);

	described = venture_config_describe(config);
	settings = json_node_get_array(described);

	for (i = 0; i < json_array_get_length(settings); i++)
	{
		JsonObject *setting;

		setting = json_array_get_object_element(settings, i);

		if (0 == g_strcmp0(json_object_get_string_member(setting, "section"),
		                   "modules"))
		{
			found = TRUE;
			g_assert_cmpstr(json_object_get_string_member(setting, "key"),
			                ==, "outreach.enabled");
			g_assert_cmpstr(json_object_get_string_member(setting, "env"),
			                ==, "VENTURE_MODULE_OUTREACH");
			g_assert_false(json_object_get_boolean_member(setting, "value"));
		}
	}

	g_assert_true(found);
}

/*
 * Validation catches the conflict before anything is opened.
 */
static void
test_module_config_validate_refuses_conflict(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);

	venture_config_set_module_enabled(config, "tickets", FALSE);

	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "forge"));
	g_assert_nonnull(strstr(error->message, "tickets"));
}

/* --- Applied to a context ------------------------------------------------- */

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	venture_config_set_module_enabled(fixture->config, "crm", FALSE);
	venture_config_set_module_enabled(fixture->config, "invoicing", FALSE);
	venture_config_set_module_enabled(fixture->config, "receivables", FALSE);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);

	/* Migrated after the context, because the context is what masks the
	 * registry: a disabled module's tables are not created. */
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	/* The entity registry is process-wide. Put every type back so the
	 * next test in this binary starts from the default. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/*
 * A disabled module's types are hidden from the registry, and its reports
 * from the report registry, while the rest stay.
 *
 * What breaks if this regresses: the one loop in
 * venture_entity_registry_list_names() is what makes a disabled module
 * disappear from REST, the schema, the forms, the AI tools and the CLI at
 * once. Everything else asks the registry.
 */
static void
test_module_context_masks_registries(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureEntityRegistry *entities;
	VentureReportRegistry *reports;
	g_auto(GStrv) names = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	(void)user_data;

	entities = venture_context_get_entity_registry(fixture->context);
	reports = venture_context_get_report_registry(fixture->context);

	g_assert_false(venture_context_module_enabled(fixture->context, "crm"));
	g_assert_true(venture_context_module_enabled(fixture->context, "sales"));

	g_assert_cmpuint(venture_entity_registry_lookup(entities, "contact"), ==,
	                 G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(entities, "contacts"), ==,
	                 G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(entities, "invoice"), ==,
	                 G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(entities, "sale"), ==,
	                 VENTURE_TYPE_SALE);

	/* Still known, just not offered. */
	g_assert_cmpuint(venture_entity_registry_lookup_any(entities, "contact"),
	                 ==, VENTURE_TYPE_CONTACT);
	g_assert_false(venture_entity_registry_is_type_enabled(entities, "contact"));
	g_assert_true(venture_entity_registry_is_type_enabled(entities, "sale"));
	g_assert_cmpstr(venture_entity_registry_get_type_module(entities, "sale"),
	                ==, "sales");
	g_assert_cmpstr(venture_entity_registry_get_type_module(entities, "contact"),
	                ==, "crm");

	names = venture_entity_registry_list_names(entities);
	g_assert_false(g_strv_contains((const gchar *const *)names, "contact"));
	g_assert_false(g_strv_contains((const gchar *const *)names, "company"));
	g_assert_false(g_strv_contains((const gchar *const *)names, "invoice"));
	g_assert_true(g_strv_contains((const gchar *const *)names, "sale"));
	g_assert_true(g_strv_contains((const gchar *const *)names, "ticket"));

	schema = venture_entity_registry_describe_all(entities);

	for (i = 0; i < json_array_get_length(json_node_get_array(schema)); i++)
	{
		JsonObject *entry;

		entry = json_array_get_object_element(json_node_get_array(schema), i);
		g_assert_cmpstr(json_object_get_string_member(entry, "name"), !=,
		                "contact");
	}

	/* Creating by name says which switch, not "no such type". */
	g_assert_null(venture_entity_registry_create(entities, "contact", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_nonnull(strstr(error->message, "crm module"));
	g_assert_nonnull(strstr(error->message, "modules.crm.enabled"));
	g_clear_error(&error);

	g_assert_null(venture_entity_registry_create(entities, "contakt", &error));
	g_assert_nonnull(strstr(error->message, "Known types"));

	/* Reports follow their module. */
	g_assert_null(venture_report_registry_lookup(reports, "pipeline"));
	g_assert_null(venture_report_registry_lookup(reports, "receivables"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "pnl"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "inventory"));

	/* The module registry knows the type's owner too. */
	g_assert_cmpstr(venture_module_get_name(
		venture_module_registry_get_module_for_type(
			venture_context_get_modules(fixture->context), "deal")), ==,
		"crm");
	g_assert_null(venture_module_registry_get_module_for_type(
		venture_context_get_modules(fixture->context), "nothing"));
}

/*
 * Writing a reference into a disabled module is refused, with the switch
 * named; a record that keeps a value it already had is left alone.
 *
 * What breaks if this regresses: a sale pointing at contact #7 with the
 * CRM off cannot be checked, and a reference nobody can check is a
 * dangling one waiting to happen.
 */
static void
test_module_save_refuses_reference_into_disabled_module(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSale) sale = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	venture = venture_venture_new();
	g_object_set(venture, "name", "Imprint", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(venture), NULL, &error));
	g_assert_no_error(error);

	sale = venture_sale_new();
	g_object_set(sale,
	             "venture-id", venture_entity_get_id(VENTURE_ENTITY(venture)),
	             "contact-id", (gint64)7,
	             NULL);

	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(sale), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "sale.contact_id"));
	g_assert_nonnull(strstr(error->message, "crm module"));
	g_clear_error(&error);

	g_object_set(sale, "contact-id", (gint64)0, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(sale), NULL, &error));
	g_assert_no_error(error);
}

/*
 * A context built from a configuration with everything on shows every
 * type again -- the mask is per configuration, not permanent.
 */
static void
test_module_reapply_restores_types(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureContext) context = NULL;

	(void)user_data;

	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "contact"), ==, G_TYPE_INVALID);

	everything = venture_config_new();
	context = venture_context_new(everything, fixture->database);

	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "contact"), ==,
		VENTURE_TYPE_CONTACT);
	g_assert_nonnull(venture_report_registry_lookup(
		venture_context_get_report_registry(context), "pipeline"));
}

/*
 * A plugin registers a module through the context in one call: its types
 * are registered and masked according to the configuration.
 */
static const gchar *const test_module_plugin_requires[] = { "sales", NULL };

static void
test_module_context_registers_a_plugin_module(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	VentureModuleInfo info;

	(void)user_data;

	memset(&info, 0, sizeof(info));
	info.name = "royalties";
	info.label = "Royalties";
	info.requires = test_module_plugin_requires;

	g_assert_true(venture_context_register_module(fixture->context, &info,
	                                              &error));
	g_assert_no_error(error);
	g_assert_true(venture_context_module_enabled(fixture->context,
	                                             "royalties"));

	/* One requiring the disabled CRM is refused with the reason. */
	info.name = "crm-extras";
	info.requires = test_module_requires_crm;
	g_assert_false(venture_context_register_module(fixture->context, &info,
	                                               &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "crm"));
}

/* --- Over HTTP ------------------------------------------------------------ */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

/*
 * One request, driving the main context by hand: the server runs on this
 * same context, so a blocking send would wait for a listener that cannot
 * run until the send returns.
 */
static guint
server_request(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *form_body,
	gchar		**out_body,
	gchar		**out_set_cookie
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != fixture->cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	if (NULL != form_body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(form_body, strlen(form_body));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	if (NULL != out_set_cookie)
		*out_set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "module-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-modules-XXXXXX", NULL);

	/* A different offset from test-auth's, so the two suites can run at
	 * once without colliding. */
	fixture->port = (guint16)(20000 + ((getpid() + 4099) % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);
	venture_config_set_module_enabled(fixture->config, "crm", FALSE);
	venture_config_set_module_enabled(fixture->config, "invoicing", FALSE);
	venture_config_set_module_enabled(fixture->config, "receivables", FALSE);
	venture_config_set_module_enabled(fixture->config, "tickets", FALSE);
	venture_config_set_module_enabled(fixture->config, "forge", FALSE);
	venture_config_set_module_enabled(fixture->config, "factory", FALSE);
	venture_config_set_module_enabled(fixture->config, "kb", FALSE);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	g_assert_cmpuint(server_request(fixture, "POST", "/login",
	                                "username=owner&password=owner-password-1",
	                                NULL, &set_cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(set_cookie);

	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/*
 * A disabled module answers 404 on every surface: its generic record
 * routes, its own pages, its API routes, and the schema.
 *
 * What breaks if this regresses: the whole point. A module that is "off"
 * but still answers is not off.
 */
static void
test_module_http_disabled_module_is_absent(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *schema = NULL;
	g_autofree gchar *page = NULL;

	(void)user_data;

	/* Generic record routes. */
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/contact", NULL,
	                                &body, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_nonnull(strstr(body, "crm module"));
	g_clear_pointer(&body, g_free);

	g_assert_cmpuint(server_request(fixture, "GET", "/e/contact", NULL, NULL,
	                                NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "POST", "/api/v1/company", NULL,
	                                NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);

	/* Still-enabled ones work. */
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/sale", NULL, NULL,
	                                NULL), ==, SOUP_STATUS_OK);
	g_assert_cmpuint(server_request(fixture, "GET", "/e/sale", NULL, NULL,
	                                NULL), ==, SOUP_STATUS_OK);

	/* Module pages and API routes. */
	g_assert_cmpuint(server_request(fixture, "GET", "/tickets", NULL, &page,
	                                NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_nonnull(strstr(page, "tickets module is turned off"));
	g_clear_pointer(&page, g_free);

	g_assert_cmpuint(server_request(fixture, "GET", "/kb", NULL, NULL, NULL),
	                 ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET",
	                                "/api/v1/kb/search?q=anything", NULL, NULL,
	                                NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "POST", "/forges/1/verify", "",
	                                NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "POST", "/hooks/forge/1", "{}",
	                                NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET", "/invoices/1/print", NULL,
	                                NULL, NULL), ==, SOUP_STATUS_NOT_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET", "/automations", NULL, NULL,
	                                NULL), ==, SOUP_STATUS_OK);

	/* The schema does not describe what is off. */
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/schema", NULL,
	                                &schema, NULL), ==, SOUP_STATUS_OK);
	/* As a type of its own. Other types still carry a reference field
	 * naming contact, and should: the column is there, only its target
	 * is off. */
	g_assert_null(strstr(schema, "\"name\" : \"contact\""));
	g_assert_null(strstr(schema, "\"name\" : \"ticket\""));
	g_assert_nonnull(strstr(schema, "\"name\" : \"sale\""));
}

/*
 * The sidebar offers only what is on, and the Modules page and endpoint
 * say why the rest is off.
 */
static void
test_module_http_navigation_and_modules_page(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *page = NULL;
	g_autofree gchar *modules = NULL;
	g_autofree gchar *json = NULL;

	(void)user_data;

	g_assert_cmpuint(server_request(fixture, "GET", "/", NULL, &page, NULL),
	                 ==, SOUP_STATUS_OK);
	g_assert_null(strstr(page, "href=\"/e/contact\""));
	g_assert_null(strstr(page, "href=\"/tickets\""));
	g_assert_null(strstr(page, "href=\"/e/forge_repo\""));
	g_assert_nonnull(strstr(page, "href=\"/e/sale\""));
	g_assert_nonnull(strstr(page, "href=\"/modules\""));

	/* The "Relations" section had only CRM links; with them gone the
	 * heading goes too. "Code" likewise. */
	g_assert_null(strstr(page, ">Relations<"));
	g_assert_null(strstr(page, ">Code<"));
	g_assert_nonnull(strstr(page, ">Money<"));

	g_assert_cmpuint(server_request(fixture, "GET", "/modules", NULL,
	                                &modules, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(modules, "modules.crm.enabled is false"));
	g_assert_nonnull(strstr(modules, "always on"));

	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/modules", NULL,
	                                &json, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(json, "\"name\" : \"crm\""));
	g_assert_nonnull(strstr(json, "\"required_by\""));
}

/*
 * Anonymous requests into a disabled module's page are sent to sign in,
 * not told which modules the install runs.
 */
static void
test_module_http_anonymous_is_redirected_first(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cookie = NULL;

	(void)user_data;

	cookie = g_steal_pointer(&fixture->cookie);

	g_assert_cmpuint(server_request(fixture, "GET", "/tickets", NULL, NULL,
	                                NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/modules", NULL,
	                                NULL, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	g_assert_cmpuint(server_request(fixture, "GET", "/modules", NULL, NULL,
	                                NULL), ==, SOUP_STATUS_FOUND);

	fixture->cookie = g_steal_pointer(&cookie);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/module/builtins-are-in-dependency-order",
	                test_module_builtins_are_in_dependency_order);
	g_test_add_func("/module/everything-is-on-by-default",
	                test_module_everything_is_on_by_default);
	g_test_add_func("/module/dependency-conflict-is-refused",
	                test_module_dependency_conflict_is_refused);
	g_test_add_func("/module/legacy-switches-still-count",
	                test_module_legacy_switches_still_count);
	g_test_add_func("/module/core-cannot-be-disabled",
	                test_module_core_cannot_be_disabled);
	g_test_add_func("/module/unknown-name-is-reported",
	                test_module_unknown_name_is_reported);
	g_test_add_func("/module/add-enforces-the-rules",
	                test_module_add_enforces_the_rules);
	g_test_add_func("/module/describe", test_module_describe);

	g_test_add_func("/module/config/reads-both-spellings",
	                test_module_config_reads_both_spellings);
	g_test_add_func("/module/config/round-trips",
	                test_module_config_round_trips);
	g_test_add_func("/module/config/environment-and-describe",
	                test_module_config_environment_and_describe);
	g_test_add_func("/module/config/validate-refuses-conflict",
	                test_module_config_validate_refuses_conflict);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/module/context/masks-registries",
	    test_module_context_masks_registries);
	ADD("/module/context/save-refuses-reference-into-disabled-module",
	    test_module_save_refuses_reference_into_disabled_module);
	ADD("/module/context/reapply-restores-types",
	    test_module_reapply_restores_types);
	ADD("/module/context/registers-a-plugin-module",
	    test_module_context_registers_a_plugin_module);

#undef ADD
#define ADD(path, func) \
	g_test_add(path, ServerFixture, NULL, server_fixture_set_up, func, \
	           server_fixture_tear_down)

	ADD("/module/http/disabled-module-is-absent",
	    test_module_http_disabled_module_is_absent);
	ADD("/module/http/navigation-and-modules-page",
	    test_module_http_navigation_and_modules_page);
	ADD("/module/http/anonymous-is-redirected-first",
	    test_module_http_anonymous_is_redirected_first);

	return g_test_run();
}
