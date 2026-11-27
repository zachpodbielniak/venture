/*
 * venture-dashboard-query.inc - The query widget: an LQL query, drawn
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-dashboard.c. A query widget holds an LQL query in
 * `body` and, in `options`, how to draw its rows: {"chart": "table" |
 * "bar" | "line" | "pie", "x": COLUMN, "series": COLUMN, "value": COLUMN}
 * (docs/dashboards.org, "The query kind").
 *
 * The query never runs as the widget's author. It runs through the
 * scope's query function, which the web layer builds from the request
 * asking for the page -- the viewer's token or session, their access
 * policy, their organizations -- so two people looking at one shared
 * dashboard see two answers, each only what they could list themselves.
 * A scope with no query function (the assistant, a caller with nobody
 * looking) gets a refusal in place, never somebody else's rows.
 *
 * The drawing is venture_web_query_chart(), the same function the list
 * pages' Chart toggle calls, and the card's JSON is the query's own
 * document with what was drawn beside it, from that one pass.
 */

/* The options a query widget reads; anything else is refused at the save. */
static const gchar *const venture_widget_query_options[] = {
	"chart", "x", "series", "value", NULL
};

static const gchar *const venture_widget_uses_query[] = {
	"body", "options", NULL
};

/* An option of a query widget, as a string, or NULL. */
static const gchar *
venture_widget_query_option(
	JsonNode	*options,
	const gchar	*name
){
	if ((NULL == options) || !JSON_NODE_HOLDS_OBJECT(options))
		return NULL;

	return venture_json_object_get_string(json_node_get_object(options), name, NULL);
}

static VentureWidgetResult *
venture_widget_kind_query(
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
){
	g_autoptr(VentureWidgetResult) result = NULL;
	g_autoptr(JsonNode) options = NULL;
	g_autoptr(JsonNode) document = NULL;
	g_autoptr(JsonNode) drawn = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *html = NULL;
	JsonObject *data;

	(void)context;
	(void)user_data;

	text = venture_widget_get_string(widget, "body");

	if (NULL == text)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A query widget needs its LQL in body, such as: "
		                    "fetch ticket | summarize n = count(), by: status");
		return NULL;
	}

	options = venture_widget_get_options(widget, error);

	if ((NULL == options) && (NULL != error) && (NULL != *error))
		return NULL;

	/* As the viewer, or not at all. */
	if (NULL == scope->query)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		                    "A query widget runs as the person looking at it, and "
		                    "there is nobody here to run it as");
		return NULL;
	}

	document = scope->query(text, scope->query_data, error);

	if (NULL == document)
		return NULL;

	html = venture_web_query_chart(document,
		venture_widget_query_option(options, "chart"),
		venture_widget_query_option(options, "x"),
		venture_widget_query_option(options, "series"),
		venture_widget_query_option(options, "value"),
		&drawn, error);

	if (NULL == html)
		return NULL;

	result = venture_widget_result_new();
	result->title = g_strdup("Query");
	result->html = g_steal_pointer(&html);

	/* The query's own document, with what was drawn from it. */
	data = json_node_get_object(document);
	json_object_set_member(data, "chart", g_steal_pointer(&drawn));
	result->data = g_steal_pointer(&document);

	return g_steal_pointer(&result);
}

/*
 * A query widget's save-time check: a query that parses, and options the
 * kind reads, each a string, with a chart it can draw. Whether the query
 * runs -- its record types, its columns -- depends on who is looking, so
 * that is the page's to say, in place.
 */
static gboolean
venture_dashboard_validate_query(
	VentureDashboardWidget	 *widget,
	JsonNode		 *options,
	GError			**error
){
	g_autofree gchar *text = NULL;
	g_autoptr(LqlPipeline) pipeline = NULL;
	g_autoptr(GError) parse_error = NULL;

	text = venture_widget_get_string(widget, "body");

	if (NULL == text)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A query widget needs its LQL in body, such as: "
		                    "fetch ticket | summarize n = count(), by: status");
		return FALSE;
	}

	/* Before LQL parses it: its lexer reads UTF-8 alone, and the text is
	 * whatever the author sent (venture_lql_text_check_utf8()). */
	if (!venture_lql_text_check_utf8(text, error))
		return FALSE;

	pipeline = lql_parse(text, &parse_error);

	if (NULL == pipeline)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "The query does not parse: %s", parse_error->message);
		return FALSE;
	}

	if ((NULL != options) && JSON_NODE_HOLDS_OBJECT(options))
	{
		JsonObject *object = json_node_get_object(options);
		g_autoptr(GList) members = json_object_get_members(object);
		GList *item;

		for (item = members; NULL != item; item = item->next)
		{
			JsonNode *member = json_object_get_member(object, item->data);

			if (!g_strv_contains(venture_widget_query_options, item->data))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "A query widget has no option \"%s\"; it takes chart, x, "
				            "series and value", (const gchar *)item->data);
				return FALSE;
			}

			if (!JSON_NODE_HOLDS_VALUE(member) ||
			    (G_TYPE_STRING != json_node_get_value_type(member)))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "options.%s must be a string: a chart kind or a column name",
				            (const gchar *)item->data);
				return FALSE;
			}
		}

		if (json_object_has_member(object, "chart") &&
		    !venture_web_query_chart_is_kind(json_object_get_string_member(object, "chart")))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "options.chart is \"%s\"; a query widget draws table, bar, line or pie",
			            json_object_get_string_member(object, "chart"));
			return FALSE;
		}
	}

	return TRUE;
}
