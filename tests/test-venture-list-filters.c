/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Filters must survive moving through a list and exporting its records. */
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
	f->directory = g_dir_make_tmp("venture-list-filters-XXXXXX", NULL);
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


static void
test_controls(Fixture *f, gconstpointer data)
{
	g_autofree gchar *companies = page(f, "/e/company?customer=true");
	g_autofree gchar *tickets = page(f, "/e/ticket");
	g_autofree gchar *expenses = page(f, "/e/expense");
	const gchar *form;
	(void)data;
	form = strstr(companies, "id=\"list-filters\"");
	g_assert_nonnull(form);
	g_assert_nonnull(strstr(form, "name=\"kind\""));
	g_assert_nonnull(strstr(form, "name=\"customer\""));
	g_assert_null(strstr(form, "name=\"external-id\""));
	g_assert_nonnull(strstr(tickets, "name=\"status\""));
	g_assert_nonnull(strstr(tickets, "name=\"assignee\""));
	g_assert_nonnull(strstr(expenses, "name=\"occurred-at__gte\""));
	g_assert_nonnull(strstr(expenses, "name=\"amount\""));
}

static void
test_links_keep_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) second = record(f, "company");
	g_autoptr(VentureEntity) supplier = record(f, "company");
	g_autofree gchar *html = NULL;
	(void)data;
	field(second, "name", "Second Bakery"); field(second, "kind", "customer"); save(f, second);
	field(supplier, "name", "Supplier Only"); field(supplier, "kind", "supplier"); save(f, supplier);
	html = page(f, "/e/company?customer=true&limit=1&order=name");
	g_assert_nonnull(strstr(html, "/e/company/export?customer=true&amp;limit=1&amp;order=name"));
	g_assert_nonnull(strstr(html, "/e/company?customer=true&amp;limit=1&amp;order=name&amp;page=2"));
	g_assert_nonnull(strstr(html, "/e/company?customer=true&amp;limit=1&amp;order=-name"));
	g_assert_null(strstr(html, ">Supplier Only</a>"));
}

static void
test_blank_controls_do_not_narrow(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = page(f, "/e/company?list_filters=1&customer=true&kind=&supplier=&search=Bakery");
	(void)data;
	g_assert_nonnull(strstr(html, ">Blue Pine Bakery</a>"));
	g_assert_nonnull(strstr(html, "value=\"Bakery\""));
}

static void
test_empty_api_filter(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) empty = record(f, "contact");
	g_autoptr(VentureEntity) filled = record(f, "contact");
	g_autofree gchar *html = NULL;
	(void)data;
	field(empty, "name", "No Email Person"); save(f, empty);
	field(filled, "name", "Has Email Person"); field(filled, "email", "person@example.com"); save(f, filled);
	html = page(f, "/e/contact?email=");
	g_assert_nonnull(strstr(html, ">No Email Person</a>"));
	g_assert_null(strstr(html, ">Has Email Person</a>"));
}

static void
test_date_and_money_filters(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) earlier = record(f, "expense");
	g_autoptr(VentureEntity) later = record(f, "expense");
	g_autofree gchar *html = NULL;
	g_autofree gchar *zero = NULL;
	(void)data;
	field(earlier, "description", "Earlier supplies");
	field(earlier, "amount", "20 USD");
	field(earlier, "occurred-at", "2026-10-08T09:00:00Z"); save(f, earlier);
	field(later, "description", "Later supplies");
	field(later, "amount", "30 USD");
	field(later, "occurred-at", "2026-10-20T09:00:00Z"); save(f, later);
	html = page(f, "/e/expense?list_filters=1&amount=positive&occurred-at__gte=2026-10-01&occurred-at__lt=2026-10-15");
	g_assert_nonnull(strstr(html, ">Earlier supplies</a>"));
	g_assert_null(strstr(html, ">Later supplies</a>"));
	zero = page(f, "/e/expense?list_filters=1&amount=zero");
	g_assert_null(strstr(zero, ">Earlier supplies</a>"));
	g_assert_null(strstr(zero, ">Later supplies</a>"));
}

static void
test_filter_history(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = page(f, "/e/company?customer=true");
	const gchar *form = strstr(html, "id=\"list-filters\"");
	const gchar *end;
	const gchar *history;
	(void)data;
	g_assert_nonnull(form);
	end = strchr(form, '>');
	history = strstr(form, "hx-push-url=\"true\"");
	g_assert_nonnull(history);
	g_assert_true(history < end);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/list-filters/history", Fixture, NULL, setup, test_filter_history, teardown);
	g_test_add("/list-filters/controls", Fixture, NULL, setup, test_controls, teardown);
	g_test_add("/list-filters/links-keep-scope", Fixture, NULL, setup, test_links_keep_scope, teardown);
	g_test_add("/list-filters/blank-controls", Fixture, NULL, setup, test_blank_controls_do_not_narrow, teardown);
	g_test_add("/list-filters/empty-api-filter", Fixture, NULL, setup, test_empty_api_filter, teardown);
	g_test_add("/list-filters/date-and-money", Fixture, NULL, setup, test_date_and_money_filters, teardown);
	return g_test_run();
}
