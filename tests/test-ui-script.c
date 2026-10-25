/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <glib.h>

static void
test_script(gconstpointer script)
{
	const gchar *argv[] = { "node", (const gchar *)script, NULL };
	g_autofree gchar *output = NULL;
	g_autofree gchar *failure = NULL;
	g_autoptr(GError) error = NULL;
	gint status;

	/* A dropped response is not proof that a bill was not saved. */
	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, &output, &failure, &status, &error));
	g_assert_no_error(error);
	if (status)
		g_test_message("Script failure: %s", failure);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_no_error(error);
	g_test_message("%s", output);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/ui-script/inline-forms", "tests/ui-inline-forms.cjs", test_script);
	g_test_add_data_func("/ui-script/filter-race", "tests/ui-filter-race.cjs", test_script);
	g_test_add_data_func("/ui-script/filter-focus", "tests/ui-filter-focus.cjs", test_script);
	g_test_add_data_func("/ui-script/navigation", "tests/ui-navigation.cjs", test_script);
	return g_test_run();
}
