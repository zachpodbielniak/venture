/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-accounting.h"

typedef struct {
	VentureDatabase *db;
	gint64 org;
} Fixture;

typedef struct {
	GSocketListener *listener;
	gchar *bodies[2];
	guint recipients;
} SmtpFixture;

static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
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
}

static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
}

static VentureMailMessage *enqueue(Fixture *f, VentureMailOutbox *outbox, const gchar *key)
{
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(GError) error = NULL;
	VentureMailMessage *row;
	g_object_set(message, "organization-id", f->org, "to", "reader@example.test",
		"subject", "Receipt", "text-body", "Hello", "idempotency-key", key, NULL);
	row = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
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

/* First DATA ends without 250, so the outbox records uncertain. The second
 * completes and is the body the customer would have received. */
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
				} else {
					g_string_append_printf(body, "%s\n", line);
					continue;
				}
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

/* Customers see the business, never the tool. A stored "Venture" is the
 * schema default. A different stored name is the owner's choice. */
static void test_tool_name_is_not_the_sender(Fixture *fixture, gconstpointer data)
{
	SmtpFixture wire;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureOrganizationMailer) selector = NULL;
	g_autoptr(VentureMailOutbox) outbox = NULL;
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) business = NULL;
	g_autoptr(VentureMailMessage) row = NULL, chosen = NULL;
	g_autofree gchar *endpoint = NULL;
	GThread *thread;
	gint64 binding, version;
	guint16 port;
	(void)data;
	business = venture_database_get(fixture->db, VENTURE_TYPE_ORGANIZATION, fixture->org, &error);
	g_assert_no_error(error);
	g_object_set(business, "name", "Maple Street Bakery", NULL);
	g_assert_true(venture_database_save(fixture->db, business, NULL, &error));
	g_assert_no_error(error);
	wire.listener = g_socket_listener_new();
	wire.bodies[0] = NULL;
	wire.bodies[1] = NULL;
	wire.recipients = 0;
	port = g_socket_listener_add_any_inet_port(wire.listener, NULL, &error);
	g_assert_no_error(error);
	endpoint = g_strdup_printf("127.0.0.1:%u", port);
	g_object_set(config, "mail-allowed-endpoints", endpoint, "mail-plaintext-endpoints", endpoint, NULL);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(fixture->db), key, &error));
	g_assert_no_error(error);
	selector = venture_organization_mailer_new(fixture->db, config);
	json_object_set_string_member(values, "host", "127.0.0.1");
	json_object_set_int_member(values, "port", port);
	json_object_set_string_member(values, "tls", "none");
	json_object_set_string_member(values, "auth", "none");
	json_object_set_string_member(values, "from", "owner@example.test");
	json_object_set_string_member(values, "from-name", "Venture");
	connection = venture_organization_mailer_configure(selector, fixture->org, values, 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(connection);
	outbox = venture_mail_outbox_new(fixture->db, VENTURE_MAILER(selector));
	row = enqueue(fixture, outbox, "sender-name-default");
	thread = g_thread_new("sender-name", smtp_server, &wire);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, fixture->org, 1, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	assert_state(fixture, venture_entity_get_id(VENTURE_ENTITY(row)), "uncertain");
	g_assert_true(venture_mail_outbox_retry(outbox, fixture->org, venture_entity_get_id(VENTURE_ENTITY(row)), NULL, &error));
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, fixture->org, 1, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	assert_state(fixture, venture_entity_get_id(VENTURE_ENTITY(row)), "sent");
	g_thread_join(thread);
	g_assert_nonnull(wire.bodies[1]);
	g_assert_nonnull(strstr(wire.bodies[1], "Maple Street Bakery"));
	g_assert_nonnull(strstr(wire.bodies[1], "owner@example.test"));
	g_assert_null(strstr(wire.bodies[1], "Venture"));
	g_free(wire.bodies[0]);
	g_free(wire.bodies[1]);
	wire.bodies[0] = wire.bodies[1] = NULL;
	binding = venture_entity_get_id(VENTURE_ENTITY(connection));
	version = venture_entity_get_version(VENTURE_ENTITY(connection));
	json_object_set_string_member(values, "from-name", "Dana Chen");
	g_clear_object(&connection);
	connection = venture_organization_mailer_configure(selector, fixture->org, values, version, binding, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(connection);
	chosen = enqueue(fixture, outbox, "sender-name-owner");
	thread = g_thread_new("sender-name-owner", smtp_server, &wire);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, fixture->org, 1, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	assert_state(fixture, venture_entity_get_id(VENTURE_ENTITY(chosen)), "uncertain");
	g_assert_true(venture_mail_outbox_retry(outbox, fixture->org, venture_entity_get_id(VENTURE_ENTITY(chosen)), NULL, &error));
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, fixture->org, 1, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	assert_state(fixture, venture_entity_get_id(VENTURE_ENTITY(chosen)), "sent");
	g_thread_join(thread);
	g_assert_nonnull(wire.bodies[1]);
	g_assert_nonnull(strstr(wire.bodies[1], "Dana Chen"));
	g_assert_null(strstr(wire.bodies[1], "Maple Street Bakery"));
	g_assert_null(strstr(wire.bodies[1], "Venture"));
	g_free(wire.bodies[0]);
	g_free(wire.bodies[1]);
	g_object_unref(wire.listener);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mail/tool-name-is-not-the-sender", Fixture, NULL, setup, test_tool_name_is_not_the_sender, teardown);
	return g_test_run();
}
