/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

typedef struct {
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	VentureLogMailer *mailer;
	VentureEntity *contact, *sequence, *enrollment;
	GDateTime *now;
	gchar *directory, *uri;
} Fixture;

static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, row, NULL, &error));
	g_assert_no_error(error);
}

static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	guint i;
	(void)unused;
	f->config = venture_config_new();
	if (unused == GINT_TO_POINTER(2)) {
		f->directory = g_dir_make_tmp("venture-sequence-mail-XXXXXX", &error);
		g_assert_no_error(error);
		f->uri = g_strdup_printf("sqlite://%s/state.db", f->directory);
	}
	f->db = venture_database_new(f->uri != NULL ? f->uri : "sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->mailer = venture_log_mailer_new();
	venture_context_set_mailer(f->context, VENTURE_MAILER(f->mailer));
	f->now = g_date_time_new_now_utc();
	f->contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", (gint64)1,
		"name", "Example Reader", "email", "reader+sequence@example.test", NULL);
	save(f, f->contact);
	f->sequence = g_object_new(VENTURE_TYPE_SEQUENCE, "organization-id", (gint64)1,
		"name", "Requested information", "active", TRUE, "timezone", "UTC",
		"send-window-start", (gint64)0, "send-window-end", (gint64)24,
		"weekdays", "1,2,3,4,5,6,7", "goal", 1, "exit-on-reply", TRUE, NULL);
	save(f, f->sequence);
	for (i = 1; i <= 2; i++) {
		g_autoptr(VentureEntity) step = g_object_new(VENTURE_TYPE_SEQUENCE_STEP,
			"organization-id", (gint64)1, "sequence-id", venture_entity_get_id(f->sequence),
			"position", (gint64)i, "active", TRUE, "delay-hours", (gint64)(i - 1),
			"subject", "Hello {contact.name}", "body", "Requested information", NULL);
		save(f, step);
	}
	f->enrollment = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, "organization-id", (gint64)1,
		"sequence-id", venture_entity_get_id(f->sequence), "contact-id", venture_entity_get_id(f->contact),
		"enrolled-at", f->now, "enrollment-reason", "Requested information", NULL);
	save(f, f->enrollment);
}

static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->enrollment);
	g_clear_object(&f->sequence);
	g_clear_object(&f->contact);
	g_clear_pointer(&f->now, g_date_time_unref);
	g_clear_object(&f->mailer);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	if (f->directory != NULL) venture_test_remove_tree(f->directory);
	g_free(f->directory);
	g_free(f->uri);
}

static GPtrArray *messages(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GError) error = NULL;
	GPtrArray *found;
	venture_query_set_organization(query, 1);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	found = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return found;
}

static void test_outbox(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) queued = NULL, repeated = NULL;
	g_autofree gchar *to = NULL, *related = NULL, *state = NULL;
	gint64 delivery_id = 0;
	g_autoptr(VentureEntity) delivery = NULL;
	VentureSequenceService *service = venture_sequence_service_get(f->db);
	(void)unused;
	g_assert_cmpint(venture_sequence_service_run_due(service, 1, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	queued = messages(f);
	g_assert_cmpuint(queued->len, ==, 1);
	g_object_get(g_ptr_array_index(queued, 0), "to", &to, "related-type", &related, "related-id", &delivery_id, NULL);
	g_assert_cmpstr(to, ==, "reader+sequence@example.test");
	g_assert_cmpstr(related, ==, "sequence_delivery");
	g_assert_cmpint(delivery_id, >, 0);
	g_assert_cmpint(venture_sequence_service_run_due(service, 1, f->now, NULL, &error), ==, 0);
	g_assert_no_error(error);
	repeated = messages(f);
	g_assert_cmpuint(repeated->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(queued, 0)), ==,
		venture_entity_get_id(g_ptr_array_index(repeated, 0)));
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context), 1, 10, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	delivery = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_DELIVERY, delivery_id, &error);
	g_assert_no_error(error);
	{
		gint sent = 0;
		g_object_get(delivery, "state", &sent, "external-message-id", &state, NULL);
		g_assert_cmpint(sent, ==, 1);
		g_assert_nonnull(state);
		g_assert_cmpstr(state, !=, "");
	}
}

static void reply(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-sequence-reply-XXXXXX", &error);
	g_autofree gchar *secret = g_uuid_string_random();
	g_autofree gchar *identity = g_uuid_string_random();
	g_autoptr(GBytes) key = g_bytes_new(secret, 32);
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureFakeImapClient) client = venture_fake_imap_client_new();
	g_autoptr(VentureMailSyncService) sync = venture_mail_sync_service_new(f->db, VENTURE_IMAP_CLIENT(client));
	g_autoptr(GPtrArray) queued = messages(f);
	g_autofree gchar *message_id = NULL, *raw = NULL;
	g_assert_no_error(error);
	g_object_get(g_ptr_array_index(queued, 0), "message-id", &message_id, NULL);
	g_object_set(f->config, "imap-allowed-endpoints", "imap.example.test:993", NULL);
	g_object_set(sync, "config", f->config, "attachment-root", directory, NULL);
	account = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", (gint64)1,
		"address", "sender@example.test", "imap-host", "imap.example.test", "imap-port", (gint64)993,
		"imap-tls", "tls", "username", "sender@example.test", "folders", "INBOX", "active", TRUE, NULL);
	save(f, account);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	json_object_set_string_member(values, "password", secret);
	binding = venture_connector_configure(f->db, f->config, account, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(binding);
	raw = g_strdup_printf("From: reader+sequence@example.test\r\nTo: sender@example.test\r\n"
		"Message-ID: <%s@example.test>\r\nIn-Reply-To: <%s>\r\nSubject: Re: Requested information\r\n"
		"Content-Type: text/plain; charset=utf-8\r\n\r\nThank you, please stop the follow-up.\r\n", identity, message_id);
	venture_fake_imap_client_add_message(client, "INBOX", 1, raw);
	g_assert_cmpint(venture_mail_sync_service_sync(sync, account, NULL, &error), ==, 1);
	g_assert_no_error(error);
	venture_test_remove_tree(directory);
}

static void test_reply(Fixture *f, gconstpointer final_step)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) later = g_date_time_add_hours(f->now, 2);
	g_autoptr(GDateTime) last = g_date_time_add_hours(f->now, 4);
	g_autoptr(VentureEntity) current = NULL;
	gint status = 0;
	VentureSequenceService *service = venture_sequence_service_get(f->db);
	VentureMailOutbox *outbox = venture_context_get_mail_outbox(f->context);
	g_assert_cmpint(venture_sequence_service_run_due(service, 1, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	if (final_step != NULL) {
		g_assert_cmpint(venture_sequence_service_run_due(service, 1, later, NULL, &error), ==, 1);
		g_assert_no_error(error);
	}
	reply(f);
	current = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_ENROLLMENT,
		venture_entity_get_id(f->enrollment), &error);
	g_assert_no_error(error);
	g_object_get(current, "status", &status, NULL);
	g_assert_cmpint(status, ==, 3);
	g_assert_cmpint(venture_sequence_service_run_due(service, 1, last, NULL, &error), ==, 0);
	g_assert_no_error(error);
	/* A cancelled claim may count as an attempt, but cannot reach transport. */
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, last, NULL, &error), >=, 0);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}

static void run(Fixture *f, gint expected)
{
	g_autoptr(GError) error = NULL;
	g_assert_cmpint(venture_sequence_service_run_due(venture_sequence_service_get(f->db), 1, f->now, NULL, &error), ==, expected);
	g_assert_no_error(error);
}

static void restart(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	f->db = venture_database_new(f->uri, &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	venture_context_set_mailer(f->context, VENTURE_MAILER(f->mailer));
}

static void test_restart(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) before = NULL, after = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first = NULL, *second = NULL;
	(void)unused;
	run(f, 1);
	before = messages(f);
	g_assert_cmpuint(before->len, ==, 1);
	g_object_get(g_ptr_array_index(before, 0), "message-id", &first, NULL);
	restart(f);
	run(f, 0);
	after = messages(f);
	g_assert_cmpuint(after->len, ==, 1);
	g_object_get(g_ptr_array_index(after, 0), "message-id", &second, NULL);
	g_assert_cmpstr(first, ==, second);
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context), 1, 10, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	restart(f);
	run(f, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context), 1, 10, f->now, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}

static void test_mail_disabled(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) queued = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_autoptr(VentureEntity) pending = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	venture_config_set_module_enabled(f->config, "marketing", FALSE);
	venture_config_set_module_enabled(f->config, "recurring", FALSE);
	venture_config_set_module_enabled(f->config, "mail_sync", FALSE);
	venture_config_set_module_enabled(f->config, "dunning", FALSE);
	venture_config_set_module_enabled(f->config, "mail", FALSE);
	restart(f);
	run(f, 1);
	pending = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pending);
	venture_config_set_module_enabled(f->config, "mail", TRUE);
	restart(f);
	run(f, 0);
	queued = messages(f);
	g_assert_cmpuint(queued->len, ==, 1);
}

static void test_pause(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureSequenceService *service = venture_sequence_service_get(f->db);
	VentureMailOutbox *outbox = venture_context_get_mail_outbox(f->context);
	(void)unused;
	run(f, 1);
	g_assert_true(venture_sequence_service_transition(service, 1, venture_entity_get_id(f->enrollment), "pause", "Pause", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, f->now, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	g_assert_true(venture_sequence_service_transition(service, 1, venture_entity_get_id(f->enrollment), "resume", "Resume", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}

static gboolean reply_at_transport(VentureMailOutbox *outbox, VentureMailMessage *message, GError **error, gpointer data)
{
	(void)outbox; (void)message; (void)error;
	reply(data);
	return FALSE;
}

static void test_final_gate(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureMailOutbox *outbox = venture_context_get_mail_outbox(f->context);
	gulong handler;
	(void)unused;
	run(f, 1);
	handler = g_signal_connect(outbox, "before-send", G_CALLBACK(reply_at_transport), f);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, 1, 10, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_signal_handler_disconnect(outbox, handler);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
}

static void test_projection_failure(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) queued = NULL;
	g_autoptr(VentureEntity) delivery = NULL;
	g_autofree gchar *status = NULL;
	gint64 id = 0;
	gint sent = 0;
	(void)unused;
	run(f, 1);
	g_assert_true(venture_database_execute(f->db,
		"CREATE TRIGGER reject_sequence_result BEFORE UPDATE ON sequence_deliveries WHEN NEW.state = 'sent' "
		"BEGIN SELECT RAISE(ABORT, 'result unavailable'); END", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context), 1, 10, f->now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	queued = messages(f);
	g_object_get(g_ptr_array_index(queued, 0), "state", &status, "related-id", &id, NULL);
	g_assert_cmpstr(status, ==, "sent");
	delivery = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_DELIVERY, id, &error);
	g_assert_no_error(error);
	g_object_get(delivery, "state", &sent, NULL);
	g_assert_cmpint(sent, ==, 0);
	g_clear_object(&delivery);
	g_assert_true(venture_database_execute(f->db, "DROP TRIGGER reject_sequence_result", NULL, &error));
	g_assert_no_error(error);
	restart(f);
	run(f, 0);
	delivery = venture_database_get(f->db, VENTURE_TYPE_SEQUENCE_DELIVERY, id, &error);
	g_assert_no_error(error);
	g_object_get(delivery, "state", &sent, NULL);
	g_assert_cmpint(sent, ==, 1);
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_context_get_mail_outbox(f->context), 1, 10, f->now, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
}

static void test_atomic_enqueue(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_autoptr(GPtrArray) deliveries = NULL;
	(void)unused;
	g_assert_true(venture_database_execute(f->db,
		"CREATE TRIGGER reject_sequence_mail BEFORE INSERT ON mail_messages "
		"BEGIN SELECT RAISE(ABORT, 'outbox unavailable'); END", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_sequence_service_run_due(venture_sequence_service_get(f->db), 1, f->now, NULL, &error), ==, -1);
	g_assert_nonnull(error);
	g_clear_error(&error);
	deliveries = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(deliveries->len, ==, 0);
	g_assert_true(venture_database_execute(f->db, "DROP TRIGGER reject_sequence_mail", NULL, &error));
	g_assert_no_error(error);
	run(f, 1);
}

static void test_forged_source(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMailMessage) forged = g_object_new(VENTURE_TYPE_MAIL_MESSAGE,
		"organization-id", (gint64)1, "to", "reader@example.test", "subject", "Example",
		"text-body", "Example", "related-type", "sequence_delivery", "related-id", (gint64)1, NULL);
	g_autoptr(VentureMailMessage) queued = NULL;
	(void)unused;
	queued = venture_mail_outbox_enqueue(venture_context_get_mail_outbox(f->context), forged, NULL, &error);
	g_assert_null(queued);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void test_private_body(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) form = g_object_new(VENTURE_TYPE_FORM, "organization-id", (gint64)1,
		"name", "Survey", "state", VENTURE_FORM_LIVE, NULL);
	g_autoptr(VentureEntity) field = NULL, version = NULL, step = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_STEP);
	g_autoptr(GPtrArray) queued = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autofree gchar *private_body = NULL, *public_json = NULL;
	(void)unused;
	save(f, form);
	field = g_object_new(VENTURE_TYPE_FORM_FIELD, "organization-id", (gint64)1,
		"form-id", venture_entity_get_id(form), "key", "email", "label", "Email",
		"kind", VENTURE_FORM_FIELD_EMAIL, "contact-field", "email", NULL);
	save(f, field);
	version = venture_forms_publish(f->db, form, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(version);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	step = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_object_set(step, "survey-form-id", venture_entity_get_id(form), NULL);
	save(f, step);
	g_object_set(venture_sequence_service_get(f->db), "base-url", "https://forms.example.test", NULL);
	run(f, 1);
	queued = messages(f);
	g_object_get(g_ptr_array_index(queued, 0), "private-html-body", &private_body, NULL);
	g_assert_nonnull(strstr(private_body, "personal="));
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(queued, 0)), FALSE);
	public_json = json_to_string(json, FALSE);
	g_assert_null(strstr(public_json, "personal="));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/sequence-mail/outbox", Fixture, NULL, setup, test_outbox, teardown);
	g_test_add("/sequence-mail/inbound-reply", Fixture, NULL, setup, test_reply, teardown);
	g_test_add("/sequence-mail/reply-before-final-send", Fixture, GINT_TO_POINTER(1), setup, test_reply, teardown);
	g_test_add("/sequence-mail/restart", Fixture, GINT_TO_POINTER(2), setup, test_restart, teardown);
	g_test_add("/sequence-mail/enable-mail", Fixture, GINT_TO_POINTER(2), setup, test_mail_disabled, teardown);
	g_test_add("/sequence-mail/pause", Fixture, NULL, setup, test_pause, teardown);
	g_test_add("/sequence-mail/final-gate", Fixture, NULL, setup, test_final_gate, teardown);
	g_test_add("/sequence-mail/retained-acceptance", Fixture, GINT_TO_POINTER(2), setup, test_projection_failure, teardown);
	g_test_add("/sequence-mail/atomic-enqueue", Fixture, NULL, setup, test_atomic_enqueue, teardown);
	g_test_add("/sequence-mail/forged-source", Fixture, NULL, setup, test_forged_source, teardown);
	g_test_add("/sequence-mail/private-body", Fixture, NULL, setup, test_private_body, teardown);
	return g_test_run();
}
