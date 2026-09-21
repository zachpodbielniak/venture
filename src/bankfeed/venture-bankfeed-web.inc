/* SPDX-License-Identifier: AGPL-3.0-or-later */
typedef struct { gboolean done; gint imported; GError *error; } BankfeedSyncWait;
static void
bankfeed_sync_finished(GObject *source, GAsyncResult *result, gpointer data)
{
	BankfeedSyncWait *wait = data;
	wait->imported = venture_bankfeed_service_sync_finish(VENTURE_BANKFEED_SERVICE(source), result, &wait->error);
	wait->done = TRUE;
}
static HtmxResponse *
bankfeed_sync(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) object = json_object_new();
	g_autoptr(GDateTime) from = NULL, to = NULL;
	VentureBankFeedService *service;
	VentureActor actor;
	gint imported;
	const gchar *id = g_hash_table_lookup(params, "id");
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_api(self, "bankfeed");
	if (gate != NULL) return gate;
	service = venture_context_get_bankfeed_service(self->context);
	if (service == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Bank feed module is disabled (bankfeed.enabled)");
		return venture_web_error_response(error);
	}
	{
		g_autoptr(VentureEntity) connection = venture_database_get(venture_context_get_database(self->context),
			VENTURE_TYPE_BANK_CONNECTION, id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0, &error);
		if (connection == NULL)
			return venture_web_error_response(error);
		if (venture_entity_get_organization_id(connection) != venture_web_active_organization(self, request))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Bank connection is not in the active organization");
			return venture_web_error_response(error);
		}
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	body = htmx_request_get_json(request, NULL);
	if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
	{
		const gchar *start = venture_json_object_get_string(json_node_get_object(body), "period_start", NULL);
		const gchar *end = venture_json_object_get_string(json_node_get_object(body), "period_end", NULL);
		if (start != NULL)
		{
			g_autofree gchar *iso = strlen(start) == 10 ? g_strconcat(start, "T00:00:00Z", NULL) : g_strdup(start);
			from = g_date_time_new_from_iso8601(iso, NULL);
		}
		if (end != NULL)
		{
			g_autofree gchar *iso = strlen(end) == 10 ? g_strconcat(end, "T00:00:00Z", NULL) : g_strdup(end);
			to = g_date_time_new_from_iso8601(iso, NULL);
		}
	}
	{
		BankfeedSyncWait wait = { FALSE, -1, NULL };
		g_autoptr(GCancellable) cancel = g_cancellable_new();
		venture_bankfeed_service_sync_async(service, id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0,
			from, to, &actor, cancel, bankfeed_sync_finished, &wait);
		while (!wait.done)
			g_main_context_iteration(NULL, TRUE);
		imported = wait.imported;
		if (wait.error != NULL)
			g_propagate_error(&error, wait.error);
	}
	if (imported < 0) return venture_web_error_response(error);
	if (htmx_request_get_form_value(request, "action") != NULL)
		return venture_web_redirect_to("/bankfeed");
	json_object_set_int_member(object, "imported", imported);
	json_node_take_object(node, g_steal_pointer(&object));
	return venture_web_json_response(node, 200);
}

static HtmxResponse *
bankfeed_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Bank feeds</h1><a class=\"btn\" href=\"/e/bank_connection/new\">Link account</a></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "bankfeed");
	if (gate != NULL) return gate;
	query = venture_query_new(VENTURE_TYPE_BANK_CONNECTION);
	venture_query_set_organization(query, venture_web_active_organization(self, request));
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL) return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Connections</h2></div><table><thead><tr><th>Name</th><th>Provider</th><th></th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL, *provider = NULL;
		g_object_get(row, "name", &name, "provider", &provider, NULL);
		g_string_append_printf(content, "<tr><td><a href=\"/e/bank_connection/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(row));
		venture_html_escape_append(content, name);
		g_string_append_printf(content, "</a> · <a href=\"/bankfeed/%" G_GINT64_FORMAT "/settings\">Settings</a></td><td>", venture_entity_get_id(row));
		venture_html_escape_append(content, provider);
		g_string_append_printf(content, "</td><td><form method=\"post\" action=\"/bankfeed/%" G_GINT64_FORMAT "/sync\">"
			"<input type=\"hidden\" name=\"action\" value=\"sync\"><button>Sync</button></form></td></tr>",
			venture_entity_get_id(row));
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/bankfeed", "Bank feeds", content->str), 200);
}

static HtmxResponse *
bankfeed_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureBankFeedService) service = NULL;
	g_autoptr(VentureEntity) connection = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) content = g_string_new("<h1>Bank feed settings</h1>");
	g_autofree gchar *provider = NULL, *key = NULL;
	gint64 id, org, binding_id = 0, version = 0;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "bankfeed");
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "integrations");
	if (gate != NULL) return gate;
	{
		GBytes *bytes = htmx_request_get_body_bytes(request);
		if (bytes != NULL && g_bytes_get_size(bytes) > 256 * 1024)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Bank feed settings request exceeds 256 KiB");
			return venture_web_error_response(error);
		}
	}
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, &error))
		return venture_web_error_response(error);
	connection = venture_database_get(database, VENTURE_TYPE_BANK_CONNECTION, id, &error);
	if (connection == NULL) return venture_web_error_response(error);
	if (venture_entity_is_deleted(connection))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Bank connection is unavailable");
		return venture_web_error_response(error);
	}
	org = venture_entity_get_organization_id(connection);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database), principal,
		org, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required");
		return venture_web_error_response(error);
	}
	venture_auth_to_actor(principal, &actor);
	/* Preserve explicitly registered provider factories on the live service. */
	if (venture_context_get_bankfeed_service(self->context) != NULL)
		service = g_object_ref(venture_context_get_bankfeed_service(self->context));
	else service = venture_bankfeed_service_new(database, org, NULL, &error);
	if (service == NULL) return venture_web_error_response(error);
	g_object_get(connection, "provider", &provider, NULL);
	schema = venture_bankfeed_service_settings_schema(service, provider, &error);
	if (schema == NULL) return venture_web_error_response(error);
	key = g_strconcat("bankfeed-", venture_entity_get_uuid(connection), NULL);
	binding = venture_integration_service_find(integrations, org, key, &error);
	if (error != NULL && !g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG)) return venture_web_error_response(error);
	g_clear_error(&error);
	binding_id = binding ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0;
	version = binding ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0;
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *binding_text = htmx_request_get_form_value(request, "binding_id");
		const gchar *version_text = htmx_request_get_form_value(request, "version");
		gint64 expected_binding, expected_version;
		if (binding_text == NULL || version_text == NULL ||
			!g_ascii_string_to_signed(binding_text, 10, 0, G_MAXINT64, &expected_binding, NULL) ||
			!g_ascii_string_to_signed(version_text, 10, 0, G_MAXINT64, &expected_version, NULL) ||
			expected_binding != binding_id || expected_version != version)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Bank feed binding changed; reload settings");
		else if (!g_strcmp0(operation, "configure"))
		{
			g_autoptr(JsonObject) values = venture_integration_web_values(request, schema, &error);
			g_autoptr(VentureIntegrationConnection) updated = NULL;
			if (values != NULL) updated = venture_bankfeed_service_configure(service, id, values, binding_id, version, &actor, &error);
			if (updated != NULL) g_string_append(content, "<p>Settings saved. Sync and test to verify access and import statement evidence.</p>");
		}
		else if (!g_strcmp0(operation, "disconnect") && binding != NULL)
		{
			if (venture_integration_service_disable(integrations, org, binding_id, version, &actor, &error))
				g_string_append(content, "<p>Disconnected. Previously imported statement evidence is retained.</p>");
		}
		else if (!g_strcmp0(operation, "test") && binding != NULL)
		{
			gint imported = venture_bankfeed_service_sync(service, id, NULL, NULL, &actor, &error);
			if (imported >= 0) g_string_append_printf(content, "<p>Provider access verified. Imported %d new statement rows; replayed rows were skipped.</p>", imported);
		}
		else g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a bank feed settings operation");
		if (error != NULL)
		{
			g_string_append(content, "<p role=\"alert\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</p>");
		}
		g_clear_object(&binding);
		binding = venture_integration_service_find(integrations, org, key, NULL);
		binding_id = binding ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0;
		version = binding ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0;
	}
	g_string_append(content, binding ? "<p>Organization-owned configuration. Saved credentials are write-only.</p>" :
		"<p>Unconfigured. Installation tokens are not used.</p>");
	g_string_append_printf(content, "<form method=\"post\"><input type=\"hidden\" name=\"binding_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">", binding_id, version);
	venture_integration_web_fields(content, schema);
	g_string_append(content, "<button name=\"operation\" value=\"configure\">Save settings</button></form>");
	if (binding != NULL)
		g_string_append_printf(content, "<form method=\"post\"><input type=\"hidden\" name=\"binding_id\" value=\"%" G_GINT64_FORMAT "\">"
			"<input type=\"hidden\" name=\"version\" value=\"%" G_GINT64_FORMAT "\">"
			"<button name=\"operation\" value=\"test\">Sync and test</button> "
			"<button name=\"operation\" value=\"disconnect\">Disconnect</button></form>", binding_id, version);
	g_string_append(content, "<p><a href=\"/bankfeed\">Back to bank feeds</a></p>");
	{
		HtmxResponse *reply = venture_web_html_response(venture_web_page(self, request, "/bankfeed", "Bank feed settings", content->str), error ? 400 : 200);
		htmx_response_add_header(reply, "Cache-Control", "no-store");
		return reply;
	}
}

static void
venture_bankfeed_web_register(HtmxRouter *router, VentureWebServer *self)
{
	(void)router;
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/bankfeed", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, bankfeed_ui, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/bankfeed/:id/settings", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, bankfeed_settings, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/bankfeed/:id/settings", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, bankfeed_settings, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/bankfeed/:id/sync", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, bankfeed_sync, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/bankfeed/:id/sync", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, bankfeed_sync, self);
}
