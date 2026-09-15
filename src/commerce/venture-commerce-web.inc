/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
commerce_import(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) object = json_object_new();
	VentureCommerceService *service;
	VentureActor actor;
	gint imported;
	const gchar *name = NULL;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "commerce");
	if (gate != NULL) return gate;
	service = venture_context_get_commerce_service(self->context);
	if (service == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Commerce module is disabled (commerce.enabled)");
		return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	body = htmx_request_get_json(request, NULL);
	if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
		name = venture_json_object_get_string(json_node_get_object(body), "connector", NULL);
	imported = venture_commerce_service_import(service, name, NULL, NULL, &actor, &error);
	if (imported < 0) return venture_web_error_response(error);
	json_object_set_int_member(object, "imported", imported);
	json_node_take_object(node, g_steal_pointer(&object));
	return venture_web_json_response(node, 200);
}
