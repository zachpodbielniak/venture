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
	gint64 organization_id;
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
	organization_id = venture_web_active_organization(self, request);
	body = htmx_request_get_json(request, &error);
	if (error != NULL) return venture_web_error_response(error);
	if (body != NULL)
	{
		JsonObject *values;
		JsonNode *selection;
		if (!JSON_NODE_HOLDS_OBJECT(body))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Commerce import requires a JSON object");
			return venture_web_error_response(error);
		}
		values = json_node_get_object(body);
		name = venture_json_object_get_string(values, "connector", NULL);
		selection = json_object_get_member(values, "organization_id");
		if (selection != NULL)
		{
			if (!JSON_NODE_HOLDS_VALUE(selection) || json_node_get_value_type(selection) != G_TYPE_INT64 || json_node_get_int(selection) <= 0)
			{
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a positive integer organization_id");
				return venture_web_error_response(error);
			}
			organization_id = json_node_get_int(selection);
		}
	}
	imported = venture_commerce_service_import_for_organization(service, organization_id, name, NULL, NULL, &actor, &error);
	if (imported < 0) return venture_web_error_response(error);
	json_object_set_int_member(object, "imported", imported);
	json_node_take_object(node, g_steal_pointer(&object));
	return venture_web_json_response(node, 200);
}

static HtmxResponse *
commerce_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureCommerceService) service = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) content = g_string_new("<h1>Commerce account settings</h1>");
	g_autofree gchar *shop = NULL;
	gint64 org = 0, expected_id = 0, expected_version = 0;
	gboolean failed = FALSE;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "commerce");
	if (gate != NULL) return gate;
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &org, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database), principal, org, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required");
		return venture_web_error_response(error);
	}
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, org, &error);
	if (organization == NULL) return venture_web_error_response(error);
	if (venture_entity_is_deleted(organization))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization is unavailable");
		return venture_web_error_response(error);
	}
	service = venture_commerce_service_new(database, org, NULL, &error);
	if (service == NULL) return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		GBytes *bytes = htmx_request_get_body_bytes(request);
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *id_text = htmx_request_get_form_value(request, "connection_id");
		const gchar *version_text = htmx_request_get_form_value(request, "version");
		if (bytes != NULL && g_bytes_get_size(bytes) > 16384)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Commerce settings request exceeds 16 KiB");
		else if (id_text == NULL || version_text == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reload the account settings before applying this operation");
		else if (g_ascii_string_to_signed(id_text, 10, 0, G_MAXINT64, &expected_id, &error) &&
			g_ascii_string_to_signed(version_text, 10, 0, G_MAXINT64, &expected_version, &error))
		{
			if (g_strcmp0(operation, "configure") == 0)
				binding = venture_commerce_service_configure_shopify(service, org,
					htmx_request_get_form_value(request, "shop"), htmx_request_get_form_value(request, "access_token"),
					expected_id, expected_version, &actor, &error);
			else if (g_strcmp0(operation, "disconnect") == 0)
			{
				if (venture_commerce_service_disconnect(service, org, expected_id, expected_version, &actor, &error))
					g_string_append(content, "<p>Disconnected. Historical import identity is retained.</p>");
			}
			else if (g_strcmp0(operation, "test") == 0)
			{
				binding = venture_integration_service_find(integrations, org, "commerce.shopify", &error);
				if (binding && (venture_entity_get_id(VENTURE_ENTITY(binding)) != expected_id ||
					venture_entity_get_version(VENTURE_ENTITY(binding)) != expected_version))
					g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Commerce account changed; reload settings before testing");
				if (error == NULL && venture_commerce_service_test_shopify(service, org, &error))
					g_string_append(content, "<p>Shopify authenticated the configured shop identity. No orders were imported.</p>");
			}
			else g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose connect, rotate, test or disconnect");
		}
		failed = error != NULL;
		if (error != NULL)
		{
			g_string_append(content, "<p role=\"alert\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</p>");
			g_clear_error(&error);
		}
		g_clear_object(&binding);
	}
	binding = venture_integration_service_find(integrations, org, "commerce.shopify", NULL);
	if (binding != NULL)
	{
		g_object_get(binding, "account-id", &shop, NULL);
		g_string_append(content, "<p>Configured Shopify account: ");
		venture_html_escape_append(content, shop);
		g_string_append_printf(content, "</p><p><a href=\"/e/integration_connection/%" G_GINT64_FORMAT "\">Account record and legacy adoption action</a></p>", venture_entity_get_id(VENTURE_ENTITY(binding)));
	}
	else g_string_append(content, "<p>No Shopify account is configured for this organization.</p>");
	g_string_append(content, "<p>Credentials are write-only. Supply the complete shop hostname and token for connect or rotation. Disconnect before changing shops. Configuration does not verify provider access; use Test connection explicitly.</p><form method=\"post\">");
	g_string_append_printf(content, "<input type=\"hidden\" name=\"connection_id\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">",
		binding ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0, binding ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0);
	g_string_append(content, "<label>Shop hostname<input name=\"shop\" placeholder=\"your-shop.myshopify.com\" maxlength=\"76\" autocomplete=\"off\"></label><label>Access token<input name=\"access_token\" type=\"password\" maxlength=\"4096\" autocomplete=\"new-password\"></label><button name=\"operation\" value=\"configure\">Connect or rotate</button>");
	if (binding) g_string_append(content, "<button name=\"operation\" value=\"test\">Test connection</button><button name=\"operation\" value=\"disconnect\">Disconnect</button>");
	g_string_append(content, "</form>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Commerce settings", content->str), failed ? 400 : 200);
}
