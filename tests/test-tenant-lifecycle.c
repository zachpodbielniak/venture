/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gio/gio.h>
#include "venture-test-util.h"

static void
test_operator_boundary(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = g_dir_make_tmp("venture-tenant-lifecycle-XXXXXX", &error);
	g_autoptr(GSubprocess) child = NULL;
	g_assert_no_error(error);
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
		"python3", "tests/test-tenant-lifecycle.py", root, NULL);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_wait_check(child, NULL, &error));
	g_assert_no_error(error);
	venture_test_remove_tree(root);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tenant-lifecycle/operator-boundary", test_operator_boundary);
	return g_test_run();
}
