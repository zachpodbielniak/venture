/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
struct _VentureMailOutbox {
	GObject parent_instance;
	VentureDatabase *database;
	VentureMailer *mailer;
	VentureEntity *permit;
	guint max_attempts;
};
G_DEFINE_FINAL_TYPE(VentureMailOutbox, venture_mail_outbox, G_TYPE_OBJECT)
static gboolean refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "mail", "%s (VentureMailOutbox)", message);
	return FALSE;
}
static gboolean enabled(VentureMailOutbox *self, gint64 org, GError **error)
{
	if (!self->database || org <= 0) return refuse(error, "An exact organization is required");
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message"))
		return refuse(error, "The mail module is disabled");
	return TRUE;
}
static gboolean validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	VentureMailOutbox *self = data;
	g_autofree gchar *state = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *key = NULL;
	gint64 attempts;
	gboolean permitted = self->permit == entity;
	self->permit = NULL;
	if (!enabled(self, venture_entity_get_organization_id(entity), error)) return FALSE;
	if (permitted) return TRUE;
	if (previous) return refuse(error, "Stored messages are immutable; use retry for a deliberate resend");
	g_object_get(entity, "state", &state, "message-id", &id, "idempotency-key", &key, "attempts", &attempts, NULL);
	if ((state && *state && strcmp(state, "queued")) || (id && *id) || attempts)
		return refuse(error, "Delivery state is managed by the outbox");
	if (!key || !*key) { g_free(key); key = g_uuid_string_random(); }
	g_free(id);
	id = g_strdup_printf("%s@venture.invalid", venture_entity_get_uuid(entity));
	g_object_set(entity, "state", "queued", "message-id", id, "idempotency-key", key,
		"lease-until", NULL, "sent-at", NULL, "last-error", NULL, NULL);
	return TRUE;
}
static gboolean save(VentureMailOutbox *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->permit = row;
	ok = venture_database_save(self->database, row, actor, error);
	self->permit = NULL;
	return ok;
}
static void finalize(GObject *object)
{
	VentureMailOutbox *self = VENTURE_MAIL_OUTBOX(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_object(&self->mailer);
	G_OBJECT_CLASS(venture_mail_outbox_parent_class)->finalize(object);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureMailOutbox *self = VENTURE_MAIL_OUTBOX(object);
	switch (id) {
	case 1:
		self->database = g_value_get_object(value);
		g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		break;
	case 2: g_set_object(&self->mailer, g_value_get_object(value)); break;
	case 3: self->max_attempts = g_value_get_uint(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureMailOutbox *self = VENTURE_MAIL_OUTBOX(object);
	switch (id) {
	case 1: g_value_set_object(value, self->database); break;
	case 2: g_value_set_object(value, self->mailer); break;
	case 3: g_value_set_uint(value, self->max_attempts); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void venture_mail_outbox_class_init(VentureMailOutboxClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->finalize = finalize;
	object->set_property = set_property;
	object->get_property = get_property;
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database", "Weak owning database", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 2, g_param_spec_object("mailer", "Mailer", "One-attempt transport", VENTURE_TYPE_MAILER, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 3, g_param_spec_uint("max-attempts", "Maximum attempts", "Automatic retry budget", 1, 20, 5, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
}
static void venture_mail_outbox_init(VentureMailOutbox *self) { }
VentureMailOutbox *venture_mail_outbox_new(VentureDatabase *database, VentureMailer *mailer)
{
	VentureMailOutbox *self = g_object_new(VENTURE_TYPE_MAIL_OUTBOX, "database", database, "mailer", mailer, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_MAIL_MESSAGE, validate, g_object_ref(self), g_object_unref);
	return self;
}
VentureMailMessage *venture_mail_outbox_enqueue(VentureMailOutbox *self, VentureMailMessage *message, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureMailMessage) copy = NULL;
	g_autofree gchar *key = NULL;
	gint64 org = venture_entity_get_organization_id(VENTURE_ENTITY(message));
	if (!enabled(self, org, error)) return NULL;
	if (venture_entity_get_id(VENTURE_ENTITY(message))) { refuse(error, "Enqueue requires an unsaved message"); return NULL; }
	if (!venture_database_begin(self->database, error)) return NULL;
	g_object_get(message, "idempotency-key", &key, NULL);
	if (key && *key) {
		query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		venture_query_set_organization(query, org);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_string(query, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
		existing = venture_database_find_one(self->database, query, error);
		if (error && *error) goto fail;
	}
	if (existing) {
		if (!venture_database_commit(self->database, error)) return NULL;
		return VENTURE_MAIL_MESSAGE(g_steal_pointer(&existing));
	}
	copy = VENTURE_MAIL_MESSAGE(venture_entity_duplicate(VENTURE_ENTITY(message)));
	if (!venture_database_save(self->database, VENTURE_ENTITY(copy), actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&copy);
fail:
	venture_database_rollback(self->database);
	return NULL;
}
static VentureEntity *get_row(VentureMailOutbox *self, gint64 org, gint64 id, GError **error)
{
	VentureEntity *row;
	if (!enabled(self, org, error)) return NULL;
	row = venture_database_get(self->database, VENTURE_TYPE_MAIL_MESSAGE, id, error);
	if (row && venture_entity_get_organization_id(row) != org) {
		g_clear_object(&row);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Mail message not found");
	}
	return row;
}
static gboolean due(VentureEntity *row, GDateTime *now)
{
	g_autofree gchar *state = NULL;
	g_autoptr(GDateTime) next = NULL;
	g_object_get(row, "state", &state, "next-attempt-at", &next, NULL);
	return (!g_strcmp0(state, "queued") || !g_strcmp0(state, "failed")) && (!next || g_date_time_compare(next, now) <= 0);
}
VentureMailMessage *venture_mail_outbox_claim(VentureMailOutbox *self, gint64 org, gint64 id, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GDateTime) lease = NULL;
	gint64 attempts;
	if (!enabled(self, org, error)) return NULL;
	if (!venture_database_begin(self->database, error)) return NULL;
	row = get_row(self, org, id, error);
	if (!row) goto fail;
	if (!due(row, now)) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Message is not due or already claimed"); goto fail; }
	g_object_get(row, "attempts", &attempts, NULL);
	lease = g_date_time_add_seconds(now, 600);
	g_object_set(row, "state", "sending", "attempts", attempts + 1, "lease-until", lease, NULL);
	if (!save(self, row, NULL, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return VENTURE_MAIL_MESSAGE(g_steal_pointer(&row));
fail:
	venture_database_rollback(self->database);
	return NULL;
}
gint venture_mail_outbox_deliver_due(VentureMailOutbox *self, gint64 org, guint limit, GDateTime *now, GCancellable *cancellable, GError **error)
{
	g_autoptr(GDateTime) clock = now ? g_date_time_ref(now) : g_date_time_new_now_utc();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	gint count = 0;
	if (!enabled(self, org, error)) return -1;
	if (!self->mailer) { refuse(error, "No mail transport configured"); return -1; }
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return -1;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *state = NULL;
		g_autoptr(GDateTime) lease = NULL;
		g_autoptr(VentureMailMessage) claimed = NULL;
		g_autoptr(GError) send_error = NULL;
		g_autoptr(GDateTime) next = NULL;
		gint64 attempts;
		gboolean sent;
		g_object_get(row, "state", &state, "lease-until", &lease, NULL);
		if (!g_strcmp0(state, "sending") && (!lease || g_date_time_compare(lease, clock) <= 0)) {
			g_object_set(row, "state", "uncertain", "lease-until", NULL, "last-error", "Sending lease expired; submission may have succeeded", NULL);
			if (!save(self, row, NULL, error)) return -1;
			continue;
		}
		if ((guint)count >= limit || !due(row, clock)) continue;
		if (g_cancellable_set_error_if_cancelled(cancellable, error)) return -1;
		claimed = venture_mail_outbox_claim(self, org, venture_entity_get_id(row), clock, &send_error);
		if (!claimed) {
			if (g_error_matches(send_error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT)) continue;
			g_propagate_error(error, g_steal_pointer(&send_error)); return -1;
		}
		count++;
		sent = venture_mailer_send(self->mailer, claimed, cancellable, &send_error);
		g_object_get(claimed, "attempts", &attempts, NULL);
		if (sent) g_object_set(claimed, "state", "sent", "sent-at", clock, "last-error", NULL, NULL);
		else {
			const gchar *outcome = "uncertain";
			if (g_error_matches(send_error, VENTURE_ERROR, VENTURE_ERROR_MAIL_TRANSIENT)) {
				outcome = attempts >= self->max_attempts ? "dead" : "failed";
				next = g_date_time_add_seconds(clock, 60 * ((gint64)1 << MIN(attempts - 1, 10)));
			} else if (g_error_matches(send_error, VENTURE_ERROR, VENTURE_ERROR_MAIL_PERMANENT)) outcome = "dead";
			g_object_set(claimed, "state", outcome, "last-error", send_error ? send_error->message : "Unknown submission outcome", NULL);
		}
		g_object_set(claimed, "lease-until", NULL, "next-attempt-at", next, NULL);
		/* If persistence fails the committed sending lease remains. Its
		 * expiry is uncertain, so a crash never causes an automatic resend. */
		if (!save(self, VENTURE_ENTITY(claimed), NULL, error)) return -1;
	}
	return count;
}
gboolean venture_mail_outbox_retry(VentureMailOutbox *self, gint64 org, gint64 id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = get_row(self, org, id, error);
	g_autofree gchar *state = NULL;
	if (!row) return FALSE;
	g_object_get(row, "state", &state, NULL);
	if (g_strcmp0(state, "uncertain") && g_strcmp0(state, "failed") && g_strcmp0(state, "dead")) return refuse(error, "Only uncertain, failed or dead messages can be retried");
	g_object_set(row, "state", "queued", "attempts", (gint64)0, "next-attempt-at", NULL, "lease-until", NULL, NULL);
	return save(self, row, actor, error);
}
