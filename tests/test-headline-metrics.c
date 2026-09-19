/*
 * test-headline-metrics.c - CAC, churn, LTV, LTV:CAC and the headline cards
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every formula here has a refusal: a zero it will not divide by, a
 * threshold below which it will not project. Each rule gets a fixture that
 * makes the number checkable by hand, and each refusal a case that shows
 * "n/a" where a zero would have been a lie.
 *
 * Several tests are worked examples from an audit of the first version:
 * each states the business, the number the first version gave and the
 * right one, so a regression shows up as the old wrong number.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, record, NULL, &error);
	if (!ok)
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(record), error != NULL ? error->message : "(no error)");
}

static VentureEntity *
record_new(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	VentureEntity *record;
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	record = g_object_new(type, NULL);
	venture_entity_set_organization_id(record, f->org);
	return record;
}

static void
field(VentureEntity *record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(record, name, value, &error));
	g_assert_no_error(error);
}

static gint64
integer(VentureEntity *record, const gchar *name)
{
	gint64 value = 0;
	g_object_get(record, name, &value, NULL);
	return value;
}

static gint64
company(Fixture *f, const gchar *name, const gchar *source)
{
	g_autoptr(VentureEntity) record = record_new(f, "company");
	g_object_set(record, "name", name, "source", source, NULL);
	save(f, record);
	return venture_entity_get_id(record);
}

/* An issued invoice for @amount, not yet paid. */
static gint64
issued_invoice(Fixture *f, gint64 company_id, const gchar *number, const gchar *amount, const gchar *issued)
{
	g_autoptr(VentureEntity) invoice = record_new(f, "invoice");
	g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
	g_object_set(invoice, "number", number, "company-id", company_id, NULL);
	field(invoice, "issued-at", issued);
	field(invoice, "due-at", issued);
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", amount);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	return venture_entity_get_id(invoice);
}

/* A receipt through the settlement service: applied to @invoice_id when
 * one is given, else held as a deposit. */
static gint64
pay(Fixture *f, gint64 company_id, gint64 invoice_id, const gchar *amount, const gchar *date)
{
	g_autoptr(VentureEntity) payment = record_new(f, "payment");
	g_object_set(payment, "customer-id", company_id, "invoice-id", invoice_id, "method", "manual", NULL);
	field(payment, "amount", amount);
	field(payment, "date", date);
	save(f, payment);
	return venture_entity_get_id(payment);
}

/* An issued invoice settled in full by a receipt on @paid: the customer's
 * first cash is what CAC counts, and the receipt is what LTV counts, so both
 * come from the settlement service rather than a status. */
static gint64
paid_invoice(Fixture *f, gint64 company_id, const gchar *number, const gchar *amount, const gchar *paid)
{
	g_autoptr(VentureEntity) stored = NULL;
	gint status = 0;
	gint64 invoice = issued_invoice(f, company_id, number, amount, paid);
	pay(f, company_id, invoice, amount, paid);
	stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE, invoice, NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_PAID);
	return invoice;
}

static VentureEntity *
find_one(Fixture *f, GType type, const gchar *field_name, gint64 value)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	VentureEntity *found;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_int(query, field_name, VENTURE_FILTER_OP_EQ, value, &error));
	g_assert_true(venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, &error));
	found = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	return found;
}

static void
expense(Fixture *f, const gchar *what, const gchar *amount, const gchar *when, gboolean acquisition)
{
	g_autoptr(VentureEntity) record = record_new(f, "expense");
	g_object_set(record, "description", what, "acquisition", acquisition, NULL);
	field(record, "amount", amount);
	field(record, "occurred-at", when);
	save(f, record);
}

static void
cost_of_revenue(Fixture *f, const gchar *what, const gchar *amount, const gchar *when)
{
	g_autoptr(VentureEntity) record = record_new(f, "expense");
	g_object_set(record, "description", what, "cost-of-revenue", TRUE, NULL);
	field(record, "amount", amount);
	field(record, "occurred-at", when);
	save(f, record);
}

static gint64
campaign(Fixture *f, const gchar *name, const gchar *spend, const gchar *started, const gchar *ended)
{
	g_autoptr(VentureEntity) record = record_new(f, "campaign");
	g_object_set(record, "name", name, NULL);
	field(record, "spend", spend);
	field(record, "started-at", started);
	if (ended != NULL)
		field(record, "ended-at", ended);
	save(f, record);
	return venture_entity_get_id(record);
}

static void
settings(Fixture *f, gint64 minimum_months, const gchar *hourly_rate)
{
	g_autoptr(VentureEntity) setting = record_new(f, "headline_setting");
	g_object_set(setting, "minimum-customer-months", minimum_months, NULL);
	if (hourly_rate != NULL)
		field(setting, "hourly-rate", hourly_rate);
	save(f, setting);
}

static VentureReportResult *
run_report(Fixture *f, const gchar *name, const gchar *period_text, JsonObject *options)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	VentureReportResult *result;
	g_assert_nonnull(report);
	period = venture_context_parse_period(f->context, period_text, &error);
	g_assert_no_error(error);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static VentureMetric *
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *candidate = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(candidate), key) == 0)
			return candidate;
	}
	g_error("no metric %s", key);
	return NULL;
}

static gboolean
has_metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;
	for (i = 0; i < metrics->len; i++)
		if (g_strcmp0(venture_metric_get_key(g_ptr_array_index(metrics, i)), key) == 0)
			return TRUE;
	return FALSE;
}

static gint64
money_metric(VentureReportResult *result, const gchar *key)
{
	const VentureMoney *amount = venture_metric_get_money(metric(result, key));
	g_assert_nonnull(amount);
	return venture_money_get_amount(amount);
}

static const gchar *
text_metric(VentureReportResult *result, const gchar *key)
{
	return venture_metric_get_text(metric(result, key));
}

static gchar *
cell(VentureReportResult *result, guint row, const gchar *key)
{
	return venture_report_result_format_cell(result, row, key);
}

/* The row whose @key column reads @value, or G_MAXUINT. */
static guint
row_where(VentureReportResult *result, const gchar *key, const gchar *value)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		g_autofree gchar *text = cell(result, i, key);
		if (g_strcmp0(text, value) == 0)
			return i;
	}
	return G_MAXUINT;
}

static gboolean
has_note(VentureReportResult *result, const gchar *fragment)
{
	g_autoptr(JsonNode) node = venture_report_result_to_json(result);
	g_autofree gchar *text = venture_json_to_string(node, FALSE);
	return strstr(text, fragment) != NULL;
}

/* --- Billing ---------------------------------------------------------------- */

static gint64
plan_price(Fixture *f, const gchar *code, const gchar *interval, const gchar *amount, gboolean per_seat)
{
	g_autoptr(VentureEntity) plan = record_new(f, "plan");
	g_autoptr(VentureEntity) price = record_new(f, "plan_price");
	g_object_set(plan, "name", code, "code", code, "active", TRUE, NULL);
	save(f, plan);
	g_object_set(price, "plan-id", venture_entity_get_id(plan), "currency", "USD", "active", TRUE,
		"per-seat", per_seat, NULL);
	field(price, "interval", interval);
	field(price, "amount", amount);
	save(f, price);
	return venture_entity_get_id(price);
}

/* A billing action; @out_invoice receives the invoice it issued, if any. */
static gint64
billing(Fixture *f, const gchar *action, gint64 subscription, gint64 company_id, gint64 price,
	const gchar *date, gint64 *out_invoice)
{
	g_autoptr(VentureEntity) request = record_new(f, "billing_request");
	g_object_set(request, "action", action, "subscription-id", subscription, NULL);
	if (company_id != 0)
		g_object_set(request, "company-id", company_id, "plan-price-id", price, NULL);
	field(request, "at", date);
	save(f, request);
	if (out_invoice != NULL)
		*out_invoice = integer(request, "invoice-id");
	return integer(request, "subscription-id");
}

/* --- 1. Classification ----------------------------------------------------- */

/* Classification must survive the same metadata and save path as money;
 * changing a category later must not silently rewrite historical spend. */
static void
test_acquisition(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureExpense) expense_record = venture_expense_new();
	g_autoptr(VentureVendorBillLine) line = venture_vendor_bill_line_new();
	g_autoptr(VentureTaxCategory) category = venture_tax_category_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(2500, "USD");
	g_autoptr(GDateTime) date = venture_time_from_string("2026-01-15", NULL);
	gboolean acquisition = TRUE;
	gboolean cost = TRUE;
	static const gchar *const flags[] = { "acquisition", "cost-of-revenue" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(flags); i++)
	{
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(expense_record), flags[i]));
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(line), flags[i]));
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(category), flags[i]));
	}
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(expense_record), "campaign-id"));
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(line), "campaign-id"));
	g_object_get(expense_record, "acquisition", &acquisition, "cost-of-revenue", &cost, NULL);
	g_assert_false(acquisition);
	g_assert_false(cost);
	g_object_get(line, "acquisition", &acquisition, NULL);
	g_assert_false(acquisition);
	g_object_set(category, "organization-id", f->org, "name", "Customer acquisition",
		"code", "headline-acquisition", "acquisition", TRUE, "cost-of-revenue", TRUE, NULL);
	save(f, VENTURE_ENTITY(category));
	g_object_set(expense_record, "organization-id", f->org, "description", "Acquisition tooling",
		"amount", amount, "occurred-at", date,
		"tax-category-id", venture_entity_get_id(VENTURE_ENTITY(category)), NULL);
	save(f, VENTURE_ENTITY(expense_record));
	stored = venture_database_get(f->db, VENTURE_TYPE_EXPENSE,
		venture_entity_get_id(VENTURE_ENTITY(expense_record)), NULL);
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(stored), FALSE);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(json), "acquisition"));
	g_assert_true(json_object_get_boolean_member(json_node_get_object(json), "cost_of_revenue"));
	g_object_set(stored, "acquisition", FALSE, "cost-of-revenue", FALSE, NULL);
	save(f, stored);
	g_clear_object(&stored);
	stored = venture_database_get(f->db, VENTURE_TYPE_EXPENSE,
		venture_entity_get_id(VENTURE_ENTITY(expense_record)), NULL);
	g_object_get(stored, "acquisition", &acquisition, "cost-of-revenue", &cost, NULL);
	g_assert_false(acquisition);
	g_assert_false(cost);

	/* Bill lines inherit from the category code, and only on create. */
	{
		g_autoptr(VentureEntity) vendor = record_new(f, "company");
		g_autoptr(VentureEntity) bill = record_new(f, "vendor_bill");
		g_autoptr(VentureEntity) bill_line = record_new(f, "vendor_bill_line");
		g_autoptr(VentureEntity) stored_line = NULL;

		g_object_set(vendor, "name", "Ads vendor", "kind",
			VENTURE_COMPANY_KIND_SUPPLIER, NULL);
		save(f, vendor);
		g_object_set(bill, "company-id", venture_entity_get_id(vendor),
			"number", "ACQ-1", "currency", "USD", "status", "draft", NULL);
		field(bill, "bill-date", "2026-01-15");
		save(f, bill);
		g_object_set(bill_line, "bill-id", venture_entity_get_id(bill),
			"description", "Ads", "quantity", "1", "category",
			"headline-acquisition", NULL);
		field(bill_line, "unit-price", "40 USD");
		save(f, bill_line);
		stored_line = venture_database_get(f->db, VENTURE_TYPE_VENDOR_BILL_LINE,
			venture_entity_get_id(bill_line), NULL);
		g_object_get(stored_line, "acquisition", &acquisition, "cost-of-revenue", &cost, NULL);
		g_assert_true(acquisition);
		g_assert_true(cost);
		g_object_set(stored_line, "acquisition", FALSE, NULL);
		save(f, stored_line);
		g_clear_object(&stored_line);
		stored_line = venture_database_get(f->db, VENTURE_TYPE_VENDOR_BILL_LINE,
			venture_entity_get_id(bill_line), NULL);
		g_object_get(stored_line, "acquisition", &acquisition, NULL);
		g_assert_false(acquisition);
	}
}

/* --- 2. CAC ---------------------------------------------------------------- */

static void
test_cac(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) january = NULL;
	g_autoptr(VentureReportResult) empty = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *row_campaign = NULL;
	g_autofree gchar *row_customers = NULL;
	gint64 a = company(f, "Acme", NULL);
	gint64 b = company(f, "Bolt", NULL);
	gint64 c = company(f, "Cog", "referral");
	/* Spring ran 28 days, ten in January and eighteen in February: $280 is
	 * $100 and $180. The dates are calendar dates, as the campaign form
	 * writes them, so they meet the periods' midnight-UTC boundaries. */
	gint64 spring = campaign(f, "Spring", "280 USD", "2026-01-22", "2026-02-19");
	guint web_row;
	guint referral_row;

	/* A: first paid in February, via a converted web lead on Spring. */
	paid_invoice(f, a, "A-1", "50 USD", "2026-02-05");
	/* B: paid in January already, so February's invoice is not a first. */
	paid_invoice(f, b, "B-1", "50 USD", "2026-01-10");
	paid_invoice(f, b, "B-2", "50 USD", "2026-02-20");
	/* C: first paid in February, no lead; the company's own source. */
	paid_invoice(f, c, "C-1", "50 USD", "2026-02-25");

	lead = record_new(f, "lead");
	g_object_set(lead, "name", "Acme inquiry", "source", "web", "campaign-id", spring,
		"status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	json_object_set_int_member(options, "company_id", a);
	json_object_set_boolean_member(options, "deal", FALSE);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(converted);

	expense(f, "Ads", "200 USD", "2026-02-10", TRUE);
	expense(f, "Rent", "1000 USD", "2026-02-11", FALSE);
	expense(f, "March ads", "999 USD", "2026-03-03", TRUE);

	/* Flagged bill lines of issued bills count; drafts and unflagged
	 * lines do not. Status is derived, so the issued one is approved. */
	{
		g_autoptr(VentureEntity) vendor = record_new(f, "company");
		g_autoptr(VentureEntity) issued = record_new(f, "vendor_bill");
		g_autoptr(VentureEntity) draft = record_new(f, "vendor_bill");
		g_autoptr(VentureEntity) issued_line = record_new(f, "vendor_bill_line");
		g_autoptr(VentureEntity) other_line = record_new(f, "vendor_bill_line");
		g_autoptr(VentureEntity) draft_line = record_new(f, "vendor_bill_line");
		g_autoptr(VentureEntity) event = NULL;
		gint64 vendor_id;

		g_object_set(vendor, "name", "Printer", "kind",
			VENTURE_COMPANY_KIND_SUPPLIER, NULL);
		save(f, vendor);
		vendor_id = venture_entity_get_id(vendor);
		g_object_set(issued, "company-id", vendor_id, "number", "BILL-1",
			"currency", "USD", "status", "draft", NULL);
		field(issued, "bill-date", "2026-02-12");
		save(f, issued);
		g_object_set(issued_line, "bill-id", venture_entity_get_id(issued),
			"description", "Flyers", "quantity", "1", "acquisition", TRUE,
			NULL);
		field(issued_line, "unit-price", "50 USD");
		save(f, issued_line);
		g_object_set(other_line, "bill-id", venture_entity_get_id(issued),
			"description", "Paper", "quantity", "1", "acquisition", FALSE,
			NULL);
		field(other_line, "unit-price", "25 USD");
		save(f, other_line);
		event = record_new(f, "vendor_bill_event");
		g_object_set(event, "bill-id", venture_entity_get_id(issued),
			"vendor-id", vendor_id, "kind", "approve", "state", "approved",
			NULL);
		field(event, "date", "2026-02-12");
		save(f, event);
		g_object_set(draft, "company-id", vendor_id, "number", "BILL-DRAFT",
			"currency", "USD", "status", "draft", NULL);
		field(draft, "bill-date", "2026-02-13");
		save(f, draft);
		g_object_set(draft_line, "bill-id", venture_entity_get_id(draft),
			"description", "Unsent ads", "quantity", "1", "acquisition", TRUE,
			NULL);
		field(draft_line, "unit-price", "999 USD");
		save(f, draft_line);
	}

	/* February: Spring's eighteen days (180) plus Ads (200) and Flyers (50)
	 * is 430 over A and C: 215. The first version counted the whole 280 in
	 * February, the month the campaign started. */
	result = run_report(f, "cac", "2026-02", NULL);
	g_assert_cmpint(money_metric(result, "spend"), ==, 43000);
	g_assert_cmpint(money_metric(result, "campaign_spend"), ==, 18000);
	g_assert_cmpint(money_metric(result, "expense_spend"), ==, 25000);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "new_customers")), ==, 2.0);
	g_assert_cmpint(money_metric(result, "cac"), ==, 21500);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	web_row = row_where(result, "source", "web");
	referral_row = row_where(result, "source", "referral");
	g_assert_cmpuint(web_row, ==, 0);
	g_assert_cmpuint(referral_row, ==, 1);
	row_campaign = cell(result, web_row, "campaign");
	g_assert_cmpstr(row_campaign, ==, "Spring");
	row_customers = cell(result, web_row, "new_customers");
	g_assert_cmpstr(row_customers, ==, "1");
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, web_row, "cac"))), ==, 18000);

	/* January: Spring's ten days, and B. */
	january = run_report(f, "cac", "2026-01", NULL);
	g_assert_cmpint(money_metric(january, "campaign_spend"), ==, 10000);
	g_assert_cmpint(money_metric(january, "cac"), ==, 10000);

	/* March: spend, but nobody new. Reported, never divided. */
	empty = run_report(f, "cac", "2026-03", NULL);
	g_assert_cmpint(money_metric(empty, "spend"), ==, 99900);
	g_assert_cmpfloat(venture_metric_get_number(metric(empty, "new_customers")), ==, 0.0);
	g_assert_null(venture_metric_get_money(metric(empty, "cac")));
	g_assert_cmpstr(venture_metric_get_text(metric(empty, "cac")), ==, "n/a");
}

/*
 * A new customer is the company's first cash, not an invoice status.
 *
 * January: $1,000 of acquisition spend; A pays a $2,000 invoice; B's $5,000
 * invoice is written off. The first version counted invoices that reached
 * "paid" by their paid_at, and a write-off makes an invoice paid: two new
 * customers and a CAC of $500. Only A paid anything: one, and $1,000.
 *
 * A $50 refund to A in March cleared A's paid_at, and the first version
 * moved A out of January after the fact. First cash, once received, stays.
 *
 * C pays a $900 invoice in two instalments, February and March. The first
 * version made C new in March, when the last instalment turned the invoice
 * paid; C arrived in February.
 */
static void
test_cac_first_cash(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) january = NULL;
	g_autoptr(VentureReportResult) later = NULL;
	g_autoptr(VentureReportResult) february = NULL;
	g_autoptr(VentureReportResult) march = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(GDateTime) written = venture_time_from_string("2026-01-25", NULL);
	g_autoptr(GError) error = NULL;
	gint64 a = company(f, "Alder", NULL);
	gint64 b = company(f, "Birch", NULL);
	gint64 c = company(f, "Cedar", NULL);
	gint64 a_invoice;
	gint64 b_invoice;
	gint64 c_invoice;

	expense(f, "January ads", "1000 USD", "2026-01-05", TRUE);
	a_invoice = paid_invoice(f, a, "ALDER-1", "2000 USD", "2026-01-20");
	b_invoice = issued_invoice(f, b, "BIRCH-1", "5000 USD", "2026-01-12");
	g_assert_true(venture_settlement_service_write_off(venture_settlement_service_get(f->db),
		b_invoice, written, NULL, &error));
	g_assert_no_error(error);
	c_invoice = issued_invoice(f, c, "CEDAR-1", "900 USD", "2026-02-04");
	pay(f, c, c_invoice, "300 USD", "2026-02-06");
	pay(f, c, c_invoice, "600 USD", "2026-03-06");

	january = run_report(f, "cac", "2026-01", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(january, "new_customers")), ==, 1.0);
	g_assert_cmpint(money_metric(january, "cac"), ==, 100000);

	february = run_report(f, "cac", "2026-02", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(february, "new_customers")), ==, 1.0);
	march = run_report(f, "cac", "2026-03", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(march, "new_customers")), ==, 0.0);

	allocation = find_one(f, VENTURE_TYPE_PAYMENT_ALLOCATION, "invoice-id", a_invoice);
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", a, "allocation-id", venture_entity_get_id(allocation), NULL);
	field(refund, "amount", "50 USD");
	field(refund, "date", "2026-03-10");
	save(f, refund);
	later = run_report(f, "cac", "2026-01", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(later, "new_customers")), ==, 1.0);
	g_assert_cmpint(money_metric(later, "cac"), ==, 100000);
}

/*
 * Campaign spend is counted once. Summer's own spend figure says $999, but
 * its costs are recorded as an expense of $60 and a bill line of $40 that
 * name it: the rows are the spend and the figure is ignored. The bill line
 * is later paid, and the payables service projects that payment into a cash
 * expense copying its category -- which the category default flags
 * acquisition again. The first version counted the campaign figure, the
 * rows, and the projection at the payment date.
 */
static void
test_cac_double_count(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) april = NULL;
	g_autoptr(VentureReportResult) may = NULL;
	g_autoptr(VentureEntity) category = record_new(f, "tax_category");
	g_autoptr(VentureEntity) vendor = record_new(f, "company");
	g_autoptr(VentureEntity) bill = record_new(f, "vendor_bill");
	g_autoptr(VentureEntity) line = record_new(f, "vendor_bill_line");
	g_autoptr(VentureEntity) linked = record_new(f, "expense");
	g_autoptr(VentureEntity) approve = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) projection = record_new(f, "expense");
	g_autofree gchar *external = NULL;
	gint64 summer = campaign(f, "Summer", "999 USD", "2026-04-01T00:00:00-04:00", "2026-04-30T00:00:00-04:00");
	gint64 buyer = company(f, "Dune", NULL);
	gboolean flagged = FALSE;

	paid_invoice(f, buyer, "DUNE-1", "100 USD", "2026-04-20");
	g_object_set(category, "name", "Advertising", "code", "ads", "acquisition", TRUE, NULL);
	save(f, category);
	g_object_set(linked, "description", "Summer banners", "campaign-id", summer, NULL);
	field(linked, "amount", "60 USD");
	field(linked, "occurred-at", "2026-04-10");
	save(f, linked);
	g_object_set(vendor, "name", "Billboards", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, vendor);
	g_object_set(bill, "company-id", venture_entity_get_id(vendor), "number", "SUMMER-1",
		"currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-04-15");
	save(f, bill);
	g_object_set(line, "bill-id", venture_entity_get_id(bill), "description", "Billboard",
		"quantity", "1", "category", "ads", "campaign-id", summer, NULL);
	field(line, "unit-price", "40 USD");
	save(f, line);
	approve = record_new(f, "vendor_bill_event");
	g_object_set(approve, "bill-id", venture_entity_get_id(bill), "vendor-id", venture_entity_get_id(vendor),
		"kind", "approve", "state", "approved", NULL);
	field(approve, "date", "2026-04-15");
	save(f, approve);
	payment = record_new(f, "bill_payment");
	g_object_set(payment, "vendor-id", venture_entity_get_id(vendor), "bill-id", venture_entity_get_id(bill),
		"method", "transfer", NULL);
	field(payment, "amount", "40 USD");
	field(payment, "date", "2026-05-05");
	save(f, payment);
	external = g_strdup_printf("bill_line:%s", venture_entity_get_uuid(line));
	g_object_set(projection, "external-id", external, "description", "Billboard", NULL);
	field(projection, "occurred-at", "2026-05-05");
	save(f, projection);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->db, VENTURE_TYPE_EXPENSE,
			venture_entity_get_id(projection), NULL);
		g_object_get(stored, "acquisition", &flagged, NULL);
		g_assert_true(flagged);
	}

	april = run_report(f, "cac", "2026-04", NULL);
	g_assert_cmpint(money_metric(april, "campaign_spend"), ==, 10000);
	g_assert_cmpint(money_metric(april, "expense_spend"), ==, 0);
	g_assert_cmpint(money_metric(april, "spend"), ==, 10000);
	may = run_report(f, "cac", "2026-05", NULL);
	g_assert_cmpint(money_metric(may, "spend"), ==, 0);
}

/*
 * The breakdown splits a campaign's spend across its sources. Fall cost
 * $300 and brought three customers, two from the web and one from a
 * referral: $200 and $100, each row a CAC of $100. The first version gave
 * each row the whole $300.
 */
static void
test_cac_breakdown(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	gint64 fall = campaign(f, "Fall", "300 USD", "2026-09-01T00:00:00-04:00", "2026-09-30T00:00:00-04:00");
	static const gchar *const sources[] = { "web", "web", "referral" };
	guint web;
	guint referral;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(sources); i++)
	{
		g_autoptr(VentureEntity) lead = record_new(f, "lead");
		g_autoptr(VentureEntity) converted = NULL;
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(GError) error = NULL;
		g_autofree gchar *name = g_strdup_printf("Fall %u", i);
		g_autofree gchar *number = g_strdup_printf("FALL-%u", i);
		gint64 customer = company(f, name, NULL);

		g_object_set(lead, "name", name, "source", sources[i], "campaign-id", fall,
			"status", VENTURE_LEAD_QUALIFIED, NULL);
		save(f, lead);
		json_object_set_int_member(options, "company_id", customer);
		json_object_set_boolean_member(options, "deal", FALSE);
		converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, options, NULL, &error);
		g_assert_no_error(error);
		paid_invoice(f, customer, number, "10 USD", "2026-09-15");
	}

	result = run_report(f, "cac", "2026-09", NULL);
	g_assert_cmpint(money_metric(result, "spend"), ==, 30000);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	web = row_where(result, "source", "web");
	referral = row_where(result, "source", "referral");
	g_assert_cmpuint(web, !=, G_MAXUINT);
	g_assert_cmpuint(referral, !=, G_MAXUINT);
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, web, "spend"))), ==, 20000);
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, referral, "spend"))), ==, 10000);
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, web, "cac"))), ==, 10000);
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, referral, "cac"))), ==, 10000);
}

/* A bill line costs its net plus only the tax that cannot be recovered:
 * recoverable input tax comes back and was never acquisition spend. */
static void
test_cac_recoverable_tax(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) recoverable = record_new(f, "tax_code");
	g_autoptr(VentureEntity) consumed = record_new(f, "tax_code");
	g_autoptr(VentureEntity) vendor = record_new(f, "company");
	g_autoptr(VentureEntity) bill = record_new(f, "vendor_bill");
	g_autoptr(VentureEntity) event = record_new(f, "vendor_bill_event");
	gint64 buyer = company(f, "Elm", NULL);
	static const gchar *const codes[] = { "recoverable", "consumed" };
	guint i;

	g_object_set(recoverable, "code", "VAT-R", "name", "Recoverable VAT", "jurisdiction", "EU",
		"rate-numerator", (gint64)20, "rate-denominator", (gint64)100, "recoverable", TRUE, "active", TRUE, NULL);
	save(f, recoverable);
	g_object_set(consumed, "code", "VAT-N", "name", "Non-recoverable VAT", "jurisdiction", "EU",
		"rate-numerator", (gint64)20, "rate-denominator", (gint64)100, "recoverable", FALSE, "active", TRUE, NULL);
	save(f, consumed);
	g_object_set(vendor, "name", "Ad network", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, vendor);
	g_object_set(bill, "company-id", venture_entity_get_id(vendor), "number", "VAT-ADS",
		"currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-06-10");
	save(f, bill);
	for (i = 0; i < G_N_ELEMENTS(codes); i++)
	{
		g_autoptr(VentureEntity) line = record_new(f, "vendor_bill_line");
		g_object_set(line, "bill-id", venture_entity_get_id(bill), "description", codes[i],
			"quantity", "1", "acquisition", TRUE,
			"tax-code-id", venture_entity_get_id(i == 0 ? recoverable : consumed), NULL);
		field(line, "unit-price", "100 USD");
		field(line, "tax-amount", "20 USD");
		save(f, line);
	}
	g_object_set(event, "bill-id", venture_entity_get_id(bill), "vendor-id", venture_entity_get_id(vendor),
		"kind", "approve", "state", "approved", NULL);
	field(event, "date", "2026-06-10");
	save(f, event);
	paid_invoice(f, buyer, "ELM-1", "10 USD", "2026-06-12");

	/* 100 for the recoverable line, 120 for the other. */
	result = run_report(f, "cac", "2026-06", NULL);
	g_assert_cmpint(money_metric(result, "spend"), ==, 22000);
}

/* --- 3. Customer cash ------------------------------------------------------ */

/*
 * Cash is what the settlement service called cash. A $500 invoice paid with
 * $600, the extra $100 refunded: $500. The first version subtracted every
 * refund, including of the $100 it never counted, and made it $400.
 */
static void
test_cash_overpayment(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) refund = record_new(f, "refund");
	gint64 buyer = company(f, "Fir", NULL);
	gint64 invoice = issued_invoice(f, buyer, "FIR-1", "500 USD", "2026-03-01");
	gint64 payment = pay(f, buyer, invoice, "600 USD", "2026-03-02");

	credit = find_one(f, VENTURE_TYPE_CUSTOMER_CREDIT, "payment-id", payment);
	g_object_set(refund, "customer-id", buyer, "credit-id", venture_entity_get_id(credit), NULL);
	field(refund, "amount", "100 USD");
	field(refund, "date", "2026-03-05");
	save(f, refund);

	result = run_report(f, "ltv", "2026-03", NULL);
	g_assert_cmpint(money_metric(result, "realised_total"), ==, 50000);
}

/*
 * A $6,000 retainer taken as a deposit and applied to twelve $500 invoices
 * is twelve receipts of $500 on the dates they were applied. The first
 * version skipped every application of a credit, so the customer had no
 * cash at all -- missing from LTV and churn -- while their "paid" invoices
 * still made them new in CAC.
 */
static void
test_cash_retainer(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) ltv = NULL;
	g_autoptr(VentureReportResult) cac = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	gint64 buyer = company(f, "Grove", NULL);
	gint64 payment = pay(f, buyer, 0, "6000 USD", "2025-07-01");
	guint month;

	credit = find_one(f, VENTURE_TYPE_CUSTOMER_CREDIT, "payment-id", payment);
	for (month = 0; month < 12; month++)
	{
		g_autoptr(VentureEntity) allocation = record_new(f, "payment_allocation");
		g_autofree gchar *number = g_strdup_printf("GROVE-%u", month);
		g_autofree gchar *date = g_strdup_printf("%04u-%02u-10", 2025 + (7 + month - 1) / 12, (7 + month - 1) % 12 + 1);
		gint64 invoice = issued_invoice(f, buyer, number, "500 USD", date);

		g_object_set(allocation, "credit-id", venture_entity_get_id(credit), "invoice-id", invoice, NULL);
		field(allocation, "amount", "500 USD");
		field(allocation, "date", date);
		save(f, allocation);
	}

	ltv = run_report(f, "ltv", "2026-06", NULL);
	g_assert_cmpint(money_metric(ltv, "realised_total"), ==, 600000);
	g_assert_cmpfloat(venture_metric_get_number(metric(ltv, "customer_months")), ==, 12.0);
	cac = run_report(f, "cac", "2025-07", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(cac, "new_customers")), ==, 1.0);
}

/*
 * A foreign invoice settles in book currency: a 100 EUR invoice paid with
 * 105 USD is $105 of cash and one customer-month. The first version summed
 * the 100 EUR allocation, refused it as another currency, and still counted
 * the customer-month -- lowering revenue per customer-month.
 */
static void
test_cash_foreign(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	static const gchar *const rates[][3] = { { "110", "2026-01-10", "EUR-1" }, { "105", "2026-02-01", NULL } };
	gint64 buyer = company(f, "Hazel", NULL);
	gint64 invoice = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rates); i++)
	{
		g_autoptr(VentureEntity) rate = record_new(f, "exchange_rate");
		g_object_set(rate, "from-currency", "EUR", "to-currency", "USD",
			"rate-numerator", g_ascii_strtoll(rates[i][0], NULL, 10), "rate-denominator", (gint64)100,
			"source", "manual", "reason", "board rate", NULL);
		field(rate, "effective-at", rates[i][1]);
		save(f, rate);
		if (rates[i][2] != NULL)
			invoice = issued_invoice(f, buyer, rates[i][2], "100 EUR", rates[i][1]);
	}
	pay(f, buyer, invoice, "105 USD", "2026-02-01");

	result = run_report(f, "ltv", "2026-02", NULL);
	g_assert_cmpint(money_metric(result, "realised_total"), ==, 10500);
	g_assert_cmpint(money_metric(result, "revenue_12m"), ==, 10500);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "customer_months")), ==, 1.0);
	g_assert_cmpint(money_metric(result, "arpu"), ==, 10500);
}

/* --- 4. Churn -------------------------------------------------------------- */

static gint64
schedule(Fixture *f, const gchar *name, gint64 company_id, const gchar *start, const gchar *end)
{
	g_autoptr(VentureEntity) record = record_new(f, "recurring_schedule");
	g_autofree gchar *template = g_strdup_printf("{\"company_id\":%" G_GINT64_FORMAT ",\"lines\":[]}", company_id);
	g_object_set(record, "name", name, "kind", 0, "frequency", 0, "timezone", "UTC", NULL);
	field(record, "start-at", start);
	if (end != NULL)
		field(record, "end-at", end);
	field(record, "template", template);
	save(f, record);
	return venture_entity_get_id(record);
}

static gchar *
days_ago(gint days)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) then = g_date_time_add_days(now, -days);
	return venture_time_to_string(then);
}

static void
cancel_schedule(Fixture *f, gint64 id)
{
	g_autoptr(VentureEntity) cancelled = venture_database_get(f->db, VENTURE_TYPE_RECURRING_SCHEDULE, id, NULL);
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_delete(f->db, cancelled, NULL, &error));
	g_assert_no_error(error);
}

/*
 * A soft delete is stamped with the moment it happens, so the cancelled
 * schedule is cancelled now and the period is this month; the rest of the
 * fixture is laid out relative to today for the same reason.
 */
static void
test_churn(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) loose = NULL;
	g_autoptr(VentureReportResult) bare = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *long_ago = days_ago(400);
	g_autofree gchar *quiet_since = days_ago(200);
	g_autofree gchar *weeks_ago = days_ago(45);
	g_autofree gchar *just_now = days_ago(0);
	g_autofree gchar *lapsed_at = NULL;
	gint64 x = company(f, "Xeno", NULL);
	gint64 y = company(f, "Yarn", NULL);
	gint64 z = company(f, "Zed", NULL);
	gint64 gone;
	guint rows;
	guint i;
	gboolean saw_activity = FALSE;

	/* Nothing yet: neither ratio has a denominator. */
	bare = run_report(f, "customer_churn", "this_month", NULL);
	g_assert_cmpstr(venture_metric_get_text(metric(bare, "recurring_churn")), ==, "n/a");
	g_assert_cmpstr(venture_metric_get_text(metric(bare, "activity_churn")), ==, "n/a");

	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(GDateTime) minute_ago = g_date_time_add_minutes(now, -1);
		lapsed_at = venture_time_to_string(minute_ago);
	}

	/* Four invoice schedules: three running before the month opened --
	 * one cancelled now, one lapsing a minute ago, one running on -- and
	 * one that only started today. */
	gone = schedule(f, "Gone", x, long_ago, NULL);
	schedule(f, "Lapsed", y, long_ago, lapsed_at);
	schedule(f, "Stays", z, long_ago, NULL);
	schedule(f, "New", z, just_now, NULL);
	cancel_schedule(f, gone);

	/* Cash: X paid 200 days ago only, Y 45 days ago, Z today. */
	paid_invoice(f, x, "X-1", "80 USD", quiet_since);
	paid_invoice(f, y, "Y-1", "80 USD", weeks_ago);
	paid_invoice(f, z, "Z-1", "80 USD", just_now);

	result = run_report(f, "customer_churn", "this_month", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "active_at_start")), ==, 3.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "churned")), ==, 2.0);
	/* 2 of 3 is 6,666.67 basis points, rounded half to even: 6,667. The
	 * first version truncated it to 0.6666. */
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "recurring_churn")), ==, 0.6667);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "active_customers")), ==, 3.0);
	/*
	 * Activity: X's single receipt plus the 90-day threshold fell 110 days
	 * ago, inside the window -- one event. Customer-months at risk: X was
	 * paid up (one receipt covers 31 days) at the start of the month that
	 * opened six months ago; Y at the start of the month that opened one
	 * month ago; Z joined today, too late for any. One event over two
	 * customer-months: 50%. The first version divided quiet customers by
	 * everyone who paid (1 of 3, 0.3333) and called that the churn rate.
	 */
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "inactive_customers")), ==, 1.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "customer_months")), ==, 2.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "activity_churn")), ==, 0.5);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "days")), ==, 90.0);

	rows = venture_report_result_get_row_count(result);
	g_assert_cmpuint(rows, ==, 3);
	for (i = 0; i < rows; i++)
	{
		g_autofree gchar *row_kind = cell(result, i, "kind");
		g_autofree gchar *row_who = cell(result, i, "customer");
		g_autofree gchar *row_what = cell(result, i, "detail");
		if (g_strcmp0(row_kind, "activity") == 0)
		{
			saw_activity = TRUE;
			g_assert_cmpstr(row_who, ==, "Xeno");
		}
		else if (g_strcmp0(row_who, "Yarn") == 0)
			g_assert_cmpstr(row_what, ==, "lapsed");
		else
		{
			g_assert_cmpstr(row_who, ==, "Xeno");
			g_assert_cmpstr(row_what, ==, "cancelled");
		}
	}
	g_assert_true(saw_activity);

	/* A looser threshold -- 300 quiet days -- keeps X active. */
	json_object_set_int_member(options, "days", 300);
	loose = run_report(f, "customer_churn", "this_month", options);
	g_assert_cmpfloat(venture_metric_get_number(metric(loose, "inactive_customers")), ==, 0.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(loose, "activity_churn")), ==, 0.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(loose, "days")), ==, 300.0);
}

/*
 * Recurring churn counts customers. Kite has three schedules and cancels
 * one; Lark's only schedule is paused; Moss swaps its schedule for another;
 * Nest cancels everything. Lark is paused, which is neither running nor
 * gone, so the cohort is Kite, Moss and Nest, and only Nest has left: one
 * of three, 33.33%. The first version counted schedules, Lark's paused one
 * among the running: three cancelled or replaced of six, 50%.
 */
static void
test_churn_recurring_customers(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *long_ago = days_ago(400);
	g_autofree gchar *just_now = days_ago(0);
	gint64 kite = company(f, "Kite", NULL);
	gint64 lark = company(f, "Lark", NULL);
	gint64 moss = company(f, "Moss", NULL);
	gint64 nest = company(f, "Nest", NULL);
	gint64 cancelled;
	gint64 swapped;
	gint64 last;

	cancelled = schedule(f, "Kite A", kite, long_ago, NULL);
	schedule(f, "Kite B", kite, long_ago, NULL);
	schedule(f, "Kite C", kite, long_ago, NULL);
	{
		g_autoptr(VentureEntity) paused = NULL;
		gint64 id = schedule(f, "Lark", lark, long_ago, NULL);
		paused = venture_database_get(f->db, VENTURE_TYPE_RECURRING_SCHEDULE, id, NULL);
		g_object_set(paused, "paused", TRUE, NULL);
		/* Paused now, so it was not running when the month opened either:
		 * a flag with no date cannot say otherwise. */
		save(f, paused);
	}
	swapped = schedule(f, "Moss old", moss, long_ago, NULL);
	last = schedule(f, "Nest", nest, long_ago, NULL);
	cancel_schedule(f, cancelled);
	cancel_schedule(f, swapped);
	schedule(f, "Moss new", moss, just_now, NULL);
	cancel_schedule(f, last);

	result = run_report(f, "customer_churn", "this_month", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "active_at_start")), ==, 3.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "churned")), ==, 1.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "recurring_churn")), ==, 0.3333);
	g_assert_cmpuint(row_where(result, "customer", "Nest"), !=, G_MAXUINT);
	g_assert_cmpuint(row_where(result, "customer", "Kite"), ==, G_MAXUINT);
	g_assert_cmpuint(row_where(result, "customer", "Moss"), ==, G_MAXUINT);
}

/*
 * The worked example for monthly churn: fifty monthly customers, one leaving
 * and one joining every month, is 2% a month.
 *
 * Each customer is on a monthly subscription, so their rhythm is known and
 * only their first and last receipts matter. The period is June 2026; the
 * window is July 2025 to June 2026. Thirty-seven customers pay throughout.
 * Fourteen leavers paid last on the 15th of each month from April 2025 to
 * May 2026; twelve joiners paid first on the 15th of each month from June
 * 2025 to May 2026. At the start of every month of the window exactly 50
 * are paid up: 600 customer-months. The leavers whose last receipt was
 * April 2025 to March 2026 went quiet (90 days later) inside the window:
 * 12 events. 12 / 600 = 2.00%.
 *
 * The first version divided customers quiet at the end (9) by everyone who
 * paid in the year (60) times twelve: 1.25%, and every lifetime projected
 * from it 60% too long.
 */
static void
test_churn_monthly_worked_example(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) ltv = NULL;
	gint64 price = plan_price(f, "monthly", "month", "30 USD", FALSE);
	guint customer = 0;
	guint i;

	for (i = 0; i < 63; i++)
	{
		g_autofree gchar *name = g_strdup_printf("Monthly %u", i);
		g_autofree gchar *first = NULL;
		g_autofree gchar *last = NULL;
		gint64 buyer = company(f, name, NULL);
		gint64 invoice = 0;

		if (i < 37)
		{
			first = g_strdup("2025-01-15");
			last = g_strdup("2026-06-15");
		}
		else if (i < 37 + 14)
		{
			guint month = i - 37;
			first = g_strdup("2025-01-15");
			last = g_strdup_printf("%04u-%02u-15", 2025 + (3 + month) / 12, (3 + month) % 12 + 1);
		}
		else
		{
			guint month = i - 37 - 14;
			first = g_strdup_printf("%04u-%02u-15", 2025 + (5 + month) / 12, (5 + month) % 12 + 1);
			last = g_strdup("2026-06-15");
		}

		billing(f, "start", 0, buyer, price, first, &invoice);
		g_assert_cmpint(invoice, >, 0);
		pay(f, buyer, invoice, "10 USD", first);
		pay(f, buyer, invoice, "10 USD", last);
		customer++;
	}
	g_assert_cmpuint(customer, ==, 63);

	result = run_report(f, "customer_churn", "2026-06", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "inactive_customers")), ==, 12.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "customer_months")), ==, 600.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "activity_churn")), ==, 0.02);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "activity_churn_bps")), ==, 200.0);
	ltv = run_report(f, "ltv", "2026-06", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(ltv, "monthly_churn")), ==, 0.02);
}

/*
 * The worked example for revenue per customer-month: twelve annual
 * customers at $1,200, one renewing each month, nobody leaving.
 *
 * Each receipt pays for the twelve months from the one it arrived in, so
 * inside the trailing year every customer contributes exactly $1,200 over
 * twelve customer-months: $100 a customer-month, and no churn. The first
 * version counted each receipt in its own month only -- $1,200 a
 * customer-month -- and, with a 90-day quiet threshold, called nine of the
 * twelve churned: 75%, and an LTV of $19,200 for a business losing nobody.
 */
static void
test_ltv_annual_worked_example(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) ltv = NULL;
	g_autoptr(VentureReportResult) churn = NULL;
	g_autoptr(VentureReportResult) mrr = NULL;
	g_autoptr(VentureReportResult) ratio = NULL;
	gint64 price = plan_price(f, "annual", "year", "1200 USD", FALSE);
	guint c;

	for (c = 0; c < 12; c++)
	{
		g_autofree gchar *name = g_strdup_printf("Annual %u", c);
		g_autofree gchar *started = g_strdup_printf("%04u-%02u-15", 2024 + (6 + c) / 12, (6 + c) % 12 + 1);
		g_autofree gchar *renewed = g_strdup_printf("%04u-%02u-15", 2025 + (6 + c) / 12, (6 + c) % 12 + 1);
		gint64 buyer = company(f, name, NULL);
		gint64 invoice = 0;
		gint64 subscription;

		subscription = billing(f, "start", 0, buyer, price, started, &invoice);
		pay(f, buyer, invoice, "1200 USD", started);
		invoice = 0;
		billing(f, "renew", subscription, 0, 0, renewed, &invoice);
		g_assert_cmpint(invoice, >, 0);
		pay(f, buyer, invoice, "1200 USD", renewed);
	}

	ltv = run_report(f, "ltv", "2026-06", NULL);
	g_assert_cmpint(money_metric(ltv, "revenue_12m"), ==, 1440000);
	g_assert_cmpfloat(venture_metric_get_number(metric(ltv, "customer_months")), ==, 144.0);
	g_assert_cmpint(money_metric(ltv, "arpu"), ==, 10000);
	g_assert_cmpfloat(venture_metric_get_number(metric(ltv, "churn_events")), ==, 0.0);
	g_assert_cmpstr(text_metric(ltv, "projected"), ==, "n/a");
	churn = run_report(f, "customer_churn", "2026-06", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(churn, "inactive_customers")), ==, 0.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(churn, "activity_churn")), ==, 0.0);

	/* ARPA, from billing: $1,200 of MRR over twelve paying companies. */
	mrr = run_report(f, "mrr", "2026-06", NULL);
	g_assert_cmpint(money_metric(mrr, "mrr"), ==, 120000);
	g_assert_cmpfloat(venture_metric_get_number(metric(mrr, "customers")), ==, 12.0);
	g_assert_cmpint(money_metric(mrr, "arpa"), ==, 10000);
	ratio = run_report(f, "ltv_cac", "2026-06", NULL);
	g_assert_cmpint(money_metric(ratio, "arpa"), ==, 10000);
}

/* --- 5. LTV ---------------------------------------------------------------- */

static void
test_ltv(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) projected = NULL;
	g_autoptr(VentureReportResult) quiet = NULL;
	g_autoptr(VentureEntity) twin = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) worklog = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *top = NULL;
	gint64 p = company(f, "Pine", NULL);
	gint64 q = company(f, "Quill", NULL);
	gint64 r = company(f, "Reed", NULL);
	gint64 first;

	/* Realised: P 300 less a 50 refund, Q 200, R 40 (quiet since last
	 * August). Total 490, mean 163.33, median 200 (Q sits in the middle). */
	first = paid_invoice(f, p, "P-1", "100 USD", "2026-03-10");
	paid_invoice(f, p, "P-2", "200 USD", "2026-04-10");
	paid_invoice(f, q, "Q-1", "200 USD", "2026-05-10");
	paid_invoice(f, r, "R-1", "40 USD", "2025-08-10");

	allocation = find_one(f, VENTURE_TYPE_PAYMENT_ALLOCATION, "invoice-id", first);
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", p, "allocation-id", venture_entity_get_id(allocation), NULL);
	field(refund, "amount", "50 USD");
	field(refund, "date", "2026-04-20");
	save(f, refund);

	/* Support hours on Q's ticket, priced later. */
	ticket = record_new(f, "ticket");
	g_object_set(ticket, "title", "Help", "company-id", q, NULL);
	save(f, ticket);
	worklog = record_new(f, "worklog");
	g_object_set(worklog, "ticket-id", venture_entity_get_id(ticket), "hours", 2.5, "author", "zach", NULL);
	field(worklog, "occurred-at", "2026-05-12");
	save(f, worklog);

	result = run_report(f, "ltv", "2026-06", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "customers")), ==, 3.0);
	g_assert_cmpint(money_metric(result, "realised_total"), ==, 49000);
	g_assert_cmpint(money_metric(result, "realised_mean"), ==, 16333);
	g_assert_cmpint(money_metric(result, "realised_median"), ==, 20000);
	top = cell(result, 0, "customer");
	g_assert_cmpstr(top, ==, "Pine");
	/* Four customer-months (P x2, Q, R) is below the default twelve. */
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "customer_months")), ==, 4.0);
	g_assert_null(venture_metric_get_money(metric(result, "projected")));
	g_assert_cmpstr(venture_metric_get_text(metric(result, "projected")), ==, "insufficient data");
	/* No rate: support costs nothing yet. */
	g_assert_cmpint(money_metric(result, "support_cost"), ==, 0);

	/* Lower the bar and price the hours; the projection appears. */
	settings(f, 3, "20 USD");
	twin = record_new(f, "headline_setting");
	g_assert_false(venture_database_save(f->db, twin, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	projected = run_report(f, "ltv", "2026-06", NULL);
	/*
	 * Support cost: 2.5 h x 20 = 50, cost of revenue. Trailing-year revenue
	 * 490 (the P&L's cash sales are the same receipts less the refund):
	 * margin (490 - 50) / 490 = 8,979.59 bps, rounded half to even: 8,980
	 * (the first version truncated to 8,979). ARPU 490 / 4 = 122.50;
	 * 122.50 x 0.8980 = 110.005, rounded once: 110.00.
	 *
	 * Monthly churn: R's receipt plus 90 days fell in the window, one event.
	 * Customer-months at risk: R at the start of September; P at the starts
	 * of April and May; Q at the start of June -- four. 1 / 4 is 25%, so
	 * the lifetime is 110.00 x 4 = 440.00. The first version's 0.0277 (one
	 * quiet customer over three who paid, over twelve months) and its
	 * 3,959.64 made a four-month business look like a three-year one.
	 */
	g_assert_cmpint(money_metric(projected, "support_cost"), ==, 5000);
	g_assert_cmpint(money_metric(projected, "cost_of_revenue"), ==, 5000);
	g_assert_cmpint(money_metric(projected, "arpu"), ==, 12250);
	g_assert_cmpfloat(venture_metric_get_number(metric(projected, "gross_margin")), ==, 0.898);
	g_assert_cmpfloat(venture_metric_get_number(metric(projected, "gross_margin_bps")), ==, 8980.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(projected, "monthly_churn")), ==, 0.25);
	g_assert_cmpfloat(venture_metric_get_number(metric(projected, "churn_events")), ==, 1.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(projected, "risk_months")), ==, 4.0);
	g_assert_cmpint(money_metric(projected, "projected"), ==, 44000);

	/* With nobody quiet, lifetime is unbounded: n/a, not a number. */
	{
		g_autoptr(JsonObject) options = json_object_new();
		json_object_set_int_member(options, "days", 400);
		quiet = run_report(f, "ltv", "2026-06", options);
		g_assert_cmpstr(venture_metric_get_text(metric(quiet, "projected")), ==, "n/a");
	}
}

/*
 * Gross margin is revenue less cost of revenue, not the whole P&L. Revenue
 * $120,000; hosting $20,000 flagged cost of revenue; advertising $40,000
 * flagged acquisition; salaries $50,000 and rent $20,000. Margin is
 * (120,000 - 20,000) / 120,000 = 83.33%. The first version subtracted every
 * expense -- advertising included, which CAC already charges -- and got
 * -8.33%, a negative LTV and a negative LTV:CAC.
 */
static void
test_ltv_margin(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) negative = NULL;
	gint64 buyer = company(f, "Iris", NULL);
	g_autoptr(VentureEntity) category = record_new(f, "tax_category");

	paid_invoice(f, buyer, "IRIS-1", "120000 USD", "2026-01-15");
	/* Hosting is flagged through its category's default. */
	g_object_set(category, "name", "Hosting", "code", "hosting", "cost-of-revenue", TRUE, NULL);
	save(f, category);
	{
		g_autoptr(VentureEntity) hosting = record_new(f, "expense");
		g_object_set(hosting, "description", "Hosting", "tax-category-id", venture_entity_get_id(category), NULL);
		field(hosting, "amount", "20000 USD");
		field(hosting, "occurred-at", "2026-02-01");
		save(f, hosting);
	}
	expense(f, "Advertising", "40000 USD", "2026-02-02", TRUE);
	expense(f, "Salaries", "50000 USD", "2026-02-03", FALSE);
	expense(f, "Rent", "20000 USD", "2026-02-04", FALSE);

	result = run_report(f, "ltv", "2026-06", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "gross_margin_bps")), ==, 8333.0);
	g_assert_cmpint(money_metric(result, "cost_of_revenue"), ==, 2000000);

	/* Costs of revenue beyond revenue: no margin, and no projection from
	 * one. A negative lifetime value is not a number to run a business on. */
	cost_of_revenue(f, "Fulfilment", "150000 USD", "2026-03-01");
	settings(f, 1, NULL);
	negative = run_report(f, "ltv", "2026-06", NULL);
	g_assert_cmpstr(text_metric(negative, "gross_margin"), ==, "n/a");
	g_assert_false(has_metric(negative, "gross_margin_bps"));
	g_assert_cmpstr(text_metric(negative, "projected"), ==, "n/a");
	g_assert_true(has_note(negative, "at or below zero"));
}

/* --- 6. LTV:CAC ------------------------------------------------------------ */

static void
test_ltv_cac(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) bare = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	gint64 p = company(f, "Pine", NULL);
	gint64 q = company(f, "Quill", NULL);
	gint64 r = company(f, "Reed", NULL);

	paid_invoice(f, p, "P-1", "100 USD", "2026-03-10");
	paid_invoice(f, p, "P-2", "200 USD", "2026-04-10");
	paid_invoice(f, q, "Q-1", "200 USD", "2026-06-10");
	paid_invoice(f, r, "R-1", "40 USD", "2025-08-10");
	expense(f, "Ads", "100 USD", "2026-06-02", TRUE);

	/* LTV withheld: the ratio is n/a even though CAC is 100. */
	bare = run_report(f, "ltv_cac", "2026-06", NULL);
	g_assert_cmpint(money_metric(bare, "cac"), ==, 10000);
	g_assert_cmpstr(venture_metric_get_text(metric(bare, "ltv_cac")), ==, "insufficient data");

	settings(f, 3, NULL);

	/*
	 * Revenue 540. The ads are acquisition spend, which CAC divides, so they
	 * are not cost of revenue: nothing is, and the margin is 100% (the
	 * first version's 81.48% charged the ads twice). ARPU 540 / 4 = 135.
	 * Churn: R went quiet in the window, one event; P was at risk at the
	 * starts of April and May, R of September, and Q joined in June, too
	 * late: three customer-months. LTV 135 x 1.00 x 3 = 405.00. Over CAC
	 * 100: 4.05. The first version said 3,960 and 39.6.
	 *
	 * ARPA without billing is revenue per customer-month, 135; payback is
	 * 100 / (135 x 1.00) = 0.7407 months, rounded half to even: 0.74.
	 */
	result = run_report(f, "ltv_cac", "2026-06", NULL);
	g_assert_cmpint(money_metric(result, "projected_ltv"), ==, 40500);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "ltv_cac")), ==, 4.05);
	g_assert_cmpint(money_metric(result, "arpa"), ==, 13500);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "payback_months")), ==, 0.74);
	{
		g_autoptr(VentureReportResult) ltv = run_report(f, "ltv", "2026-06", NULL);
		g_assert_cmpfloat(venture_metric_get_number(metric(ltv, "gross_margin")), ==, 1.0);
		g_assert_true(has_note(ltv, "gross margin is 100%"));
	}

	/* No new customer in May: CAC n/a, so the ratio and payback are too. */
	{
		g_autoptr(VentureReportResult) may = run_report(f, "ltv_cac", "2026-05", NULL);
		g_assert_cmpstr(venture_metric_get_text(metric(may, "cac")), ==, "n/a");
		g_assert_cmpstr(venture_metric_get_text(metric(may, "ltv_cac")), ==, "n/a");
		g_assert_cmpstr(venture_metric_get_text(metric(may, "payback_months")), ==, "n/a");
	}
}

/* --- 6b. Cohorts ------------------------------------------------------------ */

/*
 * January's cohort is A (paying January to March), B (January only) and C
 * (a year paid in January); February's is D (February and April) and E
 * (February and March). Followed to the end of April: January keeps 100%,
 * 66.67%, 66.67%, 33.33%; February 100%, 50%, 50%, and May is blank.
 * Month 1 pools both cohorts, 3 of 5; month 3 only January's, 1 of 3.
 */
static void
test_cohorts(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	static const gchar *const receipts[][2] = {
		{ "A", "2026-01-15" }, { "A", "2026-02-15" }, { "A", "2026-03-15" },
		{ "B", "2026-01-15" },
		{ "D", "2026-02-15" }, { "D", "2026-04-15" },
		{ "E", "2026-02-15" }, { "E", "2026-03-15" }
	};
	static const gchar *const january[] = { "100.0%", "66.7%", "66.7%", "33.3%", "" };
	static const gchar *const february[] = { "100.0%", "50.0%", "50.0%", "", "" };
	gint64 price = plan_price(f, "yearly", "year", "1200 USD", FALSE);
	gint64 annual = company(f, "C", NULL);
	gint64 invoice = 0;
	guint row;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(receipts); i++)
	{
		g_autofree gchar *number = g_strdup_printf("COHORT-%u", i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
		g_autoptr(VentureEntity) existing = NULL;
		gint64 buyer;

		venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, receipts[i][0], NULL);
		existing = venture_database_find_one(f->db, query, NULL);
		buyer = (existing != NULL) ? venture_entity_get_id(existing) : company(f, receipts[i][0], NULL);
		paid_invoice(f, buyer, number, "10 USD", receipts[i][1]);
	}
	billing(f, "start", 0, annual, price, "2026-01-15", &invoice);
	pay(f, annual, invoice, "1200 USD", "2026-01-15");

	json_object_set_string_member(options, "as_of", "2026-04-30");
	result = run_report(f, "customer_cohorts", "2026-01-01..2026-02-28", options);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "cohorts")), ==, 2.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "customers")), ==, 5.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "month_1")), ==, 0.6);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "month_3")), ==, 0.3333);
	g_assert_cmpstr(text_metric(result, "month_6"), ==, "n/a");
	row = row_where(result, "cohort", "2026-01");
	g_assert_cmpuint(row, ==, 0);
	for (i = 0; i < G_N_ELEMENTS(january); i++)
	{
		g_autofree gchar *column = g_strdup_printf("m%u", i);
		g_autofree gchar *jan = cell(result, 0, column);
		g_autofree gchar *feb = cell(result, 1, column);
		g_assert_cmpstr(jan, ==, january[i]);
		g_assert_cmpstr(feb, ==, february[i]);
	}
}

/* --- 7. Scope: venture and as_of ------------------------------------------- */

/*
 * venture_id narrows every source and as_of every read, and both reach the
 * P&L the margin comes from. The first version ignored both silently: a
 * venture's CAC was the whole portfolio's.
 */
static void
test_scope(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) books = record_new(f, "venture");
	g_autoptr(VentureEntity) shop = record_new(f, "venture");
	g_autoptr(JsonObject) books_only = json_object_new();
	g_autoptr(JsonObject) before = json_object_new();
	g_autoptr(VentureReportResult) all = NULL;
	g_autoptr(VentureReportResult) scoped = NULL;
	g_autoptr(VentureReportResult) early = NULL;
	gint64 reader = company(f, "Reader", NULL);
	gint64 shopper = company(f, "Shopper", NULL);

	g_object_set(books, "name", "Books", "slug", "books", NULL);
	save(f, books);
	g_object_set(shop, "name", "Shop", "slug", "shop", NULL);
	save(f, shop);
	/* An invoice's venture is fixed at issue, and its cash sales carry it. */
	{
		g_autoptr(VentureEntity) draft = record_new(f, "invoice");
		g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
		g_object_set(draft, "number", "BOOK-2", "company-id", reader, "venture-id", venture_entity_get_id(books), NULL);
		field(draft, "issued-at", "2026-06-06");
		save(f, draft);
		g_object_set(line, "invoice-id", venture_entity_get_id(draft), "description", "Book", "quantity", 1.0, NULL);
		field(line, "unit-price", "40 USD");
		save(f, line);
		g_object_set(draft, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		save(f, draft);
		pay(f, reader, venture_entity_get_id(draft), "40 USD", "2026-06-20");
	}
	{
		g_autoptr(VentureEntity) draft = record_new(f, "invoice");
		g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
		g_object_set(draft, "number", "SHOP-1", "company-id", shopper, "venture-id", venture_entity_get_id(shop), NULL);
		field(draft, "issued-at", "2026-06-06");
		save(f, draft);
		g_object_set(line, "invoice-id", venture_entity_get_id(draft), "description", "Mug", "quantity", 1.0, NULL);
		field(line, "unit-price", "60 USD");
		save(f, line);
		g_object_set(draft, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		save(f, draft);
		pay(f, shopper, venture_entity_get_id(draft), "60 USD", "2026-06-08");
	}
	{
		g_autoptr(VentureEntity) ads = record_new(f, "expense");
		g_object_set(ads, "description", "Book ads", "acquisition", TRUE, "venture-id", venture_entity_get_id(books), NULL);
		field(ads, "amount", "30 USD");
		field(ads, "occurred-at", "2026-06-03");
		save(f, ads);
	}
	expense(f, "Portfolio ads", "70 USD", "2026-06-04", TRUE);

	all = run_report(f, "cac", "2026-06", NULL);
	g_assert_cmpint(money_metric(all, "spend"), ==, 10000);
	g_assert_cmpfloat(venture_metric_get_number(metric(all, "new_customers")), ==, 2.0);

	json_object_set_int_member(books_only, "venture_id", venture_entity_get_id(books));
	scoped = run_report(f, "cac", "2026-06", books_only);
	g_assert_cmpint(money_metric(scoped, "spend"), ==, 3000);
	g_assert_cmpfloat(venture_metric_get_number(metric(scoped, "new_customers")), ==, 1.0);
	g_assert_cmpint(money_metric(scoped, "cac"), ==, 3000);

	/* As of the 10th, Reader had not paid for the book yet. */
	json_object_set_string_member(before, "as_of", "2026-06-10");
	early = run_report(f, "ltv", "2026-06", before);
	g_assert_cmpint(money_metric(early, "realised_total"), ==, 6000);
}

/* --- 8. Comparison periods ------------------------------------------------- */

static void
assert_comparison(VentureDateRange *range, const gchar *now_text, const gchar *start, const gchar *end)
{
	g_autoptr(GDateTime) now = now_text != NULL ? venture_time_from_string(now_text, NULL) : NULL;
	g_autoptr(VentureDateRange) previous = venture_date_range_comparison_period(range, now);
	g_autoptr(GDateTime) expected_start = venture_time_from_string(start, NULL);
	g_autoptr(GDateTime) expected_end = venture_time_from_string(end, NULL);
	g_assert_nonnull(previous);
	if (!g_date_time_equal(venture_date_range_get_start(previous), expected_start) ||
	    !g_date_time_equal(venture_date_range_get_end(previous), expected_end))
	{
		g_autofree gchar *got_start = venture_time_to_string(venture_date_range_get_start(previous));
		g_autofree gchar *got_end = venture_time_to_string(venture_date_range_get_end(previous));
		g_error("comparison is %s..%s, expected %s..%s", got_start, got_end, start, end);
	}
}

/*
 * A month is compared with the month before it, found on the calendar. The
 * first version subtracted the length: September compared with 2 August to
 * 1 September, March (with its lost daylight-saving hour) with 29 January to
 * 1 March, and "this month" on the third with all of the month before -- a
 * red arrow at the start of every month.
 */
static void
test_comparison_period(void)
{
	g_autoptr(GTimeZone) york = g_time_zone_new_identifier("America/New_York");
	g_autoptr(VentureDateRange) september = venture_date_range_new_month(2026, 9, york);
	g_autoptr(VentureDateRange) march = venture_date_range_new_month(2026, 3, york);
	g_autoptr(VentureDateRange) november = venture_date_range_new_month(2026, 11, york);
	g_autoptr(VentureDateRange) quarter = venture_date_range_new_quarter(2026, 2, york);
	g_autoptr(VentureDateRange) year = venture_date_range_new_year(2026, york);
	g_autoptr(VentureDateRange) all = venture_date_range_new_all_time();
	g_autoptr(GDateTime) sep1 = g_date_time_new(york, 2026, 9, 1, 0, 0, 0);
	g_autoptr(GDateTime) sep4 = g_date_time_new(york, 2026, 9, 4, 0, 0, 0);
	g_autoptr(GDateTime) jan1 = g_date_time_new(york, 2026, 1, 1, 0, 0, 0);
	g_autoptr(GDateTime) sep10 = g_date_time_new(york, 2026, 9, 10, 0, 0, 0);
	g_autoptr(GDateTime) sep17 = g_date_time_new(york, 2026, 9, 17, 0, 0, 0);
	g_autoptr(VentureDateRange) mtd = venture_date_range_new(sep1, sep4);
	g_autoptr(VentureDateRange) ytd = venture_date_range_new(jan1, sep4);
	g_autoptr(VentureDateRange) week = venture_date_range_new(sep10, sep17);

	g_assert_null(venture_date_range_comparison_period(all, NULL));
	/* Closed months, in New York time. */
	assert_comparison(september, "2026-12-01T00:00:00Z", "2026-08-01T00:00:00-04:00", "2026-09-01T00:00:00-04:00");
	assert_comparison(march, "2026-12-01T00:00:00Z", "2026-02-01T00:00:00-05:00", "2026-03-01T00:00:00-05:00");
	assert_comparison(november, "2026-12-15T00:00:00Z", "2026-10-01T00:00:00-04:00", "2026-11-01T00:00:00-04:00");
	assert_comparison(quarter, "2026-12-01T00:00:00Z", "2026-01-01T00:00:00-05:00", "2026-04-01T00:00:00-04:00");
	assert_comparison(year, "2027-02-01T00:00:00Z", "2025-01-01T00:00:00-05:00", "2026-01-01T00:00:00-05:00");
	/* This month on the third at ten: the first two days and ten hours of
	 * each month. */
	assert_comparison(september, "2026-09-03T10:00:00-04:00", "2026-08-01T00:00:00-04:00", "2026-08-03T10:00:00-04:00");
	/* The thirty-first of March against all of February, never March. */
	assert_comparison(march, "2026-03-31T12:00:00-04:00", "2026-02-01T00:00:00-05:00", "2026-03-01T00:00:00-05:00");
	/* This year in September: the previous year to the same day. */
	assert_comparison(year, "2026-09-03T10:00:00-04:00", "2025-01-01T00:00:00-05:00", "2025-09-03T10:00:00-04:00");
	/* Month and year to date end tomorrow; the calendar unit they sit in
	 * decides what they are compared with. */
	assert_comparison(mtd, "2026-09-03T10:00:00-04:00", "2026-08-01T00:00:00-04:00", "2026-08-03T10:00:00-04:00");
	assert_comparison(ytd, "2026-09-03T10:00:00-04:00", "2025-01-01T00:00:00-05:00", "2025-09-03T10:00:00-04:00");
	/* Whole days: the same number of days before. */
	assert_comparison(week, NULL, "2026-09-03T00:00:00-04:00", "2026-09-10T00:00:00-04:00");
}

/* --- 9. Billing retention -------------------------------------------------- */

/*
 * Quick ratio, gross and net revenue retention from subscription events.
 * February opens with Alpha at $100 and Beta at $50. In February Alpha
 * expands to $150 (a second $50 seat), Beta cancels, and Gamma starts at
 * $50. Gained 100 (new 50, expansion 50) over lost 50: quick ratio 2.00.
 * Revenue churn 50 / 150 = 33.33%, so GRR 66.67%; NRR (150 - 50 + 50) /
 * 150 = 100.00%.
 */
static void
test_billing_retention(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) mrr = NULL;
	g_autoptr(VentureReportResult) churn = NULL;
	g_autoptr(VentureEntity) seats = NULL;
	gint64 per_seat;
	gint64 flat;
	gint64 alpha = company(f, "Alpha", NULL);
	gint64 beta = company(f, "Beta", NULL);
	gint64 gamma = company(f, "Gamma", NULL);
	gint64 alpha_sub;
	gint64 beta_sub;

	per_seat = plan_price(f, "seat", "month", "50 USD", TRUE);
	flat = plan_price(f, "flat", "month", "50 USD", FALSE);
	{
		g_autoptr(VentureEntity) request = record_new(f, "billing_request");
		g_object_set(request, "action", "start", "company-id", alpha, "plan-price-id", per_seat, "seats", (gint64)2, NULL);
		field(request, "at", "2026-01-25");
		save(f, request);
		alpha_sub = integer(request, "subscription-id");
	}
	beta_sub = billing(f, "start", 0, beta, flat, "2026-01-26", NULL);
	seats = record_new(f, "billing_request");
	g_object_set(seats, "action", "change-seats", "subscription-id", alpha_sub, "seats", (gint64)3, NULL);
	field(seats, "at", "2026-02-10");
	save(f, seats);
	billing(f, "cancel", beta_sub, 0, 0, "2026-02-12", NULL);
	billing(f, "start", 0, gamma, flat, "2026-02-14", NULL);

	mrr = run_report(f, "mrr", "2026-02", NULL);
	g_assert_cmpint(money_metric(mrr, "mrr"), ==, 20000);
	g_assert_cmpfloat(venture_metric_get_number(metric(mrr, "quick_ratio")), ==, 2.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(mrr, "customers")), ==, 2.0);
	g_assert_cmpint(money_metric(mrr, "arpa"), ==, 10000);
	churn = run_report(f, "churn", "2026-02", NULL);
	g_assert_cmpstr(venture_report_result_get_title(churn), ==, "Subscription churn");
	g_assert_cmpfloat(venture_metric_get_number(metric(churn, "revenue_churn_bps")), ==, 3333.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(churn, "grr_bps")), ==, 6667.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(churn, "nrr_bps")), ==, 10000.0);
	g_assert_cmpint(money_metric(churn, "expansion_mrr"), ==, 5000);
}

/* --- 10. Settings ---------------------------------------------------------- */

static gint64
scalar(VentureDatabase *db, const gchar *sql)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = venture_database_query_raw(db, sql, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	return orm_row_get_integer(orm_result_get_row(result), 0);
}

/* One row per organisation on every save, not just the first: moving a row
 * to an organisation that already has one is the same duplicate. */
static void
test_settings_one_row(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) other = record_new(f, "organization");
	g_autoptr(VentureEntity) mine = record_new(f, "headline_setting");
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(GError) error = NULL;

	g_object_set(other, "name", "Other", "slug", "other", NULL);
	save(f, other);
	save(f, mine);
	theirs = g_object_new(VENTURE_TYPE_HEADLINE_SETTING, NULL);
	venture_entity_set_organization_id(theirs, venture_entity_get_id(other));
	save(f, theirs);
	/* Saving a row again is not a duplicate of itself. */
	g_object_set(mine, "activity-days", (gint64)60, NULL);
	save(f, mine);
	venture_entity_set_organization_id(theirs, f->org);
	g_assert_false(venture_database_save(f->db, theirs, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* A restore skips validators; the database refuses a second living row. */
	g_assert_false(venture_database_execute(f->db,
		"INSERT INTO headline_settings (uuid, organization_id, version) VALUES ('dup', 1, 1)", NULL, &error));
	g_assert_nonnull(error);
}

/*
 * Migration 000350 on an install that already has duplicates keeps the row
 * every reader used (the lowest id) and soft-deletes the rest, then adds the
 * index. With the headline module off the table is created so the index
 * exists before the module is first switched on.
 */
static void
test_settings_migration(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-headline-migration-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/books.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GError) error = NULL;
	guint run;

	/* Off: the table and index come from the migration alone. Customer
	 * health requires headline, so it goes off with it. */
	venture_config_set_module_enabled(config, "headline", FALSE);
	venture_config_set_module_enabled(config, "customer_health", FALSE);
	db = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_cmpint(scalar(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'index' AND name = 'uq_headline_settings_organization'"), ==, 1);

	/* A pre-000350 install with three rows for one organisation. */
	g_assert_true(venture_database_execute(db,
		"DROP INDEX uq_headline_settings_organization;"
		"INSERT INTO headline_settings (uuid, organization_id, version, activity_days) VALUES ('a', 1, 1, 30), ('b', 1, 1, 60), ('c', 1, 1, 90), ('d', 2, 1, 45);"
		"DELETE FROM schema_migrations WHERE version >= 350", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&context);
	g_clear_object(&db);
	venture_config_set_module_enabled(config, "headline", TRUE);

	for (run = 0; run < 2; run++)
	{
		db = venture_database_new(uri, &error);
		g_assert_no_error(error);
		context = venture_context_new(config, db);
		g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		g_assert_cmpint(scalar(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM headline_settings WHERE deleted_at IS NULL AND organization_id = 1"), ==, 1);
		g_assert_cmpint(scalar(db, "SELECT activity_days FROM headline_settings WHERE deleted_at IS NULL AND organization_id = 1"), ==, 30);
		g_assert_cmpint(scalar(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM headline_settings WHERE deleted_at IS NOT NULL"), ==, 2);
		g_assert_cmpint(scalar(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM headline_settings WHERE deleted_at IS NULL AND organization_id = 2"), ==, 1);
		g_assert_false(venture_database_execute(db,
			"INSERT INTO headline_settings (uuid, organization_id, version) VALUES ('e', 1, 1)", NULL, NULL));
		g_clear_object(&context);
		g_clear_object(&db);
	}
	venture_test_remove_tree(directory);
}

/* --- 11. Timezones --------------------------------------------------------- */

/*
 * A bare date is stored as midnight UTC and every period boundary is a
 * midnight UTC too, whatever the configured zone (America/New_York here).
 * Before that, a March expense was 19:00 on 28 February in New York: the
 * P&L for March left it out and February's took it, and an expense dated
 * 1 January landed in the previous year. Every report read it that way.
 */
static void
test_bare_date_timezone(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) february = NULL;
	g_autoptr(VentureReportResult) march = NULL;
	g_autoptr(VentureReportResult) last_year = NULL;
	g_autoptr(VentureReportResult) this_year = NULL;

	expense(f, "First of March", "10 USD", "2026-03-01", FALSE);
	expense(f, "New year", "5 USD", "2026-01-01", FALSE);
	february = run_report(f, "pnl", "2026-02", NULL);
	march = run_report(f, "pnl", "2026-03", NULL);
	last_year = run_report(f, "pnl", "2025", NULL);
	this_year = run_report(f, "pnl", "2026", NULL);
	g_assert_cmpint(money_metric(february, "expenses"), ==, 0);
	g_assert_cmpint(money_metric(march, "expenses"), ==, 1000);
	g_assert_cmpint(money_metric(last_year, "expenses"), ==, 0);
	g_assert_cmpint(money_metric(this_year, "expenses"), ==, 1500);
}

/* --- 12. The cards --------------------------------------------------------- */

static JsonObject *
card_with_key(JsonArray *cards, const gchar *key)
{
	guint i;
	for (i = 0; i < json_array_get_length(cards); i++)
	{
		JsonObject *card = json_array_get_object_element(cards, i);
		if (g_strcmp0(json_object_get_string_member(card, "key"), key) == 0)
			return card;
	}
	return NULL;
}

static const gchar *
line_value(JsonObject *card, const gchar *label)
{
	JsonArray *lines = json_object_get_array_member(card, "lines");
	guint i;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *line = json_array_get_object_element(lines, i);
		if (g_strcmp0(json_object_get_string_member(line, "label"), label) == 0)
			return json_object_get_string_member(line, "value");
	}
	return NULL;
}

static void
test_home_cards(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) cards = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *array;
	JsonObject *card;
	static const gchar *const keys[] = { "pnl", "cac", "churn", "ltv_cac", "support" };
	guint i;
	gint64 p = company(f, "Pine", NULL);

	paid_invoice(f, p, "P-1", "100 USD", "2026-06-10");
	paid_invoice(f, p, "P-2", "300 USD", "2026-07-10");
	/* A statement balance is derived by import, so the cash beside the
	 * books comes from a real import rather than a typed-in field. */
	account = record_new(f, "bank_account");
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		g_autoptr(VentureEntity) cash = NULL;
		venture_query_set_organization(query, f->org);
		g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", &error));
		cash = venture_database_find_one(f->db, query, &error);
		g_assert_no_error(error);
		g_assert_nonnull(cash);
		g_object_set(account, "account-id", venture_entity_get_id(cash), NULL);
	}
	g_object_set(account, "name", "Main", "currency", "USD", "date-column", "Date",
		"amount-column", "Amount", "description-column", "Memo", "reference-column", "Ref",
		"external-id-column", "ID", "date-format", "%Y-%m-%d", "sign-convention", "normal", NULL);
	save(f, account);
	{
		g_autoptr(JsonObject) args = json_object_new();
		g_autoptr(VentureEntity) statement = NULL;
		json_object_set_string_member(args, "period_start", "2026-07-01");
		json_object_set_string_member(args, "period_end", "2026-07-31");
		json_object_set_string_member(args, "opening_balance", "1224 USD");
		json_object_set_string_member(args, "closing_balance", "1234 USD");
		json_object_set_string_member(args, "format", "csv");
		json_object_set_string_member(args, "data", "Date,Amount,Memo,Ref,ID\n2026-07-10,10,Interest,July,int-1\n");
		statement = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db), "import",
			venture_entity_get_id(account), args, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(statement);
	}
	ticket = record_new(f, "ticket");
	g_object_set(ticket, "title", "Open one", "status", VENTURE_TICKET_STATUS_TODO, NULL);
	save(f, ticket);

	period = venture_context_parse_period(f->context, "2026-07", &error);
	g_assert_no_error(error);
	g_assert_true(venture_headline_home_enabled(f->db, f->org));
	cards = venture_headline_home_cards(f->context, f->org, period, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cards);
	array = json_node_get_array(cards);
	/* Billing is on but unused: no recurring revenue card. */
	g_assert_cmpuint(json_array_get_length(array), ==, 5);
	for (i = 0; i < 5; i++)
	{
		g_autofree gchar *link = NULL;
		card = json_array_get_object_element(array, i);
		g_assert_cmpstr(json_object_get_string_member(card, "key"), ==, keys[i]);
		g_assert_true(json_object_has_member(card, "value"));
		g_assert_true(json_object_has_member(card, "trend"));
		g_assert_cmpstr(json_object_get_string_member(card, "state"), ==, "ok");
		g_assert_true(strlen(json_object_get_string_member(card, "definition")) > 40);
		g_assert_true(g_str_has_prefix(json_object_get_string_member(card, "link"), "/reports/"));
		/* The report link asks the question the card answered. */
		link = g_strdup_printf("?period=2026-07-01..2026-07-31&organization_id=%" G_GINT64_FORMAT, f->org);
		g_assert_nonnull(strstr(json_object_get_string_member(card, "link"), link));
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(card, "lines")), >=, 2);
	}
	/* July's profit is above June's: the arrow points up. */
	card = json_array_get_object_element(array, 0);
	g_assert_cmpint(json_object_get_int_member(card, "trend"), ==, 1);
	g_assert_cmpstr(line_value(card, "Bank cash"), ==, "$1,234.00");
	/* One open ticket on the support card. */
	card = card_with_key(array, "support");
	g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, "1");

	/* The tickets module off: nobody counted, so n/a -- not zero. */
	venture_config_set_module_enabled(f->config, "tickets", FALSE);
	{
		g_autoptr(JsonNode) off = venture_headline_home_cards(f->context, f->org, period, &error);
		g_assert_no_error(error);
		card = card_with_key(json_node_get_array(off), "support");
		g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, "n/a");
		g_assert_cmpstr(line_value(card, "Raised in period"), ==, "n/a");
	}
	venture_config_set_module_enabled(f->config, "tickets", TRUE);

	/* One card that cannot be answered is an error card; the rest render. */
	{
		g_autoptr(JsonNode) broken = NULL;
		VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
		venture_report_registry_set_enabled(registry, "support", FALSE);
		g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "Headline card support could not be computed*");
		broken = venture_headline_home_cards(f->context, f->org, period, &error);
		g_test_assert_expected_messages();
		venture_report_registry_set_enabled(registry, "support", TRUE);
		g_assert_no_error(error);
		card = card_with_key(json_node_get_array(broken), "support");
		g_assert_cmpstr(json_object_get_string_member(card, "state"), ==, "error");
		g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, "n/a");
		card = card_with_key(json_node_get_array(broken), "pnl");
		g_assert_cmpstr(json_object_get_string_member(card, "state"), ==, "ok");
	}

	/* Restricted: every card, nothing computed. */
	{
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(JsonNode) restricted = NULL;
		g_autofree gchar *html = NULL;
		g_autofree gchar *csv = NULL;
		json_object_set_int_member(options, "organization_id", f->org);
		g_assert_true(venture_headline_home_render(f->context, options, period, "2026-07", NULL, FALSE,
			&restricted, &html, &csv, &error));
		g_assert_no_error(error);
		for (i = 0; i < json_array_get_length(json_node_get_array(restricted)); i++)
		{
			card = json_array_get_object_element(json_node_get_array(restricted), i);
			g_assert_cmpstr(json_object_get_string_member(card, "state"), ==, "restricted");
			g_assert_cmpuint(json_array_get_length(json_object_get_array_member(card, "lines")), ==, 0);
		}
		g_assert_nonnull(strstr(html, "data-state=\"restricted\""));
		g_assert_null(strstr(html, "$"));
		g_assert_true(g_str_has_prefix(csv, "card,metric,value,trend,state\n"));
		g_assert_nonnull(strstr(csv, "cac,Customer acquisition cost,restricted,0,restricted"));
	}
}

/* With billing in use the cards gain recurring revenue, and churn is read
 * from subscriptions. */
static void
test_home_cards_billing(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) cards = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	gint64 price = plan_price(f, "starter", "month", "30 USD", FALSE);
	gint64 buyer = company(f, "Subscriber", NULL);
	JsonObject *card;

	billing(f, "start", 0, buyer, price, "2026-01-05", NULL);
	period = venture_context_parse_period(f->context, "2026-02", &error);
	g_assert_no_error(error);
	cards = venture_headline_home_cards(f->context, f->org, period, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(cards)), ==, 6);
	card = card_with_key(json_node_get_array(cards), "mrr");
	g_assert_nonnull(card);
	g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, "$30.00");
	g_assert_cmpstr(line_value(card, "ARR"), ==, "$360.00");
	card = card_with_key(json_node_get_array(cards), "churn");
	g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, "0.0%");
	g_assert_nonnull(strstr(json_object_get_string_member(card, "link"), "/reports/churn?"));
}

/* --- Over HTTP: / is the cards until the organisation opts out ------------ */

typedef struct
{
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *cookie;
	guint16 port;
	gint64 org;
} ServerFixture;

typedef struct { gboolean done; GBytes *body; GError *error; } RequestResult;

static void
request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request(ServerFixture *f, const gchar *method, const gchar *path, const gchar *form, gchar **out_body)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", f->port, path);
	RequestResult outcome = { FALSE, NULL, NULL };
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (f->cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", f->cookie);
	if (form != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	if (outcome.error != NULL)
		g_error("%s %s: %s", method, path, outcome.error->message);
	if (out_body != NULL)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL), g_bytes_get_size(outcome.body));
	g_clear_pointer(&outcome.body, g_bytes_unref);
	return soup_message_get_status(message);
}

/* Signs @username in and returns the session cookie. */
static gchar *
login(ServerFixture *f, const gchar *username, const gchar *password)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/login", f->port);
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	g_autoptr(GBytes) bytes = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };
	gchar *cookie;
	gchar *semicolon;
	message = soup_message_new("POST", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	bytes = g_bytes_new(form, strlen(form));
	soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);
	cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	g_assert_nonnull(cookie);
	semicolon = strchr(cookie, ';');
	if (semicolon != NULL)
		*semicolon = '\0';
	return cookie;
}

static void
server_setup(ServerFixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_setenv("VENTURE_TEST_SESSION_SECRET", "headline-test-secret", TRUE);
	f->state_dir = g_dir_make_tmp("venture-headline-XXXXXX", NULL);
	f->port = (guint16)(20000 + ((getpid() + 14071) % 20000));
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)f->port, "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
		"security-password-iterations", (gint64)100000, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, NULL));
	f->cookie = login(f, "owner", "owner-password-1");
}

static void
server_teardown(ServerFixture *f, gconstpointer unused)
{
	if (f->server != NULL)
		venture_web_server_stop(f->server);
	g_clear_pointer(&f->cookie, g_free);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	if (f->state_dir != NULL)
	{
		venture_test_remove_tree(f->state_dir);
		g_clear_pointer(&f->state_dir, g_free);
	}
	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
}

static void
test_home_page(ServerFixture *f, gconstpointer unused)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *api = NULL;
	g_autofree gchar *classic = NULL;
	g_autofree gchar *link = NULL;
	g_autoptr(VentureEntity) setting = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	guint status;

	status = server_request(f, "GET", "/", NULL, &body);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(body, "headline-cards"));
	g_assert_nonnull(strstr(body, "data-card=\"pnl\""));
	g_assert_nonnull(strstr(body, "data-card=\"support\""));
	link = g_strdup_printf("href=\"/reports/cac?period=this_month&amp;organization_id=%" G_GINT64_FORMAT "\"", f->org);
	g_assert_nonnull(strstr(body, link));
	/* The period picker steps through the report page's periods. */
	g_assert_nonnull(strstr(body, "href=\"/?period=last_month\""));
	g_assert_nonnull(strstr(body, "/api/v1/headline?format=csv&amp;period=this_month"));
	g_assert_nonnull(strstr(body, "headline-definition"));

	/* A mistyped period is a notice, not the old dashboard. */
	{
		g_autofree gchar *bad = NULL;
		g_autofree gchar *picked = NULL;

		status = server_request(f, "GET", "/?period=fortnight", NULL, &bad);
		g_assert_cmpuint(status, ==, 200);
		g_assert_nonnull(strstr(bad, "headline-cards"));
		g_assert_nonnull(strstr(bad, "<div class=\"notice negative\">"));
		g_assert_nonnull(strstr(bad, "fortnight"));
		status = server_request(f, "GET", "/?period=last_month", NULL, &picked);
		g_assert_cmpuint(status, ==, 200);
		g_assert_nonnull(strstr(picked, "class=\"btn btn-sm active\" href=\"/?period=last_month\""));
		g_assert_nonnull(strstr(picked, "period=last_month&amp;organization_id="));
		status = server_request(f, "GET", "/api/v1/headline?period=fortnight", NULL, NULL);
		g_assert_cmpuint(status, >=, 400);
	}

	/* /overview is the built-in page even while / is the cards. */
	{
		g_autofree gchar *overview = NULL;

		status = server_request(f, "GET", "/overview", NULL, &overview);
		g_assert_cmpuint(status, ==, 200);
		g_assert_nonnull(strstr(overview, "<h1>Dashboard</h1>"));
		g_assert_null(strstr(overview, "headline-cards"));
	}

	status = server_request(f, "GET", "/api/v1/headline?period=this_month", NULL, &api);
	g_assert_cmpuint(status, ==, 200);
	node = venture_json_parse(api, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), ==, 5);

	/* The same cards as CSV. */
	{
		g_autofree gchar *csv = NULL;

		status = server_request(f, "GET", "/api/v1/headline?period=this_month&format=csv", NULL, &csv);
		g_assert_cmpuint(status, ==, 200);
		g_assert_true(g_str_has_prefix(csv, "card,metric,value,trend,state\n"));
		g_assert_nonnull(strstr(csv, "\npnl,Profit and loss,"));
		g_assert_nonnull(strstr(csv, "\nsupport,Service levels missed,0,,ok\n"));
	}

	/* The module off hides the cards and the API; reports stay registered
	 * until the mask is applied, and / falls through to the built-in. */
	venture_config_set_module_enabled(f->config, "customer_health", FALSE);
	venture_config_set_module_enabled(f->config, "headline", FALSE);
	{
		g_autofree gchar *off_home = NULL;
		g_autofree gchar *off_api = NULL;

		status = server_request(f, "GET", "/", NULL, &off_home);
		g_assert_cmpuint(status, ==, 200);
		g_assert_null(strstr(off_home, "headline-cards"));
		g_assert_nonnull(strstr(off_home, "<h1>Dashboard</h1>"));
		status = server_request(f, "GET", "/api/v1/headline?period=this_month",
		                        NULL, &off_api);
		g_assert_cmpuint(status, ==, SOUP_STATUS_NOT_FOUND);
	}
	venture_config_set_module_enabled(f->config, "headline", TRUE);
	venture_config_set_module_enabled(f->config, "customer_health", TRUE);

	g_clear_pointer(&body, g_free);
	status = server_request(f, "GET", "/", NULL, &body);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(body, "headline-cards"));

	/* Opting out sends / back to the old dashboard. The API stays. */
	setting = g_object_new(VENTURE_TYPE_HEADLINE_SETTING, NULL);
	venture_entity_set_organization_id(setting, f->org);
	g_object_set(setting, "classic-home", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, setting, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_headline_home_enabled(f->db, f->org));
	status = server_request(f, "GET", "/", NULL, &classic);
	g_assert_cmpuint(status, ==, 200);
	g_assert_null(strstr(classic, "headline-cards"));
	g_assert_nonnull(strstr(classic, "<h1>Dashboard</h1>"));
	{
		g_autofree gchar *overview = NULL;
		g_autofree gchar *still = NULL;

		status = server_request(f, "GET", "/overview", NULL, &overview);
		g_assert_cmpuint(status, ==, 200);
		g_assert_nonnull(strstr(overview, "<h1>Dashboard</h1>"));
		status = server_request(f, "GET", "/api/v1/headline?period=this_month",
		                        NULL, &still);
		g_assert_cmpuint(status, ==, 200);
	}
}

/* Adds a user holding @role in the default organisation. */
static void
member(ServerFixture *f, const gchar *username, VentureUserRole user_role, gint role)
{
	g_autoptr(VentureUser) user = venture_user_new();
	g_autoptr(VentureEntity) membership = NULL;
	g_object_set(user, "username", username, "role", user_role, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "member-password-1", 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(user), NULL, NULL));
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", venture_entity_get_id(VENTURE_ENTITY(user)), "active", TRUE, "role", role, NULL);
	g_assert_true(venture_database_save(f->db, membership, NULL, NULL));
}

/*
 * Organisation totals are for the roles that may read every row behind
 * them. A viewer's record policy filters rows, and a total of the rows a
 * viewer may read, presented as the organisation's, is a smaller and wrong
 * number that looks exactly like the right one; the first version showed
 * it. A viewer sees the cards restricted; finance sees the totals.
 */
static void
test_home_page_roles(ServerFixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *api = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *owner = g_steal_pointer(&f->cookie);
	guint status;
	static const gint totals[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_FINANCE };

	member(f, "viewer", VENTURE_USER_ROLE_VIEWER, VENTURE_ORGANIZATION_ROLE_VIEWER);
	member(f, "finance", VENTURE_USER_ROLE_EDITOR, VENTURE_ORGANIZATION_ROLE_FINANCE);

	f->cookie = login(f, "viewer", "member-password-1");
	status = server_request(f, "GET", "/api/v1/headline?period=this_month", NULL, &api);
	g_assert_cmpuint(status, ==, 200);
	node = venture_json_parse(api, NULL);
	g_assert_nonnull(node);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(json_node_get_array(node), 0), "state"),
		==, "restricted");
	status = server_request(f, "GET", "/", NULL, &page);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(page, "data-state=\"restricted\""));
	g_clear_pointer(&f->cookie, g_free);
	g_clear_pointer(&api, g_free);
	g_clear_pointer(&node, json_node_unref);

	f->cookie = login(f, "finance", "member-password-1");
	status = server_request(f, "GET", "/api/v1/headline?period=this_month", NULL, &api);
	g_assert_cmpuint(status, ==, 200);
	node = venture_json_parse(api, NULL);
	g_assert_nonnull(node);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(json_node_get_array(node), 0), "state"),
		==, "ok");

	/* The same decision, asked of the policy directly. */
	{
		VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
		VentureAuthPrincipal actor;
		gchar name[] = "viewer";
		g_autoptr(VentureEntity) viewer = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_USER);

		venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, "viewer", NULL);
		viewer = venture_database_find_one(f->db, query, NULL);
		actor.user_id = venture_entity_get_id(viewer);
		actor.token_id = 0;
		actor.role = VENTURE_USER_ROLE_VIEWER;
		actor.name = name;
		actor.authenticated = TRUE;
		g_assert_false(venture_access_policy_has_organization_role(policy, &actor, f->org, totals, G_N_ELEMENTS(totals)));
		actor.role = VENTURE_USER_ROLE_ADMIN;
		g_assert_true(venture_access_policy_has_organization_role(policy, &actor, f->org, totals, G_N_ELEMENTS(totals)));
		actor.authenticated = FALSE;
		g_assert_false(venture_access_policy_has_organization_role(policy, &actor, f->org, totals, G_N_ELEMENTS(totals)));
	}
	g_clear_pointer(&f->cookie, g_free);
	f->cookie = g_steal_pointer(&owner);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/headline/acquisition", Fixture, NULL, setup, test_acquisition, teardown);
	g_test_add("/headline/cac", Fixture, NULL, setup, test_cac, teardown);
	g_test_add("/headline/cac/first-cash", Fixture, NULL, setup, test_cac_first_cash, teardown);
	g_test_add("/headline/cac/double-count", Fixture, NULL, setup, test_cac_double_count, teardown);
	g_test_add("/headline/cac/breakdown", Fixture, NULL, setup, test_cac_breakdown, teardown);
	g_test_add("/headline/cac/recoverable-tax", Fixture, NULL, setup, test_cac_recoverable_tax, teardown);
	g_test_add("/headline/cash/overpayment", Fixture, NULL, setup, test_cash_overpayment, teardown);
	g_test_add("/headline/cash/retainer", Fixture, NULL, setup, test_cash_retainer, teardown);
	g_test_add("/headline/cash/foreign", Fixture, NULL, setup, test_cash_foreign, teardown);
	g_test_add("/headline/churn", Fixture, NULL, setup, test_churn, teardown);
	g_test_add("/headline/churn/recurring-customers", Fixture, NULL, setup, test_churn_recurring_customers, teardown);
	g_test_add("/headline/churn/monthly-worked-example", Fixture, NULL, setup, test_churn_monthly_worked_example, teardown);
	g_test_add("/headline/ltv", Fixture, NULL, setup, test_ltv, teardown);
	g_test_add("/headline/ltv/margin", Fixture, NULL, setup, test_ltv_margin, teardown);
	g_test_add("/headline/ltv/annual-worked-example", Fixture, NULL, setup, test_ltv_annual_worked_example, teardown);
	g_test_add("/headline/ltv_cac", Fixture, NULL, setup, test_ltv_cac, teardown);
	g_test_add("/headline/cohorts", Fixture, NULL, setup, test_cohorts, teardown);
	g_test_add("/headline/scope", Fixture, NULL, setup, test_scope, teardown);
	g_test_add_func("/headline/comparison-period", test_comparison_period);
	g_test_add("/headline/billing/retention", Fixture, NULL, setup, test_billing_retention, teardown);
	g_test_add("/headline/settings/one-row", Fixture, NULL, setup, test_settings_one_row, teardown);
	g_test_add_func("/headline/settings/migration", test_settings_migration);
	g_test_add("/headline/timezone/bare-date", Fixture, NULL, setup, test_bare_date_timezone, teardown);
	g_test_add("/headline/home/cards", Fixture, NULL, setup, test_home_cards, teardown);
	g_test_add("/headline/home/cards-billing", Fixture, NULL, setup, test_home_cards_billing, teardown);
	g_test_add("/headline/home/page", ServerFixture, NULL, server_setup, test_home_page, server_teardown);
	g_test_add("/headline/home/roles", ServerFixture, NULL, server_setup, test_home_page_roles, server_teardown);
	return g_test_run();
}
