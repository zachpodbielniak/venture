/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the web adapters. Replying to a synced message from the
 * contact's page: the composer under each message, and the route that
 * queues the reply through venture_mail_reply_enqueue(). */

/* POST /interactions/:id/reply (the page's form, body=...) and
 * POST /api/v1/interactions/:id/reply ({"body": "..."}). */
static HtmxResponse *
venture_web_mail_reply(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) interaction = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	const gchar *body = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	gint64 contact = 0;
	if (ui)
	{
		gate = venture_web_ui_require_session(self, request);
		if (gate != NULL)
			return gate;
	}
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "mail");
	if (gate != NULL)
		return gate;
	interaction = venture_database_get(db, VENTURE_TYPE_INTERACTION,
		g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (interaction == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Message not found");
		return venture_web_error_response(error);
	}
	if (ui)
		body = htmx_request_get_form_value(request, "body");
	else
	{
		json = htmx_request_get_json(request, NULL);
		if (json != NULL && JSON_NODE_HOLDS_OBJECT(json))
			body = venture_json_object_get_string(json_node_get_object(json), "body", NULL);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	queued = venture_mail_reply_enqueue(self->context, interaction, body, &actor, &error);
	if (queued == NULL)
		return venture_web_error_response(error);
	if (ui)
	{
		g_autofree gchar *path = NULL;
		g_object_get(interaction, "contact-id", &contact, NULL);
		path = contact > 0 ? g_strdup_printf("/e/contact/%" G_GINT64_FORMAT "#mail", contact)
			: g_strdup_printf("/e/interaction/%" G_GINT64_FORMAT, venture_entity_get_id(interaction));
		return venture_web_redirect_to(path);
	}
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(queued), FALSE);
	return venture_web_json_response(result, 201);
}

/* A contact's mail, newest first, as a timeline: what came in and what
 * went out, and under a synced message a composer whose reply threads to
 * it. The newest answerable message has its composer open. */
static void
venture_web_mail_reply_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean opened = FALSE;
	guint i;
	if (!VENTURE_IS_CONTACT(record) || !venture_web_module_enabled(self, "mail") || !venture_web_module_enabled(self, "mail_sync"))
		return;
	query = venture_query_new(VENTURE_TYPE_INTERACTION);
	venture_query_set_organization(query, venture_entity_get_organization_id(record));
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), NULL);
	venture_query_add_filter_int(query, "kind", VENTURE_FILTER_OP_EQ, VENTURE_INTERACTION_KIND_EMAIL, NULL);
	venture_query_add_order(query, "occurred-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 20);
	rows = venture_database_find(db, query, NULL);
	if (rows == NULL || rows->len == 0)
		return;
	g_string_append(content, "<section class=\"card mail-thread\" id=\"mail\"><div class=\"card-head\"><h2>Mail</h2></div>"
		"<div class=\"card-body\"><ol class=\"timeline\">");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) source = venture_mail_reply_source(db, row);
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *subject = NULL, *body = NULL, *day = NULL, *from = NULL;
		gboolean outbound = FALSE;
		g_object_get(row, "subject", &subject, "body", &body, "outbound", &outbound, "occurred-at", &when, NULL);
		if (when != NULL)
		{
			g_autoptr(GDateTime) local = g_date_time_to_timezone(when, venture_context_get_timezone(self->context));
			day = g_date_time_format(local, "%Y-%m-%d %H:%M");
		}
		g_string_append_printf(content, "<li class=\"timeline-item timeline-%s\"><span class=\"timeline-dot\"></span>"
			"<div class=\"timeline-body\"><div class=\"timeline-head\"><span class=\"timeline-actor\">%s</span>",
			outbound ? "outbound" : "inbound", outbound ? "Sent" : "Received");
		if (day != NULL)
		{
			g_string_append(content, " <time class=\"timeline-when\">");
			venture_html_escape_append(content, day);
			g_string_append(content, "</time>");
		}
		g_string_append(content, "</div><div class=\"timeline-text\"><strong>");
		venture_html_escape_append(content, subject != NULL ? subject : "(no subject)");
		g_string_append(content, "</strong>");
		if (!venture_string_is_empty(body))
		{
			g_autofree gchar *excerpt = g_utf8_substring(body, 0, MIN(g_utf8_strlen(body, -1), 600));
			g_string_append(content, "<p style=\"white-space:pre-wrap\">");
			venture_html_escape_append(content, excerpt);
			g_string_append(content, "</p>");
		}
		g_string_append(content, "</div>");
		if (source != NULL)
		{
			g_autofree gchar *reply_subject = NULL;
			g_object_get(source, "from-address", &from, NULL);
			reply_subject = subject != NULL && g_ascii_strncasecmp(subject, "Re:", 3) == 0 ? g_strdup(subject)
				: g_strdup_printf("Re: %s", subject != NULL ? subject : "");
			g_string_append_printf(content, "<details class=\"reply-composer\"%s><summary>Reply</summary>"
				"<form method=\"post\" action=\"/interactions/%" G_GINT64_FORMAT "/reply\"><p class=\"muted\">To ",
				opened ? "" : " open", venture_entity_get_id(row));
			venture_html_escape_append(content, from != NULL ? from : "");
			g_string_append(content, " &middot; Subject: ");
			venture_html_escape_append(content, reply_subject);
			g_string_append(content, "</p><label>Your reply<textarea name=\"body\" rows=\"5\" required></textarea></label>"
				"<p class=\"muted\">Goes out through the outbox and threads under this message in the customer's mail.</p>"
				"<button class=\"btn btn-primary\" type=\"submit\">Send reply</button></form></details>");
			opened = TRUE;
		}
		g_string_append(content, "</div></li>");
	}
	g_string_append(content, "</ol></div></section>");
}
