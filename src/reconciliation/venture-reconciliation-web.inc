/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_api_reconciliation_suggest(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *input;
	VentureActor actor;
	HtmxResponse *gate;
	GType type;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "reconciliation");
	if (gate != NULL) return gate;
	body = htmx_request_get_json(request, &error);
	if (body == NULL) return venture_web_error_response(error);
	if (!JSON_NODE_HOLDS_OBJECT(body))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Reconciliation arguments must be an object");
		return venture_web_error_response(error);
	}
	input = json_node_get_object(body);
	principal = venture_auth_authenticate(self->auth, request);
	type = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context), venture_json_object_get_string(input, "type", ""));
	if (type != G_TYPE_INVALID && !venture_web_require_for_type(self, principal, type, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	result = venture_reconciliation_service_request(venture_context_get_reconciliation_service(self->context), input, &actor, "rest-api", &error);
	if (result == NULL) return venture_web_error_response(error);
	return venture_web_json_response(result, 200);
}
