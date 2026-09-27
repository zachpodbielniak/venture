/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <glib.h>

static void
test_inline_forms(void)
{
	const gchar *argv[] = { "node", "tests/ui-inline-forms.cjs", NULL };
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
	g_test_add_func("/ui-script/inline-forms", test_inline_forms);
	return g_test_run();
}
