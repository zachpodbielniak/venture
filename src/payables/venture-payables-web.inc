/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
venture_web_payables_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) command = NULL;
	g_autoptr(JsonNode) input = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *action = g_hash_table_lookup(params, "action");
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean stage = FALSE;
	HtmxResponse *gate;
	VentureActor actor;

	if (!api)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
	}
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_api(self, "payables");
	if (gate != NULL)
		return gate;
	if (api)
	{
		input = htmx_request_get_json(request, &error);
		if (input == NULL || !venture_web_stage_requested(request, &stage, &error))
			return venture_web_error_response(error);
	}
	command = venture_payables_service_prepare_action(
		venture_payables_service_get(venture_context_get_database(self->context)), id, action, input, &error);
	if (command == NULL)
		return venture_web_error_response(error);
	if (stage)
		return venture_web_api_stage(self, command, NULL, principal, VENTURE_AUDIT_ACTION_CREATE);
	venture_auth_to_actor(principal, &actor);
	if (!venture_database_save(venture_context_get_database(self->context), command, &actor, &error))
		return venture_web_error_response(error);
	if (api)
	{
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(command), FALSE);
		return venture_web_json_response(output, 201);
	}
	destination = g_strdup_printf("/e/vendor_bill/%" G_GINT64_FORMAT, id);
	return venture_web_redirect_to(destination);
}

static void
venture_web_append_payables_actions(VentureWebServer *self, GString *content, VentureEntity *record)
{
	g_autofree gchar *state = NULL;
	const gchar *action;
	const gchar *label;
	if (!VENTURE_IS_VENDOR_BILL(record) || !venture_web_module_enabled(self, "payables"))
		return;
	g_object_get(record, "status", &state, NULL);
	if (g_strcmp0(state, "draft") == 0)
	{
		action = "approve";
		label = "Approve";
	}
	else if (g_strcmp0(state, "approved") == 0 || g_strcmp0(state, "partially_paid") == 0)
	{
		action = "pay";
		label = "Pay";
	}
	else
		return;
	g_string_append_printf(content, "<form method=\"post\" action=\"/bills/%" G_GINT64_FORMAT "/%s\">"
		"<button class=\"btn\" type=\"submit\">%s</button></form>", venture_entity_get_id(record), action, label);
}
