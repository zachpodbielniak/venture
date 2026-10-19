/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-mail-inbox.c - Email in the one inbox (docs/inbox.org)
 *
 * Mail is already on the contact as email interactions: the sync files
 * what came in, the outbox records what went out once the relay has it.
 * This lists those by reference and answers through the timeline reply,
 * which threads to the message and goes through the one outbox.
 */
#include <venture.h>

#define INBOX_EMAIL_SHOWN 4000

static gchar *
mail_string(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static GDateTime *
mail_when(VentureEntity *row)
{
	GDateTime *when = NULL;
	g_object_get(row, "occurred-at", &when, NULL);
	if (when == NULL && venture_entity_get_created_at(row) != NULL)
		when = g_date_time_ref(venture_entity_get_created_at(row));
	return when;
}

static gboolean
mail_list(VentureContext *context, VentureEntity *contact, GDateTime *since, guint limit, GPtrArray *items, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTERACTION);
	g_autoptr(GPtrArray) rows = NULL;
	const VentureInboxSource *self = venture_inbox_source_lookup("email");
	guint i;
	(void)since;
	venture_query_set_organization(query, venture_entity_get_organization_id(contact));
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
	venture_query_add_filter_int(query, "kind", VENTURE_FILTER_OP_EQ, VENTURE_INTERACTION_KIND_EMAIL, NULL);
	venture_query_add_order(query, "occurred-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, limit);
	rows = venture_database_find(venture_context_get_database(context), query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) when = mail_when(row);
		gboolean outbound = FALSE;
		g_object_get(row, "outbound", &outbound, NULL);
		g_ptr_array_add(items, venture_inbox_item_new(self, row, when, !outbound));
	}
	return TRUE;
}

static gboolean
mail_observe(VentureEntity *record, gint64 *contact_id, GDateTime **at, gboolean *inbound)
{
	gint kind = 0;
	gboolean outbound = FALSE;
	if (!VENTURE_IS_INTERACTION(record))
		return FALSE;
	g_object_get(record, "kind", &kind, "outbound", &outbound, "contact-id", contact_id, NULL);
	if (kind != VENTURE_INTERACTION_KIND_EMAIL)
		return FALSE;
	*at = mail_when(record);
	*inbound = !outbound;
	return TRUE;
}

static void
mail_render(VentureContext *context, const VentureInboxItem *item, GString *html)
{
	g_autofree gchar *subject = mail_string(item->record, "subject"), *body = mail_string(item->record, "body");
	(void)context;
	g_string_append(html, "<strong class=\"inbox-subject\">");
	venture_html_escape_append(html, !venture_string_is_empty(subject) ? subject : "(no subject)");
	g_string_append(html, "</strong>");
	if (!venture_string_is_empty(body))
	{
		gboolean cut = g_utf8_strlen(body, -1) > INBOX_EMAIL_SHOWN;
		if (cut)
			*g_utf8_offset_to_pointer(body, INBOX_EMAIL_SHOWN) = '\0';
		g_string_append(html, "<div class=\"inbox-text\">");
		venture_html_escape_append(html, body);
		if (cut)
			g_string_append(html, " …");
		g_string_append(html, "</div>");
	}
	g_string_append_printf(html, "<a class=\"inbox-open\" href=\"/e/interaction/%" G_GINT64_FORMAT "\">Open</a>",
		venture_entity_get_id(item->record));
}

static gchar *
mail_summary(VentureContext *context, const VentureInboxItem *item)
{
	g_autofree gchar *subject = mail_string(item->record, "subject");
	(void)context;
	return g_strdup(!venture_string_is_empty(subject) ? subject : "(no subject)");
}

static gchar *
mail_reply(VentureContext *context, VentureEntity *contact, const VentureInboxItem *answering, const gchar *body,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMailMessage) message = NULL;
	(void)contact; (void)now;
	message = venture_mail_reply_enqueue(context, answering->record, body, actor, error);
	if (message == NULL)
		return NULL;
	return g_strdup("Email queued as a reply to their message; it shows here once the mail relay accepts it");
}

static const VentureInboxSource mail_inbox_source = {
	"email", "Email", NULL, mail_list, mail_observe, mail_render, mail_summary, mail_reply, NULL
};
VENTURE_INBOX_SOURCE(mail_inbox_source)
