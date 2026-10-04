/*
 * test-web-chart.c - The server-rendered SVG charts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The charts draw names that came from outside feeds, so every test here
 * starts from a hostile one. They also pin the drawing rules both
 * stylesheets rely on: currentColor and classes, never a literal colour;
 * a title, a description and a table of every figure for whoever cannot
 * see the picture.
 */

#include <venture.h>

#include <math.h>
#include <string.h>

static const gchar hostile[] = "<script>alert(\"x\")</script> & 'quoted'";

/* How many times @needle occurs in @haystack. */
static guint
occurrences(
	const gchar	*haystack,
	const gchar	*needle
){
	const gchar *at;
	guint count = 0;

	for (at = strstr(haystack, needle); NULL != at; at = strstr(at + strlen(needle), needle))
		count++;

	return count;
}

/* Nothing reached the markup unescaped, and no colour was written into it. */
static void
assert_clean(const gchar *svg)
{
	g_assert_null(strstr(svg, "<script>"));
	g_assert_null(strstr(svg, "\"x\""));
	g_assert_null(strstr(svg, "'quoted'"));
	g_assert_nonnull(strstr(svg, "&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt; &amp; &#39;quoted&#39;"));

	/* Colour comes from the stylesheet's tokens through currentColor. */
	g_assert_null(strstr(svg, "fill=\"#"));
	g_assert_null(strstr(svg, "stroke=\"#"));
	g_assert_null(strstr(svg, "rgb("));
	g_assert_null(strstr(svg, "style="));
}

/*
 * A price and a quantity on one chart, the second on its own axis, with
 * a gap: the hostile name is escaped everywhere it appears (aria-label,
 * <title>, legend, table), the gap breaks the line into two runs and a
 * point alone between gaps is a dot. What breaks if this regresses: a
 * feed that names an item "<script>" runs it on the instrument page; a
 * missing hour is drawn as a plunge to zero; a screen reader gets
 * "image" and no numbers.
 */
static void
test_line(void)
{
	static const gdouble prices[] = { 100.0, 120.0, NAN, 90.0, NAN, 110.0, 105.0 };
	static const gdouble quantities[] = { 5.0, 4.0, 3.0, 9.0, 8.0, 7.0, 6.0 };
	static const gchar *const labels[] = {
		"00:00", "01:00", "02:00", hostile, "04:00", "05:00", "06:00", NULL
	};
	VentureWebChartSeries series[2];
	VentureWebChart chart;
	g_autofree gchar *svg = NULL;
	const gchar *path;
	const gchar *end;
	g_autofree gchar *d = NULL;

	memset(series, 0, sizeof(series));
	series[0].name = hostile;
	series[0].values = prices;
	series[0].format = venture_web_chart_format_minor;
	series[0].format_data = (gpointer)"USD";
	series[1].name = "Quantity";
	series[1].values = quantities;
	series[1].secondary = TRUE;
	memset(&chart, 0, sizeof(chart));
	chart.title = hostile;
	chart.summary = hostile;
	chart.labels = labels;
	chart.n_points = G_N_ELEMENTS(prices);
	chart.series = series;
	chart.n_series = 2;

	svg = venture_web_chart_line(&chart);
	assert_clean(svg);

	g_assert_true(g_str_has_prefix(svg, "<figure class=\"chart chart-line\">"));
	g_assert_nonnull(strstr(svg, "role=\"img\" aria-label=\"&lt;script&gt;"));
	g_assert_nonnull(strstr(svg, "<title>&lt;script&gt;"));
	g_assert_nonnull(strstr(svg, "<desc>&lt;script&gt;"));
	g_assert_nonnull(strstr(svg, "class=\"chart-line chart-line-2\""));
	g_assert_nonnull(strstr(svg, "Quantity (right)"));

	/* The first series: M at 0, L at 1, then a new run at 5..6; the
	 * lone point at 3 is a dot. */
	path = strstr(svg, "class=\"chart-line chart-line-1\"");
	g_assert_nonnull(path);
	path = strstr(path, " d=\"");
	end = strchr(path + 4, '"');
	d = g_strndup(path + 4, end - (path + 4));
	g_assert_cmpuint(occurrences(d, "M"), ==, 3);
	g_assert_cmpuint(occurrences(d, "L"), ==, 2);
	g_assert_cmpuint(occurrences(svg, "<circle class=\"chart-dot chart-line-1\""), ==, 1);

	/* The table: one row per point, a gap as a dash, money as money. */
	g_assert_nonnull(strstr(svg, "<table class=\"visually-hidden chart-data\">"));
	g_assert_cmpuint(occurrences(svg, "<th scope=\"row\">"), ==, G_N_ELEMENTS(prices));
	g_assert_nonnull(strstr(svg, "<td>$1.00</td>"));
	g_assert_nonnull(strstr(svg, "<td>\xe2\x80\x94</td>"));

	/* Coordinates are C-locale numbers, never "12,5". */
	g_assert_null(strstr(d, ","));
}

/*
 * Nothing to draw, or too much, is an empty state saying so, not an empty
 * picture or a megabyte of path.
 */
static void
test_empty_and_bounds(void)
{
	static const gdouble gaps[] = { NAN, NAN };
	g_autofree gdouble *many = NULL;
	VentureWebChartSeries series;
	VentureWebChart chart;
	g_autofree gchar *svg = NULL;
	g_autofree gchar *huge = NULL;
	g_autofree gchar *spark = NULL;
	gsize i;

	memset(&series, 0, sizeof(series));
	series.name = "Price";
	series.values = gaps;
	memset(&chart, 0, sizeof(chart));
	chart.title = hostile;
	chart.n_points = 2;
	chart.series = &series;
	chart.n_series = 1;

	svg = venture_web_chart_line(&chart);
	g_assert_null(strstr(svg, "<svg"));
	g_assert_nonnull(strstr(svg, "nothing to draw"));
	g_assert_null(strstr(svg, "<script>"));

	many = g_new(gdouble, VENTURE_WEB_CHART_MAX_POINTS + 1);

	for (i = 0; i <= VENTURE_WEB_CHART_MAX_POINTS; i++)
		many[i] = (gdouble)i;

	series.values = many;
	chart.n_points = VENTURE_WEB_CHART_MAX_POINTS + 1;
	huge = venture_web_chart_bar(&chart);
	g_assert_null(strstr(huge, "<svg"));

	spark = venture_web_chart_sparkline(hostile, gaps, 2);
	g_assert_true(g_str_has_prefix(spark, "<span"));
	g_assert_nonnull(strstr(spark, "aria-label=\"&lt;script&gt;"));
	g_assert_null(strstr(spark, "<script>"));
}

/*
 * Bars grow from zero, each with a tooltip; the table has a row a bar.
 */
static void
test_bar(void)
{
	static const gdouble values[] = { 3.0, -2.0, 0.0, NAN };
	static const gchar *const labels[] = { "a", hostile, "c", "d", NULL };
	VentureWebChartSeries series;
	VentureWebChart chart;
	g_autofree gchar *svg = NULL;

	memset(&series, 0, sizeof(series));
	series.name = "Sold";
	series.values = values;
	memset(&chart, 0, sizeof(chart));
	chart.title = "Sold a day";
	chart.labels = labels;
	chart.n_points = G_N_ELEMENTS(values);
	chart.series = &series;
	chart.n_series = 1;

	svg = venture_web_chart_bar(&chart);
	g_assert_true(g_str_has_prefix(svg, "<figure class=\"chart chart-bar\">"));
	g_assert_cmpuint(occurrences(svg, "<rect class=\"chart-bar\""), ==, 3);
	g_assert_nonnull(strstr(svg, "<title>&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt; &amp; &#39;quoted&#39;: -2</title>"));
	g_assert_cmpuint(occurrences(svg, "<th scope=\"row\">"), ==, G_N_ELEMENTS(values));
	g_assert_null(strstr(svg, "<script>"));
}

/*
 * A weekday by hour map is seven rows of twenty-four cells. Cheapest is
 * strongest on a price map; an empty cell is outlined, not filled. What
 * breaks if this regresses: a heat map that shades dear hours darkest
 * sends a buyer to the worst hour, and an empty hour reads as the
 * cheapest.
 */
static void
test_heat(void)
{
	static const gchar *const rows[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", hostile, NULL };
	gchar *hours[25];
	gdouble values[7 * 24];
	VentureWebChartHeat heat;
	g_autofree gchar *svg = NULL;
	const gchar *cheapest;
	guint i;

	for (i = 0; i < 24; i++)
		hours[i] = g_strdup_printf("%02u", i);

	hours[24] = NULL;

	for (i = 0; i < 7 * 24; i++)
		values[i] = 100.0 + (gdouble)i;

	values[0] = 10.0;
	values[5] = NAN;

	memset(&heat, 0, sizeof(heat));
	heat.title = hostile;
	heat.row_labels = rows;
	heat.n_rows = 7;
	heat.column_labels = (const gchar *const *)hours;
	heat.n_columns = 24;
	heat.values = values;
	heat.format = venture_web_chart_format_minor;
	heat.format_data = (gpointer)"USD";
	heat.low_is_strong = TRUE;

	svg = venture_web_chart_heat(&heat);
	assert_clean(svg);
	g_assert_cmpuint(occurrences(svg, "<rect class=\"chart-heat-cell"), ==, 168);
	g_assert_cmpuint(occurrences(svg, "chart-heat-empty"), ==, 1);

	/* The cheapest cell is the strongest, the dearest the faintest. */
	cheapest = strstr(svg, "<title>Mon 00: $0.10</title>");
	g_assert_nonnull(cheapest);
	g_assert_nonnull(g_strrstr_len(svg, cheapest - svg, "fill-opacity=\"0.92\""));
	g_assert_nonnull(strstr(svg, "fill-opacity=\"0.12\""));

	/* It says what it is and which way the shading runs, in words. */
	g_assert_nonnull(strstr(svg, "<figcaption class=\"chart-caption\">&lt;script&gt;"));
	g_assert_nonnull(strstr(svg, "The stronger the cell, the lower the figure."));

	/* The table: a row per weekday, 24 cells each. */
	g_assert_cmpuint(occurrences(svg, "<th scope=\"row\">"), ==, 7);

	for (i = 0; i < 24; i++)
		g_free(hours[i]);
}

/* A sparkline is named by its label and needs no table. */
static void
test_sparkline(void)
{
	static const gdouble values[] = { 5.0, NAN, 7.0, 6.0 };
	g_autofree gchar *svg = NULL;

	svg = venture_web_chart_sparkline(hostile, values, G_N_ELEMENTS(values));
	g_assert_true(g_str_has_prefix(svg, "<svg class=\"sparkline\""));
	g_assert_nonnull(strstr(svg, "role=\"img\" aria-label=\"&lt;script&gt;"));
	g_assert_null(strstr(svg, "<script>"));
	g_assert_null(strstr(svg, "<table"));
	/* The lone first point still has a length. */
	g_assert_nonnull(strstr(svg, "l1.0 0"));
}

/* Money reads as money in its own currency; percents to one place. */
static void
test_formats(void)
{
	g_autofree gchar *usd = NULL;
	g_autofree gchar *pct = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *fraction = NULL;
	g_autofree gchar *bad = NULL;

	usd = venture_web_chart_format_minor(12345.0, (gpointer)"USD");
	g_assert_cmpstr(usd, ==, "$123.45");
	pct = venture_web_chart_format_percent(80.0, NULL);
	g_assert_cmpstr(pct, ==, "80.0%");
	number = venture_web_chart_format_number(42.0, NULL);
	g_assert_cmpstr(number, ==, "42");
	fraction = venture_web_chart_format_number(2.5, NULL);
	g_assert_cmpstr(fraction, ==, "2.50");

	/* No currency is a number, not a guessed one. */
	bad = venture_web_chart_format_minor(5.0, NULL);
	g_assert_cmpstr(bad, ==, "5");
}

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-chart/line", test_line);
	g_test_add_func("/web-chart/empty-and-bounds", test_empty_and_bounds);
	g_test_add_func("/web-chart/bar", test_bar);
	g_test_add_func("/web-chart/heat", test_heat);
	g_test_add_func("/web-chart/sparkline", test_sparkline);
	g_test_add_func("/web-chart/formats", test_formats);

	return g_test_run();
}
