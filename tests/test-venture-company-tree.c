/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Parent accounts: a customer company with branches. A branch names its
 * parent, a loop is refused at save, and the parent's page can count its
 * deals, balance due and tickets with or without the branches. Each test
 * names the rule that breaks if it regresses.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 loss_reason;
} Fixture;

static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_assert_cmpint(venture_deal_service_ensure_default(venture_database_get_deal_service(f->db), f->org, &error), >, 0);
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) reason = g_object_new(VENTURE_TYPE_LOSS_REASON, "organization-id", f->org,
			"name", "Went elsewhere", "active", TRUE, NULL);
		g_assert_true(venture_database_save(f->db, reason, NULL, &error));
		g_assert_no_error(error);
		f->loss_reason = venture_entity_get_id(reason);
	}
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context); g_clear_object(&f->db); g_clear_object(&f->config);
}

static VentureEntity *record(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return g_object_new(type, "organization-id", f->org, NULL);
}
static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}
static void field(VentureEntity *row, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(row, name, value, &error));
	g_assert_no_error(error);
}
static gint64 company(Fixture *f, const gchar *name, gint64 parent)
{
	g_autoptr(VentureEntity) row = record(f, "company");
	g_object_set(row, "name", name, "parent-id", parent, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}
static void deal(Fixture *f, gint64 company_id, VentureDealStage stage, const gchar *value)
{
	g_autoptr(VentureEntity) row = record(f, "deal");
	g_object_set(row, "name", "Service contract", "company-id", company_id, "stage", stage, NULL);
	if (VENTURE_DEAL_STAGE_LOST == stage)
		g_object_set(row, "loss-reason-id", f->loss_reason, NULL);
	field(row, "value", value);
	save(f, row);
}
static void ticket(Fixture *f, gint64 company_id, VentureTicketStatus status)
{
	g_autoptr(VentureEntity) row = record(f, "ticket");
	g_object_set(row, "title", "Boiler check", "company-id", company_id, "status", status, NULL);
	save(f, row);
}
static gint64 invoice(Fixture *f, gint64 company_id, const gchar *price, gboolean send)
{
	g_autoptr(VentureEntity) row = record(f, "invoice");
	g_autoptr(VentureEntity) line = record(f, "invoice_line");
	g_autoptr(GDateTime) issued = venture_time_from_string("2026-03-01", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *number = g_strdup_printf("INV-T-%" G_GINT64_FORMAT "-%s", company_id, price);
	g_object_set(row, "number", number, "company-id", company_id, NULL);
	field(row, "issued-at", "2026-03-01");
	field(row, "due-at", "2026-03-31");
	save(f, row);
	g_object_set(line, "invoice-id", venture_entity_get_id(row), "description", "Service visit", "quantity", 1.0, NULL);
	field(line, "unit-price", price);
	save(f, line);
	if (send)
	{
		g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
			VENTURE_INVOICE(row), "sent", issued, NULL, &error));
		g_assert_no_error(error);
	}
	return venture_entity_get_id(row);
}
static gint64 minor(const VentureMoney *money)
{
	g_assert_nonnull(money);
	g_assert_cmpstr(venture_money_get_currency(money), ==, "USD");
	return venture_money_get_amount(money);
}

/* A -> B -> C; making C the parent of A would close a loop that every
 * rollup would then walk forever, so the save is refused and A keeps no
 * parent. A company cannot be its own parent either. */
static void test_loop_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) top = NULL;
	g_autoptr(VentureEntity) reread = NULL;
	g_autoptr(GError) error = NULL;
	gint64 a, b, c, parent = -1;
	(void)unused;
	a = company(f, "Northwind Holdings", 0);
	b = company(f, "Northwind East", a);
	c = company(f, "Northwind East Depot", b);

	top = venture_database_get(f->db, VENTURE_TYPE_COMPANY, a, NULL);
	g_object_set(top, "parent-id", c, NULL);
	g_assert_false(venture_database_save(f->db, top, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "loop"));
	g_clear_error(&error);

	g_object_set(top, "parent-id", a, NULL);
	g_assert_false(venture_database_save(f->db, top, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	reread = venture_database_get(f->db, VENTURE_TYPE_COMPANY, a, NULL);
	g_object_get(reread, "parent-id", &parent, NULL);
	g_assert_cmpint(parent, ==, 0);
}

/* The rollup on A counts A alone, or A with every branch beneath it:
 * open deals and their value, won revenue, open invoices and the balance
 * due, open tickets. Lost deals, drafts and finished tickets never count. */
static void test_rollup(Fixture *f, gconstpointer unused)
{
	VentureCompanyRollup alone = { 0 }, all = { 0 };
	g_autoptr(GPtrArray) children = NULL;
	g_autoptr(GError) error = NULL;
	gint64 a, b, c, other;
	(void)unused;
	a = company(f, "Northwind Holdings", 0);
	b = company(f, "Northwind East", a);
	c = company(f, "Northwind East Depot", b);
	other = company(f, "Unrelated Bakery", 0);

	deal(f, a, VENTURE_DEAL_STAGE_PROPOSAL, "50.00 USD");
	deal(f, b, VENTURE_DEAL_STAGE_LEAD, "100.00 USD");
	deal(f, c, VENTURE_DEAL_STAGE_WON, "250.00 USD");
	deal(f, c, VENTURE_DEAL_STAGE_LOST, "999.00 USD");
	deal(f, other, VENTURE_DEAL_STAGE_WON, "777.00 USD");
	invoice(f, a, "1000.00 USD", FALSE);
	invoice(f, b, "300.00 USD", TRUE);
	invoice(f, c, "200.00 USD", TRUE);
	invoice(f, other, "400.00 USD", TRUE);
	ticket(f, c, VENTURE_TICKET_STATUS_TODO);
	ticket(f, b, VENTURE_TICKET_STATUS_DONE);
	ticket(f, other, VENTURE_TICKET_STATUS_TODO);

	g_assert_true(venture_company_tree_rollup(f->db, a, FALSE, &alone, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(alone.companies, ==, 1);
	g_assert_cmpuint(alone.open_deals, ==, 1);
	g_assert_cmpint(minor(alone.open_deal_value), ==, 5000);
	g_assert_cmpuint(alone.won_deals, ==, 0);
	g_assert_cmpint(minor(alone.won_revenue), ==, 0);
	g_assert_cmpuint(alone.open_invoices, ==, 0);
	g_assert_cmpint(minor(alone.balance_due), ==, 0);
	g_assert_cmpuint(alone.open_tickets, ==, 0);

	g_assert_true(venture_company_tree_rollup(f->db, a, TRUE, &all, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(all.companies, ==, 3);
	g_assert_cmpuint(all.open_deals, ==, 2);
	g_assert_cmpint(minor(all.open_deal_value), ==, 15000);
	g_assert_cmpuint(all.won_deals, ==, 1);
	g_assert_cmpint(minor(all.won_revenue), ==, 25000);
	g_assert_cmpuint(all.open_invoices, ==, 2);
	g_assert_cmpint(minor(all.balance_due), ==, 50000);
	g_assert_cmpuint(all.open_tickets, ==, 1);

	children = venture_company_tree_children(f->db, a, &error);
	g_assert_no_error(error);
	g_assert_nonnull(children);
	g_assert_cmpuint(children->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(children, 0)), ==, b);

	venture_company_rollup_clear(&alone);
	venture_company_rollup_clear(&all);
}

/* --- The company page ------------------------------------------------------ */

typedef struct {
	GBytes *bytes;
	GError *error;
	gboolean done;
} Response;

static void response_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Response *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

/* The server shares this context, so the request runs on the loop. */
static gchar *get(VentureWebServer *server, const gchar *path)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	Response response;
	gchar *page;
	memset(&response, 0, sizeof(response));
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, response_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	page = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	g_bytes_unref(response.bytes);
	return page;
}

/* The parent's page lists its branches and shows the figures for the
 * company alone and including branches; a branch's page names its parent.
 * A company with neither shows no hierarchy card at all. */
static void test_company_page(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-company-tree-XXXXXX", NULL);
	g_autofree gchar *top = NULL, *branch = NULL, *alone = NULL;
	g_autofree gchar *path = NULL, *link = NULL;
	gint64 a, b, other;
	(void)unused;
	a = company(f, "Northwind Holdings", 0);
	b = company(f, "Northwind East", a);
	other = company(f, "Unrelated Bakery", 0);
	invoice(f, b, "300.00 USD", TRUE);
	ticket(f, b, VENTURE_TICKET_STATUS_TODO);

	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);

	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, a);
	top = get(server, path);
	link = g_strdup_printf("href=\"/e/company/%" G_GINT64_FORMAT "\"", b);
	g_assert_nonnull(strstr(top, "class=\"card company-tree\""));
	g_assert_nonnull(strstr(top, "Branches"));
	g_assert_nonnull(strstr(top, link));
	g_assert_nonnull(strstr(top, "Including branches"));
	g_assert_nonnull(strstr(top, "$300.00"));
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&link, g_free);

	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, b);
	branch = get(server, path);
	link = g_strdup_printf("href=\"/e/company/%" G_GINT64_FORMAT "\"", a);
	g_assert_nonnull(strstr(branch, "Part of"));
	g_assert_nonnull(strstr(branch, link));
	g_clear_pointer(&path, g_free);

	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, other);
	alone = get(server, path);
	g_assert_null(strstr(alone, "class=\"card company-tree\""));

	venture_web_server_stop(server);
	venture_test_remove_tree(dir);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/company-tree/loop-refused", Fixture, NULL, setup, test_loop_refused, teardown);
	g_test_add("/company-tree/rollup", Fixture, NULL, setup, test_rollup, teardown);
	g_test_add("/company-tree/company-page", Fixture, NULL, setup, test_company_page, teardown);
	return g_test_run();
}
