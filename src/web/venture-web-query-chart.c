/*
 * venture-web-query-chart.c - A query's rows as a table, bars, a line or a pie
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The plan, in one pass over the rows:
 *
 *   1. the labels: the distinct values of the x column, in the order the
 *      rows first show them (a summarize's own order, a sort's order);
 *   2. the groups: one per series value and currency. A number is in no
 *      currency; money is in its own; sum_money()'s list holds one total
 *      per currency and adds one point to each currency's group. Nothing
 *      here ever adds two currencies together;
 *   3. the drawings: bars and pies one per group, a line one per currency
 *      with that currency's groups as its lines.
 *
 * The JSON beside the HTML is written from the same groups, so what a
 * page draws and what the API says it drew cannot disagree.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

/* One series: a series value in one currency (or none, for numbers). */
typedef struct
{
	gchar		*name;
	gchar		*series;	/* the series column's text, or NULL */
	gchar		*currency;	/* the currency, or NULL for numbers */
	GPtrArray	*money;		/* per point: VentureMoney or NULL */
	GArray		*numbers;	/* per point: gdouble, NAN when absent */
} QueryGroup;

static void
query_group_free(gpointer data)
{
	QueryGroup *group = data;

	g_free(group->name);
	g_free(group->series);
	g_free(group->currency);
	g_ptr_array_unref(group->money);
	g_array_unref(group->numbers);
	g_free(group);
}

/* Everything a drawing needs, gathered from the rows. */
typedef struct
{
	GPtrArray	*labels;	/* gchar*: one per point */
	GHashTable	*label_index;	/* label -> index + 1 */
	GPtrArray	*groups;	/* QueryGroup* in first-seen order */
	GHashTable	*group_index;	/* "series\x1fcurrency" -> QueryGroup* */
} QueryPlot;

static void
query_plot_clear(QueryPlot *plot)
{
	g_clear_pointer(&plot->labels, g_ptr_array_unref);
	g_clear_pointer(&plot->label_index, g_hash_table_unref);
	g_clear_pointer(&plot->groups, g_ptr_array_unref);
	g_clear_pointer(&plot->group_index, g_hash_table_unref);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(QueryPlot, query_plot_clear)

gboolean
venture_web_query_chart_is_kind(const gchar *chart)
{
	return (0 == g_strcmp0(chart, "table")) || (0 == g_strcmp0(chart, "bar")) ||
	       (0 == g_strcmp0(chart, "line")) || (0 == g_strcmp0(chart, "pie"));
}

/* --- Cells ---------------------------------------------------------------- */

/* A money record from the document, or NULL when @node is not one. */
static VentureMoney *
query_cell_money(JsonNode *node)
{
	JsonObject *object;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	object = json_node_get_object(node);

	if (!json_object_has_member(object, "amount") ||
	    !json_object_has_member(object, "currency"))
		return NULL;

	return venture_money_from_json(node, NULL, NULL);
}

/* Whether @node is a non-empty list of money: sum_money()'s answer. */
static gboolean
query_cell_is_money_list(JsonNode *node)
{
	JsonArray *array;
	guint i;

	if ((NULL == node) || !JSON_NODE_HOLDS_ARRAY(node))
		return FALSE;

	array = json_node_get_array(node);

	for (i = 0; i < json_array_get_length(array); i++)
	{
		g_autoptr(VentureMoney) money = query_cell_money(json_array_get_element(array, i));

		if (NULL == money)
			return FALSE;
	}

	return TRUE;
}

/* A cell as words: for a label, a legend and a table. */
static gchar *
query_cell_text(JsonNode *node)
{
	g_autoptr(VentureMoney) money = NULL;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return g_strdup("(none)");

	if (JSON_NODE_HOLDS_VALUE(node))
	{
		if (G_TYPE_STRING == json_node_get_value_type(node))
			return g_strdup(json_node_get_string(node));

		if (G_TYPE_BOOLEAN == json_node_get_value_type(node))
			return g_strdup(json_node_get_boolean(node) ? "true" : "false");

		return venture_json_to_string(node, FALSE);
	}

	money = query_cell_money(node);

	if (NULL != money)
		return venture_money_to_display_string(money, TRUE);

	return venture_json_to_string(node, FALSE);
}

/* --- The columns ---------------------------------------------------------- */

/* Whether the document has a column @name. */
static gboolean
query_has_column(
	JsonArray	*columns,
	const gchar	*name
){
	guint i;

	for (i = 0; i < json_array_get_length(columns); i++)
	{
		if (0 == g_strcmp0(json_object_get_string_member(
			json_array_get_object_element(columns, i), "name"), name))
			return TRUE;
	}

	return FALSE;
}

/* "a, b, c": the columns there are, for a refusal. */
static gchar *
query_column_list(JsonArray *columns)
{
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;

	for (i = 0; i < json_array_get_length(columns); i++)
		g_string_append_printf(out, "%s%s", (i > 0) ? ", " : "",
			json_object_get_string_member(json_array_get_object_element(columns, i), "name"));

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* Whether a cell can be drawn: a number, money or a list of money. */
static gboolean
query_cell_is_drawable(JsonNode *node)
{
	g_autoptr(VentureMoney) money = NULL;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return FALSE;

	if (JSON_NODE_HOLDS_VALUE(node))
		return (G_TYPE_INT64 == json_node_get_value_type(node)) ||
		       (G_TYPE_DOUBLE == json_node_get_value_type(node));

	money = query_cell_money(node);

	return (NULL != money) || query_cell_is_money_list(node);
}

/*
 * The value column when none was picked: the first column, other than
 * the label and series columns, whose first non-null cell can be drawn.
 */
static const gchar *
query_default_value(
	JsonArray	*columns,
	JsonArray	*rows,
	const gchar	*x,
	const gchar	*series
){
	guint i;
	guint j;

	for (i = 0; i < json_array_get_length(columns); i++)
	{
		const gchar *name = json_object_get_string_member(
			json_array_get_object_element(columns, i), "name");

		if ((0 == g_strcmp0(name, x)) || (0 == g_strcmp0(name, series)))
			continue;

		for (j = 0; j < json_array_get_length(rows); j++)
		{
			JsonNode *cell = json_object_get_member(
				json_array_get_object_element(rows, j), name);

			if ((NULL == cell) || JSON_NODE_HOLDS_NULL(cell))
				continue;

			if (query_cell_is_drawable(cell))
				return name;

			break;
		}
	}

	return NULL;
}

/* --- The plot ------------------------------------------------------------- */

/* The index of @label, added at the end when it is new. */
static guint
query_plot_label(
	QueryPlot	*plot,
	const gchar	*label
){
	gpointer known = g_hash_table_lookup(plot->label_index, label);

	if (NULL != known)
		return GPOINTER_TO_UINT(known) - 1;

	g_ptr_array_add(plot->labels, g_strdup(label));
	g_hash_table_insert(plot->label_index, g_strdup(label),
	                    GUINT_TO_POINTER(plot->labels->len));

	return plot->labels->len - 1;
}

/* The group for (@series, @currency), made when it is new. */
static QueryGroup *
query_plot_group(
	QueryPlot	*plot,
	const gchar	*series,
	const gchar	*currency
){
	g_autofree gchar *key = NULL;
	QueryGroup *group;

	key = g_strdup_printf("%s\x1f%s", (NULL != series) ? series : "",
	                      (NULL != currency) ? currency : "");
	group = g_hash_table_lookup(plot->group_index, key);

	if (NULL != group)
		return group;

	group = g_new0(QueryGroup, 1);
	group->series = g_strdup(series);
	group->currency = g_strdup(currency);
	group->money = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	group->numbers = g_array_new(FALSE, FALSE, sizeof(gdouble));

	if ((NULL != series) && (NULL != currency))
		group->name = g_strdup_printf("%s (%s)", series, currency);
	else if (NULL != series)
		group->name = g_strdup(series);
	else if (NULL != currency)
		group->name = g_strdup(currency);
	else
		group->name = NULL;

	g_ptr_array_add(plot->groups, group);
	g_hash_table_insert(plot->group_index, g_steal_pointer(&key), group);

	return group;
}

/* Makes @group as long as the plot has labels, absent points NAN. */
static void
query_group_grow(
	QueryGroup	*group,
	guint		 n_points
){
	while (group->numbers->len < n_points)
	{
		gdouble absent = NAN;

		g_array_append_val(group->numbers, absent);
		g_ptr_array_add(group->money, NULL);
	}
}

/*
 * Adds @amount to @group at @point: within the group's one currency, so
 * two rows with the same label are one bar of their sum.
 */
static gboolean
query_group_add_money(
	QueryGroup		 *group,
	guint			  point,
	const VentureMoney	 *amount,
	GError			**error
){
	VentureMoney *held = g_ptr_array_index(group->money, point);
	VentureMoney *sum;

	if (NULL == held)
	{
		g_ptr_array_index(group->money, point) = venture_money_copy(amount);
		return TRUE;
	}

	sum = venture_money_add(held, amount, error);

	if (NULL == sum)
		return FALSE;

	venture_money_free(held);
	g_ptr_array_index(group->money, point) = sum;

	return TRUE;
}

static void
query_group_add_number(
	QueryGroup	*group,
	guint		 point,
	gdouble		 number
){
	gdouble *held = &g_array_index(group->numbers, gdouble, point);

	*held = isnan(*held) ? number : (*held + number);
}

/*
 * Reads every row into the plot: its label, its group and its value,
 * money kept in its currency and never added across two.
 */
static gboolean
query_plot_build(
	QueryPlot	 *plot,
	JsonArray	 *rows,
	const gchar	 *x,
	const gchar	 *series,
	const gchar	 *value,
	GError		**error
){
	guint i;
	guint g;

	plot->labels = g_ptr_array_new_with_free_func(g_free);
	plot->label_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	plot->groups = g_ptr_array_new_with_free_func(query_group_free);
	plot->group_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonNode *cell = json_object_get_member(row, value);
		g_autofree gchar *label = NULL;
		g_autofree gchar *series_text = NULL;
		g_autoptr(VentureMoney) money = NULL;
		guint point;

		label = query_cell_text(json_object_get_member(row, x));
		point = query_plot_label(plot, label);

		if (plot->labels->len > VENTURE_WEB_CHART_MAX_POINTS)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "A chart draws at most %d points along the bottom; this query "
			            "has more %s values. Summarize or limit it first",
			            VENTURE_WEB_CHART_MAX_POINTS, x);
			return FALSE;
		}

		if (NULL != series)
			series_text = query_cell_text(json_object_get_member(row, series));

		/* An empty cell is a gap, not a zero. */
		if ((NULL == cell) || JSON_NODE_HOLDS_NULL(cell))
			continue;

		if (JSON_NODE_HOLDS_VALUE(cell) &&
		    ((G_TYPE_INT64 == json_node_get_value_type(cell)) ||
		     (G_TYPE_DOUBLE == json_node_get_value_type(cell))))
		{
			QueryGroup *group = query_plot_group(plot, series_text, NULL);

			query_group_grow(group, plot->labels->len);
			query_group_add_number(group, point, json_node_get_double(cell));
			continue;
		}

		money = query_cell_money(cell);

		if (NULL != money)
		{
			QueryGroup *group = query_plot_group(plot, series_text,
			                                     venture_money_get_currency(money));

			query_group_grow(group, plot->labels->len);

			if (!query_group_add_money(group, point, money, error))
				return FALSE;

			continue;
		}

		if (query_cell_is_money_list(cell))
		{
			JsonArray *totals = json_node_get_array(cell);
			guint t;

			/* One total per currency: each its own group's point. */
			for (t = 0; t < json_array_get_length(totals); t++)
			{
				g_autoptr(VentureMoney) total = query_cell_money(json_array_get_element(totals, t));
				QueryGroup *group = query_plot_group(plot, series_text,
				                                     venture_money_get_currency(total));

				query_group_grow(group, plot->labels->len);

				if (!query_group_add_money(group, point, total, error))
					return FALSE;
			}

			continue;
		}

		{
			g_autofree gchar *text = query_cell_text(cell);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The %s column holds %s, which is neither a number nor money; "
			            "pick a value column that is (count(), sum(), sum_money())",
			            value, text);
			return FALSE;
		}
	}

	/* Every group as long as the labels, and money's points as numbers
	 * in the currency's own minor units, for the drawing. */
	for (g = 0; g < plot->groups->len; g++)
	{
		QueryGroup *group = g_ptr_array_index(plot->groups, g);
		guint p;

		query_group_grow(group, plot->labels->len);

		if (NULL == group->currency)
			continue;

		for (p = 0; p < group->money->len; p++)
		{
			VentureMoney *amount = g_ptr_array_index(group->money, p);
			g_autoptr(VentureMoney) natural = NULL;

			if (NULL == amount)
				continue;

			natural = venture_money_rescale(amount,
				(guint8)venture_currency_get_exponent(group->currency), error);

			if (NULL == natural)
				return FALSE;

			g_array_index(group->numbers, gdouble, p) =
				(gdouble)venture_money_get_amount(natural);
		}
	}

	return TRUE;
}

/* --- Drawing -------------------------------------------------------------- */

/* "n by assignee", and the group when there is more than one drawing. */
static gchar *
query_chart_title(
	const gchar	*value,
	const gchar	*x,
	const gchar	*group
){
	if (NULL != group)
		return g_strdup_printf("%s by %s \xe2\x80\x94 %s", value, x, group);

	return g_strdup_printf("%s by %s", value, x);
}

/* One chart's series from one group. */
static void
query_chart_series(
	VentureWebChartSeries	*series,
	QueryGroup		*group,
	const gchar		*value
){
	memset(series, 0, sizeof(*series));
	series->name = (NULL != group->name) ? group->name : value;
	series->values = (const gdouble *)group->numbers->data;
	series->format = (NULL != group->currency) ? venture_web_chart_format_minor
	                                           : venture_web_chart_format_number;
	series->format_data = group->currency;
	series->currency = group->currency;
}

/* Bars or a pie: one drawing per group. */
static gboolean
query_chart_per_group(
	GString		 *html,
	QueryPlot	 *plot,
	const gchar	 *chart,
	const gchar	 *x,
	const gchar	 *value,
	GError		**error
){
	gboolean pie = (0 == g_strcmp0(chart, "pie"));
	guint g;

	if (plot->groups->len > VENTURE_WEB_QUERY_CHART_MAX_CHARTS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "This would be %u %s charts, one per series and currency; at most %d "
		            "are drawn. Filter the query, or pick another series column",
		            plot->groups->len, chart, VENTURE_WEB_QUERY_CHART_MAX_CHARTS);
		return FALSE;
	}

	if (pie && (plot->labels->len > VENTURE_WEB_CHART_MAX_SLICES))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "A pie draws at most %d slices and this query has %u %s values; "
		            "use bars, or summarize into fewer",
		            VENTURE_WEB_CHART_MAX_SLICES, plot->labels->len, x);
		return FALSE;
	}

	for (g = 0; g < plot->groups->len; g++)
	{
		QueryGroup *group = g_ptr_array_index(plot->groups, g);
		g_autofree gchar *title = NULL;
		g_autofree gchar *drawing = NULL;
		VentureWebChartSeries series;
		VentureWebChart picture;
		guint p;

		if (pie)
		{
			for (p = 0; p < group->numbers->len; p++)
			{
				if (g_array_index(group->numbers, gdouble, p) < 0.0)
				{
					g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					            "A pie shows shares of a whole, and %s is negative for %s; "
					            "use bars", value,
					            (const gchar *)g_ptr_array_index(plot->labels, p));
					return FALSE;
				}
			}
		}

		title = query_chart_title(value, x, (NULL != group->name) ? group->name : NULL);
		query_chart_series(&series, group, value);
		memset(&picture, 0, sizeof(picture));
		picture.title = title;
		picture.labels = (const gchar *const *)plot->labels->pdata;
		picture.n_points = plot->labels->len;
		picture.series = &series;
		picture.n_series = 1;

		drawing = pie ? venture_web_chart_pie(&picture) : venture_web_chart_bar(&picture);
		g_string_append(html, drawing);
	}

	return TRUE;
}

/* A line: one drawing per currency (numbers are one), its groups the lines. */
static gboolean
query_chart_lines(
	GString		 *html,
	QueryPlot	 *plot,
	const gchar	 *x,
	const gchar	 *value,
	GError		**error
){
	g_autoptr(GPtrArray) units = g_ptr_array_new_with_free_func(g_free);
	guint u;
	guint g;

	/* The currencies, in the order their groups appeared; "" for numbers. */
	for (g = 0; g < plot->groups->len; g++)
	{
		QueryGroup *group = g_ptr_array_index(plot->groups, g);
		const gchar *unit = (NULL != group->currency) ? group->currency : "";

		if (!g_ptr_array_find_with_equal_func(units, unit, g_str_equal, NULL))
			g_ptr_array_add(units, g_strdup(unit));
	}

	if (units->len > VENTURE_WEB_QUERY_CHART_MAX_CHARTS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "This would be %u line charts, one per currency; at most %d are drawn",
		            units->len, VENTURE_WEB_QUERY_CHART_MAX_CHARTS);
		return FALSE;
	}

	for (u = 0; u < units->len; u++)
	{
		const gchar *unit = g_ptr_array_index(units, u);
		VentureWebChartSeries lines[VENTURE_WEB_CHART_MAX_SERIES];
		g_autofree gchar *title = NULL;
		g_autofree gchar *drawing = NULL;
		VentureWebChart picture;
		gsize n_lines = 0;

		for (g = 0; g < plot->groups->len; g++)
		{
			QueryGroup *group = g_ptr_array_index(plot->groups, g);

			if (0 != g_strcmp0((NULL != group->currency) ? group->currency : "", unit))
				continue;

			if (n_lines == VENTURE_WEB_CHART_MAX_SERIES)
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "A line chart draws at most %d series and this query has more; "
				            "pick another series column, or filter it",
				            VENTURE_WEB_CHART_MAX_SERIES);
				return FALSE;
			}

			query_chart_series(&lines[n_lines], group, value);

			/* One currency per drawing: the legend names the series
			 * value, the title the currency. */
			if ((NULL != group->series) && (NULL != group->currency))
				lines[n_lines].name = group->series;

			n_lines++;
		}

		title = query_chart_title(value, x, ('\0' != unit[0]) ? unit : NULL);
		memset(&picture, 0, sizeof(picture));
		picture.title = title;
		picture.labels = (const gchar *const *)plot->labels->pdata;
		picture.n_points = plot->labels->len;
		picture.series = lines;
		picture.n_series = n_lines;

		drawing = venture_web_chart_line(&picture);
		g_string_append(html, drawing);
	}

	return TRUE;
}

/* The JSON beside the drawing, from the same groups. */
static JsonNode *
query_chart_data(
	QueryPlot	*plot,
	const gchar	*chart,
	const gchar	*x,
	const gchar	*series,
	const gchar	*value
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	guint g;
	guint p;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "chart");
	json_builder_add_string_value(builder, chart);
	json_builder_set_member_name(builder, "x");
	json_builder_add_string_value(builder, x);
	json_builder_set_member_name(builder, "series");

	if (NULL != series)
		json_builder_add_string_value(builder, series);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "value");
	json_builder_add_string_value(builder, value);

	json_builder_set_member_name(builder, "labels");
	json_builder_begin_array(builder);

	for (p = 0; p < plot->labels->len; p++)
		json_builder_add_string_value(builder, g_ptr_array_index(plot->labels, p));

	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "groups");
	json_builder_begin_array(builder);

	for (g = 0; g < plot->groups->len; g++)
	{
		QueryGroup *group = g_ptr_array_index(plot->groups, g);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");

		if (NULL != group->name)
			json_builder_add_string_value(builder, group->name);
		else
			json_builder_add_string_value(builder, value);

		json_builder_set_member_name(builder, "series");

		if (NULL != group->series)
			json_builder_add_string_value(builder, group->series);
		else
			json_builder_add_null_value(builder);

		json_builder_set_member_name(builder, "currency");

		if (NULL != group->currency)
			json_builder_add_string_value(builder, group->currency);
		else
			json_builder_add_null_value(builder);

		json_builder_set_member_name(builder, "values");
		json_builder_begin_array(builder);

		for (p = 0; p < plot->labels->len; p++)
		{
			VentureMoney *amount = g_ptr_array_index(group->money, p);
			gdouble number = g_array_index(group->numbers, gdouble, p);

			if (NULL != amount)
				json_builder_add_value(builder, venture_money_to_json(amount));
			else if (!isfinite(number))
				json_builder_add_null_value(builder);
			else
				json_builder_add_double_value(builder, number);
		}

		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* --- The table ------------------------------------------------------------ */

/*
 * Every column of every row. Money reads as its currency writes it, and a
 * list of money -- one total per currency -- is one line per currency,
 * never one figure.
 */
static void
query_chart_table(
	GString		*html,
	JsonArray	*columns,
	JsonArray	*rows
){
	guint i;
	guint j;

	if (0 == json_array_get_length(rows))
	{
		g_string_append(html, "<p class=\"muted\">No rows.</p>");
		return;
	}

	g_string_append(html, "<div class=\"table-wrap\"><table class=\"table query-table\"><thead><tr>");

	for (i = 0; i < json_array_get_length(columns); i++)
	{
		g_string_append(html, "<th scope=\"col\">");
		venture_html_escape_append(html, json_object_get_string_member(
			json_array_get_object_element(columns, i), "name"));
		g_string_append(html, "</th>");
	}

	g_string_append(html, "</tr></thead><tbody>");

	for (j = 0; j < json_array_get_length(rows); j++)
	{
		JsonObject *row = json_array_get_object_element(rows, j);

		g_string_append(html, "<tr>");

		for (i = 0; i < json_array_get_length(columns); i++)
		{
			const gchar *name = json_object_get_string_member(
				json_array_get_object_element(columns, i), "name");
			JsonNode *cell = json_object_get_member(row, name);

			g_string_append(html, "<td>");

			if ((NULL == cell) || JSON_NODE_HOLDS_NULL(cell))
			{
				g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");
			}
			else if (query_cell_is_money_list(cell))
			{
				JsonArray *totals = json_node_get_array(cell);
				guint t;

				if (0 == json_array_get_length(totals))
					g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");

				for (t = 0; t < json_array_get_length(totals); t++)
				{
					g_autoptr(VentureMoney) total = query_cell_money(json_array_get_element(totals, t));
					g_autofree gchar *text = venture_money_to_display_string(total, TRUE);

					g_string_append(html, "<span class=\"figure-line\" data-currency=\"");
					venture_html_escape_append(html, venture_money_get_currency(total));
					g_string_append(html, "\">");
					venture_html_escape_append(html, text);
					g_string_append(html, "</span>");

					if (t + 1 < json_array_get_length(totals))
						g_string_append(html, "<br>");
				}
			}
			else
			{
				g_autofree gchar *text = query_cell_text(cell);

				venture_html_escape_append(html, text);
			}

			g_string_append(html, "</td>");
		}

		g_string_append(html, "</tr>");
	}

	g_string_append(html, "</tbody></table></div>");
}

/* --- The entry point ------------------------------------------------------ */

gchar *
venture_web_query_chart(
	JsonNode	 *document,
	const gchar	 *chart,
	const gchar	 *x,
	const gchar	 *series,
	const gchar	 *value,
	JsonNode	**out_data,
	GError		**error
){
	g_auto(QueryPlot) plot = { NULL, NULL, NULL, NULL };
	g_autoptr(GString) html = NULL;
	g_autofree gchar *known = NULL;
	JsonObject *root;
	JsonArray *columns;
	JsonArray *rows;

	g_return_val_if_fail(NULL != document, NULL);

	if (NULL != out_data)
		*out_data = NULL;

	if (!JSON_NODE_HOLDS_OBJECT(document) ||
	    !json_object_has_member(json_node_get_object(document), "columns") ||
	    !json_object_has_member(json_node_get_object(document), "rows"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Not a query result: it has no columns and rows");
		return NULL;
	}

	if (venture_string_is_empty(chart))
		chart = "table";

	if (!venture_web_query_chart_is_kind(chart))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "\"%s\" is not a chart; use table, bar, line or pie", chart);
		return NULL;
	}

	root = json_node_get_object(document);
	columns = json_object_get_array_member(root, "columns");
	rows = json_object_get_array_member(root, "rows");
	known = query_column_list(columns);
	html = g_string_new(NULL);

	/* --- a table shows everything ------------------------------------- */
	if (0 == g_strcmp0(chart, "table"))
	{
		query_chart_table(html, columns, rows);

		if (NULL != out_data)
		{
			g_autoptr(JsonBuilder) builder = json_builder_new();

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "chart");
			json_builder_add_string_value(builder, "table");
			json_builder_end_object(builder);
			*out_data = json_builder_get_root(builder);
		}

		return g_string_free(g_steal_pointer(&html), FALSE);
	}

	/* --- the picks ------------------------------------------------------ */
	if (0 == json_array_get_length(columns))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The query has no columns to chart");
		return NULL;
	}

	if (venture_string_is_empty(x))
		x = json_object_get_string_member(json_array_get_object_element(columns, 0), "name");

	if (venture_string_is_empty(series))
		series = NULL;

	if (venture_string_is_empty(value))
		value = query_default_value(columns, rows, x, series);

	if (!query_has_column(columns, x) ||
	    ((NULL != series) && !query_has_column(columns, series)) ||
	    ((NULL != value) && !query_has_column(columns, value)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "The query has no column called \"%s\"; its columns are %s",
		            !query_has_column(columns, x) ? x
		            : ((NULL != series) && !query_has_column(columns, series)) ? series
		            : value, known);
		return NULL;
	}

	if (NULL == value)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "The query has no number or money column to draw; its columns are %s. "
		            "Summarize it (n = count(), total = sum_money(...))", known);
		return NULL;
	}

	if (!query_plot_build(&plot, rows, x, series, value, error))
		return NULL;

	/* --- the drawing ----------------------------------------------------- */
	if (0 == plot.groups->len)
	{
		g_string_append(html, "<p class=\"muted\">Nothing to draw.</p>");
	}
	else
	{
		gboolean drawn;

		if (plot.groups->len > 1)
			g_string_append(html, "<div class=\"query-charts\">");

		drawn = (0 == g_strcmp0(chart, "line"))
			? query_chart_lines(html, &plot, x, value, error)
			: query_chart_per_group(html, &plot, chart, x, value, error);

		if (!drawn)
			return NULL;

		if (plot.groups->len > 1)
			g_string_append(html, "</div>");
	}

	if (NULL != out_data)
		*out_data = query_chart_data(&plot, chart, x, series, value);

	return g_string_free(g_steal_pointer(&html), FALSE);
}
