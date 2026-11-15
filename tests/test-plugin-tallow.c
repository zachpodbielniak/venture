/*
 * test-plugin-tallow.c - .tlw plugins on the sandboxed suet VM
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The claims, each with a way to be quietly false:
 *
 *   - a .tlw plugin loads like any other, and what it registers (a fee
 *     model, an automation step) behaves like a built-in's;
 *   - the sandbox holds: no file, no limitless loop, no registration after
 *     load, and a plugin switched off by configuration does not load;
 *   - a grant opens exactly what it names, for exactly the plugin it names;
 *   - the kernels a plugin calls are the compiled ones, never interpreted:
 *     the kernel sources are not on the VM's path, and a contract a plugin
 *     violates in a kernel is an error for that plugin, not an abort of
 *     the server;
 *   - one plugin is safe to call from several threads at once.
 */

#include <venture.h>

#include <glib/gstdio.h>
#include <string.h>

#include "venture-test-util.h"
#include "plugin/venture-tallow-runtime-private.h"
#include "arbitrage/venture-arbitrage-engine.h"

typedef struct
{
	VentureDatabase		*database;
	VentureConfig		*config;
	VentureContext		*context;
	VenturePluginManager	*manager;
	gchar			*plugin_dir;
	gchar			*state_dir;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->plugin_dir = g_dir_make_tmp("venture-tallow-XXXXXX", NULL);
	fixture->state_dir = g_dir_make_tmp("venture-tstate-XXXXXX", NULL);
	g_object_set(fixture->config, "state-dir", fixture->state_dir, NULL);

	fixture->manager = venture_plugin_manager_new(fixture->context);
	venture_context_set_plugin_manager(fixture->context, fixture->manager);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	venture_test_remove_tree(fixture->plugin_dir);
	venture_test_remove_tree(fixture->state_dir);
	g_clear_pointer(&fixture->plugin_dir, g_free);
	g_clear_pointer(&fixture->state_dir, g_free);
	venture_context_set_plugin_manager(fixture->context, NULL);
	g_clear_object(&fixture->manager);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

/* writes a plugin into the scratch directory and returns its path */
static gchar *
write_plugin(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*source
){
	g_autoptr(GError) error = NULL;
	gchar *path;

	path = g_build_filename(fixture->plugin_dir, name, NULL);
	g_assert_true(g_file_set_contents(path, source, -1, &error));
	g_assert_no_error(error);
	return path;
}

/* loads a plugin that must load, reporting why when it does not */
static void
load_ok(
	Fixture		*fixture,
	const gchar	*path
){
	g_autoptr(GError) error = NULL;
	gboolean loaded;

	loaded = venture_plugin_manager_load_file(fixture->manager, path, &error);
	g_assert_no_error(error);
	g_assert_true(loaded);
}

/* the reference plugin as the build copied it */
static gchar *
reference_plugin_path(void)
{
	g_autofree gchar *out = NULL;
	const gchar *dir;

	/* VENTURE_PLUGIN_PATH is <outdir>/plugins; the copy is in its sibling */
	dir = g_getenv("VENTURE_PLUGIN_PATH");
	g_assert_nonnull(dir);
	out = g_path_get_dirname(dir);
	return g_build_filename(out, "plugins-optional", "tiered-fees.tlw", NULL);
}

/* the fee a registered model charges on an amount */
static gint64
fee_of(
	Fixture		*fixture,
	const gchar	*model,
	const gchar	*params_json,
	gint64		 amount,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(VentureMoney) gross = NULL;
	VentureFeeQuote quote;
	gint64 fee;

	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, params_json, -1, NULL));
	gross = venture_money_new(amount, "USD", 2);
	memset(&quote, 0, sizeof(quote));
	if (!venture_fee_model_registry_compute(venture_context_get_fee_models(fixture->context),
	                                        model, json_node_get_object(json_parser_get_root(parser)),
	                                        VENTURE_FEE_SIDE_SELL, gross, 1, NULL, NULL,
	                                        &quote, error))
		return -1;
	fee = venture_money_get_amount(quote.fee);
	g_assert_cmpstr(venture_money_get_currency(quote.fee), ==, "USD");
	venture_fee_quote_clear(&quote);
	return fee;
}

/* --- The reference plugin --------------------------------------------------- */

/*
 * plugins/tallow/tiered-fees.tlw loads, says what it is, and its fee model
 * prices trades through the compiled kernels: half to even per tier.
 */
static void
test_tallow_reference_fee_model(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) listing = NULL;
	g_autofree gchar *listed = NULL;

	(void)user_data;

	path = reference_plugin_path();
	load_ok(fixture, path);

	listing = venture_plugin_manager_list(fixture->manager);
	listed = json_to_string(listing, FALSE);
	g_assert_nonnull(strstr(listed, "\"tallow\""));
	g_assert_nonnull(strstr(listed, "Tiered percentage fees"));

	g_assert_true(venture_fee_model_registry_has(venture_context_get_fee_models(fixture->context),
	                                             "tiered_ppm"));
	/* 2.5% of 100.00 */
	g_assert_cmpint(fee_of(fixture, "tiered_ppm", "{\"rate_ppm\":25000}", 10000, &error), ==, 250);
	g_assert_no_error(error);
	/* 2.5% of the first 40.00, 1% of the other 60.00 */
	g_assert_cmpint(fee_of(fixture, "tiered_ppm",
		"{\"rate_ppm\":25000,\"rate_above_ppm\":10000,\"above\":4000}", 10000, &error), ==, 160);
	g_assert_no_error(error);
	/* half to even, like the ledger: 0.5% of 1.01 is 0.505 -> 0.50 */
	g_assert_cmpint(fee_of(fixture, "tiered_ppm", "{\"rate_ppm\":5000}", 101, &error), ==, 1);
	g_assert_no_error(error);
	/* no rate prices nothing, and says so */
	g_assert_cmpint(fee_of(fixture, "tiered_ppm", "{}", 10000, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* the plugin's validate function judges a venue's parameters */
static void
test_tallow_reference_validate(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	VentureFeeModelRegistry *models;

	(void)user_data;

	path = reference_plugin_path();
	load_ok(fixture, path);
	models = venture_context_get_fee_models(fixture->context);

	g_assert_true(venture_fee_model_registry_validate(models, "tiered_ppm", "rate_ppm: 25000\n", &error));
	g_assert_no_error(error);
	g_assert_false(venture_fee_model_registry_validate(models, "tiered_ppm", "above: 10\n", &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "needs rate_ppm"));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "tiered_ppm",
		"rate_ppm: 1\nrate_above_ppm: 2000000\n", &error));
	g_assert_nonnull(error);
}

/* the automation step takes a rule's positional arguments */
static void
test_tallow_reference_automation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GVariant) args = NULL;
	gint64 ppm;

	(void)user_data;

	path = reference_plugin_path();
	load_ok(fixture, path);

	args = g_variant_ref_sink(g_variant_new("(ss)", "1000", "800"));
	g_assert_true(venture_automation_handler_registry_call(
		venture_context_get_automation_handlers(fixture->context), fixture->context,
		"margin_ppm", args, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(g_variant_lookup(result, "margin_ppm", "x", &ppm));
	g_assert_cmpint(ppm, ==, 200000);

	/* the plugin's own failure is the step's failure */
	g_clear_pointer(&args, g_variant_unref);
	g_clear_pointer(&result, g_variant_unref);
	args = g_variant_ref_sink(g_variant_new("(ss)", "0", "800"));
	g_assert_false(venture_automation_handler_registry_call(
		venture_context_get_automation_handlers(fixture->context), fixture->context,
		"margin_ppm", args, &result, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "positive price"));
}

/* --- The sandbox -------------------------------------------------------------- */

/* reading a file without a grant is refused, so the load fails */
static void
test_tallow_sandbox_denies_files(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	path = write_plugin(fixture, "snoop.tlw",
		"fn plugin_register() ! GError {\n"
		"\tlet text = read_file(\"/etc/hostname\")?;\n"
		"\tprint(text);\n"
		"}\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, path, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "denied"));
}

/* a grant names a plugin and a capability; it opens that, for that plugin */
static void
test_tallow_grant_opens_one_directory(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *data_dir = NULL;
	g_autofree gchar *data = NULL;
	g_autofree gchar *grant = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *reader = NULL;
	g_autofree gchar *other = NULL;
	const gchar *grants[2];

	(void)user_data;

	data_dir = g_build_filename(fixture->state_dir, "rates", NULL);
	g_assert_cmpint(g_mkdir_with_parents(data_dir, 0700), ==, 0);
	data = g_build_filename(data_dir, "rate.txt", NULL);
	g_assert_true(g_file_set_contents(data, "25000", -1, NULL));

	grant = g_strdup_printf("reader=fs.read:%s", data_dir);
	grants[0] = grant;
	grants[1] = NULL;
	g_object_set(fixture->config, "plugins-tallow-capabilities", grants, NULL);

	source = g_strdup_printf(
		"fn plugin_register() ! GError {\n"
		"\tlet text = read_file(\"%s\")?;\n"
		"\tif text != \"25000\" {\n"
		"\t\tfail(\"read the wrong thing\");\n"
		"\t}\n"
		"}\n", data);
	reader = write_plugin(fixture, "reader.tlw", source);
	load_ok(fixture, reader);

	/* the same code under another name has no grant */
	other = write_plugin(fixture, "other.tlw", source);
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, other, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "denied"));
}

/* an endless loop meets the instruction budget, not the server's patience */
static void
test_tallow_step_limit(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	g_object_set(fixture->config, "plugins-tallow-max-steps", (gint64)100000, NULL);
	path = write_plugin(fixture, "spin.tlw",
		"fn plugin_register() {\n"
		"\tvar n = 0;\n"
		"\twhile true {\n"
		"\t\tn += 1;\n"
		"\t}\n"
		"}\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, path, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "limit"));
}

/* plugins.allow_tallow off: a .tlw file does not load */
static void
test_tallow_switched_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	g_object_set(fixture->config, "plugins-allow-tallow", FALSE, NULL);
	path = write_plugin(fixture, "quiet.tlw", "fn plugin_register() {\n}\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, path, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "allow_tallow"));
}

/* registering is for plugin_register(); a handler that tries later fails */
static void
test_tallow_register_only_at_load(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GVariant) args = NULL;
	g_autoptr(GVariant) result = NULL;

	(void)user_data;

	path = write_plugin(fixture, "late.tlw",
		"import venture;\n"
		"fn plugin_register() ! GError {\n"
		"\tventure.register_automation_handler(\"late_step\", \"registers late\", \"late\")?;\n"
		"}\n"
		"fn late(params: str) -> own str ! GError {\n"
		"\tventure.register_automation_handler(\"later\", \"no\", \"late\")?;\n"
		"\treturn \"{}\";\n"
		"}\n");
	load_ok(fixture, path);

	args = g_variant_ref_sink(g_variant_new("()"));
	g_assert_false(venture_automation_handler_registry_call(
		venture_context_get_automation_handlers(fixture->context), fixture->context,
		"late_step", args, &result, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "only available inside plugin_register"));
}

/* a plugin without plugin_register() is not a plugin */
static void
test_tallow_needs_register(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	path = write_plugin(fixture, "bare.tlw", "pure fn twice(n: i64) -> i64 = n * 2;\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, path, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "plugin_register"));
}

/* --- Compiled kernels, not interpreted ones ----------------------------------- */

/*
 * The kernel sources are not on the VM's search path: a plugin importing a
 * kernel module by its own name finds nothing, so it cannot end up running
 * an interpreted copy. It reaches them only through venture_kernels.
 */
static void
test_tallow_kernels_not_interpretable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	path = write_plugin(fixture, "copycat.tlw",
		"import vk_money;\n"
		"fn plugin_register() {\n"
		"}\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, path, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "vk_money"));
}

/* every venture_kernels function a plugin sees is bound to compiled code */
static void
test_tallow_kernels_are_host_functions(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	const VentureTallowHostEntry *entry;
	const gchar *source;
	guint count;

	(void)fixture;
	(void)user_data;

	source = venture_tallow_kernel_module_source();
	count = 0;
	for (entry = venture_tallow_kernel_functions(); NULL != entry->symbol; entry++)
	{
		g_autofree gchar *quoted = NULL;

		/* every declared extern names one of these symbols */
		quoted = g_strdup_printf("= \"%s\";", entry->symbol);
		g_assert_nonnull(strstr(source, quoted));
		g_assert_true(g_str_has_prefix(entry->symbol, "venture_tlw_vk_"));
		count++;
	}
	g_assert_cmpuint(count, >, 10);
	/* and the module declares nothing but externs: no kernel body */
	g_assert_null(strstr(source, "\npure fn "));
	g_assert_null(strstr(source, "\nfn "));
	g_assert_null(strstr(source, "\trequires "));
}

/*
 * A plugin passing a kernel something its `requires` forbids gets an error
 * for that call; the server -- this test process -- carries on.
 */
static void
test_tallow_kernel_contract_is_an_error(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GVariant) args = NULL;
	g_autoptr(GVariant) result = NULL;

	(void)user_data;

	path = write_plugin(fixture, "careless.tlw",
		"import venture;\n"
		"import venture_kernels as K;\n"
		"fn plugin_register() ! GError {\n"
		"\tventure.register_automation_handler(\"halve_by\", \"divides\", \"halve_by\")?;\n"
		"}\n"
		"fn halve_by(params: str) -> own str ! GError {\n"
		"\tlet d = (venture.json_arg(params, 0) ?? \"0\").parse_i64() ?? 0;\n"
		"\treturn \"{\\\"q\\\":${K.round_half_even(7, d)}}\";\n"
		"}\n");
	load_ok(fixture, path);

	args = g_variant_ref_sink(g_variant_new("(s)", "2"));
	g_assert_true(venture_automation_handler_registry_call(
		venture_context_get_automation_handlers(fixture->context), fixture->context,
		"halve_by", args, &result, &error));
	g_assert_no_error(error);
	g_clear_pointer(&result, g_variant_unref);
	g_clear_pointer(&args, g_variant_unref);

	/* half_even requires d > 1 */
	args = g_variant_ref_sink(g_variant_new("(s)", "0"));
	g_assert_false(venture_automation_handler_registry_call(
		venture_context_get_automation_handlers(fixture->context), fixture->context,
		"halve_by", args, &result, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "contract violated"));
	g_clear_error(&error);
	g_clear_pointer(&args, g_variant_unref);

	/* the plugin is still usable afterwards */
	args = g_variant_ref_sink(g_variant_new("(s)", "3"));
	g_assert_true(venture_automation_handler_registry_call(
		venture_context_get_automation_handlers(fixture->context), fixture->context,
		"halve_by", args, &result, &error));
	g_assert_no_error(error);
}

/* --- Threads ----------------------------------------------------------------------- */

typedef struct
{
	Fixture	*fixture;
	gint	 failures;
} Hammer;

static gpointer
hammer_thread(gpointer data)
{
	Hammer *hammer;
	guint i;

	hammer = data;
	for (i = 0; i < 50; i++)
	{
		g_autoptr(GError) error = NULL;

		if (250 != fee_of(hammer->fixture, "tiered_ppm", "{\"rate_ppm\":25000}", 10000, &error))
			g_atomic_int_inc(&hammer->failures);
	}
	return NULL;
}

/* a VM runs one call at a time; the plugin's lock makes that true */
static void
test_tallow_threads(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	GThread *threads[4];
	Hammer hammer;
	guint i;

	(void)user_data;

	path = reference_plugin_path();
	load_ok(fixture, path);

	hammer.fixture = fixture;
	hammer.failures = 0;
	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		threads[i] = g_thread_new("hammer", hammer_thread, &hammer);
	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		g_thread_join(threads[i]);
	g_assert_cmpint(hammer.failures, ==, 0);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/plugin-tallow/reference/fee-model", test_tallow_reference_fee_model);
	ADD("/plugin-tallow/reference/validate", test_tallow_reference_validate);
	ADD("/plugin-tallow/reference/automation", test_tallow_reference_automation);
	ADD("/plugin-tallow/sandbox/denies-files", test_tallow_sandbox_denies_files);
	ADD("/plugin-tallow/sandbox/grant", test_tallow_grant_opens_one_directory);
	ADD("/plugin-tallow/sandbox/step-limit", test_tallow_step_limit);
	ADD("/plugin-tallow/sandbox/switched-off", test_tallow_switched_off);
	ADD("/plugin-tallow/register/only-at-load", test_tallow_register_only_at_load);
	ADD("/plugin-tallow/register/required", test_tallow_needs_register);
	ADD("/plugin-tallow/kernels/not-interpretable", test_tallow_kernels_not_interpretable);
	ADD("/plugin-tallow/kernels/host-functions", test_tallow_kernels_are_host_functions);
	ADD("/plugin-tallow/kernels/contract-error", test_tallow_kernel_contract_is_an_error);
	ADD("/plugin-tallow/threads", test_tallow_threads);

#undef ADD

	return g_test_run();
}
