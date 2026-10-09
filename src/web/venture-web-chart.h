/*
 * venture-web-chart.h - Server-rendered SVG charts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Four drawings -- a line (with an optional second series on its own
 * axis), bars, a sparkline and a weekday by hour heat map -- written as
 * inline SVG by the server, so a chart needs no script and prints. Every
 * mark is drawn in currentColor or a class the two stylesheets colour from
 * their tokens: no literal colour reaches the markup. Every chart carries
 * a <title>, a <desc> and an aria-label, and the line, bar and heat charts
 * a visually hidden table of every figure, because a picture of numbers
 * is not the numbers. Every label is escaped: an instrument's name is
 * whatever a feed said it was.
 */

#ifndef VENTURE_WEB_CHART_H
#define VENTURE_WEB_CHART_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_WEB_CHART_MAX_POINTS:
 *
 * The most points a line or bar chart draws; past it the chart refuses
 * rather than drawing an unreadable smear or an enormous page.
 */
#define VENTURE_WEB_CHART_MAX_POINTS (2000)

/**
 * VENTURE_WEB_CHART_MAX_SERIES:
 *
 * The most series a line chart draws. Each has its own mark
 * (chart-line-1 to chart-line-4) in both stylesheets; a fifth would have
 * none, so it is refused rather than drawn in a style nobody chose.
 */
#define VENTURE_WEB_CHART_MAX_SERIES (4)

/**
 * VentureWebChartFormat:
 * @value: a value, never NAN
 * @user_data: the data given beside the function
 *
 * How a value reads on an axis, in a tooltip and in the data table.
 *
 * Returns: (transfer full): the text, which the chart escapes
 */
typedef gchar * (*VentureWebChartFormat) (
	gdouble		 value,
	gpointer	 user_data
);

/**
 * VentureWebChartSeries:
 * @name: what the series is, for the legend and the table's heading
 * @values: (array): one value per point; NAN is a gap
 * @secondary: draw against the right-hand axis (a line chart's second
 *   series, such as quantity beside a price)
 * @format: (nullable): how a value reads; %NULL for a plain number
 * @format_data: (nullable): passed to @format
 *
 * One line or one set of bars.
 */
typedef struct
{
	const gchar		*name;
	const gdouble		*values;
	gboolean		 secondary;
	VentureWebChartFormat	 format;
	gpointer		 format_data;
} VentureWebChartSeries;

/**
 * VentureWebChart:
 * @title: what the chart shows; its <title> and aria-label
 * @summary: (nullable): a sentence a screen reader reads as the <desc>
 * @labels: (array length=n_points): one label per point, along the
 *   bottom (thinned to fit) and down the data table
 * @n_points: how many points
 * @series: (array length=n_series): the series
 * @n_series: how many: one to %VENTURE_WEB_CHART_MAX_SERIES for a line,
 *   one for bars
 * @width: the drawing's width in user units, 0 for 640
 * @height: its height, 0 for 220
 *
 * A line or bar chart.
 */
typedef struct
{
	const gchar			*title;
	const gchar			*summary;
	const gchar *const		*labels;
	gsize				 n_points;
	const VentureWebChartSeries	*series;
	gsize				 n_series;
	guint				 width;
	guint				 height;
} VentureWebChart;

/**
 * VentureWebChartHeat:
 * @title: what the map shows; its <title> and aria-label
 * @summary: (nullable): the <desc>
 * @row_labels: (array length=n_rows): one per row (a weekday)
 * @n_rows: rows, at least one
 * @column_labels: (array length=n_columns): one per column (an hour)
 * @n_columns: columns, at least one
 * @values: (array): @n_rows times @n_columns values, row by row; NAN is a
 *   cell with nothing in it
 * @format: (nullable): how a value reads
 * @format_data: (nullable): passed to @format
 * @low_is_strong: draw the lowest values darkest (a price map, where
 *   cheap is what a buyer looks for) rather than the highest
 *
 * A grid of cells shaded by value.
 */
typedef struct
{
	const gchar		*title;
	const gchar		*summary;
	const gchar *const	*row_labels;
	gsize			 n_rows;
	const gchar *const	*column_labels;
	gsize			 n_columns;
	const gdouble		*values;
	VentureWebChartFormat	 format;
	gpointer		 format_data;
	gboolean		 low_is_strong;
} VentureWebChartHeat;

/**
 * venture_web_chart_line:
 * @chart: the chart
 *
 * Draws a line chart: up to %VENTURE_WEB_CHART_MAX_SERIES series, those
 * marked secondary against an axis of their own on the right, with a
 * legend and the data table. The series on one side share that side's
 * scale -- a price and a median of prices are read against one axis, or
 * the gap between them would mean nothing. A gap (NAN) breaks the line; a
 * point with gaps both sides is drawn as a dot.
 *
 * Returns: (transfer full): an HTML <figure>, or an empty-state paragraph
 *   when there is nothing to draw
 */
gchar *
venture_web_chart_line(const VentureWebChart *chart);

/**
 * venture_web_chart_bar:
 * @chart: the chart; its first series is drawn
 *
 * Draws vertical bars from zero, with the data table.
 *
 * Returns: (transfer full): an HTML <figure>, or an empty-state paragraph
 */
gchar *
venture_web_chart_bar(const VentureWebChart *chart);

/**
 * venture_web_chart_sparkline:
 * @label: what the line is, read out as the image's name
 * @values: (array length=n_values): the values, NAN for gaps
 * @n_values: how many
 *
 * A word-sized line for a table cell: no axes, no table, the label is its
 * accessible name. Nothing to draw gives a dash with the same name.
 *
 * Returns: (transfer full): an inline <svg>, or a <span> when empty
 */
gchar *
venture_web_chart_sparkline(
	const gchar	*label,
	const gdouble	*values,
	gsize		 n_values
);

/**
 * venture_web_chart_heat:
 * @heat: the map
 *
 * Draws a heat map, one cell per value, shaded in the current colour by
 * where the value falls between the smallest and largest. Each cell has a
 * tooltip, and the map a data table.
 *
 * Returns: (transfer full): an HTML <figure>, or an empty-state paragraph
 */
gchar *
venture_web_chart_heat(const VentureWebChartHeat *heat);

/**
 * venture_web_chart_format_number:
 * @value: a value
 * @user_data: (nullable): unused
 *
 * A plain number: whole when it is whole, else up to two decimals.
 *
 * Returns: (transfer full): the text
 */
gchar *
venture_web_chart_format_number(
	gdouble		 value,
	gpointer	 user_data
);

/**
 * venture_web_chart_format_minor:
 * @value: an amount in minor units
 * @user_data: (type utf8): the currency code
 *
 * An amount of money, read the way the currency reads it (its symbol, or
 * its coins). Charts are display: the double came from integer minor
 * units and is rounded back to them before formatting.
 *
 * Returns: (transfer full): the text
 */
gchar *
venture_web_chart_format_minor(
	gdouble		 value,
	gpointer	 user_data
);

/**
 * venture_web_chart_format_percent:
 * @value: a percent (80 is 80%)
 * @user_data: (nullable): unused
 *
 * Returns: (transfer full): the text, to one decimal
 */
gchar *
venture_web_chart_format_percent(
	gdouble		 value,
	gpointer	 user_data
);

G_END_DECLS

#endif /* VENTURE_WEB_CHART_H */
