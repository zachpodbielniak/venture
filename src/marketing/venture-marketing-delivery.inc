/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gboolean recipient_allowed(VentureMarketingService *self, VentureEntity *recipient, const gchar **reason, GError **error)
{
	g_autoptr(VentureEntity) source = NULL, consent = NULL, organization = NULL;
	g_autofree gchar *email = string(recipient, "email"), *current = NULL, *raw = NULL, *tracking = NULL;
	g_autoptr(GError) missing = NULL;
	gboolean blocked;
	gint64 org = venture_entity_get_organization_id(recipient);
	*reason = "Permission is no longer valid";
	if (!suppressed(self, org, email, &blocked, error)) return FALSE;
	if (blocked) { *reason = "Address is suppressed for marketing"; return FALSE; }
	source = subject(self, recipient, &missing);
	if (!source) {
		if (missing && !g_error_matches(missing, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) g_propagate_error(error, g_steal_pointer(&missing));
		return FALSE;
	}
	raw = string(source, "email"); current = normalize(raw);
	if (g_strcmp0(email, current)) { *reason = "Recipient address changed after approval"; return FALSE; }
	consent = get_row(self, VENTURE_TYPE_MARKETING_CONSENT, org, number(recipient, "consent-id"), error);
	if (!consent) return FALSE;
	{
		g_autofree gchar *address = string(consent, "email"), *purpose = string(consent, "purpose");
		g_autoptr(GDateTime) withdrawn = NULL;
		g_object_get(consent, "withdrawn-at", &withdrawn, NULL);
		if (withdrawn || g_strcmp0(address, email) || g_strcmp0(purpose, "marketing") ||
			number(consent, subject_field(G_OBJECT_TYPE(source))) != venture_entity_get_id(source)) return FALSE;
	}
	tracking = string(recipient, "tracking-hash");
	if (tracking && *tracking) {
		organization = get_row(self, VENTURE_TYPE_ORGANIZATION, org, org, error);
		if (!organization) return FALSE;
		if (!flag(organization, "marketing-tracking")) { *reason = "Tracking permission changed; create a new untracked send"; return FALSE; }
	}
	return TRUE;
}
static gboolean validate_mail(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureMarketingService *self = data;
	g_autofree gchar *related = string(row, "related-type");
	VentureEntity *recipient = self->enqueue_recipient;
	g_autoptr(VentureEntity) stored = NULL;
	const gchar *fields[] = { "subject", "text-body", "html-body", "private-text-body", "private-html-body" };
	guint i;
	if (g_strcmp0(related, "marketing_recipient")) return TRUE;
	self->enqueue_recipient = NULL;
	if (previous) {
		stored = get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, venture_entity_get_organization_id(row), number(row, "related-id"), error);
		if (!stored) return FALSE;
		return number(stored, "mail-id") == venture_entity_get_id(row) || refuse(error, "Outbox identity does not match the approved recipient");
	}
	if (!recipient || venture_entity_get_organization_id(recipient) != venture_entity_get_organization_id(row) ||
		venture_entity_get_id(recipient) != number(row, "related-id"))
		return refuse(error, "Marketing mail must be enqueued from its approved recipient");
	for (i = 0; i < G_N_ELEMENTS(fields); i++) {
		g_autofree gchar *expected = string(recipient, fields[i]), *actual = string(row, fields[i]);
		if (g_strcmp0(expected, actual)) return refuse(error, "Marketing mail content differs from its approved snapshot");
	}
	{
		g_autofree gchar *email = string(recipient, "email"), *to = string(row, "to"), *url = string(recipient, "unsubscribe-url"), *actual_url = string(row, "private-unsubscribe-url");
		g_autofree gchar *key = g_strconcat("marketing:", venture_entity_get_uuid(recipient), NULL), *actual_key = string(row, "idempotency-key");
		g_autofree gchar *cc = string(row, "cc"), *bcc = string(row, "bcc");
		if (g_strcmp0(email, to) || g_strcmp0(url, actual_url) || g_strcmp0(key, actual_key) || (cc && *cc) || (bcc && *bcc))
			return refuse(error, "Marketing mail requires its exact approved address and identity");
	}
	return TRUE;
}
static gboolean cancel_address(VentureMarketingService *self, gint64 org, const gchar *email,
	gint reason, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	(void)reason; (void)now;
	venture_query_set_organization(query, org); venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	{
		g_autoptr(GPtrArray) states = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(states, g_strdup("eligible")); g_ptr_array_add(states, g_strdup("queued"));
		venture_query_add_filter(query, "state", VENTURE_FILTER_OP_IN, states, NULL);
	}
	rows = venture_database_find(self->database, query, error);
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *recipient = g_ptr_array_index(rows, i);
		gint64 mail_id = number(recipient, "mail-id");
		const gchar *result = "cancelled";
		if (mail_id) {
			g_autoptr(VentureEntity) mail = get_row(self, VENTURE_TYPE_MAIL_MESSAGE, org, mail_id, error);
			g_autofree gchar *state = mail ? string(mail, "state") : NULL;
			if (!mail) return FALSE;
			if (!g_strcmp0(state, "queued") || !g_strcmp0(state, "failed")) {
				if (!venture_mail_outbox_cancel(venture_database_get_mail_outbox(self->database), org, mail_id, "Marketing permission withdrawn", actor, error)) return FALSE;
			} else if (!g_strcmp0(state, "sent")) result = "sent";
			else if (!g_strcmp0(state, "uncertain") || !g_strcmp0(state, "sending")) result = "uncertain";
		}
		g_object_set(recipient, "state", result, NULL);
		if (!save(self, recipient, actor, error)) return FALSE;
	}
	return TRUE;
}
static gboolean validate_suppression(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureMarketingService *self = data;
	g_autoptr(OrmInspector) inspector = orm_inspector_new(venture_database_get_connection(database), error);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *raw = string(row, "email"), *email = normalize(raw);
	gboolean exists;
	if (!inspector) return FALSE;
	exists = orm_inspector_has_table(inspector, "marketing_recipients", NULL, error);
	if (error && *error) return FALSE;
	if (!exists) return TRUE;
	return cancel_address(self, venture_entity_get_organization_id(row), email, choice(row, "reason"), now, NULL, error);
}
static gboolean before_claim(VentureMailOutbox *outbox, VentureMailMessage *message, GDateTime *now, gpointer data)
{
	VentureMarketingService *self = data;
	g_autofree gchar *related = string(VENTURE_ENTITY(message), "related-type");
	g_autoptr(VentureEntity) recipient = NULL, send = NULL;
	g_autoptr(GDateTime) next = NULL;
	g_autoptr(GError) error = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(message));
	if (g_strcmp0(related, "marketing_recipient")) return FALSE;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send")) return TRUE;
	recipient = get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, number(VENTURE_ENTITY(message), "related-id"), &error);
	if (!recipient) return FALSE;
	send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, number(recipient, "send-id"), &error);
	if (!send) return FALSE;
	g_object_get(send, "next-delivery-at", &next, NULL);
	return choice(send, "state") == 3 || (next && g_date_time_compare(next, now) > 0);
}
static gboolean before_transport(VentureMailOutbox *outbox, VentureMailMessage *message, GError **error, gpointer data)
{
	VentureMarketingService *self = data;
	g_autofree gchar *related = string(VENTURE_ENTITY(message), "related-type"), *state = NULL;
	g_autoptr(VentureEntity) recipient = NULL, send = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(message));
	const gchar *reason = "Marketing send is no longer approved";
	if (g_strcmp0(related, "marketing_recipient")) return FALSE;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send")) goto veto;
	recipient = get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, number(VENTURE_ENTITY(message), "related-id"), error);
	if (!recipient) return TRUE;
	send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, number(recipient, "send-id"), error);
	if (!send) return TRUE;
	state = string(recipient, "state");
	if (choice(send, "state") != 2 || g_strcmp0(state, "queued") || number(recipient, "mail-id") != venture_entity_get_id(VENTURE_ENTITY(message))) goto veto;
	if (!recipient_allowed(self, recipient, &reason, error)) goto veto;
	return FALSE;
veto:
	if (!error || !*error) refuse(error, reason);
	return TRUE;
}
static gboolean before_send(VentureMailOutbox *outbox, VentureMailMessage *message, GError **error, gpointer data)
{
	VentureMarketingService *self = data;
	g_autofree gchar *related = string(VENTURE_ENTITY(message), "related-type"), *state = NULL;
	g_autoptr(VentureEntity) recipient = NULL, send = NULL;
	g_autoptr(GDateTime) lease = NULL, now = NULL, next = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(message));
	const gchar *reason = "Marketing send is no longer approved";
	if (g_strcmp0(related, "marketing_recipient")) return FALSE;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send")) goto veto;
	recipient = get_row(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, number(VENTURE_ENTITY(message), "related-id"), error);
	if (!recipient) return TRUE;
	send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, number(recipient, "send-id"), error);
	if (!send) return TRUE;
	state = string(recipient, "state");
	if (choice(send, "state") != 2 || g_strcmp0(state, "queued") || number(recipient, "mail-id") != venture_entity_get_id(VENTURE_ENTITY(message))) goto veto;
	if (!recipient_allowed(self, recipient, &reason, error)) goto veto;
	g_object_get(message, "lease-until", &lease, NULL);
	if (!lease) { reason = "Marketing delivery has no lease clock"; goto veto; }
	now = g_date_time_add_seconds(lease, -VENTURE_MAIL_LEASE_SECONDS);
	next = g_date_time_add_seconds(now, number(send, "interval-seconds"));
	if (!next) { reason = "Marketing delivery interval exceeds calendar bounds"; goto veto; }
	g_object_set(send, "next-delivery-at", next, NULL);
	if (!save(self, send, NULL, error)) return TRUE;
	return before_transport(outbox, message, error, data);
veto:
	if (!error || !*error) refuse(error, reason);
	return TRUE;
}

gboolean venture_marketing_service_transition(VentureMarketingService *self, gint64 org,
	gint64 send_id, const gchar *transition, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) send = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gint state, next;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), FALSE);
	if (!allowed(self, org, error) || !venture_database_begin(self->database, error)) return FALSE;
	send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, send_id, error);
	if (!send) goto fail;
	state = choice(send, "state"); next = state;
	if (!g_strcmp0(transition, "approve") && state == 1 && number(send, "eligible-count") > 0) {
		next = 2; g_object_set(send, "approved-at", now, "approved-by", actor && actor->name ? actor->name : "system", NULL);
	} else if (!g_strcmp0(transition, "pause") && state == 2) next = 3;
	else if (!g_strcmp0(transition, "resume") && state == 3) next = 2;
	else if (!g_strcmp0(transition, "cancel") && state >= 1 && state <= 4) {
		g_autoptr(GPtrArray) rows = children(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, "send-id", send_id, 0, error);
		guint i;
		if (!rows) goto fail;
		next = 4;
		for (i = 0; i < rows->len; i++) {
			VentureEntity *recipient = g_ptr_array_index(rows, i);
			g_autofree gchar *status = string(recipient, "state");
			gint64 mail_id = number(recipient, "mail-id");
			if (g_strcmp0(status, "eligible") && g_strcmp0(status, "queued")) continue;
			if (mail_id) {
				g_autoptr(VentureEntity) mail = get_row(self, VENTURE_TYPE_MAIL_MESSAGE, org, mail_id, error);
				g_autofree gchar *mail_state = mail ? string(mail, "state") : NULL;
				if (!mail) goto fail;
				if (g_strcmp0(mail_state, "queued") && g_strcmp0(mail_state, "failed")) continue;
				if (!venture_mail_outbox_cancel(venture_database_get_mail_outbox(self->database), org, mail_id, "Campaign cancelled", actor, error)) goto fail;
			}
			g_object_set(recipient, "state", "cancelled", NULL);
			if (!save(self, recipient, actor, error)) goto fail;
		}
	} else { refuse(error, "Invalid marketing transition or empty approved audience"); goto fail; }
	g_object_set(send, "state", next, NULL);
	if (!save(self, send, actor, error)) goto fail;
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database); return FALSE;
}
gint venture_marketing_service_run(VentureMarketingService *self, gint64 org, gint64 send_id,
	guint limit, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) send = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) next = NULL, approved = NULL;
	guint i;
	gint count = 0;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), -1);
	if (!allowed(self, org, error)) return -1;
	if (!now || limit < 1 || limit > 100) { refuse(error, "Run requires a clock and limit 1 through 100"); return -1; }
	if (!venture_database_begin(self->database, error)) return -1;
	send = get_row(self, VENTURE_TYPE_MARKETING_SEND, org, send_id, error);
	if (!send) goto fail;
	if (choice(send, "state") != 2) goto done;
	g_object_get(send, "next-enqueue-at", &next, "approved-at", &approved, NULL);
	if (!approved || g_date_time_compare(approved, now) > 0 || (next && g_date_time_compare(next, now) > 0)) goto done;
	if (number(send, "last-mail-id")) {
		g_autoptr(VentureEntity) last = get_row(self, VENTURE_TYPE_MAIL_MESSAGE, org, number(send, "last-mail-id"), error);
		g_autofree gchar *state = last ? string(last, "state") : NULL;
		if (!last) goto fail;
		if (!g_strcmp0(state, "queued") || !g_strcmp0(state, "failed") || !g_strcmp0(state, "sending") || !g_strcmp0(state, "uncertain")) goto done;
	}
	venture_query_set_organization(query, org); venture_query_set_limit(query, limit);
	venture_query_add_filter_int(query, "send-id", VENTURE_FILTER_OP_EQ, send_id, NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "eligible", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (!rows) goto fail;
	if (!rows->len) { g_object_set(send, "state", 5, NULL); if (!save(self, send, actor, error)) goto fail; }
	for (i = 0; i < rows->len; i++) {
		VentureEntity *recipient = g_ptr_array_index(rows, i);
		g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
		g_autofree gchar *email = NULL, *key = NULL, *url = NULL;
		const gchar *reason = NULL;
		const gchar *fields[] = { "subject", "text-body", "html-body", "private-text-body", "private-html-body" };
		guint j;
		if (!recipient_allowed(self, recipient, &reason, error)) {
			if (error && *error) goto fail;
			g_object_set(recipient, "state", "cancelled", NULL);
			if (!save(self, recipient, actor, error)) goto fail;
			continue;
		}
		message = venture_mail_message_new();
		for (j = 0; j < G_N_ELEMENTS(fields); j++) {
			g_autofree gchar *value = string(recipient, fields[j]);
			g_object_set(message, fields[j], value, NULL);
		}
		email = string(recipient, "email"); url = string(recipient, "unsubscribe-url");
		key = g_strconcat("marketing:", venture_entity_get_uuid(recipient), NULL);
		g_object_set(message, "organization-id", org, "to", email, "private-unsubscribe-url", url,
			"related-type", "marketing_recipient", "related-id", venture_entity_get_id(recipient),
			"idempotency-key", key, "next-attempt-at", now, NULL);
		self->enqueue_recipient = recipient;
		queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, actor, error);
		self->enqueue_recipient = NULL;
		if (!queued) goto fail;
		g_object_set(recipient, "mail-id", venture_entity_get_id(VENTURE_ENTITY(queued)), "state", "queued", NULL);
		if (!save(self, recipient, actor, error)) goto fail;
		g_clear_pointer(&next, g_date_time_unref); next = g_date_time_add_seconds(now, number(send, "interval-seconds"));
		if (!next) { refuse(error, "Run interval exceeds calendar bounds"); goto fail; }
		g_object_set(send, "next-enqueue-at", next, "last-mail-id", venture_entity_get_id(VENTURE_ENTITY(queued)), NULL);
		if (!save(self, send, actor, error)) goto fail;
		count++; break;
	}
done:
	return venture_database_commit(self->database, error) ? count : -1;
fail:
	venture_database_rollback(self->database); return -1;
}
