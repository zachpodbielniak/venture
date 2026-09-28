/*
 * test-bare-money.c - A bare amount through the generic writers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The default organization keeps its books in EUR on a USD install. An
 * amount written through the generic record machinery without a currency
 * -- a REST create or update, the record form, set_field_from_string as
 * the CLI and CSV import use it -- is the organization's money, so "12.50"
 * is 12.50 EUR. The decoder cannot know that (a record does not know its
 * organization's book currency), so it marks the field and the save reads
 * it again; these tests go through a real server to pin the whole trip.
 * An amount that names its currency is never touched.
 */

#include <venture.h>

#include <string.h>
#include <libsoup/soup.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	VentureWebServer *server;
	gchar		*state_dir;
	gint64		 org;
	gint64		 invoice;
} Fixture;

typedef struct
{
	gboolean	 done;
	GBytes		*bytes;
	GError		*error;
} Reply;

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
set_book_currency(Fixture *f, const gchar *code)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GError) error = NULL;

	organization = venture_database_get(f->database, VENTURE_TYPE_ORGANIZATION, f->org, &error);
	g_assert_no_error(error);
	g_object_set(organization, "default-currency", code, NULL);
	save(f, organization);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketListener) listener = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GError) error = NULL;
	guint16 port;

	(void)data;
	g_assert_cmpstr(venture_money_get_default_currency(), ==, "USD");
	f->config = venture_config_new();
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->org = venture_context_get_default_organization_id(f->context);
	set_book_currency(f, "EUR");

	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org, "name", "Buyer", NULL);
	save(f, company);
	invoice = g_object_new(VENTURE_TYPE_INVOICE, "organization-id", f->org,
		"number", "INV-EUR-1", "company-id", venture_entity_get_id(company), NULL);
	save(f, invoice);
	f->invoice = venture_entity_get_id(invoice);

	f->state_dir = g_dir_make_tmp("venture-bare-money-XXXXXX", &error);
	g_assert_no_error(error);
	listener = g_socket_listener_new();
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", f->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)port,
	             "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	(void)data;
	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
}

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                 result, &reply->error);
	reply->done = TRUE;
}

/*
 * Sends @body to @path with @method and returns the status, the reply in
 * @reply_body. Async on this thread's context, which the server shares.
 */
static guint
send(Fixture *f, const gchar *method, const gchar *path, const gchar *type,
	const gchar *body, gchar **reply_body)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	bytes = g_bytes_new(body, strlen(body));
	soup_message_set_request_body_from_bytes(message, type, bytes);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	if (soup_message_get_status(message) >= 400 && NULL != reply.bytes)
		g_test_message("%s: %.*s", path, (gint)g_bytes_get_size(reply.bytes),
		               (const gchar *)g_bytes_get_data(reply.bytes, NULL));
	if (NULL != reply_body && NULL != reply.bytes)
		*reply_body = g_strndup(g_bytes_get_data(reply.bytes, NULL), g_bytes_get_size(reply.bytes));
	g_clear_pointer(&reply.bytes, g_bytes_unref);
	return soup_message_get_status(message);
}

/* The id in a JSON reply's top-level "id". */
static gint64
reply_id(const gchar *reply)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;

	node = venture_json_parse(reply, &error);
	g_assert_no_error(error);
	return json_object_get_int_member(json_node_get_object(node), "id");
}

/* The newest invoice line on the fixture's invoice. */
static VentureEntity *
latest_line(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	g_autoptr(GError) error = NULL;
	VentureEntity *line;

	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, f->invoice, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	line = venture_database_find_one(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(line);
	return line;
}

static void
assert_money(gpointer record, const gchar *property, gint64 amount, const gchar *currency)
{
	g_autoptr(VentureMoney) money = NULL;

	g_object_get(record, property, &money, NULL);
	g_assert_nonnull(money);
	g_assert_cmpstr(venture_money_get_currency(money), ==, currency);
	g_assert_cmpint(venture_money_get_amount(money), ==, amount);
}

/*
 * REST create and update: "12.50" is 12.50 EUR, and an update of the
 * stored row with "20" is 20.00 EUR. If this regresses the API writes
 * dollar lines onto a euro invoice, and the invoice's total refuses to add.
 */
static void
test_rest(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *reply = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *updated = NULL;
	g_autoptr(VentureEntity) line = NULL;
	gint64 id;

	(void)data;
	body = g_strdup_printf("{\"invoice_id\":%" G_GINT64_FORMAT ",\"description\":\"Hours\","
		"\"quantity\":1,\"unit_price\":\"12.50\"}", f->invoice);
	g_assert_cmpuint(send(f, "POST", "/api/v1/invoice_lines", "application/json", body, &reply), ==, 201);
	id = reply_id(reply);
	line = venture_database_get(f->database, VENTURE_TYPE_INVOICE_LINE, id, NULL);
	g_assert_nonnull(line);
	assert_money(line, "unit-price", 1250, "EUR");
	g_clear_object(&line);

	path = g_strdup_printf("/api/v1/invoice_lines/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(send(f, "PATCH", path, "application/json", "{\"unit_price\":\"20\"}", &updated), ==, 200);
	line = venture_database_get(f->database, VENTURE_TYPE_INVOICE_LINE, id, NULL);
	assert_money(line, "unit-price", 2000, "EUR");
}

/*
 * An amount that names its currency is left as written, through the same
 * doors. An expense in dollars stays in dollars.
 */
static void
test_explicit(Fixture *f, gconstpointer data)
{
	g_autofree gchar *reply = NULL;
	g_autoptr(VentureEntity) expense = NULL;

	(void)data;
	g_assert_cmpuint(send(f, "POST", "/api/v1/expenses", "application/json",
		"{\"description\":\"Train\",\"amount\":\"7 USD\",\"occurred_at\":\"2026-03-01\"}",
		&reply), ==, 201);
	expense = venture_database_get(f->database, VENTURE_TYPE_EXPENSE, reply_id(reply), NULL);
	g_assert_nonnull(expense);
	assert_money(expense, "amount", 700, "USD");
}

/*
 * The record form: the same "12.50" typed into a browser.
 */
static void
test_form(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autoptr(VentureEntity) line = NULL;
	guint status;

	(void)data;
	body = g_strdup_printf("invoice-id=%" G_GINT64_FORMAT "&description=Form+hours&quantity=1&unit-price=12.50",
		f->invoice);
	status = send(f, "POST", "/e/invoice_line", "application/x-www-form-urlencoded", body, NULL);
	g_assert_cmpuint(status, <, 400);
	line = latest_line(f);
	assert_money(line, "unit-price", 1250, "EUR");
}

/*
 * The helper the CLI and CSV import reach, then a save: the mark lives on
 * the record, and a field set to something else before the save keeps the
 * something else.
 */
static void
test_set_field(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureMoney) explicit = venture_money_new_for_currency(300, "EUR");
	g_autoptr(GError) error = NULL;

	(void)data;
	line = g_object_new(VENTURE_TYPE_INVOICE_LINE, "organization-id", f->org,
		"invoice-id", f->invoice, "description", "Set", "quantity", 1.0, NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "4", &error));
	g_assert_no_error(error);
	g_assert_true(venture_entity_has_bare_money(line));
	save(f, line);
	g_assert_false(venture_entity_has_bare_money(line));
	assert_money(line, "unit-price", 400, "EUR");

	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "5", &error));
	g_object_set(line, "unit-price", explicit, NULL);
	save(f, line);
	stored = venture_database_get(f->database, VENTURE_TYPE_INVOICE_LINE, venture_entity_get_id(line), NULL);
	assert_money(stored, "unit-price", 300, "EUR");
}

/*
 * A line of a document that names its own currency is in that currency:
 * "25" on a dollar bill of a euro organization is 25 USD, found through
 * the line's reference to the bill. Read in the book currency it would
 * be 25 EUR, and the bill would refuse to total.
 */
static void
test_document_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *reply = NULL;

	(void)data;
	vendor = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org, "name", "Paper Co",
		"kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, vendor);
	bill = g_object_new(VENTURE_TYPE_VENDOR_BILL, "organization-id", f->org, "number", "B-1",
		"company-id", venture_entity_get_id(vendor), "currency", "USD", "status", "draft", NULL);
	g_assert_true(venture_entity_set_field_from_string(bill, "bill-date", "2026-01-01", &error));
	save(f, bill);
	body = g_strdup_printf("{\"bill_id\":%" G_GINT64_FORMAT ",\"description\":\"Paper\","
		"\"quantity\":\"1\",\"category\":\"supplies\",\"unit_price\":\"25\"}",
		venture_entity_get_id(bill));
	g_assert_cmpuint(send(f, "POST", "/api/v1/vendor_bill_lines", "application/json", body, &reply), ==, 201);
	line = venture_database_get(f->database, VENTURE_TYPE_VENDOR_BILL_LINE, reply_id(reply), NULL);
	g_assert_nonnull(line);
	assert_money(line, "unit-price", 2500, "USD");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/bare-money/rest", Fixture, NULL, set_up, test_rest, tear_down);
	g_test_add("/bare-money/explicit", Fixture, NULL, set_up, test_explicit, tear_down);
	g_test_add("/bare-money/form", Fixture, NULL, set_up, test_form, tear_down);
	g_test_add("/bare-money/document-currency", Fixture, NULL, set_up, test_document_currency, tear_down);
	g_test_add("/bare-money/set-field", Fixture, NULL, set_up, test_set_field, tear_down);
	return g_test_run();
}
