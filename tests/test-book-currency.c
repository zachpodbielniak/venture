/*
 * test-book-currency.c - What an amount that names no currency is in
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The default organization keeps its books in EUR while the install's
 * default currency stays USD, which is exactly the install on which a
 * literal "USD" fallback reads as correct in every USD test and wrong in
 * production. Each writer below is given an amount or a document with no
 * currency and must land it in the organization's book currency
 * (venture_database_get_book_currency()). The one deliberate USD -- a US
 * return -- is pinned in test-tax-filing.c instead.
 */

#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*db;
	VentureContext	*context;
	gint64		 org;
	gint64		 company;
} Fixture;

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "clerk";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static void
set_book_currency(Fixture *f, const gchar *code)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GError) error = NULL;

	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, &error);
	g_assert_no_error(error);
	g_object_set(organization, "default-currency", code, NULL);
	save(f, organization);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL;

	(void)data;
	g_assert_cmpstr(venture_money_get_default_currency(), ==, "USD");
	f->config = venture_config_new();
	g_object_set(f->config, "payroll-enabled", TRUE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	set_book_currency(f, "EUR");
	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org,
		"name", "Customer", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static gint64
account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;

	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

static void
assert_currency(gpointer record, const gchar *property, const gchar *expected)
{
	g_autoptr(VentureMoney) money = NULL;

	g_object_get(record, property, &money, NULL);
	g_assert_nonnull(money);
	g_assert_cmpstr(venture_money_get_currency(money), ==, expected);
}

/*
 * The helper itself: the organization's book currency, and the install's
 * when there is no organization or it names none. If this regresses every
 * caller below inherits the wrong answer at once.
 */
static void
test_helper(Fixture *f, gconstpointer data)
{
	g_autofree gchar *book = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *missing = NULL;
	g_autofree gchar *unnamed = NULL;

	(void)data;
	book = venture_database_get_book_currency(f->db, f->org);
	none = venture_database_get_book_currency(f->db, 0);
	missing = venture_database_get_book_currency(f->db, 99999);
	g_assert_cmpstr(book, ==, "EUR");
	g_assert_cmpstr(none, ==, "USD");
	g_assert_cmpstr(missing, ==, "USD");
	set_book_currency(f, "");
	unnamed = venture_database_get_book_currency(f->db, f->org);
	g_assert_cmpstr(unnamed, ==, "USD");
}

/*
 * A record action's bare money parameter: collecting a retainer of "500"
 * is 500 euros in a euro organization. It used to be read as the install's
 * default and made a dollar retainer against euro books.
 */
static void
test_action_parameter(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GHashTable) parameters = NULL;
	g_autoptr(VentureEntity) retainer = NULL;
	g_autofree gchar *json = NULL;
	VentureActor actor;

	(void)data;
	actor_init(&actor);
	json = g_strdup_printf("{\"amount\":\"500\",\"liability_account_id\":%" G_GINT64_FORMAT "}",
		account_id(f, "2200"));
	node = venture_json_parse(json, &error);
	g_assert_no_error(error);
	parameters = venture_action_parameters_from_json(node, &error);
	g_assert_no_error(error);
	retainer = venture_action_registry_perform(venture_database_get_action_registry(f->db),
		"company", f->company, "collect_retainer", parameters, &actor, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error);
	g_assert_nonnull(retainer);
	assert_currency(retainer, "amount", "EUR");
}

/*
 * A payroll run that names no currency is in the book currency, and so
 * are its lines' bare amounts. Before, the run was labelled USD and the
 * lines parsed in the install's default, however the books were kept.
 */
static void
test_payroll(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYROLL_LINE);
	g_autoptr(VentureEntity) line = NULL;
	g_autofree gchar *currency = NULL;
	const gchar *csv =
		"employee,gross,employer_cost,deductions,net,liabilities\n"
		"Bea,2000.00,160.00,400.00,1600.00,560.00\n";

	(void)data;
	run = venture_payroll_service_import_csv(venture_payroll_service_get(f->db),
		f->org, "2026-02", "2026-02-01T00:00:00Z", "2026-03-01T00:00:00Z", NULL,
		csv, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(run);
	g_object_get(run, "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "EUR");
	venture_query_add_filter_int(query, "run-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(run), NULL);
	line = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(line);
	assert_currency(line, "gross", "EUR");
}

/*
 * A quote saved with no currency takes the book currency instead of being
 * refused (which every caller used to dodge by guessing "USD"), and the
 * quotes report reads the book currency's cohort unless told otherwise.
 */
static void
test_quote(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *currency = NULL;
	GPtrArray *metrics;
	guint i;
	gboolean seen = FALSE;

	(void)data;
	quote = g_object_new(VENTURE_TYPE_QUOTE, "organization-id", f->org,
		"number", "Q-1", "company-id", f->company, NULL);
	save(f, quote);
	g_object_get(quote, "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "EUR");

	result = venture_report_generate(venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "quotes"), f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	metrics = venture_report_result_get_metrics(result);
	for (i = 0; i < metrics->len; i++)
	{
		const VentureMoney *money = venture_metric_get_money(g_ptr_array_index(metrics, i));

		if (money == NULL)
			continue;
		g_assert_cmpstr(venture_money_get_currency(money), ==, "EUR");
		seen = TRUE;
	}
	g_assert_true(seen);
}

/*
 * Documents composed from a specification: a quote naming no currency is
 * in the book currency, and so are bare unit prices on its lines and on an
 * invoice's.
 */
static void
test_documents(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) spec = json_object_new();
	g_autoptr(JsonObject) line = json_object_new();
	g_autoptr(JsonArray) lines = json_array_new();
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) invoice_line = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *currency = NULL;
	VentureActor actor;

	(void)data;
	actor_init(&actor);
	json_object_set_string_member(line, "description", "Hours");
	json_object_set_int_member(line, "quantity", 2);
	json_object_set_string_member(line, "unit_price", "100");
	json_array_add_object_element(lines, json_object_ref(line));
	json_object_set_int_member(spec, "company_id", f->company);
	json_object_set_array_member(spec, "lines", json_array_ref(lines));

	quote = venture_document_service_compose_quote(venture_document_service_get(f->db),
		f->org, spec, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(quote);
	g_object_get(quote, "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "EUR");
	assert_currency(quote, "total", "EUR");

	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db),
		f->org, spec, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(invoice), NULL);
	invoice_line = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice_line);
	assert_currency(invoice_line, "unit-price", "EUR");
}

/*
 * A captured supplier invoice with no amount yet becomes a draft bill in
 * the book currency, not a dollar bill.
 */
static void
test_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *currency = NULL;

	(void)data;
	item = venture_capture_service_ingest_for_organization(venture_capture_service_get(f->db),
		f->org, "supplier_invoice", "Paper", "email", 0, "Customer", NULL, NULL, NULL,
		NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(item);
	json_object_set_int_member(options, "company_id", f->company);
	bill = venture_capture_service_convert(venture_capture_service_get(f->db),
		item, "vendor_bill", options, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bill);
	g_object_get(bill, "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "EUR");
}

/*
 * A budget whose organization names no currency is anchored on the
 * install's default, not on "USD": with the install in euros, the totals
 * are euro zeroes. (An organization that names one was read already.)
 */
static void
test_budget(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *metrics;
	guint i;

	(void)data;
	set_book_currency(f, "");
	venture_money_set_default_currency("EUR");
	result = venture_budget_service_vs_actual(venture_budget_service_get(f->db),
		f->org, "2026-01", NULL, &error);
	venture_money_set_default_currency("USD");
	g_assert_no_error(error);
	g_assert_nonnull(result);
	metrics = venture_report_result_get_metrics(result);
	g_assert_cmpuint(metrics->len, >, 0);
	for (i = 0; i < metrics->len; i++)
	{
		const VentureMoney *money = venture_metric_get_money(g_ptr_array_index(metrics, i));

		g_assert_nonnull(money);
		g_assert_cmpstr(venture_money_get_currency(money), ==, "EUR");
	}
}

/*
 * A manual journal through the generic action with bare amounts. The
 * lines decode in the install's default and are marked; the posting
 * service splits by currency and balances before anything is saved, so
 * the lines must be resolved first -- otherwise the header says USD, the
 * saved lines say EUR and the book amounts disagree with both. Every
 * journal, line amount and book amount is EUR.
 */
static void
test_journal_action(Fixture *f, gconstpointer data)
{
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL;
	guint i;
	JsonNode *node;

	(void)data;
	body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"source_type\":\"organization\","
		"\"source_id\":%" G_GINT64_FORMAT ",\"occurred_at\":\"2026-03-01\",\"lines\":["
		"{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"debit\",\"amount\":\"12.50\"},"
		"{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"credit\",\"amount\":\"12.50\"}]}",
		f->org, f->org, account_id(f, "1000"), account_id(f, "4000"));
	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	node = json_node_new(JSON_NODE_VALUE);
	json_node_set_string(node, body);
	g_hash_table_insert(params, g_strdup("journal"), node);
	result = venture_action_registry_perform(venture_database_get_action_registry(f->db),
		"journal", 0, "create_and_post", params, NULL, VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);

	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		"organization", f->org, f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(journals->len, ==, 1);
	{
		g_autofree gchar *currency = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(GPtrArray) lines = NULL;

		g_object_get(g_ptr_array_index(journals, 0), "currency", &currency, NULL);
		g_assert_cmpstr(currency, ==, "EUR");
		venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(g_ptr_array_index(journals, 0)), NULL);
		lines = venture_database_find(f->db, query, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(lines->len, ==, 2);
		for (i = 0; i < lines->len; i++)
		{
			assert_currency(g_ptr_array_index(lines, i), "amount", "EUR");
			assert_currency(g_ptr_array_index(lines, i), "book-amount", "EUR");
		}
	}
}

/*
 * Paying a bill with a bare amount: the payment is money against that
 * bill, so "10" is in the bill's currency -- here a dollar bill of a euro
 * organization. Read in the book currency it would be 10 EUR against a
 * USD bill and refused (or worse, paid in the wrong money).
 */
static void
test_bill_payment(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) approve = NULL;
	g_autoptr(VentureEntity) pay = NULL;
	g_autoptr(JsonNode) options = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-01-02", NULL);
	VenturePayablesService *service = venture_payables_service_get(f->db);

	(void)data;
	bill = g_object_new(VENTURE_TYPE_VENDOR_BILL, "organization-id", f->org, "number", "B-USD",
		"company-id", f->company, "currency", "USD", "status", "draft", NULL);
	g_assert_true(venture_entity_set_field_from_string(bill, "bill-date", "2026-01-01", &error));
	g_assert_true(venture_entity_set_field_from_string(bill, "due-date", "2026-01-31", &error));
	save(f, bill);
	line = g_object_new(VENTURE_TYPE_VENDOR_BILL_LINE, "organization-id", f->org,
		"bill-id", venture_entity_get_id(bill), "description", "Paper", "quantity", "1",
		"category", "supplies", NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "25 USD", &error));
	save(f, line);
	approve = venture_payables_service_prepare_action(service, venture_entity_get_id(bill), "approve", NULL, &error);
	g_assert_no_error(error);
	g_object_set(approve, "date", date, NULL);
	save(f, approve);

	options = venture_json_parse("{\"amount\":\"10\",\"date\":\"2026-01-03\"}", &error);
	g_assert_no_error(error);
	pay = venture_payables_service_prepare_action(service, venture_entity_get_id(bill), "pay", options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(pay);
	save(f, pay);
	assert_currency(pay, "amount", "USD");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/book-currency/helper", Fixture, NULL, setup, test_helper, teardown);
	g_test_add("/book-currency/action-parameter", Fixture, NULL, setup, test_action_parameter, teardown);
	g_test_add("/book-currency/payroll", Fixture, NULL, setup, test_payroll, teardown);
	g_test_add("/book-currency/quote", Fixture, NULL, setup, test_quote, teardown);
	g_test_add("/book-currency/documents", Fixture, NULL, setup, test_documents, teardown);
	g_test_add("/book-currency/capture", Fixture, NULL, setup, test_capture, teardown);
	g_test_add("/book-currency/budget", Fixture, NULL, setup, test_budget, teardown);
	g_test_add("/book-currency/bill-payment", Fixture, NULL, setup, test_bill_payment, teardown);
	g_test_add("/book-currency/journal-action", Fixture, NULL, setup, test_journal_action, teardown);
	return g_test_run();
}
