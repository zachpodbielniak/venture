/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-activity-inbox.c - Calls in the one inbox (docs/inbox.org)
 *
 * Every call, missed or answered, is already a call activity written by
 * log_call. A voicemail is a call whose outcome is voicemail; its
 * recording link and transcript are shown only where the activity holds
 * them, which is only where the organization allowed it. A call is not
 * answered by writing, so the action is calling back.
 */
#include <venture.h>

static gchar *
call_string(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gboolean
call_list(VentureContext *context, VentureEntity *contact, GDateTime *since, guint limit, GPtrArray *items, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autoptr(GPtrArray) rows = NULL;
	const VentureInboxSource *self = venture_inbox_source_lookup("call");
	guint i;
	(void)since;
	venture_query_set_organization(query, venture_entity_get_organization_id(contact));
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
	venture_query_add_filter_int(query, "kind", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_KIND_CALL, NULL);
	venture_query_add_order(query, "call-occurred-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, limit);
	rows = venture_database_find(venture_context_get_database(context), query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) when = NULL;
		gint direction = 0;
		g_object_get(row, "call-occurred-at", &when, "call-direction", &direction, NULL);
		if (when == NULL)
			continue;
		g_ptr_array_add(items, venture_inbox_item_new(self, row, when, direction == VENTURE_CALL_DIRECTION_INBOUND));
	}
	return TRUE;
}

static gboolean
call_observe(VentureEntity *record, gint64 *contact_id, GDateTime **at, gboolean *inbound)
{
	gint kind = 0, direction = 0;
	if (!VENTURE_IS_ACTIVITY(record))
		return FALSE;
	g_object_get(record, "kind", &kind, "call-occurred-at", at, "call-direction", &direction, "contact-id", contact_id, NULL);
	if (kind != VENTURE_ACTIVITY_KIND_CALL || *at == NULL)
	{
		g_clear_pointer(at, g_date_time_unref);
		return FALSE;
	}
	*inbound = direction == VENTURE_CALL_DIRECTION_INBOUND;
	return TRUE;
}

static gchar *
call_label(VentureEntity *row)
{
	gint outcome = 0, direction = 0;
	gint64 seconds = 0;
	g_object_get(row, "call-outcome", &outcome, "call-direction", &direction, "call-duration", &seconds, NULL);
	if (outcome == VENTURE_CALL_OUTCOME_MISSED)
		return g_strdup("Missed call");
	if (outcome == VENTURE_CALL_OUTCOME_VOICEMAIL)
		return g_strdup(direction == VENTURE_CALL_DIRECTION_INBOUND ? "Voicemail" : "Left a voicemail");
	if (outcome == VENTURE_CALL_OUTCOME_NO_ANSWER)
		return g_strdup("Call, no answer");
	if (seconds >= 60)
		return g_strdup_printf("%s call, %" G_GINT64_FORMAT " min", direction == VENTURE_CALL_DIRECTION_INBOUND ? "Inbound" : "Outbound", seconds / 60);
	return g_strdup_printf("%s call", direction == VENTURE_CALL_DIRECTION_INBOUND ? "Inbound" : "Outbound");
}

static void
call_render(VentureContext *context, const VentureInboxItem *item, GString *html)
{
	g_autofree gchar *label = call_label(item->record), *recording = call_string(item->record, "call-recording-url");
	g_autofree gchar *transcript = call_string(item->record, "call-transcript"), *outcome = call_string(item->record, "outcome");
	(void)context;
	g_string_append(html, "<strong class=\"inbox-subject\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</strong>");
	if (!venture_string_is_empty(outcome))
	{
		g_string_append(html, "<div class=\"inbox-text\">");
		venture_html_escape_append(html, outcome);
		g_string_append(html, "</div>");
	}
	if (!venture_string_is_empty(transcript))
	{
		g_string_append(html, "<div class=\"inbox-text inbox-transcript\">");
		venture_html_escape_append(html, transcript);
		g_string_append(html, "</div>");
	}
	/* The activity only holds a stable https link the organization chose
	 * to keep; it is never fetched here. */
	if (!venture_string_is_empty(recording) && g_str_has_prefix(recording, "https://"))
	{
		g_string_append(html, "<a class=\"inbox-open\" rel=\"noreferrer noopener\" target=\"_blank\" href=\"");
		venture_html_escape_append(html, recording);
		g_string_append(html, "\">Recording</a> ");
	}
	g_string_append_printf(html, "<a class=\"inbox-open\" href=\"/e/activity/%" G_GINT64_FORMAT "\">Open</a>",
		venture_entity_get_id(item->record));
}

static gchar *
call_summary(VentureContext *context, const VentureInboxItem *item)
{
	(void)context;
	return call_label(item->record);
}

/* Calling back: through the phone system when telephony is on (the
 * route rings the person's own extension first), and always the
 * number itself for a phone in hand. */
static void
call_action(VentureContext *context, VentureEntity *contact, GString *html)
{
	g_autofree gchar *phone = call_string(contact, "phone");
	if (venture_string_is_empty(phone))
	{
		g_string_append(html, "<p class=\"muted\">They called, but there is no number on the contact to call back.</p>");
		return;
	}
	g_string_append(html, "<div class=\"inbox-call\">");
	if (venture_context_module_enabled(context, "telephony"))
		g_string_append_printf(html, "<form method=\"post\" action=\"/telephony/call\"><input type=\"hidden\" name=\"contact_id\" "
			"value=\"%" G_GINT64_FORMAT "\"><button class=\"btn btn-primary\" type=\"submit\">Call back</button></form>",
			venture_entity_get_id(contact));
	g_string_append(html, "<a class=\"btn\" href=\"tel:");
	venture_html_escape_append(html, phone);
	g_string_append(html, "\">");
	venture_html_escape_append(html, phone);
	g_string_append(html, "</a></div>");
}

static const VentureInboxSource activity_inbox_source = {
	"call", "Phone call", "activities", call_list, call_observe, call_render, call_summary, NULL, call_action
};
VENTURE_INBOX_SOURCE(activity_inbox_source)
