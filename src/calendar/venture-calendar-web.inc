/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server so these thin adapters share its guards. */

/* Calendar sync. A fresh service per request: the sweep is bounded and
 * synchronous, and the libsoup client holds no state between calls. */
static VentureCalendarSyncService *venture_web_calendar_sync_service(VentureWebServer *self)
{
	g_autoptr(VentureSoupCalDavClient) client = venture_soup_caldav_client_new();
	return venture_calendar_sync_service_new(venture_context_get_database(self->context), VENTURE_CALDAV_CLIENT(client));
}
static gint64 venture_web_calendar_organization(VentureWebServer *self, HtmxRequest *request, JsonObject *body)
{
	const gchar *value = htmx_request_get_query_param(request, "organization_id");
	gint64 org = value ? g_ascii_strtoll(value, NULL, 10) : 0;
	if (org <= 0 && body) org = venture_json_object_get_int(body, "organization_id", 0);
	if (org <= 0) org = venture_web_active_organization(self, request);
	return org > 0 ? org : venture_context_get_default_organization_id(self->context);
}
static HtmxResponse *venture_web_calendar_sync(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureCalendarSyncService) service = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL, report = NULL;
	JsonObject *object = NULL;
	VentureActor actor;
	gint64 limit = 100;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	(void)params;
	if (gate) return gate;
	gate = venture_web_require_module_api(self, "calendar");
	if (gate) return gate;
	if (htmx_request_get_query_param(request, "stage")) {
		venture_set_error_validation(&error, "stage", "Calendar sync is an immediate sweep"); return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	body = htmx_request_get_json(request, NULL);
	if (body && JSON_NODE_HOLDS_OBJECT(body)) object = json_node_get_object(body);
	if (object) limit = venture_json_object_get_int(object, "limit", 100);
	if (limit < 1 || limit > 1000) { venture_set_error_validation(&error, "limit", "Expected 1 through 1000"); return venture_web_error_response(error); }
	service = venture_web_calendar_sync_service(self);
	report = venture_calendar_sync_service_sweep(service, venture_web_calendar_organization(self, request, object), (guint)limit, &actor, &error);
	if (!report) return venture_web_error_response(error);
	return venture_web_json_response(report, 200);
}

/* The public scheduling link: GET offers the free slots, POST books one.
 * No session is involved; the page's owner and organization come from the
 * slug, and the same per-address rate limit as lead capture applies. */
static HtmxResponse *venture_web_booking_plain(guint status, const gchar *text)
{
	HtmxResponse *response = htmx_response_new_with_content(text);
	htmx_response_set_status(response, status);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	return response;
}
static gboolean venture_web_booking_wants_json(HtmxRequest *request)
{
	const gchar *content_type = htmx_request_get_content_type(request);
	const gchar *format = htmx_request_get_query_param(request, "format");
	return !g_strcmp0(format, "json") || (content_type && g_str_has_prefix(content_type, "application/json"));
}
static HtmxResponse *venture_web_booking_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureBookingService) service = NULL;
	g_autoptr(VentureEntity) page = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) slots = NULL;
	g_autofree gchar *address = venture_auth_remote_address(request);
	const gchar *slug = g_hash_table_lookup(params, "slug");
	GBytes *bytes = htmx_request_get_body_bytes(request);
	gint64 rate = 0, max_mb = 0;
	if (!venture_web_module_enabled(self, "calendar")) return venture_web_booking_plain(404, "Not found");
	g_object_get(venture_context_get_config(self->context), "security-login-rate-limit", &rate, "server-max-request-size-mb", &max_mb, NULL);
	if (bytes && g_bytes_get_size(bytes) > (guint64)MAX(max_mb, 1) * 1024 * 1024) return venture_web_booking_plain(413, "Too large");
	if (self->lead_limiter == NULL) self->lead_limiter = htmx_rate_limiter_new((guint)MAX(rate, 1), MAX(rate, 1) / 60.0);
	if (!htmx_rate_limiter_allow(self->lead_limiter, address != NULL ? address : "unknown")) return venture_web_booking_plain(429, "Try again later");
	service = venture_booking_service_new(venture_context_get_database(self->context));
	page = venture_booking_service_find_page(service, slug, &error);
	if (!page) return venture_web_booking_plain(404, "Not found");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		g_autoptr(VentureEntity) activity = NULL;
		g_autoptr(JsonNode) body = NULL;
		const gchar *start = NULL, *name = NULL, *email = NULL, *notes = NULL;
		VentureActor actor;
		if (venture_web_booking_wants_json(request)) {
			JsonObject *object;
			body = htmx_request_get_json(request, NULL);
			if (!body || !JSON_NODE_HOLDS_OBJECT(body)) return venture_web_booking_plain(400, "Bad request");
			object = json_node_get_object(body);
			start = venture_json_object_get_string(object, "start", NULL);
			name = venture_json_object_get_string(object, "name", NULL);
			email = venture_json_object_get_string(object, "email", NULL);
			notes = venture_json_object_get_string(object, "notes", NULL);
		} else {
			start = htmx_request_get_form_value(request, "start");
			name = htmx_request_get_form_value(request, "name");
			email = htmx_request_get_form_value(request, "email");
			notes = htmx_request_get_form_value(request, "notes");
		}
		actor.kind = VENTURE_ACTOR_KIND_USER;
		actor.name = "public booking";
		actor.prompt = NULL; actor.request_id = NULL; actor.approved_by = NULL;
		activity = venture_booking_service_book(service, page, start, name, email, notes, NULL, &actor, &error);
		if (!activity) {
			guint status = g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT) ? 409 : 422;
			if (venture_web_booking_wants_json(request)) return venture_web_error_response(error);
			return venture_web_booking_plain(status, status == 409 ? "<p>That time was just taken. Please choose another.</p>" : "<p>Please check the name, email and time.</p>");
		}
		if (venture_web_booking_wants_json(request)) {
			g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(activity), FALSE);
			return venture_web_json_response(node, 201);
		}
		return venture_web_booking_plain(201, "<p>Booked. Thank you — you will hear from us.</p>");
	}
	slots = venture_booking_service_slots(service, page, NULL, &error);
	if (!slots) return venture_web_error_response(error);
	if (venture_web_booking_wants_json(request)) return venture_web_json_response(slots, 200);
	{
		g_autoptr(GString) html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>Book a time</title><style>body{font-family:sans-serif;max-width:40em;margin:2em auto}label{display:block;margin:.6em 0}</style></head><body><h1>");
		g_autofree gchar *title = NULL, *description = NULL, *action = NULL;
		JsonArray *array = json_node_get_array(slots);
		guint i;
		g_object_get(page, "title", &title, "description", &description, NULL);
		venture_html_escape_append(html, venture_string_is_empty(title) ? "Book a time" : title);
		g_string_append(html, "</h1>");
		if (!venture_string_is_empty(description)) { g_string_append(html, "<p>"); venture_html_escape_append(html, description); g_string_append(html, "</p>"); }
		if (json_array_get_length(array) == 0) g_string_append(html, "<p>No times are open right now. Please check back later.</p>");
		else {
			action = g_strconcat("/book/", slug, NULL);
			g_string_append(html, "<form method=\"post\" action=\"");
			venture_html_escape_append(html, action);
			g_string_append(html, "\"><label>Time <select name=\"start\">");
			for (i = 0; i < json_array_get_length(array); i++) {
				JsonObject *slot = json_array_get_object_element(array, i);
				g_string_append(html, "<option value=\"");
				venture_html_escape_append(html, venture_json_object_get_string(slot, "start", ""));
				g_string_append(html, "\">");
				venture_html_escape_append(html, venture_json_object_get_string(slot, "label", ""));
				g_string_append(html, "</option>");
			}
			g_string_append(html, "</select></label><label>Name <input name=\"name\" required></label><label>Email <input name=\"email\" type=\"email\" required></label>"
				"<label>Notes <textarea name=\"notes\"></textarea></label><button type=\"submit\">Book</button></form>");
		}
		g_string_append(html, "</body></html>");
		return venture_web_booking_plain(200, html->str);
	}
}
