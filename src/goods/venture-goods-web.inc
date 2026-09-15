/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean
refuse_web(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Unknown goods action");
	return FALSE;
}

static HtmxResponse *
venture_web_goods_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) input = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *destination = NULL;
	const gchar *path = htmx_request_get_path(request);
	const gchar *kind = strstr(path, "sales_order") != NULL ? "sales_order" : "purchase_order";
	const gchar *action = g_hash_table_lookup(params, "action");
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	gboolean api = g_str_has_prefix(path, "/api/");
	HtmxResponse *gate;
	VentureActor actor;
	gboolean ok = FALSE;
	gint64 line_id, qty, bill_id;
	gboolean exception = FALSE;

	if (!api)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
	}
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_api(self, "goods");
	if (gate != NULL)
		return gate;
	if (api)
	{
		input = htmx_request_get_json(request, &error);
		if (error != NULL)
			return venture_web_error_response(error);
	}
	venture_auth_to_actor(principal, &actor);
	if (input != NULL && JSON_NODE_HOLDS_OBJECT(input) &&
		venture_json_object_get_string(json_node_get_object(input), "date", NULL) != NULL)
		date = g_date_time_new_from_iso8601(venture_json_object_get_string(json_node_get_object(input), "date", NULL), NULL);
	if (date == NULL)
		date = venture_time_now();
	line_id = input != NULL && JSON_NODE_HOLDS_OBJECT(input) ?
		venture_json_object_get_int(json_node_get_object(input), "line_id", 0) : 0;
	qty = input != NULL && JSON_NODE_HOLDS_OBJECT(input) ?
		venture_json_object_get_int(json_node_get_object(input), "quantity", 0) : 0;
	bill_id = input != NULL && JSON_NODE_HOLDS_OBJECT(input) ?
		venture_json_object_get_int(json_node_get_object(input), "vendor_bill_id", 0) : 0;
	exception = input != NULL && JSON_NODE_HOLDS_OBJECT(input) &&
		venture_json_object_get_bool(json_node_get_object(input), "exception", FALSE);
	if (g_strcmp0(kind, "purchase_order") == 0)
	{
		VenturePurchasingService *svc = venture_purchasing_service_get(venture_context_get_database(self->context));
		if (g_strcmp0(action, "approve") == 0)
			ok = venture_purchasing_service_approve(svc, id, date, &actor, &error);
		else if (g_strcmp0(action, "send") == 0)
			ok = venture_purchasing_service_send(svc, id, date, &actor, &error);
		else if (g_strcmp0(action, "receive") == 0)
			ok = venture_purchasing_service_receive_line(svc, line_id != 0 ? line_id : id, qty, date, &actor, &error);
		else if (g_strcmp0(action, "match") == 0)
			ok = venture_purchasing_service_match(svc, id, bill_id, exception, &actor, &error);
		else if (g_strcmp0(action, "cancel") == 0)
			ok = venture_purchasing_service_cancel(svc, id, date, &actor, &error);
		else if (g_strcmp0(action, "return") == 0)
			ok = venture_purchasing_service_return_line(svc, line_id != 0 ? line_id : id, qty, date, &actor, &error);
		else
			ok = refuse_web(&error);
	}
	else if (g_strcmp0(kind, "sales_order") == 0)
	{
		VentureSalesOrderService *svc = venture_sales_order_service_get(venture_context_get_database(self->context));
		if (g_strcmp0(action, "allocate") == 0)
			ok = venture_sales_order_service_allocate(svc, id, date, &actor, &error);
		else if (g_strcmp0(action, "ship") == 0)
			ok = venture_sales_order_service_ship_line(svc, line_id != 0 ? line_id : id, qty, date, &actor, &error);
		else if (g_strcmp0(action, "invoice") == 0)
			ok = venture_sales_order_service_invoice_fulfilled(svc, id, date, &actor, &error);
		else if (g_strcmp0(action, "cancel") == 0)
			ok = venture_sales_order_service_cancel(svc, id, date, &actor, &error);
		else
			ok = refuse_web(&error);
	}
	if (!ok)
		return venture_web_error_response(error);
	if (api)
	{
		g_autoptr(JsonNode) output = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(output, json_object_new());
		json_object_set_boolean_member(json_node_get_object(output), "ok", TRUE);
		return venture_web_json_response(output, 200);
	}
	destination = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, kind, id);
	return venture_web_redirect_to(destination);
}

static HtmxResponse *
venture_web_purchasing_workbench(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Purchasing</h1></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "goods");
	if (gate != NULL)
		return gate;
	query = venture_query_new(VENTURE_TYPE_PURCHASE_ORDER);
	venture_query_set_organization(query, venture_web_active_organization(self, request));
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Purchase orders</h2></div><table><thead><tr><th>Number</th><th>Status</th><th></th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *status = NULL;
		g_autofree gchar *number = NULL;
		gint64 id = venture_entity_get_id(g_ptr_array_index(rows, i));
		g_object_get(g_ptr_array_index(rows, i), "status", &status, "number", &number, NULL);
		g_string_append(content, "<tr><td>");
		venture_html_escape_append(content, number);
		g_string_append(content, "</td><td>");
		venture_html_escape_append(content, status);
		g_string_append_printf(content, "</td><td><form method=\"post\" action=\"/purchase_order/%" G_GINT64_FORMAT "/approve\"><button class=\"btn\">Approve</button></form>"
			"<form method=\"post\" action=\"/purchase_order/%" G_GINT64_FORMAT "/send\"><button class=\"btn\">Send</button></form></td></tr>", id, id);
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/purchasing", "Purchasing", content->str), 200);
}
