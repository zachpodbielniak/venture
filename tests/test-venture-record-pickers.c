/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Every choice of a record is a box you type into: the record pickers say
 * so to the page script, which always gives them a filter and opens it on
 * the first letter typed. And the companies offered are the right ones
 * first: customers where a customer is wanted, suppliers where a supplier
 * is.
 *
 * The pickers showed a filter only past seven options, and an invoice's
 * customer list held suppliers and prospects in one alphabet. What breaks
 * if these regress: an owner scrolls a list of hundreds, or bills a
 * supplier by accident.
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
	f->directory = g_dir_make_tmp("venture-record-pickers-XXXXXX", NULL);
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

static gint64
company(Fixture *f, const gchar *name, const gchar *kind)
{
	g_autoptr(VentureEntity) row = record(f, "company");
	field(row, "name", name);
	field(row, "kind", kind);
	save(f, row);
	return venture_entity_get_id(row);
}

/* The <select> named @name in @html, up to its end. */
static gchar *
select_named(const gchar *html, const gchar *name)
{
	g_autofree gchar *needle = g_strdup_printf("<select name=\"%s\"", name);
	const gchar *start = strstr(html, needle);
	const gchar *end;
	g_assert_nonnull(start);
	end = strstr(start, "</select>");
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

/* A record reference on a generic form is a searchable picker, however
 * few records there are. */
static void
test_generic_form_pickers(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = NULL;
	g_autofree gchar *picker = NULL;
	(void)data;
	html = page(f, "/e/contact/new");
	picker = select_named(html, "company-id");
	g_assert_nonnull(strstr(picker, "data-picker-search"));
}

/* The invoice's customer picker is searchable and offers customers first,
 * then every other company under its own heading. */
static void
test_invoice_customer_picker(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = NULL;
	g_autofree gchar *picker = NULL;
	const gchar *customers, *others, *buyer, *seller;
	(void)data;
	company(f, "Aardvark Supplies", "supplier");
	company(f, "Zebra Cafe", "customer");
	html = page(f, "/invoices/compose");
	picker = select_named(html, "company-id");
	g_assert_nonnull(strstr(picker, "data-picker-search"));
	customers = strstr(picker, "<optgroup label=\"Customers\">");
	others = strstr(picker, "<optgroup label=\"Other companies\">");
	buyer = strstr(picker, "Zebra Cafe");
	seller = strstr(picker, "Aardvark Supplies");
	g_assert_nonnull(customers); g_assert_nonnull(others);
	g_assert_true(customers < buyer && buyer < others && others < seller);
}

/* A bill's vendor and an expense's supplier offer suppliers first. */
static void
test_supplier_pickers(Fixture *f, gconstpointer data)
{
	g_autofree gchar *html = NULL;
	g_autofree gchar *picker = NULL;
	(void)data;
	company(f, "Aardvark Cafe", "customer");
	company(f, "Zinc Supplies", "supplier");
	html = page(f, "/e/expense/new");
	picker = select_named(html, "supplier-id");
	g_assert_nonnull(strstr(picker, "data-picker-search"));
	g_assert_true(strstr(picker, "<optgroup label=\"Suppliers\">") < strstr(picker, "Zinc Supplies"));
	g_assert_true(strstr(picker, "Zinc Supplies") < strstr(picker, "Aardvark Cafe"));
}

/* An expense filed against a supplier carries the supplier's name as its
 * vendor, so reports grouped by vendor keep working. */
static void
test_expense_supplier_names_vendor(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) expense = record(f, "expense");
	g_autofree gchar *vendor = NULL;
	gint64 supplier = company(f, "Northfield Supply Co", "supplier");
	(void)data;
	g_object_set(expense, "description", "Steel stock", "supplier-id", supplier, NULL);
	field(expense, "amount", "220 USD");
	field(expense, "occurred-at", "2026-10-08");
	save(f, expense);
	g_object_get(expense, "vendor", &vendor, NULL);
	g_assert_cmpstr(vendor, ==, "Northfield Supply Co");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/record-pickers/generic-form", Fixture, NULL, setup, test_generic_form_pickers, teardown);
	g_test_add("/record-pickers/invoice-customer", Fixture, NULL, setup, test_invoice_customer_picker, teardown);
	g_test_add("/record-pickers/suppliers", Fixture, NULL, setup, test_supplier_pickers, teardown);
	g_test_add("/record-pickers/expense-supplier-names-vendor", Fixture, NULL, setup, test_expense_supplier_names_vendor, teardown);
	return g_test_run();
}
