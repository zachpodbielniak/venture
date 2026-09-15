/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_payroll_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *action = g_hash_table_lookup(params, "action");
	gint64 id;
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	HtmxResponse *gate;
	VentureActor actor;
	gboolean ok;
	if (!api)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
	}
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = api ? venture_web_require_module_api(self, "payroll")
		: venture_web_require_module_ui(self, request, "payroll");
	if (gate != NULL)
		return gate;
	venture_auth_to_actor(principal, &actor);
	if (g_strcmp0(action, "import") == 0 ||
		g_str_has_suffix(htmx_request_get_path(request), "/payroll/import"))
	{
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
			return venture_web_error_response(error);
		run = venture_payroll_service_import_json(venture_payroll_service_get(venture_context_get_database(self->context)),
			venture_web_active_organization(self, request), json_node_get_object(body), &actor, &error);
		if (run == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(run), FALSE);
		return venture_web_json_response(output, 201);
	}
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	run = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_PAYROLL_RUN, id, &error);
	if (run == NULL)
		return venture_web_error_response(error);
	body = htmx_request_get_json(request, NULL);
	if (g_strcmp0(action, "disburse") == 0)
		ok = venture_payroll_service_disburse(venture_payroll_service_get(venture_context_get_database(self->context)),
			run, body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? venture_json_object_get_string(json_node_get_object(body), "kind", "all") : "all",
			&actor, &error);
	else if (g_strcmp0(action, "reverse") == 0)
		ok = venture_payroll_service_reverse(venture_payroll_service_get(venture_context_get_database(self->context)),
			run, &actor, &error);
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown payroll action");
		return venture_web_error_response(error);
	}
	if (!ok)
		return venture_web_error_response(error);
	if (api)
	{
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(run), FALSE);
		return venture_web_json_response(output, 200);
	}
	return venture_web_redirect_to("/payroll");
}

static HtmxResponse *
venture_web_payroll_workbench(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Payroll</h1></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	HtmxResponse *gate;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "payroll");
	if (gate != NULL)
		return gate;
	query = venture_query_new(VENTURE_TYPE_PAYROLL_RUN);
	venture_query_set_organization(query, venture_web_active_organization(self, request));
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Pay runs</h2></div><table><thead><tr><th>Key</th><th>Status</th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *key = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(row, "run-key", &key, "status", &status, NULL);
		g_string_append_printf(content, "<tr><td><a href=\"/e/payroll_run/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(row));
		venture_html_escape_append(content, key);
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, status);
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/payroll", "Payroll", content->str), 200);
}
