/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * A customer's page answers "where do we stand with them" before anything
 * else: what they owe, how much of it is late, which invoices are open,
 * what they paid lately, what was last done and what is next.
 *
 * The page used to open on counts ("Invoices 2"), health and support cost;
 * what a customer owed was a report away. What breaks if these regress: an
 * owner calls a customer about money without knowing the amount, chases an
 * invoice already paid, or misses one that is late.
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
	f->directory = g_dir_make_tmp("venture-customer-account-XXXXXX", NULL);
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

/* An issued invoice of one line for @amount, due on @due. */
static VentureEntity *
invoice(Fixture *f, const gchar *number, const gchar *due, const gchar *amount)
{
	g_autoptr(VentureEntity) row = record(f, "invoice");
	g_autoptr(VentureEntity) line = NULL;
	g_object_set(row, "number", number, "company-id", f->customer, NULL);
	field(row, "issued-at", "2026-07-01");
	field(row, "due-at", due);
	save(f, row);
	line = record(f, "invoice_line");
	g_object_set(line, "invoice-id", venture_entity_get_id(row), "description", "Widgets", "quantity", 1.0, NULL);
	field(line, "unit-price", amount);
	save(f, line);
	g_object_set(row, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, row);
	return g_steal_pointer(&row);
}

/* The text of the page's account card, so the assertions below cannot be
 * satisfied by the same words elsewhere on the page. */
static gchar *
account_card(const gchar *html)
{
	const gchar *start = strstr(html, "<section class=\"card customer-account\"");
	const gchar *end;
	g_assert_nonnull(start);
	end = strstr(start, "</section>");
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

/* Owed, late, open, paid lately, last done and next: all on the card, and
 * the card first on the page. */
static void
test_where_we_stand(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) late = invoice(f, "INV-1", "2026-07-31", "50 USD");
	g_autoptr(VentureEntity) also_late = invoice(f, "INV-2", "2026-08-31", "40 USD");
	g_autoptr(VentureEntity) not_yet = invoice(f, "INV-3", "2099-01-31", "30 USD");
	g_autoptr(VentureEntity) settled = invoice(f, "INV-4", "2026-08-15", "25 USD");
	g_autoptr(VentureEntity) payment = record(f, "payment");
	g_autoptr(VentureEntity) full = record(f, "payment");
	g_autoptr(VentureEntity) done = record(f, "activity");
	g_autoptr(VentureEntity) next = record(f, "activity");
	g_autoptr(VentureEntity) completed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->customer);
	g_autofree gchar *html = NULL;
	g_autofree gchar *card = NULL;
	(void)data;
	g_object_set(payment, "customer-id", f->customer, "invoice-id", venture_entity_get_id(late), "method", "transfer", NULL);
	field(payment, "amount", "20 USD"); field(payment, "date", "2026-08-02");
	save(f, payment);
	g_object_set(full, "customer-id", f->customer, "invoice-id", venture_entity_get_id(settled), "method", "card", NULL);
	field(full, "amount", "25 USD"); field(full, "date", "2026-08-10");
	save(f, full);
	g_object_set(done, "subject", "Called about the late invoice", "company-id", f->customer, NULL);
	field(done, "due-at", "2026-09-01");
	save(f, done);
	completed = venture_activity_service_complete(venture_database_get_activity_service(f->db), done,
		"Promised to pay", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(completed);
	g_object_set(next, "subject", "Send the October order", "company-id", f->customer, NULL);
	field(next, "due-at", "2099-01-05");
	save(f, next);

	html = page(f, path);
	card = account_card(html);
	/* Before the work counts, health and support. */
	g_assert_true(strstr(html, "<section class=\"card customer-account\"") < strstr(html, "<section class=\"card customer-work\""));
	/* 30 + 40 + 30 still open of 50 + 40 + 30; INV-4 is paid. */
	g_assert_nonnull(strstr(card, "Owes</dt><dd>$100.00"));
	/* INV-1's 30 and INV-2's 40 are past due; INV-3 is not. */
	g_assert_nonnull(strstr(card, "Overdue</dt><dd class=\"negative\">$70.00"));
	g_assert_nonnull(strstr(card, "INV-1")); g_assert_nonnull(strstr(card, "INV-2")); g_assert_nonnull(strstr(card, "INV-3"));
	g_assert_null(strstr(card, "INV-4"));
	/* The most recent payment first. */
	g_assert_nonnull(strstr(card, "$25.00")); g_assert_nonnull(strstr(card, "$20.00"));
	g_assert_true(strstr(card, "2026-08-10") < strstr(card, "2026-08-02"));
	g_assert_nonnull(strstr(card, "Called about the late invoice"));
	g_assert_nonnull(strstr(card, "Send the October order"));
}

/* A customer with no history says so, rather than showing empty tables. */
static void
test_nothing_yet(Fixture *f, gconstpointer data)
{
	g_autofree gchar *path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->customer);
	g_autofree gchar *html = NULL;
	g_autofree gchar *card = NULL;
	(void)data;
	html = page(f, path);
	card = account_card(html);
	g_assert_nonnull(strstr(card, "Owes</dt><dd>Nothing"));
	g_assert_null(strstr(card, "<table"));
}

/* A company that is only a supplier has no customer account card. */
static void
test_not_for_suppliers(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) supplier = record(f, "company");
	g_autofree gchar *path = NULL;
	g_autofree gchar *html = NULL;
	(void)data;
	field(supplier, "name", "Northfield Supply Co");
	field(supplier, "kind", "supplier");
	save(f, supplier);
	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, venture_entity_get_id(supplier));
	html = page(f, path);
	g_assert_null(strstr(html, "<section class=\"card customer-account\""));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/customer-account/where-we-stand", Fixture, NULL, setup, test_where_we_stand, teardown);
	g_test_add("/customer-account/nothing-yet", Fixture, NULL, setup, test_nothing_yet, teardown);
	g_test_add("/customer-account/not-for-suppliers", Fixture, NULL, setup, test_not_for_suppliers, teardown);
	return g_test_run();
}
