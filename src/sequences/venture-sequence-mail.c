/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-sequence-service-private.h"

#define STATE_KEY "venture-sequence-mail"
typedef struct {
	VentureDatabase *database; /* Weak; owned by the sequence service's repository. */
	VentureEntity *enqueuing;
	GArray *outcomes;
	gboolean settling;
} SequenceMail;

static gchar *string(VentureEntity *row, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(row, name, &value, NULL);
	return value;
}
static gint64 number(VentureEntity *row, const gchar *name)
{
	gint64 value = 0;
	g_object_get(row, name, &value, NULL);
	return value;
}
static gint choice(VentureEntity *row, const gchar *name)
{
	gint value = 0;
	g_object_get(row, name, &value, NULL);
	return value;
}
static gboolean ours(VentureEntity *message)
{
	g_autofree gchar *type = string(message, "related-type");
	return g_strcmp0(type, "sequence_delivery") == 0;
}
static gchar *delivery_key(VentureEntity *delivery)
{
	return g_strconcat("sequence-delivery:", venture_entity_get_uuid(delivery), NULL);
}
static gboolean refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "sequence_mail", "%s", message);
	return FALSE;
}
static VentureEntity *reference(VentureDatabase *database, GType type, gint64 id, gint64 org, GError **error)
{
	VentureEntity *row = venture_database_get(database, type, id, error);
	if (row != NULL && (venture_entity_get_organization_id(row) != org || venture_entity_is_deleted(row)))
		g_clear_object(&row);
	if (row == NULL && (error == NULL || *error == NULL))
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Sequence mail source not found");
	return row;
}
static SequenceMail *state(VentureDatabase *database)
{
	return g_object_get_data(G_OBJECT(venture_sequence_service_get(database)), STATE_KEY);
}

/* The outbox's sent state has already committed. A failed projection must
 * never roll that acceptance back into a sending lease or cause a resend. */
static gboolean settle(SequenceMail *self, gint64 id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	g_autoptr(VentureEntity) message = venture_database_get(self->database, VENTURE_TYPE_MAIL_MESSAGE, id, error);
	g_autoptr(VentureEntity) delivery = NULL;
	g_autofree gchar *status = NULL, *message_id = NULL;
	if (message == NULL) return error == NULL || *error == NULL;
	if (!ours(message)) return TRUE;
	status = string(message, "state");
	if (g_strcmp0(status, "sent") != 0) return TRUE;
	delivery = reference(self->database, VENTURE_TYPE_SEQUENCE_DELIVERY, number(message, "related-id"),
		venture_entity_get_organization_id(message), error);
	if (delivery == NULL) return FALSE;
	if (choice(delivery, "state") != 0) return TRUE;
	message_id = string(message, "message-id");
	g_object_set(delivery, "state", 1, "external-message-id", message_id, NULL);
	return venture_database_save(self->database, delivery, NULL, error);
}

static void finished(VentureDatabase *database, gboolean committed, VentureSequenceService *service)
{
	SequenceMail *self = g_object_get_data(G_OBJECT(service), STATE_KEY);
	g_autoptr(GArray) pending = NULL;
	guint i;
	(void)database;
	if (self->settling) return;
	pending = self->outcomes;
	self->outcomes = g_array_new(FALSE, FALSE, sizeof(gint64));
	if (!committed) return;
	self->settling = TRUE;
	for (i = 0; i < pending->len; i++) {
		g_autoptr(GError) error = NULL;
		if (!settle(self, g_array_index(pending, gint64, i), &error))
			g_debug("Sequence delivery projection deferred; accepted mail remains retained");
	}
	self->settling = FALSE;
}

static void saved(VentureDatabase *database, VentureEntity *row, gboolean created, VentureSequenceService *service)
{
	SequenceMail *self = g_object_get_data(G_OBJECT(service), STATE_KEY);
	g_autofree gchar *status = NULL;
	gint64 id;
	(void)created;
	if (!VENTURE_IS_MAIL_MESSAGE(row) || !ours(row)) return;
	status = string(row, "state");
	if (g_strcmp0(status, "sent") != 0) return;
	id = venture_entity_get_id(row);
	g_array_append_val(self->outcomes, id);
	if (!venture_database_has_transaction(database)) finished(database, TRUE, service);
}

static gboolean validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	SequenceMail *self = data;
	g_autofree gchar *expected = NULL, *actual = NULL;
	(void)database;
	if (previous != NULL || !ours(row)) return TRUE;
	if (self->enqueuing == NULL || number(row, "related-id") != venture_entity_get_id(self->enqueuing) ||
		venture_entity_get_organization_id(row) != venture_entity_get_organization_id(self->enqueuing))
		return refuse(error, "Sequence mail is enqueued only by its adapter");
	expected = delivery_key(self->enqueuing);
	actual = string(row, "idempotency-key");
	return g_strcmp0(expected, actual) == 0 || refuse(error, "Sequence delivery identity does not match");
}

static gboolean eligible(SequenceMail *self, VentureEntity *message, GDateTime *now, gboolean *defer, GError **error)
{
	gint64 org = venture_entity_get_organization_id(message);
	g_autoptr(VentureEntity) delivery = NULL, enrollment = NULL, contact = NULL;
	g_autofree gchar *recipient = NULL, *current = NULL, *expected = NULL, *actual = NULL;
	*defer = FALSE;
	delivery = reference(self->database, VENTURE_TYPE_SEQUENCE_DELIVERY, number(message, "related-id"), org, error);
	if (delivery == NULL) return FALSE;
	if (!venture_sequence_delivery_eligible(self->database, delivery, now, defer, error)) return FALSE;
	if (*defer) return TRUE;
	expected = delivery_key(delivery);
	actual = string(message, "idempotency-key");
	if (g_strcmp0(expected, actual) != 0) return refuse(error, "Sequence delivery identity does not match");
	enrollment = reference(self->database, VENTURE_TYPE_SEQUENCE_ENROLLMENT, number(delivery, "enrollment-id"), org, error);
	if (enrollment == NULL) return FALSE;
	contact = reference(self->database, VENTURE_TYPE_CONTACT, number(enrollment, "contact-id"), org, error);
	if (contact == NULL) return FALSE;
	recipient = string(message, "to");
	current = string(contact, "email");
	return g_strcmp0(recipient, current) == 0 || refuse(error, "Sequence recipient changed after enqueue");
}

static gboolean before_claim(VentureMailOutbox *outbox, VentureMailMessage *message, GDateTime *now,
	VentureSequenceService *service)
{
	SequenceMail *self = g_object_get_data(G_OBJECT(service), STATE_KEY);
	g_autoptr(GError) error = NULL;
	gboolean defer = FALSE;
	(void)outbox;
	if (!ours(VENTURE_ENTITY(message))) return FALSE;
	if (eligible(self, VENTURE_ENTITY(message), now, &defer, &error)) return defer;
	/* Permanent refusals reach the final gate and become cancelled. A read
 * failure leaves the same message queued without spending its retry budget. */
	return !g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION) &&
		!g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND) &&
		!g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

static gboolean before_transport(VentureMailOutbox *outbox, VentureMailMessage *message, GError **error,
	VentureSequenceService *service)
{
	SequenceMail *self = g_object_get_data(G_OBJECT(service), STATE_KEY);
	g_autoptr(GDateTime) lease = NULL, now = NULL;
	gboolean defer = FALSE;
	(void)outbox;
	if (!ours(VENTURE_ENTITY(message))) return FALSE;
	g_object_get(message, "lease-until", &lease, NULL);
	now = lease != NULL ? g_date_time_add_seconds(lease, -VENTURE_MAIL_LEASE_SECONDS) : venture_time_now();
	if (!eligible(self, VENTURE_ENTITY(message), now, &defer, error)) return TRUE;
	if (!defer) return FALSE;
	refuse(error, "Sequence was paused or left its sending window before submission");
	return TRUE;
}

static void free_state(gpointer data)
{
	SequenceMail *self = data;
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_array_unref(self->outcomes);
	g_free(self);
}

void venture_sequence_mail_install(VentureSequenceService *service, VentureDatabase *database)
{
	SequenceMail *self = g_new0(SequenceMail, 1);
	VentureMailOutbox *outbox = venture_database_get_mail_outbox(database);
	self->database = database;
	g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
	self->outcomes = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_object_set_data_full(G_OBJECT(service), STATE_KEY, self, free_state);
	venture_database_add_save_validator(database, VENTURE_TYPE_MAIL_MESSAGE, validate, self, NULL);
	g_signal_connect_object(database, "entity-saved", G_CALLBACK(saved), service, 0);
	g_signal_connect_object(database, "transaction-finished", G_CALLBACK(finished), service, 0);
	g_signal_connect_object(outbox, "before-claim", G_CALLBACK(before_claim), service, 0);
	g_signal_connect_object(outbox, "before-send", G_CALLBACK(before_transport), service, 0);
	g_signal_connect_object(outbox, "before-transport", G_CALLBACK(before_transport), service, 0);
}

VentureMailMessage *venture_sequence_mail_build(VentureDatabase *database, VentureEntity *delivery, GError **error)
{
	gint64 org = venture_entity_get_organization_id(delivery);
	g_autoptr(VentureEntity) enrollment = NULL, contact = NULL;
	g_autofree gchar *key = NULL, *recipient = NULL, *subject = NULL, *body = NULL, *private_body = NULL;
	g_autofree gchar *text = NULL, *private_text = NULL, *token = NULL, *base = NULL, *url = NULL, *html = NULL;
	g_autofree gchar *private_html = NULL, *escaped_url = NULL, *footer_text = NULL;
	gboolean markup;
	enrollment = reference(database, VENTURE_TYPE_SEQUENCE_ENROLLMENT, number(delivery, "enrollment-id"), org, error);
	if (enrollment == NULL) return NULL;
	contact = reference(database, VENTURE_TYPE_CONTACT, number(enrollment, "contact-id"), org, error);
	if (contact == NULL) return NULL;
	key = delivery_key(delivery);
	recipient = string(contact, "email");
	subject = string(delivery, "subject");
	body = string(delivery, "body");
	private_body = string(delivery, "private-body");
	token = string(delivery, "unsubscribe-token");
	g_object_get(venture_sequence_service_get(database), "base-url", &base, NULL);
	if (token == NULL || *token == '\0' || base == NULL || *base == '\0') {
		refuse(error, "Sequence email requires an unsubscribe token and a public base URL");
		return NULL;
	}
	{
		g_autoptr(GUri) uri = g_uri_parse(base, G_URI_FLAGS_NONE, NULL);
		const gchar *host = uri != NULL ? g_uri_get_host(uri) : NULL;
		const gchar *scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
		if (host == NULL || *host == '\0' || g_uri_get_userinfo(uri) != NULL ||
			g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
			strpbrk(base, "\r\n\t \"'<>" ) != NULL ||
			(g_strcmp0(scheme, "https") != 0 &&
				(g_strcmp0(scheme, "http") != 0 ||
					(g_strcmp0(host, "localhost") != 0 && g_strcmp0(host, "127.0.0.1") != 0 && g_strcmp0(host, "::1") != 0)))) {
			refuse(error, "Sequence email requires a public HTTPS base URL");
			return NULL;
		}
	}
	/* A step written with markup is the HTML part and its text the plain
	 * part; plain words are only a text part, never HTML. */
	markup = body != NULL && strchr(body, '<') != NULL;
	text = markup ? venture_document_html_to_text(body, -1) : g_strdup(body);
	if (!venture_string_is_empty(private_body))
		private_text = venture_document_html_to_text(private_body, -1);
	{
		/* A trailing slash on the public origin would miss the route. */
		if (g_str_has_suffix(base, "/")) base[strlen(base) - 1] = '\0';
		url = g_strdup_printf("%s/sequences/u/%s", base, token);
		footer_text = g_strdup_printf("%s\n\nStop these emails: %s", private_text != NULL ? private_text : text != NULL ? text : "", url);
		escaped_url = g_markup_escape_text(url, -1);
		html = private_body != NULL ? g_strdup(private_body) : markup ? g_strdup(body) : g_markup_escape_text(text != NULL ? text : "", -1);
		private_html = g_strdup_printf("%s<p><a href=\"%s\">Stop these emails</a></p>", html, escaped_url);
	}
	return g_object_new(VENTURE_TYPE_MAIL_MESSAGE, "organization-id", org,
		"to", recipient, "subject", subject, "text-body", text, "html-body", markup ? body : NULL,
		"private-text-body", footer_text != NULL ? footer_text : private_text,
		"private-html-body", private_html != NULL ? private_html : private_text != NULL ? private_body : NULL,
		"private-unsubscribe-url", url,
		"related-type", "sequence_delivery", "related-id", venture_entity_get_id(delivery),
		"idempotency-key", key, NULL);
}
gboolean venture_sequence_mail_enqueue(VentureDatabase *database, VentureEntity *delivery,
	const VentureActor *actor, GError **error)
{
	SequenceMail *self = state(database);
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	if (choice(delivery, "channel") != 0 || choice(delivery, "state") != 0 ||
		!venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message")) return TRUE;
	message = venture_sequence_mail_build(database, delivery, error);
	if (message == NULL) return FALSE;
	self->enqueuing = delivery;
	queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(database), message, actor, error);
	self->enqueuing = NULL;
	return queued != NULL;
}

gboolean venture_sequence_mail_recover(VentureDatabase *database, gint64 organization, GDateTime *now,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) pending = NULL;
	guint i;
	if (!venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message")) return TRUE;
	query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "pending", NULL);
	venture_query_add_filter_string(query, "channel", VENTURE_FILTER_OP_EQ, "email", NULL);
	pending = venture_database_find(database, query, error);
	if (pending == NULL) return FALSE;
	for (i = 0; i < pending->len; i++) {
		VentureEntity *delivery = g_ptr_array_index(pending, i);
		g_autoptr(VentureQuery) mail_query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(VentureEntity) message = NULL;
		g_autofree gchar *key = delivery_key(delivery);
		venture_query_set_organization(mail_query, organization);
		venture_query_add_filter_string(mail_query, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
		message = venture_database_find_one(database, mail_query, error);
		if (message == NULL && error != NULL && *error != NULL) return FALSE;
		if (message != NULL) {
			if (!settle(state(database), venture_entity_get_id(message), error)) return FALSE;
		} else {
			g_autoptr(GError) eligibility_error = NULL;
			gboolean defer = FALSE;
			if (!venture_sequence_delivery_eligible(database, delivery, now, &defer, &eligibility_error)) {
				if (g_error_matches(eligibility_error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION) ||
					g_error_matches(eligibility_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) continue;
				g_propagate_error(error, g_steal_pointer(&eligibility_error));
				return FALSE;
			}
			if (!defer && !venture_sequence_mail_enqueue(database, delivery, actor, error)) return FALSE;
		}
	}
	return TRUE;
}

gboolean venture_sequence_mail_unsubscribe(VentureDatabase *database, const gchar *token, GError **error)
{
	g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_autoptr(VentureEntity) delivery = NULL, suppression = NULL;
	g_autoptr(VentureQuery) suppressed_query = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *email = NULL;
	gsize i;
	if (token == NULL || strlen(token) != 64) goto missing;
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(token[i])) goto missing;
	venture_query_add_filter_string(query, "unsubscribe-token", VENTURE_FILTER_OP_EQ, token, NULL);
	delivery = venture_database_find_one(database, query, error);
	if (delivery == NULL) {
		if (error != NULL && *error != NULL) return FALSE;
		goto missing;
	}
	if (!venture_database_begin(database, error)) return FALSE;
	email = string(delivery, "unsubscribe-email");
	if (email == NULL || *email == '\0') goto fail;
	suppressed_query = venture_query_new(VENTURE_TYPE_SUPPRESSION);
	venture_query_set_organization(suppressed_query, venture_entity_get_organization_id(delivery));
	venture_query_add_filter_string(suppressed_query, "email", VENTURE_FILTER_OP_EQ, email, NULL);
	suppression = venture_database_find_one(database, suppressed_query, error);
	if (error != NULL && *error != NULL) goto fail;
	if (suppression == NULL) {
		suppression = g_object_new(VENTURE_TYPE_SUPPRESSION, "organization-id", venture_entity_get_organization_id(delivery),
			"email", email, "reason", 0, "at", now, NULL);
		if (!venture_database_save(database, suppression, NULL, error)) goto fail;
	}
	return venture_database_commit(database, error);
fail:
	venture_database_rollback(database);
	return FALSE;
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Unknown sequence unsubscribe capability");
	return FALSE;
}
