/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Campaign replies, counted by the sequences' own rule
 * (venture_sequence_interaction_is_reply()): an inbound interaction for a
 * contact or lead. Mail sync files a reply as exactly that, matched to the
 * contact by address, so there is no second matcher here. It counts for
 * the most recent campaign message the relay accepted for that person
 * before the reply and within VENTURE_MARKETING_REPLY_DAYS, once. Like the
 * sequence rule, any inbound interaction counts, a logged call back too. */
#define VENTURE_MARKETING_REPLY_DAYS 30

static gboolean validate_reply(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureMarketingService *self = data;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) occurred = NULL, earliest = NULL, best_at = NULL;
	g_autoptr(VentureAccessScope) capability = NULL;
	VentureEntity *best = NULL;
	gint64 org = venture_entity_get_organization_id(row), contact_id = 0, lead_id = 0;
	guint i;
	(void)database;
	if (!venture_sequence_interaction_is_reply(row, previous)) return TRUE;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send")) return TRUE;
	g_object_get(row, "contact-id", &contact_id, "lead-id", &lead_id, "occurred-at", &occurred, NULL);
	if (org <= 0 || (!contact_id && !lead_id)) return TRUE;
	if (!occurred) occurred = venture_time_now();
	earliest = g_date_time_add_days(occurred, -VENTURE_MARKETING_REPLY_DAYS);
	query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 20);
	venture_query_add_filter_int(query, contact_id ? "contact-id" : "lead-id", VENTURE_FILTER_OP_EQ, contact_id ? contact_id : lead_id, NULL);
	venture_query_add_filter_int(query, "mail-id", VENTURE_FILTER_OP_GT, 0, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *recipient = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) mail = get_row(self, VENTURE_TYPE_MAIL_MESSAGE, org, number(recipient, "mail-id"), NULL);
		g_autoptr(GDateTime) sent = NULL;
		g_autofree gchar *state = mail ? string(mail, "state") : NULL;
		if (!mail || g_strcmp0(state, "sent")) continue;
		g_object_get(mail, "sent-at", &sent, NULL);
		if (!sent || g_date_time_compare(sent, occurred) > 0 || g_date_time_compare(sent, earliest) < 0) continue;
		if (!best_at || g_date_time_compare(sent, best_at) > 0) {
			best = recipient;
			g_clear_pointer(&best_at, g_date_time_unref);
			best_at = g_date_time_ref(sent);
		}
	}
	if (!best || has_stamp(best, "replied-at")) return TRUE;
	/* The reply is the recipient's evidence, not the writer's: whoever
	 * logged the interaction need not be allowed to edit a campaign. */
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	{
		g_autofree gchar *key = g_strdup_printf("reply:%s", venture_entity_get_uuid(best));
		g_autoptr(VentureEntity) event = NULL, existing = find_string(self, VENTURE_TYPE_MARKETING_EVENT, org, "event-key", key, error);
		if (error && *error) return FALSE;
		if (!existing) {
			event = g_object_new(VENTURE_TYPE_MARKETING_EVENT, "organization-id", org,
				"recipient-id", venture_entity_get_id(best), "kind", "reply", "occurred-at", occurred,
				"source", "Inbound interaction after this campaign message, counted by the sequence reply rule",
				"event-key", key, NULL);
			if (!save(self, event, NULL, error)) return FALSE;
		}
	}
	g_object_set(best, "replied-at", occurred, NULL);
	return save(self, best, NULL, error);
}
