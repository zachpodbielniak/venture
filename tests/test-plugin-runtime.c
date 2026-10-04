/*
 * test-plugin-runtime.c - Plugin runtimes, manifests, exec and its protocol
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three claims, each of which has a way to be quietly false:
 *
 *   - a runtime is a seam: a plugin can add one, a file waiting for it
 *     loads once it exists, and the bare .so/.c/.yaml files that worked
 *     before still load exactly as before;
 *   - a manifest cannot reach outside its own directory, by `..`, by an
 *     absolute path, by a symlink, or by a sibling whose name starts the
 *     same way;
 *   - an exec plugin gets its request on stdin and nothing else of the
 *     server's -- no environment, no secret in argv or in what comes back
 *     -- and cannot hold the server past its deadline or its caps.
 *
 * The exec cases drive tests/fixtures/exec/probe.sh, copied into a scratch
 * plugin directory per test with a manifest naming the mode.
 */

#include <venture.h>

#include <glib/gstdio.h>

#include <stdlib.h>
#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureDatabase		*database;
	VentureConfig		*config;
	VentureContext		*context;
	VenturePluginManager	*manager;
	gchar			*plugin_dir;
	gchar			*outside_dir;
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

	/* A scratch plugin directory, a second directory standing for
	 * "somewhere else on the machine", and a state directory so the crispy
	 * cache never lands in the developer's real one. */
	fixture->plugin_dir = g_dir_make_tmp("venture-runtime-XXXXXX", NULL);
	fixture->outside_dir = g_dir_make_tmp("venture-outside-XXXXXX", NULL);
	fixture->state_dir = g_dir_make_tmp("venture-rstate-XXXXXX", NULL);
	g_object_set(fixture->config, "state-dir", fixture->state_dir, NULL);

	/* As main() does: on the context before anything loads, because a
	 * plugin reaches the manager through the context. */
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
	venture_test_remove_tree(fixture->outside_dir);
	venture_test_remove_tree(fixture->state_dir);

	g_clear_pointer(&fixture->plugin_dir, g_free);
	g_clear_pointer(&fixture->outside_dir, g_free);
	g_clear_pointer(&fixture->state_dir, g_free);

	/* The context holds the manager and the manager the context, so the
	 * cycle is broken by hand. */
	venture_context_set_plugin_manager(fixture->context, NULL);
	g_clear_object(&fixture->manager);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

static gchar *
write_file(
	const gchar	*directory,
	const gchar	*name,
	const gchar	*contents
){
	gchar *path;

	path = g_build_filename(directory, name, NULL);
	g_assert_true(g_file_set_contents(path, contents, -1, NULL));

	return path;
}

static gchar *
make_dir(
	const gchar	*parent,
	const gchar	*name
){
	gchar *path;

	path = g_build_filename(parent, name, NULL);
	g_assert_cmpint(g_mkdir_with_parents(path, 0755), ==, 0);

	return path;
}

/* Copies the probe script into @directory, executable. */
static gchar *
install_probe(const gchar *directory)
{
	g_autofree gchar *source = NULL;
	g_autofree gchar *contents = NULL;
	gchar *target;
	gsize length;

	source = g_build_filename(VENTURE_TEST_FIXTURES, "exec", "probe.sh", NULL);
	g_assert_true(g_file_get_contents(source, &contents, &length, NULL));

	target = g_build_filename(directory, "probe.sh", NULL);
	g_assert_true(g_file_set_contents(target, contents, (gssize)length, NULL));
	g_assert_cmpint(g_chmod(target, 0755), ==, 0);

	return target;
}

/*
 * Writes an exec plugin: a directory named @name under the plugin
 * directory, holding the probe and a manifest that runs it in @mode.
 * @exec_extra is more YAML for the exec section, indented two spaces;
 * @top_extra is more top-level YAML.
 */
static gchar *
write_exec_plugin(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*mode,
	const gchar	*exec_extra,
	const gchar	*top_extra
){
	g_autofree gchar *directory = NULL;
	g_autofree gchar *probe = NULL;
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *manifest_name = NULL;

	directory = make_dir(fixture->plugin_dir, name);
	probe = install_probe(directory);

	manifest = g_strdup_printf("name: %s\n"
	                           "runtime: exec\n"
	                           "protocol: 1\n"
	                           "description: The probe, in %s mode\n"
	                           "entry: probe.sh\n"
	                           "exec:\n"
	                           "  args: [%s]\n"
	                           "%s"
	                           "%s",
	                           name, mode, mode,
	                           (NULL != exec_extra) ? exec_extra : "",
	                           (NULL != top_extra) ? top_extra : "");
	manifest_name = g_strdup_printf("%s.plugin.yaml", name);

	return write_file(directory, manifest_name, manifest);
}

static void
allow_exec(
	Fixture		*fixture,
	gboolean	 allowed
){
	g_object_set(fixture->config, "plugins-allow-exec", allowed, NULL);
}

/* The record for @name in the list JSON, or NULL. */
static JsonObject *
find_listed(
	JsonNode	*list,
	const gchar	*name
){
	JsonArray *array;
	guint i;

	array = json_node_get_array(list);

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *entry;

		entry = json_array_get_object_element(array, i);

		if (0 == g_strcmp0(json_object_get_string_member(entry, "name"), name))
			return entry;
	}

	return NULL;
}

/* ==========================================================================
 * Runtimes
 * ========================================================================== */

/*
 * A fake runtime: claims one extension, counts its loads and writes down
 * what it was handed. What the manager does around it is under test, not
 * what it does.
 */
typedef struct
{
	GPtrArray		*paths;		/* basenames, in load order */
	guint			 loaded_at_call;	/* manager count at last call */
	VenturePluginManager	*manager;
	gboolean		 add_late;	/* register ".late" when loading */
} FakeRuntime;

static void
fake_runtime_free(gpointer data)
{
	FakeRuntime *fake;

	fake = data;
	g_ptr_array_unref(fake->paths);
	g_free(fake);
}

static gboolean
fake_runtime_load(
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
);

static FakeRuntime *
fake_runtime_new(VenturePluginManager *manager)
{
	FakeRuntime *fake;

	fake = g_new0(FakeRuntime, 1);
	fake->paths = g_ptr_array_new_with_free_func(g_free);
	fake->manager = manager;

	return fake;
}

static gboolean
fake_runtime_load(
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
){
	FakeRuntime *fake;

	(void)context;
	(void)manifest;

	fake = user_data;
	g_ptr_array_add(fake->paths, g_path_get_basename(path));
	fake->loaded_at_call = venture_plugin_manager_get_count(manager);
	venture_plugin_record_set_description(record, "faked");

	if (g_str_has_suffix(path, "refuse.fake"))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "the fake runtime refuses %s", path);
		return FALSE;
	}

	/* A plugin that registers a runtime from inside its own load, as an
	 * embedded interpreter would. */
	if (fake->add_late)
	{
		static const gchar *const late[] = { ".late", NULL };
		FakeRuntime *late_fake;
		g_autoptr(VenturePluginRuntime) runtime = NULL;

		late_fake = g_object_get_data(G_OBJECT(manager), "late-fake");
		runtime = venture_func_plugin_runtime_new("late", late,
		                                          fake_runtime_load,
		                                          late_fake, NULL);
		g_assert_true(venture_plugin_manager_add_runtime(manager, runtime,
		                                                 NULL));
	}

	return TRUE;
}

static void
test_runtime_builtins_registered(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_auto(GStrv) names = NULL;
	const gchar *const expected[] = {
		"native", "crispy", "declarative", "exec", NULL
	};

	(void)user_data;

	/* The four that ship, in order. A manager missing one would load
	 * every bare file of that kind as "no runtime", quietly. */
	names = venture_plugin_manager_list_runtimes(fixture->manager);
	g_assert_true(g_strv_equal((const gchar *const *)names, expected));
}

static void
test_runtime_bare_files_unchanged(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) list = NULL;
	JsonObject *entry;

	(void)user_data;

	/*
	 * A bare .yaml loads as it always did -- named by its file, kind
	 * declarative -- and the list now also says which runtime took it.
	 * The old fields keeping their values is what keeps the /plugins page
	 * and the API's consumers working.
	 */
	path = write_file(fixture->plugin_dir, "etsy.yaml",
	                  "name: etsy\ndescription: A shop\n"
	                  "fields:\n  shop_name: string\n");

	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, NULL), ==, 1);

	list = venture_plugin_manager_list(fixture->manager);
	entry = find_listed(list, "etsy.yaml");
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "kind"), ==,
	                "declarative");
	g_assert_cmpstr(json_object_get_string_member(entry, "runtime"), ==,
	                "declarative");
	g_assert_cmpstr(json_object_get_string_member(entry, "path"), ==, path);
	g_assert_cmpstr(json_object_get_string_member(entry, "description"), ==,
	                "A shop");
	g_assert_false(json_object_has_member(entry, "provides"));
}

static void
test_runtime_deferred_file_loads_when_runtime_arrives(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginRuntime) runtime = NULL;
	g_autoptr(JsonNode) list = NULL;
	g_auto(GStrv) deferred = NULL;
	g_autofree gchar *fake_path = NULL;
	g_autofree gchar *readme = NULL;
	static const gchar *const extensions[] = { ".fake", NULL };
	FakeRuntime *fake;
	JsonObject *entry;

	(void)user_data;

	fake_path = write_file(fixture->plugin_dir, "a.fake", "anything\n");
	readme = write_file(fixture->plugin_dir, "README.org", "* Notes\n");
	g_free(write_file(fixture->plugin_dir, "b.yaml",
	                  "name: bee\nfields:\n  thing: string\n"));
	g_free(write_file(fixture->plugin_dir, "LICENSE", "AGPL\n"));

	/*
	 * Nothing claims .fake yet: it is held, quietly -- a plugin directory
	 * with a README in it is normal, and a warning per file would be
	 * noise. A file with no extension at all is not even held.
	 */
	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, NULL), ==, 1);

	deferred = venture_plugin_manager_list_deferred(fixture->manager);
	g_assert_cmpuint(g_strv_length(deferred), ==, 2);
	g_assert_cmpstr(deferred[0], ==, readme);
	g_assert_cmpstr(deferred[1], ==, fake_path);

	fake = fake_runtime_new(fixture->manager);
	runtime = venture_func_plugin_runtime_new("fake", extensions,
	                                          fake_runtime_load, fake,
	                                          fake_runtime_free);

	/* Registering the runtime is what loads the held file. */
	g_assert_true(venture_plugin_manager_add_runtime(fixture->manager,
	                                                 runtime, NULL));
	g_assert_cmpuint(fake->paths->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(fake->paths, 0), ==, "a.fake");
	g_assert_cmpuint(venture_plugin_manager_get_count(fixture->manager), ==, 2);

	list = venture_plugin_manager_list(fixture->manager);
	entry = find_listed(list, "a.fake");
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "runtime"), ==,
	                "fake");

	/* A runtime that does not name a kind is "other": the kind is a
	 * closed list, the runtimes are not. */
	g_assert_cmpstr(json_object_get_string_member(entry, "kind"), ==, "other");

	/* Only the README is still waiting. */
	g_clear_pointer(&deferred, g_strfreev);
	deferred = venture_plugin_manager_list_deferred(fixture->manager);
	g_assert_cmpuint(g_strv_length(deferred), ==, 1);
	g_assert_cmpstr(deferred[0], ==, readme);
}

static void
test_runtime_explicit_unknown_file_is_refused_and_held(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(VenturePluginRuntime) runtime = NULL;
	static const gchar *const extensions[] = { ".fake", NULL };
	FakeRuntime *fake;

	(void)user_data;

	path = write_file(fixture->plugin_dir, "thing.fake", "x\n");

	/* Asked for by name, an unclaimed file is an error -- the caller
	 * wanted it loaded now -- and is still held for later. */
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, path,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);

	fake = fake_runtime_new(fixture->manager);
	runtime = venture_func_plugin_runtime_new("fake", extensions,
	                                          fake_runtime_load, fake,
	                                          fake_runtime_free);
	g_assert_true(venture_plugin_manager_add_runtime(fixture->manager,
	                                                 runtime, NULL));
	g_assert_cmpuint(fake->paths->len, ==, 1);
}

static void
test_runtime_added_mid_scan_loads_in_order(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginRuntime) boot = NULL;
	static const gchar *const boot_extensions[] = { ".boot", NULL };
	FakeRuntime *boot_fake;
	FakeRuntime *late_fake;

	(void)user_data;

	g_free(write_file(fixture->plugin_dir, "a.late", "1\n"));
	g_free(write_file(fixture->plugin_dir, "b.boot", "2\n"));
	g_free(write_file(fixture->plugin_dir, "c.late", "3\n"));

	late_fake = fake_runtime_new(fixture->manager);
	g_object_set_data_full(G_OBJECT(fixture->manager), "late-fake",
	                       late_fake, fake_runtime_free);

	boot_fake = fake_runtime_new(fixture->manager);
	boot_fake->add_late = TRUE;
	boot = venture_func_plugin_runtime_new("boot", boot_extensions,
	                                       fake_runtime_load, boot_fake,
	                                       fake_runtime_free);
	g_assert_true(venture_plugin_manager_add_runtime(fixture->manager, boot,
	                                                 NULL));

	/*
	 * a.late is met before anything claims it; b.boot registers the
	 * runtime that does, from inside its own load; c.late comes after.
	 * The held file loads once b.boot has *finished* -- loading it from
	 * inside b.boot's registration would interleave two plugins' setup --
	 * and the order is the sorted order, every time.
	 */
	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, NULL), ==, 2);

	g_assert_cmpuint(late_fake->paths->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(late_fake->paths, 0), ==, "a.late");
	g_assert_cmpstr(g_ptr_array_index(late_fake->paths, 1), ==, "c.late");
	g_assert_cmpuint(venture_plugin_manager_get_count(fixture->manager), ==, 3);

	/* When c.late loaded, b.boot and a.late were both already recorded. */
	g_assert_cmpuint(late_fake->loaded_at_call, ==, 2);
}

static void
test_runtime_conflicts_refused(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const so[] = { ".so", NULL };
	static const gchar *const manifest[] = { ".plugin.yaml", NULL };
	static const gchar *const no_dot[] = { "lua", NULL };
	g_autoptr(VenturePluginRuntime) same_name = NULL;
	g_autoptr(VenturePluginRuntime) same_extension = NULL;
	g_autoptr(VenturePluginRuntime) claims_manifest = NULL;
	g_autoptr(VenturePluginRuntime) bad_extension = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	/*
	 * One name, one runtime; one extension, one runtime. Either clash
	 * would make what loads a file depend on which plugin registered
	 * first -- which is file naming -- and a manifest must stay a
	 * manifest whatever claims .yaml.
	 */
	same_name = venture_func_plugin_runtime_new("native", NULL,
	                                            fake_runtime_load, NULL, NULL);
	g_assert_false(venture_plugin_manager_add_runtime(fixture->manager,
	                                                  same_name, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	same_extension = venture_func_plugin_runtime_new("othernative", so,
	                                                 fake_runtime_load, NULL,
	                                                 NULL);
	g_assert_false(venture_plugin_manager_add_runtime(fixture->manager,
	                                                  same_extension, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	claims_manifest = venture_func_plugin_runtime_new("grabby", manifest,
	                                                  fake_runtime_load, NULL,
	                                                  NULL);
	g_assert_false(venture_plugin_manager_add_runtime(fixture->manager,
	                                                  claims_manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	bad_extension = venture_func_plugin_runtime_new("lua", no_dot,
	                                                fake_runtime_load, NULL,
	                                                NULL);
	g_assert_false(venture_plugin_manager_add_runtime(fixture->manager,
	                                                  bad_extension, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* ==========================================================================
 * Manifests
 * ========================================================================== */

static void
test_manifest_declarative_in_subdirectory(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *directory = NULL;
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *deeper = NULL;
	g_autoptr(JsonNode) list = NULL;
	VentureVentureTypeRegistry *types;
	JsonObject *entry;

	(void)user_data;

	directory = make_dir(fixture->plugin_dir, "shop");
	g_free(write_file(directory, "shop.yaml",
	                  "name: shop\nfields:\n  sku: string\n"));

	/* Beside the manifest but not named by it: never a plugin. */
	g_free(write_file(directory, "helper.yaml",
	                  "name: helper\nfields:\n  x: string\n"));

	/* Two levels down: never scanned. */
	deeper = make_dir(directory, "nested");
	g_free(write_file(deeper, "deep.plugin.yaml",
	                  "name: deep\nentry: ../shop.yaml\n"));

	manifest = write_file(directory, "shop.plugin.yaml",
	                      "name: shop-type\n"
	                      "description: A shop, by manifest\n"
	                      "entry: shop.yaml\n");

	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, NULL), ==, 1);

	types = venture_context_get_venture_types(fixture->context);
	g_assert_nonnull(venture_venture_type_registry_lookup(types, "shop"));
	g_assert_null(venture_venture_type_registry_lookup(types, "helper"));

	/* Named by the manifest, at the manifest's path; the runtime was
	 * picked by the entry's extension since none was named. */
	list = venture_plugin_manager_list(fixture->manager);
	entry = find_listed(list, "shop-type");
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "path"), ==, manifest);
	g_assert_cmpstr(json_object_get_string_member(entry, "runtime"), ==,
	                "declarative");
	g_assert_null(find_listed(list, "deep"));
}

static void
test_manifest_entry_must_stay_inside(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *directory = NULL;
	g_autofree gchar *outside = NULL;
	g_autofree gchar *evil = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	directory = make_dir(fixture->plugin_dir, "plug");
	outside = write_file(fixture->outside_dir, "outside.yaml",
	                     "name: outside\nfields:\n  x: string\n");

	/* `..` out of the directory, to a file that exists. */
	{
		g_autofree gchar *outside_base = NULL;
		g_autofree gchar *up = NULL;

		outside_base = g_path_get_basename(fixture->outside_dir);
		up = g_strdup_printf("name: up\nentry: ../../%s/outside.yaml\n",
		                     outside_base);
		manifest = write_file(directory, "a.plugin.yaml", up);
	}
	g_assert_false(venture_plugin_manager_load_file(fixture->manager,
	                                                manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_clear_pointer(&manifest, g_free);

	/* An absolute path elsewhere. */
	body = g_strdup_printf("name: absolute\nentry: %s\n", outside);
	manifest = write_file(directory, "b.plugin.yaml", body);
	g_assert_false(venture_plugin_manager_load_file(fixture->manager,
	                                                manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_clear_pointer(&manifest, g_free);

	/* A symlink inside that points outside: realpath follows it. */
	link = g_build_filename(directory, "inside.yaml", NULL);
	g_assert_cmpint(symlink(outside, link), ==, 0);
	manifest = write_file(directory, "c.plugin.yaml",
	                      "name: linked\nentry: inside.yaml\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager,
	                                                manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_clear_pointer(&manifest, g_free);

	/* A sibling whose name starts with the directory's: `plug-evil` is
	 * not inside `plug`, though a bare prefix test says it is. */
	evil = make_dir(fixture->plugin_dir, "plug-evil");
	g_free(write_file(evil, "evil.yaml", "name: evil\nfields:\n  x: string\n"));
	manifest = write_file(directory, "d.plugin.yaml",
	                      "name: sibling\nentry: ../plug-evil/evil.yaml\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager,
	                                                manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);

	g_assert_cmpuint(venture_plugin_manager_get_count(fixture->manager), ==, 0);
	g_assert_null(venture_venture_type_registry_lookup(
		venture_context_get_venture_types(fixture->context), "outside"));
	g_assert_null(venture_venture_type_registry_lookup(
		venture_context_get_venture_types(fixture->context), "evil"));
}

static void
test_manifest_rejects_bad_shapes(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	struct
	{
		const gchar	*body;
		const gchar	*expect;
	} cases[] = {
		{ "entry: x.yaml\n", "\"name\" is required" },
		{ "name: Has Spaces\nentry: x.yaml\n", "\"name\" is required" },
		{ "name: ok\n", "\"entry\" is required" },
		{ "name: ok\nentry: x.yaml\nprovides: one\n", "must be a list" },
		{ "name: ok\nentry: x.yaml\nprovides:\n  - name: no-kind\n",
		  "needs a \"kind\"" },
		{ "name: ok\nentry: x.yaml\nprotocol: soon\n", "whole number" },
		{ "name: ok\nentry: missing.yaml\n", "cannot be resolved" },
		{ "- a list\n- not a mapping\n", "must be a mapping" },
		{ "name: ok\nentry: x.yaml\ntypo_key: 1\n", "not a manifest key" },
	};
	g_autofree gchar *directory = NULL;
	gsize i;

	(void)user_data;

	directory = make_dir(fixture->plugin_dir, "shapes");
	g_free(write_file(directory, "x.yaml", "name: x\nfields:\n  a: string\n"));

	/*
	 * Each is refused with a message that says what is wrong. A key that
	 * is not one is refused too: a misspelt setting that silently does
	 * nothing is the hardest manifest bug to find.
	 */
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *manifest = NULL;
		g_autoptr(GError) error = NULL;

		name = g_strdup_printf("case%" G_GSIZE_FORMAT ".plugin.yaml", i);
		manifest = write_file(directory, name, cases[i].body);

		g_assert_false(venture_plugin_manager_load_file(fixture->manager,
		                                                manifest, &error));
		g_assert_nonnull(error);

		if (NULL == strstr(error->message, cases[i].expect))
			g_error("case %" G_GSIZE_FORMAT ": \"%s\" lacks \"%s\"", i,
			        error->message, cases[i].expect);
	}
}

static void
test_manifest_unknown_runtime_is_held(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *directory = NULL;
	g_autofree gchar *manifest = NULL;
	g_autoptr(VenturePluginRuntime) runtime = NULL;
	g_autoptr(JsonNode) list = NULL;
	FakeRuntime *fake;
	JsonObject *entry;

	(void)user_data;

	directory = make_dir(fixture->plugin_dir, "scripted");
	g_free(write_file(directory, "main.lua", "print('hi')\n"));

	/* A runtime of its own gets a section of its own in the manifest. */
	manifest = write_file(directory, "scripted.plugin.yaml",
	                      "name: scripted\n"
	                      "runtime: lua\n"
	                      "entry: main.lua\n"
	                      "lua:\n  strict: true\n");

	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, NULL), ==, 0);

	fake = fake_runtime_new(fixture->manager);
	runtime = venture_func_plugin_runtime_new("lua", NULL, fake_runtime_load,
	                                          fake, fake_runtime_free);
	g_assert_true(venture_plugin_manager_add_runtime(fixture->manager,
	                                                 runtime, NULL));

	/* The runtime loads the entry, not the manifest. */
	g_assert_cmpuint(fake->paths->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(fake->paths, 0), ==, "main.lua");

	list = venture_plugin_manager_list(fixture->manager);
	entry = find_listed(list, "scripted");
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "path"), ==, manifest);

	/* The plugin described itself; the manifest's description is only
	 * the fallback. */
	g_assert_cmpstr(json_object_get_string_member(entry, "description"), ==,
	                "faked");
}

static void
test_manifest_name_is_unique(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *one = NULL;
	g_autofree gchar *two = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	one = make_dir(fixture->plugin_dir, "one");
	two = make_dir(fixture->plugin_dir, "two");
	g_free(write_file(one, "a.yaml", "name: a\nfields:\n  x: string\n"));
	g_free(write_file(two, "b.yaml", "name: b\nfields:\n  x: string\n"));
	first = write_file(one, "m.plugin.yaml", "name: same\nentry: a.yaml\n");
	second = write_file(two, "m.plugin.yaml", "name: same\nentry: b.yaml\n");

	/* The name keys the plugin's stored configuration and, for exec, its
	 * program: two plugins sharing one would share both. */
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, first,
	                                               &error));
	g_assert_no_error(error);

	g_assert_false(venture_plugin_manager_load_file(fixture->manager, second,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_assert_nonnull(strstr(error->message, first));
	g_assert_null(venture_venture_type_registry_lookup(
		venture_context_get_venture_types(fixture->context), "b"));
}

/*
 * The example plugin, loaded through a manifest by the native runtime.
 * In a subprocess: a copy of the .so is a second object with the same
 * GType names, and registering those twice in one process is fatal -- so
 * the copy is the only one that process ever sees.
 */
static void
test_manifest_native(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	const gchar *example;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *copy = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) list = NULL;
	gsize length;
	JsonObject *entry;

	(void)user_data;

	example = g_getenv("VENTURE_TEST_EXAMPLE_PLUGIN");

	if ((NULL == example) || !g_file_test(example, G_FILE_TEST_EXISTS))
	{
		g_test_skip("the example plugin was not built");
		return;
	}

	if (!g_test_subprocess())
	{
		g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}

	directory = make_dir(fixture->plugin_dir, "example");
	g_assert_true(g_file_get_contents(example, &contents, &length, NULL));
	copy = g_build_filename(directory, "example.so", NULL);
	g_assert_true(g_file_set_contents(copy, contents, (gssize)length, NULL));
	g_free(write_file(directory, "example.plugin.yaml",
	                  "name: example-native\n"
	                  "runtime: native\n"
	                  "entry: example.so\n"));

	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, &error), ==, 1);

	list = venture_plugin_manager_list(fixture->manager);
	entry = find_listed(list, "example-native");
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "kind"), ==, "native");
	g_assert_cmpstr(json_object_get_string_member(entry, "runtime"), ==,
	                "native");

	/* It really registered: its report is in the shared registry. */
	g_assert_nonnull(venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context),
		"subscriptions"));
}

static void
test_manifest_crispy(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *directory = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) list = NULL;
	JsonObject *entry;

	(void)user_data;

	if (!g_file_test(VENTURE_DEV_INCLUDE_DIR, G_FILE_TEST_IS_DIR))
	{
		g_test_skip("no development include tree to compile against");
		return;
	}

	directory = make_dir(fixture->plugin_dir, "probe");
	g_free(write_file(directory, "probe.c",
		"#include <venture/venture.h>\n"
		"\n"
		"gboolean venture_plugin_register(VentureContext *context,\n"
		"                                 GError **error);\n"
		"\n"
		"gboolean\n"
		"venture_plugin_register(VentureContext *context, GError **error)\n"
		"{\n"
		"\t(void)error;\n"
		"\tg_object_set_data(G_OBJECT(context), \"crispy-probe\",\n"
		"\t                  GINT_TO_POINTER(42));\n"
		"\treturn TRUE;\n"
		"}\n"));
	g_free(write_file(directory, "probe.plugin.yaml",
	                  "name: crispy-probe\n"
	                  "runtime: crispy\n"
	                  "description: Compiled through a manifest\n"
	                  "entry: probe.c\n"));

	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, &error), ==, 1);

	g_assert_cmpint(GPOINTER_TO_INT(g_object_get_data(
		G_OBJECT(fixture->context), "crispy-probe")), ==, 42);

	list = venture_plugin_manager_list(fixture->manager);
	entry = find_listed(list, "crispy-probe");
	g_assert_nonnull(entry);
	g_assert_cmpstr(json_object_get_string_member(entry, "kind"), ==, "crispy");

	/* No venture_plugin_info(), so the manifest's description stands. */
	g_assert_cmpstr(json_object_get_string_member(entry, "description"), ==,
	                "Compiled through a manifest");
}

/* ==========================================================================
 * Provides
 * ========================================================================== */

typedef struct
{
	GPtrArray	*names;
	gboolean	 saw_exec;
} ProvidesProbe;

static gboolean
provides_probe_accept(
	VenturePluginManager	 *manager,
	VenturePluginManifest	 *manifest,
	JsonObject		 *entry,
	gpointer		  user_data,
	GError			**error
){
	ProvidesProbe *probe;
	const gchar *name;

	probe = user_data;
	name = json_object_get_string_member_with_default(entry, "name", "");

	/* The plugin is loaded before its provides are judged, so the
	 * handler can already find the program it will run. */
	probe->saw_exec = (NULL != venture_plugin_manager_lookup_exec(manager,
		venture_plugin_manifest_get_name(manifest)));

	if (0 == g_strcmp0(name, "refuse-me"))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "this entry is refused");
		return FALSE;
	}

	g_ptr_array_add(probe->names, g_strdup(name));

	return TRUE;
}

static void
test_provides_dispatched_to_registered_kind(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	ProvidesProbe probe;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) list = NULL;
	g_autofree gchar *good = NULL;
	g_autofree gchar *unknown = NULL;
	g_autofree gchar *refused = NULL;
	JsonObject *entry;
	JsonArray *provides;

	(void)user_data;

	allow_exec(fixture, TRUE);
	probe.names = g_ptr_array_new_with_free_func(g_free);
	probe.saw_exec = FALSE;

	g_assert_true(venture_plugin_provides_registry_add(
		venture_context_get_plugin_provides(fixture->context),
		"probe_kind", "A kind only this test understands",
		provides_probe_accept, &probe, NULL, &error));
	g_assert_no_error(error);

	/* A kind registered twice would have two owners. */
	g_assert_false(venture_plugin_provides_registry_add(
		venture_context_get_plugin_provides(fixture->context),
		"probe_kind", NULL, provides_probe_accept, &probe, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	good = write_exec_plugin(fixture, "good", "echo", NULL,
	                         "provides:\n"
	                         "  - kind: probe_kind\n    name: first\n"
	                         "  - kind: probe_kind\n    name: second\n");
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, good,
	                                               &error));
	g_assert_no_error(error);

	g_assert_cmpuint(probe.names->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(probe.names, 0), ==, "first");
	g_assert_cmpstr(g_ptr_array_index(probe.names, 1), ==, "second");
	g_assert_true(probe.saw_exec);

	list = venture_plugin_manager_list(fixture->manager);
	entry = find_listed(list, "good");
	provides = json_object_get_array_member(entry, "provides");
	g_assert_cmpuint(json_array_get_length(provides), ==, 2);
	g_assert_cmpstr(json_array_get_string_element(provides, 0), ==,
	                "probe_kind");

	/*
	 * A kind nothing understands fails the plugin, naming the kinds that
	 * exist -- and the program it registered is withdrawn, so nothing
	 * can run a plugin that did not load.
	 */
	unknown = write_exec_plugin(fixture, "unknown", "echo", NULL,
	                            "provides:\n  - kind: data_source_typo\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, unknown,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "probe_kind"));
	g_assert_null(venture_plugin_manager_lookup_exec(fixture->manager,
	                                                 "unknown"));
	g_clear_error(&error);

	refused = write_exec_plugin(fixture, "refused", "echo", NULL,
	                            "provides:\n"
	                            "  - kind: probe_kind\n    name: refuse-me\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, refused,
	                                                &error));
	g_assert_nonnull(strstr(error->message, "this entry is refused"));
	g_assert_null(venture_plugin_manager_lookup_exec(fixture->manager,
	                                                 "refused"));

	g_ptr_array_unref(probe.names);
}

/*
 * A manifest's provides are all or nothing. Every entry is judged before
 * any is registered, so a plugin whose third entry is malformed leaves
 * neither its provider nor its handler behind; and two entries naming
 * the same provider -- which judging one at a time cannot see -- take the
 * first back when the second is refused. The names are free again for a
 * plugin that loads.
 *
 * What breaks if this regresses: a plugin reported as not loaded still
 * offers a data source provider or an automation handler whose program
 * the manager has withdrawn, and the next plugin that wants the name is
 * refused as a duplicate of something that never loaded.
 */
static void
test_provides_all_or_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *malformed = NULL;
	g_autofree gchar *twice = NULL;
	g_autofree gchar *good = NULL;
	VentureDataSourceProviderRegistry *providers;
	VentureAutomationHandlerRegistry *handlers;

	(void)user_data;

	allow_exec(fixture, TRUE);
	providers = venture_context_get_data_source_providers(fixture->context);
	handlers = venture_context_get_automation_handlers(fixture->context);

	malformed = write_exec_plugin(fixture, "malformed", "echo", NULL,
	                              "provides:\n"
	                              "  - kind: data_source_provider\n    name: whole_feed\n"
	                              "  - kind: automation_handler\n    name: whole_step\n"
	                              "  - kind: data_source_provider\n    name: Not A Name\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, malformed, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_clear_error(&error);
	g_assert_null(venture_data_source_provider_registry_lookup(providers, "whole_feed"));
	g_assert_false(venture_automation_handler_registry_has(handlers, "whole_step"));

	twice = write_exec_plugin(fixture, "twice", "echo", NULL,
	                          "provides:\n"
	                          "  - kind: data_source_provider\n    name: twice_feed\n"
	                          "  - kind: data_source_provider\n    name: twice_feed\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, twice, &error));
	g_assert_nonnull(strstr(error->message, "already registered"));
	g_clear_error(&error);
	g_assert_null(venture_data_source_provider_registry_lookup(providers, "twice_feed"));
	g_assert_null(venture_plugin_manager_lookup_exec(fixture->manager, "twice"));

	good = write_exec_plugin(fixture, "whole", "echo", NULL,
	                         "provides:\n"
	                         "  - kind: data_source_provider\n    name: whole_feed\n"
	                         "  - kind: automation_handler\n    name: whole_step\n");
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, good, &error));
	g_assert_no_error(error);
	g_assert_nonnull(venture_data_source_provider_registry_lookup(providers, "whole_feed"));
	g_assert_true(venture_automation_handler_registry_has(handlers, "whole_step"));
}

/*
 * An exec plugin's data_source_provider may carry an `attribution`: the
 * line every page shows beside the provider's data. It is plain text,
 * judged when the manifest loads: a string of one line under the cap is
 * kept (trimmed), anything else -- a number, a list, a line with a
 * newline in it, a paragraph past the cap -- refuses the whole plugin
 * with the reason, rather than putting something mangled on every page.
 *
 * What breaks if this regresses: a plugin whose provider's terms require
 * attribution loads without it, or one whose manifest says `attribution:
 * 42` shows "42" as the credit line on every Trading page.
 */
static void
test_provides_attribution(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const struct
	{
		const gchar	*value;
		const gchar	*says;
	} refused[] = {
		{ "42", "must be a string" },
		{ "true", "must be a string" },
		{ "[one, two]", "must be a string" },
		{ "\"two\\nlines\"", "one line" },
		{ "\"   \"", "blank" },
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *good = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *overlong = NULL;
	g_autofree gchar *long_manifest = NULL;
	VentureDataSourceProviderRegistry *providers;
	guint i;

	(void)user_data;

	allow_exec(fixture, TRUE);
	providers = venture_context_get_data_source_providers(fixture->context);

	good = write_exec_plugin(fixture, "credited", "echo", NULL,
	                         "provides:\n"
	                         "  - kind: data_source_provider\n    name: credited_feed\n"
	                         "    attribution: \"  Prices from <Example> & 'Co'.  \"\n");
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, good, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(venture_data_source_provider_get_attribution(
		venture_data_source_provider_registry_lookup(providers, "credited_feed")), ==,
		"Prices from <Example> & 'Co'.");

	none = write_exec_plugin(fixture, "uncredited", "echo", NULL,
	                         "provides:\n"
	                         "  - kind: data_source_provider\n    name: uncredited_feed\n");
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, none, &error));
	g_assert_no_error(error);
	g_assert_null(venture_data_source_provider_get_attribution(
		venture_data_source_provider_registry_lookup(providers, "uncredited_feed")));

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autofree gchar *name = g_strdup_printf("refused%u", i);
		g_autofree gchar *feed = g_strdup_printf("refused_feed_%u", i);
		g_autofree gchar *extra = NULL;
		g_autofree gchar *path = NULL;

		extra = g_strdup_printf("provides:\n"
		                        "  - kind: data_source_provider\n    name: %s\n"
		                        "    attribution: %s\n", feed, refused[i].value);
		path = write_exec_plugin(fixture, name, "echo", NULL, extra);
		g_assert_false(venture_plugin_manager_load_file(fixture->manager, path, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);

		if (NULL == strstr(error->message, refused[i].says))
			g_error("attribution %s refused with \"%s\", not \"%s\"", refused[i].value,
			        error->message, refused[i].says);

		g_clear_error(&error);
		g_assert_null(venture_data_source_provider_registry_lookup(providers, feed));
	}

	/* Past the cap: refused, never cut short. */
	overlong = g_strnfill(VENTURE_DATA_SOURCE_ATTRIBUTION_MAX + 1, 'x');
	long_manifest = g_strdup_printf("provides:\n"
	                                "  - kind: data_source_provider\n    name: wordy_feed\n"
	                                "    attribution: %s\n", overlong);
	g_clear_pointer(&good, g_free);
	good = write_exec_plugin(fixture, "wordy", "echo", NULL, long_manifest);
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, good, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "at most"));
	g_clear_error(&error);
	g_assert_null(venture_data_source_provider_registry_lookup(providers, "wordy_feed"));
}

/* --- A plugin whose register function fails ------------------------------ */

static gboolean
rollback_fee(
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	JsonObject		 *attrs,
	VentureFeeQuote		 *out,
	gpointer		  user_data,
	GError			**error
){
	(void)params; (void)side; (void)amount; (void)units; (void)reference;
	(void)attrs; (void)out; (void)user_data; (void)error;
	return TRUE;
}

static GBytes *
rollback_export(
	JsonArray	 *rows,
	JsonObject	 *options,
	gpointer	  user_data,
	GError		**error
){
	(void)rows; (void)options; (void)user_data; (void)error;
	return g_bytes_new_static("", 0);
}

static gboolean
rollback_handler(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	(void)context; (void)name; (void)params; (void)result; (void)user_data; (void)error;
	return TRUE;
}

static VentureFeedBatch *
rollback_fetch(
	VentureFeedRequest	 *request,
	gpointer		  user_data,
	GError			**error
){
	(void)request;
	(void)user_data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN, "never fetched");
	return NULL;
}

static gboolean
rollback_web(
	VentureWebServer	 *server,
	gpointer		  user_data,
	GError			**error
){
	(void)server; (void)user_data; (void)error;
	return TRUE;
}

static gboolean
rollback_kind(
	VenturePluginManager	 *manager,
	VenturePluginManifest	 *manifest,
	JsonObject		 *entry,
	gpointer		  user_data,
	GError			**error
){
	(void)manager; (void)manifest; (void)entry; (void)user_data; (void)error;
	return TRUE;
}

/*
 * What a native plugin's venture_plugin_register() does, from a runtime's
 * load: register a fee model, an export format, an automation handler, a
 * data source provider, a provides kind, a web extension and a runtime;
 * then, for a .fail file, load another plugin through that runtime from
 * inside this load, and refuse.
 */
static gboolean
rollback_load(
	VenturePluginManager	 *manager,
	VentureContext		 *context,
	const gchar		 *path,
	VenturePluginManifest	 *manifest,
	VenturePluginRecord	 *record,
	gpointer		  user_data,
	GError			**error
){
	static const gchar *const inner_extensions[] = { ".inner", NULL };
	g_autoptr(VentureDataSourceProvider) provider = NULL;
	g_autoptr(VenturePluginRuntime) inner = NULL;

	(void)manifest;
	(void)record;
	(void)user_data;

	if (g_str_has_suffix(path, ".inner"))
	{
		venture_context_add_web_extension(context, rollback_web, NULL, NULL);

		return venture_automation_handler_registry_add(
			venture_context_get_automation_handlers(context), "inner_step", NULL,
			rollback_handler, NULL, NULL, error);
	}

	g_assert_true(venture_fee_model_registry_add(venture_context_get_fee_models(context),
		"rollback_fee", NULL, rollback_fee, NULL, NULL, NULL, NULL));
	g_assert_true(venture_export_format_registry_add(venture_context_get_export_formats(context),
		"rollback_fmt", "Rollback", "text/plain", "txt", rollback_export, NULL, NULL, NULL));
	g_assert_true(venture_automation_handler_registry_add(
		venture_context_get_automation_handlers(context), "rollback_step", NULL,
		rollback_handler, NULL, NULL, NULL));
	provider = venture_func_data_source_provider_new("rollback_feed", NULL, NULL,
	                                                 rollback_fetch, NULL, NULL);
	g_assert_true(venture_data_source_provider_registry_add(
		venture_context_get_data_source_providers(context), provider, NULL));
	g_assert_true(venture_plugin_provides_registry_add(venture_context_get_plugin_provides(context),
		"rollback_kind", NULL, rollback_kind, NULL, NULL, NULL));
	venture_context_add_web_extension(context, rollback_web, NULL, NULL);
	inner = venture_func_plugin_runtime_new("inner", inner_extensions, rollback_load, NULL, NULL);
	g_assert_true(venture_plugin_manager_add_runtime(manager, inner, NULL));

	if (g_str_has_suffix(path, ".fail"))
	{
		g_autofree gchar *directory = g_path_get_dirname(path);
		g_autofree gchar *nested = g_build_filename(directory, "nested.inner", NULL);

		/* A plugin that loads another from its own register function:
		 * the inner load succeeds, inside this one. */
		g_assert_true(venture_plugin_manager_load_file(manager, nested, NULL));

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s refused to register after all", path);
		return FALSE;
	}

	return TRUE;
}

/*
 * A plugin whose register function fails after registering takes nothing
 * with it into the registries: every name it added is removed again and
 * its web extension dropped, while what was there before (the built-in
 * percent model, the csv format) stays. A plugin it loaded from inside
 * its own load did load, and keeps what it registered.
 *
 * What breaks if this regresses: a scan offers a fee model, a page an
 * export, a rule a handler, all from a plugin the server reports as not
 * loaded; and a retried load is refused as a duplicate of itself.
 */
static void
test_failed_register_takes_back(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const extensions[] = { ".fail", ".pass", NULL };
	g_autoptr(VenturePluginRuntime) runtime = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *nested = NULL;
	g_autofree gchar *failing = NULL;
	g_autofree gchar *passing = NULL;
	g_auto(GStrv) runtimes = NULL;
	VentureContext *context;
	guint extensions_before;

	(void)user_data;

	context = fixture->context;
	runtime = venture_func_plugin_runtime_new("rollback", extensions, rollback_load, NULL, NULL);
	g_assert_true(venture_plugin_manager_add_runtime(fixture->manager, runtime, &error));
	g_assert_no_error(error);
	extensions_before = venture_context_count_web_extensions(context);

	nested = write_file(fixture->plugin_dir, "nested.inner", "");

	failing = write_file(fixture->plugin_dir, "broken.fail", "");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, failing, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_clear_error(&error);

	g_assert_false(venture_fee_model_registry_has(venture_context_get_fee_models(context),
	                                              "rollback_fee"));
	g_assert_true(venture_fee_model_registry_has(venture_context_get_fee_models(context), "percent"));
	g_assert_false(venture_export_format_registry_has(venture_context_get_export_formats(context),
	                                                  "rollback_fmt"));
	g_assert_true(venture_export_format_registry_has(venture_context_get_export_formats(context),
	                                                 "csv"));
	g_assert_false(venture_automation_handler_registry_has(
		venture_context_get_automation_handlers(context), "rollback_step"));
	g_assert_null(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(context), "rollback_feed"));
	g_assert_false(venture_plugin_provides_registry_has(venture_context_get_plugin_provides(context),
	                                                    "rollback_kind"));
	/* Its own extension went; the nested plugin's, added after it, stayed. */
	g_assert_cmpuint(venture_context_count_web_extensions(context), ==, extensions_before + 1);
	runtimes = venture_plugin_manager_list_runtimes(fixture->manager);
	g_assert_false(g_strv_contains((const gchar *const *)runtimes, "inner"));

	/* The plugin loaded inside the failed one keeps its own. */
	g_assert_true(venture_automation_handler_registry_has(
		venture_context_get_automation_handlers(context), "inner_step"));

	/* Nothing is left to collide with: the same registrations load. */
	passing = write_file(fixture->plugin_dir, "fixed.pass", "");
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, passing, &error));
	g_assert_no_error(error);
	g_assert_true(venture_fee_model_registry_has(venture_context_get_fee_models(context),
	                                             "rollback_fee"));
	g_assert_cmpuint(venture_context_count_web_extensions(context), ==, extensions_before + 2);
}

/* ==========================================================================
 * Exec
 * ========================================================================== */

/* The record message of a given record_type, or NULL. */
static JsonObject *
find_record(
	VentureExecResult	*result,
	const gchar		*record_type
){
	GPtrArray *messages;
	guint i;

	messages = venture_exec_result_get_messages(result);

	for (i = 0; i < messages->len; i++)
	{
		VentureJsonlMessage *message;

		message = g_ptr_array_index(messages, i);

		if ((VENTURE_JSONL_MESSAGE_RECORD ==
		     venture_jsonl_message_get_kind(message)) &&
		    (0 == g_strcmp0(venture_jsonl_message_get_string(message,
		                                                     "record_type"),
		                    record_type)))
			return json_object_get_object_member(
				venture_jsonl_message_get_object(message), "fields");
	}

	return NULL;
}

static void
test_exec_round_trip(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *real_root = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;
	g_autoptr(JsonObject) params = NULL;
	g_autoptr(JsonObject) secrets = NULL;
	JsonObject *request;
	JsonObject *env;
	GPtrArray *messages;
	gchar *resolved;

	(void)user_data;

	allow_exec(fixture, TRUE);

	/* Set before load, as an operator's environment would be. */
	g_setenv("VENTURE_TEST_LEAK", "the-servers-own-secret", TRUE);
	g_setenv("VENTURE_TEST_DECLARED", "declared-value", TRUE);

	manifest = write_exec_plugin(fixture, "probe", "echo",
	                             "  env: [VENTURE_TEST_DECLARED]\n", NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               &error));
	g_assert_no_error(error);

	/* Settings come from the plugin's stored configuration, read at the
	 * run -- so an edit on /plugins reaches the next run. */
	g_assert_true(venture_plugin_manager_set_config(fixture->manager, "probe",
		"rate: 65\nregion: eu\n", NULL, &error));
	g_assert_no_error(error);

	params = json_object_new();
	json_object_set_int_member(params, "page", 2);
	secrets = json_object_new();
	json_object_set_string_member(secrets, "token", "s3cret-token-value");

	result = venture_plugin_manager_run_exec(fixture->manager, "probe",
	                                         "fetch", params, secrets, NULL,
	                                         &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(venture_exec_result_check(result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_exec_result_get_exit_status(result), ==, 0);

	messages = venture_exec_result_get_messages(result);
	g_assert_cmpuint(messages->len, ==, 4);
	g_assert_cmpint(venture_jsonl_message_get_kind(
		g_ptr_array_index(messages, 0)), ==, VENTURE_JSONL_MESSAGE_LOG);
	g_assert_cmpint(venture_jsonl_message_get_kind(
		g_ptr_array_index(messages, 3)), ==, VENTURE_JSONL_MESSAGE_CURSOR);
	g_assert_cmpstr(venture_jsonl_message_get_string(
		g_ptr_array_index(messages, 3), "value"), ==, "c-1");

	/* The request, as the program read it on stdin. */
	request = find_record(result, "request");
	g_assert_nonnull(request);
	g_assert_cmpint(json_object_get_int_member(request, "protocol"), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(request, "plugin"), ==,
	                "probe");
	g_assert_cmpstr(json_object_get_string_member(request, "command"), ==,
	                "fetch");
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(request, "settings"), "rate"), ==, 65);
	g_assert_cmpint(json_object_get_int_member(
		json_object_get_object_member(request, "params"), "page"), ==, 2);

	/*
	 * The secret reached the program -- the member is there -- and was
	 * redacted out of what came back, so the batch can be stored and
	 * logged without carrying it.
	 */
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(request, "secrets"), "token"), ==,
		VENTURE_EXEC_REDACTED);
	g_assert_null(strstr(venture_exec_result_get_stderr(result),
	                     "s3cret-token-value"));
	g_assert_nonnull(strstr(venture_exec_result_get_stderr(result),
	                        VENTURE_EXEC_REDACTED));

	/* What the program saw of the server: its declared variable and the
	 * basics, not the server's own; its argv is the manifest's alone; it
	 * ran in its own directory. */
	env = find_record(result, "env");
	g_assert_nonnull(env);
	g_assert_cmpstr(json_object_get_string_member(env, "leak"), ==, "");
	g_assert_cmpstr(json_object_get_string_member(env, "declared"), ==,
	                "declared-value");
	g_assert_cmpstr(json_object_get_string_member(env, "has_path"), ==, "yes");
	g_assert_cmpstr(json_object_get_string_member(env, "argc"), ==, "1");
	g_assert_cmpstr(json_object_get_string_member(env, "args"), ==, "echo");

	{
		g_autofree gchar *root = NULL;

		root = g_build_filename(fixture->plugin_dir, "probe", NULL);
		resolved = realpath(root, NULL);
	}

	real_root = g_strdup(resolved);
	free(resolved);
	g_assert_cmpstr(json_object_get_string_member(env, "cwd"), ==, real_root);

	g_unsetenv("VENTURE_TEST_LEAK");
	g_unsetenv("VENTURE_TEST_DECLARED");
}

/*
 * Whether the process whose pid the probe left in @plugin's directory has
 * ended -- gone, or a zombie waiting for whoever adopted it to reap it,
 * which is ended for every purpose that matters here. Waits up to two
 * seconds for the signal to land.
 */
static gboolean
process_ends(
	Fixture		*fixture,
	const gchar	*plugin
){
	g_autofree gchar *pid_file = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *stat_path = NULL;
	gint64 deadline;

	pid_file = g_build_filename(fixture->plugin_dir, plugin, "sleep.pid",
	                            NULL);
	g_assert_true(g_file_get_contents(pid_file, &text, NULL, NULL));
	stat_path = g_strdup_printf("/proc/%s/stat", g_strstrip(text));

	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline)
	{
		g_autofree gchar *stat = NULL;
		const gchar *close_paren;

		if (!g_file_get_contents(stat_path, &stat, NULL, NULL))
			return TRUE;

		/* The state follows the command name's closing parenthesis. */
		close_paren = strrchr(stat, ')');

		if ((NULL != close_paren) && (0 == strncmp(close_paren, ") Z", 3)))
			return TRUE;

		g_usleep(20 * 1000);
	}

	return FALSE;
}

static void
test_exec_deadline_kills_the_group(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;
	gint64 started;
	gint64 elapsed;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "sleepy", "sleep",
	                             "  timeout: 1\n", NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/*
	 * The script's `sleep 30` is its child. The deadline stops the run on
	 * time, and the whole process group goes with it.
	 */
	started = g_get_monotonic_time();
	result = venture_plugin_manager_run_exec(fixture->manager, "sleepy", NULL,
	                                         NULL, NULL, NULL, &error);
	elapsed = g_get_monotonic_time() - started;

	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT);
	g_assert_cmpint(elapsed, <, 10 * G_USEC_PER_SEC);

	/* And the sleep is gone, not orphaned: killing only the script would
	 * leave it running for its thirty seconds, holding the pipe. */
	g_assert_true(process_ends(fixture, "sleepy"));
}

static void
test_exec_output_cap(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "flood", "flood",
	                             "  max_output: 4096\n", NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/* Failed, not truncated: a batch cut at the cap reads exactly like a
	 * complete one to whatever consumes it. */
	result = venture_plugin_manager_run_exec(fixture->manager, "flood", NULL,
	                                         NULL, NULL, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "more than 4096 bytes"));
}

static void
test_exec_stderr_is_capped_not_fatal(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "chatty", "stderr-flood",
	                             "  max_stderr: 100\n", NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/* Kept to the cap and drained past it: a program that logs a lot is
	 * neither blocked on a full pipe nor failed for logging. */
	result = venture_plugin_manager_run_exec(fixture->manager, "chatty", NULL,
	                                         NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(venture_exec_result_check(result, NULL));
	g_assert_true(venture_exec_result_get_stderr_truncated(result));
	g_assert_cmpuint(strlen(venture_exec_result_get_stderr(result)), <=, 100);
	g_assert_cmpuint(venture_exec_result_get_messages(result)->len, ==, 1);
}

static void
test_exec_nonzero_exit(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "failing", "fail", NULL, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/* The run completed, so there is a result -- with what was written
	 * before the failure -- and judging it says why it failed. */
	result = venture_plugin_manager_run_exec(fixture->manager, "failing", NULL,
	                                         NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(venture_exec_result_get_exit_status(result), ==, 3);
	g_assert_cmpuint(venture_exec_result_get_messages(result)->len, ==, 1);

	g_assert_false(venture_exec_result_check(result, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "status 3"));
	g_assert_nonnull(strstr(error->message, "something broke upstream"));
}

static void
test_exec_error_message_keeps_partial_batch(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "refusing", "error", NULL, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	result = venture_plugin_manager_run_exec(fixture->manager, "refusing",
	                                         NULL, NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);

	/* The stat before the error is still there for a consumer that
	 * takes partial batches; the error and its back-off are read out. */
	g_assert_cmpuint(venture_exec_result_get_messages(result)->len, ==, 2);
	g_assert_cmpstr(venture_exec_result_get_error_message(result), ==,
	                "upstream said no");
	g_assert_cmpint(venture_exec_result_get_retry_after(result), ==, 60);
	g_assert_false(venture_exec_result_check(result, &error));
	g_assert_nonnull(strstr(error->message, "upstream said no"));
}

static void
test_exec_protocol_violation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "garbled", "garbage", NULL, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/* The first line that is not the protocol fails the run, naming the
	 * line -- and not quoting it, since a line can carry anything. */
	result = venture_plugin_manager_run_exec(fixture->manager, "garbled", NULL,
	                                         NULL, NULL, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "Line 2"));
	g_assert_null(strstr(error->message, "this is not json"));
}

static void
test_exec_program_that_never_reads(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *big = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;
	g_autoptr(JsonObject) params = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "deaf", "no-read", NULL, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/*
	 * A request far larger than any socket buffer, to a program that exits
	 * without reading it. Through a pipe that write raises SIGPIPE and
	 * this test process dies; through the socket it is an error code the
	 * run ignores, because not reading is the program's choice.
	 */
	big = g_strnfill(4 * 1024 * 1024, 'x');
	params = json_object_new();
	json_object_set_string_member(params, "padding", big);

	result = venture_plugin_manager_run_exec(fixture->manager, "deaf", NULL,
	                                         params, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(venture_exec_result_check(result, NULL));
	g_assert_cmpuint(venture_exec_result_get_messages(result)->len, ==, 1);
}

/* Runs @name (the echo probe) with a secret, returning its stderr. */
static gchar *
probe_stderr(
	Fixture		*fixture,
	const gchar	*name,
	gboolean	*out_truncated
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;
	g_autoptr(JsonObject) secrets = NULL;

	secrets = json_object_new();
	json_object_set_string_member(secrets, "token", "s3cret-token-value");
	result = venture_plugin_manager_run_exec(fixture->manager, name, "fetch", NULL, secrets, NULL,
	                                         &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	*out_truncated = venture_exec_result_get_stderr_truncated(result);

	return g_strdup(venture_exec_result_get_stderr(result));
}

/*
 * A stderr cap that falls inside a secret keeps none of it. Redaction
 * replaces whole secrets only, so the first part of one cut off by the
 * cap survived at the very end of the capture -- the tail an error
 * quotes into run records and logs.
 */
static void
test_exec_stderr_cap_cuts_no_secret(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *wide_manifest = NULL;
	g_autofree gchar *cut_manifest = NULL;
	g_autofree gchar *cap = NULL;
	g_autofree gchar *wide = NULL;
	g_autofree gchar *cut = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *marker;
	gboolean truncated;

	(void)user_data;

	allow_exec(fixture, TRUE);

	/* Same-length names, so both requests -- echoed to stderr -- put
	 * the secret at the same offset. */
	wide_manifest = write_exec_plugin(fixture, "tellera", "echo", NULL, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, wide_manifest, &error));
	g_assert_no_error(error);
	wide = probe_stderr(fixture, "tellera", &truncated);
	marker = strstr(wide, VENTURE_EXEC_REDACTED);
	g_assert_nonnull(marker);
	g_assert_false(truncated);

	/* Cap ten bytes into the secret. */
	cap = g_strdup_printf("  max_stderr: %" G_GSIZE_FORMAT "\n", (gsize)(marker - wide) + 10);
	cut_manifest = write_exec_plugin(fixture, "tellerb", "echo", cap, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, cut_manifest, &error));
	g_assert_no_error(error);
	cut = probe_stderr(fixture, "tellerb", &truncated);
	g_assert_true(truncated);
	g_assert_null(strstr(cut, "s3cret"));

	/* Exactly the secret's ten bytes went; everything before stays. */
	g_assert_cmpuint(strlen(cut), ==, (gsize)(marker - wide));
}

static gpointer
cancel_later(gpointer data)
{
	g_usleep(200 * 1000);
	g_cancellable_cancel(G_CANCELLABLE(data));

	return NULL;
}

static void
test_exec_cancellation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;
	g_autoptr(GCancellable) cancellable = NULL;
	GThread *thread;
	gint64 started;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "waiting", "sleep",
	                             "  timeout: 60\n", NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/* Cancelled from another thread, as the feeds worker's shutdown will
	 * be: the run stops promptly rather than at its sixty-second deadline. */
	cancellable = g_cancellable_new();
	thread = g_thread_new("cancel-later", cancel_later, cancellable);

	started = g_get_monotonic_time();
	result = venture_plugin_manager_run_exec(fixture->manager, "waiting", NULL,
	                                         NULL, NULL, cancellable, &error);
	g_thread_join(thread);

	g_assert_null(result);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
}

static void
test_exec_executable_must_stay_inside(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *root = NULL;
	g_autofree gchar *evil = NULL;
	g_autofree gchar *evil_probe = NULL;
	g_autofree gchar *outside_probe = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *sibling = NULL;
	g_autofree gchar *absolute = NULL;
	g_autoptr(VentureExecSpec) spec = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);

	root = make_dir(fixture->plugin_dir, "plug");
	g_free(install_probe(root));
	evil = make_dir(fixture->plugin_dir, "plug-evil");
	evil_probe = install_probe(evil);
	outside_probe = install_probe(fixture->outside_dir);

	/*
	 * The sibling whose name extends the root's. A prefix test without
	 * the separator accepts it; the spec must not, whichever way the
	 * path is spelled.
	 */
	spec = venture_exec_spec_new("plug", root, "../plug-evil/probe.sh", &error);
	g_assert_null(spec);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	spec = venture_exec_spec_new("plug", root, evil_probe, &error);
	g_assert_null(spec);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* Through a manifest: the same refusal, before anything runs. */
	sibling = write_file(root, "sibling.plugin.yaml",
	                     "name: sibling\nruntime: exec\nprotocol: 1\n"
	                     "entry: ../plug-evil/probe.sh\n");
	g_assert_false(venture_plugin_manager_load_file(fixture->manager,
	                                                sibling, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	body = g_strdup_printf("name: absolute\nruntime: exec\nprotocol: 1\n"
	                       "entry: %s\n", outside_probe);
	absolute = write_file(root, "absolute.plugin.yaml", body);
	g_assert_false(venture_plugin_manager_load_file(fixture->manager,
	                                                absolute, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* A symlink inside the directory to a program outside it. */
	link = g_build_filename(root, "linked.sh", NULL);
	g_assert_cmpint(symlink(outside_probe, link), ==, 0);
	spec = venture_exec_spec_new("plug", root, "linked.sh", &error);
	g_assert_null(spec);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* A file that cannot be executed is not a program. */
	g_free(write_file(root, "data.txt", "not a program\n"));
	spec = venture_exec_spec_new("plug", root, "data.txt", &error);
	g_assert_null(spec);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_clear_error(&error);

	/* And the probe itself is fine. */
	spec = venture_exec_spec_new("plug", root, "probe.sh", &error);
	g_assert_no_error(error);
	g_assert_nonnull(spec);
}

static void
test_exec_rechecked_at_every_run(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *probe = NULL;
	g_autofree gchar *outside_probe = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;

	(void)user_data;

	allow_exec(fixture, TRUE);
	manifest = write_exec_plugin(fixture, "swapped", "echo", NULL, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                               NULL));

	/* After load, the program is replaced by a link to one elsewhere.
	 * The check at load proved nothing about now. */
	probe = g_build_filename(fixture->plugin_dir, "swapped", "probe.sh", NULL);
	outside_probe = install_probe(fixture->outside_dir);
	g_assert_cmpint(g_unlink(probe), ==, 0);
	g_assert_cmpint(symlink(outside_probe, probe), ==, 0);

	result = venture_plugin_manager_run_exec(fixture->manager, "swapped", NULL,
	                                         NULL, NULL, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static void
test_exec_allow_exec_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *later = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureExecResult) result = NULL;
	gboolean allowed;

	(void)user_data;

	/* Off is the default: an exec plugin appearing in a directory does
	 * not get to run because it appeared. */
	g_object_get(fixture->config, "plugins-allow-exec", &allowed, NULL);
	g_assert_false(allowed);

	manifest = write_exec_plugin(fixture, "blocked", "echo", NULL, NULL);
	g_assert_false(venture_plugin_manager_load_file(fixture->manager, manifest,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "allow_exec"));
	g_assert_null(venture_plugin_manager_lookup_exec(fixture->manager,
	                                                 "blocked"));
	g_clear_error(&error);

	/* Loaded while on, then switched off: the next run is refused
	 * without a restart. */
	allow_exec(fixture, TRUE);
	later = write_exec_plugin(fixture, "later", "echo", NULL, NULL);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, later,
	                                               NULL));
	allow_exec(fixture, FALSE);

	result = venture_plugin_manager_run_exec(fixture->manager, "later", NULL,
	                                         NULL, NULL, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "allow_exec"));
}

static void
test_exec_manifest_rules(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	struct
	{
		const gchar	*name;
		const gchar	*body;
		const gchar	*expect;
	} cases[] = {
		{ "noproto", "runtime: exec\nentry: probe.sh\n", "protocol: 1" },
		{ "proto2", "runtime: exec\nprotocol: 2\nentry: probe.sh\n",
		  "protocol: 1" },
		{ "typo", "runtime: exec\nprotocol: 1\nentry: probe.sh\n"
		          "exec:\n  timout: 5\n", "exec.timout is not a setting" },
		{ "slow", "runtime: exec\nprotocol: 1\nentry: probe.sh\n"
		          "exec:\n  timeout: 99999\n", "exec.timeout must be between" },
		{ "badenv", "runtime: exec\nprotocol: 1\nentry: probe.sh\n"
		            "exec:\n  env: [\"NOT-A-NAME\"]\n",
		  "environment variable" },
		{ "badargs", "runtime: exec\nprotocol: 1\nentry: probe.sh\n"
		             "exec:\n  args: [{nested: map}]\n", "must be text" },
	};
	gsize i;

	(void)user_data;

	allow_exec(fixture, TRUE);

	/*
	 * The protocol is declared, never assumed, and every exec setting is
	 * checked: a typo is refused rather than becoming a default nobody
	 * chose.
	 */
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *directory = NULL;
		g_autofree gchar *body = NULL;
		g_autofree gchar *manifest = NULL;
		g_autoptr(GError) error = NULL;

		directory = make_dir(fixture->plugin_dir, cases[i].name);
		g_free(install_probe(directory));
		body = g_strdup_printf("name: %s\n%s", cases[i].name, cases[i].body);
		manifest = write_file(directory, "m.plugin.yaml", body);

		g_assert_false(venture_plugin_manager_load_file(fixture->manager,
		                                                manifest, &error));
		g_assert_nonnull(error);

		if (NULL == strstr(error->message, cases[i].expect))
			g_error("%s: \"%s\" lacks \"%s\"", cases[i].name,
			        error->message, cases[i].expect);

		g_assert_null(venture_plugin_manager_lookup_exec(fixture->manager,
		                                                 cases[i].name));
	}
}

/* ==========================================================================
 * Broken versus required
 * ========================================================================== */

static void
test_broken_plugin_skipped_required_fatal(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	const gchar *paths[] = { NULL, NULL };
	const gchar *required[] = { "brokenexec", NULL };

	(void)user_data;

	/* An exec plugin while exec is off: broken, from this install's view. */
	g_free(write_exec_plugin(fixture, "brokenexec", "echo", NULL, NULL));
	g_free(write_file(fixture->plugin_dir, "fine.yaml",
	                  "name: fine\nfields:\n  x: string\n"));

	/* Skipped, with a warning, and the rest still loads. */
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING,
	                      "*Skipping plugin*allow_exec*");
	g_assert_cmpuint(venture_plugin_manager_load_directory(fixture->manager,
		fixture->plugin_dir, NULL), ==, 1);
	g_test_assert_expected_messages();

	/*
	 * Required, the same failure is fatal -- and says why, because "did
	 * not load" alone sends the operator looking for a file that is
	 * there.
	 */
	g_clear_object(&fixture->manager);
	fixture->manager = venture_plugin_manager_new(fixture->context);
	venture_context_set_plugin_manager(fixture->context, fixture->manager);

	paths[0] = fixture->plugin_dir;
	g_object_set(fixture->config, "plugins-paths", paths,
	             "plugins-required", required, NULL);

	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING,
	                      "*Skipping plugin*allow_exec*");
	g_assert_false(venture_plugin_manager_load_configured(fixture->manager,
	                                                      &error));
	g_test_assert_expected_messages();
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "brokenexec"));
	g_assert_nonnull(strstr(error->message, "allow_exec"));
}

static void
test_required_deferred_file_says_why(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	const gchar *paths[] = { NULL, NULL };
	const gchar *required[] = { "script.lua", NULL };

	(void)user_data;

	g_free(write_file(fixture->plugin_dir, "script.lua", "print(1)\n"));

	paths[0] = fixture->plugin_dir;
	g_object_set(fixture->config, "plugins-paths", paths,
	             "plugins-required", required, NULL);

	/* Present, held for a runtime nobody registered: required means it
	 * must load, and the refusal names the reason. */
	g_assert_false(venture_plugin_manager_load_configured(fixture->manager,
	                                                      &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "no runtime loads it"));
}

/* ==========================================================================
 * The JSON-lines protocol
 * ========================================================================== */

static void
test_jsonl_kind_names_match_enum(void)
{
	GEnumClass *klass;
	guint i;

	/* The parser's wire names and the GEnum's nicks are two tables; they
	 * must be one vocabulary. */
	klass = g_type_class_ref(VENTURE_TYPE_JSONL_MESSAGE_KIND);

	for (i = 0; i < klass->n_values; i++)
	{
		VentureJsonlMessageKind kind;

		g_assert_true(venture_jsonl_kind_from_name(
			klass->values[i].value_nick, &kind));
		g_assert_cmpint(kind, ==, klass->values[i].value);
	}

	/* Twelve, plus `result`, which an exec automation handler answers
	 * with. A thirteenth that the parser does not name fails above. */
	g_assert_cmpuint(klass->n_values, ==, 13);
	g_type_class_unref(klass);

	g_assert_false(venture_jsonl_kind_from_name("Listing", NULL));
	g_assert_false(venture_jsonl_kind_from_name("", NULL));
}

static void
test_jsonl_decimal_grammar(void)
{
	struct
	{
		const gchar	*text;
		gboolean	 negative_ok;
		gboolean	 valid;
	} cases[] = {
		{ "0", FALSE, TRUE },
		{ "12.50", FALSE, TRUE },
		{ "0.0001", FALSE, TRUE },
		{ "-3.25", TRUE, TRUE },
		{ "-3.25", FALSE, FALSE },
		{ "", FALSE, FALSE },
		{ ".5", FALSE, FALSE },
		{ "5.", FALSE, FALSE },
		{ "+5", FALSE, FALSE },
		{ "1e5", FALSE, FALSE },
		{ "1.2.3", FALSE, FALSE },
		{ "12 ", FALSE, FALSE },
		{ "NaN", FALSE, FALSE },
		{ "12345678901234567890123456789012345678901", FALSE, FALSE },
	};
	gsize i;

	/* Exact numbers only: no exponent, no bare point, no sign unless
	 * asked for, and bounded in length. */
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		if (cases[i].valid !=
		    venture_jsonl_is_decimal(cases[i].text, cases[i].negative_ok))
			g_error("\"%s\" should be %s", cases[i].text,
			        cases[i].valid ? "valid" : "invalid");
	}
}

static void
test_jsonl_accepts_every_kind(void)
{
	const gchar *const lines[] = {
		"{\"type\":\"venue\",\"key\":\"realm-1\",\"name\":\"Argent Dawn\","
		"\"kind\":\"auction_house\",\"group\":\"eu\",\"currency\":\"GOLD\","
		"\"attrs\":{\"locale\":\"en\"}}",
		"{\"type\":\"instrument\",\"key\":\"item:19019\",\"name\":\"Sword\","
		"\"kind\":\"item\",\"category\":\"Weapons/Swords\",\"parent\":null}",
		"{\"type\":\"snapshot\",\"venue\":\"realm-1\","
		"\"taken_at\":\"2026-10-03T12:00:00Z\",\"complete\":true}",
		"{\"type\":\"listing\",\"venue\":\"realm-1\",\"instrument\":\"i\","
		"\"price\":\"12.3456\",\"quantity\":5,\"id\":\"a-1\",\"side\":\"sell\"}",
		"{\"type\":\"stat\",\"venue\":\"v\",\"instrument\":\"i\","
		"\"min\":\"1.00\",\"quantity\":0}",
		"{\"type\":\"quote\",\"venue\":\"book\",\"instrument\":\"e/home\","
		"\"odds\":\"2.10\"}",
		"{\"type\":\"quote\",\"venue\":\"book\",\"instrument\":\"e/home\","
		"\"odds\":\"+150\",\"format\":\"american\",\"side\":\"back\"}",
		"{\"type\":\"quote\",\"venue\":\"book\",\"instrument\":\"e/home\","
		"\"odds\":\"5/2\",\"format\":\"fractional\"}",
		"{\"type\":\"quote\",\"venue\":\"shop\",\"instrument\":\"sku-1\","
		"\"price\":\"9.99\",\"currency\":\"USD\",\"side\":\"ask\"}",
		"{\"type\":\"entry\",\"key\":\"guid-1\",\"title\":\"News\","
		"\"url\":\"https://example.org/a\","
		"\"published_at\":\"2026-10-03T08:00:00+02:00\"}",
		"{\"type\":\"record\",\"record_type\":\"exchange_rate\","
		"\"fields\":{\"rate\":\"1.08\"},\"match\":[\"base\",\"quote\"]}",
		"{\"type\":\"cursor\",\"value\":\"page-2\"}",
		"{\"type\":\"not_modified\"}",
		"{\"type\":\"log\",\"level\":\"warning\",\"message\":\"slow\"}",
		"{\"type\":\"error\",\"message\":\"quota\",\"retry_after\":30,"
		"\"future_member\":\"ignored\"}",
		"{\"type\":\"result\"}",
		"{\"type\":\"result\",\"count\":3,\"summary\":\"three\","
		"\"detail\":\"a\\nb\"}",
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(lines); i++)
	{
		g_autoptr(VentureJsonlMessage) message = NULL;
		g_autoptr(GError) error = NULL;

		message = venture_jsonl_message_parse(lines[i], -1, (guint)(i + 1),
		                                      &error);

		if (NULL == message)
			g_error("line %" G_GSIZE_FORMAT " refused: %s", i + 1,
			        error->message);
	}
}

static void
test_jsonl_refusals(void)
{
	struct
	{
		const gchar	*line;
		const gchar	*expect;
	} cases[] = {
		{ "not json", "is not valid JSON" },
		{ "[1,2]", "must be a JSON object" },
		{ "{\"key\":\"x\"}", "type is required" },
		{ "{\"type\":\"Listing\"}", "not a protocol-1 message type" },
		{ "{\"type\":\"venue\"}", "venue.key is required" },
		{ "{\"type\":\"venue\",\"key\":\"\"}", "must not be empty" },
		{ "{\"type\":\"venue\",\"key\":\"v\",\"currency\":\"€\"}",
		  "not a currency code" },
		{ "{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"price\":12.5}", "never a JSON number" },
		{ "{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"price\":\"-1\"}", "non-negative decimal" },
		{ "{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"price\":\"1\",\"quantity\":2.5}", "whole number" },
		{ "{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"price\":\"1\",\"quantity\":0}", "at least 1" },
		{ "{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"price\":\"1\",\"side\":\"short\"}", "must be one of" },
		{ "{\"type\":\"snapshot\",\"venue\":\"v\","
		  "\"taken_at\":\"2026-10-03T12:00:00\"}", "with a zone" },
		{ "{\"type\":\"stat\",\"venue\":\"v\",\"instrument\":\"i\"}",
		  "carries no figure" },
		{ "{\"type\":\"quote\",\"venue\":\"v\",\"instrument\":\"i\"}",
		  "exactly one of price and odds" },
		{ "{\"type\":\"quote\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"price\":\"1\",\"odds\":\"2\"}", "exactly one of price and odds" },
		{ "{\"type\":\"quote\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"odds\":\"1.00\"}", "not a price in its format" },
		{ "{\"type\":\"quote\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"odds\":\"+50\",\"format\":\"american\"}",
		  "not a price in its format" },
		{ "{\"type\":\"quote\",\"venue\":\"v\",\"instrument\":\"i\","
		  "\"odds\":\"5/0\",\"format\":\"fractional\"}",
		  "not a price in its format" },
		{ "{\"type\":\"entry\",\"key\":\"k\",\"title\":\"t\","
		  "\"url\":\"javascript:alert(1)\"}", "http or https" },
		{ "{\"type\":\"record\",\"record_type\":\"../users\","
		  "\"fields\":{}}", "record type name" },
		{ "{\"type\":\"record\",\"record_type\":\"sale\"}",
		  "fields is required" },
		{ "{\"type\":\"record\",\"record_type\":\"sale\",\"fields\":{},"
		  "\"match\":[1]}", "list of field names" },
		{ "{\"type\":\"log\",\"message\":\"x\",\"level\":\"loud\"}",
		  "must be one of" },
		{ "{\"type\":\"error\",\"message\":\"x\",\"retry_after\":-1}",
		  "at least 0" },
		{ "{\"type\":\"result\",\"count\":-1}", "at least 0" },
		{ "{\"type\":\"result\",\"count\":2.5}", "whole number" },
		{ "{\"type\":\"result\",\"summary\":7}", "must be a string" },
	};
	gsize i;

	/* Each refused, with the member named and nothing of the line's text
	 * quoted back. */
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureJsonlMessage) message = NULL;
		g_autoptr(GError) error = NULL;

		message = venture_jsonl_message_parse(cases[i].line, -1, 7, &error);

		if (NULL != message)
			g_error("case %" G_GSIZE_FORMAT " was accepted: %s", i,
			        cases[i].line);

		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION);
		g_assert_true(g_str_has_prefix(error->message, "Line 7: "));

		if (NULL == strstr(error->message, cases[i].expect))
			g_error("case %" G_GSIZE_FORMAT ": \"%s\" lacks \"%s\"", i,
			        error->message, cases[i].expect);
	}

	/* An embedded NUL would let half a line be judged. */
	{
		g_autoptr(VentureJsonlMessage) message = NULL;
		g_autoptr(GError) error = NULL;
		static const gchar with_nul[] = "{\"type\":\"cursor\",\"value\":\"a\"}\0x";

		message = venture_jsonl_message_parse(with_nul,
		                                      (gssize)(sizeof(with_nul) - 1),
		                                      1, &error);
		g_assert_null(message);
		g_assert_nonnull(strstr(error->message, "NUL"));
	}
}

static void
test_jsonl_reader_chunks(void)
{
	g_autoptr(VentureJsonlReader) reader = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *input;
	gsize length;
	gsize i;

	/*
	 * A line split across reads, CRLF endings, blank lines and a last line
	 * with no newline: fed one byte at a time, every boundary a pipe can
	 * produce is exercised.
	 */
	input = "{\"type\":\"cursor\",\"value\":\"a\"}\r\n"
	        "\n"
	        "   \n"
	        "{\"type\":\"cursor\",\"value\":\"b\"}\n"
	        "{\"type\":\"not_modified\"}";
	length = strlen(input);

	reader = venture_jsonl_reader_new(1024);
	messages = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_jsonl_message_unref);

	for (i = 0; i < length; i++)
	{
		g_assert_true(venture_jsonl_reader_feed(reader,
			(const guint8 *)input + i, 1, messages, &error));
		g_assert_no_error(error);
	}

	g_assert_cmpuint(messages->len, ==, 2);
	g_assert_true(venture_jsonl_reader_finish(reader, messages, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(messages->len, ==, 3);

	g_assert_cmpstr(venture_jsonl_message_get_string(
		g_ptr_array_index(messages, 1), "value"), ==, "b");
	g_assert_cmpuint(venture_jsonl_message_get_line(
		g_ptr_array_index(messages, 1)), ==, 4);
	g_assert_cmpuint(venture_jsonl_reader_get_line(reader), ==, 5);
}

static void
test_jsonl_reader_line_limit(void)
{
	g_autoptr(VentureJsonlReader) reader = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *long_line = NULL;

	reader = venture_jsonl_reader_new(64);
	messages = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_jsonl_message_unref);

	/* Refused as soon as it is too long, before its newline arrives: a
	 * producer that never ends a line must not grow the buffer forever. */
	long_line = g_strnfill(100, 'x');
	g_assert_false(venture_jsonl_reader_feed(reader, (const guint8 *)long_line,
	                                         100, messages, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION);
	g_assert_nonnull(strstr(error->message, "longer than the limit"));
	g_clear_error(&error);

	/* And it stays refused. */
	g_assert_false(venture_jsonl_reader_feed(reader, (const guint8 *)"\n", 1,
	                                         messages, &error));
}

int
main(
	int	  argc,
	char	**argv
){
	const gchar *plugin_path;

	/*
	 * Where the example plugin is, for the native-manifest test, handed to
	 * its subprocess through the environment. The subprocess loads a copy
	 * and nothing else -- see test_manifest_native.
	 */
	plugin_path = g_getenv("VENTURE_PLUGIN_PATH");

	if ((NULL == g_getenv("VENTURE_TEST_EXAMPLE_PLUGIN")) &&
	    (NULL != plugin_path))
	{
		g_autofree gchar *example = NULL;

		example = g_build_filename(plugin_path, "example.so", NULL);
		g_setenv("VENTURE_TEST_EXAMPLE_PLUGIN", example, TRUE);
	}

	/* And out of load_configured's way: the required-plugin tests expect
	 * exactly the messages their own directory produces, and the example
	 * plugin announcing itself in the middle is not one of them. */
	g_unsetenv("VENTURE_PLUGIN_PATH");

	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/plugin-runtime/builtins-registered",
	    test_runtime_builtins_registered);
	ADD("/plugin-runtime/bare-files-unchanged",
	    test_runtime_bare_files_unchanged);
	ADD("/plugin-runtime/deferred-file-loads-when-runtime-arrives",
	    test_runtime_deferred_file_loads_when_runtime_arrives);
	ADD("/plugin-runtime/explicit-unknown-file-refused-and-held",
	    test_runtime_explicit_unknown_file_is_refused_and_held);
	ADD("/plugin-runtime/runtime-added-mid-scan-loads-in-order",
	    test_runtime_added_mid_scan_loads_in_order);
	ADD("/plugin-runtime/conflicts-refused", test_runtime_conflicts_refused);

	ADD("/plugin-runtime/manifest/declarative-in-subdirectory",
	    test_manifest_declarative_in_subdirectory);
	ADD("/plugin-runtime/manifest/entry-must-stay-inside",
	    test_manifest_entry_must_stay_inside);
	ADD("/plugin-runtime/manifest/rejects-bad-shapes",
	    test_manifest_rejects_bad_shapes);
	ADD("/plugin-runtime/manifest/unknown-runtime-is-held",
	    test_manifest_unknown_runtime_is_held);
	ADD("/plugin-runtime/manifest/name-is-unique",
	    test_manifest_name_is_unique);
	ADD("/plugin-runtime/manifest/native", test_manifest_native);
	ADD("/plugin-runtime/manifest/crispy", test_manifest_crispy);

	ADD("/plugin-runtime/provides/dispatched-to-registered-kind",
	    test_provides_dispatched_to_registered_kind);
	ADD("/plugin-runtime/provides/all-or-nothing", test_provides_all_or_nothing);
	ADD("/plugin-runtime/provides/attribution", test_provides_attribution);
	ADD("/plugin-runtime/failed-register-takes-back", test_failed_register_takes_back);

	ADD("/plugin-runtime/exec/round-trip", test_exec_round_trip);
	ADD("/plugin-runtime/exec/deadline-kills-the-group",
	    test_exec_deadline_kills_the_group);
	ADD("/plugin-runtime/exec/output-cap", test_exec_output_cap);
	ADD("/plugin-runtime/exec/stderr-capped-not-fatal",
	    test_exec_stderr_is_capped_not_fatal);
	ADD("/plugin-runtime/exec/stderr-cap-cuts-no-secret",
	    test_exec_stderr_cap_cuts_no_secret);
	ADD("/plugin-runtime/exec/nonzero-exit", test_exec_nonzero_exit);
	ADD("/plugin-runtime/exec/error-message-keeps-partial-batch",
	    test_exec_error_message_keeps_partial_batch);
	ADD("/plugin-runtime/exec/protocol-violation",
	    test_exec_protocol_violation);
	ADD("/plugin-runtime/exec/program-that-never-reads",
	    test_exec_program_that_never_reads);
	ADD("/plugin-runtime/exec/cancellation", test_exec_cancellation);
	ADD("/plugin-runtime/exec/executable-must-stay-inside",
	    test_exec_executable_must_stay_inside);
	ADD("/plugin-runtime/exec/rechecked-at-every-run",
	    test_exec_rechecked_at_every_run);
	ADD("/plugin-runtime/exec/allow-exec-off", test_exec_allow_exec_off);
	ADD("/plugin-runtime/exec/manifest-rules", test_exec_manifest_rules);

	ADD("/plugin-runtime/broken-skipped-required-fatal",
	    test_broken_plugin_skipped_required_fatal);
	ADD("/plugin-runtime/required-deferred-file-says-why",
	    test_required_deferred_file_says_why);

	g_test_add_func("/plugin-runtime/jsonl/kind-names-match-enum",
	                test_jsonl_kind_names_match_enum);
	g_test_add_func("/plugin-runtime/jsonl/decimal-grammar",
	                test_jsonl_decimal_grammar);
	g_test_add_func("/plugin-runtime/jsonl/accepts-every-kind",
	                test_jsonl_accepts_every_kind);
	g_test_add_func("/plugin-runtime/jsonl/refusals", test_jsonl_refusals);
	g_test_add_func("/plugin-runtime/jsonl/reader-chunks",
	                test_jsonl_reader_chunks);
	g_test_add_func("/plugin-runtime/jsonl/reader-line-limit",
	                test_jsonl_reader_line_limit);

	return g_test_run();
}
