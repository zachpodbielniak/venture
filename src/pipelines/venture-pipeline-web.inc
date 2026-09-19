/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the web adapters to use their common authentication and rendering. */
static HtmxResponse *
venture_web_deal_move(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(VentureDeal) moved = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	gint64 stage_id;
	const gchar *note;
	gboolean staged = FALSE;
	gboolean form;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (NULL != gate)
		return gate;
	gate = venture_web_require_module_api(self, "pipelines");
	if (NULL != gate)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	deal = venture_database_get(db, VENTURE_TYPE_DEAL,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (NULL == deal)
		return venture_web_error_response(error);
	body = htmx_request_get_json(request, NULL);
	form = NULL == body;
	if (form)
	{
		const gchar *text = htmx_request_get_form_value(request, "stage_id");
		stage_id = NULL != text ? g_ascii_strtoll(text, NULL, 10) : 0;
		note = htmx_request_get_form_value(request, "note");
	}
	else if (JSON_NODE_HOLDS_OBJECT(body))
	{
		stage_id = venture_json_object_get_int(json_node_get_object(body), "stage_id", 0);
		note = venture_json_object_get_string(json_node_get_object(body), "note", NULL);
	}
	else
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected a stage_id and note object");
		return venture_web_error_response(error);
	}
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &staged, &error))
		return venture_web_error_response(error);
	if (staged)
	{
		VentureConfirmation *confirmation = venture_confirmation_store_stage_deal_move(
			venture_context_get_confirmations(self->context), VENTURE_DEAL(deal), stage_id, note, &actor, "REST", &error);
		if (NULL == confirmation)
			return venture_web_error_response(error);
		node = venture_confirmation_to_json(confirmation);
		return venture_web_json_response(node, 202);
	}
	moved = venture_deal_service_move_stage(venture_database_get_deal_service(db), VENTURE_DEAL(deal), stage_id, note, &actor, &error);
	if (NULL == moved)
		return venture_web_error_response(error);
	if (form)
		return venture_web_redirect_to("/deals");
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(moved), FALSE);
	return venture_web_json_response(node, 200);
}

static HtmxResponse *
venture_web_deals_board(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEAL);
	g_autoptr(VentureQuery) stage_query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(GPtrArray) deals = NULL;
	g_autoptr(GPtrArray) stages = NULL;
	g_autoptr(GString) html = g_string_new("<div class=\"page-head\"><h1>Deals</h1><a href=\"/e/deal/new\">New deal</a></div><div class=\"grid cols-3\">");
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	guint i;
	(void)params;
	gate = venture_web_ui_require_session(self, request);
	if (NULL != gate)
		return gate;
	gate = venture_web_require_module_ui(self, request, "pipelines");
	if (NULL != gate)
		return gate;
	venture_web_scope_to_active_organization(self, request, query);
	venture_web_scope_to_active_organization(self, request, stage_query);
	venture_query_add_order(stage_query, "position", VENTURE_SORT_ASCENDING, NULL);
	deals = venture_database_find(db, query, &error);
	stages = venture_database_find(db, stage_query, &error);
	if (NULL == deals || NULL == stages)
		return venture_web_error_response(error);
	for (i = 0; i < stages->len; i++)
	{
		VentureEntity *stage = g_ptr_array_index(stages, i);
		g_autofree gchar *name = NULL;
		gint64 stage_id = venture_entity_get_id(stage), pipeline_id;
		guint j;
		g_object_get(stage, "name", &name, "pipeline-id", &pipeline_id, NULL);
		g_string_append(html, "<section class=\"card\"><h2>");
		venture_html_escape_append(html, name);
		g_string_append(html, "</h2>");
		for (j = 0; j < deals->len; j++)
		{
			VentureEntity *deal = g_ptr_array_index(deals, j);
			g_autofree gchar *title = NULL;
			g_autofree gchar *next = NULL;
			gint64 current;
			guint k;
			g_object_get(deal, "stage-id", &current, "name", &title, "next-step", &next, NULL);
			if (current != stage_id)
				continue;
			g_string_append_printf(html, "<article class=\"card-body\"><a href=\"/e/deal/%" G_GINT64_FORMAT "\">", venture_entity_get_id(deal));
			venture_html_escape_append(html, title);
			g_string_append(html, "</a><p>");
			venture_html_escape_append(html, NULL != next ? next : "");
			g_string_append_printf(html, "</p><form method=\"post\" action=\"/deals/%" G_GINT64_FORMAT "/move\"><label>Stage<select name=\"stage_id\">", venture_entity_get_id(deal));
			for (k = 0; k < stages->len; k++)
			{
				VentureEntity *target = g_ptr_array_index(stages, k);
				g_autofree gchar *target_name = NULL;
				gint64 target_pipeline;
				g_object_get(target, "pipeline-id", &target_pipeline, "name", &target_name, NULL);
				if (target_pipeline != pipeline_id)
					continue;
				g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>", venture_entity_get_id(target), target == stage ? " selected" : "");
				venture_html_escape_append(html, target_name);
				g_string_append(html, "</option>");
			}
			g_string_append(html, "</select></label><label>Note<input name=\"note\"></label><button type=\"submit\">Move</button></form></article>");
		}
		g_string_append(html, "</section>");
	}
	g_string_append(html, "</div>");
	return venture_web_html_response(venture_web_page(self, request, "/deals", "Deals", html->str), 200);
}

static HtmxResponse *
venture_web_deal_move_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	HtmxResponse *gate = venture_web_ui_require_session(data, request);
	return NULL != gate ? gate : venture_web_deal_move(request, params, data);
}

/* POST /api/v1/deals/:id/quote and the deal page's form both reach the
 * deal service; the page form arrives without an Accept: JSON header. */
static HtmxResponse *
venture_web_deal_quote(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	VentureWebServer *self = user_data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) deal = NULL;
	g_autoptr(VentureQuote) quote = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	gboolean api = g_str_has_prefix(htmx_request_get_path(request), "/api/");

	if (!api)
	{
		gate = venture_web_ui_require_session(self, request);
		if (NULL != gate)
			return gate;
	}
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (NULL != gate)
		return gate;
	gate = venture_web_require_module_api(self, "pipelines");
	if (NULL != gate)
		return gate;
	gate = venture_web_require_module_api(self, "quotes");
	if (NULL != gate)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	deal = venture_database_get(db, VENTURE_TYPE_DEAL,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (NULL == deal)
	{
		if (NULL == error)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Deal not found");
		return venture_web_error_response(error);
	}
	quote = venture_deal_service_create_quote(venture_database_get_deal_service(db), VENTURE_DEAL(deal), &actor, &error);
	if (NULL == quote)
		return venture_web_error_response(error);
	if (!api)
	{
		g_autofree gchar *destination = g_strdup_printf("/e/quote/%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(quote)));
		return venture_web_redirect_to(destination);
	}
	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(quote), FALSE);
	return venture_web_json_response(node, 201);
}

/* The deal page offers the quote handoff only when both modules can act. */
static void
venture_web_deal_buttons(VentureWebServer *self, GString *html, VentureEntity *record)
{
	if (!VENTURE_IS_DEAL(record) || !venture_web_module_enabled(self, "pipelines") ||
		!venture_web_module_enabled(self, "quotes"))
		return;
	g_string_append_printf(html, "<form method=\"post\" action=\"/deals/%" G_GINT64_FORMAT "/quote\"><button>Create quote from lines</button></form>",
		venture_entity_get_id(record));
}
