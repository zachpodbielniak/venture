/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
accounting_api_home(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) home = NULL;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "accounting");
	if (gate != NULL)
		return gate;
	home = venture_accounting_home(self->context, venture_web_active_organization(self, request), &error);
	if (home == NULL)
		return venture_web_error_response(error);
	return venture_web_json_response(home, 200);
}

static HtmxResponse *
accounting_ui_home(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) home = NULL;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Books</h1><p class=\"muted\">What to do next, with the reason each item is on the list.</p></div>");
	JsonArray *array;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "accounting");
	if (gate != NULL)
		return gate;
	home = venture_accounting_home(self->context, venture_web_active_organization(self, request), &error);
	if (home == NULL)
		return venture_web_error_response(error);
	array = json_node_get_array(home);
	g_string_append(content, "<div class=\"grid cols-2\">");
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);
		g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>");
		venture_html_escape_append(content, json_object_get_string_member(row, "title"));
		g_string_append_printf(content, "</h2><span class=\"stat-value\">%" G_GINT64_FORMAT "</span></div><div class=\"card-body\"><p>",
			venture_json_object_get_int(row, "count", 0));
		venture_html_escape_append(content, json_object_get_string_member(row, "reason"));
		g_string_append(content, "</p><a class=\"btn\" href=\"");
		venture_html_escape_append(content, json_object_get_string_member(row, "href"));
		g_string_append(content, "\">Open</a></div></div>");
	}
	g_string_append(content, "</div>");
	return venture_web_html_response(venture_web_page(self, request, "/accounting", "Books", content->str), 200);
}
