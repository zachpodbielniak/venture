/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The owner reaches daily tasks and reports for the business they selected. */
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
	gint64 selected;
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
	f->directory = g_dir_make_tmp("venture-owner-home-XXXXXX", NULL);
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
	g_autofree gchar *cookie = g_strdup_printf("venture_entity=%" G_GINT64_FORMAT, f->selected);
	gchar *text;
	if (f->selected > 0)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
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
test_owner_jobs(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = page(f, "/");
	const gchar *nav = strstr(html, "aria-label=\"Everyday tasks\"");
	const gchar *end;
	g_autofree gchar *jobs = NULL;
	const gchar *paths[] = { "/e/company?customer=true", "/e/contact", "/invoices/compose", "/e/expense/new", "/reports/pnl" };
	guint i;
	(void)data;
	g_assert_nonnull(nav);
	end = strstr(nav, "</nav>");
	g_assert_nonnull(end);
	g_assert_nonnull(strstr(end, "<details class=\"owner-overview\"><summary>Business overview</summary>"));
	jobs = g_strndup(nav, end - nav);
	for (i = 0; i < G_N_ELEMENTS(paths); i++)
	{
		g_autofree gchar *href = g_strdup_printf("href=\"%s\"", paths[i]);
		g_assert_nonnull(strstr(jobs, href));
	}
}

static void
test_sidebar_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureEntity) expense = record(f, "expense");
	g_autofree gchar *url = NULL;
	g_autofree gchar *explicit_page = NULL;
	g_autofree gchar *sidebar_page = NULL;
	g_autofree gchar *original_page = NULL;
	(void)data;
	g_object_set(other, "name", "Blue Pine Workshop", "default-currency", "USD", NULL);
	save(f, VENTURE_ENTITY(other));
	f->selected = venture_entity_get_id(VENTURE_ENTITY(other));
	g_assert_cmpint(f->selected, !=, f->org);
	venture_entity_set_organization_id(expense, f->selected);
	field(expense, "description", "Workshop supplies");
	field(expense, "amount", "220 USD");
	field(expense, "occurred-at", "2026-10-08T09:00:00Z");
	save(f, expense);
	url = g_strdup_printf("/reports/pnl?period=2026-10&organization_id=%" G_GINT64_FORMAT, f->selected);
	explicit_page = page(f, url);
	g_assert_nonnull(strstr(explicit_page, "220.00"));
	g_clear_pointer(&url, g_free);
	url = g_strdup_printf("/reports/pnl?period=2026-10&organization_id=%" G_GINT64_FORMAT, f->org);
	original_page = page(f, url);
	g_assert_null(strstr(original_page, "220.00"));
	sidebar_page = page(f, data ? "/api/v1/reports/pnl?period=2026-10&format=csv" : "/reports/pnl?period=2026-10");
	g_assert_nonnull(strstr(sidebar_page, "220.00"));
}

static void
test_classic_jobs(Fixture *f, gconstpointer data)
{
	venture_config_set_module_enabled(f->config, "headline", FALSE);
	test_owner_jobs(f, data);
}

static void
test_disabled_jobs(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = NULL;
	g_autofree gchar *jobs = NULL;
	const gchar *nav, *end;
	(void)data;
	venture_config_set_module_enabled(f->config, "crm", FALSE);
	venture_config_set_module_enabled(f->config, "invoicing", FALSE);
	venture_config_set_module_enabled(f->config, "finance", FALSE);
	html = page(f, "/");
	nav = strstr(html, "aria-label=\"Everyday tasks\"");
	if (nav == NULL)
		return;
	end = strstr(nav, "</nav>");
	g_assert_nonnull(end);
	jobs = g_strndup(nav, end - nav);
	g_assert_null(strstr(jobs, "href="));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/owner-home/jobs", Fixture, NULL, setup, test_owner_jobs, teardown);
	g_test_add("/owner-home/sidebar-report", Fixture, NULL, setup, test_sidebar_report, teardown);
	g_test_add("/owner-home/sidebar-export", Fixture, GINT_TO_POINTER(1), setup, test_sidebar_report, teardown);
	g_test_add("/owner-home/classic-jobs", Fixture, NULL, setup, test_classic_jobs, teardown);
	g_test_add("/owner-home/disabled-jobs", Fixture, NULL, setup, test_disabled_jobs, teardown);
	return g_test_run();
}
