/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the web adapters. A lead's AI score proposal: asked for
 * through the lead action route (/leads/:id/ai_score), decided here, shown
 * on the lead's page beside the rule score. */

/* POST /api/v1/lead_score_proposals/:id/{accept,dismiss} and the page's
 * /lead_score_proposals/:id/{accept,dismiss}: both through the one
 * decision in venture-lead-ai-score.c. */
static HtmxResponse *
venture_web_lead_ai_score_decide(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) proposal = NULL;
	g_autoptr(VentureEntity) decided = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *action = g_hash_table_lookup(params, "action");
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gboolean accept;
	HtmxResponse *gate;
	VentureActor actor;
	gint64 lead_id = 0;
	if (ui)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
	}
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "leads");
	if (gate != NULL)
		return gate;
	if (g_strcmp0(action, "accept") != 0 && g_strcmp0(action, "dismiss") != 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such decision");
		return venture_web_error_response(error);
	}
	accept = g_strcmp0(action, "accept") == 0;
	proposal = venture_database_get(db, VENTURE_TYPE_LEAD_SCORE_PROPOSAL,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (proposal == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "AI score proposal not found");
		return venture_web_error_response(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	if (!venture_lead_ai_score_decide(self->context, proposal, accept, &actor, &error))
		return venture_web_error_response(error);
	g_object_get(proposal, "lead-id", &lead_id, NULL);
	if (ui)
	{
		g_autofree gchar *path = g_strdup_printf("/e/lead/%" G_GINT64_FORMAT, lead_id);
		return venture_web_redirect_to(path);
	}
	decided = venture_database_get(db, VENTURE_TYPE_LEAD_SCORE_PROPOSAL, venture_entity_get_id(proposal), &error);
	if (decided == NULL)
		return venture_web_error_response(error);
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(decided), FALSE);
	return venture_web_json_response(result, 200);
}

/* On a lead's page, with the organization's switch on: the rule score as
 * the lead's score, any pending proposal with its reasons and the two
 * decisions, and the button that asks for one. Nothing at all when off. */
static void
venture_web_lead_ai_score_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) pending = NULL;
	gint64 score = 0, id = venture_entity_get_id(record);
	VentureLeadStatus status;
	if (!VENTURE_IS_LEAD(record) || !venture_lead_ai_score_enabled(self->context, venture_entity_get_organization_id(record)))
		return;
	g_object_get(record, "score", &score, "status", &status, NULL);
	pending = venture_lead_ai_score_pending(db, record);
	g_string_append_printf(content, "<section class=\"card lead-ai-score\"><div class=\"card-head\"><h2>AI score proposal</h2></div>"
		"<div class=\"card-body\"><p>Rule score <strong>%" G_GINT64_FORMAT "</strong>. This stays the lead's score unless you accept a proposal.</p>",
		score);
	if (pending != NULL)
	{
		g_autofree gchar *reasons = NULL;
		g_auto(GStrv) lines = NULL;
		gint64 proposed = 0;
		guint i;
		g_object_get(pending, "score", &proposed, "reasons", &reasons, NULL);
		g_string_append_printf(content, "<p>The assistant proposes <strong class=\"proposed-score\">%" G_GINT64_FORMAT
			"</strong>, from this lead's own record and activities:</p><ul class=\"proposal-reasons\">", proposed);
		lines = g_strsplit(reasons != NULL ? reasons : "", "\n", -1);
		for (i = 0; lines[i] != NULL; i++)
		{
			if (*lines[i] == '\0')
				continue;
			g_string_append(content, "<li>");
			venture_html_escape_append(content, lines[i]);
			g_string_append(content, "</li>");
		}
		g_string_append_printf(content, "</ul><div class=\"actions\">"
			"<form method=\"post\" action=\"/lead_score_proposals/%" G_GINT64_FORMAT "/accept\"><button class=\"btn btn-primary\" type=\"submit\">Accept %" G_GINT64_FORMAT "</button></form>"
			"<form method=\"post\" action=\"/lead_score_proposals/%" G_GINT64_FORMAT "/dismiss\"><button class=\"btn\" type=\"submit\">Dismiss</button></form></div>",
			venture_entity_get_id(pending), proposed, venture_entity_get_id(pending));
	}
	if (status != VENTURE_LEAD_CONVERTED)
		g_string_append_printf(content, "<form method=\"post\" action=\"/leads/%" G_GINT64_FORMAT "/ai_score\">"
			"<button class=\"btn\" type=\"submit\">%s</button></form>", id, pending != NULL ? "Ask again" : "Ask the assistant for a score");
	g_string_append(content, "</div></section>");
}
