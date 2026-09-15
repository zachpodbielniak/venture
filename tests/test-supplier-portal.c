/*
 * test-supplier-portal.c - Isolated supplier bills and payment status.
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
	gint64 vendor;
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
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "supplier-portal";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) vendor = NULL;
	g_autoptr(VentureCompany) other = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	vendor = venture_company_new();
	g_object_set(vendor, "name", "Acme Supply", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(vendor), f->org);
	save(f, VENTURE_ENTITY(vendor));
	f->vendor = venture_entity_get_id(VENTURE_ENTITY(vendor));
	other = venture_company_new();
	g_object_set(other, "name", "Other Vendor", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
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

static VentureEntity *
issued_bill(Fixture *f, gint64 vendor, const gchar *number, const gchar *amount)
{
	g_autoptr(VentureVendorBill) bill = venture_vendor_bill_new();
	g_autoptr(VentureVendorBillLine) line = venture_vendor_bill_line_new();
	g_autoptr(VentureVendorBillEvent) event = venture_vendor_bill_event_new();
	g_autoptr(GError) error = NULL;
	venture_entity_set_organization_id(VENTURE_ENTITY(bill), f->org);
	g_object_set(bill, "number", number, "company-id", vendor, "currency", "USD",
		"status", "draft", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(bill), "bill-date", "2026-01-01", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(bill), "due-date", "2026-01-31", NULL));
	save(f, VENTURE_ENTITY(bill));
	venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
	g_object_set(line, "bill-id", venture_entity_get_id(VENTURE_ENTITY(bill)),
		"description", "Parts", "quantity", "1", "category", "supplies", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", amount, NULL));
	save(f, VENTURE_ENTITY(line));
	venture_entity_set_organization_id(VENTURE_ENTITY(event), f->org);
	g_object_set(event, "bill-id", venture_entity_get_id(VENTURE_ENTITY(bill)),
		"vendor-id", vendor, "kind", "approve", "state", "approved", NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(event), "date", "2026-01-01", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(event), NULL, &error));
	g_assert_no_error(error);
	return VENTURE_ENTITY(g_object_ref(bill));
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} HttpResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpResult *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	HttpResult response;
	memset(&response, 0, sizeof(response));
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message,
			content_type ? content_type : "application/json", payload);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out && response.bytes)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
test_invite_isolate_revoke(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) stranger = NULL;
	g_autoptr(GPtrArray) bills = NULL;
	g_autoptr(VentureSupplierPortalAccess) forged = NULL;
	g_autofree gchar *token = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	bill = issued_bill(f, f->vendor, "BILL-P1", "40 USD");
	stranger = issued_bill(f, f->other, "BILL-X", "99 USD");
	access = venture_portal_service_invite_supplier(venture_portal_service_get(f->db),
		f->org, f->vendor, "ap@example.org", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(access);
	g_object_get(access, "token", &token, NULL);
	g_assert_cmpint((int)strlen(token), ==, 64);
	bills = venture_portal_service_bills(venture_portal_service_get(f->db),
		VENTURE_SUPPLIER_PORTAL_ACCESS(access), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(bills->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(bills, 0)), ==,
		venture_entity_get_id(bill));
	g_assert_null(venture_portal_service_lookup_supplier(venture_portal_service_get(f->db),
		"deadbeef", &error));
	g_clear_error(&error);
	forged = venture_supplier_portal_access_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(forged), f->org);
	g_object_set(forged, "company-id", f->vendor, "token",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		"email", "x@y.z", "revoked", FALSE, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(forged), &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "VenturePortalService"));
	g_clear_error(&error);
	g_assert_true(venture_portal_service_revoke_supplier(venture_portal_service_get(f->db),
		VENTURE_SUPPLIER_PORTAL_ACCESS(access), &actor, &error));
	g_assert_no_error(error);
	g_assert_null(venture_portal_service_lookup_supplier(venture_portal_service_get(f->db), token, &error));
	g_assert_cmpint(venture_entity_get_id(stranger), >, 0);
}

static void
test_http_isolation(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *print_path = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *print_body = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	bill = issued_bill(f, f->vendor, "BILL-WEB", "10 USD");
	issued_bill(f, f->other, "BILL-HIDDEN", "50 USD");
	access = venture_portal_service_invite_supplier(venture_portal_service_get(f->db),
		f->org, f->vendor, "a@b.c", &actor, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);
	dir = g_dir_make_tmp("venture-supplier-portal-XXXXXX", &error);
	g_assert_no_error(error);
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(http_request(server, "GET", "/supplier/deadbeef", NULL, NULL, NULL), ==, 404);
	path = g_strdup_printf("/supplier/%s", token);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "BILL-WEB"));
	g_assert_null(strstr(body, "BILL-HIDDEN"));
	g_assert_null(strstr(body, "Pay"));
	g_assert_nonnull(strstr(body, "Print"));
	print_path = g_strdup_printf("/supplier/%s/bills/%" G_GINT64_FORMAT "/print",
		token, venture_entity_get_id(bill));
	g_assert_cmpuint(http_request(server, "GET", print_path, NULL, NULL, &print_body), ==, 200);
	g_assert_nonnull(strstr(print_body, "BILL-WEB"));
	g_clear_pointer(&print_path, g_free);
	print_path = g_strdup_printf("/supplier/%s/bills/99/print", token);
	g_assert_cmpuint(http_request(server, "GET", print_path, NULL, NULL, NULL), ==, 404);
	{
		g_autofree gchar *payload = g_strdup_printf("{\"company_id\":%" G_GINT64_FORMAT ",\"email\":\"ap@example.org\"}", f->vendor);
		g_autofree gchar *reply = NULL;
		g_object_set(f->config, "server-base-url", "https://venture.example.org", NULL);
		g_assert_cmpuint(http_request(server, "POST", "/api/v1/supplier_portal/invite", NULL, payload, &reply), ==, 201);
		g_assert_null(strstr(reply, "\"token\""));
	}
	venture_config_set_module_enabled(f->config, "supplier_portal", FALSE);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, NULL, NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "supplier_portal", TRUE);
	venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/supplier-portal/invite-isolate-revoke", Fixture, NULL, setup,
		test_invite_isolate_revoke, teardown);
	g_test_add("/supplier-portal/http-isolation", Fixture, NULL, setup,
		test_http_isolation, teardown);
	return g_test_run();
}
