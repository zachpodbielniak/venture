/*
 * venture-web-query-chart.h - A query's rows as a table, bars, a line or a pie
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One function turns a venture.query/1 document (what POST /api/v1/query
 * answers) and three column picks -- along the bottom, the series, the
 * value -- into server-rendered HTML and SVG, through the drawings in
 * venture-web-chart.h. The dashboard's query widget calls it; so will the
 * Chart toggle on a list page, with the query its bar built.
 *
 * Money is never added across currencies: a money value column draws one
 * series per currency -- for bars and pies one chart each, for a line one
 * chart per currency holding that currency's series -- so dollars and gold
 * never share an axis, a bar or a slice. sum_money() hands one total per
 * currency, and each becomes its own series.
 */

#ifndef VENTURE_WEB_QUERY_CHART_H
#define VENTURE_WEB_QUERY_CHART_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_WEB_QUERY_CHART_MAX_CHARTS:
 *
 * The most drawings one query chart makes: bars and pies draw one per
 * series (a series value, times a currency), a line one per currency.
 * Past it the chart is refused with a suggestion, rather than a page of
 * slivers.
 */
#define VENTURE_WEB_QUERY_CHART_MAX_CHARTS (8)

/**
 * venture_web_query_chart:
 * @document: a venture.query/1 document (columns and rows)
 * @chart: (nullable): `table` (the default), `bar`, `line` or `pie`
 * @x: (nullable): the column along the bottom, or a pie's slice labels;
 *   the first column when %NULL
 * @series: (nullable): a column whose values split the rows into series
 * @value: (nullable): the column drawn: a number, money, or a list of
 *   money (sum_money()); the first such column other than @x and @series
 *   when %NULL
 * @out_data: (out) (optional) (transfer full): what was drawn, as JSON:
 *   `chart`, `x`, `series`, `value`, `labels` (one per point) and `groups`,
 *   each `{name, series, currency, values}` with money as its exact JSON
 *   (venture_money_to_json()) and a missing point null
 * @error: (out) (optional): return location for a #GError
 *
 * Draws a query's rows. `table` shows every column, money one figure per
 * currency; the other kinds read @x, @series and @value. A point that
 * appears twice in a series (two rows with the same @x) is the sum of
 * both, within that series' one currency. Refused, naming the columns
 * there are, when a pick is not a column or the value column is neither a
 * number nor money; a pie refuses a negative value and more than
 * %VENTURE_WEB_CHART_MAX_SLICES slices.
 *
 * Returns: (transfer full) (nullable): the HTML, or %NULL with @error set
 */
gchar *
venture_web_query_chart(
	JsonNode	 *document,
	const gchar	 *chart,
	const gchar	 *x,
	const gchar	 *series,
	const gchar	 *value,
	JsonNode	**out_data,
	GError		**error
);

/**
 * venture_web_query_chart_is_kind:
 * @chart: (nullable): a chart kind
 *
 * Returns: %TRUE for `table`, `bar`, `line` and `pie`
 */
gboolean
venture_web_query_chart_is_kind(const gchar *chart);

G_END_DECLS

#endif /* VENTURE_WEB_QUERY_CHART_H */
