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
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, provider);
		g_string_append_printf(content, "</td><td><form method=\"post\" action=\"/bankfeed/%" G_GINT64_FORMAT "/sync\">"
			"<input type=\"hidden\" name=\"action\" value=\"sync\"><button>Sync</button></form></td></tr>",
			venture_entity_get_id(row));
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/bankfeed", "Bank feeds", content->str), 200);
}

static void
venture_bankfeed_web_register(HtmxRouter *router, VentureWebServer *self)
{
	htmx_router_get(router, "/bankfeed", bankfeed_ui, self);
	htmx_router_post(router, "/bankfeed/:id/sync", bankfeed_sync, self);
	htmx_router_post(router, "/api/v1/bankfeed/:id/sync", bankfeed_sync, self);
}
