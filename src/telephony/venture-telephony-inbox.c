/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-telephony-inbox.c - Texts in the one inbox (docs/inbox.org)
 *
 * A contact's text messages, by reference, and replies through the one
 * text send path, which checks consent and quiet hours itself.
 */
#include <venture.h>

static gchar *
text_string(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static GDateTime *
text_when(VentureEntity *row, gboolean inbound)
{
	GDateTime *when = NULL;
	g_object_get(row, inbound ? "received-at" : "sent-at", &when, NULL);
	if (when == NULL && venture_entity_get_created_at(row) != NULL)
		when = g_date_time_ref(venture_entity_get_created_at(row));
	return when;
}

static gboolean
text_list(VentureContext *context, VentureEntity *contact, GDateTime *since, guint limit, GPtrArray *items, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	const VentureInboxSource *self = venture_inbox_source_lookup("text");
	guint i;
	(void)since;
	rows = venture_telephony_service_thread(venture_telephony_service_get(venture_context_get_database(context)),
		contact, limit, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		gint direction = 0;
		g_autoptr(GDateTime) when = NULL;
		g_object_get(row, "direction", &direction, NULL);
		when = text_when(row, direction == VENTURE_TEXT_DIRECTION_INBOUND);
		if (when == NULL)
			continue;
		g_ptr_array_add(items, venture_inbox_item_new(self, row, when, direction == VENTURE_TEXT_DIRECTION_INBOUND));
	}
	return TRUE;
}

static gboolean
text_observe(VentureEntity *record, gint64 *contact_id, GDateTime **at, gboolean *inbound)
{
	gint direction = 0;
	if (!VENTURE_IS_TEXT_MESSAGE(record))
		return FALSE;
	g_object_get(record, "contact-id", contact_id, "direction", &direction, NULL);
	*inbound = direction == VENTURE_TEXT_DIRECTION_INBOUND;
	*at = text_when(record, *inbound);
	return TRUE;
}

static void
text_render(VentureContext *context, const VentureInboxItem *item, GString *html)
{
	g_autofree gchar *body = text_string(item->record, "body"), *state = text_string(item->record, "state");
	g_autofree gchar *why = text_string(item->record, "error"), *documents = text_string(item->record, "media-documents");
	(void)context;
	g_string_append(html, "<div class=\"inbox-text\">");
	venture_html_escape_append(html, body);
	g_string_append(html, "</div>");
	if (!venture_string_is_empty(documents))
		g_string_append(html, "<span class=\"inbox-note\">A picture came with it, kept privately</span>");
	if (!item->inbound)
	{
		g_string_append(html, "<span class=\"inbox-note\">");
		venture_html_escape_append(html, state);
		if (!venture_string_is_empty(why) && g_strcmp0(state, "sent") != 0 && g_strcmp0(state, "delivered") != 0)
		{
			g_string_append(html, " · ");
			venture_html_escape_append(html, why);
		}
		g_string_append(html, "</span>");
	}
}

static gchar *
text_summary(VentureContext *context, const VentureInboxItem *item)
{
	(void)context;
	return text_string(item->record, "body");
}

static gchar *
text_reply(VentureContext *context, VentureEntity *contact, const VentureInboxItem *answering, const gchar *body,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	VentureTelephonyService *service = venture_telephony_service_get(venture_context_get_database(context));
	g_autoptr(VentureEntity) message = NULL;
	g_autoptr(GDateTime) until = NULL;
	g_autofree gchar *state = NULL, *why = NULL;
	(void)answering;
	message = venture_telephony_service_send_text(service, contact, body, 0, now, actor, error);
	if (message == NULL)
		return NULL;
	g_object_get(message, "state", &state, "error", &why, "hold-until", &until, NULL);
	if (g_strcmp0(state, "held") == 0)
	{
		g_autoptr(GDateTime) local = until != NULL ? g_date_time_to_timezone(until, venture_context_get_timezone(context)) : NULL;
		g_autofree gchar *when = local != NULL ? g_date_time_format(local, "%Y-%m-%d %H:%M") : g_strdup("the morning");
		return g_strdup_printf("Held for their quiet hours; it is sent at %s", when);
	}
	if (g_strcmp0(state, "failed") == 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The text was not sent: %s",
			!venture_string_is_empty(why) ? why : "the provider refused it");
		return NULL;
	}
	return g_strdup("Text sent");
}

static const VentureInboxSource telephony_inbox_source = {
	"text", "Text", "telephony", text_list, text_observe, text_render, text_summary, text_reply, NULL
};
VENTURE_INBOX_SOURCE(telephony_inbox_source)
