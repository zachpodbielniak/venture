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
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
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

static HtmxResponse *
venture_web_payables_workbench(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	(void)params;
	if (g_str_has_prefix(htmx_request_get_path(request), "/api/"))
	{
		g_autoptr(VentureAuthPrincipal) principal = NULL;
		g_autoptr(JsonNode) body = NULL;
		g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_autoptr(GDateTime) date = NULL;
		JsonObject *object;
		JsonArray *bills;
		VentureActor actor;
		guint i;
		gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
		if (gate != NULL)
			return gate;
		gate = venture_web_require_module_api(self, "payables");
		if (gate != NULL)
			return gate;
		principal = venture_auth_authenticate(self->auth, request);
		venture_auth_to_actor(principal, &actor);
		body = htmx_request_get_json(request, &error);
		if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
			return venture_web_error_response(error);
		object = json_node_get_object(body);
		bills = json_object_get_array_member(object, "bills");
		if (bills == NULL)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Select bills");
			return venture_web_error_response(error);
		}
		for (i = 0; i < json_array_get_length(bills); i++)
		{
			gint64 id = json_array_get_int_element(bills, i);
			g_array_append_val(ids, id);
		}
		if (venture_json_object_get_string(object, "date", NULL) != NULL)
			{
			date = venture_time_from_string(venture_json_object_get_string(object, "date", NULL), &error);
			if (date == NULL) return venture_web_error_response(error);
		}
		if (date == NULL)
			date = venture_time_from_string("today", NULL);
		if (!venture_payables_service_pay_bills(venture_payables_service_get(venture_context_get_database(self->context)),
			ids, date, venture_json_object_get_string(object, "method", NULL),
			venture_json_object_get_string(object, "adapter", "transfer"),
			venture_json_object_get_string(object, "reference", NULL), &actor, &error))
			return venture_web_error_response(error);
		{
			g_autoptr(JsonNode) ok = json_node_new(JSON_NODE_OBJECT);
			json_node_take_object(ok, json_object_new());
			json_object_set_boolean_member(json_node_get_object(ok), "ok", TRUE);
			return venture_web_json_response(ok, 200);
		}
	}
	{
		g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Pay bills</h1></div>");
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
		gate = venture_web_require_module_ui(self, request, "payables");
		if (gate != NULL)
			return gate;
		query = venture_query_new(VENTURE_TYPE_VENDOR_BILL);
		venture_query_set_organization(query, venture_web_active_organization(self, request));
		rows = venture_database_find(venture_context_get_database(self->context), query, &error);
		if (rows == NULL)
			return venture_web_error_response(error);
		g_string_append(content, "<form method=\"post\" action=\"/payables/pay\" class=\"card\"><div class=\"card-head\"><h2>Approved bills</h2></div><table><thead><tr><th></th><th>Number</th><th>Status</th></tr></thead><tbody>");
		for (i = 0; i < rows->len; i++)
		{
			g_autofree gchar *status = NULL;
			g_autofree gchar *number = NULL;
			g_object_get(g_ptr_array_index(rows, i), "status", &status, "number", &number, NULL);
			if (g_strcmp0(status, "approved") != 0 && g_strcmp0(status, "partially_paid") != 0)
				continue;
			g_string_append_printf(content, "<tr><td><input type=\"checkbox\" name=\"bill\" value=\"%" G_GINT64_FORMAT "\"></td><td>",
				venture_entity_get_id(g_ptr_array_index(rows, i)));
			venture_html_escape_append(content, number);
			g_string_append(content, "</td><td>");
			venture_html_escape_append(content, status);
			g_string_append(content, "</td></tr>");
		}
		g_string_append(content, "</tbody></table><label class=\"field\"><span class=\"field-label\">Adapter</span><select name=\"adapter\"><option>transfer</option><option>manual</option><option>card</option><option>check</option><option>ach</option></select></label><button class=\"btn\" type=\"submit\">Pay selected</button></form>");
		return venture_web_html_response(venture_web_page(self, request, "/payables", "Pay bills", content->str), 200);
	}
}

static HtmxResponse *
venture_web_payables_workbench_pay(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GDateTime) date = venture_time_from_string("today", NULL);
	VentureActor actor;
	HtmxResponse *gate;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_ui(self, request, "payables");
	if (gate != NULL)
		return gate;
	{
		const gchar *raw = htmx_request_get_form_value(request, "bill");
		if (raw != NULL)
		{
			gint64 id = g_ascii_strtoll(raw, NULL, 10);
			g_array_append_val(ids, id);
		}
	}
	venture_auth_to_actor(principal, &actor);
	if (!venture_payables_service_pay_bills(venture_payables_service_get(venture_context_get_database(self->context)),
		ids, date, NULL, htmx_request_get_form_value(request, "adapter"), NULL, &actor, &error))
		return venture_web_error_response(error);
	return venture_web_redirect_to("/payables");
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
