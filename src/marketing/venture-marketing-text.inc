/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Text consent and text campaigns (venture-marketing-text.h). Included at
 * the end of the marketing service, whose helpers and write permit it
 * shares: text consent is the same evidence record as email consent. */

static const VentureMarketingTextChannel *text_channel;
static gpointer text_channel_data;

void venture_marketing_text_channel_register(const VentureMarketingTextChannel *channel, gpointer data)
{
	text_channel = channel;
	text_channel_data = data;
}

static gboolean text_send(VentureEntity *send)
{
	return choice(send, "channel") == 1;
}

/* The latest text consent row for a number: an agreement, or a withdrawal. */
static VentureEntity *latest_text_consent(VentureMarketingService *self, gint64 org, const gchar *phone, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_CONSENT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "phone", VENTURE_FILTER_OP_EQ, phone, NULL);
	venture_query_add_filter_string(query, "purpose", VENTURE_FILTER_OP_EQ, "text", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	return venture_database_find_one(self->database, query, error);
}

VentureEntity *venture_marketing_service_text_consent(VentureMarketingService *self, gint64 organization_id,
	VentureEntity *contact, const gchar *phone, gboolean granted, const gchar *source,
	const gchar *evidence, const gchar *evidence_key, GDateTime *at, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) existing = NULL, latest = NULL, row = NULL;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	if (!self->database) { refuse(error, "Marketing database is unavailable"); return NULL; }
	if (!VENTURE_IS_CONTACT(contact) || venture_entity_get_organization_id(contact) != organization_id ||
		!phone || phone[0] != '+' || !bounded(source, 2048) || !bounded(evidence, 8192) || !bounded(evidence_key, 128) ||
		!at || g_date_time_get_year(at) < 1970)
	{
		refuse(error, "Text consent needs a contact, their number and bounded source, evidence and identity");
		return NULL;
	}
	if (!venture_database_begin(self->database, error)) return NULL;
	existing = find_string(self, VENTURE_TYPE_MARKETING_CONSENT, organization_id, "evidence-key", evidence_key, error);
	if (error && *error) goto fail;
	if (existing) {
		/* The same evidence again (a webhook replayed) is the first row. */
		if (!venture_database_commit(self->database, error)) return NULL;
		return g_steal_pointer(&existing);
	}
	latest = latest_text_consent(self, organization_id, phone, error);
	if (error && *error) goto fail;
	row = g_object_new(VENTURE_TYPE_MARKETING_CONSENT, "organization-id", organization_id,
		"contact-id", venture_entity_get_id(contact), "phone", phone, "email", "", "purpose", "text",
		"source", source, "evidence", evidence, "evidence-key", evidence_key, "recorded-at", at, NULL);
	if (!granted) {
		g_object_set(row, "withdrawn-at", at, NULL);
		if (latest && !has_stamp(latest, "withdrawn-at"))
			g_object_set(row, "withdraws-id", venture_entity_get_id(latest), "withdraws-uuid", venture_entity_get_uuid(latest), NULL);
	}
	if (!save(self, row, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&row);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

gboolean venture_marketing_service_text_permitted(VentureMarketingService *self, gint64 organization_id,
	const gchar *phone, gint64 *consent_id, GError **error)
{
	g_autoptr(VentureEntity) latest = NULL;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (consent_id) *consent_id = 0;
	if (!self->database || !phone || !*phone) return FALSE;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_consent")) return FALSE;
	latest = latest_text_consent(self, organization_id, phone, error);
	if (!latest || has_stamp(latest, "withdrawn-at")) return FALSE;
	if (consent_id) *consent_id = venture_entity_get_id(latest);
	return TRUE;
}

/* One frozen text recipient of a text campaign: the number the telephony
 * channel gives, the rendered text with the opt-out line. Excluded with a
 * reason when there is no number, no consent or no line. */
static gboolean make_text_recipient(VentureMarketingService *self, VentureEntity *send, VentureEntity *source,
	const gchar *source_key, GHashTable *seen, gint64 *eligible, gint64 *excluded, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMarketingRecipient) recipient = venture_marketing_recipient_new();
	g_autofree gchar *key = g_strdup_printf("%s:%s", venture_entity_get_uuid(send), source_key), *address = NULL;
	gint64 org = venture_entity_get_organization_id(send);
	const gchar *reason = "eligible";
	if (!source) reason = "source_missing";
	else if (!VENTURE_IS_CONTACT(source)) reason = "texts_need_a_contact";
	else if (!text_channel) reason = "texting_off";
	else {
		const gchar *why = NULL;
		if (!text_channel->recipient(self->database, source, &address, &why, text_channel_data, error)) {
			if (error && *error) return FALSE;
			reason = why ? why : "no_text_permission";
		} else if (g_hash_table_contains(seen, address)) reason = "duplicate_address";
	}
	g_object_set(recipient, "organization-id", org, "send-id", venture_entity_get_id(send), "recipient-key", key,
		"email", address ? address : "", "eligibility", reason, "state", !strcmp(reason, "eligible") ? "eligible" : "excluded", NULL);
	if (source) g_object_set(recipient, subject_field(G_OBJECT_TYPE(source)), venture_entity_get_id(source), NULL);
	if (strcmp(reason, "eligible")) { (*excluded)++; return save(self, VENTURE_ENTITY(recipient), actor, error); }
	{
		g_autofree gchar *subject_text = string(send, "subject"), *text = string(send, "text-body"), *body = NULL;
		g_autoptr(VentureMailTemplate) template = g_object_new(VENTURE_TYPE_MAIL_TEMPLATE, "organization-id", org,
			"subject", subject_text, "text-body", text, NULL);
		g_autoptr(VentureMailMessage) rendered = venture_mail_template_render(template, source, error);
		g_autofree gchar *words = NULL, *title = NULL;
		if (!rendered) return FALSE;
		words = string(VENTURE_ENTITY(rendered), "text-body");
		title = string(VENTURE_ENTITY(rendered), "subject");
		/* Every campaign text says how to stop them. */
		body = strstr(words ? words : "", "STOP") ? g_strdup(words) : g_strdup_printf("%s\nReply STOP to opt out.", words ? words : "");
		if (g_utf8_strlen(body, -1) > 1600) return refuse(error, "A text campaign's message is at most 1600 characters");
		g_object_set(recipient, "subject", title, "text-body", body, NULL);
	}
	if (!save(self, VENTURE_ENTITY(recipient), actor, error)) return FALSE;
	g_hash_table_add(seen, g_strdup(address)); (*eligible)++;
	return TRUE;
}

/* Queues one approved text recipient through the channel. A refusal (the
 * person said STOP after approval) cancels the recipient; only a storage
 * failure fails the run. */
static gboolean text_queue_recipient(VentureMarketingService *self, VentureEntity *send, VentureEntity *recipient,
	GDateTime *now, gboolean *queued, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) missing = NULL;
	g_autofree gchar *body = string(recipient, "text-body");
	(void)send;
	*queued = FALSE;
	source = subject(self, recipient, &missing);
	if (source && text_channel)
		*queued = text_channel->queue(self->database, recipient, source, body, now, actor, text_channel_data, error);
	if (error && *error) return FALSE;
	g_object_set(recipient, "state", *queued ? "queued" : "cancelled", NULL);
	return save(self, recipient, actor, error);
}
