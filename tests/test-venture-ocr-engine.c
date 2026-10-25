/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* The public deadline is the page budget. Zero and values above 30 seconds
 * stay configuration errors, including before any helper is spawned. */
static void test_page_deadline_bounds(void)
{
	g_autoptr(GError) error = NULL;
	g_assert_null(venture_ocr_local_new("/bin/sh", 0, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "deadline"));
	g_clear_error(&error);
	g_assert_null(venture_ocr_local_new("/bin/sh", 30001, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "deadline"));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ocr-engine/page-deadline-bounds", test_page_deadline_bounds);
	return g_test_run();
}
