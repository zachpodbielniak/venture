/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* POST /api/v1/lightsite/bookings/:id/move: Lightsite's daemon moving a
 * confirmed booking because its business owner rescheduled it. :id is the
 * booking's meeting; the body is exactly {"organization_id": N,
 * "starts_at": "<ISO 8601>"}, at most 4096 bytes. Only the trusted service
 * token reaches the booking service; a browser session is refused as no
 * service credential. */
static HtmxResponse *
lightsite_booking_finish(HtmxResponse *response)
{
	return account_request_finish(response, "Lightsite booking move");
}

static HtmxResponse *
venture_web_lightsite_booking_move(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureBookingService) service = NULL;
	g_autoptr(VentureEntity) moved = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GDateTime) when = NULL, starts = NULL, ends = NULL;
	g_autofree gchar *starts_text = NULL, *ends_text = NULL;
	JsonObject *object, *answer;
	JsonNode *root, *organization, *start;
	g_autoptr(JsonNode) result = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	const gchar *bytes = NULL;
	gsize length = 0;
	gint64 activity_id = 0, organization_id;
	VentureActor actor;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (response) return lightsite_booking_finish(response);
	principal = venture_auth_authenticate(self->auth, request);
	if (principal->token_id <= 0) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED, "A service bearer token is required");
		return lightsite_booking_finish(venture_web_error_response(error));
	}
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &activity_id, NULL)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A positive booking ID is required");
		return lightsite_booking_finish(venture_web_error_response(error));
	}
	if (body) bytes = g_bytes_get_data(body, &length);
	/* An escaped NUL would silently truncate the time once it is a C string. */
	if (!bytes || !length || length > 4096 || memchr(bytes, '\0', length) || g_strstr_len(bytes, (gssize)length, "\\u0000") ||
	    !json_parser_load_from_data(parser, bytes, (gssize)length, NULL)) goto malformed;
	root = json_parser_get_root(parser);
	if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto malformed;
	object = json_node_get_object(root);
	organization = json_object_get_member(object, "organization_id");
	start = json_object_get_member(object, "starts_at");
	if (json_object_get_size(object) != 2 || !organization || !start ||
	    !JSON_NODE_HOLDS_VALUE(organization) || json_node_get_value_type(organization) != G_TYPE_INT64 ||
	    !JSON_NODE_HOLDS_VALUE(start) || json_node_get_value_type(start) != G_TYPE_STRING) goto malformed;
	organization_id = json_node_get_int(organization);
	when = venture_time_from_string(json_node_get_string(start), NULL);
	if (organization_id <= 0 || when == NULL) goto malformed;
	scope = venture_access_policy_enter(policy, principal);
	if (!venture_tenant_service_check_trusted_service(venture_tenant_service_get(database), NULL, &error))
		return lightsite_booking_finish(venture_web_error_response(error));
	venture_auth_to_actor(principal, &actor);
	/* The trusted service acts for the business, not as one of its members. */
	internal = venture_access_policy_enter(policy, NULL);
	service = venture_booking_service_new(database);
	moved = venture_booking_service_move(service, organization_id, activity_id, json_node_get_string(start), NULL, &actor, &error);
	if (!moved) return lightsite_booking_finish(venture_web_error_response(error));
	g_object_get(moved, "starts-at", &starts, "ends-at", &ends, NULL);
	starts_text = venture_time_to_string(starts);
	ends_text = venture_time_to_string(ends);
	answer = json_object_new();
	json_object_set_string_member(answer, "starts_at", starts_text);
	json_object_set_string_member(answer, "ends_at", ends_text);
	result = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(result, answer);
	return lightsite_booking_finish(venture_web_json_response(result, 200));
malformed:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Expected a JSON object of exactly a positive organization_id and an ISO 8601 starts_at");
	return lightsite_booking_finish(venture_web_error_response(error));
}
