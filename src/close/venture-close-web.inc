/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint64
close_org(VentureWebServer *self, HtmxRequest *request)
{
	const gchar *value = htmx_request_get_query_param(request, "organization_id");
	gint64 organization = value ? g_ascii_strtoll(value, NULL, 10) : venture_web_active_organization(self, request);
	return organization > 0 ? organization : venture_context_get_default_organization_id(self->context);
}

static HtmxResponse *
close_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(VentureEntity) workspace = NULL;
	VentureCloseService *service;
	VentureActor actor;
	const gchar *action = g_hash_table_lookup(params, "action");
	if (action == NULL && g_str_has_suffix(htmx_request_get_path(request), "/open"))
		action = "open";
	if (action == NULL && g_str_has_suffix(htmx_request_get_path(request), "/pack"))
		action = "pack";
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "close");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	service = venture_close_service_get(venture_context_get_database(self->context));
	body = htmx_request_get_json(request, NULL);
	if (g_strcmp0(action, "open") == 0)
	{
		gint64 period_id = 0;
		const gchar *currency = "USD";
		if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
		{
			period_id = venture_json_object_get_int(json_node_get_object(body), "fiscal_period_id", 0);
			currency = venture_json_object_get_string(json_node_get_object(body), "currency", "USD");
		}
		workspace = venture_close_service_open(service, period_id, currency, &actor, &error);
		if (workspace == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(workspace), FALSE);
		return venture_web_json_response(output, 201);
	}
	workspace = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_CLOSE_WORKSPACE, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (workspace == NULL)
		return venture_web_error_response(error);
	if (g_strcmp0(action, "pack") == 0)
	{
		output = venture_close_service_pack(service, workspace, &error);
		if (output == NULL)
			return venture_web_error_response(error);
		return venture_web_json_response(output, 200);
	}
	if (g_strcmp0(action, "run") == 0)
	{
		if (!venture_close_service_run_checks(service, workspace, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "sign") == 0)
	{
		const gchar *role = "preparer";
		if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
			role = venture_json_object_get_string(json_node_get_object(body), "role", "preparer");
		if (!venture_close_service_sign(service, workspace, role, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "complete") == 0)
	{
		if (!venture_close_service_complete(service, workspace, &actor, &error))
			return venture_web_error_response(error);
	}
	else if (g_strcmp0(action, "reopen") == 0)
	{
		if (!venture_close_service_reopen(service, workspace, &actor, &error))
			return venture_web_error_response(error);
	}
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown close action");
		return venture_web_error_response(error);
	}
	output = venture_serializable_to_json(VENTURE_SERIALIZABLE(workspace), FALSE);
	return venture_web_json_response(output, 200);
}

static HtmxResponse *
close_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Period close</h1></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "close");
	if (gate != NULL)
		return gate;
	query = venture_query_new(VENTURE_TYPE_CLOSE_WORKSPACE);
	venture_query_set_organization(query, close_org(self, request));
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Workspaces</h2></div><table><thead><tr><th>Period</th><th>Status</th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "name", &name, "status", &status, NULL);
		g_string_append(content, "<tr><td><a href=\"/e/close_workspace/");
		g_string_append_printf(content, "%" G_GINT64_FORMAT, venture_entity_get_id(g_ptr_array_index(rows, i)));
		g_string_append(content, "\">");
		venture_html_escape_append(content, name);
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, status);
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/close", "Period close", content->str), 200);
}
