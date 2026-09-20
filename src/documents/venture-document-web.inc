/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Restrict the guided workflow to inputs accepted by the shared composer; field types stay metadata-derived. */
static const gchar *const document_header_fields[] = { "number", "company-id", "contact-id", "terms", "notes", NULL };
static const gchar *const document_line_fields[] = { "description", "quantity", "unit-price", "product-id", "discount-percent", "tax-percent", NULL };

static gboolean
document_read_fields(HtmxRequest *request, VentureEntity *record, const gchar *const *fields,
	const gchar *prefix, GError **error)
{
	guint i;
	for (i = 0; fields[i] != NULL; i++)
	{
		g_autofree gchar *name = g_strconcat(prefix, fields[i], NULL);
		const gchar *value = htmx_request_get_form_value(request, name);
		if (value != NULL && value[0] != '\0' &&
			!venture_entity_set_field_from_string(record, fields[i], value, error))
			return FALSE;
	}
	return TRUE;
}

static void
document_append_fields(VentureWebServer *self, GString *html, VentureEntity *record,
	const gchar *const *fields, const gchar *prefix)
{
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(record);
	guint i;
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		g_autoptr(VentureFieldSpec) input = NULL;
		if (!g_strv_contains(fields, spec->name) || (spec->flags & VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;
		input = venture_field_spec_copy(spec);
		g_free(input->name);
		input->name = g_strconcat(prefix, spec->name, NULL);
		/* Empty optional line groups are ignored; the composer validates completed groups. */
		input->required = FALSE;
		venture_web_append_form_field(self, html, input, NULL);
	}
}

static JsonObject *
document_spec_from_request(HtmxRequest *request, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) body = NULL;
	const gchar *form = htmx_request_get_form_value(request, "payload");
	if (htmx_request_get_form_value(request, "compose-form") != NULL)
	{
		gboolean quote = strstr(htmx_request_get_path(request), "quote") != NULL;
		g_autoptr(VentureEntity) header = g_object_new(quote ? VENTURE_TYPE_QUOTE : VENTURE_TYPE_INVOICE, NULL);
		g_autoptr(JsonNode) node = NULL;
		JsonObject *spec;
		JsonArray *lines = json_array_new();
		guint i;
		if (!document_read_fields(request, header, document_header_fields, "", error))
		{
			json_array_unref(lines);
			return NULL;
		}
		node = venture_serializable_to_json(VENTURE_SERIALIZABLE(header), FALSE);
		spec = json_object_ref(json_node_get_object(node));
		json_object_set_array_member(spec, "lines", lines);
		json_object_set_boolean_member(spec, "send", htmx_request_get_form_value(request, "send") != NULL);
		for (i = 0; i < 5; i++)
		{
			g_autofree gchar *prefix = g_strdup_printf("line-%u-", i);
			g_autofree gchar *description_name = g_strconcat(prefix, "description", NULL);
			const gchar *description = htmx_request_get_form_value(request, description_name);
			g_autoptr(VentureEntity) line = NULL;
			g_autoptr(JsonNode) line_node = NULL;
			g_autoptr(VentureMoney) price = NULL;
			g_autofree gchar *price_text = NULL;
			if (description == NULL || description[0] == '\0')
				continue;
			line = g_object_new(quote ? VENTURE_TYPE_QUOTE_LINE : VENTURE_TYPE_INVOICE_LINE, NULL);
			if (!document_read_fields(request, line, document_line_fields, prefix, error))
			{
				json_object_unref(spec);
				return NULL;
			}
			line_node = venture_serializable_to_json(VENTURE_SERIALIZABLE(line), FALSE);
			/* Record JSON represents money as an object; the compose contract
			 * accepts the exact textual amount shared with the CLI. */
			g_object_get(line, "unit-price", &price, NULL);
			if (price != NULL)
			{
				price_text = venture_money_to_string(price);
				json_object_set_string_member(json_node_get_object(line_node), "unit_price", price_text);
			}
			json_array_add_element(lines, g_steal_pointer(&line_node));
		}
		return spec;
	}
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
		g_autoptr(GString) html = g_string_new("<div class=\"page-header\"><h1>");
		g_autoptr(VentureEntity) header = g_object_new(quote ? VENTURE_TYPE_QUOTE : VENTURE_TYPE_INVOICE, NULL);
		g_autoptr(VentureEntity) line = g_object_new(quote ? VENTURE_TYPE_QUOTE_LINE : VENTURE_TYPE_INVOICE_LINE, NULL);
		guint i;
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
		gate = venture_web_require_module_ui(self, request, quote ? "quotes" : "invoicing");
		if (gate != NULL)
			return gate;
		g_string_append(html, quote ? "Guided quote" : "Guided invoice");
		g_string_append(html, "</h1></div><p>Add up to five lines. Leave unused lines empty.</p>"
			"<form method=\"post\"><input type=\"hidden\" name=\"compose-form\" value=\"1\">"
			"<div class=\"card\"><div class=\"card-body form-grid\">");
		document_append_fields(self, html, header, document_header_fields, "");
		g_string_append(html, "</div></div>");
		for (i = 0; i < 5; i++)
		{
			g_autofree gchar *prefix = g_strdup_printf("line-%u-", i);
			g_string_append_printf(html, "<fieldset class=\"card\"><legend>Line %u</legend><div class=\"card-body form-grid\">", i + 1);
			document_append_fields(self, html, line, document_line_fields, prefix);
			g_string_append(html, "</div></fieldset>");
		}
		g_string_append(html, "<label><input type=\"checkbox\" name=\"send\" value=\"1\"> Send after saving</label>"
			"<button class=\"btn btn-primary\" type=\"submit\">Save document</button></form>");
		return venture_web_html_response(venture_web_page(self, request, NULL,
			quote ? "Guided quote" : "Guided invoice", html->str), 200);
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
			venture_web_active_organization(self, request), spec, &actor, &error);
	else
		result = venture_document_service_compose_invoice(venture_document_service_get(venture_context_get_database(self->context)),
			venture_web_active_organization(self, request), spec, &actor, &error);
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
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/invoices/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/invoices/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/invoices/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/quotes/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/quotes/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/quotes/compose", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, venture_document_web_compose, self);
}
