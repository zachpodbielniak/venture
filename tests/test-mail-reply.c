/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Replying to a synced message from the contact timeline (docs/mail.org,
 * Replying from the timeline): the reply threads to what it answers, goes
 * through the one outbox, and shows on the timeline as outbound once the
 * relay has it. No mail leaves the machine: delivery is the log mailer and
 * a loopback SMTP listener in a thread. */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureFakeImapClient *imap;
	VentureMailSyncService *service;
	gchar *root;
	gint64 org;
	gint64 contact;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static const gchar *msg_question =
	"From: Ada <Ada@Example.com>\r\nTo: ops@example.net\r\nSubject: Spring order\r\n"
	"Date: Tue, 15 Sep 2026 09:00:00 +0000\r\nMessage-ID: <m1@example.net>\r\n"
	"In-Reply-To: <root@example.net>\r\nReferences: <root@example.net>\r\n"
	"Content-Type: text/plain\r\n\r\nCan you deliver in March?\r\n";
static const gchar *msg_booking =
	"From: Ada <Ada@Example.com>\r\nTo: r-0123456789abcdef01234567+42@lightsite.app\r\nSubject: Spring order\r\n"
	"Date: Tue, 15 Sep 2026 09:00:00 +0000\r\nMessage-ID: <m1@example.net>\r\n"
	"X-Original-To: r-0123456789abcdef01234567+42@lightsite.app\r\n"
	"Content-Type: text/plain\r\n\r\nCan you deliver in March?\r\n";

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(VentureEntity) contact = NULL, account = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	f->root = g_dir_make_tmp("venture-mail-reply-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->root, "imap-allowed-endpoints", "imap.example.net:993",
		"mail-from-address", "ops@example.net", NULL);
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Ada", "email", "ada@example.com", NULL);
	save(f, contact);
	f->contact = venture_entity_get_id(contact);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
	account = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org,
		"address", "ops@example.net", "imap-host", "imap.example.net", "imap-port", (gint64)993, "imap-tls", "tls",
		"username", "ops@example.net", "folders", "INBOX", "active", TRUE, NULL);
	if (data) g_object_set(account, "reply-alias-prefix", "r-0123456789abcdef01234567+", NULL);
	save(f, account);
	json_object_set_string_member(values, "password", "app-password");
	binding = venture_connector_configure(f->db, f->config, account, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	f->imap = venture_fake_imap_client_new();
	f->service = venture_mail_sync_service_new(f->db, VENTURE_IMAP_CLIENT(f->imap));
	g_object_set(f->service, "attachment-root", f->root, "config", f->config, NULL);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, data ? msg_booking : msg_question);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, account, NULL, &error), ==, 1);
	g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->service);
	g_clear_object(&f->imap);
	g_clear_object(&f->context);
	g_clear_object(&f->config);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	venture_test_remove_tree(f->root);
	g_free(f->root);
}

static GPtrArray *
contact_mail(Fixture *f)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INTERACTION);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_int(q, "contact-id", VENTURE_FILTER_OP_EQ, f->contact, NULL);
	venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, q, &error);
	g_assert_no_error(error);
	return rows;
}

static VentureActor
person(void)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "ops";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

/* The reply answers the message it was written under: In-Reply-To names
 * it, References carries the thread root and it, the subject gains one
 * "Re:", and it goes to the sender as filed. If the headers were dropped
 * the customer's client would start a new conversation for every reply. */
static void
test_reply_threads(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) before = contact_mail(f);
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureMailMessage) reply = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *to = NULL, *subject = NULL, *in_reply_to = NULL, *references = NULL, *related = NULL, *body = NULL;
	VentureActor actor = person();
	VentureEntity *question;
	gint64 related_id = 0;
	(void)data;
	g_assert_cmpuint(before->len, ==, 1);
	question = g_ptr_array_index(before, 0);
	source = venture_mail_reply_source(f->db, question);
	g_assert_nonnull(source);
	reply = venture_mail_reply_enqueue(f->context, question, "Yes, the first week of March.", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reply);
	g_object_get(reply, "to", &to, "subject", &subject, "in-reply-to", &in_reply_to, "reply-references", &references,
		"related-type", &related, "related-id", &related_id, "text-body", &body, NULL);
	g_assert_cmpstr(to, ==, "ada@example.com");
	g_assert_cmpstr(subject, ==, "Re: Spring order");
	g_assert_cmpstr(in_reply_to, ==, "<m1@example.net>");
	g_assert_cmpstr(references, ==, "<root@example.net> <m1@example.net>");
	g_assert_cmpstr(related, ==, "interaction");
	g_assert_cmpint(related_id, ==, venture_entity_get_id(question));
	g_assert_cmpstr(body, ==, "Yes, the first week of March.");
}

/* A Lightsite alias remains the customer's Reply-To after an office answer,
 * so their next message returns to the same business and Venture thread. */
static void
test_reply_keeps_booking_alias(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) before = contact_mail(f);
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureMailMessage) reply = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reply_to = NULL;
	VentureActor actor = person();
	(void)data;
	source = venture_mail_reply_source(f->db, g_ptr_array_index(before, 0));
	g_assert_nonnull(source);
	reply = venture_mail_reply_enqueue(f->context, g_ptr_array_index(before, 0), "See you then.", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reply);
	g_object_get(reply, "reply-to", &reply_to, NULL);
	g_assert_cmpstr(reply_to, ==, "r-0123456789abcdef01234567+42@lightsite.app");
}

/* Delivery is the ordinary sweep: once the relay has the reply it is on
 * the contact's timeline as outbound, and a second sweep adds nothing. */
static void
test_reply_on_timeline(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) before = contact_mail(f);
	g_autoptr(GPtrArray) after = NULL;
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(VentureMailMessage) reply = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *subject = NULL;
	VentureActor actor = person();
	gboolean outbound = FALSE;
	(void)data;
	reply = venture_mail_reply_enqueue(f->context, g_ptr_array_index(before, 0), "Booked.", &actor, &error);
	g_assert_no_error(error);
	{ g_autoptr(GPtrArray) queued = contact_mail(f); g_assert_cmpuint(queued->len, ==, 1); }
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	after = contact_mail(f);
	g_assert_cmpuint(after->len, ==, 2);
	g_object_get(g_ptr_array_index(after, 1), "outbound", &outbound, "subject", &subject, NULL);
	g_assert_true(outbound);
	g_assert_cmpstr(subject, ==, "Re: Spring order");
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 0);
	g_ptr_array_unref(after);
	after = contact_mail(f);
	g_assert_cmpuint(after->len, ==, 2);
}

/* A reply is refused for what cannot be answered: an empty or oversized
 * body, a note that never came by mail, and a message the timeline sent
 * itself is answered by replying to the customer's message instead. */
static void
test_reply_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) before = contact_mail(f);
	g_autoptr(VentureEntity) note = VENTURE_ENTITY(venture_interaction_new());
	g_autoptr(VentureMailMessage) reply = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *large = g_strnfill(VENTURE_MAIL_REPLY_MAX_BODY + 1, 'x');
	VentureActor actor = person();
	(void)data;
	reply = venture_mail_reply_enqueue(f->context, g_ptr_array_index(before, 0), "   ", &actor, &error);
	g_assert_null(reply);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	reply = venture_mail_reply_enqueue(f->context, g_ptr_array_index(before, 0), large, &actor, &error);
	g_assert_null(reply);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(note, "organization-id", f->org, "contact-id", f->contact, "kind", VENTURE_INTERACTION_KIND_NOTE,
		"subject", "Met at the fair", NULL);
	save(f, note);
	g_assert_null(venture_mail_reply_source(f->db, note));
	reply = venture_mail_reply_enqueue(f->context, note, "Hello", &actor, &error);
	g_assert_null(reply);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

typedef struct { GSocketListener *listener; gchar *wire; } SmtpWire;

static gpointer
smtp_once(gpointer data)
{
	SmtpWire *w = data;
	g_autoptr(GSocketConnection) connection = g_socket_listener_accept(w->listener, NULL, NULL, NULL);
	g_autoptr(GDataInputStream) input = NULL;
	GOutputStream *output;
	GString *body = g_string_new(NULL);
	gboolean in_body = FALSE;
	g_assert_nonnull(connection);
	g_socket_set_timeout(g_socket_connection_get_socket(connection), 10);
	input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
	g_data_input_stream_set_newline_type(input, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
	output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
	g_assert_true(g_output_stream_write_all(output, "220 local test\r\n", 16, NULL, NULL, NULL));
	for (;;) {
		g_autofree gchar *line = g_data_input_stream_read_line(input, NULL, NULL, NULL);
		const gchar *answer = "250 OK\r\n";
		if (line == NULL) break;
		if (in_body) {
			if (!strcmp(line, ".")) in_body = FALSE;
			else { g_string_append_printf(body, "%s\n", line); continue; }
		} else if (g_str_has_prefix(line, "EHLO")) answer = "250 local test\r\n";
		else if (!strcmp(line, "DATA")) { answer = "354 Send data\r\n"; in_body = TRUE; }
		else if (!strcmp(line, "QUIT")) { answer = "221 bye\r\n"; g_output_stream_write_all(output, answer, strlen(answer), NULL, NULL, NULL); break; }
		g_assert_true(g_output_stream_write_all(output, answer, strlen(answer), NULL, NULL, NULL));
	}
	w->wire = g_string_free(body, FALSE);
	g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	return NULL;
}

/* On the wire the threading headers are real headers, and a value that
 * would add a header of its own is refused before anything is sent. */
static void
test_reply_wire_headers(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) before = contact_mail(f);
	g_autoptr(VentureMailMessage) reply = NULL;
	g_autoptr(VentureSmtpMailer) mailer = NULL;
	g_autoptr(VentureConfig) relay = venture_config_new();
	g_autoptr(VentureMailMessage) hostile = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	VentureActor actor = person();
	SmtpWire wire = { NULL, NULL };
	GThread *thread;
	guint16 port;
	(void)data;
	reply = venture_mail_reply_enqueue(f->context, g_ptr_array_index(before, 0), "See you then.", &actor, &error);
	g_assert_no_error(error);
	g_object_set(reply, "message-id", "reply-1@example.net", NULL);
	wire.listener = g_socket_listener_new();
	port = g_socket_listener_add_any_inet_port(wire.listener, NULL, &error);
	g_assert_no_error(error);
	g_object_set(relay, "mail-host", "127.0.0.1", "mail-port", (gint64)port, "mail-security", "none",
		"mail-from-address", "ops@example.net", NULL);
	mailer = venture_smtp_mailer_new(relay);
	thread = g_thread_new("reply-smtp", smtp_once, &wire);
	g_assert_true(venture_mailer_send(VENTURE_MAILER(mailer), reply, NULL, &error));
	g_assert_no_error(error);
	g_thread_join(thread);
	g_assert_nonnull(strstr(wire.wire, "In-Reply-To: <m1@example.net>"));
	g_assert_nonnull(strstr(wire.wire, "References: <root@example.net> <m1@example.net>"));
	g_assert_nonnull(strstr(wire.wire, "Subject: Re: Spring order"));
	g_free(wire.wire);
	g_object_unref(wire.listener);
	g_object_set(hostile, "to", "ada@example.com", "subject", "Re: x", "text-body", "x", "message-id", "h@example.net",
		"in-reply-to", "<m1@example.net>\r\nBcc: someone@example.org", NULL);
	g_assert_false(venture_mailer_send(VENTURE_MAILER(mailer), hostile, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_MAIL_PERMANENT);
}

typedef struct { gboolean done; GBytes *body; GError *error; } WebCall;

static void
web_done(GObject *source, GAsyncResult *result, gpointer data)
{
	WebCall *call = data;
	call->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &call->error);
	call->done = TRUE;
}

/* One request to a server on this thread's context, driven by hand. */
static guint
web_request(VentureWebServer *server, const gchar *method, const gchar *path, const gchar *form, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", venture_web_server_get_port(server), path);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	WebCall call = { FALSE, NULL, NULL };
	guint status;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (form != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, web_done, &call);
	while (!call.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(call.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(call.body, NULL), g_bytes_get_size(call.body));
	status = soup_message_get_status(message);
	g_bytes_unref(call.body);
	return status;
}

static VentureWebServer *
web_start(VentureConfig *config, VentureContext *context)
{
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	g_object_set(config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

/* The contact's page lists its mail as a timeline with a reply composer
 * under a synced message; posting it queues the threaded reply and comes
 * back to the contact. */
static void
test_reply_composer(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) before = contact_mail(f);
	g_autoptr(VentureWebServer) server = web_start(f->config, f->context);
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(VentureEntity) queued = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL, *path = NULL, *action = NULL, *in_reply_to = NULL;
	(void)data;
	path = g_strdup_printf("/e/contact/%" G_GINT64_FORMAT, f->contact);
	action = g_strdup_printf("/interactions/%" G_GINT64_FORMAT "/reply", venture_entity_get_id(g_ptr_array_index(before, 0)));
	g_assert_cmpuint(web_request(server, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, action));
	g_assert_nonnull(strstr(page, "Re: Spring order"));
	g_assert_nonnull(strstr(page, "Can you deliver in March?"));
	g_assert_cmpuint(web_request(server, "POST", action, "body=Yes%2C+March+works.", NULL), ==, 302);
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_string(q, "related-type", VENTURE_FILTER_OP_EQ, "interaction", NULL);
	queued = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	g_assert_nonnull(queued);
	g_object_get(queued, "in-reply-to", &in_reply_to, NULL);
	g_assert_cmpstr(in_reply_to, ==, "<m1@example.net>");
	venture_web_server_stop(server);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mail-reply/threads", Fixture, NULL, setup, test_reply_threads, teardown);
	g_test_add("/mail-reply/booking-alias", Fixture, GINT_TO_POINTER(1), setup, test_reply_keeps_booking_alias, teardown);
	g_test_add("/mail-reply/on-timeline", Fixture, NULL, setup, test_reply_on_timeline, teardown);
	g_test_add("/mail-reply/refusals", Fixture, NULL, setup, test_reply_refusals, teardown);
	g_test_add("/mail-reply/wire-headers", Fixture, NULL, setup, test_reply_wire_headers, teardown);
	g_test_add("/mail-reply/composer", Fixture, NULL, setup, test_reply_composer, teardown);
	return g_test_run();
}
