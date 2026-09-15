/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_claims_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) claim = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *action = g_hash_table_lookup(params, "action");
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
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
	gate = api ? venture_web_require_module_api(self, "claims")
		: venture_web_require_module_ui(self, request, "claims");
	if (gate != NULL)
		return gate;
	claim = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_EXPENSE_CLAIM, id, &error);
	if (claim == NULL)
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	if (g_strcmp0(action, "submit") == 0)
		ok = venture_claims_service_submit(venture_claims_service_get(venture_context_get_database(self->context)), claim, &actor, &error);
	else if (g_strcmp0(action, "approve") == 0)
		ok = venture_claims_service_approve(venture_claims_service_get(venture_context_get_database(self->context)), claim, &actor, &error);
	else if (g_strcmp0(action, "pay") == 0)
		ok = venture_claims_service_pay(venture_claims_service_get(venture_context_get_database(self->context)), claim, &actor, &error);
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown claim action");
		return venture_web_error_response(error);
	}
	if (!ok)
		return venture_web_error_response(error);
	if (api)
	{
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(claim), FALSE);
		return venture_web_json_response(output, 200);
	}
	destination = g_strdup_printf("/e/expense_claim/%" G_GINT64_FORMAT, id);
	return venture_web_redirect_to(destination);
}

static void
venture_web_append_claims_actions(VentureWebServer *self, GString *content, VentureEntity *record)
{
	g_autofree gchar *state = NULL;
	const gchar *action = NULL;
	const gchar *label = NULL;
	if (!VENTURE_IS_EXPENSE_CLAIM(record) || !venture_web_module_enabled(self, "claims"))
		return;
	g_object_get(record, "status", &state, NULL);
	if (g_strcmp0(state, "draft") == 0)
	{
		action = "submit";
		label = "Submit";
	}
	else if (g_strcmp0(state, "submitted") == 0)
	{
		action = "approve";
		label = "Approve";
	}
	else if (g_strcmp0(state, "approved") == 0)
	{
		action = "pay";
		label = "Pay";
	}
	else
		return;
	g_string_append_printf(content, "<form method=\"post\" action=\"/claims/%" G_GINT64_FORMAT "/%s\">"
		"<button class=\"btn\" type=\"submit\">%s</button></form>",
		venture_entity_get_id(record), action, label);
}

static HtmxResponse *
venture_web_claims_workbench(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Claims</h1></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	HtmxResponse *gate;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "claims");
	if (gate != NULL)
		return gate;
	query = venture_query_new(VENTURE_TYPE_EXPENSE_CLAIM);
	venture_query_set_organization(query, venture_web_active_organization(self, request));
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Expense claims</h2></div><table><thead><tr><th>Number</th><th>Status</th><th></th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *status = NULL;
		g_autofree gchar *number = NULL;
		g_object_get(row, "status", &status, "number", &number, NULL);
		g_string_append(content, "<tr><td><a href=\"/e/expense_claim/");
		g_string_append_printf(content, "%" G_GINT64_FORMAT, venture_entity_get_id(row));
		g_string_append(content, "\">");
		venture_html_escape_append(content, number);
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, status);
		g_string_append(content, "</td><td>");
		venture_web_append_claims_actions(self, content, row);
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/claims", "Claims", content->str), 200);
}
