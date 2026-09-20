/* SPDX-License-Identifier: AGPL-3.0-or-later */
static HtmxResponse *
capture_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) result = NULL;
	VentureCaptureService *service;
	VentureActor actor;
	const gchar *action = g_hash_table_lookup(params, "action");
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "capture");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	service = venture_capture_service_get(venture_context_get_database(self->context));
	body = htmx_request_get_json(request, NULL);
	if (g_strcmp0(action, "ingest") == 0 || action == NULL)
	{
		JsonObject *object = body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) when = NULL;
		const gchar *amount_text;
		if (object == NULL)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected a capture object");
			return venture_web_error_response(error);
		}
		amount_text = venture_json_object_get_string(object, "amount", NULL);
		if (amount_text != NULL)
			amount = venture_money_from_string(amount_text, NULL, NULL);
		if (venture_json_object_get_string(object, "occurred_at", NULL) != NULL)
			when = g_date_time_new_from_iso8601(venture_json_object_get_string(object, "occurred_at", NULL), NULL);
		item = venture_capture_service_ingest_for_organization(service, venture_web_active_organization(self, request),
			venture_json_object_get_string(object, "kind", "receipt"),
			venture_json_object_get_string(object, "title", NULL),
			venture_json_object_get_string(object, "source", "upload"),
			venture_json_object_get_int(object, "document_id", 0),
			venture_json_object_get_string(object, "vendor", NULL),
			amount, when, venture_json_object_get_string(object, "notes", NULL),
			&actor, &error);
		if (item == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(item), FALSE);
		return venture_web_json_response(output, 201);
	}
	item = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_CAPTURE_ITEM, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (item == NULL)
		return venture_web_error_response(error);
	if (g_strcmp0(action, "convert") == 0)
	{
		JsonObject *object = body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
		result = venture_capture_service_convert(service, item,
			object != NULL ? venture_json_object_get_string(object, "as", "expense") : "expense",
			object, &actor, &error);
		if (result == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE);
		return venture_web_json_response(output, 201);
	}
	if (g_strcmp0(action, "reject") == 0)
	{
		const gchar *reason = NULL;
		if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
			reason = venture_json_object_get_string(json_node_get_object(body), "reason", NULL);
		if (!venture_capture_service_reject(service, item, reason, &actor, &error))
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(item), FALSE);
		return venture_web_json_response(output, 200);
	}
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown capture action");
	return venture_web_error_response(error);
}

static HtmxResponse *
capture_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Capture inbox</h1><a class=\"btn\" href=\"/e/capture_item/new\">File a document</a></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "capture");
	if (gate != NULL)
		return gate;
	{
		g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
		g_autoptr(VentureEntity) subject = VENTURE_ENTITY(venture_capture_item_new());
		venture_entity_set_organization_id(subject, venture_web_active_organization(self, request));
		venture_web_append_record_actions(self, content, subject, principal);
	}
	query = venture_query_new(VENTURE_TYPE_CAPTURE_ITEM);
	venture_query_set_organization(query, venture_web_active_organization(self, request));
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "inbox", NULL);
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Waiting</h2></div><table><thead><tr><th>Title</th><th>Kind</th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *title = NULL;
		g_autofree gchar *kind = NULL;
		g_object_get(g_ptr_array_index(rows, i), "title", &title, "kind", &kind, NULL);
		g_string_append_printf(content, "<tr><td><a href=\"/e/capture_item/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(g_ptr_array_index(rows, i)));
		venture_html_escape_append(content, title);
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, kind);
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/capture", "Capture inbox", content->str), 200);
}
