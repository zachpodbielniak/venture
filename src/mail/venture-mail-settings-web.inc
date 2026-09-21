/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Settings are schema-driven, but never filled from decrypted credentials. */
static JsonObject *
venture_integration_web_values(HtmxRequest *request, JsonNode *schema, GError **error)
{
	g_autoptr(JsonObject) values = json_object_new();
	JsonObject *properties = json_object_get_object_member(json_node_get_object(schema), "properties");
	JsonObjectIter iterator;
	const gchar *name;
	JsonNode *node;
	json_object_iter_init(&iterator, properties);
	while (json_object_iter_next(&iterator, &name, &node))
	{
		JsonObject *field = json_node_get_object(node);
		const gchar *value = htmx_request_get_form_value(request, name);
		if (value == NULL || *value == '\0') continue;
		if (!g_strcmp0(venture_json_object_get_string(field, "type", ""), "integer"))
		{
			gint64 number;
			if (!g_ascii_string_to_signed(value, 10, 0, G_MAXINT64, &number, NULL))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Settings require a non-negative whole number");
				return NULL;
			}
			json_object_set_int_member(values, name, number);
		}
		else json_object_set_string_member(values, name, value);
	}
	return g_steal_pointer(&values);
}
static void venture_integration_web_fields(GString *content, JsonNode *schema)
{
	JsonObject *properties = json_object_get_object_member(json_node_get_object(schema), "properties");
	JsonObjectIter iterator;
	const gchar *name;
	JsonNode *node;
	json_object_iter_init(&iterator, properties);
	while (json_object_iter_next(&iterator, &name, &node))
	{
		JsonObject *field = json_node_get_object(node);
		JsonNode *fallback = json_object_get_member(field, "default");
		gboolean secret = venture_json_object_get_bool(field, "x-sensitive", FALSE);
		gboolean integer = !g_strcmp0(venture_json_object_get_string(field, "type", ""), "integer");
		g_string_append(content, "<label>");
		venture_html_escape_append(content, venture_json_object_get_string(field, "title", name));
		if (venture_json_object_get_bool(field, "x-multiline", FALSE))
		{
			g_string_append(content, "<textarea autocomplete=\"off\" spellcheck=\"false\" name=\"");
			venture_html_escape_append(content, name);
			g_string_append(content, "\"></textarea></label>");
			continue;
		}
		g_string_append(content, "<input name=\"");
		venture_html_escape_append(content, name);
		g_string_append_printf(content, "\" type=\"%s\" autocomplete=\"%s\" value=\"",
			secret ? "password" : integer ? "number" : "text", secret ? "new-password" : "off");
		if (fallback != NULL && !secret)
		{
			if (integer) g_string_append_printf(content, "%" G_GINT64_FORMAT, json_node_get_int(fallback));
			else venture_html_escape_append(content, json_node_get_string(fallback));
		}
		g_string_append(content, "\"></label>");
	}
}
static HtmxResponse *venture_web_mail_settings(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureOrganizationMailer) selector = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(JsonNode) schema = venture_organization_mailer_dup_schema();
	g_autoptr(GString) content = g_string_new("<h1>Organization mail settings</h1>");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *account = NULL, *test_key = g_uuid_string_random();
	gint64 id = 0, version = 0, connection_id = 0;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
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
	selector = venture_organization_mailer_new(database, venture_context_get_config(self->context));
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
				connection = venture_organization_mailer_configure(selector, id, values, version, connection_id, &actor, &error);
			if (connection != NULL) g_string_append(content, "<p>SMTP settings saved. Send a test message to verify relay acceptance.</p>");
		}
		else if (!g_strcmp0(operation, "disconnect"))
		{
			if (venture_integration_service_disable(integrations, id, connection_id, version, &actor, &error))
				g_string_append(content, "<p>Disconnected. Previously attempted messages retain their original account.</p>");
		}
		else if (!g_strcmp0(operation, "test"))
		{
			const gchar *recipient = htmx_request_get_form_value(request, "test_recipient");
			const gchar *nonce = htmx_request_get_form_value(request, "test_key");
			g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
			g_autoptr(VentureEntity) outcome = NULL;
			g_autofree gchar *key = NULL, *state = NULL, *reason = NULL;
			VentureMailOutbox *outbox = venture_context_get_mail_outbox(self->context);
			connection = venture_integration_service_find(integrations, id, "smtp", &error);
			if (connection != NULL && (connection_id != venture_entity_get_id(VENTURE_ENTITY(connection)) ||
				version != venture_entity_get_version(VENTURE_ENTITY(connection))))
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "SMTP binding changed; reload settings");
			if (error == NULL && (recipient == NULL || *recipient == '\0' || nonce == NULL || !g_uuid_string_is_valid(nonce)))
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Supply a test recipient and reload the test form");
			if (error == NULL)
			{
				key = g_strdup_printf("smtp-test:%" G_GINT64_FORMAT ":%s", connection_id, nonce);
				g_object_set(message, "organization-id", id, "to", recipient, "subject", "Venture SMTP connection test",
					"text-body", "This test verifies relay acceptance using this organization's explicit SMTP settings.",
					"idempotency-key", key, "related-type", "integration_connection", "related-id", connection_id, NULL);
				queued = venture_mail_outbox_enqueue(outbox, message, &actor, &error);
			}
			if (queued != NULL && venture_mail_outbox_deliver_one(outbox, id, venture_entity_get_id(VENTURE_ENTITY(queued)),
				connection_id, version, NULL, &error) >= 0)
				outcome = venture_database_get(database, VENTURE_TYPE_MAIL_MESSAGE, venture_entity_get_id(VENTURE_ENTITY(queued)), &error);
			if (outcome != NULL)
			{
				g_object_get(outcome, "state", &state, "last-error", &reason, NULL);
				g_string_append(content, "<p>Test delivery: ");
				venture_html_escape_append(content, state);
				if (reason != NULL && *reason != '\0') { g_string_append(content, " — "); venture_html_escape_append(content, reason); }
				g_string_append_printf(content, " · <a href=\"/e/mail_message/%" G_GINT64_FORMAT "\">Delivery evidence</a></p>", venture_entity_get_id(outcome));
			}
		}
		else g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a mail settings operation");
		if (error != NULL)
		{
			g_string_append(content, "<p role=\"alert\">"); venture_html_escape_append(content, error->message); g_string_append(content, "</p>");
		}
		g_clear_object(&connection);
	}
	connection = venture_integration_service_find(integrations, id, "smtp", NULL);
	version = connection != NULL ? venture_entity_get_version(VENTURE_ENTITY(connection)) : 0;
	connection_id = connection != NULL ? venture_entity_get_id(VENTURE_ENTITY(connection)) : 0;
	if (connection != NULL)
	{
		g_object_get(connection, "account-id", &account, NULL);
		g_string_append(content, "<p>Connected SMTP account fingerprint: <code>");
		venture_html_escape_append(content, account);
		g_string_append(content, "</code></p>");
	}
	else g_string_append(content, "<p>Organization mail is unconfigured. Installation credentials are not used.</p>");
	if (connection != NULL)
	{
		g_autoptr(JsonNode) status = venture_organization_mailer_status(selector, id, NULL);
		if (status != NULL)
		{
			JsonObject *details = json_node_get_object(status);
			g_string_append(content, "<p>Current configuration: ");
			venture_html_escape_append(content, venture_json_object_get_string(details, "state", "unverified"));
			if (json_object_has_member(details, "error"))
			{
				g_string_append(content, " — ");
				venture_html_escape_append(content, venture_json_object_get_string(details, "error", ""));
			}
			g_string_append(content, ". Delivery status describes the latest attempt with this version; sent means relay acceptance.</p>");
		}
	}
	g_string_append(content, "<p>Supply the complete settings when connecting or rotating. Credential inputs remain blank. Account replacement requires disconnect. The operator must allow the relay endpoint.</p><form method=\"post\">");
	g_string_append_printf(content, "<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"connection_id\" value=\"%" G_GINT64_FORMAT "\">", version, connection_id);
	venture_integration_web_fields(content, schema);
	g_string_append(content, "<button name=\"operation\" value=\"configure\">Connect or rotate</button>");
	if (connection != NULL)
	{
		g_string_append_printf(content, "<input type=\"hidden\" name=\"test_key\" value=\"%s\">", test_key);
		g_string_append(content, "<label>Test recipient<input name=\"test_recipient\" type=\"email\"></label><button name=\"operation\" value=\"test\">Send test message</button><button name=\"operation\" value=\"disconnect\">Disconnect</button>");
	}
	g_string_append(content, "</form>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Organization mail settings", content->str), error != NULL ? 400 : 200);
}

static void venture_mail_web_settings_link(VentureWebServer *self, GString *content,
	VentureEntity *record, const VentureAuthPrincipal *principal)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	gint64 id = venture_entity_get_id(record);
	if (!VENTURE_IS_ORGANIZATION(record) || !venture_context_module_enabled(self->context, "mail") ||
		!venture_context_module_enabled(self->context, "integrations")) return;
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(
		venture_context_get_database(self->context)), principal, id, roles, G_N_ELEMENTS(roles))) return;
	g_string_append_printf(content, "<p><a class=\"btn\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/mail\">Mail settings</a></p>", id);
}
