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
 * - An axis steps in round figures and its margin is as wide as its
 *   labels: a price axis reads 2600g, 2800g, never a cut-off
 *   "958g 31s 50c".
 * - A line or bar chart carries its raw numbers as JSON beside the table
 *   (script[data-plot]); venture.js redraws from them at the size the
 *   chart is shown and adds the readout, toggles and zoom. Nothing in
 *   that JSON is markup, and <, >, & and ' in it are escapes.
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
#define CHART_TICK_TARGET (4)
#define CHART_X_LABELS (6)
/* One character of the 10px monospace an axis is set in, in user units. */
#define CHART_CHAR_WIDTH (6.2)
#define CHART_HEAT_CELL (22.0)
#define CHART_HEAT_LABEL_WIDTH (40.0)
#define CHART_HEAT_LABEL_HEIGHT (18.0)
#define CHART_SPARK_WIDTH (100.0)
#define CHART_SPARK_HEIGHT (24.0)

/* One side's axis: from @low to @high in @n_steps steps of @step. */
typedef struct
{
	gdouble		low;
	gdouble		high;
	gdouble		step;
	gsize		n_steps;
	gboolean	integral;
} ChartScale;

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

/* How wide @text is drawn in the 10px monospace of an axis. */
static gdouble
chart_text_width(const gchar *text)
{
	return (gdouble)g_utf8_strlen((NULL != text) ? text : "", -1) * CHART_CHAR_WIDTH;
}

/* Whether every finite value is whole: a count, or minor units. */
static gboolean
chart_integral(
	const gdouble	*values,
	gsize		 n
){
	gsize i;

	for (i = 0; i < n; i++)
	{
		if (isfinite(values[i]) && (values[i] != floor(values[i])))
			return FALSE;
	}

	return TRUE;
}

/*
 * A round step that cuts @span into about CHART_TICK_TARGET pieces: 1, 2,
 * 2.5 or 5 times a power of ten, so an axis reads 2600g, 2800g, 3000g
 * rather than 2958g 31s 50c. Whole data gets a whole step.
 */
static gdouble
chart_nice_step(
	gdouble		 span,
	gboolean	 integral
){
	gdouble raw;
	gdouble magnitude;
	gdouble fraction;
	gdouble step;

	raw = span / (gdouble)CHART_TICK_TARGET;

	if (!isfinite(raw) || (raw <= 0.0))
		raw = 1.0;

	magnitude = pow(10.0, floor(log10(raw)));
	fraction = raw / magnitude;

	if (fraction <= 1.0)
		step = 1.0;
	else if (fraction <= 2.0)
		step = 2.0;
	else if (fraction <= 2.5)
		step = 2.5;
	else if (fraction <= 5.0)
		step = 5.0;
	else
		step = 10.0;

	step *= magnitude;

	/* 2.5 of a whole unit is not a whole step; 2 is near enough. */
	if (integral && (step != floor(step)))
		step = MAX(1.0, floor(step));

	return step;
}

/*
 * The scale of one side: the data's range padded a little, then widened
 * out to whole steps, never below zero when nothing is. A flat series
 * gets a range around it, so it is a line across the middle.
 */
static void
chart_scale(
	gdouble		 low,
	gdouble		 high,
	gboolean	 integral,
	ChartScale	*scale
){
	gboolean non_negative;
	gdouble pad;
	gdouble step;

	non_negative = (low >= 0.0);

	if (high <= low)
		pad = integral ? 1.0 : ((0.0 == low) ? 1.0 : fabs(low) * 0.1);
	else
		pad = (high - low) * 0.04;

	low -= pad;
	high += pad;

	if (non_negative && (low < 0.0))
		low = 0.0;

	step = chart_nice_step(high - low, integral);
	scale->step = step;
	scale->low = floor(low / step) * step;
	scale->high = ceil(high / step) * step;

	if (scale->high <= scale->low)
		scale->high = scale->low + step;

	scale->n_steps = (gsize)llround((scale->high - scale->low) / step);
	scale->integral = integral;
}

/* Where @value sits between @top and @bottom on @scale. */
static gdouble
chart_y(
	const ChartScale	*scale,
	gdouble			 value,
	gdouble			 top,
	gdouble			 bottom
){
	return bottom - (bottom - top) * (value - scale->low) / (scale->high - scale->low);
}

/* The labels of one axis, bottom up, and the widest of them. */
static GPtrArray *
chart_tick_labels(
	const ChartScale	*scale,
	VentureWebChartFormat	 format,
	gpointer		 format_data,
	gdouble			*widest
){
	GPtrArray *labels;
	gsize k;

	labels = g_ptr_array_new_with_free_func(g_free);
	*widest = 0.0;

	for (k = 0; k <= scale->n_steps; k++)
	{
		gdouble value;
		gchar *text;

		value = scale->low + scale->step * (gdouble)k;

		/* Whole steps stay whole; a sum of 0.1s must not read 0.30000004. */
		if (scale->integral)
			value = (gdouble)llround(value);
		else
			value = round(value / scale->step * 1e6) / 1e6 * scale->step;

		text = chart_format(format, format_data, value);
		*widest = MAX(*widest, chart_text_width(text));
		g_ptr_array_add(labels, text);
	}

	return labels;
}

/*
 * The bottom labels: as many as fit their width, at most CHART_X_LABELS,
 * and the two at the ends kept inside the drawing.
 */
static void
chart_x_labels(
	GString			*out,
	const VentureWebChart	*chart,
	gdouble			 left,
	gdouble			 right,
	gdouble			 step,
	gdouble			 baseline,
	gboolean		 centred,
	gdouble			 width
){
	gdouble widest;
	gsize fit;
	gsize every;
	gsize i;

	if (NULL == chart->labels)
		return;

	widest = 0.0;

	for (i = 0; i < chart->n_points; i++)
		widest = MAX(widest, chart_text_width(chart->labels[i]));

	fit = (gsize)MAX(1.0, floor((right - left) / (widest + 14.0)));
	fit = MIN(fit, (gsize)CHART_X_LABELS);
	every = (chart->n_points + fit - 1) / fit;

	if (0 == every)
		every = 1;

	for (i = 0; i < chart->n_points; i += every)
	{
		const gchar *anchor;
		gdouble x;
		gdouble half;

		x = left + step * (gdouble)i + (centred ? step / 2.0 : 0.0);
		half = chart_text_width(chart->labels[i]) / 2.0;
		anchor = (x - half < 0.0) ? "start" : (x + half > width) ? "end" : "middle";

		if (0 == g_strcmp0(anchor, "start"))
			x = MAX(x - half, 0.0);
		else if (0 == g_strcmp0(anchor, "end"))
			x = MIN(x + half, width);

		chart_text(out, "chart-axis", x, baseline, anchor, chart->labels[i]);
	}
}

/* The horizontal rules and their labels on one side. */
static void
chart_y_axis(
	GString			*out,
	const ChartScale	*scale,
	GPtrArray		*labels,
	gdouble			 top,
	gdouble			 bottom,
	gdouble			 left,
	gdouble			 right,
	gboolean		 on_right,
	gboolean		 rules
){
	gsize k;

	for (k = 0; k <= scale->n_steps; k++)
	{
		gdouble y;

		y = bottom - (bottom - top) * (gdouble)k / (gdouble)scale->n_steps;

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

		chart_text(out, "chart-axis", on_right ? right + 6.0 : left - 6.0, y + 3.5,
		           on_right ? "start" : "end", g_ptr_array_index(labels, k));
	}
}

/*
 * Appends @text as a JSON string that is safe inside <script>: the
 * characters HTML could read (<, >, &, ') are \u escapes, so a feed's
 * "</script>" in a currency symbol stays a string.
 */
static void
chart_json_string(
	GString		*out,
	const gchar	*text
){
	const guchar *p;

	g_string_append_c(out, '"');

	for (p = (const guchar *)((NULL != text) ? text : ""); '\0' != *p; p++)
	{
		if (('"' == *p) || ('\\' == *p))
		{
			g_string_append_c(out, '\\');
			g_string_append_c(out, (gchar)*p);
		}
		else if ((*p < 0x20) || ('<' == *p) || ('>' == *p) || ('&' == *p) || ('\'' == *p))
		{
			g_string_append_printf(out, "\\u%04x", (guint)*p);
		}
		else
		{
			g_string_append_c(out, (gchar)*p);
		}
	}

	g_string_append_c(out, '"');
}

/*
 * How a series' axis reads, for a script that draws ticks of its own: a
 * currency's exponent and what goes either side of the figure ("" and
 * "g", "$" and ""), a percent, or a plain number.
 */
static void
chart_spec_axis(
	GString				*out,
	const VentureWebChartSeries	*series,
	gsize				 n_points
){
	const gchar *currency;

	currency = series->currency;

	if ((NULL == currency) && (series->format == venture_web_chart_format_minor))
		currency = series->format_data;

	if (!venture_string_is_empty(currency) && venture_currency_is_valid(currency))
	{
		g_autoptr(VentureMoney) one = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *prefix = NULL;
		const gchar *digit;

		/* One whole unit at exponent 0: "1g", "$1", "1 PTS". */
		one = venture_money_new(1, currency, 0);
		text = (NULL != one) ? venture_money_to_display_string(one, FALSE) : NULL;
		digit = (NULL != text) ? strchr(text, '1') : NULL;

		if (NULL != digit)
		{
			prefix = g_strndup(text, (gsize)(digit - text));
			g_string_append_printf(out, "{\"kind\":\"money\",\"exp\":%u,\"pre\":",
			                       (guint)venture_currency_get_exponent(currency));
			chart_json_string(out, prefix);
			g_string_append(out, ",\"suf\":");
			chart_json_string(out, digit + 1);
			g_string_append_c(out, '}');
			return;
		}
	}

	if (series->format == venture_web_chart_format_percent)
	{
		g_string_append(out, "{\"kind\":\"percent\"}");
		return;
	}

	g_string_append_printf(out, "{\"kind\":\"number\",\"int\":%s}",
	                       chart_integral(series->values, n_points) ? "true" : "false");
}

/*
 * The numbers behind a line or bar chart, as JSON in a script element the
 * browser never runs (type="application/json"). venture.js reads them
 * with the table's text -- names, labels and every figure as the server
 * wrote it -- and redraws the chart at the size it is shown.
 */
static void
chart_spec(
	GString			*out,
	const VentureWebChart	*chart,
	gsize			 n_series,
	const gchar		*kind
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	gsize s;
	gsize i;

	g_string_append_printf(out, "<script type=\"application/json\" data-plot>{\"kind\":\"%s\",\"sec\":[",
	                       kind);

	for (s = 0; s < n_series; s++)
		g_string_append_printf(out, "%s%s", (s > 0) ? "," : "", chart->series[s].secondary ? "true" : "false");

	g_string_append(out, "],\"axis\":[");

	for (s = 0; s < n_series; s++)
	{
		if (s > 0)
			g_string_append_c(out, ',');

		chart_spec_axis(out, &chart->series[s], chart->n_points);
	}

	g_string_append(out, "],\"v\":[");

	for (s = 0; s < n_series; s++)
	{
		g_string_append(out, (s > 0) ? ",[" : "[");

		for (i = 0; i < chart->n_points; i++)
		{
			gdouble value = chart->series[s].values[i];

			if (i > 0)
				g_string_append_c(out, ',');

			if (isfinite(value))
				g_string_append(out, g_ascii_formatd(buffer, sizeof(buffer), "%.15g", value));
			else
				g_string_append(out, "null");
		}

		g_string_append_c(out, ']');
	}

	g_string_append(out, "]}</script>");
}

/* --- Line ------------------------------------------------------------------ */

gchar *
venture_web_chart_line(const VentureWebChart *chart)
{
	g_autoptr(GString) out = NULL;
	GPtrArray *tick_labels[2];
	ChartScale scales[2];
	gdouble widest[2];
	gdouble width;
	gdouble height;
	gdouble left;
	gdouble right;
	gdouble top;
	gdouble bottom;
	gdouble step;
	gboolean sides[2];
	gboolean integral[2];
	gdouble lows[2];
	gdouble highs[2];
	gint axis_series[2];
	gsize side;
	gsize s;

	out = g_string_new(NULL);

	if (!chart_is_drawable(chart, VENTURE_WEB_CHART_MAX_SERIES))
	{
		chart_empty(out, (NULL != chart) && (NULL != chart->title) ? chart->title : "Chart");
		return g_string_free(g_steal_pointer(&out), FALSE);
	}

	width = (0 != chart->width) ? (gdouble)chart->width : CHART_DEFAULT_WIDTH;
	height = (0 != chart->height) ? (gdouble)chart->height : CHART_DEFAULT_HEIGHT;

	/*
	 * Each side is scaled to the series drawn against it: a price and a
	 * quantity share a picture, not a unit, but two prices share a unit
	 * and must share an axis. Side 0 is the left, 1 the right. The first
	 * drawable series of a side labels it, in its own format.
	 */
	for (side = 0; side < 2; side++)
	{
		tick_labels[side] = NULL;
		sides[side] = FALSE;
		integral[side] = TRUE;
		lows[side] = highs[side] = 0.0;
		axis_series[side] = -1;
		widest[side] = 0.0;
	}

	for (s = 0; s < chart->n_series; s++)
	{
		gdouble low = 0.0;
		gdouble high = 0.0;

		side = chart->series[s].secondary ? 1 : 0;

		if (!chart_range(chart->series[s].values, chart->n_points, &low, &high))
			continue;

		if (!sides[side] || (low < lows[side]))
			lows[side] = low;
		if (!sides[side] || (high > highs[side]))
			highs[side] = high;
		if (!sides[side])
			axis_series[side] = (gint)s;

		integral[side] = integral[side] && chart_integral(chart->series[s].values, chart->n_points);
		sides[side] = TRUE;
	}

	for (side = 0; side < 2; side++)
	{
		const VentureWebChartSeries *labelled;

		if (!sides[side])
			continue;

		labelled = &chart->series[axis_series[side]];
		chart_scale(lows[side], highs[side], integral[side], &scales[side]);
		tick_labels[side] = chart_tick_labels(&scales[side], labelled->format, labelled->format_data,
		                                      &widest[side]);
	}

	/* The margins are as wide as the labels in them, so none is cut. */
	left = sides[0] ? MAX(CHART_MARGIN_BARE * 2.0, widest[0] + 10.0) : CHART_MARGIN_BARE;
	right = width - (sides[1] ? MAX(CHART_MARGIN_BARE * 2.0, widest[1] + 10.0) : CHART_MARGIN_BARE);
	left = MIN(left, width * 0.4);
	right = MAX(right, width * 0.6);
	top = CHART_MARGIN_TOP;
	bottom = height - CHART_MARGIN_BOTTOM;
	step = (chart->n_points > 1) ? (right - left) / (gdouble)(chart->n_points - 1) : 0.0;

	chart_open(out, "line", chart->title, chart->summary, width, height);

	for (side = 0; side < 2; side++)
	{
		if (sides[side])
			chart_y_axis(out, &scales[side], tick_labels[side], top, bottom, left, right, 1 == side,
			             (0 == side) || !sides[0]);
	}

	for (s = 0; s < chart->n_series; s++)
	{
		const VentureWebChartSeries *series;
		const ChartScale *scale;
		g_autoptr(GString) bridges = NULL;
		gdouble low = 0.0;
		gdouble high = 0.0;
		gboolean pen_down;
		gssize previous;
		gsize i;

		series = &chart->series[s];

		/* A series with nothing to draw draws nothing. */
		if (!chart_range(series->values, chart->n_points, &low, &high))
			continue;

		scale = &scales[series->secondary ? 1 : 0];
		bridges = g_string_new(NULL);

		g_string_append_printf(out, "<path class=\"chart-line chart-line-%" G_GSIZE_FORMAT
		                       "\" fill=\"none\" stroke=\"currentColor\" d=\"", s + 1);
		pen_down = FALSE;
		previous = -1;

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
			y = chart_y(scale, series->values[i], top, bottom);
			g_string_append_c(out, pen_down ? 'L' : 'M');
			chart_num(out, x);
			g_string_append_c(out, ' ');
			chart_num(out, y);
			g_string_append_c(out, ' ');
			pen_down = TRUE;

			/* Across a gap, a bridge from the last point seen. */
			if ((previous >= 0) && ((gsize)previous + 1 < i))
			{
				g_string_append_c(bridges, 'M');
				chart_num(bridges, left + step * (gdouble)previous);
				g_string_append_c(bridges, ' ');
				chart_num(bridges, chart_y(scale, series->values[previous], top, bottom));
				g_string_append(bridges, " L");
				chart_num(bridges, x);
				g_string_append_c(bridges, ' ');
				chart_num(bridges, y);
				g_string_append_c(bridges, ' ');
			}

			previous = (gssize)i;
		}

		g_string_append(out, "\"/>");

		if (bridges->len > 0)
			g_string_append_printf(out, "<path class=\"chart-bridge chart-line-%" G_GSIZE_FORMAT
			                       "\" fill=\"none\" stroke=\"currentColor\" d=\"%s\"/>", s + 1,
			                       bridges->str);

		/* A point with a gap on each side is a dot, or it vanishes. */
		for (i = 0; i < chart->n_points; i++)
		{
			gboolean before;
			gboolean after;

			if (!isfinite(series->values[i]))
				continue;

			before = (i > 0) && isfinite(series->values[i - 1]);
			after = (i + 1 < chart->n_points) && isfinite(series->values[i + 1]);

			if (before || after)
				continue;

			g_string_append_printf(out, "<circle class=\"chart-dot chart-line-%" G_GSIZE_FORMAT
			                       "\" r=\"2.5\" fill=\"currentColor\" cx=\"", s + 1);
			chart_num(out, left + step * (gdouble)i);
			g_string_append(out, "\" cy=\"");
			chart_num(out, chart_y(scale, series->values[i], top, bottom));
			g_string_append(out, "\"/>");
		}
	}

	chart_x_labels(out, chart, left, right, step, height - 8.0, FALSE, width);
	g_string_append(out, "</svg>");
	g_clear_pointer(&tick_labels[0], g_ptr_array_unref);
	g_clear_pointer(&tick_labels[1], g_ptr_array_unref);

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
	chart_spec(out, chart, chart->n_series, "line");
	g_string_append(out, "</figure>");

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Bars ------------------------------------------------------------------ */

gchar *
venture_web_chart_bar(const VentureWebChart *chart)
{
	g_autoptr(GString) out = NULL;
	g_autoptr(GPtrArray) tick_labels = NULL;
	const VentureWebChartSeries *series;
	ChartScale scale;
	gdouble widest;
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

	chart_scale(low, high, chart_integral(series->values, chart->n_points), &scale);
	tick_labels = chart_tick_labels(&scale, series->format, series->format_data, &widest);

	width = (0 != chart->width) ? (gdouble)chart->width : CHART_DEFAULT_WIDTH;
	height = (0 != chart->height) ? (gdouble)chart->height : CHART_DEFAULT_HEIGHT;
	left = MIN(MAX(CHART_MARGIN_BARE * 2.0, widest + 10.0), width * 0.4);
	right = width - CHART_MARGIN_BARE;
	top = CHART_MARGIN_TOP;
	bottom = height - CHART_MARGIN_BOTTOM;
	step = (right - left) / (gdouble)chart->n_points;
	zero = chart_y(&scale, 0.0, top, bottom);

	chart_open(out, "bar", chart->title, chart->summary, width, height);
	chart_y_axis(out, &scale, tick_labels, top, bottom, left, right, FALSE, TRUE);

	for (i = 0; i < chart->n_points; i++)
	{
		g_autofree gchar *text = NULL;
		gdouble y;
		gdouble x;

		if (!isfinite(series->values[i]))
			continue;

		y = chart_y(&scale, series->values[i], top, bottom);
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

	chart_x_labels(out, chart, left, right, step, height - 8.0, TRUE, width);
	g_string_append(out, "</svg>");
	chart_table(out, chart, 1);
	chart_spec(out, chart, 1, "bar");
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
