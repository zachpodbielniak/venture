/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server so these thin adapters share its guards. */

/* Calendar sync. A fresh service per request: the sweep is bounded and
 * synchronous, and the libsoup client holds no state between calls. */
static VentureCalendarSyncService *venture_web_calendar_sync_service(VentureWebServer *self)
{
	g_autoptr(VentureSoupCalDavClient) client = venture_soup_caldav_client_new();
	VentureCalendarSyncService *service = venture_calendar_sync_service_new(venture_context_get_database(self->context), VENTURE_CALDAV_CLIENT(client));
	g_object_set(service, "config", venture_context_get_config(self->context), NULL);
	return service;
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
 * slug, and the same per-address rate limit as lead capture applies.
 * A peer listed in security.booking_trusted_relays may name the visitor
 * with the last X-Forwarded-For hop. Anyone else is limited as the socket
 * peer, even when they send that header. */

static gboolean venture_web_booking_relay_trusted(VentureWebServer *self, const gchar *peer)
{
	g_auto(GStrv) relays = NULL;
	g_autoptr(GInetAddress) mine = NULL;
	guint i;
	if (venture_string_is_empty(peer)) return FALSE;
	mine = g_inet_address_new_from_string(peer);
	if (mine == NULL) return FALSE;
	g_object_get(venture_context_get_config(self->context), "security-booking-trusted-relays", &relays, NULL);
	for (i = 0; (relays != NULL) && (relays[i] != NULL); i++)
	{
		g_autoptr(GInetAddress) item = g_inet_address_new_from_string(relays[i]);
		if ((item != NULL) && g_inet_address_equal(item, mine)) return TRUE;
	}
	return FALSE;
}
/* The hop the trusted relay appended. A missing or non-address value is not a key. */
static gchar *venture_web_booking_forwarded_client(HtmxRequest *request)
{
	SoupServerMessage *message = htmx_request_get_message(request);
	SoupMessageHeaders *headers;
	const gchar *header, *start;
	g_autofree gchar *last = NULL;
	g_autoptr(GInetAddress) parsed = NULL;
	if (message == NULL) return NULL;
	headers = soup_server_message_get_request_headers(message);
	if (headers == NULL) return NULL;
	header = soup_message_headers_get_list(headers, "X-Forwarded-For");
	if (venture_string_is_empty(header)) return NULL;
	start = strrchr(header, ',');
	start = (start != NULL) ? start + 1 : header;
	last = g_strstrip(g_strdup(start));
	if (last[0] == '\0') return NULL;
	parsed = g_inet_address_new_from_string(last);
	if (parsed == NULL) return NULL;
	return g_inet_address_to_string(parsed);
}
static gchar *venture_web_booking_client_address(VentureWebServer *self, HtmxRequest *request)
{
	g_autofree gchar *peer = venture_auth_remote_address(request);
	if (venture_web_booking_relay_trusted(self, peer))
	{
		gchar *forwarded = venture_web_booking_forwarded_client(request);
		if (forwarded != NULL) return forwarded;
	}
	return g_strdup((peer != NULL) ? peer : "unknown");
}
static void venture_web_booking_note_address(VentureDatabase *database, VentureEntity *activity, const gchar *address)
{
	/* The public page has no signed-in actor. The audit row is still a
	 * system write, so a hosted organization scope must not hide it or
	 * refuse the address that this request just decided. */
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	guint i;
	if ((activity == NULL) || venture_string_is_empty(address) || (g_strcmp0(address, "unknown") == 0)) return;
	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ, venture_entity_get_entity_name(activity), NULL);
	venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(activity), NULL);
	entries = venture_database_find(database, query, NULL);
	if (entries == NULL) return;
	for (i = 0; i < entries->len; i++)
	{
		g_autoptr(GError) error = NULL;
		VentureEntity *entry = g_ptr_array_index(entries, i);
		g_autofree gchar *current = NULL;
		g_object_get(entry, "ip-address", &current, NULL);
		if (!venture_string_is_empty(current)) continue;
		g_object_set(entry, "ip-address", address, NULL);
		if (!venture_database_save(database, entry, NULL, &error))
			g_warning("Cannot record the booking address: %s", error->message);
	}
}
static HtmxResponse *venture_web_booking_plain(guint status, const gchar *text)
{
	HtmxResponse *response = htmx_response_new_with_content(text);
	htmx_response_set_status(response, status);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	htmx_response_add_header(response, "Cache-Control", "no-store");
	/* Preserve the origin of local form posts without sending capability
	 * URLs as referrers to another origin. */
	htmx_response_add_header(response, "Referrer-Policy", "same-origin");
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
	g_autofree gchar *address = venture_web_booking_client_address(self, request);
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
		const gchar *start = NULL, *name = NULL, *email = NULL, *notes = NULL, *resource = NULL;
		g_autofree gchar *selected_start = NULL, *selected_resource = NULL;
		gint64 group_size = 1;
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
			resource = venture_json_object_get_string(object, "resource", NULL);
			group_size = json_object_get_int_member_with_default(object, "group_size", 1);
		} else {
			start = htmx_request_get_form_value(request, "start");
			name = htmx_request_get_form_value(request, "name");
			email = htmx_request_get_form_value(request, "email");
			notes = htmx_request_get_form_value(request, "notes");
			resource = htmx_request_get_form_value(request, "resource");
			{
				const gchar *size = htmx_request_get_form_value(request, "group_size");
				if (size != NULL && !g_ascii_string_to_signed(size, 10, 1, 10000, &group_size, NULL))
					return venture_web_booking_plain(422, "Choose a valid group size");
			}
			if (start != NULL && strchr(start, '|') != NULL) {
				const gchar *separator = strchr(start, '|');
				selected_start = g_strndup(start, separator - start);
				selected_resource = g_strdup(separator + 1);
				start = selected_start; resource = selected_resource;
			}
		}
		actor.kind = VENTURE_ACTOR_KIND_USER;
		actor.name = "public booking";
		actor.prompt = NULL; actor.request_id = NULL; actor.approved_by = NULL;
		activity = venture_booking_service_book_selected(service, page, start, resource, group_size,
			name, email, notes, NULL, &actor, &error);
		if (!activity) {
			guint status = g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT) ? 409 : 422;
			if (venture_web_booking_wants_json(request)) return venture_web_error_response(error);
			if (status == 409) {
				g_autoptr(GString) message = g_string_new("<p>");
				venture_html_escape_append(message, error != NULL ? error->message : "That time is unavailable");
				g_string_append(message, "</p>");
				return venture_web_booking_plain(status, message->str);
			}
			return venture_web_booking_plain(status, "<p>Please check the name, email and time.</p>");
		}
		venture_web_booking_note_address(venture_context_get_database(self->context), activity, address);
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
		g_autofree gchar *title = NULL, *description = NULL, *action = NULL, *selection = NULL, *resources = NULL;
		JsonArray *array = json_node_get_array(slots);
		guint i;
		g_object_get(page, "title", &title, "description", &description,
			"resource-selection", &selection, "resources", &resources, NULL);
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
				JsonArray *available = json_object_has_member(slot, "resources") ? json_object_get_array_member(slot, "resources") : NULL;
				guint j, choices = g_strcmp0(selection, "pick") == 0 && available != NULL ? json_array_get_length(available) : 1;
				for (j = 0; j < choices; j++) {
					const gchar *name = available != NULL && g_strcmp0(selection, "pick") == 0 ? json_array_get_string_element(available, j) : NULL;
				g_string_append(html, "<option value=\"");
				venture_html_escape_append(html, venture_json_object_get_string(slot, "start", ""));
				if (name != NULL) { g_string_append(html, "|"); venture_html_escape_append(html, name); }
				g_string_append(html, "\">");
				venture_html_escape_append(html, venture_json_object_get_string(slot, "label", ""));
				if (name != NULL) { g_string_append(html, " — "); venture_html_escape_append(html, name); }
				g_string_append(html, "</option>");
				}
			}
			g_string_append(html, "</select></label>");
			if (!venture_string_is_empty(resources)) g_string_append(html, "<label>Group size <input name=\"group_size\" type=\"number\" min=\"1\" max=\"10000\" value=\"1\" required></label>");
			g_string_append(html, "<label>Name <input name=\"name\" required></label><label>Email <input name=\"email\" type=\"email\" required></label>"
				"<label>Notes <textarea name=\"notes\"></textarea></label><button type=\"submit\">Book</button></form>");
		}
		g_string_append(html, "</body></html>");
		return venture_web_booking_plain(200, html->str);
	}
}

/* A GET only offers explicit actions, so mail scanners cannot cancel a meeting.
 * The token is the route's guard and is never reflected into page content. */
static HtmxResponse *venture_web_booking_manage(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureBookingService) service = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) page = NULL, reservation = NULL, activity = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) slots = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *address = venture_web_booking_client_address(self, request);
	const gchar *slug = g_hash_table_lookup(params, "slug"), *capability = g_hash_table_lookup(params, "capability");
	GBytes *bytes = htmx_request_get_body_bytes(request);
	guint i;
	if (!venture_web_module_enabled(self, "calendar")) return venture_web_booking_plain(404, "Not found");
	if (bytes != NULL && g_bytes_get_size(bytes) > 16384) return venture_web_booking_plain(413, "Too large");
	if (self->lead_limiter == NULL) self->lead_limiter = htmx_rate_limiter_new(30, 0.5);
	if (!htmx_rate_limiter_allow(self->lead_limiter, address != NULL ? address : "unknown")) return venture_web_booking_plain(429, "Try again later");
	service = venture_booking_service_new(database); query = venture_query_new(VENTURE_TYPE_BOOKING_PAGE);
	venture_query_add_filter_string(query, "slug", VENTURE_FILTER_OP_EQ, slug, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	page = venture_database_find_one(database, query, NULL);
	if (page == NULL || venture_entity_is_deleted(page)) return venture_web_booking_plain(404, "Not found");
	reservation = venture_booking_service_lookup_capability(service, page, capability, NULL, &error);
	if (reservation == NULL) return venture_web_booking_plain(404, "Not found");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *start = htmx_request_get_form_value(request, "start");
		const gchar *resource = NULL;
		g_autofree gchar *selected_start = NULL;
		VentureActor actor;
		if (g_strcmp0(operation, "cancel") != 0 && g_strcmp0(operation, "reschedule") != 0)
			return venture_web_booking_plain(422, "Choose cancellation or rescheduling.");
		if (g_strcmp0(operation, "reschedule") == 0 && venture_string_is_empty(start))
			return venture_web_booking_plain(422, "Choose an offered time.");
		if (start != NULL && strchr(start, '|') != NULL) {
			const gchar *separator = strchr(start, '|');
			selected_start = g_strndup(start, separator - start);
			resource = separator + 1; start = selected_start;
		}
		actor.kind = VENTURE_ACTOR_KIND_USER; actor.name = "public booking";
		actor.prompt = NULL; actor.request_id = NULL; actor.approved_by = NULL;
		activity = venture_booking_service_manage_selected(service, page, capability,
			g_strcmp0(operation, "cancel") == 0 ? NULL : start, resource, NULL, &actor, &error);
		if (activity == NULL) return venture_web_booking_plain(g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND) ? 404 : 409,
			"The booking could not be changed. Reload and choose an available time.");
		venture_web_booking_note_address(database, activity, address);
		if (g_strcmp0(operation, "cancel") == 0) return venture_web_booking_plain(200, "<p>Your booking has been cancelled.</p>");
		return venture_web_booking_plain(200, "<p>Your booking has been rescheduled. A new private link will be sent when confirmation email is configured.</p>");
	}
	slots = venture_booking_service_slots(service, page, NULL, &error);
	html = g_string_new("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Manage booking</title></head><body><main><h1>Manage booking</h1><form method=\"post\"><button name=\"operation\" value=\"cancel\">Cancel booking</button></form>");
	if (slots != NULL && json_array_get_length(json_node_get_array(slots)) > 0)
	{
		g_string_append(html, "<form method=\"post\"><label>New time <select name=\"start\" required>");
		for (i = 0; i < json_array_get_length(json_node_get_array(slots)); i++)
		{
			JsonObject *slot = json_array_get_object_element(json_node_get_array(slots), i);
			JsonArray *available = json_object_has_member(slot, "resources") ? json_object_get_array_member(slot, "resources") : NULL;
			guint j, choices = available != NULL ? json_array_get_length(available) : 1;
			for (j = 0; j < choices; j++) {
				const gchar *name = available != NULL ? json_array_get_string_element(available, j) : NULL;
				g_string_append(html, "<option value=\""); venture_html_escape_append(html, json_object_get_string_member(slot, "start"));
				if (name != NULL) { g_string_append(html, "|"); venture_html_escape_append(html, name); }
				g_string_append(html, "\">"); venture_html_escape_append(html, json_object_get_string_member(slot, "label"));
				if (name != NULL) { g_string_append(html, " — "); venture_html_escape_append(html, name); }
				g_string_append(html, "</option>");
			}
		}
		g_string_append(html, "</select></label><button name=\"operation\" value=\"reschedule\">Reschedule booking</button></form>");
	}
	g_string_append(html, "</main></body></html>");
	return venture_web_booking_plain(200, html->str);
}
