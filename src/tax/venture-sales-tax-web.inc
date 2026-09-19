/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
sales_tax_export_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *csv = NULL;
	HtmxResponse *response;
	static const gchar *const strings[] = { "jurisdiction", "currency", "as_of", NULL };
	static const gchar *const integers[] = { "organization_id", "venture_id", NULL };
	guint i;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "sales_tax");
	if (gate != NULL)
		return gate;
	period = venture_context_parse_period(self->context,
		htmx_request_get_query_param(request, "period"), &error);
	if (period == NULL)
		return venture_web_error_response(error);
	for (i = 0; strings[i] != NULL; i++)
	{
		const gchar *value = htmx_request_get_query_param(request, strings[i]);
		if (!venture_string_is_empty(value))
			json_object_set_string_member(options, strings[i], value);
	}
	for (i = 0; integers[i] != NULL; i++)
	{
		const gchar *value = htmx_request_get_query_param(request, integers[i]);
		if (!venture_string_is_empty(value))
			json_object_set_int_member(options, integers[i], g_ascii_strtoll(value, NULL, 10));
	}
	if (!json_object_has_member(options, "organization_id"))
		json_object_set_int_member(options, "organization_id", tax_org(self, request));
	csv = venture_sales_tax_service_export_csv(
		venture_sales_tax_service_get(venture_context_get_database(self->context)),
		self->context, period, options, &error);
	if (csv == NULL)
		return venture_web_error_response(error);
	response = htmx_response_new_with_content(csv);
	htmx_response_set_content_type(response, "text/csv; charset=utf-8");
	return response;
}
