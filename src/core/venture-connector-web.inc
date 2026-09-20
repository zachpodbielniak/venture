/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Protocol identity is metadata on the account. Both settings surfaces use
 * the same write-only schema and lifecycle service as non-web callers. */
static HtmxResponse *venture_web_connector_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	const gchar *name = g_hash_table_lookup(params, "type");
	gboolean calendar = !g_strcmp0(name, "calendar_account");
	GType type = calendar ? VENTURE_TYPE_CALENDAR_ACCOUNT : VENTURE_TYPE_MAIL_ACCOUNT;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) html = g_string_new(calendar ? "<h1>Calendar settings</h1>" : "<h1>Mailbox settings</h1>");
	g_autofree gchar *key = NULL, *label = NULL, *safe_label = NULL;
	gint64 id, binding_id = 0, version = 0;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate) return gate;
	if (!calendar && g_strcmp0(name, "mail_account")) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Connector account type is unavailable");
		return venture_web_error_response(error);
	}
	gate = venture_web_require_module_ui(self, request, calendar ? "calendar" : "mail_sync");
	if (gate) return gate;
	gate = venture_web_require_module_ui(self, request, "integrations");
	if (gate) return gate;
	if (htmx_request_get_body_bytes(request) && g_bytes_get_size(htmx_request_get_body_bytes(request)) > 32 * 1024) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Connector settings exceed 32 KiB");
		return venture_web_error_response(error);
	}
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, &error)) return venture_web_error_response(error);
	account = venture_database_get(database, type, id, &error);
	if (!account) return venture_web_error_response(error);
	if (venture_entity_is_deleted(account)) {
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Connector account is unavailable");
		return venture_web_error_response(error);
	}
	if (!venture_connector_can_manage(database, account, &error)) return venture_web_error_response(error);
	g_object_get(account, calendar ? "url" : "address", &label, NULL);
	safe_label = venture_string_redact_uri(label ? label : "");
	g_string_append(html, "<p>Account: "); venture_html_escape_append(html, safe_label); g_string_append(html, "</p>");
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	schema = venture_connector_settings_schema();
	key = venture_connector_binding_key(account);
	binding = venture_integration_service_find(venture_integration_service_get(database), venture_entity_get_organization_id(account), key, &error);
	if (error && !g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG)) return venture_web_error_response(error);
	g_clear_error(&error);
	binding_id = binding ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0;
	version = binding ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0;
	if (htmx_request_get_method(request) == HTMX_METHOD_POST) {
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *binding_text = htmx_request_get_form_value(request, "binding_id");
		const gchar *version_text = htmx_request_get_form_value(request, "version");
		gint64 expected_id, expected_version;
		if (!binding_text || !version_text || !g_ascii_string_to_signed(binding_text, 10, 0, G_MAXINT64, &expected_id, NULL) ||
			!g_ascii_string_to_signed(version_text, 10, 0, G_MAXINT64, &expected_version, NULL) || expected_id != binding_id || expected_version != version)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Connector binding changed; reload settings");
		else if (!g_strcmp0(operation, "configure")) {
			g_autoptr(JsonObject) values = venture_integration_web_values(request, schema, &error);
			g_autoptr(VentureIntegrationConnection) updated = NULL;
			if (values) updated = venture_connector_configure(database, venture_context_get_config(self->context), account,
				values, binding_id, version, &actor, &error);
			if (updated) g_string_append(html, "<p>Settings saved. Sync and test to verify access and import the selected content.</p>");
		} else if (!g_strcmp0(operation, "disconnect") && binding) {
			if (venture_connector_disconnect(database, account, binding_id, version, &actor, &error))
				g_string_append(html, "<p>Disconnected. Previously imported content keeps its ownership.</p>");
		} else if (!g_strcmp0(operation, "test") && binding) {
			gint imported;
			if (calendar) {
				g_autoptr(VentureCalendarSyncService) service = venture_web_calendar_sync_service(self);
				imported = venture_calendar_sync_service_sync(service, account, &actor, &error);
			} else {
				g_autoptr(VentureMailSyncService) service = venture_mail_sync_service_new_for_context(self->context);
				imported = venture_mail_sync_service_sync(service, account, &actor, &error);
			}
			if (imported >= 0) g_string_append_printf(html, "<p>Provider access verified. Applied %d changes from the selected account.</p>", imported);
		} else g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a connector settings operation");
		if (error) {
			g_autofree gchar *safe_error = venture_string_redact_uri(error->message);
			g_string_append(html, "<p role=\"alert\">");
			venture_html_escape_append(html, safe_error); g_string_append(html, "</p>");
		}
		g_clear_object(&binding);
		binding = venture_integration_service_find(venture_integration_service_get(database), venture_entity_get_organization_id(account), key, NULL);
		binding_id = binding ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0;
		version = binding ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0;
	}
	/* A sync saves a fresh row; reload its health instead of displaying the
	 * request's stale account snapshot after the operation completed. */
	{
		g_autoptr(VentureEntity) current = venture_database_get(database, type, id, NULL);
		g_autoptr(GDateTime) synced = NULL;
		g_autofree gchar *last_error = NULL, *safe_error = NULL, *stamp = NULL;
		if (current) {
			g_object_get(current, "last-synced-at", &synced, "last-error", &last_error, NULL);
			stamp = synced ? venture_time_to_string(synced) : g_strdup("Never");
			g_string_append(html, "<p>Last sync attempt: ");
			venture_html_escape_append(html, stamp); g_string_append(html, "</p>");
			if (!venture_string_is_empty(last_error)) {
				safe_error = venture_string_redact_uri(last_error);
				g_string_append(html, "<p role=\"alert\">Last sync error: ");
				venture_html_escape_append(html, safe_error); g_string_append(html, "</p>");
			}
		}
	}
	g_string_append(html, venture_access_policy_record_is_personal(venture_database_get_access_policy(database), account) ?
		"<p>Private account. Imported content belongs to its assigned member.</p>" : "<p>Shared business account. Imports participate in organization workflows.</p>");
	g_string_append(html, binding ? "<p>Configured. Saved credentials are write-only.</p>" : "<p>Unconfigured. No environment password is used.</p>");
	g_string_append_printf(html, "<form method=\"post\"><input type=\"hidden\" name=\"binding_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">", binding_id, version);
	venture_integration_web_fields(html, schema);
	g_string_append(html, "<button class=\"btn\" name=\"operation\" value=\"configure\">Save settings</button></form>");
	if (binding) g_string_append_printf(html, "<form method=\"post\"><input type=\"hidden\" name=\"binding_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\"><button class=\"btn\" name=\"operation\" value=\"test\">Sync and test</button> "
		"<button class=\"btn\" name=\"operation\" value=\"disconnect\">Disconnect</button></form>", binding_id, version);
	g_string_append_printf(html, "<p><a href=\"/e/%s/%" G_GINT64_FORMAT "\">Back to account</a></p>", calendar ? "calendar_account" : "mail_account", id);
	{
		HtmxResponse *response = venture_web_html_response(venture_web_page(self, request, "/connectors", "Connector settings", html->str), error ? 400 : 200);
		htmx_response_add_header(response, "Cache-Control", "no-store");
		return response;
	}
}

static void venture_web_connector_append_actions(VentureWebServer *self, GString *html, VentureEntity *record)
{
	const gchar *name;
	if (VENTURE_IS_MAIL_ACCOUNT(record)) name = "mail_account";
	else if (VENTURE_IS_CALENDAR_ACCOUNT(record)) name = "calendar_account";
	else return;
	if (venture_entity_is_deleted(record) || !venture_context_module_enabled(self->context, "integrations") ||
		!venture_connector_can_manage(venture_context_get_database(self->context), record, NULL)) return;
	g_string_append_printf(html, "<p><a href=\"/connectors/%s/%" G_GINT64_FORMAT "/settings\">Connector settings</a></p>",
		name, venture_entity_get_id(record));
}
