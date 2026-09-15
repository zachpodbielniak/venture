/* SPDX-License-Identifier: AGPL-3.0-or-later */
static JsonObject *
document_spec_from_request(HtmxRequest *request, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) body = NULL;
	const gchar *form = htmx_request_get_form_value(request, "payload");
	if (form != NULL)
	{
		if (!json_parser_load_from_data(parser, form, -1, error))
			return NULL;
		body = json_node_copy(json_parser_get_root(parser));
	}
	else
		body = htmx_request_get_json(request, error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_clear_error(error);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Compose requires a JSON object");
		return NULL;
	}
	return json_object_ref(json_node_get_object(body));
}

static HtmxResponse *
venture_document_web_compose(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonObject) spec = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	HtmxResponse *gate;
	gboolean quote = strstr(htmx_request_get_path(request), "quote") != NULL;
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	(void)params;
	if (htmx_request_get_method(request) == HTMX_METHOD_GET)
	{
		g_autoptr(GString) html = g_string_new("<!doctype html><html><head><meta charset=\"utf-8\"><title>");
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
		g_string_append(html, quote ? "Guided quote" : "Guided invoice");
		g_string_append(html, "</title></head><body><h1>");
		g_string_append(html, quote ? "Guided quote" : "Guided invoice");
		g_string_append(html, "</h1><p>Add lines, tax and send on one page.</p>"
			"<form method=\"post\"><label>JSON<textarea name=\"payload\" required>"
			"{\"company_id\":1,\"send\":true,\"lines\":[{\"description\":\"Work\",\"quantity\":1,\"unit_price\":\"100 USD\",\"tax_percent\":0}]}</textarea></label>"
			"<button>Save</button></form></body></html>");
		return htmx_response_new_with_content(html->str);
	}
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, quote ? "quotes" : "invoicing");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	spec = document_spec_from_request(request, &error);
	if (spec == NULL)
		return venture_web_error_response(error);
	if (quote)
		result = venture_document_service_compose_quote(venture_document_service_get(venture_context_get_database(self->context)),
			venture_context_get_default_organization_id(self->context), spec, &actor, &error);
	else
		result = venture_document_service_compose_invoice(venture_document_service_get(venture_context_get_database(self->context)),
			venture_context_get_default_organization_id(self->context), spec, &actor, &error);
	if (result == NULL)
		return venture_web_error_response(error);
	if (!api)
	{
		g_autofree gchar *path = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, quote ? "quote" : "invoice",
			venture_entity_get_id(result));
		return venture_web_redirect_to(path);
	}
	return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE), 200);
}

static void
venture_document_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_get(router, "/invoices/compose", venture_document_web_compose, self);
	htmx_router_post(router, "/invoices/compose", venture_document_web_compose, self);
	htmx_router_post(router, "/api/v1/invoices/compose", venture_document_web_compose, self);
	htmx_router_get(router, "/quotes/compose", venture_document_web_compose, self);
	htmx_router_post(router, "/quotes/compose", venture_document_web_compose, self);
	htmx_router_post(router, "/api/v1/quotes/compose", venture_document_web_compose, self);
}
