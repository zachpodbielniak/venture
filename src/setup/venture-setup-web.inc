/* SPDX-License-Identifier: AGPL-3.0-or-later */
static JsonObject *
setup_form_payload(HtmxRequest *request)
{
	JsonObject *payload = json_object_new();
	static const gchar *const keys[] = {
		"legal_name", "book_currency", "fiscal_year_start", "basis", "tax_profile",
		"chart_template", "bank_name", "opening_cash", "period_length", NULL
	};
	gsize i;
	for (i = 0; keys[i] != NULL; i++)
	{
		const gchar *value = htmx_request_get_form_value(request, keys[i]);
		if (value != NULL && value[0] != '\0')
			json_object_set_string_member(payload, keys[i], value);
	}
	return payload;
}

static HtmxResponse *
venture_setup_web_preview(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonObject) form = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	JsonObject *payload = NULL;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "setup");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	form = setup_form_payload(request);
	if (json_object_get_size(form) > 0)
		payload = form;
	else
	{
		body = htmx_request_get_json(request, &error);
		if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
			payload = json_node_get_object(body);
	}
	result = venture_setup_service_preview(venture_setup_service_get(venture_context_get_database(self->context)),
		venture_context_get_default_organization_id(self->context), payload, &actor, &error);
	if (result == NULL) return venture_web_error_response(error);
	if (json_object_get_size(form) > 0)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_setup/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static HtmxResponse *
venture_setup_web_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) setup = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	g_autofree gchar *path = NULL;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "setup");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	setup = venture_setup_service_preview(venture_setup_service_get(venture_context_get_database(self->context)),
		venture_context_get_default_organization_id(self->context), NULL, &actor, &error);
	if (setup == NULL) return venture_web_error_response(error);
	path = g_strdup_printf("/e/accounting_setup/%" G_GINT64_FORMAT, venture_entity_get_id(setup));
	return venture_web_redirect_to(path);
}

static HtmxResponse *
venture_setup_web_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
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
	gate = venture_web_require_module_api(self, "setup");
	if (gate != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (form_action != NULL) action = form_action;
	empty = g_hash_table_new(g_str_hash, g_str_equal);
	result = venture_action_registry_perform(venture_database_get_action_registry(venture_context_get_database(self->context)),
		"accounting_setup", id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, action, empty, &actor,
		VENTURE_USER_ROLE_EDITOR, &error);
	g_hash_table_unref(empty);
	if (result == NULL) return venture_web_error_response(error);
	if (form_action != NULL)
	{
		g_autofree gchar *path = g_strdup_printf("/e/accounting_setup/%" G_GINT64_FORMAT, venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static void
venture_setup_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/setup", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_setup_web_page, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/accounting_setups/preview", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_setup_web_preview, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/setup/preview", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_setup_web_preview, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/accounting_setups/:id/:action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_setup_web_action, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/setup/:id/action", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_setup_web_action, self);
}
