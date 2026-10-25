/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * The people at a company are on the company's page: who they are, what
 * they do there and how to reach them, with a way to add one already
 * filed under the company.
 *
 * Contacts lived in their own list with a Company column, and a company's
 * page did not name anybody at it. What breaks if these regress: an owner
 * on a customer's page cannot see whom to call, or adds a person who is
 * not filed under the company they were looking at.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	gchar *directory;
	gint64 org;
	gint64 customer;
} Fixture;

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(VentureEntity *record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(record, name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *row = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(row);
	venture_entity_set_organization_id(row, f->org);
	return row;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	(void)data;
	f->config = venture_config_new();
	f->directory = g_dir_make_tmp("venture-company-people-XXXXXX", NULL);
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	customer = record(f, "company");
	field(customer, "name", "Blue Pine Bakery");
	field(customer, "kind", "customer");
	save(f, customer);
	f->customer = venture_entity_get_id(customer);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
}

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static gchar *
page(Fixture *f, const gchar *path)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Reply reply = { FALSE, NULL, NULL };
	gchar *text;
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	g_bytes_unref(reply.body);
	return text;
}

static VentureEntity *
person(Fixture *f, const gchar *name, const gchar *role, const gchar *email, gint64 company)
{
	VentureEntity *row = record(f, "contact");
	g_object_set(row, "name", name, "role", role, "email", email, "company-id", company, NULL);
	save(f, row);
	return row;
}

/* Everybody at the company, with role and a way to write or call; nobody
 * from another company. */
static void
test_people_on_the_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) other = record(f, "company");
	g_autoptr(VentureEntity) a = NULL, b = NULL, c = NULL;
	g_autofree gchar *path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->customer);
	g_autofree gchar *html = NULL;
	const gchar *card;
	(void)data;
	field(other, "name", "Harbor Dental Studio");
	save(f, other);
	a = person(f, "Dana Okafor", "Owner", "dana@example.com", f->customer);
	b = person(f, "Sam Lee", "Bookkeeper", "sam@example.com", f->customer);
	c = person(f, "Priya Natarajan", "Office manager", "priya@example.com", venture_entity_get_id(other));
	html = page(f, path);
	card = strstr(html, "<section class=\"card company-people\"");
	g_assert_nonnull(card);
	g_assert_nonnull(strstr(card, "Dana Okafor")); g_assert_nonnull(strstr(card, "Owner"));
	g_assert_nonnull(strstr(card, "href=\"mailto:sam@example.com\""));
	g_assert_null(strstr(card, "Priya Natarajan"));
	g_assert_nonnull(strstr(card, "href=\"/e/contact/new?company_id=1\""));
}

/* "Add a person" opens the contact form with the company already chosen. */
static void
test_add_a_person(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = NULL;
	(void)data;
	html = page(f, "/e/contact/new?company_id=1");
	g_assert_nonnull(strstr(html, "<option value=\"1\" selected"));
}

/* Searching contacts by the company's name finds the people there. */
static void
test_search_by_company(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = person(f, "Dana Okafor", "Owner", "dana@example.com", f->customer);
	g_autoptr(VentureEntity) other = record(f, "company");
	g_autoptr(VentureEntity) b = NULL;
	g_autofree gchar *html = NULL;
	(void)data;
	field(other, "name", "Harbor Dental Studio");
	save(f, other);
	b = person(f, "Priya Natarajan", "Office manager", "priya@example.com", venture_entity_get_id(other));
	html = page(f, "/e/contact?search=pine");
	g_assert_nonnull(strstr(html, "Dana Okafor"));
	g_assert_null(strstr(html, "Priya Natarajan"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/company-people/on-the-page", Fixture, NULL, setup, test_people_on_the_page, teardown);
	g_test_add("/company-people/add-a-person", Fixture, NULL, setup, test_add_a_person, teardown);
	g_test_add("/company-people/search-by-company", Fixture, NULL, setup, test_search_by_company, teardown);
	return g_test_run();
}
