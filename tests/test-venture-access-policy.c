/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * What the access policy costs an owner's pages on a hosted install shared
 * by many businesses.
 *
 * Every business on a hosted install is somebody's own: its owner signs in
 * with a browser and reads their own lists. A page must cost what that
 * business's records cost, not what the whole install holds. When every list
 * read every business's rows and checked them one at a time, a tiny business
 * waited over a second per click on an install of a hundred others.
 *
 * The measure is the number of SQL statements a signed-in GET sends, which is
 * exact and does not depend on how busy the machine is.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

#define WORKSPACE "5d0c3c8e-6f8e-4d59-9a59-1f3c7c1b2a10"
#define ORIGIN "https://example.test:8443"
#define AUTHORITY "example.test:8443"
#define PASSWORD "owner-test-password"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	VentureTenantService *service;
	SoupSession *session;
	gchar *directory;
	gchar *cookie;
	gint64 own;
} Fixture;

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/* One exchange; the status is returned and the session cookie it set kept. */
static guint
exchange(Fixture *f, const gchar *method, const gchar *path, const gchar *payload, gchar **out_cookie)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Reply reply = { FALSE, NULL, NULL };
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_replace(headers, "Host", AUTHORITY);
	if (f->cookie) {
		soup_message_headers_replace(headers, "Cookie", f->cookie);
		soup_message_headers_replace(headers, "Origin", ORIGIN);
	}
	if (payload) {
		g_autoptr(GBytes) body = g_bytes_new(payload, strlen(payload));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	if (out_cookie) {
		GSList *list = soup_cookies_from_response(message), *iter;
		*out_cookie = NULL;
		for (iter = list; iter; iter = iter->next)
			if (!strcmp(soup_cookie_get_name(iter->data), "venture_session") && *soup_cookie_get_value(iter->data)) {
				g_free(*out_cookie);
				*out_cookie = soup_cookie_to_cookie_header(iter->data);
			}
		soup_cookies_free(list);
	}
	g_bytes_unref(reply.body);
	return soup_message_get_status(message);
}

/* A business with its own owner, the way a hosted sign-up leaves it. */
static gint64
business(Fixture *f, const gchar *name, const gchar *username, guint companies)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Provision a business", &error);
	g_autoptr(VentureEntity) organization = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", name, "slug", username, "active", TRUE, NULL);
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureEntity) tenant_member = NULL;
	g_autoptr(VentureEntity) org_member = NULL;
	gint64 id;
	guint i;
	g_assert_no_error(error); g_assert_nonnull(maintenance);
	g_assert_true(venture_database_save(f->db, organization, NULL, &error)); g_assert_no_error(error);
	id = venture_entity_get_id(organization);
	user = g_object_new(VENTURE_TYPE_USER, "username", username, "display-name", name, "organization-id", id,
		"role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(VENTURE_USER(user), PASSWORD, 2000, &error));
	g_assert_true(venture_database_save(f->db, user, NULL, &error)); g_assert_no_error(error);
	tenant_member = g_object_new(VENTURE_TYPE_TENANT_MEMBERSHIP, "name", username,
		"user-id", venture_entity_get_id(user), "role", VENTURE_TENANT_ROLE_MEMBER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, tenant_member, NULL, &error)); g_assert_no_error(error);
	org_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", id,
		"user-id", venture_entity_get_id(user), "role", VENTURE_ORGANIZATION_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, org_member, NULL, &error)); g_assert_no_error(error);
	for (i = 0; i < companies; i++) {
		g_autofree gchar *label = g_strdup_printf("Customer %u", i);
		g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "name", label, "organization-id", id, NULL);
		g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT, "name", label, "organization-id", id, NULL);
		g_assert_true(venture_database_save(f->db, company, NULL, &error)); g_assert_no_error(error);
		g_object_set(contact, "company-id", venture_entity_get_id(company), NULL);
		g_assert_true(venture_database_save(f->db, contact, NULL, &error)); g_assert_no_error(error);
	}
	g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	return id;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GInetAddress) loopback_address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *loopback = g_inet_address_to_string(loopback_address);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) home = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Provider", "active", TRUE,
		"is-default", TRUE, NULL);
	g_autofree gchar *form = g_strdup_printf("username=owner&password=%s", PASSWORD);
	(void)data;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-access-cost-XXXXXX", &error); g_assert_no_error(error);
	f->session = soup_session_new_with_options("timeout", 30, NULL);
	g_object_set(f->config, "hosted-enabled", TRUE, "hosted-workspace-id", WORKSPACE, "hosted-origin", ORIGIN,
		"security-password-iterations", (gint64)2000, "state-dir", f->directory,
		"server-bind-address", loopback, "server-port", (gint64)0, NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->service = venture_tenant_service_get(f->db);
	g_assert_true(venture_tenant_service_configure(f->service, f->config, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_initialize(f->service, &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(f->db, home, NULL, &error)); g_assert_no_error(error);
	g_assert_true(venture_tenant_service_bootstrap_admin(f->service, f->config, "provisioner", "private-test-password",
		FALSE, "Page cost fixture", &error)); g_assert_no_error(error);
	f->own = business(f, "Own business", "owner", 2);
	f->context = venture_context_new(f->config, f->db);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
	g_assert_cmpuint(exchange(f, "POST", "/login", form, &f->cookie), ==, 302);
	g_assert_nonnull(f->cookie);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->session);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
	g_free(f->cookie);
}

/* The statements one signed-in GET of @path sends, after a warm-up. */
static guint64
cost(Fixture *f, const gchar *path)
{
	guint64 before;
	g_assert_cmpuint(exchange(f, "GET", path, NULL, NULL), ==, 200);
	before = venture_database_get_statement_count(f->db);
	g_assert_cmpuint(exchange(f, "GET", path, NULL, NULL), ==, 200);
	return venture_database_get_statement_count(f->db) - before;
}

static const gchar *const pages[] = {
	"/", "/e/company", "/e/contact", "/e/invoice", "/e/company/new", "/invoices/compose",
};

/* Twenty other businesses, each with an owner, customers and contacts, add
 * nothing to what this owner's pages cost. */
static void
test_cost_is_local(Fixture *f, gconstpointer data)
{
	guint64 alone[G_N_ELEMENTS(pages)];
	guint64 shared[G_N_ELEMENTS(pages)];
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(pages); i++)
		alone[i] = cost(f, pages[i]);
	for (i = 0; i < 20; i++) {
		g_autofree gchar *name = g_strdup_printf("Other business %u", i);
		g_autofree gchar *username = g_strdup_printf("other%u", i);
		business(f, name, username, 5);
	}
	for (i = 0; i < G_N_ELEMENTS(pages); i++) {
		shared[i] = cost(f, pages[i]);
		g_test_message("%s: %" G_GUINT64_FORMAT " statements alone, %" G_GUINT64_FORMAT " beside 20 businesses",
			pages[i], alone[i], shared[i]);
	}
	for (i = 0; i < G_N_ELEMENTS(pages); i++)
		g_assert_cmpuint(shared[i], ==, alone[i]);
}

/* Nor does a business's own size: twenty more customers, each with a
 * contact, are read in the same statements as two. */
static void
test_cost_is_flat(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	static const gchar *const lists[] = { "/e/company", "/e/contact", "/" };
	guint64 small[G_N_ELEMENTS(lists)];
	guint64 large[G_N_ELEMENTS(lists)];
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(lists); i++)
		small[i] = cost(f, lists[i]);
	for (i = 0; i < 20; i++) {
		g_autoptr(VentureTenantMaintenance) maintenance = venture_tenant_service_enter_maintenance(f->service, "Add a customer", &error);
		g_autofree gchar *label = g_strdup_printf("Later customer %u", i);
		g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "name", label, "organization-id", f->own, NULL);
		g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT, "name", label, "organization-id", f->own, NULL);
		g_assert_no_error(error);
		g_assert_true(venture_database_save(f->db, company, NULL, &error)); g_assert_no_error(error);
		g_object_set(contact, "company-id", venture_entity_get_id(company), NULL);
		g_assert_true(venture_database_save(f->db, contact, NULL, &error)); g_assert_no_error(error);
		g_assert_true(venture_tenant_maintenance_finish(maintenance, &error)); g_assert_no_error(error);
	}
	for (i = 0; i < G_N_ELEMENTS(lists); i++) {
		large[i] = cost(f, lists[i]);
		g_test_message("%s: %" G_GUINT64_FORMAT " statements with 2 customers, %" G_GUINT64_FORMAT " with 22",
			lists[i], small[i], large[i]);
	}
	for (i = 0; i < G_N_ELEMENTS(lists); i++)
		g_assert_cmpuint(large[i], ==, small[i]);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/access-policy/page-cost-is-local", Fixture, NULL, setup, test_cost_is_local, teardown);
	g_test_add("/access-policy/page-cost-is-flat", Fixture, NULL, setup, test_cost_is_flat, teardown);
	return g_test_run();
}
