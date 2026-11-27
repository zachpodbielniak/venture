/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-web-list-chart.inc - The Chart toggle on list pages
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * Included by venture-web-server.c, after venture-web-query.inc and the
 * list helpers, ahead of venture_web_ui_list(). Every generic list,
 * /e/<type>, and the ticket page, /tickets, can draw the records it lists
 * as a chart instead of a table (docs/lql.org, "Charts on list pages"):
 *
 *   /e/ticket?view=chart&chart_by=status&chart_value=count&chart_kind=bar
 *
 * The chart is the page's own query plus a summarize: the URL's field
 * filters as the LQL they mean (venture_lql_filter_from_query_string()),
 * then the bar's q, then `summarize n = count()` -- and, for a money
 * field, `total = sum_money(FIELD)` -- by the field picked. It runs as the
 * list's caller, in the environment the bar runs in, through the one
 * off-loop helper (venture_web_query_run(), venture-web-query.inc), so
 * query.max_rows, query.max_bytes and query.timeout_ms hold, the main loop
 * stays free, and a problem is drawn in the bar as the list draws one.
 * venture_web_query_chart() draws the rows: money is one series per
 * currency, never summed across.
 *
 * The parameters are chart_by, chart_value and chart_kind rather than a
 * bare by, value and kind: those three are field names on real record
 * types (a ticket's kind, a sales credit's value, a pipeline entry's by),
 * and a list reads every other parameter as a field filter.
 */

/* The page parameter value that asks for a chart: ?view=chart. */
#define VENTURE_WEB_LIST_CHART_VIEW "chart"

/* The chart's own controls; never a field filter. */
static const gchar *const venture_web_list_chart_controls[] = {
	"chart_by", "chart_value", "chart_kind", NULL
};

/* The date buckets a date field can be grouped by: the chart_by suffix,
 * its words, and the strftime pattern the bucket is labelled with. */
static const struct
{
	const gchar	*name;
	const gchar	*words;
	const gchar	*format;
	gboolean	 week;
} venture_web_list_chart_buckets[] = {
	{ "day",   "by day",   "%Y-%m-%d", FALSE },
	{ "week",  "by week",  "%Y-%m-%d", TRUE  },
	{ "month", "by month", "%Y-%m",    FALSE },
	{ "year",  "by year",  "%Y",       FALSE }
};

/* Whether @request asks for its list drawn as a chart (?view=chart). */
static gboolean
venture_web_list_wants_chart(HtmxRequest *request)
{
	return 0 == g_strcmp0(htmx_request_get_query_param(request, "view"),
	                      VENTURE_WEB_LIST_CHART_VIEW);
}

/* Whether @name is one of the chart's own controls (chart_by, ...). */
static gboolean
venture_web_list_chart_control(const gchar *name)
{
	return g_strv_contains(venture_web_list_chart_controls, name);
}

/*
 * The Table and Chart links of a list page, as query strings ("?..." or
 * "") HTML-escaped for an attribute: the page's parameters in name order
 * without paging, the table's without the chart's, the chart's with
 * view=chart and whatever chart_* it already has.
 */
static void
venture_web_list_chart_toggle_hrefs(
	HtmxRequest	 *request,
	gchar		**out_table,
	gchar		**out_chart
){
	g_autoptr(GString) table = NULL;
	g_autoptr(GString) chart = NULL;
	GHashTable *params;
	GList *keys;
	GList *item;

	table = g_string_new(NULL);
	chart = g_string_new(NULL);
	params = htmx_request_get_query_params(request);
	keys = (NULL != params) ? g_list_sort(g_hash_table_get_keys(params), (GCompareFunc)g_strcmp0)
	                        : NULL;

	for (item = keys; NULL != item; item = item->next)
	{
		const gchar *name = item->data;
		g_autofree gchar *pair = NULL;
		g_autofree gchar *escaped_name = NULL;
		g_autofree gchar *escaped_value = NULL;

		if ((0 == g_strcmp0(name, "page")) || (0 == g_strcmp0(name, "offset")) ||
		    (0 == g_strcmp0(name, "view")))
			continue;

		escaped_name = g_uri_escape_string(name, NULL, FALSE);
		escaped_value = g_uri_escape_string(g_hash_table_lookup(params, name), NULL, FALSE);
		pair = g_strdup_printf("%s=%s", escaped_name, escaped_value);

		if (!venture_web_list_chart_control(name))
			g_string_append_printf(table, "%c%s", (0 == table->len) ? '?' : '&', pair);

		g_string_append_printf(chart, "%c%s", (0 == chart->len) ? '?' : '&', pair);
	}

	g_list_free(keys);
	g_string_append_printf(chart, "%cview=" VENTURE_WEB_LIST_CHART_VIEW,
	                       (0 == chart->len) ? '?' : '&');

	*out_table = venture_html_escape(table->str);
	*out_chart = venture_html_escape(chart->str);
}

/* --- What a chart can be grouped by and sum ------------------------------ */

/*
 * One way to split a list's records: what chart_by= says ("status",
 * "created_at:month"), what the picker says ("Created by month"), the
 * summarize key's expression and the result column it names.
 */
typedef struct
{
	gchar		*key;
	gchar		*label;
	gchar		*expression;
	gchar		*column;
	gboolean	 chronological;
} VentureWebChartGroup;

static void
venture_web_chart_group_free(VentureWebChartGroup *group)
{
	if (NULL == group)
		return;

	g_free(group->key);
	g_free(group->label);
	g_free(group->expression);
	g_free(group->column);
	g_free(group);
}

/* A money field a chart can total: chart_value=sum:COLUMN. */
typedef struct
{
	gchar	*column;
	gchar	*label;
} VentureWebChartSum;

static void
venture_web_chart_sum_free(VentureWebChartSum *sum)
{
	if (NULL == sum)
		return;

	g_free(sum->column);
	g_free(sum->label);
	g_free(sum);
}

/* Adds the plain grouping by @column, labelled @label. */
static void
venture_web_chart_groups_add(
	GPtrArray	*groups,
	const gchar	*column,
	const gchar	*label
){
	VentureWebChartGroup *group;

	group = g_new0(VentureWebChartGroup, 1);
	group->key = g_strdup(column);
	group->label = g_strdup(label);
	group->expression = g_strdup(column);
	group->column = g_strdup(column);
	g_ptr_array_add(groups, group);
}

/*
 * Adds one grouping per date bucket of the timestamp @column. A record
 * with no date is its own group, null, rather than an error: the key is
 * only formatted when there is a date to format.
 */
static void
venture_web_chart_groups_add_dates(
	GPtrArray	*groups,
	const gchar	*column,
	const gchar	*label
){
	g_autofree gchar *base = NULL;
	gsize i;

	/* "Issued at" reads "Issued by month". */
	base = g_strdup(label);

	if (g_str_has_suffix(base, " at"))
		base[strlen(base) - 3] = '\0';

	for (i = 0; i < G_N_ELEMENTS(venture_web_list_chart_buckets); i++)
	{
		VentureWebChartGroup *group;
		g_autofree gchar *moment = NULL;

		moment = venture_web_list_chart_buckets[i].week
			? g_strdup_printf("start_of_week(%s)", column)
			: g_strdup(column);
		group = g_new0(VentureWebChartGroup, 1);
		group->key = g_strdup_printf("%s:%s", column, venture_web_list_chart_buckets[i].name);
		group->label = g_strdup_printf("%s %s", base, venture_web_list_chart_buckets[i].words);
		group->expression = g_strdup_printf(
			"if(is_null(%s), null, format_timestamp(%s, \"%s\"))",
			column, moment, venture_web_list_chart_buckets[i].format);
		group->column = g_strdup(column);
		group->chronological = TRUE;
		g_ptr_array_add(groups, group);
	}
}

/*
 * What a chart of @entity_type can be grouped by, worked out from its
 * field table as the bar's chips are (venture_lql_filter_chips()): every
 * enumeration (the lifecycle first), the assigned username, every
 * reference that is not machinery, and each date that is not machinery
 * by day, week, month and year, and created_at always, last. Only fields the record really has: a declaration with
 * no property behind it is no column LQL can read.
 */
static GPtrArray *
venture_web_chart_groups(GType entity_type)
{
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	GObjectClass *klass;
	GPtrArray *groups;
	gboolean dated = FALSE;
	guint pass;
	guint i;

	groups = g_ptr_array_new_with_free_func((GDestroyNotify)venture_web_chart_group_free);
	prototype = g_object_new(entity_type, NULL);
	specs = venture_entity_get_field_specs(prototype);
	klass = G_OBJECT_GET_CLASS(prototype);

	/* Two passes, so the lifecycle enumeration leads the picker and is
	 * the default: pass 0 takes it alone, pass 1 everything else. */
	for (pass = 0; pass < 2; pass++)
	{
		for (i = 0; i < specs->len; i++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(specs, i);
			VentureFieldKind kind = venture_field_spec_get_kind(spec);
			VentureFieldRole role = venture_field_spec_get_role(spec);
			const gchar *name = venture_field_spec_get_name(spec);
			const gchar *label = venture_field_spec_get_label(spec);
			g_autofree gchar *column = NULL;
			GParamSpec *pspec;
			gboolean lifecycle;

			pspec = g_object_class_find_property(klass, name);

			if ((NULL == pspec) || (VENTURE_FIELD_ROLE_TECHNICAL == role))
				continue;

			lifecycle = (VENTURE_FIELD_KIND_ENUM == kind) && (VENTURE_FIELD_ROLE_STATUS == role);

			if ((0 == pass) != lifecycle)
				continue;

			column = venture_entity_property_to_column(name);

			switch (kind)
			{
			case VENTURE_FIELD_KIND_ENUM:
			case VENTURE_FIELD_KIND_REFERENCE:
				venture_web_chart_groups_add(groups, column, label);
				break;

			case VENTURE_FIELD_KIND_STRING:
				if (0 != (venture_field_spec_get_flags(spec) &
				          VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME))
					venture_web_chart_groups_add(groups, column, label);
				break;

			case VENTURE_FIELD_KIND_DATE:
			case VENTURE_FIELD_KIND_DATETIME:
				/* A timestamp property, or LQL has no Timestamp to
				 * format. */
				if (G_TYPE_DATE_TIME == pspec->value_type)
				{
					venture_web_chart_groups_add_dates(groups, column, label);
					dated = dated || (0 == g_strcmp0(column, "created_at"));
				}
				break;

			default:
				break;
			}
		}
	}

	/* When it was made, for every type: its own dates can all be empty. */
	if (!dated)
	{
		GParamSpec *pspec = g_object_class_find_property(klass, "created-at");

		if ((NULL != pspec) && (G_TYPE_DATE_TIME == pspec->value_type))
			venture_web_chart_groups_add_dates(groups, "created_at", "Created");
	}

	return groups;
}

/* The money fields a chart of @entity_type can total, in table order. */
static GPtrArray *
venture_web_chart_sums(GType entity_type)
{
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	GObjectClass *klass;
	GPtrArray *sums;
	guint i;

	sums = g_ptr_array_new_with_free_func((GDestroyNotify)venture_web_chart_sum_free);
	prototype = g_object_new(entity_type, NULL);
	specs = venture_entity_get_field_specs(prototype);
	klass = G_OBJECT_GET_CLASS(prototype);

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		VentureWebChartSum *sum;

		if ((VENTURE_FIELD_KIND_MONEY != venture_field_spec_get_kind(spec)) ||
		    (VENTURE_FIELD_ROLE_TECHNICAL == venture_field_spec_get_role(spec)) ||
		    (NULL == g_object_class_find_property(klass, venture_field_spec_get_name(spec))))
			continue;

		sum = g_new0(VentureWebChartSum, 1);
		sum->column = venture_entity_property_to_column(venture_field_spec_get_name(spec));
		sum->label = g_strdup(venture_field_spec_get_label(spec));
		g_ptr_array_add(sums, sum);
	}

	return sums;
}

/* --- Problems -------------------------------------------------------------- */

/* A problem with the chart as a whole, drawn under the bar with no place
 * in its text. Takes @message. */
static VentureLqlProblem *
venture_web_chart_problem_take(
	VentureError	 code,
	gchar		*message
){
	VentureLqlProblem *problem;

	problem = g_new0(VentureLqlProblem, 1);
	problem->message = message;
	problem->code = code;

	return problem;
}

/* @error as a bar problem: its own class when VENTURE's (a bound is
 * VALIDATION, the time limit TIMEOUT), INVALID_ARGUMENT otherwise. */
static VentureLqlProblem *
venture_web_chart_problem_from_error(const GError *error)
{
	return venture_web_chart_problem_take(
		(VENTURE_ERROR == error->domain) ? (VentureError)error->code
		                                 : VENTURE_ERROR_INVALID_ARGUMENT,
		g_strdup(error->message));
}

/* --- The URL's filters, as LQL --------------------------------------------- */

/*
 * The field filters of a generic list's URL as one LQL filter, for the
 * chart to run ahead of the bar's q. Every parameter the list reads as a
 * filter must have an LQL meaning, or the chart would count records the
 * table does not show: a free-text search, a period, or a filter LQL
 * cannot say (a money threshold) is refused with a problem that says to
 * write it in the bar instead. @params are the list's filters as
 * venture_web_list_url_filters() leaves them.
 *
 * Returns: (transfer full) (nullable): the LQL, %NULL with no filter or
 *   with *out_problem set
 */
static gchar *
venture_web_list_chart_url_filter(
	GType			  entity_type,
	GHashTable		 *params,
	VentureLqlProblem	**out_problem
){
	static const gchar *const not_filters[] = {
		"limit", "offset", "page", "order", "include_deleted", "view", "list_filters",
		NULL
	};
	static const gchar *const unsaid[] = {
		"search", "period", "period_field", NULL
	};
	g_autoptr(GList) keys = NULL;
	GList *item;

	*out_problem = NULL;

	if ((NULL == params) || (0 == g_hash_table_size(params)))
		return NULL;

	keys = g_list_sort(g_hash_table_get_keys(params), (GCompareFunc)g_strcmp0);

	for (item = keys; NULL != item; item = item->next)
	{
		const gchar *name = item->data;
		const gchar *value = g_hash_table_lookup(params, name);
		g_autoptr(GHashTable) one = NULL;
		g_autofree gchar *term = NULL;

		if (g_strv_contains(not_filters, name) || venture_web_list_chart_control(name))
			continue;

		if (g_strv_contains(unsaid, name))
		{
			if (venture_string_is_empty(value))
				continue;

			*out_problem = venture_web_chart_problem_take(VENTURE_ERROR_INVALID_ARGUMENT,
				g_strdup_printf("A chart cannot apply %s=%s; write it in the bar as an "
				                "LQL filter", name, value));
			return NULL;
		}

		one = g_hash_table_new(g_str_hash, g_str_equal);
		g_hash_table_insert(one, (gpointer)name, (gpointer)value);
		term = venture_lql_filter_from_query_string(entity_type, one);

		if (NULL == term)
		{
			*out_problem = venture_web_chart_problem_take(VENTURE_ERROR_INVALID_ARGUMENT,
				g_strdup_printf("A chart cannot apply the filter %s=%s; write it in the "
				                "bar as an LQL filter", name, value));
			return NULL;
		}
	}

	return venture_lql_filter_from_query_string(entity_type, params);
}

/* --- Checking the bar's text, off the loop --------------------------------- */

/* venture_lql_filter_check() as query-worker work: preparing evaluates
 * the person's operands, so it is worker work like the bar's own check. */
typedef struct
{
	LqlEnvironment		*environment;
	const gchar		*type_name;
	const gchar		*text;
	VentureLqlProblem	*problem;
} VentureWebChartCheck;

static void
venture_web_chart_check_work(gpointer data)
{
	VentureWebChartCheck *check = data;

	check->problem = venture_lql_filter_check(check->environment, check->type_name, check->text);
}

/* --- Running and drawing ----------------------------------------------------- */

/* @text with every '<' written as its JSON escape: JSON safe inside a
 * <script> element, which ends at the first "</". */
static void
venture_web_chart_append_json(
	GString		*html,
	JsonNode	*node
){
	g_autofree gchar *text = NULL;
	const gchar *p;

	text = venture_json_to_string(node, FALSE);

	for (p = text; '\0' != *p; p++)
	{
		if ('<' == *p)
			g_string_append(html, "\\u003c");
		else
			g_string_append_c(html, *p);
	}
}

/* An <option> of a chart control: @value, @label, selected when @current. */
static void
venture_web_chart_append_option(
	GString		*html,
	const gchar	*value,
	const gchar	*label,
	const gchar	*current
){
	g_string_append(html, "<option value=\"");
	venture_html_escape_append(html, value);
	g_string_append_c(html, '"');

	if (0 == g_strcmp0(value, current))
		g_string_append(html, " selected");

	g_string_append_c(html, '>');
	venture_html_escape_append(html, label);
	g_string_append(html, "</option>");
}

/*
 * The chart's controls: a plain GET form to @action, so it works with
 * scripting off. It carries every other parameter of the page -- the
 * bar's q, the field filters, the view -- and picks what to split by,
 * what to draw and how.
 */
static void
venture_web_chart_append_controls(
	GString		*html,
	HtmxRequest	*request,
	const gchar	*action,
	GPtrArray	*groups,
	GPtrArray	*sums,
	const gchar	*by,
	const gchar	*value,
	const gchar	*kind
){
	static const gchar *const kinds[] = { "bar", "table", "line", "pie", NULL };
	static const gchar *const kind_labels[] = { "Bars", "Table", "Line", "Pie", NULL };
	GHashTable *params;
	GList *keys;
	GList *item;
	guint i;

	g_string_append(html, "<form class=\"chart-controls\" method=\"get\" action=\"");
	venture_html_escape_append(html, action);
	g_string_append(html, "\" aria-label=\"Chart\">");

	/* The page's other parameters, in name order, so the form is the
	 * same for the same page; view=chart among them. */
	params = htmx_request_get_query_params(request);
	keys = (NULL != params) ? g_list_sort(g_hash_table_get_keys(params), (GCompareFunc)g_strcmp0)
	                        : NULL;

	for (item = keys; NULL != item; item = item->next)
	{
		const gchar *name = item->data;

		if (venture_web_list_chart_control(name) || (0 == g_strcmp0(name, "page")) ||
		    (0 == g_strcmp0(name, "offset")))
			continue;

		g_string_append(html, "<input type=\"hidden\" name=\"");
		venture_html_escape_append(html, name);
		g_string_append(html, "\" value=\"");
		venture_html_escape_append(html, g_hash_table_lookup(params, name));
		g_string_append(html, "\">");
	}

	g_list_free(keys);

	/* A list reached without view=chart in its parameters (never, from
	 * the toggle) still comes back as a chart. */
	if (!venture_web_list_wants_chart(request))
		g_string_append(html, "<input type=\"hidden\" name=\"view\" value=\"chart\">");

	g_string_append(html, "<label>Split by <select name=\"chart_by\">");

	for (i = 0; i < groups->len; i++)
	{
		const VentureWebChartGroup *group = g_ptr_array_index(groups, i);

		venture_web_chart_append_option(html, group->key, group->label, by);
	}

	g_string_append(html, "</select></label> <label>Show <select name=\"chart_value\">");
	venture_web_chart_append_option(html, "count", "How many", value);

	for (i = 0; i < sums->len; i++)
	{
		const VentureWebChartSum *sum = g_ptr_array_index(sums, i);
		g_autofree gchar *option = g_strdup_printf("sum:%s", sum->column);
		g_autofree gchar *words = g_strdup_printf("Total %s", sum->label);

		venture_web_chart_append_option(html, option, words, value);
	}

	g_string_append(html, "</select></label> <label>As <select name=\"chart_kind\">");

	for (i = 0; NULL != kinds[i]; i++)
		venture_web_chart_append_option(html, kinds[i], kind_labels[i], kind);

	g_string_append(html, "</select></label> "
	                      "<button class=\"btn\" type=\"submit\">Draw</button></form>");
}

/*
 * Draws a list's records as a chart into @html: the controls, the chart
 * and what was drawn as JSON (`<script type="application/json"
 * data-chart-data>`, venture_web_query_chart()'s @out_data, for a script
 * or a test to read).
 *
 * @url_filter is the page's URL filters as LQL (or %NULL), run ahead of
 * the bar's @q. With @check, @q is checked first, off the loop, so a
 * problem is placed in the text the bar shows; /tickets has checked it
 * already. *out_total is how many records were counted.
 *
 * Returns: %TRUE when drawn; %FALSE with *out_problem set, for the bar
 */
static gboolean
venture_web_list_chart_draw(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	LqlEnvironment		 *environment,
	GType			  entity_type,
	const gchar		 *type_name,
	const gchar		 *action,
	const gchar		 *url_filter,
	const gchar		 *q,
	gboolean		  check,
	GString			 *html,
	gint64			 *out_total,
	VentureLqlProblem	**out_problem
){
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(GPtrArray) sums = NULL;
	g_autoptr(GString) text = NULL;
	g_autoptr(JsonNode) document = NULL;
	g_autoptr(JsonNode) drawn = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *chart = NULL;
	const VentureWebChartGroup *group = NULL;
	const VentureWebChartSum *sum = NULL;
	const gchar *by;
	const gchar *value;
	const gchar *kind;
	JsonArray *rows;
	gint64 total = 0;
	guint i;

	*out_total = 0;
	*out_problem = NULL;

	by = htmx_request_get_query_param(request, "chart_by");
	value = htmx_request_get_query_param(request, "chart_value");
	kind = htmx_request_get_query_param(request, "chart_kind");

	/* --- UTF-8, before anything reaches LQL ---------------------------- */
	/* The picks, the URL's filters as LQL (their values are quoted into
	 * it as they came) and the bar's q all end up in the query text, and
	 * LQL's lexer reads UTF-8 alone (venture_lql_text_check_utf8()). */
	{
		const gchar *texts[5];
		gsize t;

		texts[0] = by;
		texts[1] = value;
		texts[2] = kind;
		texts[3] = url_filter;
		texts[4] = q;

		for (t = 0; t < G_N_ELEMENTS(texts); t++)
		{
			if (!venture_lql_text_check_utf8(texts[t], &error))
			{
				*out_problem = venture_web_chart_problem_from_error(error);
				return FALSE;
			}
		}
	}

	/* --- the picks ------------------------------------------------------ */
	groups = venture_web_chart_groups(entity_type);
	sums = venture_web_chart_sums(entity_type);

	if (venture_string_is_empty(value))
		value = "count";

	if (venture_string_is_empty(kind))
		kind = "bar";

	if (0 == groups->len)
	{
		*out_problem = venture_web_chart_problem_take(VENTURE_ERROR_INVALID_ARGUMENT,
			g_strdup("These records have no field a chart can split them by"));
		return FALSE;
	}

	if (venture_string_is_empty(by))
		group = g_ptr_array_index(groups, 0);

	for (i = 0; (NULL == group) && (i < groups->len); i++)
		if (0 == g_strcmp0(((const VentureWebChartGroup *)g_ptr_array_index(groups, i))->key, by))
			group = g_ptr_array_index(groups, i);

	if (NULL == group)
	{
		g_autoptr(GString) known = g_string_new(NULL);

		for (i = 0; i < groups->len; i++)
		{
			if (i > 0)
				g_string_append(known, ", ");

			g_string_append(known, ((const VentureWebChartGroup *)g_ptr_array_index(groups, i))->key);
		}

		*out_problem = venture_web_chart_problem_take(VENTURE_ERROR_INVALID_ARGUMENT,
			g_strdup_printf("chart_by: a chart cannot split these records by \"%s\"; "
			                "it can split them by %s", by, known->str));
		return FALSE;
	}

	if (0 != g_strcmp0(value, "count"))
	{
		for (i = 0; (NULL == sum) && (i < sums->len); i++)
		{
			const VentureWebChartSum *candidate = g_ptr_array_index(sums, i);

			if (g_str_has_prefix(value, "sum:") &&
			    (0 == g_strcmp0(value + strlen("sum:"), candidate->column)))
				sum = candidate;
		}

		if (NULL == sum)
		{
			*out_problem = venture_web_chart_problem_take(VENTURE_ERROR_INVALID_ARGUMENT,
				g_strdup_printf("chart_value: \"%s\" is not something a chart of these "
				                "records can show; use count%s", value,
				                (sums->len > 0) ? " or sum: and a money field" : ""));
			return FALSE;
		}
	}

	if (!venture_web_query_chart_is_kind(kind))
	{
		*out_problem = venture_web_chart_problem_take(VENTURE_ERROR_INVALID_ARGUMENT,
			g_strdup_printf("chart_kind: \"%s\" is not a chart; use bar, table, line or pie",
			                kind));
		return FALSE;
	}

	/* --- the bar's own text, placed ------------------------------------- */
	if (check && !venture_web_lql_blank(q))
	{
		VentureWebChartCheck work;

		if (!venture_lql_environment_prepare_source(environment, type_name))
		{
			*out_problem = venture_web_chart_problem_take(VENTURE_ERROR_NOT_FOUND,
				g_strdup_printf("no source named '%s'", type_name));
			return FALSE;
		}

		memset(&work, 0, sizeof work);
		work.environment = environment;
		work.type_name = type_name;
		work.text = q;

		{
			g_autoptr(GCancellable) cancellable = g_cancellable_new();

			if (!venture_web_query_off_loop(self, request, NULL, venture_web_chart_check_work,
			                                &work, cancellable, &error))
			{
				*out_problem = venture_web_chart_problem_from_error(error);
				return FALSE;
			}
		}

		if (NULL != work.problem)
		{
			*out_problem = work.problem;
			return FALSE;
		}
	}

	/* --- the query: the page's, then the summarize ---------------------- */
	text = g_string_new("fetch ");
	g_string_append(text, type_name);

	if (venture_web_wants_deleted(request))
		g_string_append(text, ", " VENTURE_LQL_PARAM_INCLUDE_DELETED ": TRUE");

	if (!venture_web_lql_blank(url_filter))
		g_string_append_printf(text, "\n| filter %s", url_filter);

	/* q may be an expression or a whole filter (filter_out) stage, as the
	 * bar takes it; on a line of its own either way. */
	if (!venture_web_lql_blank(q))
	{
		static const gchar *const stages[] = { "filter", "filter_out" };
		const gchar *start = q;
		gboolean stage = FALSE;
		gsize s;

		while (g_ascii_isspace(*start))
			start++;

		for (s = 0; s < G_N_ELEMENTS(stages); s++)
		{
			gsize length = strlen(stages[s]);

			stage = stage || ((0 == strncmp(start, stages[s], length)) &&
			                  (('\0' == start[length]) || g_ascii_isspace(start[length])));
		}

		if (stage)
			g_string_append_printf(text, "\n| %s", q);
		else
			g_string_append_printf(text, "\n| filter %s", q);
	}

	g_string_append(text, "\n| summarize n = count()");

	if (NULL != sum)
		g_string_append_printf(text, ", total = sum_money(%s)", sum->column);

	if (0 == g_strcmp0(group->expression, group->column))
		g_string_append_printf(text, ", by: {%s}", group->column);
	else
		g_string_append_printf(text, ", by: {%s = %s}", group->column, group->expression);

	/* Dates read in time order; anything else biggest first. */
	if (group->chronological)
		g_string_append_printf(text, "\n| sort %s", group->column);
	else
		g_string_append_printf(text, "\n| sort n DESC, %s", group->column);

	/* Off the main loop, the fetches read in the request's own scopes. */
	document = venture_web_query_run(self, request, environment, text->str, FALSE, &error);

	if (NULL == document)
	{
		*out_problem = venture_web_chart_problem_from_error(error);
		return FALSE;
	}

	/* How many records the chart counted: what the page's subtitle says. */
	rows = json_object_get_array_member(json_node_get_object(document), "rows");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		if (json_object_has_member(row, "n"))
			total += json_object_get_int_member(row, "n");
	}

	chart = venture_web_query_chart(document, kind, group->column, NULL,
	                                (NULL != sum) ? "total" : "n", &drawn, &error);

	if (NULL == chart)
	{
		*out_problem = venture_web_chart_problem_from_error(error);
		return FALSE;
	}

	/* --- the drawing ------------------------------------------------------ */
	venture_web_chart_append_controls(html, request, action, groups, sums, group->key,
	                                  (NULL != sum) ? value : "count", kind);
	g_string_append_printf(html, "<div class=\"card list-chart\" data-chart-kind=\"%s\">",
	                       kind);
	g_string_append(html, chart);
	g_string_append(html, "</div><script type=\"application/json\" data-chart-data>");
	venture_web_chart_append_json(html, drawn);
	g_string_append(html, "</script>");

	*out_total = total;

	return TRUE;
}
