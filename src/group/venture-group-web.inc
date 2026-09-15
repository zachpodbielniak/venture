/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
group_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GString) content = NULL;
	const gchar *label = htmx_request_get_query_param(request, "period");
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "group");
	if (gate != NULL)
		return gate;
	if (label == NULL || label[0] == '\0')
		label = "this_month";
	period = venture_context_parse_period(self->context, label, &error);
	if (period == NULL)
		return venture_web_error_response(error);
	result = venture_group_service_consolidated(venture_group_service_get(venture_context_get_database(self->context)),
		venture_context_get_default_organization_id(self->context), "consolidated_income_statement",
		period, "USD", &error);
	if (result == NULL)
		return venture_web_error_response(error);
	content = g_string_new("<div class=\"page-head\"><h1>Group consolidation</h1>"
		"<p class=\"muted\">Intercompany links, elimination journals and consolidated statements.</p></div>"
		"<section class=\"card\"><p><a href=\"/e/intercompany_link\">Intercompany links</a> "
		"<a href=\"/e/elimination\">Eliminations</a></p></section>");
	return venture_web_html_response(venture_web_page(self, request, "/group", "Group", content->str), 200);
}

static HtmxResponse *
group_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	const gchar *label = htmx_request_get_query_param(request, "period");
	const gchar *name = htmx_request_get_query_param(request, "report");
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "group");
	if (gate != NULL)
		return gate;
	if (label == NULL || label[0] == '\0')
		label = "this_month";
	if (name == NULL)
		name = "consolidated_income_statement";
	period = venture_context_parse_period(self->context, label, &error);
	if (period == NULL)
		return venture_web_error_response(error);
	result = venture_group_service_consolidated(venture_group_service_get(venture_context_get_database(self->context)),
		venture_context_get_default_organization_id(self->context), name, period, "USD", &error);
	if (result == NULL)
		return venture_web_error_response(error);
	return venture_web_json_response(venture_report_result_to_json(result), 200);
}

static void
venture_group_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_get(router, "/group", group_page, self);
	htmx_router_get(router, "/api/v1/group/reports", group_api, self);
}
