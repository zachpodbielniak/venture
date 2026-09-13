/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included by the web server so authentication and response policy stay shared. */
static HtmxResponse *
venture_bank_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL, node = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *id = g_hash_table_lookup(params, "id");
	const gchar *form = htmx_request_get_form_value(request, "parameters");
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "banking");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form != NULL)
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		if (!json_parser_load_from_data(parser, form, -1, &error)) return venture_web_error_response(error);
		body = json_node_copy(json_parser_get_root(parser));
	}
	else body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Bank action parameters must be a JSON object");
		return venture_web_error_response(error);
	}
	if (form != NULL) action = htmx_request_get_form_value(request, "action");
	result = venture_bank_match_service_execute(venture_database_get_bank_match_service(venture_context_get_database(self->context)),
		action, id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, json_node_get_object(body), &actor, &error);
	if (result == NULL) return venture_web_error_response(error);
	if (form != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT,
			venture_entity_get_entity_name(result), venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE);
	return venture_web_json_response(node, 200);
}

static void
venture_bank_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_post(router, "/api/v1/bank_transactions/:id/:action", venture_bank_web_action, self);
	htmx_router_post(router, "/api/v1/bank_statements/:id/:action", venture_bank_web_action, self);
	htmx_router_post(router, "/api/v1/bank_accounts/:id/:action", venture_bank_web_action, self);
	htmx_router_post(router, "/banking/:id/action", venture_bank_web_action, self);
}
