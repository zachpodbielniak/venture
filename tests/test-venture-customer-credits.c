/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * A customer's credits are on their page and are applied from there or
 * from the invoice, in one step.
 *
 * Credits lived only under Money in > Customer credits, and applying one
 * meant making a payment allocation by hand. What breaks if these
 * regress: an owner bills a customer who is owed money, or cannot use the
 * credit without knowing the bookkeeping behind it.
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
	f->directory = g_dir_make_tmp("venture-customer-credits-XXXXXX", NULL);
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

/* A form POST; the status is returned and the Location kept. */
static guint
post(Fixture *f, const gchar *path, const gchar *form, gchar **location)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("POST", url);
	g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
	Reply reply = { FALSE, NULL, NULL };
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_bytes_unref(reply.body);
	if (location)
		*location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	return soup_message_get_status(message);
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

static VentureEntity *
credit(Fixture *f, const gchar *amount, const gchar *reference)
{
	VentureEntity *row = record(f, "customer_credit");
	g_object_set(row, "customer-id", f->customer, "kind", "credit_note", "reference", reference, NULL);
	field(row, "amount", amount);
	field(row, "date", "2026-09-01");
	save(f, row);
	return row;
}

static gint64
remaining(Fixture *f, VentureEntity *row)
{
	g_autoptr(VentureEntity) stored = venture_database_get(f->db, VENTURE_TYPE_CUSTOMER_CREDIT, venture_entity_get_id(row), NULL);
	g_autoptr(VentureMoney) left = NULL;
	g_object_get(stored, "remaining", &left, NULL);
	return venture_money_get_amount(left);
}

static gint64
balance(Fixture *f, VentureEntity *row)
{
	g_autoptr(VentureMoney) left = venture_settlement_service_invoice_balance(
		venture_settlement_service_get(f->db), venture_entity_get_id(row), NULL, NULL);
	g_assert_nonnull(left);
	return venture_money_get_amount(left);
}

/* The account card lists unused credits with what is left on each, and
 * the amount takes them off what is owed. */
static void
test_credits_on_the_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) open = invoice(f, "INV-1", "2099-01-31", "80 USD");
	g_autoptr(VentureEntity) goodwill = credit(f, "50 USD", "Goodwill for a late delivery");
	g_autoptr(VentureEntity) used = credit(f, "10 USD", "Used up");
	g_autoptr(VentureEntity) allocation = record(f, "payment_allocation");
	g_autofree gchar *path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->customer);
	g_autofree gchar *html = NULL;
	g_autofree gchar *card = NULL;
	(void)data;
	g_object_set(allocation, "credit-id", venture_entity_get_id(used), "invoice-id", venture_entity_get_id(open), NULL);
	field(allocation, "amount", "10 USD"); field(allocation, "date", "2026-09-02");
	save(f, allocation);
	html = page(f, path);
	card = account_card(html);
	g_assert_nonnull(strstr(card, "Credit</dt><dd>$50.00"));
	g_assert_nonnull(strstr(card, "Goodwill for a late delivery"));
	g_assert_null(strstr(card, "Used up"));
	/* One step: the credit, an open invoice already chosen, the amount. */
	g_assert_nonnull(strstr(card, "action=\"/customer-credits/apply\""));
	g_assert_nonnull(strstr(card, "<option value=\"1\" selected>INV-1"));
}

/* Applying from the customer's page takes the amount off the invoice and
 * the credit, and comes back to the page. */
static void
test_apply_from_customer(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) open = invoice(f, "INV-1", "2099-01-31", "80 USD");
	g_autoptr(VentureEntity) goodwill = credit(f, "50 USD", "Goodwill");
	g_autofree gchar *form = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *back = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, f->customer);
	(void)data;
	form = g_strdup_printf("credit_id=%" G_GINT64_FORMAT "&invoice_id=%" G_GINT64_FORMAT "&amount=30.00&back=%s",
		venture_entity_get_id(goodwill), venture_entity_get_id(open), back);
	g_assert_cmpuint(post(f, "/customer-credits/apply", form, &location), ==, 303);
	g_assert_cmpstr(location, ==, back);
	g_assert_cmpint(remaining(f, goodwill), ==, 2000);
	g_assert_cmpint(balance(f, open), ==, 5000);
}

/* More than the credit holds, or more than the invoice owes, is refused
 * and nothing is applied. */
static void
test_apply_refuses_too_much(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) open = invoice(f, "INV-1", "2099-01-31", "20 USD");
	g_autoptr(VentureEntity) goodwill = credit(f, "50 USD", "Goodwill");
	g_autofree gchar *form = NULL;
	(void)data;
	form = g_strdup_printf("credit_id=%" G_GINT64_FORMAT "&invoice_id=%" G_GINT64_FORMAT "&amount=40.00",
		venture_entity_get_id(goodwill), venture_entity_get_id(open));
	g_assert_cmpuint(post(f, "/customer-credits/apply", form, NULL), ==, 422);
	g_assert_cmpint(remaining(f, goodwill), ==, 5000);
	g_assert_cmpint(balance(f, open), ==, 2000);
}

/* An open invoice of a customer with credit offers it on the invoice. */
static void
test_apply_from_invoice(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) open = invoice(f, "INV-1", "2099-01-31", "80 USD");
	g_autoptr(VentureEntity) goodwill = credit(f, "50 USD", "Goodwill");
	g_autofree gchar *path = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT, venture_entity_get_id(open));
	g_autofree gchar *html = NULL;
	const gchar *form;
	(void)data;
	html = page(f, path);
	g_assert_nonnull(strstr(html, "invoice-credits"));
	g_assert_nonnull(strstr(strstr(html, "invoice-credits"), "Goodwill"));
	form = strstr(html, "action=\"/customer-credits/apply\"");
	g_assert_nonnull(form);
	g_assert_nonnull(strstr(form, "name=\"invoice_id\" value=\"1\""));
	/* The whole of the smaller of the two is offered. */
	g_assert_nonnull(strstr(form, "name=\"amount\" value=\"50.00\""));
}

static void
test_apply_refuses_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) open = invoice(f, "INV-1", "2099-01-31", data ? "20 EUR" : "20 USD");
	g_autoptr(VentureEntity) goodwill = credit(f, "50 USD", "Goodwill");
	g_autofree gchar *form = NULL;

	form = g_strdup_printf("credit_id=%" G_GINT64_FORMAT "&invoice_id=%" G_GINT64_FORMAT "&amount=%s",
		venture_entity_get_id(goodwill), venture_entity_get_id(open), data ? "5%20USD" : "5%20EUR");
	g_assert_cmpuint(post(f, "/customer-credits/apply", form, NULL), ==, 422);
	g_assert_cmpint(remaining(f, goodwill), ==, 5000);
	g_assert_cmpint(balance(f, open), ==, 2000);
}

static void
test_apply_return_stays_local(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) open = invoice(f, "INV-1", "2099-01-31", "20 USD");
	g_autoptr(VentureEntity) goodwill = credit(f, "50 USD", "Goodwill");
	g_autofree gchar *form = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *expected = g_strdup_printf("/e/invoice/%" G_GINT64_FORMAT,
		venture_entity_get_id(open));
	(void)data;

	/* Browsers remove the tab and interpret this as an external URL. */
	form = g_strdup_printf("credit_id=%" G_GINT64_FORMAT "&invoice_id=%" G_GINT64_FORMAT
		"&amount=5.00&back=%%2F%%09%%2Fexample.invalid",
		venture_entity_get_id(goodwill), venture_entity_get_id(open));
	g_assert_cmpuint(post(f, "/customer-credits/apply", form, &location), ==, 303);
	g_assert_cmpstr(location, ==, expected);
	g_assert_cmpint(remaining(f, goodwill), ==, 4500);
	g_assert_cmpint(balance(f, open), ==, 1500);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/customer-credits/on-the-page", Fixture, NULL, setup, test_credits_on_the_page, teardown);
	g_test_add("/customer-credits/apply-from-customer", Fixture, NULL, setup, test_apply_from_customer, teardown);
	g_test_add("/customer-credits/apply-refuses-too-much", Fixture, NULL, setup, test_apply_refuses_too_much, teardown);
	g_test_add("/customer-credits/apply-from-invoice", Fixture, NULL, setup, test_apply_from_invoice, teardown);
	g_test_add("/customer-credits/refuses-amount-currency", Fixture, NULL, setup, test_apply_refuses_currency, teardown);
	g_test_add("/customer-credits/refuses-invoice-currency", Fixture, GINT_TO_POINTER(1), setup, test_apply_refuses_currency, teardown);
	g_test_add("/customer-credits/return-stays-local", Fixture, NULL, setup, test_apply_return_stays_local, teardown);
	return g_test_run();
}
