/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *venture_web_forge_settings(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) forge = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonNode) schema = venture_forge_settings_schema(), envelope = NULL, result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) content = g_string_new("<h1>Forge credentials</h1>");
	gint64 id, connection = 0, version = 0;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "forge");
	if (gate) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_OWNER, &error)) return venture_web_error_response(error);
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, &error)) return venture_web_error_response(error);
	forge = venture_database_get(database, VENTURE_TYPE_FORGE, id, &error);
	if (!forge) return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		g_autoptr(JsonObject) values = NULL;
		const gchar *raw_connection = htmx_request_get_form_value(request, "connection_id");
		const gchar *raw_version = htmx_request_get_form_value(request, "version");
		GBytes *body = htmx_request_get_body_bytes(request);
		if ((body && g_bytes_get_size(body) > 32768) || !raw_connection || !raw_version ||
			!g_ascii_string_to_signed(raw_connection, 10, 0, G_MAXINT64, &connection, NULL) ||
			!g_ascii_string_to_signed(raw_version, 10, 0, G_MAXINT64, &version, NULL))
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reload settings and supply a bounded request");
		else values = venture_integration_web_values(request, schema, &error);
		if (values)
		{
			envelope = json_node_new(JSON_NODE_OBJECT); json_node_take_object(envelope, json_object_new());
			json_object_set_object_member(json_node_get_object(envelope), "settings", g_steal_pointer(&values));
			json_object_set_string_member(json_node_get_object(envelope), "operation", htmx_request_get_form_value(request, "operation") ? htmx_request_get_form_value(request, "operation") : "");
			json_object_set_int_member(json_node_get_object(envelope), "connection_id", connection);
			json_object_set_int_member(json_node_get_object(envelope), "version", version);
			result = venture_forge_settings_apply(database, id, envelope, &actor, &error);
		}
		if (error) { g_string_append(content, "<p class=\"notice\">"); venture_html_escape_append(content, error->message); g_string_append(content, "</p>"); g_clear_error(&error); }
		else g_string_append(content, "<p>Operation completed. Credentials remain write-only.</p>");
	}
	binding = venture_forge_settings_find(database, id, NULL);
	if (binding) { connection = venture_entity_get_id(VENTURE_ENTITY(binding)); version = venture_entity_get_version(VENTURE_ENTITY(binding)); }
	else { connection = 0; version = 0; }
	g_string_append_printf(content, "<p>Organization %" G_GINT64_FORMAT ". %s</p><form method=\"post\"><input type=\"hidden\" name=\"connection_id\" value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">",
		venture_entity_get_organization_id(forge), binding ? "Connected" : "Disconnected", connection, version);
	venture_integration_web_fields(content, schema);
	g_string_append(content, "<button name=\"operation\" value=\"configure\">Connect or rotate</button><button name=\"operation\" value=\"test\">Test account</button><button name=\"operation\" value=\"disconnect\">Disconnect</button><button name=\"operation\" value=\"import\">Import legacy credentials</button></form><p>Supply both credentials when connecting or rotating. Import verifies and encrypts legacy values, then clears plaintext transactionally. Changing the account or origin requires disconnect first. Running operations are cancelled on credential changes; already transmitted external effects may need reconciliation.</p>");
	g_string_append(content, "<p>Webhook target: <code>");
	{
		g_autofree gchar *safe = venture_string_redact_uri(self->base_url ? self->base_url : "");
		venture_html_escape_append(content, safe);
	}
	g_string_append_printf(content, "/hooks/forge/%" G_GINT64_FORMAT "</code></p>", id);
	return venture_web_html_response(venture_web_page(self, request, "/e/forge", "Forge credentials", content->str), 200);
}
