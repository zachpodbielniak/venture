/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the web adapters. AI on messages (docs/ai-messages.org):
 * the card on a conversation with drafts waiting for approval, summaries
 * and the log; approve, edit and discard; drafting a reply or a follow-up;
 * the settings page with the switch, business hours, auto-reply rules
 * and intent routes; and their API twins. Every route checks its own
 * session or role. */

static const gchar venture_web_ai_messages_style[] =
	"<style>.aim-list{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:.75rem}"
	".aim-item{border:1px solid var(--border);border-radius:.6rem;padding:.75rem;background:var(--surface);overflow-wrap:anywhere}"
	".aim-item.draft{border-color:var(--accent)}"
	".aim-head{display:flex;flex-wrap:wrap;gap:.4rem;align-items:center;font-size:.85em;color:var(--text-muted);margin-bottom:.4rem}"
	".aim-badge{display:inline-block;padding:.05rem .5rem;border-radius:999px;border:1px solid var(--border);font-size:.8em}"
	".aim-badge.wait{background:var(--accent-soft);border-color:var(--accent);color:inherit;font-weight:600}"
	".aim-why{margin:.3rem 0;font-size:.9em}.aim-meta{font-size:.8em;color:var(--text-muted);margin:.2rem 0}"
	".aim-body{white-space:pre-wrap;margin:.3rem 0}"
	".aim-item textarea{width:100%;box-sizing:border-box}"
	".aim-actions{display:flex;flex-wrap:wrap;gap:.5rem;align-items:center;margin-top:.5rem}.aim-actions form{margin:0}"
	".aim-details{display:flex;flex-wrap:wrap;gap:.3rem;margin:.3rem 0;padding:0;list-style:none}"
	".aim-details li{font-size:.8em;padding:.05rem .45rem;border-radius:.3rem;background:var(--surface-sunken)}"
	".aim-off{border-left:4px solid var(--border);padding:.5rem .75rem;background:var(--surface-sunken)}"
	".aim-rules{width:100%;border-collapse:collapse}.aim-rules th,.aim-rules td{text-align:left;padding:.4rem;border-bottom:1px solid var(--border);vertical-align:top}"
	".aim-form{display:grid;grid-template-columns:repeat(auto-fit,minmax(12rem,1fr));gap:.6rem;align-items:end}"
	".aim-form label{display:flex;flex-direction:column;font-size:.9em}.aim-form .wide{grid-column:1/-1}"
	".aim-form textarea{width:100%;box-sizing:border-box}"
	"@media (max-width:600px){.aim-rules thead{display:none}.aim-rules tr{display:block;border-bottom:1px solid var(--border);padding:.4rem 0}"
	".aim-rules td{display:block;border:0;padding:.15rem 0}}</style>";

static const gchar *
venture_web_ai_messages_kind_label(gint kind)
{
	switch (kind)
	{
	case VENTURE_AI_MESSAGE_KIND_AUTO_REPLY: return "Auto-reply";
	case VENTURE_AI_MESSAGE_KIND_SUMMARY: return "Call summary";
	case VENTURE_AI_MESSAGE_KIND_FOLLOW_UP: return "Follow-up";
	default: return "Drafted reply";
	}
}

static const gchar *
venture_web_ai_messages_state_label(gint state)
{
	switch (state)
	{
	case VENTURE_AI_MESSAGE_STATE_SENT: return "Sent";
	case VENTURE_AI_MESSAGE_STATE_HELD: return "Held for quiet hours";
	case VENTURE_AI_MESSAGE_STATE_REFUSED: return "Refused";
	case VENTURE_AI_MESSAGE_STATE_DISCARDED: return "Discarded";
	case VENTURE_AI_MESSAGE_STATE_NOTE: return "Summary";
	default: return "Waiting for your approval";
	}
}

static gchar *
venture_web_ai_messages_text(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

/* The details the message itself gave, as small labels. */
static void
venture_web_ai_messages_details(GString *html, const gchar *details)
{
	static const struct { const gchar *key; const gchar *label; } names[] = {
		{ "name", "Name" }, { "phone", "Phone" }, { "address", "Address" },
		{ "requested_date", "Asked for" }, { "product", "Product or service" }
	};
	g_autoptr(JsonNode) node = !venture_string_is_empty(details) ? json_from_string(details, NULL) : NULL;
	JsonObject *object;
	gsize i;
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return;
	object = json_node_get_object(node);
	g_string_append(html, "<ul class=\"aim-details\">");
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		if (!json_object_has_member(object, names[i].key))
			continue;
		g_string_append_printf(html, "<li>%s: ", names[i].label);
		venture_html_escape_append(html, json_object_get_string_member(object, names[i].key));
		g_string_append(html, "</li>");
	}
	g_string_append(html, "</ul>");
}

static void
venture_web_ai_messages_item(VentureDatabase *db, GString *html, VentureEntity *row, gboolean may_edit)
{
	gint kind = 0, state = 0;
	gint64 id = venture_entity_get_id(row), rule_id = 0;
	g_autofree gchar *body = NULL, *reason = NULL, *intent = NULL, *details = NULL, *channel = NULL, *provider = NULL;
	g_autofree gchar *model = NULL, *approved = NULL, *outcome = NULL, *next = NULL, *removed = NULL;
	g_object_get(row, "kind", &kind, "state", &state, "rule-id", &rule_id, "body", &body, "reason", &reason, "intent", &intent,
		"details", &details, "channel", &channel, "provider", &provider, "model", &model, "approved-by", &approved,
		"outcome", &outcome, "next-step", &next, "removed", &removed, NULL);
	g_string_append_printf(html, "<li class=\"aim-item%s\" id=\"ai-message-%" G_GINT64_FORMAT "\"><div class=\"aim-head\">"
		"<span class=\"aim-badge%s\">%s</span><strong>%s</strong>", state == VENTURE_AI_MESSAGE_STATE_DRAFT ? " draft" : "", id,
		state == VENTURE_AI_MESSAGE_STATE_DRAFT ? " wait" : "", venture_web_ai_messages_state_label(state),
		venture_web_ai_messages_kind_label(kind));
	if (!venture_string_is_empty(channel))
	{
		g_string_append(html, "<span>by ");
		venture_html_escape_append(html, channel);
		g_string_append(html, "</span>");
	}
	if (!venture_string_is_empty(intent))
	{
		g_string_append(html, "<span class=\"aim-badge\">");
		venture_html_escape_append(html, intent);
		g_string_append(html, "</span>");
	}
	g_string_append(html, "</div>");
	if (!venture_string_is_empty(reason))
	{
		g_string_append(html, "<p class=\"aim-why\"><strong>Why:</strong> ");
		venture_html_escape_append(html, reason);
		g_string_append(html, "</p>");
	}
	venture_web_ai_messages_details(html, details);
	if (state == VENTURE_AI_MESSAGE_STATE_DRAFT && may_edit && !venture_string_is_empty(body))
	{
		g_string_append_printf(html, "<form method=\"post\" action=\"/ai-messages/%" G_GINT64_FORMAT "/approve\">"
			"<label class=\"muted\" for=\"aim-body-%" G_GINT64_FORMAT "\">Edit before sending</label>"
			"<textarea id=\"aim-body-%" G_GINT64_FORMAT "\" name=\"body\" rows=\"3\" maxlength=\"8000\">", id, id, id);
		venture_html_escape_append(html, body);
		g_string_append(html, "</textarea><div class=\"aim-actions\"><button class=\"btn btn-primary\" type=\"submit\">Approve and send</button></form>");
		g_string_append_printf(html, "<form method=\"post\" action=\"/ai-messages/%" G_GINT64_FORMAT "/discard\">"
			"<button class=\"btn\" type=\"submit\">Discard</button></form></div>", id);
	}
	else if (!venture_string_is_empty(body))
	{
		g_string_append(html, "<div class=\"aim-body\">");
		venture_html_escape_append(html, body);
		g_string_append(html, "</div>");
	}
	if (!venture_string_is_empty(next))
	{
		g_string_append(html, "<p class=\"aim-why\"><strong>Suggested next step:</strong> ");
		venture_html_escape_append(html, next);
		g_string_append(html, "</p>");
	}
	if (!venture_string_is_empty(removed))
	{
		g_string_append(html, "<p class=\"aim-meta\">Removed by the grounding check: ");
		venture_html_escape_append(html, removed);
		g_string_append(html, "</p>");
	}
	g_string_append(html, "<p class=\"aim-meta\">");
	if (rule_id > 0)
	{
		g_autoptr(VentureEntity) rule = venture_database_get(db, VENTURE_TYPE_AI_MESSAGE_RULE, rule_id, NULL);
		g_autofree gchar *name = rule != NULL ? venture_web_ai_messages_text(rule, "name") : NULL;
		g_string_append(html, "Rule: ");
		venture_html_escape_append(html, name != NULL ? name : "(removed rule)");
		g_string_append(html, " · ");
	}
	g_string_append(html, "Written by ");
	if (!venture_string_is_empty(model))
	{
		venture_html_escape_append(html, provider != NULL ? provider : "");
		g_string_append(html, " / ");
		venture_html_escape_append(html, model);
	}
	else
		g_string_append(html, kind == VENTURE_AI_MESSAGE_KIND_SUMMARY ? "nobody (no AI was asked)" : "the rule's own words");
	if (!venture_string_is_empty(approved))
	{
		g_string_append(html, " · Approved by ");
		venture_html_escape_append(html, approved);
	}
	if (!venture_string_is_empty(outcome))
	{
		g_string_append(html, " · ");
		venture_html_escape_append(html, outcome);
	}
	g_string_append(html, "</p></li>");
}

/* The card on a conversation: what waits for approval first, then the
 * summaries and the log. Off, it says so and nothing else. */
static void
venture_web_ai_messages_panel(VentureContext *context, VentureEntity *conversation, VentureEntity *contact, gboolean may_edit, GString *html)
{
	VentureDatabase *db = venture_context_get_database(context);
	VentureAiMessages *ai = venture_ai_messages_get(db);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *why = NULL;
	gint64 org = venture_entity_get_organization_id(conversation), contact_id = venture_entity_get_id(contact);
	gboolean drafting = FALSE, on;
	guint i, waiting = 0;
	on = venture_ai_messages_status(ai, org, &drafting, &why);
	g_string_append(html, venture_web_ai_messages_style);
	g_string_append(html, "<section class=\"card\" id=\"ai-messages\"><div class=\"card-head\"><h2>AI on messages</h2>");
	g_string_append_printf(html, "<a class=\"inbox-open\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/ai-messages\">Rules and settings</a></div>"
		"<div class=\"card-body\">", org);
	if (!on)
	{
		g_string_append(html, "<p class=\"aim-off\"><strong>AI messages are off.</strong> ");
		venture_html_escape_append(html, why != NULL ? why : "");
		g_string_append(html, "</p></div></section>");
		return;
	}
	if (!drafting)
	{
		g_string_append(html, "<p class=\"aim-off\">");
		venture_html_escape_append(html, why != NULL ? why : "");
		g_string_append(html, "</p>");
	}
	rows = venture_ai_messages_for_contact(ai, contact, NULL);
	if (rows != NULL && rows->len > 0)
	{
		guint pass;
		g_string_append(html, "<ol class=\"aim-list\">");
		/* Drafts first, newest first; then everything else, newest first. */
		for (pass = 0; pass < 2; pass++)
			for (i = 0; i < rows->len && i < 30; i++)
			{
				VentureEntity *row = g_ptr_array_index(rows, i);
				gint state = 0;
				g_object_get(row, "state", &state, NULL);
				if ((state == VENTURE_AI_MESSAGE_STATE_DRAFT) != (pass == 0))
					continue;
				waiting += state == VENTURE_AI_MESSAGE_STATE_DRAFT;
				venture_web_ai_messages_item(db, html, row, may_edit);
			}
		g_string_append(html, "</ol>");
	}
	else
		g_string_append(html, "<p class=\"muted\">Nothing from the AI yet. When they write, a reply is drafted here for you to approve.</p>");
	if (drafting && may_edit)
	{
		g_string_append_printf(html, "<div class=\"aim-actions\"><form method=\"post\" action=\"/conversations/%" G_GINT64_FORMAT "/ai-draft\">"
			"<button class=\"btn\" type=\"submit\">Draft a reply</button></form>"
			"<form method=\"post\" action=\"/contacts/%" G_GINT64_FORMAT "/ai-follow-up\"><select name=\"channel\" aria-label=\"Follow up by\">"
			"<option value=\"text\">Text</option><option value=\"email\">Email</option></select> "
			"<button class=\"btn\" type=\"submit\">Draft a follow-up</button></form></div>",
			venture_entity_get_id(conversation), contact_id);
	}
	g_string_append_printf(html, "<p class=\"muted\">%s Nothing is sent until a person approves it, unless its rule sends on its own; "
		"a text still needs their agreement and waits out their quiet hours.</p></div></section>",
		waiting == 1 ? "One draft is waiting." : waiting > 1 ? "Drafts are waiting." : "");
}

static const VentureInboxPanel venture_web_ai_messages_inbox_panel = {
	"ai-messages", "ai_messages", venture_web_ai_messages_panel
};
VENTURE_INBOX_PANEL(venture_web_ai_messages_inbox_panel)

/* --- Approve, discard, draft ------------------------------------------------------------- */

static JsonObject *
venture_web_ai_messages_body(HtmxRequest *request, gboolean ui)
{
	g_autoptr(JsonNode) json = NULL;
	JsonObject *values = json_object_new();
	static const gchar *const fields[] = { "body", "channel", "instruction" };
	gsize i;
	if (ui)
	{
		for (i = 0; i < G_N_ELEMENTS(fields); i++)
		{
			const gchar *value = htmx_request_get_form_value(request, fields[i]);
			if (value != NULL)
				json_object_set_string_member(values, fields[i], value);
		}
		return values;
	}
	json = htmx_request_get_json(request, NULL);
	if (json != NULL && JSON_NODE_HOLDS_OBJECT(json))
	{
		json_object_unref(values);
		return json_object_ref(json_node_get_object(json));
	}
	return values;
}

static VentureEntity *
venture_web_ai_messages_lookup(VentureWebServer *self, GType type, GHashTable *params, const gchar *what, GError **error)
{
	gint64 id = 0;
	VentureEntity *row = NULL;
	if (g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, NULL))
		row = venture_database_get(venture_context_get_database(self->context), type, id, NULL);
	if (row == NULL || venture_entity_is_deleted(row))
	{
		g_clear_object(&row);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "%s not found", what);
	}
	return row;
}

/* Back to where the message is shown: its conversation, else its contact. */
static HtmxResponse *
venture_web_ai_messages_back(VentureEntity *row, gint64 contact_id, const gchar *notice)
{
	g_autofree gchar *escaped = notice != NULL ? g_uri_escape_string(notice, NULL, FALSE) : NULL;
	gint64 conversation = 0;
	g_autofree gchar *path = NULL;
	if (row != NULL)
		g_object_get(row, "conversation-id", &conversation, "contact-id", &contact_id, NULL);
	if (conversation > 0)
		path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT "%s%s#ai-messages", conversation,
			escaped != NULL ? "?notice=" : "", escaped != NULL ? escaped : "");
	else
		path = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT, contact_id);
	return venture_web_redirect_to(path);
}

static HtmxResponse *
venture_web_ai_messages_json(VentureEntity *row, guint status)
{
	g_autoptr(JsonNode) json = venture_serializable_to_json(VENTURE_SERIALIZABLE(row), FALSE);
	return venture_web_json_response(json, status);
}

/* POST /ai-messages/:id/{approve,discard} and their /api/v1 twins. */
static HtmxResponse *
venture_web_ai_messages_decide(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureAiMessages *ai = venture_ai_messages_get(venture_context_get_database(self->context));
	const gchar *path = htmx_request_get_path(request);
	gboolean ui = !g_str_has_prefix(path, "/api/");
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) row = NULL, done = NULL;
	g_autoptr(JsonObject) values = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	HtmxResponse *gate;
	VentureActor actor;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "ai_messages")) != NULL) return gate;
	row = venture_web_ai_messages_lookup(self, VENTURE_TYPE_AI_MESSAGE, params, "AI message", &error);
	if (row == NULL)
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	actor.name = actor.name != NULL ? actor.name : "Staff";
	values = venture_web_ai_messages_body(request, ui);
	if (g_str_has_suffix(path, "/discard"))
	{
		if (!venture_ai_messages_discard(ai, row, &actor, &error))
			return venture_web_error_response(error);
		done = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_AI_MESSAGE, venture_entity_get_id(row), NULL);
		return ui ? venture_web_ai_messages_back(row, 0, "Discarded; nothing was sent") : venture_web_ai_messages_json(done != NULL ? done : row, 200);
	}
	done = venture_ai_messages_approve(ai, row, venture_json_object_get_string(values, "body", NULL), now, &actor, &error);
	if (done == NULL)
	{
		if (ui && error != NULL && error->code != VENTURE_ERROR_NOT_FOUND)
			return venture_web_ai_messages_back(row, 0, error->message);
		return venture_web_error_response(error);
	}
	if (ui)
	{
		g_autofree gchar *outcome = venture_web_ai_messages_text(done, "outcome");
		return venture_web_ai_messages_back(done, 0, outcome);
	}
	return venture_web_ai_messages_json(done, 200);
}

/* POST /conversations/:id/ai-draft and its /api/v1 twin. */
static HtmxResponse *
venture_web_ai_messages_draft(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureAiMessages *ai = venture_ai_messages_get(venture_context_get_database(self->context));
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) conversation = NULL, draft = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	HtmxResponse *gate;
	VentureActor actor;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "ai_messages")) != NULL) return gate;
	conversation = venture_web_ai_messages_lookup(self, VENTURE_TYPE_INBOX_CONVERSATION, params, "Conversation", &error);
	if (conversation == NULL)
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	draft = venture_ai_messages_draft_reply(ai, conversation, now, &actor, &error);
	if (ui)
	{
		g_autofree gchar *path = NULL, *escaped = NULL;
		escaped = g_uri_escape_string(draft != NULL ? "A reply is drafted for you to approve" : error->message, NULL, FALSE);
		path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT "?notice=%s#ai-messages", venture_entity_get_id(conversation), escaped);
		return venture_web_redirect_to(path);
	}
	return draft != NULL ? venture_web_ai_messages_json(draft, 201) : venture_web_error_response(error);
}

/* POST /contacts/:id/ai-follow-up, /deals/:id/ai-follow-up and their twins. */
static HtmxResponse *
venture_web_ai_messages_follow_up(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureAiMessages *ai = venture_ai_messages_get(venture_context_get_database(self->context));
	const gchar *path = htmx_request_get_path(request);
	gboolean ui = !g_str_has_prefix(path, "/api/");
	gboolean deal = strstr(path, "/deals/") != NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) subject = NULL, draft = NULL;
	g_autoptr(JsonObject) values = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	HtmxResponse *gate;
	VentureActor actor;
	const gchar *channel;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "ai_messages")) != NULL) return gate;
	subject = venture_web_ai_messages_lookup(self, deal ? VENTURE_TYPE_DEAL : VENTURE_TYPE_CONTACT, params, deal ? "Deal" : "Contact", &error);
	if (subject == NULL)
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	values = venture_web_ai_messages_body(request, ui);
	channel = venture_json_object_get_string(values, "channel", NULL);
	draft = venture_ai_messages_follow_up(ai, subject, venture_string_is_empty(channel) ? "text" : channel,
		venture_json_object_get_string(values, "instruction", NULL), now, &actor, &error);
	if (ui)
	{
		gint64 contact_id = deal ? 0 : venture_entity_get_id(subject);
		if (deal)
			g_object_get(subject, "contact-id", &contact_id, NULL);
		if (draft == NULL)
		{
			g_autofree gchar *escaped = g_uri_escape_string(error->message, NULL, FALSE);
			g_autofree gchar *back = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT "?notice=%s", deal ? "deal" : "contact",
				venture_entity_get_id(subject), escaped);
			return venture_web_redirect_to(back);
		}
		return venture_web_ai_messages_back(draft, contact_id, "A follow-up is drafted for you to approve");
	}
	return draft != NULL ? venture_web_ai_messages_json(draft, 201) : venture_web_error_response(error);
}

/* GET /api/v1/conversations/:id/ai-messages: the contact's AI messages. */
static HtmxResponse *
venture_web_ai_messages_api_list(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureEntity) conversation = NULL, contact = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonArray) array = json_array_new();
	g_autoptr(JsonObject) object = json_object_new();
	g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *why = NULL;
	HtmxResponse *gate;
	gint64 contact_id = 0;
	gboolean drafting = FALSE, on;
	guint i;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "ai_messages")) != NULL) return gate;
	conversation = venture_web_ai_messages_lookup(self, VENTURE_TYPE_INBOX_CONVERSATION, params, "Conversation", &error);
	if (conversation == NULL)
		return venture_web_error_response(error);
	g_object_get(conversation, "contact-id", &contact_id, NULL);
	contact = venture_database_get(db, VENTURE_TYPE_CONTACT, contact_id, NULL);
	if (contact == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "This conversation's contact is gone");
		return venture_web_error_response(error);
	}
	rows = venture_ai_messages_for_contact(venture_ai_messages_get(db), contact, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	for (i = 0; i < rows->len; i++)
		json_array_add_element(array, venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(rows, i)), FALSE));
	on = venture_ai_messages_status(venture_ai_messages_get(db), venture_entity_get_organization_id(conversation), &drafting, &why);
	json_object_set_boolean_member(object, "on", on);
	json_object_set_boolean_member(object, "drafting", drafting);
	if (why != NULL)
		json_object_set_string_member(object, "why", why);
	json_object_set_array_member(object, "items", g_steal_pointer(&array));
	json_node_set_object(root, object);
	return venture_web_json_response(root, 200);
}

/* --- The settings page --------------------------------------------------------------------- */

static void
venture_web_ai_messages_option(GString *html, const gchar *value, const gchar *label, gboolean selected)
{
	g_string_append(html, "<option value=\"");
	venture_html_escape_append(html, value);
	g_string_append_printf(html, "\"%s>", selected ? " selected" : "");
	venture_html_escape_append(html, label);
	g_string_append(html, "</option>");
}

static gboolean
venture_web_ai_messages_settings_post(VentureWebServer *self, HtmxRequest *request, gint64 org, const VentureActor *actor, GError **error)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureAiMessages *ai = venture_ai_messages_get(db);
	const gchar *action = htmx_request_get_form_value(request, "action");
	const gchar *id_text = htmx_request_get_form_value(request, "id");
	gint64 id = id_text != NULL ? g_ascii_strtoll(id_text, NULL, 10) : 0;
	if (g_strcmp0(action, "off") == 0 || g_strcmp0(action, "on") == 0)
		return venture_ai_messages_set_off(ai, org, g_strcmp0(action, "off") == 0, actor, error);
	if (g_strcmp0(action, "hours") == 0)
		return venture_ai_messages_set_business_hours(ai, org, htmx_request_get_form_value(request, "hours"), actor, error);
	if (g_strcmp0(action, "add_rule") == 0)
	{
		g_autoptr(VentureEntity) rule = g_object_new(VENTURE_TYPE_AI_MESSAGE_RULE, "organization-id", org, "active", TRUE,
			"auto-send", g_strcmp0(htmx_request_get_form_value(request, "auto_send"), "1") == 0, NULL);
		const gchar *channel = htmx_request_get_form_value(request, "channel");
		g_object_set(rule, "name", htmx_request_get_form_value(request, "name"), "channel", channel != NULL ? channel : "",
			"template", htmx_request_get_form_value(request, "template"), NULL);
		if (!venture_entity_set_field_from_string(rule, "trigger", htmx_request_get_form_value(request, "trigger"), NULL) ||
			!venture_entity_set_field_from_string(rule, "wording", htmx_request_get_form_value(request, "wording"), NULL))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose when the rule answers and where its words come from");
			return FALSE;
		}
		return venture_ai_messages_save_rule(ai, rule, actor, error);
	}
	if (g_strcmp0(action, "add_route") == 0)
	{
		const gchar *who = htmx_request_get_form_value(request, "owner");
		g_autoptr(VentureEntity) route = g_object_new(VENTURE_TYPE_AI_MESSAGE_ROUTE, "organization-id", org, "active", TRUE,
			"intent", htmx_request_get_form_value(request, "intent"), NULL);
		if (who != NULL && g_str_has_prefix(who, "team:"))
			g_object_set(route, "team-id", g_ascii_strtoll(who + 5, NULL, 10), NULL);
		else if (who != NULL)
			g_object_set(route, "owner-id", g_ascii_strtoll(who, NULL, 10), NULL);
		return venture_ai_messages_save_route(ai, route, actor, error);
	}
	if (g_strcmp0(action, "auto_on") == 0 || g_strcmp0(action, "auto_off") == 0 || g_strcmp0(action, "remove_rule") == 0 ||
		g_strcmp0(action, "remove_route") == 0)
	{
		gboolean route = g_strcmp0(action, "remove_route") == 0;
		g_autoptr(VentureEntity) row = venture_database_get(db, route ? VENTURE_TYPE_AI_MESSAGE_ROUTE : VENTURE_TYPE_AI_MESSAGE_RULE, id, NULL);
		if (row == NULL || venture_entity_get_organization_id(row) != org)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, route ? "Route not found" : "Rule not found");
			return FALSE;
		}
		if (g_str_has_prefix(action, "remove"))
			return venture_ai_messages_remove(ai, row, actor, error);
		g_object_set(row, "auto-send", g_strcmp0(action, "auto_on") == 0, NULL);
		return venture_ai_messages_save_rule(ai, row, actor, error);
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose something to change");
	return FALSE;
}

static void
venture_web_ai_messages_people(VentureDatabase *db, gint64 org, GString *html)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(VentureQuery) teams = venture_query_new(VENTURE_TYPE_TEAM);
	g_autoptr(GPtrArray) rows = NULL, team_rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 200);
	rows = venture_database_find(db, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		gint64 user = 0;
		gboolean active = FALSE;
		g_autofree gchar *name = NULL, *value = NULL;
		g_object_get(g_ptr_array_index(rows, i), "user-id", &user, "active", &active, NULL);
		if (!active || (name = venture_web_inbox_username(db, user)) == NULL)
			continue;
		value = g_strdup_printf("%" G_GINT64_FORMAT, user);
		venture_web_ai_messages_option(html, value, name, FALSE);
	}
	venture_query_set_organization(teams, org);
	venture_query_set_limit(teams, 100);
	team_rows = venture_database_find(db, teams, NULL);
	for (i = 0; team_rows != NULL && i < team_rows->len; i++)
	{
		VentureEntity *team = g_ptr_array_index(team_rows, i);
		g_autofree gchar *name = venture_web_ai_messages_text(team, "name");
		g_autofree gchar *value = g_strdup_printf("team:%" G_GINT64_FORMAT, venture_entity_get_id(team));
		g_autofree gchar *label = g_strdup_printf("Team %s, in turn", name != NULL ? name : "");
		venture_web_ai_messages_option(html, value, label, FALSE);
	}
}

static void
venture_web_ai_messages_rules(VentureDatabase *db, gint64 org, GString *html)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_MESSAGE_RULE);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(db, query, NULL);
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Auto-reply rules</h2></div><div class=\"card-body\">");
	if (rows == NULL || rows->len == 0)
		g_string_append(html, "<p class=\"muted\">No rules: nothing answers on its own.</p>");
	else
	{
		g_string_append(html, "<table class=\"aim-rules\"><thead><tr><th>Rule</th><th>When</th><th>Answer by</th><th>Words</th>"
			"<th>Send</th><th></th></tr></thead><tbody>");
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *rule = g_ptr_array_index(rows, i);
			g_autofree gchar *name = NULL, *channel = NULL, *template = NULL;
			gint trigger = 0, wording = 0;
			gboolean auto_send = FALSE;
			gint64 id = venture_entity_get_id(rule);
			g_object_get(rule, "name", &name, "channel", &channel, "template", &template, "trigger", &trigger,
				"wording", &wording, "auto-send", &auto_send, NULL);
			g_string_append(html, "<tr><td><strong>");
			venture_html_escape_append(html, name);
			g_string_append_printf(html, "</strong></td><td>%s</td><td>", trigger == VENTURE_AI_MESSAGE_TRIGGER_MISSED_CALL ? "A missed call" : "Outside business hours");
			venture_html_escape_append(html, venture_string_is_empty(channel) ? "The channel they used" : channel);
			g_string_append(html, "</td><td>");
			if (wording == VENTURE_AI_MESSAGE_WORDING_AI)
				g_string_append(html, "An AI draft");
			else
			{
				g_string_append(html, "<span class=\"aim-body\">");
				venture_html_escape_append(html, template != NULL ? template : "");
				g_string_append(html, "</span>");
			}
			g_string_append_printf(html, "</td><td><span class=\"aim-badge%s\">%s</span></td><td><div class=\"aim-actions\">"
				"<form method=\"post\"><input type=\"hidden\" name=\"id\" value=\"%" G_GINT64_FORMAT "\">"
				"<button class=\"btn\" name=\"action\" value=\"%s\">%s</button></form>"
				"<form method=\"post\"><input type=\"hidden\" name=\"id\" value=\"%" G_GINT64_FORMAT "\">"
				"<button class=\"btn\" name=\"action\" value=\"remove_rule\">Remove</button></form></div></td></tr>",
				auto_send ? " wait" : "", auto_send ? "Sends on its own" : "Draft for approval", id,
				auto_send ? "auto_off" : "auto_on", auto_send ? "Ask me first" : "Send on its own", id);
		}
		g_string_append(html, "</tbody></table>");
	}
	g_string_append(html, "<h3>Add a rule</h3><form method=\"post\" class=\"aim-form\"><input type=\"hidden\" name=\"action\" value=\"add_rule\">"
		"<label>Name<input name=\"name\" maxlength=\"100\" required placeholder=\"After hours\"></label>"
		"<label>When<select name=\"trigger\"><option value=\"after_hours\">They write outside business hours</option>"
		"<option value=\"missed_call\">A call is missed</option></select></label>"
		"<label>Answer by<select name=\"channel\"><option value=\"\">The channel they used</option><option value=\"text\">Text</option>"
		"<option value=\"email\">Email</option><option value=\"chat\">Web chat</option></select></label>"
		"<label>Words<select name=\"wording\"><option value=\"template\">My words below</option><option value=\"ai\">An AI draft</option></select></label>"
		"<label class=\"wide\">Reply<textarea name=\"template\" rows=\"2\" maxlength=\"2000\" "
		"placeholder=\"Thanks for your message. We are closed now and will answer in the morning.\"></textarea></label>"
		"<label class=\"wide\"><span><input type=\"checkbox\" name=\"auto_send\" value=\"1\"> Send without asking me (off by default: the reply waits as a draft)</span></label>"
		"<div><button class=\"btn btn-primary\" type=\"submit\">Add rule</button></div></form>"
		"<p class=\"muted\">The first rule that applies answers, at most once per person in 12 hours. A text from a rule still needs "
		"their agreement to texts and waits out their quiet hours.</p></div></section>");
}

static void
venture_web_ai_messages_routes(VentureDatabase *db, gint64 org, GString *html)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AI_MESSAGE_ROUTE);
	g_autoptr(GPtrArray) rows = NULL;
	static const gchar *const intents[] = { "booking", "quote", "question", "complaint", "cancellation", "other" };
	gsize k;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(db, query, NULL);
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Who answers what</h2></div><div class=\"card-body\">"
		"<p>An unassigned conversation goes to the person or team for its intent; anything else goes through your "
		"<a href=\"/e/lead_routing_rule\">lead routing rules</a>, which may name <code>intent</code> and <code>channel</code>.</p>");
	if (rows != NULL && rows->len > 0)
	{
		g_string_append(html, "<table class=\"aim-rules\"><tbody>");
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *route = g_ptr_array_index(rows, i);
			g_autofree gchar *intent = venture_web_ai_messages_text(route, "intent");
			g_autofree gchar *who = NULL;
			gint64 owner = 0, team = 0;
			g_object_get(route, "owner-id", &owner, "team-id", &team, NULL);
			if (owner > 0)
				who = venture_web_inbox_username(db, owner);
			else
			{
				g_autoptr(VentureEntity) row = venture_database_get(db, VENTURE_TYPE_TEAM, team, NULL);
				g_autofree gchar *name = row != NULL ? venture_web_ai_messages_text(row, "name") : NULL;
				who = g_strdup_printf("team %s, in turn", name != NULL ? name : "(removed)");
			}
			g_string_append(html, "<tr><td>");
			venture_html_escape_append(html, intent);
			g_string_append(html, "</td><td>");
			venture_html_escape_append(html, who != NULL ? who : "(removed person)");
			g_string_append_printf(html, "</td><td><form method=\"post\"><input type=\"hidden\" name=\"id\" value=\"%" G_GINT64_FORMAT "\">"
				"<button class=\"btn\" name=\"action\" value=\"remove_route\">Remove</button></form></td></tr>", venture_entity_get_id(route));
		}
		g_string_append(html, "</tbody></table>");
	}
	g_string_append(html, "<form method=\"post\" class=\"aim-form\"><input type=\"hidden\" name=\"action\" value=\"add_route\">"
		"<label>Intent<select name=\"intent\">");
	for (k = 0; k < G_N_ELEMENTS(intents); k++)
		venture_web_ai_messages_option(html, intents[k], intents[k], FALSE);
	g_string_append(html, "</select></label><label>Goes to<select name=\"owner\">");
	venture_web_ai_messages_people(db, org, html);
	g_string_append(html, "</select></label><div><button class=\"btn btn-primary\" type=\"submit\">Add</button></div></form></div></section>");
}

/* GET and POST /organizations/:id/settings/ai-messages: an owner's or
 * administrator's. */
static HtmxResponse *
venture_web_ai_messages_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gint managers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureAiMessages *ai = venture_ai_messages_get(db);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) html = g_string_new(venture_web_inbox_style);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autofree gchar *why = NULL, *hours = NULL, *self_path = NULL;
	const gchar *notice = htmx_request_get_query_param(request, "notice");
	const gchar *problem = htmx_request_get_query_param(request, "problem");
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	gboolean drafting = FALSE, on;
	gint64 org = 0;
	if (gate != NULL) return gate;
	if ((gate = venture_web_require_module_ui(self, request, "ai_messages")) != NULL) return gate;
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &org, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(db), principal, org, managers, G_N_ELEMENTS(managers)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "AI message settings need an owner or administrator of the organization");
		return venture_web_error_response(error);
	}
	self_path = g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/ai-messages", org);
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		g_autofree gchar *escaped = NULL, *back = NULL;
		HtmxResponse *response;
		gboolean ok;
		venture_auth_to_actor(principal, &actor);
		ok = venture_web_ai_messages_settings_post(self, request, org, &actor, &error);
		escaped = g_uri_escape_string(ok ? "Saved." : (error != NULL ? error->message : "Not saved"), NULL, FALSE);
		back = g_strdup_printf("%s?%s=%s", self_path, ok ? "notice" : "problem", escaped);
		response = venture_web_redirect_to(back);
		htmx_response_set_status(response, 303);
		return response;
	}
	organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, NULL);
	if (organization == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization not found");
		return venture_web_error_response(error);
	}
	hours = venture_web_ai_messages_text(organization, "business-hours");
	on = venture_ai_messages_status(ai, org, &drafting, &why);
	g_string_append(html, venture_web_ai_messages_style);
	g_string_append(html, "<div class=\"inbox-page\"><p><a href=\"/conversations\">← Conversations</a></p><h1>AI on messages</h1>");
	if (!venture_string_is_empty(notice))
	{
		g_string_append(html, "<div class=\"notice positive\"><div>");
		venture_html_escape_append(html, notice);
		g_string_append(html, "</div></div>");
	}
	if (!venture_string_is_empty(problem))
	{
		g_string_append(html, "<div class=\"notice negative\" role=\"alert\"><div>");
		venture_html_escape_append(html, problem);
		g_string_append(html, "</div></div>");
	}
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>The switch</h2></div><div class=\"card-body\">");
	if (!on)
	{
		g_string_append(html, "<p class=\"aim-off\"><strong>AI messages are off.</strong> ");
		venture_html_escape_append(html, why != NULL ? why : "");
		g_string_append(html, "</p><form method=\"post\"><button class=\"btn btn-primary\" name=\"action\" value=\"on\">Switch AI messages on</button></form>");
	}
	else
	{
		g_string_append(html, "<p><strong>AI messages are on.</strong> ");
		if (drafting)
			g_string_append(html, "New messages are read, drafted and routed; every draft waits for a person unless its rule sends on its own.");
		else
			venture_html_escape_append(html, why != NULL ? why : "");
		g_string_append(html, "</p><form method=\"post\"><button class=\"btn\" name=\"action\" value=\"off\">Switch all AI messages off</button></form>");
	}
	g_string_append(html, "</div></section>");
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Business hours</h2></div><div class=\"card-body\">"
		"<form method=\"post\" class=\"aim-form\"><input type=\"hidden\" name=\"action\" value=\"hours\">"
		"<label class=\"wide\">Open<input name=\"hours\" maxlength=\"200\" placeholder=\"Mon-Fri 09:00-17:00; Sat 10:00-14:00\" value=\"");
	venture_html_escape_append(html, hours != NULL ? hours : "");
	g_string_append(html, "\"></label><div><button class=\"btn btn-primary\" type=\"submit\">Save</button></div></form>"
		"<p class=\"muted\">In the organization's own time zone. Empty means no after-hours replies.</p></div></section>");
	venture_web_ai_messages_rules(db, org, html);
	venture_web_ai_messages_routes(db, org, html);
	g_string_append(html, "<p class=\"muted\">How it works, its defaults and its limits: <a href=\"/docs/ai-messages.html\">AI on messages</a>.</p></div>");
	return venture_web_html_response(venture_web_page(self, request, "/conversations", "AI on messages", html->str), 200);
}

/* On a contact's or a deal's page: draft a follow-up. On an
 * organization's: the way to the settings. */
static void
venture_web_ai_messages_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	const gchar *kind = VENTURE_IS_CONTACT(record) ? "contacts" : VENTURE_IS_DEAL(record) ? "deals" : NULL;
	if (!venture_web_module_enabled(self, "ai_messages"))
		return;
	if (G_OBJECT_TYPE(record) == VENTURE_TYPE_ORGANIZATION)
	{
		g_string_append_printf(content, "<p><a class=\"btn\" href=\"/organizations/%" G_GINT64_FORMAT "/settings/ai-messages\">AI on messages</a> "
			"<span class=\"muted\">Auto-reply rules, business hours, who answers what, and the switch.</span></p>", venture_entity_get_id(record));
		return;
	}
	if (kind == NULL)
		return;
	g_string_append_printf(content, "<form method=\"post\" action=\"/%s/%" G_GINT64_FORMAT "/ai-follow-up\" class=\"aim-follow-up\">"
		"<button class=\"btn\" type=\"submit\">Draft a follow-up</button> by <select name=\"channel\" aria-label=\"Follow up by\">"
		"<option value=\"text\">text</option><option value=\"email\">email</option></select> "
		"<span class=\"muted\">From this record only; it waits in their conversation for you to approve.</span></form>",
		kind, venture_entity_get_id(record));
}

static void
venture_ai_messages_pages_register(VentureWebServer *self)
{
	static const struct {
		HtmxMethod method;
		const gchar *path;
		HtmxRouteCallback handler;
	} routes[] = {
		{ HTMX_METHOD_POST, "/ai-messages/:id/approve", venture_web_ai_messages_decide },
		{ HTMX_METHOD_POST, "/ai-messages/:id/discard", venture_web_ai_messages_decide },
		{ HTMX_METHOD_POST, "/api/v1/ai-messages/:id/approve", venture_web_ai_messages_decide },
		{ HTMX_METHOD_POST, "/api/v1/ai-messages/:id/discard", venture_web_ai_messages_decide },
		{ HTMX_METHOD_POST, "/conversations/:id/ai-draft", venture_web_ai_messages_draft },
		{ HTMX_METHOD_POST, "/api/v1/conversations/:id/ai-draft", venture_web_ai_messages_draft },
		{ HTMX_METHOD_GET, "/api/v1/conversations/:id/ai-messages", venture_web_ai_messages_api_list },
		{ HTMX_METHOD_POST, "/contacts/:id/ai-follow-up", venture_web_ai_messages_follow_up },
		{ HTMX_METHOD_POST, "/deals/:id/ai-follow-up", venture_web_ai_messages_follow_up },
		{ HTMX_METHOD_POST, "/api/v1/contacts/:id/ai-follow-up", venture_web_ai_messages_follow_up },
		{ HTMX_METHOD_POST, "/api/v1/deals/:id/ai-follow-up", venture_web_ai_messages_follow_up },
		{ HTMX_METHOD_GET, "/organizations/:id/settings/ai-messages", venture_web_ai_messages_settings },
		{ HTMX_METHOD_POST, "/organizations/:id/settings/ai-messages", venture_web_ai_messages_settings },
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
			VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, routes[i].handler, self);
}
