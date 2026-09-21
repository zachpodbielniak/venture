/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* These public routes authorize only the opaque capability, never a session
 * or caller-selected organization. GET never changes email preferences. */
static HtmxResponse *marketing_response(gint status, const gchar *content)
{
	HtmxResponse *response = htmx_response_new_with_content(content);
	htmx_response_set_status(response, status);
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
	htmx_response_add_header(response, "Content-Security-Policy", "default-src 'none'; form-action 'self'; frame-ancestors 'none'");
	return response;
}
static HtmxResponse *venture_web_marketing_unsubscribe(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	const gchar *token = g_hash_table_lookup(params, "token");
	g_autoptr(GError) error = NULL;
	gsize i;
	if (!token || strlen(token) != 64) return marketing_response(404, "Not found");
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(token[i])) return marketing_response(404, "Not found");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		GBytes *body = htmx_request_get_body_bytes(request);
		if (!body || g_bytes_get_size(body) > 1024 ||
			g_strcmp0(htmx_request_get_form_value(request, "List-Unsubscribe"), "One-Click"))
			return marketing_response(400, "Expected List-Unsubscribe=One-Click");
		if (!venture_marketing_service_unsubscribe(venture_marketing_service_get(venture_context_get_database(self->context)), token, &error))
			return marketing_response(404, "Not found");
		return marketing_response(200, "<!doctype html><html><head><title>Preference saved</title></head><body><h1>Preference saved</h1><p>Marketing email to this address from this organization is stopped. Necessary transactional messages remain independent.</p></body></html>");
	}
	return marketing_response(200, "<!doctype html><html><head><title>Email preference</title></head><body><h1>Email preference</h1><p>Stop marketing email to this address from this organization. Necessary invoices, receipts and other transactional messages remain independent.</p><form method=\"post\"><input type=\"hidden\" name=\"List-Unsubscribe\" value=\"One-Click\"><button type=\"submit\">Stop marketing email</button></form></body></html>");
}
static HtmxResponse *venture_web_marketing_open(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autofree gchar *token = g_strdup(g_hash_table_lookup(params, "token"));
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	HtmxResponse *response;
	const guint8 *pixel;
	gsize length;
	if (!token || !g_str_has_suffix(token, ".gif")) return marketing_response(404, "Not found");
	token[strlen(token) - 4] = '\0';
	if (!venture_marketing_service_record_open(venture_marketing_service_get(venture_context_get_database(self->context)), token, now, &error))
		return marketing_response(404, "Not found");
	pixel = venture_sequence_tracking_pixel(&length); bytes = g_bytes_new_static(pixel, length);
	response = marketing_response(200, ""); htmx_response_set_bytes(response, bytes); htmx_response_set_content_type(response, "image/gif");
	return response;
}
static HtmxResponse *venture_web_marketing_click(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	const gchar *token = g_hash_table_lookup(params, "token"), *index = g_hash_table_lookup(params, "n");
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	gchar *end = NULL;
	gint64 position;
	HtmxResponse *response;
	if (!index || !*index) return marketing_response(404, "Not found");
	position = g_ascii_strtoll(index, &end, 10);
	if (!end || *end || position < 1 || position > 1000) return marketing_response(404, "Not found");
	url = venture_marketing_service_record_click(venture_marketing_service_get(venture_context_get_database(self->context)), token, position, now, &error);
	if (!url || !venture_web_url_is_web(url)) return marketing_response(404, "Not found");
	response = marketing_response(302, ""); htmx_response_add_header(response, "Location", url); return response;
}
