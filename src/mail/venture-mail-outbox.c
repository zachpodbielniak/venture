/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>
struct _VentureMailOutbox {
	GObject parent_instance;
	VentureDatabase *database;
	VentureMailer *mailer;
	VentureEntity *permit;
	VentureEntity *user_permit;
	VentureEntity *enqueue_permit;
	gchar *attachment_root;
	guint max_attempts;
};
G_DEFINE_FINAL_TYPE(VentureMailOutbox, venture_mail_outbox, G_TYPE_OBJECT)
enum { SIGNAL_BEFORE_SEND, N_SIGNALS };
static guint signals[N_SIGNALS];
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

static gboolean snapshot_attachments(VentureMailOutbox *self, VentureEntity *entity, GError **error)
{
	g_autofree gchar *refs = NULL, *root = NULL, *serialized = NULL;
	g_autoptr(JsonNode) parsed = NULL, snapshot = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	JsonArray *array;
	guint i;
	gsize total = 0;
	venture_entity_set_attribute(entity, "_mail_attachments", NULL);
	g_object_get(entity, "attachments", &refs, NULL);
	if (!refs || !*refs) return TRUE;
	parsed = json_from_string(refs, error);
	if (!parsed) return FALSE;
	if (!JSON_NODE_HOLDS_ARRAY(parsed)) return refuse(error, "Attachments must be document references in a JSON array");
	array = json_node_get_array(parsed);
	if (!json_array_get_length(array)) return TRUE;
	root = self->attachment_root ? realpath(self->attachment_root, NULL) : NULL;
	json_builder_begin_array(builder);
	for (i = 0; i < json_array_get_length(array); i++) {
		JsonNode *element = json_array_get_element(array, i);
		JsonObject *ref;
		g_autoptr(VentureEntity) document = NULL;
		g_autofree gchar *title = NULL, *mime = NULL, *encoded = NULL;
		g_autoptr(GBytes) attachment = NULL;
		gconstpointer bytes;
		gsize length;
		if (!JSON_NODE_HOLDS_OBJECT(element)) return refuse(error, "Invalid attachment reference");
		ref = json_node_get_object(element);
		/* An inline attachment carries its own bytes, base64 in "data", for
		 * content generated at enqueue time (a report pack's CSV) that has
		 * no document row. It is retained like a document snapshot. */
		if (!g_strcmp0(venture_json_object_get_string(ref, "type", ""), "inline")) {
			const gchar *name = venture_json_object_get_string(ref, "name", NULL);
			const gchar *data = venture_json_object_get_string(ref, "data", NULL);
			g_autofree guchar *decoded = NULL;
			if (!name || !*name || !data || !*data) return refuse(error, "Inline attachments need a name and base64 data");
			decoded = g_base64_decode(data, &length);
			if (!length) return refuse(error, "Inline attachment data is not base64");
			if (length > 20 * 1024 * 1024) return refuse(error, "Attachment exceeds 20 MiB");
			total += length;
			if (total > 20 * 1024 * 1024) return refuse(error, "Combined attachments exceed 20 MiB");
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "name"); json_builder_add_string_value(builder, name);
			json_builder_set_member_name(builder, "mime"); json_builder_add_string_value(builder, venture_json_object_get_string(ref, "mime", "application/octet-stream"));
			json_builder_set_member_name(builder, "data"); json_builder_add_string_value(builder, data);
			json_builder_end_object(builder);
			continue;
		}
		if (g_strcmp0(venture_json_object_get_string(ref, "type", ""), "document")) return refuse(error, "Attachments must reference documents");
		if (!root) return refuse(error, "Attachment storage is not configured");
		document = venture_database_get(self->database, VENTURE_TYPE_DOCUMENT, venture_json_object_get_int(ref, "id", 0), error);
		if (!document || venture_entity_is_deleted(document)) { if (!error || !*error) refuse(error, "Attachment document not found"); return FALSE; }
		if (venture_entity_get_organization_id(document) != venture_entity_get_organization_id(entity)) return refuse(error, "Attachment belongs to another organization");
		g_object_get(document, "title", &title, "mime-type", &mime, NULL);
		attachment = venture_document_service_read_attachment(venture_document_service_get(self->database), document, root, 20 * 1024 * 1024, error);
		if (attachment == NULL) return FALSE;
		bytes = g_bytes_get_data(attachment, &length);
		total += length;
		if (total > 20 * 1024 * 1024) return refuse(error, "Combined attachments exceed 20 MiB");
		encoded = g_base64_encode((guchar *)bytes, length);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name"); json_builder_add_string_value(builder, title && *title ? title : "attachment");
		json_builder_set_member_name(builder, "mime"); json_builder_add_string_value(builder, mime && *mime ? mime : "application/octet-stream");
		json_builder_set_member_name(builder, "data"); json_builder_add_string_value(builder, encoded);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	snapshot = json_builder_get_root(builder);
	serialized = json_to_string(snapshot, FALSE);
	venture_entity_set_attribute(entity, "_mail_attachments", serialized);
	return TRUE;
}
static gboolean validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	VentureMailOutbox *self = data;
	g_autofree gchar *private_body = NULL;
	g_autofree gchar *private_html = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *key = NULL;
	gint64 attempts;
	gboolean permitted = self->permit == entity;
	self->permit = NULL;
	if (!enabled(self, venture_entity_get_organization_id(entity), error)) return FALSE;
	if (permitted) return TRUE;
	if (previous) return refuse(error, "Stored messages are immutable; use retry for a deliberate resend");
	g_object_get(entity, "private-text-body", &private_body, "private-html-body", &private_html, NULL);
	if (((private_body && *private_body) || (private_html && *private_html)) && self->enqueue_permit != entity)
		return refuse(error, "Private delivery content requires the outbox enqueue service");
	g_object_get(entity, "state", &state, "message-id", &id, "idempotency-key", &key, "attempts", &attempts, NULL);
	if ((state && *state && strcmp(state, "queued")) || (id && *id) || attempts)
		return refuse(error, "Delivery state is managed by the outbox");
	if (!snapshot_attachments(self, entity, error)) return FALSE;
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
	g_free(self->attachment_root);
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
	case 4: g_free(self->attachment_root); self->attachment_root = g_value_dup_string(value); break;
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
	case 4: g_value_set_string(value, self->attachment_root); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void constructed(GObject *object)
{
	VentureMailOutbox *self = VENTURE_MAIL_OUTBOX(object);
	venture_database_add_save_validator(self->database, VENTURE_TYPE_MAIL_MESSAGE, validate, g_object_ref(self), g_object_unref);
	G_OBJECT_CLASS(venture_mail_outbox_parent_class)->constructed(object);
}
static void venture_mail_outbox_class_init(VentureMailOutboxClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->finalize = finalize;
	object->constructed = constructed;
	object->set_property = set_property;
	object->get_property = get_property;
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database", "Weak owning database", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 2, g_param_spec_object("mailer", "Mailer", "One-attempt transport", VENTURE_TYPE_MAILER, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 3, g_param_spec_uint("max-attempts", "Maximum attempts", "Automatic retry budget", 1, 20, 5, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object, 4, g_param_spec_string("attachment-root", "Attachment root", "Root containing uploaded document files", NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * VentureMailOutbox::before-send:
	 * @self: the outbox
	 * @message: the claimed message about to be handed to the transport
	 * @error: (out): a #GError location the vetoing handler fills
	 *
	 * Emitted after the sending lease is committed and before the one
	 * transport attempt. The source lane that enqueued a message rechecks
	 * its eligibility here (an invoice settled after its reminder was
	 * queued, a customer who opted out). A handler returning %TRUE vetoes
	 * the attempt; the row becomes =cancelled= with the handler's message
	 * and is never retried automatically. Handlers run in connection order
	 * and the first veto stops the emission.
	 *
	 * Returns: %TRUE to veto the send
	 */
	signals[SIGNAL_BEFORE_SEND] = g_signal_new("before-send", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 2, VENTURE_TYPE_MAIL_MESSAGE, G_TYPE_POINTER);
}
static void venture_mail_outbox_init(VentureMailOutbox *self) { }
VentureMailOutbox *venture_mail_outbox_new(VentureDatabase *database, VentureMailer *mailer)
{
	VentureMailOutbox *self = venture_database_get_mail_outbox(database);
	if (mailer) g_object_set(self, "mailer", mailer, NULL);
	return g_object_ref(self);
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
	/* Trust the private body only; all ordinary enqueue checks still run. */
	self->enqueue_permit = VENTURE_ENTITY(copy);
	if (!venture_database_save(self->database, VENTURE_ENTITY(copy), actor, error)) {
		self->enqueue_permit = NULL;
		goto fail;
	}
	self->enqueue_permit = NULL;
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
	if (!row || venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org) {
		g_clear_object(&row);
		if (!error || !*error) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Mail message not found");
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
	if (venture_database_has_transaction(self->database)) { refuse(error, "Claim requires a committed database"); return NULL; }
	if (!venture_database_begin(self->database, error)) return NULL;
	row = get_row(self, org, id, error);
	if (!row) goto fail;
	if (!due(row, now)) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Message is not due or already claimed"); goto fail; }
	g_object_get(row, "attempts", &attempts, NULL);
	lease = g_date_time_add_seconds(now, VENTURE_MAIL_LEASE_SECONDS);
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
	if (venture_database_has_transaction(self->database)) { refuse(error, "Delivery requires a committed database"); return -1; }
	venture_query_set_organization(query, org);
	{
		g_autoptr(GPtrArray) states = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(states, g_strdup("queued")); g_ptr_array_add(states, g_strdup("failed")); g_ptr_array_add(states, g_strdup("sending"));
		venture_query_add_filter(query, "state", VENTURE_FILTER_OP_IN, states, NULL);
	}
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
		{
			gboolean vetoed = FALSE;
			g_signal_emit(self, signals[SIGNAL_BEFORE_SEND], 0, claimed, &send_error, &vetoed);
			if (vetoed) {
				/* A veto is a decision, not a transport outcome: undo the claim's
				 * attempt increment so cancelled rows spend no retry budget. */
				g_object_get(claimed, "attempts", &attempts, NULL);
				g_object_set(claimed, "state", "cancelled", "lease-until", NULL, "next-attempt-at", NULL,
					"attempts", attempts > 0 ? attempts - 1 : (gint64)0,
					"last-error", send_error ? send_error->message : "Cancelled before submission", NULL);
				if (!save(self, VENTURE_ENTITY(claimed), NULL, error)) return -1;
				continue;
			}
		}
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
		/* The relay already accepted the message, so the timeline entry is
		 * recorded after the sent state commits and its failure is only
		 * logged. Rolling the sent state back with it left the row to
		 * become uncertain, and the operator's retry mailed the customer
		 * a second time. */
		if (sent) {
			g_autoptr(GError) timeline_error = NULL;
			if (!venture_mail_sync_record_outbound(self->database, claimed, &timeline_error))
				g_warning("VentureMailOutbox: message %" G_GINT64_FORMAT " was sent but its timeline entry was not recorded: %s",
					venture_entity_get_id(VENTURE_ENTITY(claimed)), timeline_error ? timeline_error->message : "unknown error");
		}
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

gboolean venture_mail_save_user(VentureMailOutbox *self, VentureEntity *user, const VentureActor *actor, gboolean *handled, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL, original = NULL;
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autofree gchar *email = NULL, *old_hash = NULL, *new_hash = NULL, *key = NULL;
	gboolean reset, ok;
	*handled = FALSE;
	if (self->user_permit == user) { self->user_permit = NULL; return TRUE; }
	if (!VENTURE_IS_USER(user) || !venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message")) return TRUE;
	g_object_get(user, "email", &email, "password-hash", &new_hash, NULL);
	if (!email || !*email) return TRUE;
	if (venture_entity_get_id(user)) {
		previous = venture_database_get(self->database, VENTURE_TYPE_USER, venture_entity_get_id(user), error);
		if (!previous) return FALSE;
		g_object_get(previous, "password-hash", &old_hash, NULL);
		if (!g_strcmp0(old_hash, new_hash)) return TRUE;
	}
	*handled = TRUE;
	reset = previous != NULL;
	original = g_object_new(VENTURE_TYPE_USER, NULL);
	venture_entity_copy_properties_from(original, user, FALSE);
	if (!venture_database_begin(self->database, error)) return FALSE;
	self->user_permit = user;
	ok = venture_database_save(self->database, user, actor, error);
	self->user_permit = NULL;
	if (!ok) goto fail_user;
	message = venture_mail_message_new();
	key = g_strdup_printf("user:%s:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(user), reset ? "reset" : "welcome", venture_entity_get_version(user));
	g_object_set(message, "organization-id", venture_entity_get_organization_id(user), "to", email,
		"subject", reset ? "Your password was reset" : "Welcome to Venture",
		"text-body", reset ? "Your Venture password was changed. Contact your administrator if you did not request this change." : "Your Venture account is ready. Contact your administrator for sign-in instructions.",
		"idempotency-key", key, "related-type", "user", "related-id", venture_entity_get_id(user), NULL);
	queued = venture_mail_outbox_enqueue(self, message, actor, error);
	if (!queued) goto fail_user;
	return venture_database_commit(self->database, error);
fail_user:
	venture_database_rollback(self->database);
	venture_entity_copy_properties_from(user, original, FALSE);
	return FALSE;
}

gboolean venture_mail_check_removal(VentureEntity *entity, GError **error)
{
	return !VENTURE_IS_MAIL_MESSAGE(entity) || refuse(error, "Mail history and idempotency keys cannot be removed");
}
