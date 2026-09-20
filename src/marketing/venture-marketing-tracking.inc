/* SPDX-License-Identifier: AGPL-3.0-or-later */
static VentureEntity *observable_recipient(VentureMarketingService *self, const gchar *token, GError **error)
{
	g_autoptr(VentureEntity) recipient = NULL, organization = NULL, mail = NULL;
	g_autoptr(GDateTime) withdrawn = NULL;
	g_autofree gchar *email = NULL, *state = NULL;
	gboolean blocked;
	gint64 org;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send")) {
		refuse(error, "Marketing tracking is disabled"); return NULL;
	}
	recipient = token_recipient(self, token, "tracking-hash", error);
	if (!recipient) return NULL;
	org = venture_entity_get_organization_id(recipient);
	organization = get_row(self, VENTURE_TYPE_ORGANIZATION, org, org, error);
	if (!organization) return NULL;
	g_object_get(recipient, "unsubscribed-at", &withdrawn, NULL);
	email = string(recipient, "email");
	if (!suppressed(self, org, email, &blocked, error)) return NULL;
	if (withdrawn || blocked || !flag(organization, "marketing-tracking") || !number(recipient, "mail-id")) {
		refuse(error, "Marketing observation is not permitted"); return NULL;
	}
	mail = get_row(self, VENTURE_TYPE_MAIL_MESSAGE, org, number(recipient, "mail-id"), error);
	if (!mail) return NULL;
	state = string(mail, "state");
	if (g_strcmp0(state, "sent") && g_strcmp0(state, "uncertain") && g_strcmp0(state, "sending")) {
		refuse(error, "Message has not entered submission"); return NULL;
	}
	return g_steal_pointer(&recipient);
}
static gboolean observe(VentureMarketingService *self, VentureEntity *recipient, const gchar *kind,
	gint64 position, GDateTime *now, GError **error)
{
	g_autoptr(GDateTime) utc = g_date_time_to_utc(now);
	g_autofree gchar *day = g_date_time_format(utc, "%Y-%m-%d"), *key = NULL;
	g_autoptr(VentureEntity) existing = NULL, event = NULL;
	g_autoptr(GDateTime) first = NULL;
	gint64 org = venture_entity_get_organization_id(recipient);
	const gchar *field = position ? "clicked-at" : "opened-at";
	key = g_strdup_printf("observation:%s:%s:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(recipient), day, kind, position);
	existing = find_string(self, VENTURE_TYPE_MARKETING_EVENT, org, "event-key", key, error);
	if (error && *error) return FALSE;
	if (existing) return TRUE;
	event = g_object_new(VENTURE_TYPE_MARKETING_EVENT, "organization-id", org,
		"recipient-id", venture_entity_get_id(recipient), "kind", kind, "link-position", position,
		"occurred-at", now, "source", "Mail tracking request; human readership is unknown", "event-key", key, NULL);
	if (!save(self, event, NULL, error)) return FALSE;
	g_object_get(recipient, field, &first, NULL);
	if (!first) { g_object_set(recipient, field, now, NULL); if (!save(self, recipient, NULL, error)) return FALSE; }
	return TRUE;
}
gboolean venture_marketing_service_record_open(VentureMarketingService *self, const gchar *token, GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) recipient = NULL;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (!self->database) return refuse(error, "Marketing database is unavailable");
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!valid_time(now, error) || !venture_database_begin(self->database, error)) return FALSE;
	recipient = observable_recipient(self, token, error);
	if (!recipient || !observe(self, recipient, "open", 0, now, error)) goto fail;
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}
gchar *venture_marketing_service_record_click(VentureMarketingService *self, const gchar *token, gint64 position,
	GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(VentureEntity) recipient = NULL;
	g_autofree gchar *links = NULL, *url = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	JsonArray *array;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	if (!self->database) { refuse(error, "Marketing database is unavailable"); return NULL; }
	capability = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	if (!valid_time(now, error) || !venture_database_begin(self->database, error)) return NULL;
	recipient = observable_recipient(self, token, error);
	if (!recipient) goto fail;
	links = string(recipient, "links"); parsed = json_from_string(links ? links : "[]", error);
	if (!parsed) goto fail;
	if (!JSON_NODE_HOLDS_ARRAY(parsed)) { refuse(error, "Invalid retained tracking destinations"); goto fail; }
	array = json_node_get_array(parsed);
	if (position < 1 || (guint64)position > json_array_get_length(array)) { refuse(error, "Unknown tracked destination"); goto fail; }
	url = g_strdup(json_array_get_string_element(array, position - 1));
	if (!url || (!g_str_has_prefix(url, "https://") && !g_str_has_prefix(url, "http://"))) { refuse(error, "Invalid tracked web destination"); goto fail; }
	if (!observe(self, recipient, "click", position, now, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&url);
fail:
	venture_database_rollback(self->database); return NULL;
}
gboolean venture_marketing_service_feedback(VentureMarketingService *self, gint64 org, gint64 recipient_id,
	const gchar *kind, const gchar *source, const gchar *event_key, GDateTime *occurred_at,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) recipient = NULL, mail = NULL, existing = NULL, event = NULL, send = NULL;
	g_autoptr(GDateTime) approved = NULL;
	g_autofree gchar *key = NULL, *email = NULL;
	gboolean hard = !g_strcmp0(kind, "hard_bounce"), complaint = !g_strcmp0(kind, "complaint");
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (!self->database) return refuse(error, "Marketing database is unavailable");
	if (!allowed(self, org, error) || !valid_time(occurred_at, error)) return FALSE;
	if ((!hard && !complaint && g_strcmp0(kind, "temporary_bounce")) || !bounded(source, 4096) || !bounded(event_key, 128))
		return refuse(error, "Feedback needs an explicit kind, evidence source and replay identity");
	if (!venture_database_begin(self->database, error)) return FALSE;
	recipient = get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, recipient_id, error);
	if (!recipient) goto fail;
	mail = get_row(self, VENTURE_TYPE_MAIL_MESSAGE, org, number(recipient, "mail-id"), error);
	if (!mail) goto fail;
	send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, number(recipient, "send-id"), error);
	if (!send) goto fail;
	g_object_get(send, "approved-at", &approved, NULL);
	if (number(mail, "attempts") <= 0 || !approved || g_date_time_compare(occurred_at, approved) < 0) {
		refuse(error, "Feedback requires an actual submission attempt and subsequent evidence"); goto fail;
	}
	key = g_strconcat("feedback:", event_key, NULL);
	existing = find_string(self, VENTURE_TYPE_MARKETING_EVENT, org, "event-key", key, error);
	if (error && *error) goto fail;
	if (existing) {
		g_autofree gchar *old_kind = string(existing, "kind"), *old_source = string(existing, "source");
		g_autoptr(GDateTime) old_at = NULL;
		g_object_get(existing, "occurred-at", &old_at, NULL);
		if (number(existing, "recipient-id") != recipient_id || g_strcmp0(old_kind, kind) || g_strcmp0(old_source, source) || !old_at || g_date_time_compare(old_at, occurred_at)) {
			refuse(error, "Feedback identity already belongs to different evidence"); goto fail;
		}
		return venture_database_commit(self->database, error);
	}
	event = g_object_new(VENTURE_TYPE_MARKETING_EVENT, "organization-id", org,
		"recipient-id", recipient_id, "kind", kind, "source", source, "occurred-at", occurred_at, "event-key", key, NULL);
	if (!save(self, event, actor, error)) goto fail;
	if (hard || complaint) {
		email = string(recipient, "email");
		if (!withdraw(self, org, email, hard ? 1 : 2, source, occurred_at, actor, error)) goto fail;
		if (hard) {
			g_autoptr(GDateTime) first = NULL;
			g_clear_object(&recipient); recipient = get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, recipient_id, error);
			if (!recipient) goto fail;
			g_object_get(recipient, "bounced-at", &first, NULL);
			if (!first) { g_object_set(recipient, "bounced-at", occurred_at, NULL); if (!save(self, recipient, actor, error)) goto fail; }
		}
	}
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}
