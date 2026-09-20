/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_stripe_settings(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	static const gchar *const fields[] = { "secret_key", "publishable_key", "webhook_secret", "api_version", "success_url", "cancel_url", "environment" };
	static const gchar *const labels[] = { "Secret API key", "Publishable API key", "Webhook signing secret", "API version", "Success URL", "Cancel URL", "Environment" };
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GString) content = g_string_new("<h1>Stripe settings</h1>");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *account = NULL;
	g_autofree gchar *environment = NULL;
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	gint64 version = 0;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "stripe");
	if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database),
		principal, id, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required");
		return venture_web_error_response(error);
	}
	organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, id, &error);
	if (!organization) return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *version_text = htmx_request_get_form_value(request, "version");
		const gchar *connection_text = htmx_request_get_form_value(request, "connection_id");
		gint64 expected_connection = connection_text ? g_ascii_strtoll(connection_text, NULL, 10) : 0;
		version = version_text ? g_ascii_strtoll(version_text, NULL, 10) : 0;
		if (!g_strcmp0(operation, "configure"))
		{
			g_autoptr(JsonNode) settings = json_node_new(JSON_NODE_OBJECT);
			JsonObject *object = json_object_new();
			json_node_take_object(settings, object);
			for (i = 0; i < G_N_ELEMENTS(fields); i++)
			{
				const gchar *value = htmx_request_get_form_value(request, fields[i]);
				json_object_set_string_member(object, fields[i], value ? value : "");
			}
			connection = venture_stripe_settings_configure(database, id, settings, version, expected_connection, NULL, &actor, &error);
		}
		else if (!g_strcmp0(operation, "test"))
		{
			if (venture_stripe_settings_test(database, id, NULL, &error))
				g_string_append(content, "<p>Stripe verified the configured account.</p>");
		}
		else if (!g_strcmp0(operation, "disconnect"))
		{
			if (venture_integration_service_disable(integrations, id,
				expected_connection, version, &actor, &error))
				g_string_append(content, "<p>Disconnected. Historical payment callbacks retain their original account binding.</p>");
		}
		else
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a Stripe settings operation");
		if (error)
		{
			g_string_append(content, "<p role=\"alert\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</p>");
		}
		g_clear_object(&connection);
	}
	connection = venture_integration_service_find(integrations, id, "stripe", NULL);
	version = connection ? venture_entity_get_version(VENTURE_ENTITY(connection)) : 0;
	if (connection)
	{
		g_object_get(connection, "account-id", &account, "environment", &environment, NULL);
		g_string_append(content, "<p>Connected account: ");
		venture_html_escape_append(content, account);
		g_string_append(content, " · ");
		venture_html_escape_append(content, environment);
		g_string_append_printf(content, "</p><p>Webhook path: <code>/webhooks/stripe/%" G_GINT64_FORMAT "</code></p>", venture_entity_get_id(VENTURE_ENTITY(connection)));
	}
	else g_string_append(content, "<p>No Stripe account is connected to this organization.</p>");
	g_string_append(content, "<p>Credentials are write-only. Supply all values when connecting or rotating. To change account or environment, disconnect first.</p><form method=\"post\">");
	g_string_append_printf(content, "<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">", version);
	g_string_append_printf(content, "<input type=\"hidden\" name=\"connection_id\" value=\"%" G_GINT64_FORMAT "\">", connection ? venture_entity_get_id(VENTURE_ENTITY(connection)) : 0);
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_string_append_printf(content, "<label>%s<input name=\"%s\" type=\"%s\" autocomplete=\"off\"></label>", labels[i], fields[i], i < 3 ? "password" : "text");
	}
	g_string_append(content, "<p>Environment: test or live. Return URLs: HTTPS. API version: your pinned Stripe API version.</p><button name=\"operation\" value=\"configure\">Connect or rotate</button>");
	if (connection) g_string_append(content, "<button name=\"operation\" value=\"test\">Test connection</button><button name=\"operation\" value=\"disconnect\">Disconnect</button>");
	g_string_append(content, "</form>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Stripe settings", content->str), error ? 400 : 200);
}

static void
venture_stripe_web_settings_link(VentureWebServer *self, GString *content,
	VentureEntity *record, const VentureAuthPrincipal *principal)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	gint64 id = venture_entity_get_id(record);
	if (G_OBJECT_TYPE(record) != VENTURE_TYPE_ORGANIZATION ||
		!venture_context_module_enabled(self->context, "stripe")) return;
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(
		venture_context_get_database(self->context)), principal, id, roles, G_N_ELEMENTS(roles))) return;
	g_string_append_printf(content, "<p><a class=\"btn\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/stripe\">Stripe settings</a></p>", id);
}
