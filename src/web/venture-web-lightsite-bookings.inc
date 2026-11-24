/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* POST /api/v1/lightsite/bookings/:id/move and /cancel: Lightsite's daemon
 * changing a confirmed booking because its business owner rescheduled or
 * cancelled it. :id is the booking's meeting; the body is exactly
 * {"organization_id": N, "starts_at": "<ISO 8601>"} for a move and
 * {"organization_id": N} for a cancel, at most 4096 bytes. Only the trusted
 * service token reaches the booking service; a browser session is refused as
 * no service credential. The 200 answer carries "booking", the object the
 * signed booking hook body carries, read after the change: the caller writes
 * the customer's letter and its token cannot read booking records. */
static HtmxResponse *
lightsite_booking_finish(HtmxResponse *response, gboolean move)
{
	return account_request_finish(response, move ? "Lightsite booking move" : "Lightsite booking cancel");
}

/* The change is committed, so a failed read answers null, never an error: a
 * retry would be refused, the booking having already changed. */
static JsonNode *
lightsite_booking_details(VentureDatabase *database, VentureBookingService *service, gint64 organization_id,
	gint64 activity_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BOOKING_RESERVATION);
	g_autoptr(VentureEntity) reservation = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *details = NULL;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_int(query, "activity-id", VENTURE_FILTER_OP_EQ, activity_id, NULL);
	reservation = venture_database_find_one(database, query, &error);
	if (reservation) details = venture_booking_service_hook_details(service, venture_entity_get_id(reservation), &error);
	if (details) return details;
	/* A miss here is the documented 200 with booking null. g_warning is
	 * fatal under the test harness, so this must not be one. */
	g_message("Lightsite booking %" G_GINT64_FORMAT ": no booking details: %s", activity_id,
		error ? error->message : "no reservation");
	return json_node_new(JSON_NODE_NULL);
}

static HtmxResponse *
lightsite_booking_change(VentureWebServer *self, HtmxRequest *request, GHashTable *params, gboolean move)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureBookingService) service = NULL;
	g_autoptr(VentureEntity) changed = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GDateTime) when = NULL, starts = NULL, ends = NULL;
	g_autofree gchar *starts_text = NULL, *ends_text = NULL;
	JsonObject *object, *answer;
	JsonNode *root, *organization, *start = NULL;
	g_autoptr(JsonNode) result = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	const gchar *bytes = NULL;
	gsize length = 0;
	gint64 activity_id = 0, organization_id;
	VentureActor actor;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (response) return lightsite_booking_finish(response, move);
	principal = venture_auth_authenticate(self->auth, request);
	if (principal->token_id <= 0) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED, "A service bearer token is required");
		return lightsite_booking_finish(venture_web_error_response(error), move);
	}
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &activity_id, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive booking ID is required");
		return lightsite_booking_finish(venture_web_error_response(error), move);
	}
	if (body) bytes = g_bytes_get_data(body, &length);
	/* An escaped NUL would silently truncate the time once it is a C string. */
	if (!bytes || !length || length > 4096 || memchr(bytes, '\0', length) || g_strstr_len(bytes, (gssize)length, "\\u0000") ||
	    !json_parser_load_from_data(parser, bytes, (gssize)length, NULL)) goto malformed;
	root = json_parser_get_root(parser);
	if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto malformed;
	object = json_node_get_object(root);
	organization = json_object_get_member(object, "organization_id");
	if (json_object_get_size(object) != (move ? 2u : 1u) || !organization ||
	    !JSON_NODE_HOLDS_VALUE(organization) || json_node_get_value_type(organization) != G_TYPE_INT64) goto malformed;
	organization_id = json_node_get_int(organization);
	if (organization_id <= 0) goto malformed;
	if (move) {
		start = json_object_get_member(object, "starts_at");
		if (!start || !JSON_NODE_HOLDS_VALUE(start) || json_node_get_value_type(start) != G_TYPE_STRING) goto malformed;
		when = venture_time_from_string(json_node_get_string(start), NULL);
		if (when == NULL) goto malformed;
	}
	scope = venture_access_policy_enter(policy, principal);
	if (!venture_tenant_service_check_trusted_service(venture_tenant_service_get(database), NULL, &error))
		return lightsite_booking_finish(venture_web_error_response(error), move);
	venture_auth_to_actor(principal, &actor);
	/* The trusted service acts for the business, not as one of its members. */
	internal = venture_access_policy_enter(policy, NULL);
	service = venture_booking_service_new(database);
	changed = move
		? venture_booking_service_move(service, organization_id, activity_id, json_node_get_string(start), NULL, &actor, &error)
		: venture_booking_service_cancel(service, organization_id, activity_id, &actor, &error);
	if (!changed) return lightsite_booking_finish(venture_web_error_response(error), move);
	answer = json_object_new();
	if (move) {
		g_object_get(changed, "starts-at", &starts, "ends-at", &ends, NULL);
		starts_text = venture_time_to_string(starts);
		ends_text = venture_time_to_string(ends);
		json_object_set_string_member(answer, "starts_at", starts_text);
		json_object_set_string_member(answer, "ends_at", ends_text);
	}
	json_object_set_member(answer, "booking", lightsite_booking_details(database, service, organization_id, activity_id));
	result = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(result, answer);
	return lightsite_booking_finish(venture_web_json_response(result, 200), move);
malformed:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, move
		? "Expected a JSON object of exactly a positive organization_id and an ISO 8601 starts_at"
		: "Expected a JSON object of exactly a positive organization_id");
	return lightsite_booking_finish(venture_web_error_response(error), move);
}

static HtmxResponse *
venture_web_lightsite_booking_move(HtmxRequest *request, GHashTable *params, gpointer data)
{
	return lightsite_booking_change(data, request, params, TRUE);
}

static HtmxResponse *
venture_web_lightsite_booking_cancel(HtmxRequest *request, GHashTable *params, gpointer data)
{
	return lightsite_booking_change(data, request, params, FALSE);
}
