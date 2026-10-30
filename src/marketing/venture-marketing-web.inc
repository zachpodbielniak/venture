/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* These public routes authorize only the opaque capability, never a session
 * or caller-selected organization. GET never changes email preferences. */
static HtmxResponse *marketing_response_full(gint status, const gchar *content, const gchar *referrer)
{
	HtmxResponse *response = htmx_response_new_with_content(content);
	htmx_response_set_status(response, status);
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	htmx_response_add_header(response, "Referrer-Policy", referrer);
	htmx_response_add_header(response, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
	return response;
}
/* Pixels and click redirects must not hand the tracking URL to the target. */
static HtmxResponse *marketing_response(gint status, const gchar *content)
{
	return marketing_response_full(status, content, "no-referrer");
}
/* The preference form posts to itself. no-referrer would make that Origin null. */
static HtmxResponse *marketing_preference_response(gint status, const gchar *content)
{
	return marketing_response_full(status, content, "same-origin");
}
/* Read for the machine-open judgement and never stored or logged. */
static const gchar *venture_web_marketing_user_agent(HtmxRequest *request)
{
	SoupServerMessage *message = htmx_request_get_message(request);
	return message ? soup_message_headers_get_one(soup_server_message_get_request_headers(message), "User-Agent") : NULL;
}
/* The preference centre's page: topics, how often, and stop everything.
 * It names no person and no address -- the link may be forwarded -- and
 * carries its own small style so it reads on a phone without the app's
 * stylesheet, which a recipient never loads. */
static const gchar marketing_preference_style[] =
	"<style>:root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fafaf7;--muted:#5f5f5a;--line:#d7d6cf;--accent:#1f4fd1}"
	"@media(prefers-color-scheme:dark){:root{--fg:#ecebe6;--bg:#151514;--muted:#a3a29b;--line:#3a3935;--accent:#8fb0ff}}"
	"body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,sans-serif}"
	"main{max-width:34rem;margin:0 auto;padding:2rem 1.25rem}h1{font-size:1.6rem;margin:0 0 .5rem}"
	"p{color:var(--muted)}fieldset{border:1px solid var(--line);margin:1.25rem 0;padding:1rem}legend{font-weight:600;padding:0 .3rem}"
	"label{display:flex;gap:.6rem;align-items:flex-start;padding:.35rem 0}label small{display:block;color:var(--muted)}"
	"button{font:inherit;padding:.6rem 1.1rem;border:1px solid var(--accent);background:var(--accent);color:var(--bg);cursor:pointer}"
	"button.quiet{background:transparent;color:var(--accent)}form.stop{border-top:1px solid var(--line);margin-top:2rem;padding-top:1rem}</style>";
static HtmxResponse *venture_web_marketing_preference_page(VentureMarketingService *service, const gchar *token, const gchar *notice)
{
	g_autoptr(GPtrArray) topics = NULL;
	g_autoptr(VentureEntity) preference = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) html = g_string_new("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Email preferences</title>");
	g_autofree gchar *off = NULL;
	gint frequency = 0;
	guint i;
	static const struct { const gchar *value; const gchar *label; } often[] = {
		{ "any", "Whenever there is something to send" }, { "weekly", "At most one email a week" }, { "monthly", "At most one email a month" }
	};
	if (!venture_marketing_service_preference_view(service, token, &topics, &preference, &error))
		return marketing_response(404, "Not found");
	if (preference) g_object_get(preference, "topics-off", &off, "frequency", &frequency, NULL);
	g_string_append(html, marketing_preference_style);
	g_string_append(html, "</head><body><main><h1>Email preferences</h1>");
	if (notice) g_string_append_printf(html, "<p role=\"status\"><strong>%s</strong></p>", notice);
	g_string_append(html, "<p>Choose what this organization sends you by email. Invoices, receipts and other messages you need are not affected.</p>"
		"<form method=\"post\"><input type=\"hidden\" name=\"action\" value=\"save\">");
	if (topics->len) {
		g_string_append(html, "<fieldset><legend>Topics</legend>");
		for (i = 0; i < topics->len; i++) {
			VentureEntity *topic = g_ptr_array_index(topics, i);
			g_autofree gchar *name = NULL, *description = NULL, *escaped = NULL, *about = NULL;
			g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(topic));
			g_auto(GStrv) parts = g_strsplit(off ? off : "", ",", -1);
			gboolean on = !g_strv_contains((const gchar *const *)parts, id);
			g_object_get(topic, "name", &name, "description", &description, NULL);
			escaped = g_markup_escape_text(name ? name : "", -1);
			about = description && *description ? g_markup_escape_text(description, -1) : NULL;
			g_string_append_printf(html, "<label><input type=\"checkbox\" name=\"topic\" value=\"%s\"%s><span>%s%s%s%s</span></label>",
				id, on ? " checked" : "", escaped, about ? "<small>" : "", about ? about : "", about ? "</small>" : "");
		}
		g_string_append(html, "</fieldset>");
	}
	g_string_append(html, "<fieldset><legend>How often</legend>");
	for (i = 0; i < G_N_ELEMENTS(often); i++)
		g_string_append_printf(html, "<label><input type=\"radio\" name=\"frequency\" value=\"%s\"%s><span>%s</span></label>",
			often[i].value, (gint)i == frequency ? " checked" : "", often[i].label);
	g_string_append(html, "</fieldset><button type=\"submit\">Save my choices</button></form>"
		"<form method=\"post\" class=\"stop\"><input type=\"hidden\" name=\"List-Unsubscribe\" value=\"One-Click\">"
		"<p>Or stop all marketing email from this organization.</p><button type=\"submit\" class=\"quiet\">Stop marketing email: unsubscribe from everything</button></form>"
		"</main></body></html>");
	return marketing_preference_response(200, html->str);
}
/* Every topic value in a form body: the form sends one per ticked box. */
static gchar *venture_web_marketing_ticked(GBytes *body, GPtrArray *offered)
{
	gsize length = 0;
	const gchar *data = body ? g_bytes_get_data(body, &length) : NULL;
	g_autofree gchar *text = g_strndup(data ? data : "", length);
	g_auto(GStrv) pairs = g_strsplit(text, "&", -1);
	g_autoptr(GHashTable) ticked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GString) off = g_string_new(NULL);
	guint i;
	for (i = 0; pairs[i]; i++)
		if (g_str_has_prefix(pairs[i], "topic=")) {
			g_strdelimit(pairs[i], "+", ' ');
			g_hash_table_add(ticked, g_uri_unescape_string(pairs[i] + 6, NULL));
		}
	for (i = 0; i < offered->len; i++) {
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(g_ptr_array_index(offered, i)));
		if (!g_hash_table_contains(ticked, id)) g_string_append_printf(off, "%s%s", off->len ? "," : "", id);
	}
	return g_string_free(g_steal_pointer(&off), FALSE);
}
static HtmxResponse *venture_web_marketing_unsubscribe(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureMarketingService *service = venture_marketing_service_get(venture_context_get_database(self->context));
	const gchar *token = g_hash_table_lookup(params, "token");
	g_autoptr(GError) error = NULL;
	gsize i;
	if (!token || strlen(token) != 64) return marketing_response(404, "Not found");
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(token[i])) return marketing_response(404, "Not found");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		GBytes *body = htmx_request_get_body_bytes(request);
		if (!body || g_bytes_get_size(body) > 4096) return marketing_response(400, "Expected a preference form");
		if (!g_strcmp0(htmx_request_get_form_value(request, "action"), "save")) {
			g_autoptr(GPtrArray) topics = NULL;
			g_autoptr(VentureEntity) preference = NULL;
			g_autofree gchar *off = NULL;
			const gchar *frequency = htmx_request_get_form_value(request, "frequency");
			if (!venture_marketing_service_preference_view(service, token, &topics, &preference, &error))
				return marketing_response(404, "Not found");
			off = venture_web_marketing_ticked(body, topics);
			if (!venture_marketing_service_set_preferences(service, token, off, frequency ? frequency : "any", &error))
				return g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND) ? marketing_response(404, "Not found")
					: marketing_response(400, "That choice is not one this page offers");
			return venture_web_marketing_preference_page(service, token, "Your choices are saved.");
		}
		if (g_bytes_get_size(body) > 1024 || g_strcmp0(htmx_request_get_form_value(request, "List-Unsubscribe"), "One-Click"))
			return marketing_response(400, "Expected List-Unsubscribe=One-Click");
		if (!venture_marketing_service_unsubscribe(service, token, &error))
			return marketing_response(404, "Not found");
		return marketing_preference_response(200, "<!doctype html><html><head><title>Preference saved</title></head><body><h1>Preference saved</h1><p>Marketing email to this address from this organization is stopped. Necessary transactional messages remain independent.</p></body></html>");
	}
	/* A syntax-valid unknown capability gets the same generic page as
	 * before; it shows no topics and saving it answers 404. */
	{
		HtmxResponse *page = venture_web_marketing_preference_page(service, token, NULL);
		if (htmx_response_get_status(page) == 200) return page;
		g_object_unref(page);
	}
	return marketing_preference_response(200, "<!doctype html><html><head><title>Email preference</title></head><body><h1>Email preference</h1><p>Stop marketing email to this address from this organization. Necessary invoices, receipts and other transactional messages remain independent.</p><form method=\"post\"><input type=\"hidden\" name=\"List-Unsubscribe\" value=\"One-Click\"><button type=\"submit\">Stop marketing email</button></form></body></html>");
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
	if (!venture_marketing_service_record_open(venture_marketing_service_get(venture_context_get_database(self->context)), token,
		venture_web_marketing_user_agent(request), now, &error))
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
	url = venture_marketing_service_record_click(venture_marketing_service_get(venture_context_get_database(self->context)), token, position,
		venture_web_marketing_user_agent(request), now, &error);
	if (!url || !venture_web_url_is_web(url)) return marketing_response(404, "Not found");
	response = marketing_response(302, ""); htmx_response_add_header(response, "Location", url); return response;
}
static HtmxResponse *venture_web_marketing_current(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	const gchar *requested = htmx_request_get_query_param(request, "organization_id");
	const gchar *email = htmx_request_get_query_param(request, "email");
	const gchar *purpose = htmx_request_get_query_param(request, "purpose");
	const gchar *state;
	g_autoptr(GDateTime) recorded_at = NULL, withdrawn_at = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stamp = NULL;
	HtmxResponse *denied, *response;
	gint64 org;
	(void)params;
	denied = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (denied) return denied;
	if (!requested || !g_ascii_string_to_signed(requested, 10, 1, G_MAXINT64, &org, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Choose a valid organization_id");
		return venture_web_error_response(error);
	}
	if (!venture_marketing_service_current_permission(
		venture_marketing_service_get(venture_context_get_database(self->context)), org, email, purpose,
		&state, &recorded_at, &withdrawn_at, &error))
		return venture_web_error_response(error);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "state"); json_builder_add_string_value(builder, state);
	if (recorded_at) {
		stamp = g_date_time_format_iso8601(recorded_at);
		json_builder_set_member_name(builder, "recorded_at"); json_builder_add_string_value(builder, stamp);
		g_clear_pointer(&stamp, g_free);
	}
	if (withdrawn_at) {
		stamp = g_date_time_format_iso8601(withdrawn_at);
		json_builder_set_member_name(builder, "withdrawn_at"); json_builder_add_string_value(builder, stamp);
	}
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	response = venture_web_json_response(node, 200);
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	return response;
}
