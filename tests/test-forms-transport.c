/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <glib.h>

/* Run the real browser script: testing only for a timeout string would miss
 * a deadline cleared at headers or a submit button left permanently disabled. */
static void
test_transport(void)
{
	const gchar *argv[] = { "node", "tests/forms-transport.cjs", NULL };
	g_autofree gchar *output = NULL, *failure = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, &output, &failure, &status, &error));
	g_assert_no_error(error);
	if (status != 0) g_test_message("Script failure: %s", failure);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_no_error(error);
	g_test_message("%s", output);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/forms-transport/deadlines", test_transport);
	return g_test_run();
}
