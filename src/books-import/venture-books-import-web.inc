/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_books_import_web_preview(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "books_import");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Books import preview requires a JSON manifest");
		return venture_web_error_response(error);
	}
	/* A migration lands in the selected legal entity; "all" is refused. */
	result = venture_books_import_service_preview(venture_books_import_service_get(venture_context_get_database(self->context)),
		venture_web_active_organization(self, request), json_node_get_object(body), &actor, &error);
	if (result == NULL) return venture_web_error_response(error);
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static HtmxResponse *
venture_books_import_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *id = g_hash_table_lookup(params, "id");
	GHashTable *empty;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "books_import");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	empty = g_hash_table_new(g_str_hash, g_str_equal);
	result = venture_action_registry_perform(venture_database_get_action_registry(venture_context_get_database(self->context)),
		"books_import", id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, action, empty, &actor,
		VENTURE_USER_ROLE_EDITOR, &error);
	g_hash_table_unref(empty);
	if (result == NULL) return venture_web_error_response(error);
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static void
venture_books_import_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/books_imports/preview", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_books_import_web_preview, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/books_imports/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_books_import_web_action, self);
}
