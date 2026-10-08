/*
 * test-table-sort.c - Tables sort in the browser, and what the server says
 *                     about the ones that must not
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>

/* Run the real sorter from venture.js: a grep for its selector would pass
 * with money read as text, a group's heading sorted to the top and an
 * invoice editor's lines reordered before they post. */
static void
test_script(void)
{
	const gchar *argv[] = { "node", "tests/table-sort.cjs", NULL };
	g_autofree gchar *output = NULL, *failure = NULL;
	g_autoptr(GError) error = NULL;
	gint status;

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
		NULL, NULL, &output, &failure, &status, &error));
	g_assert_no_error(error);
	if (status != 0)
		g_test_message("Script failure: %s", failure);
	g_assert_true(g_spawn_check_wait_status(status, &error));
	g_assert_no_error(error);
	g_test_message("%s", output);
}

static VentureReportResult *
materials(void)
{
	VentureReportResult *result = venture_report_result_new("Materials", NULL);
	const gchar *const names[] = { "Cloth", "Thread", "Dye" };
	guint i;

	venture_report_result_add_column(result, "product", "Product", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "units", "Units", VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "product", names[i]);
		venture_report_result_set_number(result, "units", (gdouble)(i + 1));
	}

	venture_report_result_begin_row(result);
	venture_report_result_mark_summary(result);
	venture_report_result_set_text(result, "product", "Total");
	venture_report_result_set_number(result, "units", 6.0);

	return result;
}

/* A total in a sortable report is drawn in the table's foot, where a sort
 * does not reach it; left in the body it was sorted in among the rows it
 * adds up. "and N more" is not a row either. */
static void
test_report_total_in_foot(void)
{
	g_autoptr(VentureReportResult) result = materials();
	g_autofree gchar *html = NULL;
	g_autofree gchar *cut = NULL;
	const gchar *body;
	const gchar *foot;

	html = venture_report_result_render_html_body(result, FALSE, TRUE, 0);
	g_assert_null(strstr(html, "data-no-sort"));
	body = strstr(html, "<tbody>");
	foot = strstr(html, "<tfoot><tr class=\"total\">");
	g_assert_nonnull(body);
	g_assert_nonnull(foot);
	g_assert_true(foot > strstr(html, "</tbody>"));
	g_assert_nonnull(strstr(foot, "Total"));
	g_assert_null(g_strstr_len(body, foot - body, "Total"));

	cut = venture_report_result_render_html_body(result, FALSE, TRUE, 2);
	g_assert_nonnull(strstr(cut, "</tbody><tfoot><tr><td colspan=\"2\" class=\"muted\">and 1 more</td></tr>"
	                             "<tr class=\"total\">"));
}

/* An ordered result -- a statement -- keeps its totals where they fall and
 * tells the page not to sort it. */
static void
test_report_ordered(void)
{
	g_autoptr(VentureReportResult) result = materials();
	g_autofree gchar *html = NULL;

	venture_report_result_set_ordered(result, TRUE);
	html = venture_report_result_render_html_body(result, FALSE, TRUE, 0);
	g_assert_nonnull(strstr(html, "<table class=\"data\" data-no-sort>"));
	g_assert_null(strstr(html, "<tfoot>"));
	g_assert_nonnull(strstr(html, "<tr class=\"total\"><td class=\"\">Total</td>"));
	g_assert_true(strstr(html, "Dye") < strstr(html, "Total"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/table-sort/script", test_script);
	g_test_add_func("/table-sort/report-total-in-foot", test_report_total_in_foot);
	g_test_add_func("/table-sort/report-ordered", test_report_ordered);
	return g_test_run();
}
