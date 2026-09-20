/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <gio/gio.h>

/* Database ownership safety must remain in the ordinary suite: an opt-in
 * live upgrade is not a substitute for these failure-path regressions. */
static void
test_fixture_ownership(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) child = NULL;

	child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
		"python3", "tests/test-upgrade-drill.py", NULL);
	g_assert_no_error(error);
	g_assert_nonnull(child);
	g_assert_true(g_subprocess_wait_check(child, NULL, &error));
	g_assert_no_error(error);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/upgrade-drill/fixture-ownership", test_fixture_ownership);
	return g_test_run();
}
