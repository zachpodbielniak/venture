/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the web adapters. One inbox (docs/inbox.org): the
 * conversation list and page, their API twins, the web chat settings, and
 * the public chat -- the intake form, the visitor's own link and its poll.
 * Every staff route checks its own session or role. The public chat has
 * no session by design: it never reads a cookie, sets none, and answers
 * only for what its token names. */

static const gchar venture_web_inbox_style[] =
	"<style>.inbox-tabs{display:flex;flex-wrap:wrap;gap:.5rem;margin:0 0 .75rem}"
	".inbox-filter{display:flex;flex-wrap:wrap;gap:.5rem;align-items:flex-end;margin:0 0 1rem}"
	".inbox-filter label{display:flex;flex-direction:column;font-size:.85em}"
	".inbox-list{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:.5rem}"
	".inbox-row{display:flex;flex-wrap:wrap;gap:.25rem .75rem;align-items:baseline;padding:.75rem;border:1px solid var(--border);"
	"border-radius:.5rem;color:inherit;text-decoration:none;background:var(--surface)}"
	".inbox-row:hover{border-color:var(--accent)}"
	".inbox-row .who{font-weight:600;flex:1 1 12rem;min-width:0}"
	".inbox-row .what{flex:1 1 100%;min-width:0;color:var(--text-muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
	".inbox-row .when,.inbox-meta{font-size:.85em;color:var(--text-muted)}"
	".inbox-badge{display:inline-block;padding:.05rem .45rem;border-radius:999px;border:1px solid var(--border);font-size:.8em}"
	".inbox-badge.open{background:var(--accent-soft)}"
	".inbox-thread{list-style:none;margin:0 0 1rem;padding:0;display:flex;flex-direction:column;gap:.6rem}"
	".inbox-thread li{max-width:88%;padding:.6rem .8rem;border-radius:.75rem;border:1px solid var(--border);overflow-wrap:anywhere}"
	".inbox-thread .in{align-self:flex-start;background:var(--surface-sunken)}"
	".inbox-thread .out{align-self:flex-end;background:var(--accent-soft)}"
	".inbox-thread .head{display:flex;flex-wrap:wrap;gap:.4rem;align-items:center;font-size:.8em;color:var(--text-muted);margin-bottom:.25rem}"
	".inbox-text{white-space:pre-wrap;margin:.2rem 0}.inbox-subject{display:block}"
	".inbox-transcript{font-style:italic}.inbox-note,.inbox-by{display:block;font-size:.8em;color:var(--text-muted)}"
	".inbox-open{font-size:.85em;margin-right:.5rem}"
	".inbox-controls{display:flex;flex-wrap:wrap;gap:.5rem;align-items:center}"
	".inbox-controls form{display:flex;flex-wrap:wrap;gap:.4rem;align-items:center;margin:0}"
	".inbox-call{display:flex;flex-wrap:wrap;gap:.5rem;align-items:center}.inbox-call form{margin:0}"
	".inbox-reply textarea{width:100%;box-sizing:border-box}"
	".inbox-history{font-size:.85em;color:var(--text-muted);padding-left:1rem}"
	"@media (max-width:600px){.inbox-thread li{max-width:100%}}</style>";

static void
venture_web_inbox_when(VentureWebServer *self, GString *html, GDateTime *when)
{
	g_autoptr(GDateTime) local = NULL;
	g_autofree gchar *text = NULL;
	if (when == NULL)
		return;
	local = g_date_time_to_timezone(when, venture_context_get_timezone(self->context));
	text = g_date_time_format(local, "%Y-%m-%d %H:%M");
	venture_html_escape_append(html, text);
}

static const gchar *
venture_web_inbox_status_name(gint status)
{
	return status == VENTURE_INBOX_STATUS_WAITING ? "waiting" : status == VENTURE_INBOX_STATUS_CLOSED ? "closed" : "open";
}

static gint
venture_web_inbox_status_parse(const gchar *text)
{
	if (g_strcmp0(text, "open") == 0) return VENTURE_INBOX_STATUS_OPEN;
	if (g_strcmp0(text, "waiting") == 0) return VENTURE_INBOX_STATUS_WAITING;
	if (g_strcmp0(text, "closed") == 0) return VENTURE_INBOX_STATUS_CLOSED;
	return -1;
}

static HtmxResponse *
venture_web_inbox_error(const GError *error)
{
	HtmxResponse *response = venture_web_error_response(error);
	if (error != NULL && error->domain == VENTURE_INBOX_ERROR)
		htmx_response_set_status(response, 429);
	return response;
}

static gchar *
venture_web_inbox_username(VentureDatabase *db, gint64 user_id)
{
	g_autoptr(VentureEntity) user = NULL;
	gchar *name = NULL;
	if (user_id <= 0)
		return NULL;
	user = venture_database_get(db, VENTURE_TYPE_USER, user_id, NULL);
	if (user != NULL)
		g_object_get(user, "username", &name, NULL);
	return name;
}

static VentureEntity *
venture_web_inbox_conversation(VentureWebServer *self, GHashTable *params, GError **error)
{
	gint64 id = 0;
	VentureEntity *row;
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &id, NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Conversation not found");
		return NULL;
	}
	row = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_INBOX_CONVERSATION, id, NULL);
	if (row == NULL || venture_entity_is_deleted(row))
	{
		g_clear_object(&row);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Conversation not found");
	}
	return row;
}

static VentureEntity *
venture_web_inbox_contact_of(VentureWebServer *self, VentureEntity *conversation)
{
	gint64 id = 0;
	g_object_get(conversation, "contact-id", &id, NULL);
	return venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_CONTACT, id, NULL);
}

/* The filter from a query string, the same for the page and the API. */
static void
venture_web_inbox_filter(VentureWebServer *self, HtmxRequest *request, const VentureAuthPrincipal *principal,
	VentureInboxFilter *filter)
{
	const gchar *view = htmx_request_get_query_param(request, "view");
	const gchar *status = htmx_request_get_query_param(request, "status");
	const gchar *channel = htmx_request_get_query_param(request, "channel");
	const gchar *org = htmx_request_get_query_param(request, "organization_id");
	(void)self;
	filter->organization_id = org != NULL ? g_ascii_strtoll(org, NULL, 10) : 0;
	filter->view = g_strcmp0(view, "mine") == 0 ? VENTURE_INBOX_VIEW_MINE :
		g_strcmp0(view, "unassigned") == 0 ? VENTURE_INBOX_VIEW_UNASSIGNED : VENTURE_INBOX_VIEW_ALL;
	filter->user_id = principal != NULL ? principal->user_id : 0;
	filter->status = venture_string_is_empty(status) || g_strcmp0(status, "any") == 0 ? -1 : venture_web_inbox_status_parse(status);
	if (!venture_string_is_empty(status) && g_strcmp0(status, "any") != 0 && filter->status < 0)
		filter->status = VENTURE_INBOX_STATUS_OPEN;
	filter->channel = venture_inbox_source_lookup(channel) != NULL ? channel : NULL;
	filter->limit = 200;
}

/* The latest item, for the list. */
static VentureInboxItem *
venture_web_inbox_latest(VentureWebServer *self, VentureEntity *contact)
{
	VentureInboxService *inbox = venture_inbox_service_get(venture_context_get_database(self->context));
	g_autoptr(GPtrArray) items = venture_inbox_service_thread(inbox, contact, NULL, 1, NULL);
	VentureInboxItem *item;
	if (items == NULL || items->len == 0)
		return NULL;
	item = g_ptr_array_index(items, items->len - 1);
	return venture_inbox_item_new(item->source, item->record, item->at, item->inbound);
}

/* --- The list ---------------------------------------------------------------------- */

static void
venture_web_inbox_tab(GString *html, const gchar *label, const gchar *view, const gchar *current,
	const gchar *status, const gchar *channel)
{
	g_string_append_printf(html, "<a class=\"seg%s\" href=\"/conversations?view=%s", g_strcmp0(view, current) == 0 ? " active" : "", view);
	if (status != NULL)
	{
		g_string_append(html, "&amp;status=");
		venture_html_escape_append(html, status);
	}
	if (channel != NULL)
	{
		g_string_append(html, "&amp;channel=");
		venture_html_escape_append(html, channel);
	}
	g_string_append_printf(html, "\">%s</a>", label);
}

static HtmxResponse *
venture_web_inbox_list_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) html = g_string_new(venture_web_inbox_style);
	g_autoptr(GPtrArray) rows = NULL, sources = venture_inbox_sources();
	g_autoptr(GError) error = NULL;
	const gchar *view, *status;
	VentureInboxFilter filter;
	HtmxResponse *gate;
	guint i;
	(void)params;
	if ((gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_require_module_ui(self, request, "inbox")) != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_web_inbox_filter(self, request, principal, &filter);
	rows = venture_inbox_service_list(venture_inbox_service_get(db), &filter, &error);
	if (rows == NULL)
		return venture_web_inbox_error(error);
	view = filter.view == VENTURE_INBOX_VIEW_MINE ? "mine" : filter.view == VENTURE_INBOX_VIEW_UNASSIGNED ? "unassigned" : "all";
	status = filter.status < 0 ? "any" : venture_web_inbox_status_name(filter.status);
	g_string_append(html, "<h1>Conversations</h1><p class=\"muted\">Every customer's email, texts, calls and web chat, one thread per contact. "
		"Your own notifications are in <a href=\"/inbox\">Inbox</a>.</p><div class=\"inbox-tabs segmented\">");
	venture_web_inbox_tab(html, "Mine", "mine", view, status, filter.channel);
	venture_web_inbox_tab(html, "Unassigned", "unassigned", view, status, filter.channel);
	venture_web_inbox_tab(html, "All", "all", view, status, filter.channel);
	g_string_append_printf(html, "</div><form class=\"inbox-filter\" method=\"get\" action=\"/conversations\">"
		"<input type=\"hidden\" name=\"view\" value=\"%s\"><label>Status<select name=\"status\">", view);
	{
		static const gchar *const statuses[] = { "open", "waiting", "closed", "any" };
		static const gchar *const labels[] = { "Open", "Waiting on them", "Closed", "Any" };
		gsize s;
		for (s = 0; s < G_N_ELEMENTS(statuses); s++)
			g_string_append_printf(html, "<option value=\"%s\"%s>%s</option>", statuses[s],
				g_strcmp0(statuses[s], status) == 0 ? " selected" : "", labels[s]);
	}
	g_string_append(html, "</select></label><label>Channel<select name=\"channel\"><option value=\"\">Any</option>");
	for (i = 0; i < sources->len; i++)
	{
		const VentureInboxSource *source = g_ptr_array_index(sources, i);
		g_string_append_printf(html, "<option value=\"%s\"%s>", source->channel, g_strcmp0(source->channel, filter.channel) == 0 ? " selected" : "");
		venture_html_escape_append(html, source->label);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label><button class=\"btn\" type=\"submit\">Show</button></form>");
	if (rows->len == 0)
		g_string_append(html, "<p class=\"muted\">No conversations here. A new email, text, call or chat from a contact starts one.</p>");
	else
		g_string_append(html, "<ol class=\"inbox-list\">");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) contact = venture_web_inbox_contact_of(self, row);
		g_autoptr(VentureInboxItem) latest = NULL;
		g_autoptr(GDateTime) last = NULL;
		g_autofree gchar *name = NULL, *owner = NULL, *summary = NULL;
		gint row_status = 0;
		gint64 owner_id = 0;
		g_object_get(row, "status", &row_status, "owner-id", &owner_id, "last-message-at", &last, NULL);
		if (contact != NULL)
		{
			g_object_get(contact, "name", &name, NULL);
			latest = venture_web_inbox_latest(self, contact);
		}
		if (latest != NULL)
		{
			g_autoptr(VentureContext) context = g_object_ref(self->context);
			summary = latest->source->summary != NULL ? latest->source->summary(context, latest) : NULL;
		}
		owner = venture_web_inbox_username(db, owner_id);
		g_string_append_printf(html, "<li><a class=\"inbox-row\" href=\"/conversations/%" G_GINT64_FORMAT "\"><span class=\"who\">",
			venture_entity_get_id(row));
		venture_html_escape_append(html, !venture_string_is_empty(name) ? name : "(contact removed)");
		g_string_append_printf(html, "</span><span class=\"inbox-badge %s\">%s</span>", venture_web_inbox_status_name(row_status),
			row_status == VENTURE_INBOX_STATUS_WAITING ? "Waiting" : row_status == VENTURE_INBOX_STATUS_CLOSED ? "Closed" : "Open");
		g_string_append(html, "<span class=\"when\">");
		venture_html_escape_append(html, owner != NULL ? owner : "Unassigned");
		g_string_append(html, " · ");
		venture_web_inbox_when(self, html, last);
		g_string_append(html, "</span><span class=\"what\">");
		if (latest != NULL)
		{
			venture_html_escape_append(html, latest->source->label);
			g_string_append(html, latest->inbound ? " from them: " : " from us: ");
		}
		venture_html_escape_append(html, summary != NULL ? summary : "");
		g_string_append(html, "</span></a></li>");
	}
	if (rows->len > 0)
		g_string_append(html, "</ol>");
	return venture_web_html_response(venture_web_page(self, request, "/conversations", "Conversations", html->str), 200);
}

/* --- One conversation ---------------------------------------------------------------- */

static void
venture_web_inbox_history(VentureWebServer *self, GString *html, VentureEntity *row)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) entries = NULL;
	guint i, shown = 0;
	venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ, "inbox_conversation", NULL);
	venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(row), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 20);
	entries = venture_database_find(venture_context_get_database(self->context), query, NULL);
	if (entries == NULL)
		return;
	for (i = 0; i < entries->len; i++)
	{
		VentureEntity *entry = g_ptr_array_index(entries, i);
		g_autofree gchar *actor = NULL, *diff = NULL;
		g_autoptr(GDateTime) when = NULL;
		const gchar *what;
		g_object_get(entry, "actor", &actor, "diff", &diff, "occurred-at", &when, NULL);
		if (diff == NULL)
			continue;
		what = strstr(diff, "owner_id") != NULL ? "changed the owner" : strstr(diff, "\"status\"") != NULL ? "changed the status" : NULL;
		if (what == NULL)
			continue;
		if (shown++ == 0)
			g_string_append(html, "<details><summary>Assignment and status history</summary><ul class=\"inbox-history\">");
		g_string_append(html, "<li>");
		venture_web_inbox_when(self, html, when);
		g_string_append(html, " · ");
		venture_html_escape_append(html, actor != NULL ? actor : "Someone");
		g_string_append_printf(html, " %s</li>", what);
	}
	if (shown > 0)
		g_string_append(html, "</ul></details>");
}

static void
venture_web_inbox_assign_form(VentureWebServer *self, GString *html, VentureEntity *row, gint64 me)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(GPtrArray) members = NULL;
	gint64 owner_id = 0, id = venture_entity_get_id(row);
	guint i;
	g_object_get(row, "owner-id", &owner_id, NULL);
	venture_query_set_organization(query, venture_entity_get_organization_id(row));
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_set_limit(query, 200);
	members = venture_database_find(db, query, NULL);
	g_string_append_printf(html, "<form method=\"post\" action=\"/conversations/%" G_GINT64_FORMAT "/assign\">"
		"<label class=\"sr-only\" for=\"owner\">Owner</label><select id=\"owner\" name=\"owner_id\"><option value=\"0\">Unassigned</option>", id);
	for (i = 0; members != NULL && i < members->len; i++)
	{
		gint64 user_id = 0;
		g_autofree gchar *name = NULL;
		g_object_get(g_ptr_array_index(members, i), "user-id", &user_id, NULL);
		name = venture_web_inbox_username(db, user_id);
		if (name == NULL)
			continue;
		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>", user_id, user_id == owner_id ? " selected" : "");
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}
	/* An owner who is no longer a member still shows as the owner. */
	if (owner_id > 0)
	{
		gboolean listed = FALSE;
		for (i = 0; members != NULL && i < members->len; i++)
		{
			gint64 user_id = 0;
			g_object_get(g_ptr_array_index(members, i), "user-id", &user_id, NULL);
			listed |= user_id == owner_id;
		}
		if (!listed)
		{
			g_autofree gchar *name = venture_web_inbox_username(db, owner_id);
			g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\" selected>", owner_id);
			venture_html_escape_append(html, name != NULL ? name : "(removed person)");
			g_string_append(html, "</option>");
		}
	}
	g_string_append(html, "</select><button class=\"btn\" type=\"submit\">Assign</button></form>");
	if (me > 0 && me != owner_id)
		g_string_append_printf(html, "<form method=\"post\" action=\"/conversations/%" G_GINT64_FORMAT "/assign\">"
			"<input type=\"hidden\" name=\"owner_id\" value=\"me\"><button class=\"btn\" type=\"submit\">Assign to me</button></form>", id);
}

static void
venture_web_inbox_reply_form(VentureWebServer *self, GString *html, VentureEntity *row, VentureEntity *contact)
{
	VentureInboxService *inbox = venture_inbox_service_get(venture_context_get_database(self->context));
	g_autoptr(VentureInboxItem) answering = NULL;
	g_autoptr(GPtrArray) sources = venture_inbox_sources();
	const VentureInboxSource *source = venture_inbox_service_reply_channel(inbox, contact, NULL, &answering, NULL);
	guint i;
	g_string_append(html, "<section class=\"card inbox-reply\" id=\"reply\"><div class=\"card-head\"><h2>");
	if (source == NULL)
		g_string_append(html, "Reply");
	else if (source->reply != NULL)
	{
		g_string_append(html, "Reply by ");
		{
			g_autofree gchar *label = g_ascii_strdown(source->label, -1);
			venture_html_escape_append(html, label);
		}
	}
	else
	{
		g_string_append(html, "They called: ");
		g_string_append(html, "call back");
	}
	g_string_append(html, "</h2></div><div class=\"card-body\">");
	if (source != NULL && source->reply == NULL && source->action != NULL)
	{
		g_autoptr(VentureContext) context = g_object_ref(self->context);
		source->action(context, contact, html);
		g_string_append(html, "<p class=\"muted\">Or write to them instead:</p>");
	}
	else if (source == NULL)
		g_string_append(html, "<p class=\"muted\">Nothing has come from them yet.</p>");
	g_string_append_printf(html, "<form method=\"post\" action=\"/conversations/%" G_GINT64_FORMAT "/reply\">"
		"<label>Message<textarea name=\"body\" rows=\"4\" maxlength=\"%d\" required></textarea></label>"
		"<div class=\"inbox-controls\"><label>Send by<select name=\"channel\">",
		venture_entity_get_id(row), VENTURE_INBOX_CHAT_BODY_MAX * 32);
	if (source != NULL && source->reply != NULL)
	{
		g_string_append(html, "<option value=\"\">");
		venture_html_escape_append(html, source->label);
		g_string_append(html, ", as they wrote last</option>");
	}
	for (i = 0; i < sources->len; i++)
	{
		const VentureInboxSource *other = g_ptr_array_index(sources, i);
		g_autoptr(VentureContext) context = g_object_ref(self->context);
		if (other->reply == NULL || other == source || (other->module != NULL && !venture_context_module_enabled(context, other->module)))
			continue;
		g_string_append_printf(html, "<option value=\"%s\">", other->channel);
		venture_html_escape_append(html, other->label);
		g_string_append(html, "</option>");
	}
	g_string_append(html, "</select></label><button class=\"btn btn-primary\" type=\"submit\">Send</button></div>"
		"<p class=\"muted\">A text obeys their text consent and quiet hours; an email goes through the outbox as a reply to "
		"their message; a chat reply shows on their chat page.</p></form></div></section>");
}

static HtmxResponse *
venture_web_inbox_page(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureInboxService *inbox = venture_inbox_service_get(db);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) row = NULL, contact = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GString) html = g_string_new(venture_web_inbox_style);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureContext) context = g_object_ref(self->context);
	g_autofree gchar *name = NULL, *owner = NULL, *title = NULL, *phone = NULL, *email = NULL;
	const gchar *notice = htmx_request_get_query_param(request, "notice");
	gint status = 0;
	gint64 owner_id = 0, id;
	HtmxResponse *gate;
	guint i;
	if ((gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_require_module_ui(self, request, "inbox")) != NULL) return gate;
	row = venture_web_inbox_conversation(self, params, &error);
	if (row == NULL)
		return venture_web_inbox_error(error);
	contact = venture_web_inbox_contact_of(self, row);
	if (contact == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "This conversation's contact is gone");
		return venture_web_inbox_error(error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	id = venture_entity_get_id(row);
	g_object_get(row, "status", &status, "owner-id", &owner_id, NULL);
	g_object_get(contact, "name", &name, "phone", &phone, "email", &email, NULL);
	owner = venture_web_inbox_username(db, owner_id);
	items = venture_inbox_service_thread(inbox, contact, NULL, 200, &error);
	if (items == NULL)
		return venture_web_inbox_error(error);
	/* One block, so the page arrives whole rather than card by card. */
	g_string_append(html, "<div class=\"inbox-page\"><p><a href=\"/conversations\">← Conversations</a></p><h1>");
	venture_html_escape_append(html, name);
	g_string_append(html, "</h1><p class=\"inbox-meta\">");
	if (!venture_string_is_empty(email))
	{
		venture_html_escape_append(html, email);
		g_string_append(html, " · ");
	}
	if (!venture_string_is_empty(phone))
	{
		venture_html_escape_append(html, phone);
		g_string_append(html, " · ");
	}
	g_string_append_printf(html, "<a href=\"/e/contact/%" G_GINT64_FORMAT "\">Contact page</a></p>", venture_entity_get_id(contact));
	if (!venture_string_is_empty(notice))
	{
		g_string_append(html, "<div class=\"notice positive\"><div>");
		venture_html_escape_append(html, notice);
		g_string_append(html, "</div></div>");
	}
	g_string_append_printf(html, "<section class=\"card\"><div class=\"card-body inbox-controls\"><span class=\"inbox-badge %s\">%s</span>"
		"<span>Owner: ", venture_web_inbox_status_name(status),
		status == VENTURE_INBOX_STATUS_WAITING ? "Waiting on them" : status == VENTURE_INBOX_STATUS_CLOSED ? "Closed" : "Open");
	venture_html_escape_append(html, owner != NULL ? owner : "Unassigned");
	g_string_append(html, "</span>");
	{
		static const gchar *const values[] = { "open", "waiting", "closed" };
		static const gchar *const labels[] = { "Reopen", "Waiting on them", "Close" };
		gsize s;
		g_string_append_printf(html, "<form method=\"post\" action=\"/conversations/%" G_GINT64_FORMAT "/status\">", id);
		for (s = 0; s < G_N_ELEMENTS(values); s++)
			if ((gint)s != status)
				g_string_append_printf(html, "<button class=\"btn\" name=\"status\" value=\"%s\">%s</button>", values[s], labels[s]);
		g_string_append(html, "</form>");
	}
	venture_web_inbox_assign_form(self, html, row, principal != NULL ? principal->user_id : 0);
	g_string_append(html, "</div></section>");
	if (items->len == 0)
		g_string_append(html, "<p class=\"muted\">No messages.</p>");
	else
		g_string_append(html, "<ol class=\"inbox-thread\">");
	for (i = 0; i < items->len; i++)
	{
		VentureInboxItem *item = g_ptr_array_index(items, i);
		g_string_append_printf(html, "<li class=\"%s inbox-%s\"><div class=\"head\"><span class=\"inbox-badge\">", item->inbound ? "in" : "out",
			item->source->channel);
		venture_html_escape_append(html, item->source->label);
		g_string_append_printf(html, "</span><span>%s</span><time>", item->inbound ? "From them" : "From us");
		venture_web_inbox_when(self, html, item->at);
		g_string_append(html, "</time></div>");
		item->source->render(context, item, html);
		g_string_append(html, "</li>");
	}
	if (items->len > 0)
		g_string_append(html, "</ol>");
	venture_web_inbox_reply_form(self, html, row, contact);
	venture_web_inbox_history(self, html, row);
	g_string_append(html, "</div>");
	title = g_strdup_printf("Conversation with %s", name != NULL ? name : "a contact");
	return venture_web_html_response(venture_web_page(self, request, "/conversations", title, html->str), 200);
}

/* --- Changing a conversation: the page's forms and the API ---------------------------- */

static JsonObject *
venture_web_inbox_body(HtmxRequest *request, gboolean ui)
{
	g_autoptr(JsonNode) json = NULL;
	JsonObject *values = json_object_new();
	static const gchar *const fields[] = { "owner_id", "status", "body", "channel" };
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

static gint64
venture_web_inbox_owner(JsonObject *values, const VentureAuthPrincipal *principal, gboolean *ok)
{
	JsonNode *node = json_object_get_member(values, "owner_id");
	*ok = TRUE;
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return 0;
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64)
		return json_node_get_int(node);
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
	{
		const gchar *text = json_node_get_string(node);
		gint64 id = 0;
		if (g_strcmp0(text, "me") == 0)
			return principal != NULL ? principal->user_id : 0;
		if (g_ascii_string_to_signed(text, 10, 0, G_MAXINT64, &id, NULL))
			return id;
	}
	*ok = FALSE;
	return 0;
}

static HtmxResponse *
venture_web_inbox_done(VentureWebServer *self, gboolean ui, VentureEntity *row, const gchar *notice, JsonNode *extra, guint status)
{
	if (ui)
	{
		g_autofree gchar *escaped = notice != NULL ? g_uri_escape_string(notice, NULL, FALSE) : NULL;
		g_autofree gchar *path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT "%s%s", venture_entity_get_id(row),
			escaped != NULL ? "?notice=" : "", escaped != NULL ? escaped : "");
		return venture_web_redirect_to(path);
	}
	if (extra != NULL)
		return venture_web_json_response(extra, status);
	{
		g_autoptr(VentureEntity) fresh = venture_database_get(venture_context_get_database(self->context),
			VENTURE_TYPE_INBOX_CONVERSATION, venture_entity_get_id(row), NULL);
		g_autoptr(JsonNode) json = venture_serializable_to_json(VENTURE_SERIALIZABLE(fresh != NULL ? fresh : row), FALSE);
		return venture_web_json_response(json, status);
	}
}

/* POST /conversations/:id/{assign,status,reply} and their /api/v1 twins. */
static HtmxResponse *
venture_web_inbox_change(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureInboxService *inbox = venture_inbox_service_get(db);
	const gchar *path = htmx_request_get_path(request);
	gboolean ui = !g_str_has_prefix(path, "/api/");
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(JsonObject) values = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	HtmxResponse *gate;
	VentureActor actor;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "inbox")) != NULL) return gate;
	row = venture_web_inbox_conversation(self, params, &error);
	if (row == NULL)
		return venture_web_inbox_error(error);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	values = venture_web_inbox_body(request, ui);
	if (g_str_has_suffix(path, "/assign"))
	{
		gboolean ok;
		gint64 owner = venture_web_inbox_owner(values, principal, &ok);
		if (!ok)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "owner_id is a person's id, me, or 0 to unassign");
		else if (venture_inbox_service_assign(inbox, row, owner, &actor, &error))
			return venture_web_inbox_done(self, ui, row, owner > 0 ? "Assigned" : "Unassigned", NULL, 200);
		return venture_web_inbox_error(error);
	}
	if (g_str_has_suffix(path, "/status"))
	{
		gint status = venture_web_inbox_status_parse(venture_json_object_get_string(values, "status", NULL));
		if (status < 0)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "status is open, waiting or closed");
		else if (venture_inbox_service_set_status(inbox, row, (VentureInboxStatus)status, now, &actor, &error))
			return venture_web_inbox_done(self, ui, row, NULL, NULL, 200);
		return venture_web_inbox_error(error);
	}
	{
		const gchar *channel = venture_json_object_get_string(values, "channel", NULL);
		g_autofree gchar *used = NULL, *outcome = NULL;
		/* The reply author people see is the person, not a token's label. */
		actor.name = actor.name != NULL ? actor.name : "Staff";
		outcome = venture_inbox_service_reply(inbox, row, venture_string_is_empty(channel) ? NULL : channel,
			venture_json_object_get_string(values, "body", NULL), now, &actor, &used, &error);
		if (outcome == NULL)
			return venture_web_inbox_error(error);
		if (!ui)
		{
			g_autoptr(JsonBuilder) builder = json_builder_new();
			g_autoptr(JsonNode) result = NULL;
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "channel");
			json_builder_add_string_value(builder, used);
			json_builder_set_member_name(builder, "outcome");
			json_builder_add_string_value(builder, outcome);
			json_builder_end_object(builder);
			result = json_builder_get_root(builder);
			return venture_web_inbox_done(self, FALSE, row, NULL, result, 201);
		}
		return venture_web_inbox_done(self, TRUE, row, outcome, NULL, 200);
	}
}

/* --- The API ------------------------------------------------------------------------- */

static JsonNode *
venture_web_inbox_detail_json(VentureWebServer *self, VentureEntity *row, VentureEntity *contact, GError **error)
{
	VentureInboxService *inbox = venture_inbox_service_get(venture_context_get_database(self->context));
	g_autoptr(GPtrArray) items = venture_inbox_service_thread(inbox, contact, NULL, 200, error);
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(VentureContext) context = g_object_ref(self->context);
	const VentureInboxSource *reply;
	guint i;
	if (items == NULL)
		return NULL;
	reply = venture_inbox_service_reply_channel(inbox, contact, NULL, NULL, NULL);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "conversation");
	if (row != NULL)
		json_builder_add_value(builder, venture_serializable_to_json(VENTURE_SERIALIZABLE(row), FALSE));
	else
		json_builder_add_null_value(builder);
	json_builder_set_member_name(builder, "reply_channel");
	if (reply != NULL)
		json_builder_add_string_value(builder, reply->channel);
	else
		json_builder_add_null_value(builder);
	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);
	for (i = 0; i < items->len; i++)
	{
		VentureInboxItem *item = g_ptr_array_index(items, i);
		g_autofree gchar *when = venture_time_to_string(item->at);
		g_autofree gchar *text = item->source->summary != NULL ? item->source->summary(context, item) : NULL;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "channel");
		json_builder_add_string_value(builder, item->source->channel);
		json_builder_set_member_name(builder, "record_type");
		json_builder_add_string_value(builder, venture_entity_get_entity_name(item->record));
		json_builder_set_member_name(builder, "record_id");
		json_builder_add_int_value(builder, venture_entity_get_id(item->record));
		json_builder_set_member_name(builder, "at");
		json_builder_add_string_value(builder, when);
		json_builder_set_member_name(builder, "inbound");
		json_builder_add_boolean_value(builder, item->inbound);
		json_builder_set_member_name(builder, "summary");
		json_builder_add_string_value(builder, text != NULL ? text : "");
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* GET /api/v1/conversations?view=&status=&channel=&organization_id= */
static HtmxResponse *
venture_web_inbox_api_list(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	JsonArray *array;
	VentureInboxFilter filter;
	HtmxResponse *gate;
	guint i;
	(void)params;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "inbox")) != NULL) return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_web_inbox_filter(self, request, principal, &filter);
	rows = venture_inbox_service_list(venture_inbox_service_get(venture_context_get_database(self->context)), &filter, &error);
	if (rows == NULL)
		return venture_web_inbox_error(error);
	array = json_array_new();
	for (i = 0; i < rows->len; i++)
		json_array_add_element(array, venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(rows, i)), FALSE));
	result = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(result, json_object_new());
	json_object_set_array_member(json_node_get_object(result), "conversations", array);
	return venture_web_json_response(result, 200);
}

/* GET /api/v1/conversations/:id: the conversation and its thread. */
static HtmxResponse *
venture_web_inbox_api_get(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureEntity) row = NULL, contact = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	HtmxResponse *gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "inbox")) != NULL) return gate;
	row = venture_web_inbox_conversation(self, params, &error);
	if (row == NULL)
		return venture_web_inbox_error(error);
	contact = venture_web_inbox_contact_of(self, row);
	if (contact == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "This conversation's contact is gone");
		return venture_web_inbox_error(error);
	}
	result = venture_web_inbox_detail_json(self, row, contact, &error);
	return result != NULL ? venture_web_json_response(result, 200) : venture_web_inbox_error(error);
}

/* GET /contacts/:id/conversation (to the page) and GET
 * /api/v1/contacts/:id/conversation (the thread, with the conversation
 * when there is one). A contact with messages from before the inbox gets
 * its conversation caught up here. */
static HtmxResponse *
venture_web_inbox_contact(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	gboolean ui = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	g_autoptr(VentureEntity) contact = NULL, row = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	if (ui && (gate = venture_web_ui_require_session(self, request)) != NULL) return gate;
	if ((gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER)) != NULL) return gate;
	if ((gate = venture_web_require_module_api(self, "inbox")) != NULL) return gate;
	contact = venture_database_get(db, VENTURE_TYPE_CONTACT, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), NULL);
	if (contact == NULL || venture_entity_is_deleted(contact))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Contact not found");
		return venture_web_inbox_error(error);
	}
	row = venture_inbox_service_conversation(venture_inbox_service_get(db), contact, TRUE, &error);
	if (ui)
	{
		g_autofree gchar *path = NULL;
		if (row == NULL)
		{
			g_autoptr(GString) html = g_string_new("<h1>No conversation yet</h1><p>Nothing has come from this contact or gone to them "
				"by email, text, phone or chat. Their conversation starts with the first message.</p>");
			g_string_append_printf(html, "<p><a class=\"btn\" href=\"/e/contact/%" G_GINT64_FORMAT "\">Back to the contact</a></p>",
				venture_entity_get_id(contact));
			return venture_web_html_response(venture_web_page(self, request, "/conversations", "No conversation yet", html->str), 200);
		}
		path = g_strdup_printf("/conversations/%" G_GINT64_FORMAT, venture_entity_get_id(row));
		return venture_web_redirect_to(path);
	}
	g_clear_error(&error);
	{
		g_autoptr(JsonNode) result = venture_web_inbox_detail_json(self, row, contact, &error);
		return result != NULL ? venture_web_json_response(result, 200) : venture_web_inbox_error(error);
	}
}

/* On a contact's page: the way into their conversation. */
static void
venture_web_inbox_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	if (!VENTURE_IS_CONTACT(record) || !venture_web_module_enabled(self, "inbox"))
		return;
	g_string_append_printf(content, "<p><a class=\"btn\" href=\"/contacts/%" G_GINT64_FORMAT "/conversation\">Conversation</a> "
		"<span class=\"muted\">Their email, texts, calls and chat in one thread, with an owner and a status.</span></p>",
		venture_entity_get_id(record));
}

/* --- Web chat settings ------------------------------------------------------------------- */

static gchar *
venture_web_inbox_origin(VentureWebServer *self)
{
	g_autofree gchar *configured = NULL;
	g_object_get(venture_context_get_config(self->context), "server-base-url", &configured, NULL);
	return g_strdup(!venture_string_is_empty(configured) ? configured : venture_web_server_get_base_url(self));
}

static HtmxResponse *
venture_web_inbox_chat_settings(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gint managers[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureInboxService *inbox = venture_inbox_service_get(db);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GString) html = g_string_new(venture_web_inbox_style);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INBOX_CHAT_WIDGET);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *origin = venture_web_inbox_origin(self);
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	VentureActor actor;
	gint64 org = 0;
	guint i;
	if (gate != NULL) return gate;
	if ((gate = venture_web_require_module_ui(self, request, "inbox")) != NULL) return gate;
	if (!g_ascii_string_to_signed(g_hash_table_lookup(params, "id"), 10, 1, G_MAXINT64, &org, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(db), principal, org, managers, G_N_ELEMENTS(managers)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Web chat settings need an owner or administrator of the organization");
		return venture_web_error_response(error);
	}
	venture_auth_to_actor(principal, &actor);
	g_string_append(html, "<div class=\"inbox-page\"><h1>Web chat</h1>");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *id_text = htmx_request_get_form_value(request, "widget_id");
		g_autoptr(VentureEntity) widget = id_text != NULL ? venture_database_get(db, VENTURE_TYPE_INBOX_CHAT_WIDGET, g_ascii_strtoll(id_text, NULL, 10), NULL) : NULL;
		gboolean ok = FALSE;
		if (widget != NULL && venture_entity_get_organization_id(widget) != org)
			g_clear_object(&widget);
		if (g_strcmp0(operation, "create") == 0)
		{
			g_autoptr(VentureEntity) made = venture_inbox_chat_widget_create(inbox, org, htmx_request_get_form_value(request, "name"), &actor, &error);
			ok = made != NULL;
		}
		else if (widget != NULL && g_strcmp0(operation, "update") == 0)
			ok = venture_inbox_chat_widget_update(inbox, widget, htmx_request_get_form_value(request, "greeting"),
				htmx_request_get_form_value(request, "allowed_origins"), &actor, &error);
		else if (widget != NULL && (g_strcmp0(operation, "off") == 0 || g_strcmp0(operation, "on") == 0))
			ok = venture_inbox_chat_widget_set_active(inbox, widget, g_strcmp0(operation, "on") == 0, &actor, &error);
		else
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Choose a web chat operation");
		if (ok)
			g_string_append(html, "<div class=\"notice positive\"><div>Saved.</div></div>");
		else
		{
			g_string_append(html, "<div class=\"notice negative\" role=\"alert\"><div>");
			venture_html_escape_append(html, error != NULL ? error->message : "Not saved");
			g_string_append(html, "</div></div>");
		}
	}
	g_string_append(html, "<p>Put a chat on your site: visitors give their name and an email or phone number and write; it arrives "
		"in Conversations, and your replies show on the visitor's own chat page. See <a href=\"/docs/inbox.html\">the inbox guide</a>.</p>");
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(db, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL, *token = NULL, *greeting = NULL, *origins = NULL;
		gboolean active = FALSE;
		gint64 id = venture_entity_get_id(row);
		g_object_get(row, "name", &name, "token", &token, "greeting", &greeting, "allowed-origins", &origins, "active", &active, NULL);
		g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>");
		venture_html_escape_append(html, name);
		g_string_append_printf(html, "</h2><span class=\"inbox-badge\">%s</span></div><div class=\"card-body\">", active ? "On" : "Off");
		g_string_append(html, "<p>Link to it, or put it in a frame on your site:</p><p><code>");
		venture_html_escape_append(html, origin);
		g_string_append(html, "/webchat/");
		venture_html_escape_append(html, token);
		g_string_append(html, "</code></p><p><code>&lt;iframe src=\"");
		venture_html_escape_append(html, origin);
		g_string_append(html, "/webchat/");
		venture_html_escape_append(html, token);
		g_string_append(html, "\" title=\"Chat with us\" style=\"width:100%;max-width:420px;height:560px;border:0\"&gt;&lt;/iframe&gt;</code></p>");
		g_string_append_printf(html, "<form method=\"post\"><input type=\"hidden\" name=\"operation\" value=\"update\">"
			"<input type=\"hidden\" name=\"widget_id\" value=\"%" G_GINT64_FORMAT "\"><label>Greeting<textarea name=\"greeting\" rows=\"2\" maxlength=\"500\">", id);
		venture_html_escape_append(html, greeting != NULL ? greeting : "");
		g_string_append(html, "</textarea></label><label>Sites that may show it in a frame<textarea name=\"allowed_origins\" rows=\"2\" "
			"placeholder=\"https://www.example.com, one per line; empty allows any\">");
		venture_html_escape_append(html, origins != NULL ? origins : "");
		g_string_append_printf(html, "</textarea></label><div class=\"inbox-controls\"><button class=\"btn btn-primary\" type=\"submit\">Save</button></div></form>"
			"<form method=\"post\"><input type=\"hidden\" name=\"widget_id\" value=\"%" G_GINT64_FORMAT "\">"
			"<button class=\"btn\" name=\"operation\" value=\"%s\">%s</button></form></div></section>", id,
			active ? "off" : "on", active ? "Switch off" : "Switch on");
	}
	g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>Add a web chat</h2></div><div class=\"card-body\">"
		"<form method=\"post\"><input type=\"hidden\" name=\"operation\" value=\"create\"><label>Name<input name=\"name\" maxlength=\"100\" "
		"placeholder=\"Website chat\"></label><button class=\"btn btn-primary\" type=\"submit\">Add</button></form>"
		"<p class=\"muted\">Limits: a name up to 100 characters, a message up to 2,000; one address may start 5 chats and one chat "
		"take 20 messages in 10 minutes. The visitor's page needs no cookie and no account; its address is the key, so they should keep it to themselves.</p>"
		"</div></section></div>");
	return venture_web_html_response(venture_web_page(self, request, "/organizations", "Web chat", html->str), error != NULL ? 400 : 200);
}

/* --- The public chat --------------------------------------------------------------------- */

static const gchar venture_web_webchat_style[] =
	"<style>:root{color-scheme:light dark;--fg:#1d2330;--muted:#5b6475;--bg:#fff;--soft:#f2f4f8;--line:#d9dde5;--accent:#2f5bd3}"
	"@media (prefers-color-scheme:dark){:root{--fg:#e8ebf2;--muted:#a3abbb;--bg:#141821;--soft:#1e2430;--line:#2e3646;--accent:#7d9bff}}"
	"*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,sans-serif;color:var(--fg);background:var(--bg)}"
	"main{max-width:34rem;margin:0 auto;padding:1rem}h1{font-size:1.25rem;margin:.25rem 0 .5rem}"
	"label{display:block;margin:.6rem 0 .2rem;font-weight:600}input,textarea{width:100%;font:inherit;padding:.55rem .6rem;"
	"border:1px solid var(--line);border-radius:.5rem;background:var(--bg);color:var(--fg)}"
	"button{margin-top:.8rem;font:inherit;padding:.6rem 1.1rem;border:0;border-radius:.5rem;background:var(--accent);color:#fff}"
	".hint,.meta{color:var(--muted);font-size:.85rem}.error{border:1px solid #c0392b;border-radius:.5rem;padding:.5rem .7rem;margin:.5rem 0}"
	".trap{position:absolute;left:-10000px;width:1px;height:1px;overflow:hidden}"
	"ol{list-style:none;margin:1rem 0;padding:0;display:flex;flex-direction:column;gap:.5rem}"
	"li{max-width:85%;padding:.5rem .75rem;border-radius:.75rem;border:1px solid var(--line);white-space:pre-wrap;overflow-wrap:anywhere}"
	"li.in{align-self:flex-end;background:var(--soft)}li.out{align-self:flex-start}"
	"li .meta{display:block;white-space:normal}</style>";

static void
venture_web_webchat_headers(HtmxResponse *response, VentureEntity *widget, gboolean script)
{
	g_autoptr(GString) policy = g_string_new("default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; ");
	g_autofree gchar *origins = NULL;
	if (script)
		g_string_append(policy, "script-src 'self'; connect-src 'self'; ");
	g_string_append(policy, "frame-ancestors");
	if (widget != NULL)
		g_object_get(widget, "allowed-origins", &origins, NULL);
	if (venture_string_is_empty(origins))
		g_string_append(policy, " *");
	else
	{
		g_auto(GStrv) parts = g_strsplit_set(origins, "\n\r\t ,", -1);
		guint i;
		g_string_append(policy, " 'self'");
		for (i = 0; parts[i] != NULL; i++)
			if (parts[i][0] != '\0' && strpbrk(parts[i], ";'\"") == NULL)
				g_string_append_printf(policy, " %s", parts[i]);
	}
	htmx_response_add_header(response, "Content-Security-Policy", policy->str);
	/* The visitor's link is their key: no referrer may carry it off. */
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
}

static HtmxResponse *
venture_web_webchat_html(GString *body, guint status, VentureEntity *widget, gboolean script)
{
	g_autoptr(GString) page = g_string_new("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><meta name=\"robots\" content=\"noindex\">"
		"<title>Chat</title>");
	HtmxResponse *response;
	g_string_append(page, venture_web_webchat_style);
	if (script)
		g_string_append(page, "<script src=\"/webchat.js\" defer></script>");
	g_string_append(page, "</head><body><main>");
	g_string_append(page, body->str);
	g_string_append(page, "</main></body></html>");
	response = venture_web_html_response(g_strdup(page->str), status);
	venture_web_webchat_headers(response, widget, script);
	return response;
}

static HtmxResponse *
venture_web_webchat_status(guint status, const gchar *words)
{
	g_autoptr(GString) body = g_string_new("<h1>");
	venture_html_escape_append(body, words);
	g_string_append(body, "</h1>");
	return venture_web_webchat_html(body, status, NULL, FALSE);
}

static gchar *
venture_web_webchat_org_name(VentureDatabase *db, gint64 org)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	g_autoptr(VentureEntity) row = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, NULL);
	gchar *name = NULL;
	if (row != NULL)
		g_object_get(row, "name", &name, NULL);
	return name != NULL ? name : g_strdup("Us");
}

static void
venture_web_webchat_form(GString *body, VentureEntity *widget, const gchar *token, const gchar *problem, HtmxRequest *request)
{
	g_autofree gchar *greeting = NULL;
	static const gchar *const fields[] = { "name", "email", "phone" };
	static const gchar *const labels[] = { "Your name", "Email", "Phone" };
	static const gchar *const types[] = { "text", "email", "tel" };
	static const gchar *const completes[] = { "name", "email", "tel" };
	static const gint lengths[] = { VENTURE_INBOX_CHAT_NAME_MAX, VENTURE_INBOX_CHAT_EMAIL_MAX, VENTURE_INBOX_CHAT_PHONE_MAX };
	gsize i;
	g_object_get(widget, "greeting", &greeting, NULL);
	g_string_append(body, "<h1>Chat with us</h1>");
	if (!venture_string_is_empty(greeting))
	{
		g_string_append(body, "<p>");
		venture_html_escape_append(body, greeting);
		g_string_append(body, "</p>");
	}
	if (problem != NULL)
	{
		g_string_append(body, "<p class=\"error\" role=\"alert\">");
		venture_html_escape_append(body, problem);
		g_string_append(body, "</p>");
	}
	g_string_append(body, "<form method=\"post\" action=\"/webchat/");
	venture_html_escape_append(body, token);
	g_string_append(body, "\">");
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		const gchar *value = request != NULL ? htmx_request_get_form_value(request, fields[i]) : NULL;
		g_string_append_printf(body, "<label for=\"%s\">%s</label><input id=\"%s\" name=\"%s\" type=\"%s\" autocomplete=\"%s\" maxlength=\"%d\"%s value=\"",
			fields[i], labels[i], fields[i], fields[i], types[i], completes[i], lengths[i], i == 0 ? " required" : "");
		venture_html_escape_append(body, value != NULL ? value : "");
		g_string_append(body, "\">");
	}
	g_string_append(body, "<p class=\"hint\">An email address or a phone number, so we can answer you.</p>"
		"<div class=\"trap\" aria-hidden=\"true\"><label for=\"website\">Leave this empty</label>"
		"<input id=\"website\" name=\"website\" tabindex=\"-1\" autocomplete=\"off\"></div>");
	g_string_append_printf(body, "<label for=\"message\">Message</label><textarea id=\"message\" name=\"message\" rows=\"5\" maxlength=\"%d\" required>",
		VENTURE_INBOX_CHAT_BODY_MAX);
	if (request != NULL && htmx_request_get_form_value(request, "message") != NULL)
		venture_html_escape_append(body, htmx_request_get_form_value(request, "message"));
	g_string_append(body, "</textarea><button type=\"submit\">Send</button></form>"
		"<p class=\"hint\">You get a private page for this chat; keep its address to come back to our replies.</p>");
}

static gboolean
venture_web_webchat_too_big(HtmxRequest *request)
{
	GBytes *bytes = htmx_request_get_body_bytes(request);
	return bytes != NULL && g_bytes_get_size(bytes) > VENTURE_INBOX_CHAT_MAX_REQUEST;
}

/* GET and POST /webchat/:token: the intake. */
static HtmxResponse *
venture_web_webchat_intake(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureInboxService *inbox = venture_inbox_service_get(db);
	/* Whatever cookie came along grants nothing here. */
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	const gchar *token = g_hash_table_lookup(params, "token");
	g_autoptr(VentureEntity) widget = NULL, session = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *visitor = NULL, *remote = NULL, *location = NULL;
	VentureInboxChatIntake intake;
	HtmxResponse *response;
	if (venture_web_webchat_too_big(request))
		return venture_web_webchat_status(413, "That is too long to send");
	if (!venture_context_module_enabled(self->context, "inbox") || (widget = venture_inbox_chat_widget_lookup(inbox, token)) == NULL)
		return venture_web_webchat_status(404, "This chat is not available");
	if (htmx_request_get_method(request) != HTMX_METHOD_POST)
	{
		venture_web_webchat_form(body, widget, token, NULL, NULL);
		return venture_web_webchat_html(body, 200, widget, FALSE);
	}
	remote = venture_auth_remote_address(request);
	intake.name = htmx_request_get_form_value(request, "name");
	intake.email = htmx_request_get_form_value(request, "email");
	intake.phone = htmx_request_get_form_value(request, "phone");
	intake.body = htmx_request_get_form_value(request, "message");
	intake.trap = htmx_request_get_form_value(request, "website");
	intake.remote = remote;
	session = venture_inbox_chat_start(inbox, token, &intake, now, &visitor, &error);
	if (session == NULL && error == NULL)
	{
		g_string_append(body, "<h1>Thank you</h1><p>We have your message.</p>");
		return venture_web_webchat_html(body, 200, widget, FALSE);
	}
	if (session == NULL)
	{
		guint status = error->domain == VENTURE_INBOX_ERROR ? 429 : g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION) ? 400 : 500;
		venture_web_webchat_form(body, widget, token, status == 500 ? "Something went wrong; please try again" : error->message, request);
		if (status == 500)
			g_message("Web chat intake failed: %s", error->message);
		return venture_web_webchat_html(body, status, widget, FALSE);
	}
	location = g_strdup_printf("/webchat/c/%s", visitor);
	response = htmx_response_new();
	htmx_response_set_status(response, 303);
	htmx_response_add_header(response, "Location", location);
	venture_web_webchat_headers(response, widget, FALSE);
	return response;
}

static VentureEntity *
venture_web_webchat_session(VentureWebServer *self, GHashTable *params, VentureEntity **widget)
{
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureEntity *session;
	gint64 widget_id = 0;
	if (!venture_context_module_enabled(self->context, "inbox"))
		return NULL;
	session = venture_inbox_chat_session_lookup(venture_inbox_service_get(db), g_hash_table_lookup(params, "token"));
	if (session != NULL && widget != NULL)
	{
		g_object_get(session, "widget-id", &widget_id, NULL);
		*widget = venture_database_get(db, VENTURE_TYPE_INBOX_CHAT_WIDGET, widget_id, NULL);
	}
	return session;
}

/* GET and POST /webchat/c/:token: the visitor's own chat. */
static HtmxResponse *
venture_web_webchat_visitor(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	VentureInboxService *inbox = venture_inbox_service_get(db);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	g_autoptr(VentureEntity) session = NULL, widget = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *org = NULL, *problem = NULL, *name = NULL;
	const gchar *token = g_hash_table_lookup(params, "token");
	gint64 last = 0;
	guint i, status = 200;
	if (venture_web_webchat_too_big(request))
		return venture_web_webchat_status(413, "That is too long to send");
	session = venture_web_webchat_session(self, params, &widget);
	if (session == NULL)
		return venture_web_webchat_status(404, "This chat is not available");
	if (htmx_request_get_method(request) == HTMX_METHOD_POST)
	{
		g_autofree gchar *remote = venture_auth_remote_address(request);
		g_autoptr(VentureEntity) message = venture_inbox_chat_post(inbox, session, htmx_request_get_form_value(request, "message"),
			remote, now, &error);
		if (message != NULL)
		{
			g_autofree gchar *location = g_strdup_printf("/webchat/c/%s#latest", token);
			HtmxResponse *response = htmx_response_new();
			htmx_response_set_status(response, 303);
			htmx_response_add_header(response, "Location", location);
			venture_web_webchat_headers(response, widget, FALSE);
			return response;
		}
		status = error->domain == VENTURE_INBOX_ERROR ? 429 : 400;
		problem = g_strdup(error->message);
	}
	org = venture_web_webchat_org_name(db, venture_entity_get_organization_id(session));
	g_object_get(session, "visitor-name", &name, NULL);
	messages = venture_inbox_chat_messages(inbox, session, 0, NULL);
	g_string_append(body, "<h1>Your chat with ");
	venture_html_escape_append(body, org);
	g_string_append(body, "</h1><p class=\"hint\">This page is only yours: anyone with its address can read it. Come back to it for our replies.</p>");
	g_string_append_printf(body, "<ol id=\"thread\" data-poll=\"/webchat/c/%s/messages\">", token);
	for (i = 0; messages != NULL && i < messages->len; i++)
	{
		VentureEntity *message = g_ptr_array_index(messages, i);
		g_autofree gchar *text = NULL, *author = NULL;
		g_autoptr(GDateTime) when = NULL;
		gboolean inbound = FALSE;
		g_object_get(message, "body", &text, "author", &author, "sent-at", &when, "inbound", &inbound, NULL);
		last = venture_entity_get_id(message);
		g_string_append_printf(body, "<li class=\"%s\" data-id=\"%" G_GINT64_FORMAT "\">", inbound ? "in" : "out", last);
		venture_html_escape_append(body, text);
		g_string_append(body, "<span class=\"meta\">");
		venture_html_escape_append(body, inbound ? (name != NULL ? name : "You") : org);
		g_string_append(body, " · ");
		venture_web_inbox_when(self, body, when);
		g_string_append(body, "</span></li>");
	}
	g_string_append(body, "</ol><span id=\"latest\"></span>");
	if (problem != NULL)
	{
		g_string_append(body, "<p class=\"error\" role=\"alert\">");
		venture_html_escape_append(body, problem);
		g_string_append(body, "</p>");
	}
	g_string_append(body, "<form method=\"post\" action=\"/webchat/c/");
	venture_html_escape_append(body, token);
	g_string_append_printf(body, "\"><label for=\"message\">Write again</label><textarea id=\"message\" name=\"message\" rows=\"3\" "
		"maxlength=\"%d\" required></textarea><button type=\"submit\">Send</button></form>"
		"<p class=\"hint\"><a href=\"/webchat/c/", VENTURE_INBOX_CHAT_BODY_MAX);
	venture_html_escape_append(body, token);
	g_string_append(body, "\">Check for replies</a></p>");
	(void)last;
	return venture_web_webchat_html(body, status, widget, TRUE);
}

/* GET /webchat/c/:token/messages?after=N: what the optional script polls. */
static HtmxResponse *
venture_web_webchat_poll(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *db = venture_context_get_database(self->context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
	g_autoptr(VentureEntity) session = NULL, widget = NULL;
	g_autoptr(GPtrArray) messages = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *org = NULL, *name = NULL;
	const gchar *after = htmx_request_get_query_param(request, "after");
	HtmxResponse *response;
	guint i;
	session = venture_web_webchat_session(self, params, &widget);
	if (session == NULL)
		return venture_web_webchat_status(404, "This chat is not available");
	messages = venture_inbox_chat_messages(venture_inbox_service_get(db), session,
		after != NULL ? MAX(g_ascii_strtoll(after, NULL, 10), 0) : 0, NULL);
	org = venture_web_webchat_org_name(db, venture_entity_get_organization_id(session));
	g_object_get(session, "visitor-name", &name, NULL);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "messages");
	json_builder_begin_array(builder);
	for (i = 0; messages != NULL && i < messages->len; i++)
	{
		VentureEntity *message = g_ptr_array_index(messages, i);
		g_autofree gchar *text = NULL, *when = NULL;
		g_autoptr(GDateTime) sent = NULL;
		gboolean inbound = FALSE;
		g_object_get(message, "body", &text, "sent-at", &sent, "inbound", &inbound, NULL);
		when = sent != NULL ? venture_time_to_string(sent) : NULL;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, venture_entity_get_id(message));
		json_builder_set_member_name(builder, "from");
		json_builder_add_string_value(builder, inbound ? (name != NULL ? name : "You") : org);
		json_builder_set_member_name(builder, "inbound");
		json_builder_add_boolean_value(builder, inbound);
		json_builder_set_member_name(builder, "body");
		json_builder_add_string_value(builder, text);
		json_builder_set_member_name(builder, "at");
		json_builder_add_string_value(builder, when != NULL ? when : "");
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	result = json_builder_get_root(builder);
	response = venture_web_json_response(result, 200);
	venture_web_webchat_headers(response, widget, FALSE);
	return response;
}

/* GET /webchat.js: the optional poll. The page works without it. */
static HtmxResponse *
venture_web_webchat_script(HtmxRequest *request, GHashTable *params, gpointer data)
{
	static const gchar script[] =
		"(function(){var list=document.getElementById('thread');if(!list||!window.fetch)return;"
		"var url=list.getAttribute('data-poll');function last(){var items=list.querySelectorAll('li[data-id]');"
		"return items.length?items[items.length-1].getAttribute('data-id'):'0';}"
		"function add(m){var li=document.createElement('li');li.className=m.inbound?'in':'out';li.setAttribute('data-id',m.id);"
		"li.appendChild(document.createTextNode(m.body));var meta=document.createElement('span');meta.className='meta';"
		"meta.textContent=m.from+(m.at?' \\u00b7 '+m.at.replace('T',' ').slice(0,16):'');li.appendChild(meta);list.appendChild(li);}"
		"function poll(){fetch(url+'?after='+encodeURIComponent(last()),{credentials:'omit',cache:'no-store'})"
		".then(function(r){return r.ok?r.json():{messages:[]};}).then(function(j){(j.messages||[]).forEach(add);})"
		".catch(function(){});}setInterval(poll,10000);})();";
	HtmxResponse *response = htmx_response_new_with_content(script);
	(void)request; (void)params; (void)data;
	htmx_response_set_content_type(response, "text/javascript; charset=utf-8");
	htmx_response_add_header(response, "Cache-Control", "public, max-age=3600");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	return response;
}

static void
venture_inbox_pages_register(VentureWebServer *self)
{
	static const struct {
		HtmxMethod method;
		const gchar *path;
		HtmxRouteCallback handler;
	} routes[] = {
		{ HTMX_METHOD_GET, "/conversations", venture_web_inbox_list_page },
		{ HTMX_METHOD_GET, "/conversations/:id", venture_web_inbox_page },
		{ HTMX_METHOD_POST, "/conversations/:id/assign", venture_web_inbox_change },
		{ HTMX_METHOD_POST, "/conversations/:id/status", venture_web_inbox_change },
		{ HTMX_METHOD_POST, "/conversations/:id/reply", venture_web_inbox_change },
		{ HTMX_METHOD_GET, "/api/v1/conversations", venture_web_inbox_api_list },
		{ HTMX_METHOD_GET, "/api/v1/conversations/:id", venture_web_inbox_api_get },
		{ HTMX_METHOD_POST, "/api/v1/conversations/:id/assign", venture_web_inbox_change },
		{ HTMX_METHOD_POST, "/api/v1/conversations/:id/status", venture_web_inbox_change },
		{ HTMX_METHOD_POST, "/api/v1/conversations/:id/reply", venture_web_inbox_change },
		{ HTMX_METHOD_GET, "/contacts/:id/conversation", venture_web_inbox_contact },
		{ HTMX_METHOD_GET, "/api/v1/contacts/:id/conversation", venture_web_inbox_contact },
		{ HTMX_METHOD_GET, "/organizations/:id/settings/webchat", venture_web_inbox_chat_settings },
		{ HTMX_METHOD_POST, "/organizations/:id/settings/webchat", venture_web_inbox_chat_settings },
		{ HTMX_METHOD_GET, "/webchat.js", venture_web_webchat_script },
		{ HTMX_METHOD_GET, "/webchat/:token", venture_web_webchat_intake },
		{ HTMX_METHOD_POST, "/webchat/:token", venture_web_webchat_intake },
		{ HTMX_METHOD_GET, "/webchat/c/:token", venture_web_webchat_visitor },
		{ HTMX_METHOD_POST, "/webchat/c/:token", venture_web_webchat_visitor },
		{ HTMX_METHOD_GET, "/webchat/c/:token/messages", venture_web_webchat_poll },
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
			VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, routes[i].handler, self);
}
