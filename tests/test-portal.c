/*
 * test-portal.c - Isolated customer invoices, statements and pay.
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
	gint64 other;
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
	g_autoptr(VentureCompany) other = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = venture_company_new();
	g_object_set(company, "name", "Customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
	other = venture_company_new();
	g_object_set(other, "name", "Other", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(other), f->org);
	save(f, VENTURE_ENTITY(other));
	f->other = venture_entity_get_id(VENTURE_ENTITY(other));
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
	actor->name = "portal";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static VentureEntity *
issued_invoice(Fixture *f, gint64 company, const gchar *number, const gchar *amount)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
	g_object_set(invoice, "number", number, "company-id", company, NULL);
	save(f, VENTURE_ENTITY(invoice));
	venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", amount, NULL));
	save(f, VENTURE_ENTITY(line));
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		invoice, "sent", now, NULL, &error));
	g_assert_no_error(error);
	return VENTURE_ENTITY(g_object_ref(invoice));
}

static void
test_invite_pay_revoke(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) stranger = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(4000, "USD");
	g_autoptr(VentureMoney) balance = NULL;
	g_autofree gchar *token = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	invoice = issued_invoice(f, f->company, "INV-P1", "40 USD");
	stranger = issued_invoice(f, f->other, "INV-X", "99 USD");
	access = venture_portal_service_invite(venture_portal_service_get(f->db),
		f->org, f->company, "billing@example.org", &actor, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);
	g_assert_cmpint((int)strlen(token), ==, 64);
	invoices = venture_portal_service_invoices(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), &error);
	g_assert_cmpuint(invoices->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(invoices, 0)), ==,
		venture_entity_get_id(invoice));
	g_assert_false(venture_portal_service_pay(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), venture_entity_get_id(stranger),
		amount, &actor, &error));
	g_clear_error(&error);
	g_assert_false(venture_portal_service_pay(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), venture_entity_get_id(invoice),
		amount, &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "Checkout"));
	g_clear_error(&error);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		venture_entity_get_id(invoice), NULL, &error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 4000);
	g_assert_true(venture_portal_service_revoke(venture_portal_service_get(f->db),
		VENTURE_CUSTOMER_PORTAL_ACCESS(access), &actor, &error));
	g_assert_null(venture_portal_service_lookup(venture_portal_service_get(f->db), token, &error));
	g_assert_cmpint(venture_entity_get_id(invoice), >, 0);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} PortalResponse;

static void
portal_http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	PortalResponse *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;

	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	PortalResponse response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, "application/json", payload);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, portal_http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out && response.bytes)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
test_http_isolation(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-portal-XXXXXX", NULL);
	g_autofree gchar *token = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	invoice = issued_invoice(f, f->company, "INV-WEB", "10 USD");
	issued_invoice(f, f->other, "INV-HIDDEN", "50 USD");
	access = venture_portal_service_invite(venture_portal_service_get(f->db),
		f->org, f->company, "a@b.c", &actor, &error);
	g_object_get(access, "token", &token, NULL);
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_cmpuint(http_request(server, "GET", "/portal/deadbeef", NULL, NULL), ==, 404);
	path = g_strdup_printf("/portal/%s", token);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "INV-WEB"));
	g_assert_null(strstr(body, "INV-HIDDEN"));
	g_assert_nonnull(strstr(body, "Checkout"));
	venture_test_remove_tree(dir);
	(void)invoice;
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/portal/invite-pay-revoke", Fixture, NULL, setup, test_invite_pay_revoke, teardown);
	g_test_add("/portal/http-isolation", Fixture, NULL, setup, test_http_isolation, teardown);
	return g_test_run();
}
