/*
 * test-plugin.c - Plugins, declarative venture types and automation
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The claim these tests exist to check is that an extension is
 * indistinguishable from a built-in. A plugin's record type must gain a
 * table, a REST-shaped serialisation and AI visibility with no extra code,
 * and a plugin's report must appear in the same registry every consumer
 * reads.
 */

#include <venture.h>

#include <glib/gstdio.h>

#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureDatabase	*database;
	VentureConfig	*config;
	VentureContext	*context;
	gchar		*plugin_dir;
	gchar		*type_dir;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);

	/* A scratch directory per test, so one test's definitions cannot
	 * leak into another's expectations. */
	fixture->plugin_dir = g_dir_make_tmp("venture-plugins-XXXXXX", NULL);
	fixture->type_dir = g_dir_make_tmp("venture-types-XXXXXX", NULL);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	venture_test_remove_tree(fixture->plugin_dir);
	venture_test_remove_tree(fixture->type_dir);

	g_clear_pointer(&fixture->plugin_dir, g_free);
	g_clear_pointer(&fixture->type_dir, g_free);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

static void
write_file(
	const gchar	*directory,
	const gchar	*name,
	const gchar	*contents
){
	g_autofree gchar *path = NULL;

	path = g_build_filename(directory, name, NULL);
	g_assert_true(g_file_set_contents(path, contents, -1, NULL));
}

/* ==========================================================================
 * Declarative venture types
 * ========================================================================== */

static void
test_venture_type_parses_yaml(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureType) type = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *fields;

	type = venture_venture_type_new_from_yaml(
		"name: etsy\n"
		"label: Etsy shop\n"
		"description: A handmade storefront\n"
		"fields:\n"
		"  shop_name: string\n"
		"  listing_fee:\n"
		"    type: money\n"
		"    label: Listing fee\n"
		"  material:\n"
		"    type: enum\n"
		"    choices: [wood, acrylic, paper]\n"
		"metrics: [revenue, units_sold]\n", &error);

	g_assert_no_error(error);
	g_assert_nonnull(type);

	g_assert_cmpstr(venture_venture_type_get_name(type), ==, "etsy");
	g_assert_cmpstr(venture_venture_type_get_label(type), ==, "Etsy shop");

	fields = venture_venture_type_get_fields(type);
	g_assert_cmpuint(fields->len, ==, 3);

	/* Metrics are read whether written as a list or a bare scalar. */
	g_assert_true(g_strv_contains(venture_venture_type_get_metrics(type),
	                              "revenue"));
}

static void
test_venture_type_shorthand_field(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureType) type = NULL;
	GPtrArray *fields;
	VentureFieldSpec *spec;

	/* `sku: string` is shorthand for a field of that type with everything
	 * else defaulted. Most declarations are that simple and should read
	 * that way. */
	type = venture_venture_type_new_from_yaml(
		"name: simple\nfields:\n  sku: string\n", NULL);

	fields = venture_venture_type_get_fields(type);
	spec = g_ptr_array_index(fields, 0);

	g_assert_cmpstr(venture_field_spec_get_name(spec), ==, "sku");
	g_assert_cmpint(venture_field_spec_get_kind(spec), ==,
	                VENTURE_FIELD_KIND_STRING);
}

static void
test_venture_type_requires_a_name(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureType) type = NULL;
	g_autoptr(GError) error = NULL;

	type = venture_venture_type_new_from_yaml("label: Nameless\n", &error);

	g_assert_null(type);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_venture_type_rejects_unknown_field_type(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureType) type = NULL;
	g_autoptr(GError) error = NULL;

	type = venture_venture_type_new_from_yaml(
		"name: bad\nfields:\n  thing: notatype\n", &error);

	g_assert_null(type);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	/* The message lists what is valid, because the person reading it is
	 * mid-edit in a YAML file. */
	g_assert_nonnull(g_strstr_len(error->message, -1, "money"));
}

static void
test_venture_type_enum_needs_choices(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureType) type = NULL;
	g_autoptr(GError) error = NULL;

	type = venture_venture_type_new_from_yaml(
		"name: bad\nfields:\n  thing:\n    type: enum\n", &error);

	g_assert_null(type);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_venture_type_validates_a_venture(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureType) type = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(GError) error = NULL;

	type = venture_venture_type_new_from_yaml(
		"name: etsy\n"
		"fields:\n"
		"  shop_name:\n"
		"    type: string\n"
		"    required: true\n"
		"  material:\n"
		"    type: enum\n"
		"    choices: [wood, acrylic]\n", NULL);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Shop", "venture-type", "etsy", NULL);

	/* A declared required field really is required, even though it lives
	 * in the attribute bag rather than a column. */
	g_assert_false(venture_venture_type_validate_venture(type,
		VENTURE_ENTITY(venture), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

	g_clear_error(&error);
	venture_entity_set_attribute(VENTURE_ENTITY(venture), "shop_name",
	                             "Woodwork");
	g_assert_true(venture_venture_type_validate_venture(type,
		VENTURE_ENTITY(venture), &error));
	g_assert_no_error(error);

	/* And a declared enum really rejects a value outside its choices. */
	venture_entity_set_attribute(VENTURE_ENTITY(venture), "material",
	                             "unobtainium");
	g_assert_false(venture_venture_type_validate_venture(type,
		VENTURE_ENTITY(venture), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_venture_type_registry_loads_directory(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureTypeRegistry) registry = NULL;
	guint loaded;

	write_file(fixture->type_dir, "etsy.yaml",
	           "name: etsy\nlabel: Etsy shop\nfields:\n  shop_name: string\n");
	write_file(fixture->type_dir, "books.yaml",
	           "name: books\nlabel: Book imprint\nfields:\n  imprint: string\n");
	/* A non-YAML file in the directory must simply be ignored. */
	write_file(fixture->type_dir, "notes.txt", "ignore me\n");

	registry = venture_venture_type_registry_new();
	loaded = venture_venture_type_registry_load_directory(registry,
	                                                      fixture->type_dir,
	                                                      NULL);

	g_assert_cmpuint(loaded, ==, 2);
	g_assert_nonnull(venture_venture_type_registry_lookup(registry, "etsy"));
	g_assert_nonnull(venture_venture_type_registry_lookup(registry, "books"));
	g_assert_null(venture_venture_type_registry_lookup(registry, "notes"));
}

static void
test_venture_type_registry_skips_broken_definition(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureTypeRegistry) registry = NULL;
	guint loaded;

	write_file(fixture->type_dir, "good.yaml",
	           "name: good\nfields:\n  thing: string\n");
	write_file(fixture->type_dir, "broken.yaml",
	           "name: broken\nfields:\n  thing: notatype\n");

	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING,
	                      "*Skipping venture type*");

	registry = venture_venture_type_registry_new();
	loaded = venture_venture_type_registry_load_directory(registry,
	                                                      fixture->type_dir,
	                                                      NULL);

	g_test_assert_expected_messages();

	/* One bad definition must not cost the operator the others. */
	g_assert_cmpuint(loaded, ==, 1);
	g_assert_nonnull(venture_venture_type_registry_lookup(registry, "good"));
}

static void
test_venture_type_registry_missing_directory_is_not_an_error(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureTypeRegistry) registry = NULL;

	registry = venture_venture_type_registry_new();

	/* A fresh install has no custom types; that is not a failure. */
	g_assert_cmpuint(venture_venture_type_registry_load_directory(
		registry, "/nonexistent/venture/types", NULL), ==, 0);
}

static void
test_venture_type_to_json_describes_fields(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVentureType) type = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;
	JsonArray *fields;

	type = venture_venture_type_new_from_yaml(
		"name: etsy\nfields:\n  shop_name: string\n  fee: money\n", NULL);

	node = venture_venture_type_to_json(type);
	object = json_node_get_object(node);

	g_assert_cmpstr(json_object_get_string_member(object, "name"), ==, "etsy");

	fields = json_object_get_array_member(object, "fields");
	g_assert_cmpuint(json_array_get_length(fields), ==, 2);
}

/* ==========================================================================
 * The plugin manager
 * ========================================================================== */

static void
test_plugin_manager_loads_venture_types(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;

	write_file(fixture->plugin_dir, "etsy.yaml",
	           "name: etsy\nlabel: Etsy shop\nfields:\n  shop_name: string\n");

	manager = venture_plugin_manager_new(fixture->context);
	g_assert_cmpuint(venture_plugin_manager_load_directory(manager,
		fixture->plugin_dir, &error), ==, 1);
	g_assert_no_error(error);

	/*
	 * Deliberately asserted against the *context's* registry rather than
	 * the manager's own accessor. Everything that consumes venture types
	 * -- the API, the web UI, validation -- reads the context, so loading
	 * into a private registry would pass a manager-shaped test and still
	 * leave /api/v1/venture-types empty.
	 */
	g_assert_nonnull(venture_venture_type_registry_lookup(
		venture_context_get_venture_types(fixture->context), "etsy"));

	g_assert_nonnull(venture_venture_type_registry_lookup(
		venture_plugin_manager_get_venture_types(manager), "etsy"));
}

static void
test_plugin_manager_rejects_unknown_extension(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	write_file(fixture->plugin_dir, "thing.txt", "not a plugin\n");
	path = g_build_filename(fixture->plugin_dir, "thing.txt", NULL);

	manager = venture_plugin_manager_new(fixture->context);

	g_assert_false(venture_plugin_manager_load_file(manager, path, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
}

static void
test_plugin_manager_skips_broken_plugin(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	guint loaded;

	write_file(fixture->plugin_dir, "good.yaml",
	           "name: good\nfields:\n  thing: string\n");
	write_file(fixture->plugin_dir, "broken.yaml",
	           "name: broken\nfields:\n  thing: notatype\n");

	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*Skipping plugin*");

	manager = venture_plugin_manager_new(fixture->context);
	loaded = venture_plugin_manager_load_directory(manager,
	                                               fixture->plugin_dir, NULL);

	g_test_assert_expected_messages();

	/*
	 * One broken plugin must not stop the server: the operator still
	 * needs the rest of their data.
	 */
	g_assert_cmpuint(loaded, ==, 1);
}

static void
test_plugin_manager_loads_each_file_once(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autofree gchar *path = NULL;

	write_file(fixture->plugin_dir, "etsy.yaml",
	           "name: etsy\nfields:\n  shop_name: string\n");
	path = g_build_filename(fixture->plugin_dir, "etsy.yaml", NULL);

	manager = venture_plugin_manager_new(fixture->context);

	g_assert_true(venture_plugin_manager_load_file(manager, path, NULL));
	g_assert_true(venture_plugin_manager_load_file(manager, path, NULL));

	/* Two configured directories can overlap; loading twice would
	 * otherwise register a type twice. */
	g_assert_cmpuint(venture_plugin_manager_get_count(manager), ==, 1);
}

static void
test_plugin_manager_missing_directory_is_not_an_error(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;

	manager = venture_plugin_manager_new(fixture->context);

	g_assert_cmpuint(venture_plugin_manager_load_directory(manager,
		"/nonexistent/venture/plugins", NULL), ==, 0);
}

static void
test_plugin_manager_required_plugin_is_fatal(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *required[] = { "definitely-not-here.so", NULL };

	g_object_set(fixture->config, "plugins-required", required, NULL);

	manager = venture_plugin_manager_new(fixture->context);

	/*
	 * The point of plugins.required is that its absence IS fatal: an
	 * install that depends on a plugin should not start half-configured
	 * and quietly behave differently.
	 */
	g_assert_false(venture_plugin_manager_load_configured(manager, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
}

static void
test_plugin_manager_disabled_loads_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;

	g_object_set(fixture->config, "plugins-enabled", FALSE, NULL);

	manager = venture_plugin_manager_new(fixture->context);

	g_assert_true(venture_plugin_manager_load_configured(manager, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(venture_plugin_manager_get_count(manager), ==, 0);
}

static void
test_plugin_manager_list_is_json(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *entry;
	JsonArray *array;

	write_file(fixture->plugin_dir, "etsy.yaml",
	           "name: etsy\ndescription: A shop\nfields:\n  shop_name: string\n");

	manager = venture_plugin_manager_new(fixture->context);
	venture_plugin_manager_load_directory(manager, fixture->plugin_dir, NULL);

	node = venture_plugin_manager_list(manager);
	array = json_node_get_array(node);

	g_assert_cmpuint(json_array_get_length(array), ==, 1);

	entry = json_array_get_object_element(array, 0);
	g_assert_cmpstr(json_object_get_string_member(entry, "kind"), ==,
	                "declarative");
	g_assert_cmpstr(json_object_get_string_member(entry, "name"), ==,
	                "etsy.yaml");
}

/* ==========================================================================
 * A native plugin, loaded for real
 * ========================================================================== */

/*
 * Loads the example plugin built alongside the tests. Skipped when it is not
 * present, so the suite still runs for someone who built with
 * BUILD_PLUGINS=0.
 */
static void
test_plugin_manager_loads_native_plugin(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	const gchar *plugin_path;

	plugin_path = g_getenv("VENTURE_PLUGIN_PATH");

	if (NULL == plugin_path)
	{
		g_test_skip("VENTURE_PLUGIN_PATH is unset");
		return;
	}

	path = g_build_filename(plugin_path, "example.so", NULL);

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
	{
		g_test_skip("the example plugin was not built");
		return;
	}

	manager = venture_plugin_manager_new(fixture->context);

	g_assert_true(venture_plugin_manager_load_file(manager, path, &error));
	g_assert_no_error(error);

	/*
	 * The claim under test: a plugin's record type is indistinguishable
	 * from a built-in. It is in the registry, it has a table name, and it
	 * describes itself -- all without a line of routing or SQL.
	 */
	{
		VentureEntityRegistry *registry;
		g_autoptr(JsonNode) description = NULL;

		registry = venture_context_get_entity_registry(fixture->context);

		g_assert_cmpuint(venture_entity_registry_lookup(registry,
			"subscription"), !=, G_TYPE_INVALID);
		g_assert_cmpstr(venture_entity_registry_get_table_name(registry,
			"subscription"), ==, "subscriptions");

		description = venture_entity_registry_describe(registry,
		                                               "subscription");
		g_assert_nonnull(description);
	}

	/* And its report is in the same registry every consumer reads. */
	g_assert_nonnull(venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context),
		"subscriptions"));

	/* The plugin's table is created by the next migration, and its
	 * records then round-trip like any other. */
	{
		g_autoptr(VentureEntity) record = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) loaded = NULL;
		g_autoptr(VentureMoney) amount = NULL;

		g_assert_true(venture_database_migrate(fixture->database,
			venture_context_get_entity_registry(fixture->context), &error));
		g_assert_no_error(error);

		record = venture_entity_registry_create(
			venture_context_get_entity_registry(fixture->context),
			"subscription", &error);
		g_assert_no_error(error);

		g_object_set(record, "name", "Object storage", "cadence", "monthly",
		             "active", TRUE, NULL);
		g_assert_true(venture_entity_set_field_from_string(record, "amount",
			"12.00", NULL));

		g_assert_true(venture_database_save(fixture->database, record, NULL,
		                                    &error));
		g_assert_no_error(error);

		query = venture_query_new(G_OBJECT_TYPE(record));
		loaded = venture_database_find_one(fixture->database, query, NULL);

		g_assert_nonnull(loaded);
		g_object_get(loaded, "amount", &amount, NULL);
		g_assert_cmpint(venture_money_get_amount(amount), ==, 1200);
	}
}

/*
 * A crispy plugin, compiled on demand by the server. Small on purpose --
 * what is under test is the compile-and-load path and the flags it passes,
 * not the plugin's logic. The worked example lives in
 * plugins/scripts/royalties.c.
 *
 * It deliberately touches the reporting engine, because that is only
 * visible with VENTURE_SERVER_BUILD defined: a flags regression that hid it
 * would otherwise show up as a mystifying failure in someone's own plugin
 * rather than here.
 */
static const gchar venture_test_crispy_source[] =
	"#include <venture/venture.h>\n"
	"\n"
	"static VentureReportResult *\n"
	"probe_report(VentureContext *context, VentureDateRange *period,\n"
	"             JsonObject *options, GError **error)\n"
	"{\n"
	"\tVentureReportResult *result;\n"
	"\n"
	"\tresult = venture_report_result_new(\"Probe\", period);\n"
	"\tventure_report_result_add_metric(result,\n"
	"\t\tventure_metric_new_count(\"answer\", \"Answer\", 42));\n"
	"\n"
	"\treturn result;\n"
	"}\n"
	"\n"
	"const gchar *venture_plugin_info(void);\n"
	"\n"
	"const gchar *\n"
	"venture_plugin_info(void)\n"
	"{\n"
	"\treturn \"A compiled-on-demand probe\";\n"
	"}\n"
	"\n"
	"gboolean venture_plugin_register(VentureContext *context, GError **error);\n"
	"\n"
	"gboolean\n"
	"venture_plugin_register(VentureContext *context, GError **error)\n"
	"{\n"
	"\tventure_report_registry_add(\n"
	"\t\tventure_context_get_report_registry(context),\n"
	"\t\tVENTURE_REPORT(venture_func_report_new(\n"
	"\t\t\t\"probe\", \"Probe\", \"A compiled-on-demand probe\",\n"
	"\t\t\tprobe_report)));\n"
	"\n"
	"\treturn TRUE;\n"
	"}\n";

static void
test_plugin_manager_compiles_a_crispy_plugin(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *state_dir = NULL;

	if (!g_file_test(VENTURE_DEV_INCLUDE_DIR, G_FILE_TEST_IS_DIR))
	{
		g_test_skip("no development include tree to compile against");
		return;
	}

	/* The cache goes under the state directory, so it does not leak into
	 * the developer's real one. */
	state_dir = g_dir_make_tmp("venture-crispy-XXXXXX", NULL);
	g_object_set(fixture->config, "state-dir", state_dir, NULL);

	write_file(fixture->plugin_dir, "probe.c", venture_test_crispy_source);
	path = g_build_filename(fixture->plugin_dir, "probe.c", NULL);

	manager = venture_plugin_manager_new(fixture->context);

	g_assert_true(venture_plugin_manager_load_file(manager, path, &error));
	g_assert_no_error(error);

	/* Compiled, loaded, and registered into the same registry a native
	 * plugin and a built-in report share. */
	g_assert_nonnull(venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "probe"));

	/* And it actually runs. */
	{
		g_autoptr(VentureDateRange) period = NULL;
		g_autoptr(VentureReportResult) result = NULL;
		GPtrArray *metrics;

		period = venture_context_parse_period(fixture->context, "this_month",
		                                      NULL);
		result = venture_report_generate(
			venture_report_registry_lookup(
				venture_context_get_report_registry(fixture->context),
				"probe"),
			fixture->context, period, NULL, &error);

		g_assert_no_error(error);
		g_assert_nonnull(result);

		metrics = venture_report_result_get_metrics(result);
		g_assert_cmpuint(metrics->len, ==, 1);
		g_assert_cmpint((gint64)venture_metric_get_number(
			g_ptr_array_index(metrics, 0)), ==, 42);
	}

	/* The compiled object is cached by content hash, so an unchanged
	 * source costs a lookup rather than a compile on the next start. */
	{
		g_autofree gchar *cache_dir = NULL;

		cache_dir = g_build_filename(state_dir, "crispy-cache", NULL);
		g_assert_true(g_file_test(cache_dir, G_FILE_TEST_IS_DIR));
	}

	venture_test_remove_tree(state_dir);
}

static void
test_plugin_manager_refuses_crispy_when_disabled(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	g_object_set(fixture->config, "plugins-allow-crispy", FALSE, NULL);

	write_file(fixture->plugin_dir, "probe.c", venture_test_crispy_source);
	path = g_build_filename(fixture->plugin_dir, "probe.c", NULL);

	manager = venture_plugin_manager_new(fixture->context);

	/*
	 * Loading a .c plugin runs a compiler over a file in a plugin
	 * directory. An install that does not want that must be able to say
	 * so and have it mean something.
	 */
	g_assert_false(venture_plugin_manager_load_file(manager, path, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(g_strstr_len(error->message, -1, "allow_crispy"));
}

static void
test_plugin_manager_reports_a_broken_crispy_plugin(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *state_dir = NULL;

	if (!g_file_test(VENTURE_DEV_INCLUDE_DIR, G_FILE_TEST_IS_DIR))
	{
		g_test_skip("no development include tree to compile against");
		return;
	}

	state_dir = g_dir_make_tmp("venture-crispy-XXXXXX", NULL);
	g_object_set(fixture->config, "state-dir", state_dir, NULL);

	write_file(fixture->plugin_dir, "broken.c",
	           "#include <venture/venture.h>\n"
	           "this is not C at all\n");
	path = g_build_filename(fixture->plugin_dir, "broken.c", NULL);

	manager = venture_plugin_manager_new(fixture->context);

	/* A compile failure is an error the operator can act on, not a
	 * crash and not a silent skip. */
	g_assert_false(venture_plugin_manager_load_file(manager, path, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);

	venture_test_remove_tree(state_dir);
}


/* ==========================================================================
 * The web UI's navigation
 * ========================================================================== */

/*
 * Every sidebar link must lead somewhere.
 *
 * This exists because /settings was in the sidebar for a while with no route
 * behind it: the only way to find out was to click it and get a 404. The
 * links are either a fixed page, a record list for a registered type, or a
 * report -- so each can be checked against the registries without starting a
 * server.
 */
static void
test_web_navigation_links_all_resolve(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const fixed_pages[] = {
		"/", "/reports", "/settings", "/account", "/account/tokens",
		"/users", "/entities", "/tickets", "/login", "/logout", "/search",
		"/automations", "/plugins", "/kb", "/modules", "/factory",
		"/dashboards", "/overview", "/sprints", "/runs", "/webhooks",
		"/harness", NULL
	};
	const VentureWebNavLink *links;
	gsize i;

	links = venture_web_navigation();
	g_assert_nonnull(links);

	for (i = 0; NULL != links[i].path; i++)
	{
		const gchar *path;

		path = links[i].path;

		/* Every entry needs a label and an icon too, or it renders as a
		 * blank row that is impossible to click deliberately. */
		g_assert_nonnull(links[i].label);
		g_assert_nonnull(links[i].icon);

		if (g_strv_contains(fixed_pages, path))
			continue;

		if (g_str_has_prefix(path, "/e/"))
		{
			const gchar *type_name;

			type_name = path + strlen("/e/");

			/*
			 * A list page is served for any registered record type. If
			 * a type is renamed or removed and the sidebar is not
			 * updated, this is where it shows up.
			 */
			g_assert_cmpuint(venture_entity_registry_lookup(
				venture_context_get_entity_registry(fixture->context),
				type_name), !=, G_TYPE_INVALID);
			continue;
		}

		if (g_str_has_prefix(path, "/reports/"))
		{
			g_assert_nonnull(venture_report_registry_lookup(
				venture_context_get_report_registry(fixture->context),
				path + strlen("/reports/")));
			continue;
		}

		g_error("sidebar links %s, which is not a page, a record type or "
		        "a report", path);
	}

	/* A sidebar that lost its entries would pass every check above. */
	g_assert_cmpuint(i, >, 10);
}

/* ==========================================================================
 * Automation
 * ========================================================================== */

static void
test_automation_disabled_is_not_a_failure(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;

	g_object_set(fixture->config, "automation-enabled", FALSE, NULL);

	automation = venture_automation_new(fixture->context, &error);

	/* Like AI, disabled automation is a normal state reported clearly
	 * rather than a failure that stops startup. */
	g_assert_null(automation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_automation_starts_without_rules(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state_dir = NULL;

	state_dir = g_dir_make_tmp("venture-state-XXXXXX", NULL);
	g_object_set(fixture->config, "state-dir", state_dir, NULL);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(automation);

	/* An install with no automations yet simply has none. */
	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	venture_test_remove_tree(state_dir);
}

static void
test_automation_parses_dsl(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);

	/* The venture module is registered in-process, so a rule can name it
	 * without any module path being configured. */
	g_assert_true(venture_automation_load_dsl(automation,
		"pod nightly = venture->new();\n"
		"nightly->on_created => venture->count(\"sale\");\n", &error));
	g_assert_no_error(error);

	g_assert_cmpuint(venture_automation_get_pod_count(automation), ==, 1);
}

static void
test_automation_rejects_bad_dsl(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;

	automation = venture_automation_new(fixture->context, NULL);

	g_assert_false(venture_automation_load_dsl(automation,
		"this is not the podomation dsl at all {{{\n", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION);
}

static void
test_automation_describe(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;

	automation = venture_automation_new(fixture->context, NULL);
	g_assert_nonnull(automation);

	node = venture_automation_describe(automation);
	object = json_node_get_object(node);

	g_assert_false(json_object_get_boolean_member(object, "running"));
	g_assert_true(json_object_has_member(object, "pods"));
}

/*
 * Reads the "count" field out of what a handler returned. Every handler
 * answers with the same shape so a rule can treat them uniformly.
 */
static gint64
handler_result_count(GVariant *result)
{
	g_autoptr(GVariant) count = NULL;

	g_assert_nonnull(result);

	count = g_variant_lookup_value(result, "count", G_VARIANT_TYPE_INT64);
	g_assert_nonnull(count);

	return g_variant_get_int64(count);
}

static void
test_automation_handler_counts_records(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *arguments[] = { "venture", NULL };

	automation = venture_automation_new(fixture->context, NULL);

	g_assert_true(venture_automation_invoke(automation, "count", arguments,
	                                        &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handler_result_count(result), ==, 0);

	/* Now there is one, and the same handler says so. */
	{
		g_autoptr(VentureVenture) venture = NULL;
		g_autoptr(GVariant) second = NULL;

		venture = venture_venture_new();
		g_object_set(venture, "name", "Books", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(venture),
			venture_context_get_default_organization_id(fixture->context));

		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(venture), NULL, &error));
		g_assert_no_error(error);

		g_assert_true(venture_automation_invoke(automation, "count",
			arguments, &second, &error));
		g_assert_cmpint(handler_result_count(second), ==, 1);
	}
}

static void
test_automation_handler_creates_a_record(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) created = NULL;
	g_autofree gchar *name = NULL;
	const gchar *arguments[] = {
		"idea", "title=A pop-up shop", "status=researching", NULL
	};

	automation = venture_automation_new(fixture->context, NULL);

	g_assert_true(venture_automation_invoke(automation, "create", arguments,
	                                        &result, &error));
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_IDEA);
	created = venture_database_find_one(fixture->database, query, &error);

	g_assert_nonnull(created);
	g_object_get(created, "title", &name, NULL);
	g_assert_cmpstr(name, ==, "A pop-up shop");
}

static void
test_automation_handler_write_is_attributed(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) audit = NULL;
	VentureActorKind kind;
	const gchar *arguments[] = {
		"idea", "{\"title\": \"A pop-up shop\"}", NULL
	};

	automation = venture_automation_new(fixture->context, NULL);
	g_assert_true(venture_automation_invoke(automation, "create", arguments,
	                                        &result, NULL));

	/*
	 * An automation's writes are audited as the automation, not as
	 * whoever happened to be logged in. Without that, reviewing the audit
	 * log later cannot answer "did I do this or did a rule?".
	 */
	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	audit = venture_database_find_one(fixture->database, query, NULL);

	g_assert_nonnull(audit);
	g_object_get(audit, "actor-kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_ACTOR_KIND_AUTOMATION);
}

static void
test_automation_handler_runs_a_report(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *arguments[] = { "pnl", "this_year", NULL };

	automation = venture_automation_new(fixture->context, NULL);

	g_assert_true(venture_automation_invoke(automation, "report", arguments,
	                                        &result, &error));
	g_assert_no_error(error);

	/* A report handler returns the rendered text, because what an
	 * automation does with a report is put it in a message. */
	{
		g_autoptr(GVariant) detail = NULL;

		detail = g_variant_lookup_value(result, "detail",
		                                G_VARIANT_TYPE_STRING);
		g_assert_nonnull(detail);
		g_assert_cmpuint(strlen(g_variant_get_string(detail, NULL)), >, 0);
	}
}

static void
test_automation_handler_reports_unknown_type(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *arguments[] = { "nonexistent", NULL };

	automation = venture_automation_new(fixture->context, NULL);

	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*venture->query*");

	g_assert_false(venture_automation_invoke(automation, "count", arguments,
	                                         NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION);

	g_test_assert_expected_messages();
}

static void
test_automation_rejects_unknown_handler(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;

	automation = venture_automation_new(fixture->context, NULL);

	g_assert_false(venture_automation_invoke(automation, "do_the_thing", NULL,
	                                         NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION);

	/* The error names the handlers that do exist, since the reader is
	 * writing a rule and has just guessed wrong. */
	g_assert_nonnull(g_strstr_len(error->message, -1, "low_stock"));
}

/*
 * Creates a product, an inventory item for it and one stock movement, which
 * is the smallest arrangement the inventory report understands.
 */
static void
stock_an_item(
	Fixture		*fixture,
	const gchar	*sku,
	gint64		 reorder_point,
	gint64		 on_hand
){
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureProduct) product = NULL;
	g_autoptr(VentureInventoryItem) item = NULL;
	g_autoptr(VentureInventoryTxn) movement = NULL;
	g_autoptr(GDateTime) now = NULL;
	gint64 organization;

	organization = venture_context_get_default_organization_id(fixture->context);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Books", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), organization);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(venture), NULL, NULL));

	product = venture_product_new();
	g_object_set(product, "name", sku, "sku", sku,
	             "venture-id", venture_entity_get_id(VENTURE_ENTITY(venture)),
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(product), organization);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(product), NULL, NULL));

	item = venture_inventory_item_new();
	g_object_set(item, "sku", sku, "location", "Home",
	             "product-id", venture_entity_get_id(VENTURE_ENTITY(product)),
	             "reorder-point", reorder_point, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(item), organization);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(item), NULL, NULL));

	now = g_date_time_new_now_utc();

	movement = venture_inventory_txn_new();
	g_object_set(movement,
	             "inventory-item-id", venture_entity_get_id(VENTURE_ENTITY(item)),
	             "kind", VENTURE_INVENTORY_TXN_KIND_PURCHASE,
	             "quantity", on_hand,
	             "occurred-at", now,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(movement), organization);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(movement), NULL, NULL));
}

static void
test_automation_low_stock_finds_short_items(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GVariant) detail = NULL;

	/* Plenty of one thing, almost none of another. Stock on hand is the
	 * sum of the signed movements rather than a stored number, so this
	 * builds the movements the real system would have. */
	stock_an_item(fixture, "POSTCARD", 100, 500);
	stock_an_item(fixture, "HARDBACK", 25, 3);

	automation = venture_automation_new(fixture->context, NULL);

	g_assert_true(venture_automation_invoke(automation, "low_stock", NULL,
	                                        &result, NULL));

	/* Only the item under its reorder point counts: an alert that fires
	 * for everything is one nobody reads. */
	g_assert_cmpint(handler_result_count(result), ==, 1);

	/* And it names which one, so the alert is actionable without going
	 * and looking the item up. */
	detail = g_variant_lookup_value(result, "detail", G_VARIANT_TYPE_STRING);
	g_assert_nonnull(detail);
	g_assert_nonnull(g_strstr_len(g_variant_get_string(detail, NULL), -1,
	                              "HARDBACK"));
}

static void
test_automation_does_not_cascade(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *pods_file = NULL;
	gint64 ideas;

	state_dir = g_dir_make_tmp("venture-state-XXXXXX", NULL);
	pods_file = g_build_filename(state_dir, "automations.pod", NULL);

	/*
	 * A rule that writes on every write. Without the reentrancy guard
	 * this recurses until the disk fills: the created idea raises
	 * on_created, which creates another idea, and so on.
	 */
	g_assert_true(g_file_set_contents(pods_file,
		"pod loop = venture->new();\n"
		"loop->on_created => venture->create(\"idea\", \"title=spawned\");\n",
		-1, NULL));

	g_object_set(fixture->config, "state-dir", state_dir, NULL);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	/* One write from outside. */
	{
		g_autoptr(VentureVenture) venture = NULL;

		venture = venture_venture_new();
		g_object_set(venture, "name", "Books", NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(venture),
			venture_context_get_default_organization_id(fixture->context));

		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(venture), NULL, &error));
		g_assert_no_error(error);
	}

	query = venture_query_new(VENTURE_TYPE_IDEA);
	ideas = venture_database_count(fixture->database, query, &error);
	g_assert_no_error(error);

	/* Exactly one idea: the rule ran, and its own write did not run it
	 * again. */
	g_assert_cmpint(ideas, ==, 1);

	venture_automation_stop(automation);
	venture_test_remove_tree(state_dir);
}

static void
test_automation_ignores_audit_entries(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureAuditEntry) entry = NULL;

	automation = venture_automation_new(fixture->context, NULL);
	g_assert_nonnull(automation);

	entry = venture_audit_entry_new();

	/*
	 * An audit entry is itself a record, so emitting an event for one
	 * would make every automation that writes trigger itself. This must
	 * be a silent no-op rather than a loop.
	 */
	venture_automation_emit_entity_event(automation, "on_created",
	                                     VENTURE_ENTITY(entry));
}

/* --- Plugin configuration ------------------------------------------------- */

static void
plugin_config_note_change(
	VenturePluginManager	*manager,
	const gchar		*plugin_name,
	gpointer		 user_data
){
	gchar **seen;

	seen = user_data;
	g_free(*seen);
	*seen = g_strdup(plugin_name);
}

/*
 * The whole point of configuration-as-record: store YAML, read it back
 * parsed, and be told the moment it changes. Without the signal a plugin
 * reads its settings once and the /plugins page is a lie until restart.
 */
static void
test_plugin_config_round_trip_and_signal(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(JsonNode) config = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GError) error = NULL;
	gdouble rate;

	manager = venture_plugin_manager_new(fixture->context);
	g_signal_connect(manager, "config-changed",
	                 G_CALLBACK(plugin_config_note_change), &seen);

	g_assert_true(venture_plugin_manager_set_config(manager,
		"royalties.c", "rate_percent: 65.5\nlabel: paperback\n", NULL,
		&error));
	g_assert_no_error(error);

	/* The signal named the right plugin. */
	g_assert_cmpstr(seen, ==, "royalties.c");

	/* The text survives verbatim for the editor... */
	text = venture_plugin_manager_get_config_text(manager, "royalties.c");
	g_assert_nonnull(strstr(text, "rate_percent: 65.5"));

	/* ...and the parsed view gives typed values. */
	config = venture_plugin_manager_get_config(manager, "royalties.c");
	g_assert_nonnull(config);

	rate = venture_plugin_config_get_double(config, "rate_percent", 0.0);
	g_assert_cmpfloat(rate, ==, 65.5);

	name = venture_plugin_config_get_string(config, "label", "none");
	g_assert_cmpstr(name, ==, "paperback");

	/* Absent keys fall back rather than fail. */
	g_assert_cmpfloat(venture_plugin_config_get_double(config, "missing",
	                                                   7.0), ==, 7.0);

	/* Saving again updates the same record rather than stacking a
	 * second, and an unconfigured plugin reads as NULL, not empty. */
	g_assert_true(venture_plugin_manager_set_config(manager,
		"royalties.c", "rate_percent: 50\n", NULL, NULL));
	g_clear_pointer(&text, g_free);
	text = venture_plugin_manager_get_config_text(manager, "royalties.c");
	g_assert_null(strstr(text, "65.5"));
	g_assert_null(venture_plugin_manager_get_config(manager, "other.so"));
}

static void
test_plugin_config_refuses_bad_yaml(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;

	manager = venture_plugin_manager_new(fixture->context);

	/* Refused before storage: a running plugin must never be signalled
	 * into re-reading settings it cannot parse. */
	g_assert_false(venture_plugin_manager_set_config(manager, "x.c",
		"key: [unclosed", NULL, &error));
	g_assert_nonnull(error);
	g_assert_null(venture_plugin_manager_get_config_text(manager, "x.c"));
}

/* --- The AI's page fetcher ------------------------------------------------ */

/*
 * The SSRF boundary. The model chooses the address, and the pages it reads
 * can suggest the next one, so "fetch this listing" must never become a
 * request to the cloud metadata endpoint, a container-network database
 * port, a router admin page, or the server's own disk.
 *
 * Tested directly rather than through a conversation: a model may refuse
 * such a URL on its own, which looks like the check working and proves
 * nothing about the check.
 */
static void
test_ai_fetch_refuses_private_addresses(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const refused[] = {
		/* Loopback, by literal and by name. */
		"http://127.0.0.1/settings",
		"http://localhost:8747/api/v1/settings",
		"http://[::1]/",
		/* The cloud metadata endpoint: link-local, and the single
		 * most valuable target for an SSRF in a hosted deployment. */
		"http://169.254.169.254/latest/meta-data/",
		/* Private ranges -- the database and everything else on the
		 * container or office network. */
		"http://10.0.0.5:5432/",
		"http://192.168.1.1/admin",
		"http://172.16.4.4/",
		/* Carrier-grade NAT, which no GLib predicate covers. */
		"http://100.64.0.1/",
		/* Not the web at all. */
		"file:///etc/passwd",
		"gopher://example.com/",
		"ftp://example.com/secrets",
		NULL
	};
	gsize i;

	for (i = 0; NULL != refused[i]; i++)
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(venture_ai_url_is_fetchable(refused[i], &error));
		g_assert_nonnull(error);
	}

	/* Malformed input is refused rather than crashing or guessing. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(venture_ai_url_is_fetchable("", &error));
		g_assert_false(venture_ai_url_is_fetchable("http://", NULL));
		g_assert_false(venture_ai_url_is_fetchable("not a url", NULL));
	}
}

/* --- Automation validation and reload ------------------------------------- */

static void
test_automation_validate_dsl(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *message = NULL;

	/* Valid source: no message. */
	g_assert_true(venture_automation_validate_dsl(
		"pod nightly = venture->new();\n"
		"nightly->on_created => venture->count(\"sale\");\n", &message));
	g_assert_null(message);

	/* Broken source: a message worth showing in an editor. */
	g_assert_false(venture_automation_validate_dsl(
		"pod broken = venture->new(;\n", &message));
	g_assert_nonnull(message);
	g_assert_cmpuint(strlen(message), >, 0);
}

/*
 * A reload must end with exactly the rules the file now holds -- not the
 * old rules, not both generations at once. Both generations at once is the
 * bug that doubles every automation side effect.
 */
static void
test_automation_reload_rebuilds(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autofree gchar *pods_file = NULL;
	g_autoptr(GError) error = NULL;

	/* The automation reads its rules from the configured pods file. */
	pods_file = g_build_filename(fixture->plugin_dir, "rules.pod", NULL);
	g_object_set(fixture->config, "automation-pods-file", pods_file, NULL);

	g_assert_true(g_file_set_contents(pods_file,
		"pod one = venture->new();\n"
		"one->on_created => venture->count(\"sale\");\n", -1, NULL));

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(venture_automation_get_pod_count(automation), ==, 1);

	/* The file grows a second pod; the reload must show exactly two. */
	g_assert_true(g_file_set_contents(pods_file,
		"pod one = venture->new();\n"
		"one->on_created => venture->count(\"sale\");\n"
		"pod two = venture->new();\n"
		"two->on_deleted => venture->count(\"expense\");\n", -1, NULL));

	g_assert_true(venture_automation_reload(automation, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(venture_automation_get_pod_count(automation), ==, 2);

	venture_automation_stop(automation);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/venture-type/parses-yaml", test_venture_type_parses_yaml);
	ADD("/venture-type/shorthand-field", test_venture_type_shorthand_field);
	ADD("/venture-type/requires-a-name", test_venture_type_requires_a_name);
	ADD("/venture-type/rejects-unknown-field-type",
	    test_venture_type_rejects_unknown_field_type);
	ADD("/venture-type/enum-needs-choices", test_venture_type_enum_needs_choices);
	ADD("/venture-type/validates-a-venture", test_venture_type_validates_a_venture);
	ADD("/venture-type/registry-loads-directory",
	    test_venture_type_registry_loads_directory);
	ADD("/venture-type/registry-skips-broken-definition",
	    test_venture_type_registry_skips_broken_definition);
	ADD("/venture-type/registry-missing-directory-is-not-an-error",
	    test_venture_type_registry_missing_directory_is_not_an_error);
	ADD("/venture-type/to-json-describes-fields",
	    test_venture_type_to_json_describes_fields);

	ADD("/plugin/manager-loads-venture-types",
	    test_plugin_manager_loads_venture_types);
	ADD("/plugin/manager-rejects-unknown-extension",
	    test_plugin_manager_rejects_unknown_extension);
	ADD("/plugin/manager-skips-broken-plugin",
	    test_plugin_manager_skips_broken_plugin);
	ADD("/plugin/manager-loads-each-file-once",
	    test_plugin_manager_loads_each_file_once);
	ADD("/plugin/manager-missing-directory-is-not-an-error",
	    test_plugin_manager_missing_directory_is_not_an_error);
	ADD("/plugin/manager-required-plugin-is-fatal",
	    test_plugin_manager_required_plugin_is_fatal);
	ADD("/plugin/manager-disabled-loads-nothing",
	    test_plugin_manager_disabled_loads_nothing);
	ADD("/plugin/manager-list-is-json", test_plugin_manager_list_is_json);
	ADD("/plugin/manager-loads-native-plugin",
	    test_plugin_manager_loads_native_plugin);

	ADD("/plugin/manager-compiles-a-crispy-plugin",
	    test_plugin_manager_compiles_a_crispy_plugin);
	ADD("/plugin/manager-refuses-crispy-when-disabled",
	    test_plugin_manager_refuses_crispy_when_disabled);
	ADD("/plugin/manager-reports-a-broken-crispy-plugin",
	    test_plugin_manager_reports_a_broken_crispy_plugin);

	ADD("/web/navigation-links-all-resolve",
	    test_web_navigation_links_all_resolve);

	ADD("/automation/disabled-is-not-a-failure",
	    test_automation_disabled_is_not_a_failure);
	ADD("/automation/starts-without-rules", test_automation_starts_without_rules);
	ADD("/automation/parses-dsl", test_automation_parses_dsl);
	ADD("/automation/rejects-bad-dsl", test_automation_rejects_bad_dsl);
	ADD("/automation/describe", test_automation_describe);
	ADD("/automation/handler-counts-records",
	    test_automation_handler_counts_records);
	ADD("/automation/handler-creates-a-record",
	    test_automation_handler_creates_a_record);
	ADD("/automation/handler-write-is-attributed",
	    test_automation_handler_write_is_attributed);
	ADD("/automation/handler-runs-a-report",
	    test_automation_handler_runs_a_report);
	ADD("/automation/handler-reports-unknown-type",
	    test_automation_handler_reports_unknown_type);
	ADD("/automation/rejects-unknown-handler",
	    test_automation_rejects_unknown_handler);
	ADD("/automation/low-stock-finds-short-items",
	    test_automation_low_stock_finds_short_items);
	ADD("/automation/does-not-cascade", test_automation_does_not_cascade);
	ADD("/automation/ignores-audit-entries",
	    test_automation_ignores_audit_entries);

	ADD("/plugin/config-round-trip-and-signal",
	    test_plugin_config_round_trip_and_signal);
	ADD("/plugin/config-refuses-bad-yaml",
	    test_plugin_config_refuses_bad_yaml);

	ADD("/ai/fetch-refuses-private-addresses",
	    test_ai_fetch_refuses_private_addresses);

	ADD("/automation/validate-dsl", test_automation_validate_dsl);
	ADD("/automation/reload-rebuilds", test_automation_reload_rebuilds);

#undef ADD

	return g_test_run();
}
