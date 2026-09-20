/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gio/gio.h>
#include "venture-test-util.h"

/* The shell fixture stubs only Podman. Encryption, archives, locks and the
 * published filesystem result use the real maintenance executable. */
static void test_tool_contract(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-tenant-recovery-XXXXXX", &error);
	g_autofree gchar *output = NULL;
	g_assert_no_error(error);
	process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE,
		&error, "bash", "tests/test-tenant-recovery.sh", directory, NULL);
	g_assert_no_error(error); g_assert_nonnull(process);
	g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &output, NULL, &error));
	g_assert_no_error(error);
	if (!g_subprocess_get_successful(process)) g_test_message("Recovery fixture: %s", output);
	g_assert_true(g_subprocess_get_successful(process));
	venture_test_remove_tree(directory);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tenant-recovery/tool-contract", test_tool_contract);
	return g_test_run();
}
