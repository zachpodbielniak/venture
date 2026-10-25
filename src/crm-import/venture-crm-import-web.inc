/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_crm_import_web_preview(HtmxRequest *request, GHashTable *params, gpointer user_data)
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
	gate = venture_web_require_module_api(self, "crm_import");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "CRM import preview requires a JSON manifest");
		return venture_web_error_response(error);
	}
	/* The selected legal entity, as cutover: a migration lands in one
	 * organization, and "all" is refused by the service. */
	result = venture_crm_import_service_preview(venture_crm_import_service_get(venture_context_get_database(self->context)),
		venture_web_active_organization(self, request), json_node_get_object(body), &actor, &error);
	if (result == NULL) return venture_web_error_response(error);
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static HtmxResponse *
venture_crm_import_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
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
	gate = venture_web_require_module_api(self, "crm_import");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	empty = g_hash_table_new(g_str_hash, g_str_equal);
	result = venture_action_registry_perform(venture_database_get_action_registry(venture_context_get_database(self->context)),
		"crm_import", id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, action, empty, &actor,
		VENTURE_USER_ROLE_EDITOR, &error);
	g_hash_table_unref(empty);
	if (result == NULL) return venture_web_error_response(error);
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static HtmxResponse *
venture_crm_import_web_attachment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *encoded = NULL, *kind = NULL, *name = NULL;
	g_autofree gchar *escaped = NULL, *disposition = NULL;
	g_autofree guchar *data = NULL;
	const gchar *id = g_hash_table_lookup(params, "id");
	gsize length;
	HtmxResponse *response = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (response != NULL) return response;
	response = venture_web_require_module_api(self, "crm_import");
	if (response != NULL) return response;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_require_for_type(self, principal, VENTURE_TYPE_DOCUMENT, VENTURE_USER_ROLE_VIEWER, &error))
		return venture_web_error_response(error);
	document = venture_database_get(database, VENTURE_TYPE_DOCUMENT, id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, NULL);
	if (document == NULL || venture_entity_is_deleted(document) ||
		!venture_access_policy_check_read(venture_database_get_access_policy(database), document, NULL))
		goto missing;
	g_object_get(document, "kind", &kind, "content-base64", &encoded, "title", &name, NULL);
	if (g_strcmp0(kind, "crm-import") != 0 || encoded == NULL || strlen(encoded) > 11184812)
		goto missing;
	data = g_base64_decode(encoded, &length);
	bytes = g_bytes_new_take(g_steal_pointer(&data), length);
	escaped = g_uri_escape_string(name != NULL ? name : "attachment", NULL, FALSE);
	disposition = g_strdup_printf("attachment; filename=\"attachment\"; filename*=UTF-8''%s", escaped);
	response = htmx_response_new();
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, "application/octet-stream");
	htmx_response_add_header(response, "Content-Disposition", disposition);
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
	return response;
missing:
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Attachment not found");
	return venture_web_error_response(error);
}

static void
venture_crm_import_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/crm_imports/attachments/:id", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_crm_import_web_attachment, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/crm_imports/preview", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_crm_import_web_preview, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/crm_imports/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_crm_import_web_action, self);
}
