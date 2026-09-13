/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_autojournal_backfill(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	JsonObject *body;
	gint64 org;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "autojournal");
	if (gate != NULL) return gate;
	if (!json_parser_load_from_data(parser, htmx_request_get_body(request), -1, &error))
		return venture_web_error_response(error);
	if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Backfill expects a JSON object");
		return venture_web_error_response(error);
	}
	body = json_node_get_object(json_parser_get_root(parser));
	org = venture_json_object_get_int(body, "organization_id", 0);
	if (org == 0) org = venture_context_get_default_organization_id(self->context);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	result = venture_autojournal_service_backfill(venture_database_get_autojournal_service(
		venture_context_get_database(self->context)), org,
		venture_json_object_get_bool(body, "dry_run", FALSE), &actor, &error);
	return result != NULL ? venture_web_json_response(result, 200) : venture_web_error_response(error);
}
