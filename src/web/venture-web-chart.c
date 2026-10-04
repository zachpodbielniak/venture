/*
 * venture-web-chart.c - Server-rendered SVG charts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The drawing rules, so a chart reads as part of the panel in both looks:
 *
 * - Colour comes from the stylesheet. A mark is stroked or filled with
 *   currentColor and carries a class (chart-line-1, chart-line-2,
 *   chart-bar, chart-heat-cell, chart-grid, chart-axis) the two stylesheets
 *   colour from their tokens. A heat cell's strength is its fill-opacity,
 *   which is not a colour. Nothing here is red: red means "look here", and
 *   a chart is something to read, not an alarm.
 * - Coordinates are written with g_ascii_formatd(), never printf's %f: a
 *   locale with a decimal comma would turn "12.5" into "12,5", which SVG
 *   reads as two numbers.
 * - Every label reaches the markup through venture_html_escape_append().
 *   Instrument and venue names come from outside feeds.
 * - The figures are in a visually hidden table beside the picture, so a
 *   screen reader and a copy-paste get the numbers, not a description of
 *   a line.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

#define CHART_DEFAULT_WIDTH (640)
#define CHART_DEFAULT_HEIGHT (220)
#define CHART_MARGIN_TOP (12.0)
#define CHART_MARGIN_BOTTOM (28.0)
#define CHART_MARGIN_SIDE (64.0)
#define CHART_MARGIN_BARE (12.0)
#define CHART_TICKS (4)
#define CHART_X_LABELS (6)
#define CHART_HEAT_CELL (22.0)
#define CHART_HEAT_LABEL_WIDTH (40.0)
#define CHART_HEAT_LABEL_HEIGHT (18.0)
#define CHART_SPARK_WIDTH (100.0)
#define CHART_SPARK_HEIGHT (24.0)

/* --- Small pieces -------------------------------------------------------- */

/* Appends a coordinate, one decimal, in the C locale. */
static void
chart_num(
	GString	*out,
	gdouble	 value
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	if (!isfinite(value))
		value = 0.0;

	g_string_append(out, g_ascii_formatd(buffer, sizeof(buffer), "%.1f", value));
}

/* A value as text, through the series' formatter or as a plain number. */
static gchar *
chart_format(
	VentureWebChartFormat	 format,
	gpointer		 data,
	gdouble			 value
){
	if (isnan(value))
		return g_strdup("\xe2\x80\x94");

	if (NULL != format)
		return format(value, data);

	return venture_web_chart_format_number(value, NULL);
}

static void
chart_empty(
	GString		*out,
	const gchar	*title
){
	g_string_append(out, "<p class=\"muted chart-empty\">");
	venture_html_escape_append(out, title);
	g_string_append(out, ": nothing to draw yet.</p>");
}

/* The opening of the figure and its picture, with the accessible names. */
static void
chart_open(
	GString		*out,
	const gchar	*kind,
	const gchar	*title,
	const gchar	*summary,
	gdouble		 width,
	gdouble		 height
){
	g_string_append_printf(out, "<figure class=\"chart chart-%s\">", kind);
	g_string_append(out, "<svg class=\"chart-svg\" xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ");
	chart_num(out, width);
	g_string_append_c(out, ' ');
	chart_num(out, height);
	g_string_append(out, "\" role=\"img\" aria-label=\"");
	venture_html_escape_append(out, title);
	g_string_append(out, "\"><title>");
	venture_html_escape_append(out, title);
	g_string_append(out, "</title>");

	if (NULL != summary)
	{
		g_string_append(out, "<desc>");
		venture_html_escape_append(out, summary);
		g_string_append(out, "</desc>");
	}
}

static void
chart_text(
	GString		*out,
	const gchar	*klass,
	gdouble		 x,
	gdouble		 y,
	const gchar	*anchor,
	const gchar	*text
){
	g_string_append_printf(out, "<text class=\"%s\" x=\"", klass);
	chart_num(out, x);
	g_string_append(out, "\" y=\"");
	chart_num(out, y);
	g_string_append_printf(out, "\" text-anchor=\"%s\" fill=\"currentColor\">", anchor);
	venture_html_escape_append(out, text);
	g_string_append(out, "</text>");
}

/* The smallest and largest finite values of a series; FALSE when none. */
static gboolean
chart_range(
	const gdouble	*values,
	gsize		 n,
	gdouble		*low,
	gdouble		*high
){
	gboolean any = FALSE;
	gsize i;

	for (i = 0; i < n; i++)
	{
		if (!isfinite(values[i]))
			continue;

		if (!any || (values[i] < *low))
			*low = values[i];
		if (!any || (values[i] > *high))
			*high = values[i];
		any = TRUE;
	}

	return any;
}

/* Pads a flat range so a constant series is a line across the middle. */
static void
chart_widen(
	gdouble	*low,
	gdouble	*high
){
	gdouble pad;

	if (*high > *low)
	{
		pad = (*high - *low) * 0.08;
		*low -= pad;
		*high += pad;
		return;
	}

	pad = (0.0 == *low) ? 1.0 : fabs(*low) * 0.1;
	*low -= pad;
	*high += pad;
}

/* The hidden table of a line or bar chart: one row per point. */
static void
chart_table(
	GString			*out,
	const VentureWebChart	*chart,
	gsize			 n_series
){
	gsize i;
	gsize s;

	g_string_append(out, "<table class=\"visually-hidden chart-data\"><caption>");
	venture_html_escape_append(out, chart->title);
	g_string_append(out, "</caption><thead><tr><th scope=\"col\">Point</th>");

	for (s = 0; s < n_series; s++)
	{
		g_string_append(out, "<th scope=\"col\">");
		venture_html_escape_append(out, chart->series[s].name);
		g_string_append(out, "</th>");
	}

	g_string_append(out, "</tr></thead><tbody>");

	for (i = 0; i < chart->n_points; i++)
	{
		g_string_append(out, "<tr><th scope=\"row\">");
		venture_html_escape_append(out, (NULL != chart->labels) ? chart->labels[i] : "");
		g_string_append(out, "</th>");

		for (s = 0; s < n_series; s++)
		{
			g_autofree gchar *text = NULL;

			text = chart_format(chart->series[s].format, chart->series[s].format_data,
			                    chart->series[s].values[i]);
			g_string_append(out, "<td>");
			venture_html_escape_append(out, text);
			g_string_append(out, "</td>");
		}

		g_string_append(out, "</tr>");
	}

	g_string_append(out, "</tbody></table>");
}

static gboolean
chart_is_drawable(
	const VentureWebChart	*chart,
	gsize			 max_series
){
	gsize s;
	gboolean any;

	if ((NULL == chart) || (NULL == chart->title) || (0 == chart->n_points) ||
	    (chart->n_points > VENTURE_WEB_CHART_MAX_POINTS) ||
	    (NULL == chart->series) || (0 == chart->n_series) || (chart->n_series > max_series))
		return FALSE;

	any = FALSE;

	for (s = 0; s < chart->n_series; s++)
	{
		gdouble low = 0.0;
		gdouble high = 0.0;

		if (NULL == chart->series[s].values)
			return FALSE;

		if (chart_range(chart->series[s].values, chart->n_points, &low, &high))
			any = TRUE;
	}

	return any;
}

/* The bottom labels, thinned to about CHART_X_LABELS of them. */
static void
chart_x_labels(
	GString			*out,
	const VentureWebChart	*chart,
	gdouble			 left,
	gdouble			 step,
	gdouble			 baseline,
	gboolean		 centred
){
	gsize every;
	gsize i;

	if (NULL == chart->labels)
		return;

	every = (chart->n_points + CHART_X_LABELS - 1) / CHART_X_LABELS;

	if (0 == every)
		every = 1;

	for (i = 0; i < chart->n_points; i += every)
	{
		gdouble x;

		x = left + step * (gdouble)i + (centred ? step / 2.0 : 0.0);
		chart_text(out, "chart-axis", x, baseline, "middle", chart->labels[i]);
	}
}

/* The horizontal rules and their labels on one side. */
static void
chart_y_axis(
	GString			*out,
	gdouble			 low,
	gdouble			 high,
	gdouble			 top,
	gdouble			 bottom,
	gdouble			 left,
	gdouble			 right,
	gboolean		 on_right,
	gboolean		 rules,
	VentureWebChartFormat	 format,
	gpointer		 format_data
){
	gint i;

	for (i = 0; i <= CHART_TICKS; i++)
	{
		g_autofree gchar *text = NULL;
		gdouble value;
		gdouble y;

		value = low + (high - low) * (gdouble)i / (gdouble)CHART_TICKS;
		y = bottom - (bottom - top) * (gdouble)i / (gdouble)CHART_TICKS;

		if (rules)
		{
			g_string_append(out, "<line class=\"chart-grid\" x1=\"");
			chart_num(out, left);
			g_string_append(out, "\" x2=\"");
			chart_num(out, right);
			g_string_append(out, "\" y1=\"");
			chart_num(out, y);
			g_string_append(out, "\" y2=\"");
			chart_num(out, y);
			g_string_append(out, "\" stroke=\"currentColor\"/>");
		}

		text = chart_format(format, format_data, value);
		chart_text(out, "chart-axis", on_right ? right + 4.0 : left - 4.0, y + 3.5,
		           on_right ? "start" : "end", text);
	}
}

/* --- Line ------------------------------------------------------------------ */

gchar *
venture_web_chart_line(const VentureWebChart *chart)
{
	g_autoptr(GString) out = NULL;
	gdouble width;
	gdouble height;
	gdouble left;
	gdouble right;
	gdouble top;
	gdouble bottom;
	gdouble step;
	gboolean has_secondary;
	gsize s;

	out = g_string_new(NULL);

	if (!chart_is_drawable(chart, 2))
	{
		chart_empty(out, (NULL != chart) && (NULL != chart->title) ? chart->title : "Chart");
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	has_secondary = FALSE;

	for (s = 0; s < chart->n_series; s++)
		has_secondary = has_secondary || chart->series[s].secondary;

	width = (0 != chart->width) ? (gdouble)chart->width : CHART_DEFAULT_WIDTH;
	height = (0 != chart->height) ? (gdouble)chart->height : CHART_DEFAULT_HEIGHT;
	left = CHART_MARGIN_SIDE;
	right = width - (has_secondary ? CHART_MARGIN_SIDE : CHART_MARGIN_BARE);
	top = CHART_MARGIN_TOP;
	bottom = height - CHART_MARGIN_BOTTOM;
	step = (chart->n_points > 1) ? (right - left) / (gdouble)(chart->n_points - 1) : 0.0;

	chart_open(out, "line", chart->title, chart->summary, width, height);

	/*
	 * Each axis is scaled to its own series: a price and a quantity share
	 * a picture, not a unit. The primary axis draws the rules.
	 */
	for (s = 0; s < chart->n_series; s++)
	{
		const VentureWebChartSeries *series;
		gdouble low = 0.0;
		gdouble high = 0.0;
		gboolean pen_down;
		gsize i;

		series = &chart->series[s];

		if (!chart_range(series->values, chart->n_points, &low, &high))
			continue;

		chart_widen(&low, &high);

		/* Only the first series of each side draws that side's axis. */
		if ((0 == s) || (series->secondary != chart->series[0].secondary))
			chart_y_axis(out, low, high, top, bottom, left, right, series->secondary,
			             !series->secondary, series->format, series->format_data);

		g_string_append_printf(out, "<path class=\"chart-line chart-line-%" G_GSIZE_FORMAT
		                       "\" fill=\"none\" stroke=\"currentColor\" d=\"", s + 1);
		pen_down = FALSE;

		for (i = 0; i < chart->n_points; i++)
		{
			gdouble x;
			gdouble y;

			if (!isfinite(series->values[i]))
			{
				pen_down = FALSE;
				continue;
			}

			x = left + step * (gdouble)i;
			y = bottom - (bottom - top) * (series->values[i] - low) / (high - low);
			g_string_append_c(out, pen_down ? 'L' : 'M');
			chart_num(out, x);
			g_string_append_c(out, ' ');
			chart_num(out, y);
			g_string_append_c(out, ' ');
			pen_down = TRUE;
		}

		g_string_append(out, "\"/>");

		/* A point with a gap on each side is a dot, or it vanishes. */
		for (i = 0; i < chart->n_points; i++)
		{
			gboolean before;
			gboolean after;
			gdouble x;
			gdouble y;

			if (!isfinite(series->values[i]))
				continue;

			before = (i > 0) && isfinite(series->values[i - 1]);
			after = (i + 1 < chart->n_points) && isfinite(series->values[i + 1]);

			if (before || after)
				continue;

			x = left + step * (gdouble)i;
			y = bottom - (bottom - top) * (series->values[i] - low) / (high - low);
			g_string_append_printf(out, "<circle class=\"chart-dot chart-line-%" G_GSIZE_FORMAT
			                       "\" r=\"2.5\" fill=\"currentColor\" cx=\"", s + 1);
			chart_num(out, x);
			g_string_append(out, "\" cy=\"");
			chart_num(out, y);
			g_string_append(out, "\"/>");
		}
	}

	chart_x_labels(out, chart, left, step, height - 8.0, FALSE);
	g_string_append(out, "</svg>");

	/* The legend: words, with the mark each series is drawn in. */
	g_string_append(out, "<figcaption class=\"chart-legend\">");

	for (s = 0; s < chart->n_series; s++)
	{
		g_string_append_printf(out, "<span class=\"chart-key chart-key-%" G_GSIZE_FORMAT "\">", s + 1);
		venture_html_escape_append(out, chart->series[s].name);

		if (chart->series[s].secondary)
			g_string_append(out, " (right)");

		g_string_append(out, "</span>");
	}

	g_string_append(out, "</figcaption>");
	chart_table(out, chart, chart->n_series);
	g_string_append(out, "</figure>");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Bars ------------------------------------------------------------------ */

gchar *
venture_web_chart_bar(const VentureWebChart *chart)
{
	g_autoptr(GString) out = NULL;
	const VentureWebChartSeries *series;
	gdouble width;
	gdouble height;
	gdouble left;
	gdouble right;
	gdouble top;
	gdouble bottom;
	gdouble step;
	gdouble low = 0.0;
	gdouble high = 0.0;
	gdouble zero;
	gsize i;

	out = g_string_new(NULL);

	if ((NULL == chart) || (chart->n_series < 1) || !chart_is_drawable(chart, chart->n_series))
	{
		chart_empty(out, (NULL != chart) && (NULL != chart->title) ? chart->title : "Chart");
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	series = &chart->series[0];

	if (!chart_range(series->values, chart->n_points, &low, &high))
	{
		chart_empty(out, chart->title);
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	/* Bars grow from zero, so zero is always on the scale. */
	if (low > 0.0)
		low = 0.0;
	if (high < 0.0)
		high = 0.0;
	if (high == low)
		high = low + 1.0;

	width = (0 != chart->width) ? (gdouble)chart->width : CHART_DEFAULT_WIDTH;
	height = (0 != chart->height) ? (gdouble)chart->height : CHART_DEFAULT_HEIGHT;
	left = CHART_MARGIN_SIDE;
	right = width - CHART_MARGIN_BARE;
	top = CHART_MARGIN_TOP;
	bottom = height - CHART_MARGIN_BOTTOM;
	step = (right - left) / (gdouble)chart->n_points;
	zero = bottom - (bottom - top) * (0.0 - low) / (high - low);

	chart_open(out, "bar", chart->title, chart->summary, width, height);
	chart_y_axis(out, low, high, top, bottom, left, right, FALSE, TRUE, series->format,
	             series->format_data);

	for (i = 0; i < chart->n_points; i++)
	{
		g_autofree gchar *text = NULL;
		gdouble y;
		gdouble x;

		if (!isfinite(series->values[i]))
			continue;

		y = bottom - (bottom - top) * (series->values[i] - low) / (high - low);
		x = left + step * (gdouble)i + step * 0.15;
		text = chart_format(series->format, series->format_data, series->values[i]);

		g_string_append(out, "<rect class=\"chart-bar\" fill=\"currentColor\" x=\"");
		chart_num(out, x);
		g_string_append(out, "\" y=\"");
		chart_num(out, MIN(y, zero));
		g_string_append(out, "\" width=\"");
		chart_num(out, step * 0.7);
		g_string_append(out, "\" height=\"");
		chart_num(out, MAX(fabs(zero - y), 0.5));
		g_string_append(out, "\"><title>");
		venture_html_escape_append(out, (NULL != chart->labels) ? chart->labels[i] : "");
		g_string_append(out, ": ");
		venture_html_escape_append(out, text);
		g_string_append(out, "</title></rect>");
	}

	chart_x_labels(out, chart, left, step, height - 8.0, TRUE);
	g_string_append(out, "</svg>");
	chart_table(out, chart, 1);
	g_string_append(out, "</figure>");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Sparkline ------------------------------------------------------------- */

gchar *
venture_web_chart_sparkline(
	const gchar	*label,
	const gdouble	*values,
	gsize		 n_values
){
	g_autoptr(GString) out = NULL;
	gdouble low = 0.0;
	gdouble high = 0.0;
	gdouble step;
	gboolean pen_down;
	gsize i;

	out = g_string_new(NULL);

	if ((NULL == values) || (0 == n_values) || (n_values > VENTURE_WEB_CHART_MAX_POINTS) ||
	    !chart_range(values, n_values, &low, &high))
	{
		g_string_append(out, "<span class=\"sparkline-empty muted\" aria-label=\"");
		venture_html_escape_append(out, (NULL != label) ? label : "Trend");
		g_string_append(out, ": no history\">\xe2\x80\x94</span>");
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	chart_widen(&low, &high);
	step = (n_values > 1) ? (CHART_SPARK_WIDTH - 2.0) / (gdouble)(n_values - 1) : 0.0;

	g_string_append(out, "<svg class=\"sparkline\" xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ");
	chart_num(out, CHART_SPARK_WIDTH);
	g_string_append_c(out, ' ');
	chart_num(out, CHART_SPARK_HEIGHT);
	g_string_append(out, "\" preserveAspectRatio=\"none\" role=\"img\" aria-label=\"");
	venture_html_escape_append(out, (NULL != label) ? label : "Trend");
	g_string_append(out, "\"><title>");
	venture_html_escape_append(out, (NULL != label) ? label : "Trend");
	g_string_append(out, "</title><path class=\"sparkline-line\" fill=\"none\" stroke=\"currentColor\" d=\"");
	pen_down = FALSE;

	for (i = 0; i < n_values; i++)
	{
		gdouble y;

		if (!isfinite(values[i]))
		{
			pen_down = FALSE;
			continue;
		}

		y = (CHART_SPARK_HEIGHT - 2.0) - (CHART_SPARK_HEIGHT - 4.0) * (values[i] - low) / (high - low);
		g_string_append_c(out, pen_down ? 'L' : 'M');
		chart_num(out, 1.0 + step * (gdouble)i);
		g_string_append_c(out, ' ');
		chart_num(out, y);
		g_string_append_c(out, ' ');

		/* A lone point still needs a length to be seen. */
		if (!pen_down && ((i + 1 >= n_values) || !isfinite(values[i + 1])))
		{
			g_string_append_c(out, 'l');
			chart_num(out, 1.0);
			g_string_append(out, " 0 ");
		}

		pen_down = TRUE;
	}

	g_string_append(out, "\"/></svg>");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Heat map -------------------------------------------------------------- */

gchar *
venture_web_chart_heat(const VentureWebChartHeat *heat)
{
	g_autoptr(GString) out = NULL;
	gdouble low = 0.0;
	gdouble high = 0.0;
	gdouble width;
	gdouble height;
	gsize r;
	gsize c;
	gsize n;

	out = g_string_new(NULL);

	if ((NULL == heat) || (NULL == heat->title) || (NULL == heat->values) ||
	    (0 == heat->n_rows) || (0 == heat->n_columns) ||
	    (heat->n_rows > 64) || (heat->n_columns > 64))
	{
		chart_empty(out, (NULL != heat) && (NULL != heat->title) ? heat->title : "Heat map");
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	n = heat->n_rows * heat->n_columns;

	if (!chart_range(heat->values, n, &low, &high))
	{
		chart_empty(out, heat->title);
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	width = CHART_HEAT_LABEL_WIDTH + CHART_HEAT_CELL * (gdouble)heat->n_columns;
	height = CHART_HEAT_LABEL_HEIGHT + CHART_HEAT_CELL * (gdouble)heat->n_rows;
	chart_open(out, "heat", heat->title, heat->summary, width, height);

	/* The column labels across the top, every third one. */
	for (c = 0; (NULL != heat->column_labels) && (c < heat->n_columns); c += 3)
		chart_text(out, "chart-axis",
		           CHART_HEAT_LABEL_WIDTH + CHART_HEAT_CELL * ((gdouble)c + 0.5),
		           CHART_HEAT_LABEL_HEIGHT - 6.0, "middle", heat->column_labels[c]);

	for (r = 0; r < heat->n_rows; r++)
	{
		gdouble y;

		y = CHART_HEAT_LABEL_HEIGHT + CHART_HEAT_CELL * (gdouble)r;

		if (NULL != heat->row_labels)
			chart_text(out, "chart-axis", CHART_HEAT_LABEL_WIDTH - 6.0,
			           y + CHART_HEAT_CELL * 0.65, "end", heat->row_labels[r]);

		for (c = 0; c < heat->n_columns; c++)
		{
			g_autofree gchar *text = NULL;
			gdouble value;
			gdouble strength;
			gdouble x;
			gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

			value = heat->values[r * heat->n_columns + c];
			x = CHART_HEAT_LABEL_WIDTH + CHART_HEAT_CELL * (gdouble)c;

			g_string_append(out, "<rect class=\"chart-heat-cell");

			if (!isfinite(value))
				g_string_append(out, " chart-heat-empty\" fill=\"none\" stroke=\"currentColor\"");
			else
			{
				/*
				 * The strength runs 0.12 to 0.92 so the faintest cell
				 * is still a cell and the strongest still lets the
				 * rule show; flat data reads as the middle.
				 */
				strength = (high > low) ? (value - low) / (high - low) : 0.5;

				if (heat->low_is_strong)
					strength = 1.0 - strength;

				g_string_append(out, "\" fill=\"currentColor\" fill-opacity=\"");
				g_string_append(out, g_ascii_formatd(buffer, sizeof(buffer), "%.2f",
				                                     0.12 + 0.8 * strength));
				g_string_append_c(out, '"');
			}

			g_string_append(out, " x=\"");
			chart_num(out, x + 1.0);
			g_string_append(out, "\" y=\"");
			chart_num(out, y + 1.0);
			g_string_append(out, "\" width=\"");
			chart_num(out, CHART_HEAT_CELL - 2.0);
			g_string_append(out, "\" height=\"");
			chart_num(out, CHART_HEAT_CELL - 2.0);
			g_string_append(out, "\"><title>");
			venture_html_escape_append(out, (NULL != heat->row_labels) ? heat->row_labels[r] : "");
			g_string_append_c(out, ' ');
			venture_html_escape_append(out, (NULL != heat->column_labels) ? heat->column_labels[c] : "");
			g_string_append(out, ": ");
			text = chart_format(heat->format, heat->format_data, value);
			venture_html_escape_append(out, text);
			g_string_append(out, "</title></rect>");
		}
	}

	g_string_append(out, "</svg>");

	/*
	 * A map has no legend line to say what it shows, so it says it in
	 * words, and which way the shading runs: "stronger", because the ink
	 * is the theme's text colour, dark on paper and light on the screen.
	 */
	g_string_append(out, "<figcaption class=\"chart-caption\">");
	venture_html_escape_append(out, heat->title);
	g_string_append(out, heat->low_is_strong ? ". The stronger the cell, the lower the figure."
	                                          : ". The stronger the cell, the higher the figure.");
	g_string_append(out, "</figcaption>");

	/* The table: a row per row label, a column per column label. */
	g_string_append(out, "<table class=\"visually-hidden chart-data\"><caption>");
	venture_html_escape_append(out, heat->title);
	g_string_append(out, "</caption><thead><tr><td></td>");

	for (c = 0; c < heat->n_columns; c++)
	{
		g_string_append(out, "<th scope=\"col\">");
		venture_html_escape_append(out, (NULL != heat->column_labels) ? heat->column_labels[c] : "");
		g_string_append(out, "</th>");
	}

	g_string_append(out, "</tr></thead><tbody>");

	for (r = 0; r < heat->n_rows; r++)
	{
		g_string_append(out, "<tr><th scope=\"row\">");
		venture_html_escape_append(out, (NULL != heat->row_labels) ? heat->row_labels[r] : "");
		g_string_append(out, "</th>");

		for (c = 0; c < heat->n_columns; c++)
		{
			g_autofree gchar *text = NULL;

			text = chart_format(heat->format, heat->format_data,
			                    heat->values[r * heat->n_columns + c]);
			g_string_append(out, "<td>");
			venture_html_escape_append(out, text);
			g_string_append(out, "</td>");
		}

		g_string_append(out, "</tr>");
	}

	g_string_append(out, "</tbody></table></figure>");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Formatters ---------------------------------------------------------- */

gchar *
venture_web_chart_format_number(
	gdouble		 value,
	gpointer	 user_data
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	(void)user_data;

	if (!isfinite(value))
		return g_strdup("\xe2\x80\x94");

	if ((fabs(value) < 1e15) && (value == floor(value)))
		return g_strdup_printf("%" G_GINT64_FORMAT, (gint64)value);

	return g_strdup(g_ascii_formatd(buffer, sizeof(buffer), "%.2f", value));
}

gchar *
venture_web_chart_format_minor(
	gdouble		 value,
	gpointer	 user_data
){
	g_autoptr(VentureMoney) money = NULL;
	const gchar *currency;

	currency = user_data;

	if (!isfinite(value) || (fabs(value) > 9.0e15) || venture_string_is_empty(currency) ||
	    !venture_currency_is_valid(currency))
		return venture_web_chart_format_number(value, NULL);

	money = venture_money_new_for_currency((gint64)llround(value), currency);

	if (NULL == money)
		return venture_web_chart_format_number(value, NULL);

	return venture_money_to_display_string(money, TRUE);
}

gchar *
venture_web_chart_format_percent(
	gdouble		 value,
	gpointer	 user_data
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	(void)user_data;

	if (!isfinite(value))
		return g_strdup("\xe2\x80\x94");

	return g_strdup_printf("%s%%", g_ascii_formatd(buffer, sizeof(buffer), "%.1f", value));
}
