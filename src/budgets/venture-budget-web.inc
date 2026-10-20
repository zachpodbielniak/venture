/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
budgets_report(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GString) content = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *month = NULL, *rendered = NULL;
	gint64 organization = 0;
	const gchar *period = htmx_request_get_query_param(request, "period");
	const gchar *kind = htmx_request_get_query_param(request, "kind");
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	HtmxResponse *gate;
	(void)params;
	gate = api ? venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER)
		: venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = api ? venture_web_require_module_api(self, "budgets")
		: venture_web_require_module_ui(self, request, "budgets");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	(void)principal;
	if (!venture_web_request_organization(self, request, NULL, &organization, &error))
		return venture_web_error_response(error);
	if (period == NULL || period[0] == '\0')
	{
		now = venture_time_now();
		month = g_date_time_format(now, "%Y-%m");
		period = month;
	}
	if (kind == NULL || !*kind)
		kind = venture_web_household_view(self, request) ? "envelopes" : "actual";
	if (g_strcmp0(kind, "envelopes") == 0)
		result = venture_budget_service_envelopes(venture_budget_service_get(venture_context_get_database(self->context)),
			organization, period, &error);
	else if (g_strcmp0(kind, "forecast") == 0)
		result = venture_budget_service_cash_forecast(venture_budget_service_get(venture_context_get_database(self->context)),
			organization, period, &error);
	else
		result = venture_budget_service_vs_actual(venture_budget_service_get(venture_context_get_database(self->context)),
			organization, period, NULL, &error);
	if (result == NULL)
		return venture_web_error_response(error);
	if (api)
		return venture_web_json_response(venture_report_result_to_json(result), 200);
	content = g_string_new("<div class=\"page-head\"><h1>Budgets</h1>"
		"<p class=\"muted\">Monthly envelopes show what is left to spend. Warnings start at 80% used.</p></div>"
		"<section class=\"card\"><form method=\"get\" action=\"/budgets\">"
		"<label>Period<input name=\"period\" value=\"");
	venture_html_escape_append(content, period);
	g_string_append(content, "\"></label><label>Report<select name=\"kind\">");
	g_string_append_printf(content, "<option value=\"envelopes\"%s>Monthly envelopes</option>"
		"<option value=\"actual\"%s>vs actual</option><option value=\"forecast\"%s>cash forecast</option>",
		g_str_equal(kind, "envelopes") ? " selected" : "",
		g_str_equal(kind, "actual") ? " selected" : "", g_str_equal(kind, "forecast") ? " selected" : "");
	g_string_append(content, "</select></label><button class=\"btn btn-primary\" type=\"submit\">Run</button></form>"
		"<p><a href=\"/e/budget\">Budget records</a> · <a href=\"/e/budget_line\">Budget lines</a></p>"
		"<p>For envelopes, create an active budget and a line for each spending account. "
		"Set the line period to its first month, enter the monthly amount, and turn on Monthly envelope. "
		"Choose Roll over for each category that should carry unspent money and overspending forward.</p></section>");
	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_HTML);
	g_string_append(content, rendered);

	return venture_web_html_response(venture_web_page(self, request, "/budgets", "Budgets", content->str), 200);
}

static void
venture_budget_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/budgets", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, budgets_report, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/budget_reports", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, budgets_report, self);
}
