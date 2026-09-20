/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <glib/gstdio.h>
#include <string.h>
#include "venture-test-util.h"

static gchar *server_path;

/* Run the actual entry point: library lease tests cannot prove that every
 * early-return administration command acquires its lease before writing. */
static gboolean
run_server(const gchar *config, const gchar *key, const gchar *const *options,
	const gchar *expected)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL, *diagnostic = NULL;
	guint i;
	g_ptr_array_add(argv, (gpointer)"timeout");
	g_ptr_array_add(argv, (gpointer)"30");
	g_ptr_array_add(argv, server_path);
	g_ptr_array_add(argv, (gpointer)"--config");
	g_ptr_array_add(argv, (gpointer)config);
	for (i = 0; options[i]; i++) g_ptr_array_add(argv, (gpointer)options[i]);
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_INTEGRATION_KEY", key, TRUE);
	child = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error); g_assert_nonnull(child);
	g_assert_true(g_subprocess_communicate_utf8(child, NULL, NULL, &output, &diagnostic, &error));
	g_assert_no_error(error); g_assert_true(g_subprocess_get_if_exited(child));
	g_assert_cmpint(g_subprocess_get_exit_status(child), !=, 124);
	g_assert_null(strstr(output, key)); g_assert_null(strstr(diagnostic, key));
	if (expected) g_assert_true(strstr(output, expected) != NULL || strstr(diagnostic, expected) != NULL);
	return g_subprocess_get_successful(child);
}

static void
test_hosted_key_maintenance(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = g_dir_make_tmp("venture-hosted-maintenance-XXXXXX", &error);
	g_autofree gchar *uri = NULL, *config_path = NULL, *key_path = NULL, *yaml = NULL;
	g_autofree gchar *old_key = g_base64_encode((const guchar *)"01234567890123456789012345678901", 32);
	g_autofree gchar *new_key = g_base64_encode((const guchar *)"abcdefghijklmnopqrstuvwxyzABCDEF", 32);
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureProcessLease) lease = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) settings = json_node_new(JSON_NODE_OBJECT);
	VentureTenantService *tenant;
	const gchar *check[] = { "--check-integration-key", "--tenant-reason", "Verify retained fixture", NULL };
	const gchar *no_reason[] = { "--check-integration-key", NULL };
	const gchar *mixed[] = { "--check-integration-key", "--tenant-status", NULL };
	const gchar *suspend[] = { "--tenant-state", "suspended", "--tenant-reason", "Suspend fixture", NULL };
	const gchar *rotate[5];

	g_assert_no_error(error);
	uri = g_strdup_printf("sqlite://%s/workspace.db", root);
	config_path = g_build_filename(root, "workspace.yaml", NULL);
	key_path = g_build_filename(root, "new-key", NULL);
	g_object_set(config, "database-uri", uri, "state-dir", root,
		"hosted-enabled", TRUE, "hosted-workspace-id", "0381d47a-cbf7-43a5-89e8-f540cae344da",
		"hosted-origin", "https://maintenance.example.test", NULL);
	yaml = venture_config_to_yaml(config, TRUE);
	g_assert_true(g_file_set_contents(config_path, yaml, -1, &error));
	g_assert_true(g_file_set_contents(key_path, new_key, -1, &error));
	g_assert_cmpint(g_chmod(config_path, 0600), ==, 0);
	g_assert_cmpint(g_chmod(key_path, 0600), ==, 0);
	g_assert_no_error(error);
	database = venture_database_new(uri, &error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(database), key, &error));
	json_node_take_object(settings, json_object_new());
	json_object_set_string_member(json_node_get_object(settings), "token", "retained-private-fixture");
	binding = venture_integration_service_configure(venture_integration_service_get(database),
		1, "maintenance-fixture", "account", "test", settings, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(binding);
	tenant = venture_tenant_service_get(database);
	g_assert_true(venture_tenant_service_configure(tenant, config, &error));
	g_assert_true(venture_tenant_service_initialize(tenant, &error));
	g_assert_no_error(error);
	lease = venture_process_lease_acquire(database, root, &error);
	g_assert_no_error(error); g_assert_nonnull(lease);
	g_assert_false(run_server(config_path, old_key, suspend, "Workspace:"));
	g_assert_false(run_server(config_path, old_key, check, "Workspace:"));
	g_clear_object(&lease);
	g_assert_false(run_server(config_path, old_key, mixed, "separate offline maintenance"));
	g_assert_false(run_server(config_path, old_key, no_reason, "Key maintenance:"));
	g_assert_true(run_server(config_path, old_key, suspend, "suspended"));
	g_assert_true(run_server(config_path, old_key, check, "All retained integration credentials authenticated"));
	rotate[0] = "--rotate-integration-key"; rotate[1] = key_path;
	rotate[2] = "--tenant-reason"; rotate[3] = "Rotate suspended fixture"; rotate[4] = NULL;
	g_assert_true(run_server(config_path, old_key, rotate, "Integration credentials re-encrypted"));
	g_assert_false(run_server(config_path, old_key, check, "Key rotation:"));
	g_assert_true(run_server(config_path, new_key, check, "All retained integration credentials authenticated"));
	/* Rotation must not resume the workspace or silently change its authority. */
	g_assert_false(venture_tenant_service_check_operation(tenant, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_clear_object(&binding); g_clear_object(&database);
	venture_test_remove_tree(root);
}

int
main(int argc, char **argv)
{
	g_autofree gchar *directory = g_path_get_dirname(argv[0]);
	g_autofree gchar *path = g_build_filename(directory, "..", "venture", NULL);
	gint result;
	server_path = g_canonicalize_filename(path, NULL);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/hosted-maintenance/key-and-lease", test_hosted_key_maintenance);
	result = g_test_run();
	g_free(server_path);
	return result;
}
