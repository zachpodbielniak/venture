/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *venture_web_attribution_settings(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(JsonNode) schema = venture_attribution_settings_schema();
	g_autoptr(GString) content = g_string_new("<h1>Lightsite form settings</h1>");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *account = NULL;
	gint64 id = 0, version = 0, connection_id = 0;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "attribution");
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "integrations");
	if (gate != NULL) return gate;
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database),
		principal, id, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required");
		return venture_web_error_response(error);
	}
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, id, &error);
	if (organization == NULL || venture_entity_is_deleted(organization))
	{
		if (error == NULL) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization is unavailable");
		return venture_web_error_response(error);
	}
	venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *version_text = htmx_request_get_form_value(request, "version");
		const gchar *connection_text = htmx_request_get_form_value(request, "connection_id");
		if (version_text == NULL || connection_text == NULL ||
			!g_ascii_string_to_signed(version_text, 10, 0, G_MAXINT64, &version, NULL) ||
			!g_ascii_string_to_signed(connection_text, 10, 0, G_MAXINT64, &connection_id, NULL))
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reload settings to select an exact account and version");
		else if (!g_strcmp0(operation, "configure"))
		{
			g_autoptr(JsonObject) values = venture_integration_web_values(request, schema, &error);
			if (values != NULL)
				{
					g_autoptr(JsonNode) settings = json_node_new(JSON_NODE_OBJECT);
					json_node_set_object(settings, values);
					connection = venture_attribution_settings_configure(database, id, settings, version, connection_id, &actor, &error);
				}
			if (connection != NULL) g_string_append(content, "<p>Signing settings saved. Verify the paired Lightsite forwarder with a signed form submission.</p>");
		}
		else if (!g_strcmp0(operation, "disconnect"))
		{
			connection = venture_integration_service_find(integrations, id, "lightsite_capture", &error);
			if (connection != NULL && venture_entity_get_id(VENTURE_ENTITY(connection)) != connection_id)
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Lightsite connection changed; reload settings");
			if (connection != NULL && error == NULL &&
				venture_integration_service_disable(integrations, id, connection_id, version, &actor, &error))
				g_string_append(content, "<p>Disconnected. New signed deliveries are refused; existing submission evidence remains.</p>");
		}
		else g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a Lightsite settings operation");
		if (error != NULL)
		{
			g_string_append(content, "<p role=\"alert\">"); venture_html_escape_append(content, error->message); g_string_append(content, "</p>");
		}
		g_clear_object(&connection);
	}
	connection = venture_integration_service_find(integrations, id, "lightsite_capture", NULL);
	version = connection != NULL ? venture_entity_get_version(VENTURE_ENTITY(connection)) : 0;
	connection_id = connection != NULL ? venture_entity_get_id(VENTURE_ENTITY(connection)) : 0;
	if (connection != NULL)
	{
		g_object_get(connection, "account-id", &account, NULL);
		g_string_append(content, "<p>Connected Lightsite tenant: <code>");
		venture_html_escape_append(content, account);
		g_string_append(content, "</code></p>");
	}
	else g_string_append(content, "<p>Lightsite forwarding is unconfigured. Browser capture remains independently consent-gated.</p>");
	g_string_append(content, "<p>Supply the complete settings when connecting or rotating. Credential inputs remain blank. Account replacement requires disconnect. Pair this secret with the trusted Lightsite event forwarder. These settings do not deploy or provision a site.</p><form method=\"post\">");
	g_string_append_printf(content, "<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"connection_id\" value=\"%" G_GINT64_FORMAT "\">", version, connection_id);
	venture_integration_web_fields(content, schema);
	g_string_append(content, "<button name=\"operation\" value=\"configure\">Connect or rotate</button>");
	if (connection != NULL) g_string_append(content, "<button name=\"operation\" value=\"disconnect\">Disconnect</button>");
	g_string_append(content, "</form>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Lightsite form settings", content->str), error != NULL ? 400 : 200);
}

static void venture_attribution_web_settings_link(VentureWebServer *self, GString *content,
	VentureEntity *record, const VentureAuthPrincipal *principal)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	gint64 id = venture_entity_get_id(record);
	if (!VENTURE_IS_ORGANIZATION(record) || !venture_context_module_enabled(self->context, "attribution") ||
		!venture_context_module_enabled(self->context, "integrations")) return;
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(
		venture_context_get_database(self->context)), principal, id, roles, G_N_ELEMENTS(roles))) return;
	g_string_append_printf(content, "<p><a class=\"btn\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/attribution\">Lightsite form settings</a></p>", id);
}
