/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_cutover_web_preview(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *form = htmx_request_get_form_value(request, "payload");
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "cutover");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form != NULL)
	{
		if (!json_parser_load_from_data(parser, form, -1, &error))
			return venture_web_error_response(error);
		body = json_node_copy(json_parser_get_root(parser));
	}
	else
		body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Cutover preview requires a JSON object");
		return venture_web_error_response(error);
	}
	result = venture_cutover_service_preview(venture_cutover_service_get(venture_context_get_database(self->context)),
		venture_context_get_default_organization_id(self->context), json_node_get_object(body), &actor, &error);
	if (result == NULL) return venture_web_error_response(error);
	if (form != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_cutover/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static HtmxResponse *
venture_cutover_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *id = g_hash_table_lookup(params, "id");
	const gchar *form_action = htmx_request_get_form_value(request, "action");
	GHashTable *empty;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "cutover");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form_action != NULL) action = form_action;
	empty = g_hash_table_new(g_str_hash, g_str_equal);
	result = venture_action_registry_perform(venture_database_get_action_registry(venture_context_get_database(self->context)),
		"accounting_cutover", id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, action, empty, &actor,
		VENTURE_USER_ROLE_EDITOR, &error);
	g_hash_table_unref(empty);
	if (result == NULL) return venture_web_error_response(error);
	if (form_action != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_cutover/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static void
venture_cutover_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_post(router, "/api/v1/accounting_cutovers/preview", venture_cutover_web_preview, self);
	htmx_router_post(router, "/cutover/preview", venture_cutover_web_preview, self);
	htmx_router_post(router, "/api/v1/accounting_cutovers/:id/:action", venture_cutover_web_action, self);
	htmx_router_post(router, "/cutover/:id/action", venture_cutover_web_action, self);
}
