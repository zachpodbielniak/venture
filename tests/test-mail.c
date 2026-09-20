/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

/* A mail record must participate in every generated surface. */
static void
test_mail_registered(void)
{
	VentureEntityRegistry *registry;
	registry = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "mail_message"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "mail_template"), !=, G_TYPE_INVALID);
}

typedef struct {
	VentureDatabase *db;
	VentureLogMailer *mailer;
	VentureMailOutbox *outbox;
	gint64 org;
} Fixture;
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) org = venture_database_find_one(f->db, query, &error);
		g_assert_no_error(error);
		f->org = venture_entity_get_id(org);
	}
	f->mailer = venture_log_mailer_new();
	f->outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(f->mailer));
}
static void teardown(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->outbox);
	g_clear_object(&f->mailer);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
}
static VentureMailMessage *enqueue(Fixture *f, const gchar *key)
{
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	VentureMailMessage *row;
	g_object_set(message, "organization-id", f->org, "to", "reader@example.test",
		"subject", "Receipt", "text-body", "Hello", "idempotency-key", key, NULL);
	row = venture_mail_outbox_enqueue(f->outbox, message, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	return row;
}
static void assert_state(Fixture *f, gint64 id, const gchar *expected)
{
	g_autoptr(VentureEntity) row = venture_database_get(f->db, VENTURE_TYPE_MAIL_MESSAGE, id, NULL);
	g_autofree gchar *state = NULL;
	g_object_get(row, "state", &state, NULL);
	g_assert_cmpstr(state, ==, expected);
}
static void test_uncertain(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-1");
	g_autoptr(VentureMailMessage) duplicate = enqueue(f, "receipt-1");
	g_autoptr(GError) error = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_UNCERTAIN, "Reply lost");
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(row));
	g_assert_cmpint(id, ==, venture_entity_get_id(VENTURE_ENTITY(duplicate)));
	venture_log_mailer_set_error(f->mailer, error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, NULL), ==, 1);
	assert_state(f, id, "uncertain");
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, NULL), ==, 0);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 1);
	venture_log_mailer_set_error(f->mailer, NULL);
	g_assert_true(venture_mail_outbox_retry(f->outbox, f->org, id, NULL, NULL));
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, NULL), ==, 1);
	assert_state(f, id, "sent");
	{
		const GPtrArray *messages = venture_log_mailer_get_messages(f->mailer);
		g_autofree gchar *first = NULL;
		g_autofree gchar *second = NULL;
		g_object_get(g_ptr_array_index((GPtrArray *)messages, 0), "message-id", &first, NULL);
		g_object_get(g_ptr_array_index((GPtrArray *)messages, 1), "message-id", &second, NULL);
		g_assert_nonnull(first);
		g_assert_cmpstr(first, ==, second);
	}
}
static void test_backoff(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-2");
	g_autoptr(GError) error = g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_MAIL_TRANSIENT, "Connection refused");
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) later = g_date_time_add_seconds(now, 120);
	gint64 id = venture_entity_get_id(VENTURE_ENTITY(row));
	g_object_set(f->outbox, "max-attempts", 2u, NULL);
	venture_log_mailer_set_error(f->mailer, error);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, now, NULL, NULL), ==, 1);
	assert_state(f, id, "failed");
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, now, NULL, NULL), ==, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, later, NULL, NULL), ==, 1);
	assert_state(f, id, "dead");
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, later, NULL, NULL), ==, 0);
}
static void test_lease(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-3");
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) later = g_date_time_add_seconds(now, 601);
	g_autoptr(VentureMailMessage) claimed = venture_mail_outbox_claim(f->outbox, f->org, venture_entity_get_id(VENTURE_ENTITY(row)), now, NULL);
	g_assert_nonnull(claimed);
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, later, NULL, NULL), ==, 0);
	assert_state(f, venture_entity_get_id(VENTURE_ENTITY(row)), "uncertain");
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
}
static void test_refuse_generic(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "receipt-4");
	g_autoptr(GError) error = NULL;
	g_object_set(row, "state", "sent", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	assert_state(f, venture_entity_get_id(VENTURE_ENTITY(row)), "queued");
}
/* A durable invitation must reach SMTP without leaking through record APIs. */
static void test_private_body(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) input = venture_mail_message_new();
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) public = NULL;
	g_autofree gchar *body = NULL, *serialized = NULL;
	g_object_set(input, "organization-id", f->org, "to", "reader@example.test",
		"subject", "Invitation", "text-body", "Private invitation",
		"private-text-body", "https://example.test/portal/secret-token", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(input), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	queued = venture_mail_outbox_enqueue(f->outbox, input, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(queued);
	stored = venture_database_get(f->db, VENTURE_TYPE_MAIL_MESSAGE,
		venture_entity_get_id(VENTURE_ENTITY(queued)), &error);
	g_assert_no_error(error);
	g_object_get(stored, "private-text-body", &body, NULL);
	g_assert_cmpstr(body, ==, "https://example.test/portal/secret-token");
	public = venture_serializable_to_json(VENTURE_SERIALIZABLE(stored), FALSE);
	serialized = json_to_string(public, FALSE);
	g_assert_null(strstr(serialized, "secret-token"));
	g_assert_null(strstr(serialized, "private_text_body"));
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 1, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_clear_pointer(&body, g_free);
	g_object_get(g_ptr_array_index((GPtrArray *)venture_log_mailer_get_messages(f->mailer), 0),
		"private-text-body", &body, NULL);
	g_assert_cmpstr(body, ==, "https://example.test/portal/secret-token");
}
static void test_template(void)
{
	g_autoptr(VentureMailTemplate) t = venture_mail_template_new();
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *html = NULL;
	g_object_set(company, "name", "A & B", NULL);
	g_object_set(t, "subject", "Hello {name}", "text-body", "Dear {name}", "html-body", "<p>{name}</p>", NULL);
	message = venture_mail_template_render(t, VENTURE_ENTITY(company), &error);
	g_assert_no_error(error);
	g_assert_nonnull(message);
	g_object_get(message, "html-body", &html, NULL);
	g_assert_cmpstr(html, ==, "<p>A &amp; B</p>");
}

static void test_smtp_configuration(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureSmtpMailer) mailer = NULL;
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	g_object_set(config, "mail-host", "127.0.0.1", "mail-port", (gint64)1,
		"mail-security", "none", "mail-from-address", "sender@example.test", NULL);
	mailer = venture_smtp_mailer_new(config);
	g_object_set(message, "to", "reader@example.test", "subject", "Test", "text-body", "Hello", "message-id", "stable@example.test", NULL);
	g_assert_false(venture_mailer_send(VENTURE_MAILER(mailer), message, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_MAIL_TRANSIENT);
}

typedef struct { GSocketListener *listener; gchar *bodies[2]; guint recipients; } SmtpFixture;
static gpointer smtp_server(gpointer data)
{
	SmtpFixture *f = data;
	guint n;
	for (n = 0; n < 2; n++) {
		g_autoptr(GSocketConnection) connection = g_socket_listener_accept(f->listener, NULL, NULL, NULL);
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
		while (TRUE) {
			g_autofree gchar *line = g_data_input_stream_read_line(input, NULL, NULL, NULL);
			const gchar *reply = "250 OK\r\n";
			g_assert_nonnull(line);
			if (in_body) {
				if (!strcmp(line, ".")) {
					in_body = FALSE;
					if (n == 0) break;
				} else { g_string_append_printf(body, "%s\n", line); continue; }
			} else if (g_str_has_prefix(line, "EHLO")) reply = "250 local test\r\n";
			else if (g_str_has_prefix(line, "RCPT")) f->recipients++;
			else if (!strcmp(line, "DATA")) { reply = "354 Send data\r\n"; in_body = TRUE; }
			else if (!strcmp(line, "QUIT")) break;
			g_assert_true(g_output_stream_write_all(output, reply, strlen(reply), NULL, NULL, NULL));
		}
		f->bodies[n] = g_string_free(body, FALSE);
		g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	}
	return NULL;
}
static void test_smtp_uncertain_wire(void)
{
	SmtpFixture f;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureSmtpMailer) mailer = NULL;
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	GThread *thread;
	guint16 port;
	f.listener = g_socket_listener_new(); f.bodies[0] = NULL; f.bodies[1] = NULL; f.recipients = 0;
	port = g_socket_listener_add_any_inet_port(f.listener, NULL, &error);
	g_assert_no_error(error);
	g_object_set(config, "mail-host", "127.0.0.1", "mail-port", (gint64)port,
		"mail-security", "none", "mail-from-address", "sender@example.test", NULL);
	mailer = venture_smtp_mailer_new(config);
	g_object_set(message, "to", "reader@example.test", "bcc", "private@example.test", "subject", "Test",
		"text-body", "Hello", "html-body", "<p>Hello</p>",
		"private-text-body", "https://example.test/portal/secret-token", "message-id", "stable@example.test", NULL);
	thread = g_thread_new("local-smtp", smtp_server, &f);
	g_assert_false(venture_mailer_send(VENTURE_MAILER(mailer), message, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_MAIL_UNCERTAIN);
	g_clear_error(&error);
	g_assert_true(venture_mailer_send(VENTURE_MAILER(mailer), message, NULL, &error));
	g_assert_no_error(error);
	g_thread_join(thread);
	g_assert_nonnull(strstr(f.bodies[0], "Message-Id: <stable@example.test>"));
	g_assert_nonnull(strstr(f.bodies[1], "Message-Id: <stable@example.test>"));
	g_assert_null(strstr(f.bodies[0], "Bcc:"));
	g_assert_nonnull(strstr(f.bodies[0], "https://example.test/portal/secret-token"));
	g_assert_null(strstr(f.bodies[0], "<p>Hello</p>"));
	g_assert_cmpuint(f.recipients, ==, 4);
	g_free(f.bodies[0]); g_free(f.bodies[1]); g_object_unref(f.listener);
}

/* A private HTML body replaces the public HTML on the wire. Before it
 * existed the private text body cleared the HTML alternative, so a reminder
 * rendered from an HTML-only template lost its pay link. */
static void test_smtp_private_html_wire(void)
{
	SmtpFixture f;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureSmtpMailer) mailer = NULL;
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	GThread *thread;
	guint16 port;
	f.listener = g_socket_listener_new(); f.bodies[0] = NULL; f.bodies[1] = NULL; f.recipients = 0;
	port = g_socket_listener_add_any_inet_port(f.listener, NULL, &error);
	g_assert_no_error(error);
	g_object_set(config, "mail-host", "127.0.0.1", "mail-port", (gint64)port,
		"mail-security", "none", "mail-from-address", "sender@example.test", NULL);
	mailer = venture_smtp_mailer_new(config);
	g_object_set(message, "to", "reader@example.test", "subject", "Test",
		"text-body", "", "html-body", "<p>publicmarker</p>",
		"private-html-body", "<p>privatemarker</p>", "message-id", "html@example.test", NULL);
	thread = g_thread_new("local-smtp", smtp_server, &f);
	g_assert_false(venture_mailer_send(VENTURE_MAILER(mailer), message, NULL, &error));
	g_clear_error(&error);
	g_assert_true(venture_mailer_send(VENTURE_MAILER(mailer), message, NULL, &error));
	g_assert_no_error(error);
	g_thread_join(thread);
	g_assert_nonnull(strstr(f.bodies[1], "privatemarker"));
	g_assert_null(strstr(f.bodies[1], "publicmarker"));
	g_free(f.bodies[0]); g_free(f.bodies[1]); g_object_unref(f.listener);
}

/* Private HTML is delivery content like private text: only the enqueue
 * service may write it. */
static void test_private_html_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) input = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	g_object_set(input, "organization-id", f->org, "to", "reader@example.test", "subject", "Invitation",
		"text-body", "Private invitation", "private-html-body", "<a href=\"https://example.test/portal/secret\">pay</a>", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(input), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static gboolean reject_mail(VentureDatabase *, VentureEntity *, VentureEntity *, gpointer, GError **);
static void test_invoice_consumer(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VentureMoney) price = venture_money_new(2500, "USD", 2);
	g_autoptr(VentureMailMessage) message = NULL, again = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) events = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
	g_autofree gchar *html = NULL;
	g_object_set(company, "organization-id", f->org, "name", "Customer", "email", "customer@example.test", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(company), NULL, &error));
	g_assert_no_error(error);
	g_object_set(invoice, "organization-id", f->org, "number", "MAIL-42", "terms", "Net 30", "company-id", venture_entity_get_id(VENTURE_ENTITY(company)), NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_assert_no_error(error);
	g_object_set(line, "organization-id", f->org, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "description", "Consultation", "quantity", 1.0, "unit-price", price, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error));
	g_assert_no_error(error);
	if (data) venture_database_add_save_validator(f->db, VENTURE_TYPE_MAIL_MESSAGE, reject_mail, NULL, NULL);
	message = venture_mail_send_invoice(context, f->org, venture_entity_get_id(VENTURE_ENTITY(invoice)), NULL, &error);
	if (data) {
		g_autoptr(VentureEntity) persisted = NULL;
		g_autoptr(VentureQuery) messages = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		gint status;
		g_assert_null(message);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
		persisted = venture_database_get(f->db, VENTURE_TYPE_INVOICE, venture_entity_get_id(VENTURE_ENTITY(invoice)), &error);
		g_assert_no_error(error);
		g_object_get(persisted, "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_DRAFT);
		venture_query_set_organization(events, f->org);
		venture_query_set_organization(messages, f->org);
		g_assert_cmpint(venture_database_count(f->db, events, &error), ==, 0);
		g_assert_no_error(error);
		g_assert_cmpint(venture_database_count(f->db, messages, &error), ==, 0);
		g_assert_no_error(error);
		return;
	}
	g_assert_no_error(error); g_assert_nonnull(message);
	g_object_get(message, "html-body", &html, NULL);
	g_assert_nonnull(strstr(html, "Consultation")); g_assert_nonnull(strstr(html, "25.00"));
	again = venture_mail_send_invoice(context, f->org, venture_entity_get_id(VENTURE_ENTITY(invoice)), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(message)), ==, venture_entity_get_id(VENTURE_ENTITY(again)));
	venture_query_set_organization(events, f->org);
	venture_query_add_filter_string(events, "kind", VENTURE_FILTER_OP_EQ, "mail_queued", NULL);
	g_assert_cmpint(venture_database_count(f->db, events, &error), ==, 1);
	g_assert_no_error(error);
}
static void test_user_notices(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureQuery) messages = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(GError) error = NULL;
	g_object_set(user, "organization-id", f->org, "username", "mailuser", "email", "user@example.test", "password-hash", "first", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, &error));
	g_assert_no_error(error);
	venture_query_set_organization(messages, f->org);
	g_assert_cmpint(venture_database_count(f->db, messages, &error), ==, 1);
	g_object_set(user, "password-hash", "second", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_database_count(f->db, messages, &error), ==, 2);
}
static void test_attachment_missing(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
	const gchar *refs = "[{\"type\":\"document\",\"id\":99999}]";
	g_autoptr(GError) error = NULL;
	g_object_set(f->outbox, "attachment-root", g_get_tmp_dir(), NULL);
	g_object_set(message, "organization-id", f->org, "to", "reader@example.test", "subject", "Attachment", "text-body", "Test", "attachments", refs, NULL);
	queued = venture_mail_outbox_enqueue(f->outbox, message, NULL, &error);
	g_assert_null(queued);
	g_assert_nonnull(error);
}
static void test_uncommitted_delivery(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "uncommitted");
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_begin(f->db, NULL));
	g_assert_cmpint(venture_mail_outbox_deliver_due(f->outbox, f->org, 10, NULL, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(venture_log_mailer_get_messages(f->mailer)->len, ==, 0);
	venture_database_rollback(f->db);
}

static void test_attachment_snapshot(Fixture *f, gconstpointer data)
{
	g_autofree gchar *path = NULL, *refs = NULL;
	g_autoptr(VentureDocument) document = venture_document_new();
	g_autoptr(VentureMailMessage) message = venture_mail_message_new(), queued = NULL;
	g_autoptr(GError) error = NULL;
	gint fd = g_file_open_tmp("venture-mail-attachment-XXXXXX", &path, &error);
	g_assert_no_error(error); g_assert_cmpint(fd, >=, 0); close(fd);
	g_assert_true(g_file_set_contents(path, "original attachment", -1, &error));
	g_object_set(document, "organization-id", f->org, "title", "receipt.txt", "path", path, "mime-type", "text/plain", NULL);
	g_assert_true(venture_document_service_save_attachment(venture_document_service_get(f->db), VENTURE_ENTITY(document), g_get_tmp_dir(), NULL, &error));
	g_assert_no_error(error);
	refs = g_strdup_printf("[{\"type\":\"document\",\"id\":%" G_GINT64_FORMAT "}]", venture_entity_get_id(VENTURE_ENTITY(document)));
	g_object_set(f->outbox, "attachment-root", g_get_tmp_dir(), NULL);
	g_object_set(message, "organization-id", f->org, "to", "reader@example.test", "subject", "Attachment", "text-body", "Test", "attachments", refs, NULL);
	queued = venture_mail_outbox_enqueue(f->outbox, message, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(queued);
	g_assert_true(g_file_set_contents(path, "changed", -1, &error));
	g_assert_nonnull(strstr(venture_entity_get_attribute(VENTURE_ENTITY(queued), "_mail_attachments"), "b3JpZ2luYWwgYXR0YWNobWVudA=="));
	g_assert_cmpint(g_unlink(path), ==, 0);
}
static gboolean reject_mail(VentureDatabase *db, VentureEntity *record, VentureEntity *previous, gpointer data, GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected mail persistence failure");
	return FALSE;
}
static void test_user_atomic_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(GError) error = NULL;
	g_object_set(user, "organization-id", f->org, "username", "rollback-user", "email", "rollback@example.test", NULL);
	venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, "rollback-user", NULL);
	venture_query_set_organization(query, f->org);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_MAIL_MESSAGE, reject_mail, NULL, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(user)), ==, 0);
	g_assert_cmpint(venture_database_count(f->db, query, NULL), ==, 0);
}
static void test_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = venture_context_new(config, f->db);
	venture_config_set_module_enabled(config, "mail", FALSE);
	g_assert_null(venture_context_get_mailer(context));
	g_assert_null(venture_context_get_mail_outbox(context));
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message"), ==, G_TYPE_INVALID);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), NULL));
	venture_config_set_module_enabled(config, "mail", TRUE);
	g_assert_nonnull(venture_context_get_mailer(context));
}
static void test_retained_key(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) row = enqueue(f, "retained");
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_database_purge(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void test_organization_unique(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMailMessage) first = enqueue(f, "per-organization");
	g_autoptr(VentureMailMessage) duplicate = venture_mail_message_new();
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureMailMessage) other_row = NULL;
	g_autoptr(GError) error = NULL;
	gint64 original = f->org;
	g_object_set(duplicate, "organization-id", f->org, "to", "reader@example.test", "subject", "Duplicate", "text-body", "Test", "idempotency-key", "per-organization", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(duplicate), NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_object_set(other, "name", "Other organization", "slug", "mail-other", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	g_assert_no_error(error);
	f->org = venture_entity_get_id(VENTURE_ENTITY(other));
	other_row = enqueue(f, "per-organization");
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(first)), !=, venture_entity_get_id(VENTURE_ENTITY(other_row)));
	g_assert_false(venture_mail_outbox_retry(f->outbox, f->org, venture_entity_get_id(VENTURE_ENTITY(first)), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	f->org = original;
}
static void test_restart_lease(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-mail-restart-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/outbox.db", directory);
	g_autoptr(VentureDatabase) db = venture_database_new(uri, NULL);
	g_autoptr(VentureMailOutbox) outbox = NULL;
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailMessage) input = venture_mail_message_new(), row = NULL, claimed = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(VentureEntity) organization = NULL, recovered = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc(), past = g_date_time_add_seconds(now, -601);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	gint64 org, id;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	organization = venture_database_find_one(db, query, NULL); org = venture_entity_get_id(organization);
	outbox = venture_mail_outbox_new(db, VENTURE_MAILER(mailer));
	g_object_set(input, "organization-id", org, "to", "reader@example.test", "subject", "Restart", "text-body", "Hello", NULL);
	row = venture_mail_outbox_enqueue(outbox, input, NULL, &error); g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(row));
	claimed = venture_mail_outbox_claim(outbox, org, id, past, &error); g_assert_no_error(error); g_assert_nonnull(claimed);
	g_clear_object(&outbox); g_clear_object(&db);
	db = venture_database_new(uri, &error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	outbox = venture_mail_outbox_new(db, VENTURE_MAILER(mailer));
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, org, 10, now, NULL, &error), ==, 0);
	g_assert_no_error(error);
	recovered = venture_database_get(db, VENTURE_TYPE_MAIL_MESSAGE, id, NULL);
	g_object_get(recovered, "state", &state, NULL); g_assert_cmpstr(state, ==, "uncertain");
	g_assert_cmpuint(venture_log_mailer_get_messages(mailer)->len, ==, 0);
	g_clear_object(&outbox); g_clear_object(&db);
	venture_test_remove_tree(directory);
}
static void mail_done(GObject *source, GAsyncResult *result, gpointer data)
{
	gboolean *done = data;
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_mailer_send_finish(VENTURE_MAILER(source), result, &error));
	g_assert_no_error(error); *done = TRUE;
}
static void test_async_registry(void)
{
	g_autoptr(VentureMailerRegistry) registry = venture_mailer_registry_new();
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	gboolean done = FALSE;
	g_object_set(message, "to", "reader@example.test", "subject", "Async", "text-body", "Hello", NULL);
	venture_mailer_registry_add(registry, "test", VENTURE_MAILER(mailer));
	g_assert_true(venture_mailer_registry_lookup(registry, "test") == VENTURE_MAILER(mailer));
	venture_mailer_send_async(venture_mailer_registry_lookup(registry, "test"), message, NULL, mail_done, &done);
	while (!done) g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(venture_log_mailer_get_messages(mailer)->len, ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/mail/registered", test_mail_registered);
	g_test_add("/mail/uncertain-idempotency-retry", Fixture, NULL, setup, test_uncertain, teardown);
	g_test_add("/mail/backoff-dead", Fixture, NULL, setup, test_backoff, teardown);
	g_test_add("/mail/expired-lease", Fixture, NULL, setup, test_lease, teardown);
	g_test_add("/mail/refuse-generic", Fixture, NULL, setup, test_refuse_generic, teardown);
	g_test_add("/mail/private-body", Fixture, NULL, setup, test_private_body, teardown);
	g_test_add_func("/mail/template", test_template);
	g_test_add_func("/mail/smtp-connection-failure", test_smtp_configuration);
	g_test_add_func("/mail/smtp-uncertain-wire", test_smtp_uncertain_wire);
	g_test_add_func("/mail/smtp-private-html-wire", test_smtp_private_html_wire);
	g_test_add("/mail/private-html-refused", Fixture, NULL, setup, test_private_html_refused, teardown);
	g_test_add("/mail/invoice-consumer", Fixture, NULL, setup, test_invoice_consumer, teardown);
	g_test_add("/mail/invoice-atomic-failure", Fixture, GINT_TO_POINTER(1), setup, test_invoice_consumer, teardown);
	g_test_add("/mail/user-notices", Fixture, NULL, setup, test_user_notices, teardown);
	g_test_add("/mail/attachment-missing", Fixture, NULL, setup, test_attachment_missing, teardown);
	g_test_add("/mail/uncommitted-delivery", Fixture, NULL, setup, test_uncommitted_delivery, teardown);
	g_test_add("/mail/attachment-snapshot", Fixture, NULL, setup, test_attachment_snapshot, teardown);
	g_test_add("/mail/user-atomic-failure", Fixture, NULL, setup, test_user_atomic_failure, teardown);
	g_test_add("/mail/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/mail/retained-key", Fixture, NULL, setup, test_retained_key, teardown);
	g_test_add("/mail/organization-unique", Fixture, NULL, setup, test_organization_unique, teardown);
	g_test_add_func("/mail/restart-lease", test_restart_lease);
	g_test_add_func("/mail/async-registry", test_async_registry);
	return g_test_run();
}
