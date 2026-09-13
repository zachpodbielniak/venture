/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the other web adapters; all business rules stay in the service. */
static gint64
activity_organization(VentureWebServer *self, HtmxRequest *request)
{
	const gchar *value = htmx_request_get_query_param(request, "organization_id");
	gint64 organization = value ? g_ascii_strtoll(value, NULL, 10) : venture_web_active_organization(self, request);
	return organization > 0 ? organization : venture_context_get_default_organization_id(self->context);
}

static HtmxResponse *
activity_api_action(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) changed = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureDatabase *db = venture_context_get_database(self->context);
	const gchar *action = g_hash_table_lookup(params, "action");
	const gchar *value = NULL;
	const gchar *id = g_hash_table_lookup(params, "id");
	VentureActor actor;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "activities");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	row = venture_database_get(db, VENTURE_TYPE_ACTIVITY, id ? g_ascii_strtoll(id, NULL, 10) : 0, &error);
	if (row == NULL)
		return venture_web_error_response(error);
	body = htmx_request_get_json(request, &error);
	if (body == NULL || !JSON_NODE_HOLDS_OBJECT(body))
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected an activity action object");
		return venture_web_error_response(error);
	}
	value = venture_json_object_get_string(json_node_get_object(body),
		g_strcmp0(action, "complete") == 0 ? "outcome" : g_strcmp0(action, "snooze") == 0 ? "until" : "owner", NULL);
	venture_auth_to_actor(principal, &actor);
	{
		gboolean stage;
		if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
			return venture_web_error_response(error);
		if (stage)
		{
			VentureConfirmation *confirmation;
			if (g_strcmp0(action, "complete") != 0)
			{
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"Only activity completion uses this staged action; stage other edits through generic CRUD");
				return venture_web_error_response(error);
			}
			confirmation = venture_confirmation_store_stage_activity_complete(venture_context_get_confirmations(self->context),
				row, value, &actor, "rest-api", &error);
			if (confirmation == NULL)
				return venture_web_error_response(error);
			result = venture_confirmation_to_json(confirmation);
			return venture_web_json_response(result, 202);
		}
	}
	changed = venture_activity_service_act(venture_database_get_activity_service(db), row, action, value, &actor, &error);
	if (changed == NULL)
		return venture_web_error_response(error);
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(changed), FALSE);
	return venture_web_json_response(result, 200);
}

static HtmxResponse *
activity_api_sweep(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	gint count;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	json_node_take_object(node, object);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "activities");
	if (gate != NULL)
		return gate;
	count = venture_activity_service_sweep(venture_database_get_activity_service(venture_context_get_database(self->context)),
		activity_organization(self, request), 200, &error);
	if (count < 0)
		return venture_web_error_response(error);
	json_object_set_int_member(object, "delivered", count);
	return venture_web_json_response(node, 200);
}

static HtmxResponse *
activity_api_calendar(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *calendar = NULL;
	g_autoptr(GBytes) bytes = NULL;
	HtmxResponse *response;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "activities");
	if (gate != NULL)
		return gate;
	calendar = venture_activity_service_calendar(venture_database_get_activity_service(venture_context_get_database(self->context)),
		activity_organization(self, request), htmx_request_get_query_param(request, "owner"), &error);
	if (calendar == NULL)
		return venture_web_error_response(error);
	bytes = g_bytes_new(calendar, strlen(calendar));
	response = htmx_response_new();
	htmx_response_set_status(response, 200);
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, "text/calendar; charset=utf-8");
	return response;
}

static HtmxResponse *
activity_api_list(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	JsonArray *array = json_array_new();
	const gchar *queue = htmx_request_get_query_param(request, "queue");
	guint i;
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);
	json_node_take_array(node, array);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "activities");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	rows = venture_activity_service_list(venture_database_get_activity_service(venture_context_get_database(self->context)),
		activity_organization(self, request), principal->name, queue ? queue : "mine", NULL, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	for (i = 0; i < rows->len; i++)
		json_array_add_element(array, venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(rows, i)), FALSE));
	return venture_web_json_response(node, 200);
}

static HtmxResponse *
activity_ui_done(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) changed = NULL;
	g_autoptr(GError) error = NULL;
	VentureDatabase *db = venture_context_get_database(self->context);
	const gchar *id = g_hash_table_lookup(params, "id");
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_require_for_type(self, principal, VENTURE_TYPE_ACTIVITY, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	gate = venture_web_require_module_ui(self, request, "activities");
	if (gate != NULL)
		return gate;
	row = venture_database_get(db, VENTURE_TYPE_ACTIVITY, id ? g_ascii_strtoll(id, NULL, 10) : 0, &error);
	if (row == NULL)
		return venture_web_error_response(error);
	venture_auth_to_actor(principal, &actor);
	changed = venture_activity_service_complete(venture_database_get_activity_service(db), row,
		htmx_request_get_form_value(request, "outcome"), &actor, &error);
	if (changed == NULL)
		return venture_web_error_response(error);
	return venture_web_redirect_to("/worklist");
}

static HtmxResponse *
activity_ui_worklist(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>My day</h1><a class=\"btn\" href=\"/e/activity/new\">New activity</a></div>");
	g_autoptr(GError) error = NULL;
	VentureActivityService *service = venture_database_get_activity_service(venture_context_get_database(self->context));
	const gchar *queues[] = { "overdue", "today", "upcoming" };
	const gchar *labels[] = { "Overdue", "Today", "Upcoming" };
	guint q;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "activities");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	if (venture_activity_service_sweep(service, activity_organization(self, request), 50, &error) < 0)
		return venture_web_error_response(error);
	for (q = 0; q < G_N_ELEMENTS(queues); q++)
	{
		g_autoptr(GPtrArray) rows = venture_activity_service_list(service, activity_organization(self, request), principal->name, queues[q], NULL, &error);
		guint i;
		if (rows == NULL)
			return venture_web_error_response(error);
		g_string_append_printf(content, "<div class=\"card\"><div class=\"card-head\"><h2>%s</h2></div><div class=\"card-body\"><ul>", labels[q]);
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *label = venture_entity_get_display_name(row);
			g_string_append_printf(content, "<li><a href=\"/e/activity/%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
			venture_html_escape_append(content, label);
			g_string_append_printf(content, "</a><form method=\"post\" action=\"/activities/%" G_GINT64_FORMAT "/complete\"><input name=\"outcome\" aria-label=\"Outcome\"><button class=\"btn btn-sm\">Done</button></form></li>", venture_entity_get_id(row));
		}
		g_string_append(content, "</ul></div></div>");
	}
	return venture_web_html_response(venture_web_page(self, request, "/worklist", "My day", content->str), 200);
}
