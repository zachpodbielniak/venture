/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *attribution_web_result(JsonNode *node, GError *error, const gchar *origin)
{
	HtmxResponse *response = error ? venture_web_error_response(error) : venture_web_json_response(node, 200);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
		htmx_response_set_status(response, 429); htmx_response_add_header(response, "Retry-After", "60");
	}
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	if (origin) { htmx_response_add_header(response, "Access-Control-Allow-Origin", origin); htmx_response_add_header(response, "Vary", "Origin"); }
	return response;
}
static HtmxResponse *venture_web_attribution_script(HtmxRequest *request, GHashTable *params, gpointer data)
{
	HtmxResponse *response = htmx_response_new_with_content(venture_asset_attribution_js);
	htmx_response_set_content_type(response, "application/javascript; charset=utf-8");
	htmx_response_add_header(response, "Cache-Control", "public, max-age=300");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	return response;
}
static const gchar *attribution_web_string(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
static HtmxResponse *venture_web_attribution_ingest(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureAttributionService *service = venture_attribution_service_get(database);
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(htmx_request_get_message(request));
	const gchar *origin = soup_message_headers_get_one(headers, "Origin"), *site_uuid = g_hash_table_lookup(params, "site");
	const gchar *operation = g_hash_table_lookup(params, "operation");
	g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureEntity) site = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ATTRIBUTION_SITE);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *expected = NULL;
	GBytes *body = htmx_request_get_body_bytes(request);
	const gchar *bytes;
	gsize length;
	JsonObject *payload;
	gboolean recognized = !g_strcmp0(operation, "grant") || !g_strcmp0(operation, "observe") || !g_strcmp0(operation, "capture") || !g_strcmp0(operation, "withdraw");
	if (!recognized || !site_uuid || !g_uuid_string_is_valid(site_uuid) || !origin || strlen(origin) > 2048) goto missing;
	venture_query_set_include_deleted(query, TRUE); venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, site_uuid, NULL);
	site = venture_database_find_one(database, query, NULL); if (!site) goto missing;
	g_object_get(site, "origin", &expected, NULL);
	if (g_strcmp0(origin, expected)) goto missing;
	/* Simple text/plain POSTs need no preflight. Every write still checks an
	 * exact Origin and its capability; no cookie or caller organization is used. */
	if (!body || g_bytes_get_size(body) > 32768) goto malformed;
	bytes = g_bytes_get_data(body, &length);
	if (!length || memchr(bytes, 0, length) || !json_parser_load_from_data(parser, bytes, length, NULL) ||
		!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) goto malformed;
	payload = json_node_get_object(json_parser_get_root(parser));
	if (!strcmp(operation, "grant")) {
		if (json_object_get_size(payload) != 1) goto malformed;
		result = venture_attribution_service_grant(service, site_uuid, origin, attribution_web_string(payload, "policy"), now, &error);
	} else if (!strcmp(operation, "observe")) {
		g_autoptr(VentureAttributionTouch) touch = NULL;
		JsonNode *fields = json_object_get_member(payload, "fields");
		if (json_object_get_size(payload) != 3 || !fields || !JSON_NODE_HOLDS_OBJECT(fields)) goto malformed;
		touch = venture_attribution_service_observe(service, site_uuid, origin, attribution_web_string(payload, "token"),
			attribution_web_string(payload, "event_id"), json_node_get_object(fields), now, &error);
	} else if (!strcmp(operation, "withdraw")) {
		if (json_object_get_size(payload) != 1) goto malformed;
		venture_attribution_service_withdraw(service, site_uuid, attribution_web_string(payload, "token"), now, &error);
	} else {
		g_autoptr(VentureAttributionSubmission) submission = venture_attribution_service_capture(service, site_uuid, origin, payload, now, &error);
	}
	if (!result && !error) result = json_from_string("{\"accepted\":true}", NULL);
	return attribution_web_result(result, error, expected);
missing:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Attribution endpoint unavailable");
	return attribution_web_result(NULL, error, NULL);
malformed:
	venture_set_error_validation(&error, "attribution", "Expected documented bounded JSON payload without embedded NUL");
	return attribution_web_result(NULL, error, expected);
}
static HtmxResponse *venture_web_lightsite_receive(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(htmx_request_get_message(request));
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAttributionSubmission) submission = NULL;
	g_autoptr(JsonNode) result = json_from_string("{\"accepted\":true}", NULL);
	const gchar *id = g_hash_table_lookup(params, "connection");
	gint64 connection;
	if (!id || !g_ascii_string_to_signed(id, 10, 1, G_MAXINT64, &connection, NULL))
		venture_set_error_validation(&error, "connection", "Expected exact Lightsite connection identity");
	else submission = venture_attribution_service_receive(venture_attribution_service_get(venture_context_get_database(self->context)),
		g_hash_table_lookup(params, "site"), connection, soup_message_headers_get_one(headers, "X-Venture-Timestamp"),
		soup_message_headers_get_one(headers, "X-Venture-Signature"), htmx_request_get_body_bytes(request), now, &error);
	return attribution_web_result(result, error, NULL);
}
