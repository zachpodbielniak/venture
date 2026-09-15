/*
 * test-documents.c - One guided page to add invoice/quote lines, tax and send.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Buyer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "clerk";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static JsonObject *
invoice_spec(Fixture *f, gboolean send)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "company_id");
	json_builder_add_int_value(builder, f->company);
	json_builder_set_member_name(builder, "terms");
	json_builder_add_string_value(builder, "Net 30");
	json_builder_set_member_name(builder, "due_days");
	json_builder_add_int_value(builder, 30);
	json_builder_set_member_name(builder, "send");
	json_builder_add_boolean_value(builder, send);
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Hours");
	json_builder_set_member_name(builder, "quantity");
	json_builder_add_double_value(builder, 1.5);
	json_builder_set_member_name(builder, "unit_price");
	json_builder_add_string_value(builder, "100 USD");
	json_builder_set_member_name(builder, "discount_percent");
	json_builder_add_int_value(builder, 10);
	json_builder_set_member_name(builder, "tax_percent");
	json_builder_add_int_value(builder, 5);
	json_builder_end_object(builder);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return json_object_ref(json_node_get_object(json_builder_get_root(builder)));
}

static void
test_compose_invoice_and_send(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) spec = invoice_spec(f, TRUE);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	VentureActor actor;
	gint status;
	(void)data;
	actor_init(&actor);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db),
		f->org, spec, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	g_object_get(invoice, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_SENT);
	query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(invoice), NULL);
	lines = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(lines->len, ==, 1);
	amount = venture_invoice_line_get_amount(g_ptr_array_index(lines, 0), &error);
	g_assert_no_error(error);
	/* 1.5 * 100.00, 10% discount, 5% tax: 150 - 15 = 135 + 6.75 = 141.75 */
	g_assert_cmpint(venture_money_get_amount(amount), ==, 14175);
}

static void
test_compose_quote_and_send(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonObject) spec = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureMoney) total = NULL;
	VentureActor actor;
	gint status;
	(void)data;
	actor_init(&actor);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "company_id");
	json_builder_add_int_value(builder, f->company);
	json_builder_set_member_name(builder, "currency");
	json_builder_add_string_value(builder, "USD");
	json_builder_set_member_name(builder, "send");
	json_builder_add_boolean_value(builder, TRUE);
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, "Work");
	json_builder_set_member_name(builder, "quantity");
	json_builder_add_int_value(builder, 2);
	json_builder_set_member_name(builder, "unit_price");
	json_builder_add_string_value(builder, "50 USD");
	json_builder_set_member_name(builder, "tax_percent");
	json_builder_add_int_value(builder, 10);
	json_builder_end_object(builder);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	spec = json_object_ref(json_node_get_object(json_builder_get_root(builder)));
	quote = venture_document_service_compose_quote(venture_document_service_get(f->db),
		f->org, spec, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(quote);
	g_object_get(quote, "status", &status, "total", &total, NULL);
	g_assert_cmpint(status, ==, VENTURE_QUOTE_SENT);
	g_assert_cmpint(venture_money_get_amount(total), ==, 11000);
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	GBytes *response;
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", payload);
	}
	response = soup_session_send_and_read(session, message, NULL, &error);
	g_assert_no_error(error);
	if (out && response)
		*out = g_strndup(g_bytes_get_data(response, NULL), g_bytes_get_size(response));
	g_clear_pointer(&response, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
G_GNUC_UNUSED test_http_compose(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-documents-XXXXXX", NULL);
	g_autofree gchar *body = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port;
	g_autoptr(JsonObject) spec = invoice_spec(f, FALSE);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *json = NULL;
	(void)data;
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	json_node_take_object(node, json_object_ref(spec));
	json = venture_json_to_string(node, FALSE);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/invoices/compose", json, NULL), ==, 200);
	(void)body;
	venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/documents/compose-invoice", Fixture, NULL, setup, test_compose_invoice_and_send, teardown);
	g_test_add("/documents/compose-quote", Fixture, NULL, setup, test_compose_quote_and_send, teardown);
	/* HTTP covered by cutover-style async in other suites */
	return g_test_run();
}
