/*
 * The five-card home page. Rendered for "/" when the organisation has not
 * asked for the classic dashboard; the cards themselves come from
 * venture_headline_home_cards(), so this is only HTML around one JSON pass.
 */
static gchar *
venture_web_render_headline_home(
	VentureWebServer	*self,
	HtmxRequest		*request
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonNode) cards = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *array;
	const gchar *requested;
	gint64 organization_id;
	guint i;

	if (!venture_web_module_enabled(self, "headline"))
		return NULL;

	organization_id = venture_web_active_organization(self, request);

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(self->context);

	if (!venture_headline_home_enabled(venture_context_get_database(self->context),
	                                   organization_id))
		return NULL;

	requested = htmx_request_get_query_param(request, "period");
	period = venture_context_parse_period(self->context,
		venture_string_is_empty(requested) ? "this_month" : requested, &error);

	if (NULL == period)
		return NULL;

	cards = venture_headline_home_cards(self->context, organization_id, period,
	                                    &error);

	if (NULL == cards)
		return NULL;

	content = g_string_new("<div class=\"page-head\"><div class=\"page-title\">"
	                       "<h1>The five questions</h1><span class=\"subtitle\">");
	venture_html_escape_append(content, venture_date_range_get_label(period));
	g_string_append(content, ", against the period before</span></div>"
	                         "<div class=\"page-actions\">"
	                         "<a class=\"btn\" href=\"/overview\">Old dashboard</a> "
	                         "<a class=\"btn\" href=\"/reports\">All reports</a>"
	                         "</div></div><div class=\"headline-cards widget-grid\">");

	array = json_node_get_array(cards);

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *card;
		JsonArray *lines;
		const gchar *arrow;
		const gchar *tone;
		gint64 trend;
		gboolean higher_is_better;
		guint j;

		card = json_array_get_object_element(array, i);
		trend = json_object_get_int_member(card, "trend");
		higher_is_better = json_object_get_boolean_member(card, "higher_is_better");
		arrow = (trend > 0) ? "\xe2\x86\x91" : ((trend < 0) ? "\xe2\x86\x93" : "\xe2\x86\x92");
		tone = (0 == trend) ? "flat"
			: (((trend > 0) == higher_is_better) ? "up" : "down");

		g_string_append_printf(content,
			"<div class=\"card widget headline-card\" data-card=\"%s\"><h3>",
			json_object_get_string_member(card, "key"));
		venture_html_escape_append(content, json_object_get_string_member(card, "label"));
		g_string_append(content, "</h3><div class=\"widget-figure\"><span class=\"figure\">");
		venture_html_escape_append(content, json_object_get_string_member(card, "value"));
		g_string_append_printf(content,
			"</span><span class=\"stat-delta %s\" title=\"against the period before\">%s</span></div><ul class=\"headline-lines\">",
			tone, arrow);

		lines = json_object_get_array_member(card, "lines");

		for (j = 0; j < json_array_get_length(lines); j++)
		{
			JsonObject *line;

			line = json_array_get_object_element(lines, j);
			g_string_append(content, "<li><span class=\"muted\">");
			venture_html_escape_append(content, json_object_get_string_member(line, "label"));
			g_string_append(content, "</span> ");
			venture_html_escape_append(content, json_object_get_string_member(line, "value"));
			g_string_append(content, "</li>");
		}

		g_string_append_printf(content,
			"</ul><p><a class=\"btn\" href=\"%s\">Report</a></p></div>",
			json_object_get_string_member(card, "link"));
	}

	g_string_append(content, "</div>");

	return venture_web_page(self, request, "/", "Home", content->str);
}

/* GET /api/v1/headline - the same five cards as JSON. */
static HtmxResponse *
venture_web_api_headline(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonNode) cards = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *denied;
	const gchar *requested;
	const gchar *organization;
	gint64 organization_id;

	(void)params;

	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != denied)
		return denied;

	if (!venture_web_module_enabled(self, "headline"))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The headline module is off");
		return venture_web_error_response(error);
	}

	requested = htmx_request_get_query_param(request, "period");
	organization = htmx_request_get_query_param(request, "organization_id");
	organization_id = venture_string_is_empty(organization)
		? venture_web_active_organization(self, request)
		: g_ascii_strtoll(organization, NULL, 10);
	period = venture_context_parse_period(self->context,
		venture_string_is_empty(requested) ? "this_month" : requested, &error);

	if (NULL == period)
		return venture_web_error_response(error);

	cards = venture_headline_home_cards(self->context, organization_id, period,
	                                    &error);

	if (NULL == cards)
		return venture_web_error_response(error);

	return venture_web_json_response(cards, 200);
}
