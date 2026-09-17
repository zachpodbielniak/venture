/*
 * test-pnl-cuts.c - Revenue by customer, spend by vendor, recurring costs,
 * the weekly cash outlook and the P&L card's links
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each cut of the P&L gets a fixture small enough to check by hand and a
 * negative case for what it must leave out: drafts, voids, cancelled
 * schedules, undated documents, other currencies.
 */

#include <venture.h>

#include <string.h>

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
	(void)unused;
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
	(void)unused;
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
company(Fixture *f, const gchar *name, const gchar *source)
{
	g_autoptr(VentureEntity) record = record_new(f, "company");
	g_object_set(record, "name", name, "source", source, NULL);
	save(f, record);
	return venture_entity_get_id(record);
}

static gint64
vendor(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) record = record_new(f, "company");
	g_object_set(record, "name", name, "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, record);
	return venture_entity_get_id(record);
}

/* An invoice issued on @issued for @amount in @currency, due on @due (or
 * undated when NULL). Issuing goes through the settlement service via the
 * status save, which freezes the issued amount on the event. */
static gint64
issued_invoice(Fixture *f, gint64 company_id, const gchar *number, const gchar *amount,
	const gchar *issued, const gchar *due)
{
	g_autoptr(VentureEntity) invoice = record_new(f, "invoice");
	g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
	g_object_set(invoice, "number", number, "company-id", company_id, NULL);
	field(invoice, "issued-at", issued);
	if (due != NULL)
		field(invoice, "due-at", due);
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", amount);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	return venture_entity_get_id(invoice);
}

/* An issued invoice settled in full by a receipt on @paid. */
static gint64
paid_invoice(Fixture *f, gint64 company_id, const gchar *number, const gchar *amount,
	const gchar *issued, const gchar *paid)
{
	g_autoptr(VentureEntity) payment = record_new(f, "payment");
	g_autoptr(VentureEntity) stored = NULL;
	gint64 invoice_id = issued_invoice(f, company_id, number, amount, issued, issued);
	gint status = 0;
	g_object_set(payment, "customer-id", company_id, "invoice-id", invoice_id, "method", "manual", NULL);
	field(payment, "amount", amount);
	field(payment, "date", paid);
	save(f, payment);
	stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE, invoice_id, NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_PAID);
	return invoice_id;
}

static void
refund(Fixture *f, gint64 company_id, gint64 invoice_id, const gchar *amount, const gchar *when)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureEntity) record = record_new(f, "refund");
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, &error));
	allocation = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(allocation);
	g_object_set(record, "customer-id", company_id, "allocation-id", venture_entity_get_id(allocation), NULL);
	field(record, "amount", amount);
	field(record, "date", when);
	save(f, record);
}

static void
credit_note(Fixture *f, gint64 company_id, const gchar *amount, const gchar *when)
{
	g_autoptr(VentureEntity) credit = record_new(f, "customer_credit");
	g_object_set(credit, "customer-id", company_id, "kind", "credit_note", NULL);
	field(credit, "amount", amount);
	field(credit, "date", when);
	save(f, credit);
}

static void
expense(Fixture *f, const gchar *what, const gchar *who, const gchar *category,
	const gchar *amount, const gchar *when, gboolean acquisition)
{
	g_autoptr(VentureEntity) record = record_new(f, "expense");
	g_object_set(record, "description", what, "vendor", who, "category", category,
		"acquisition", acquisition, NULL);
	field(record, "amount", amount);
	field(record, "occurred-at", when);
	save(f, record);
}

/* A bill with one line, approved unless @approve is FALSE; the due date
 * is frozen at approval so it is set first. */
static gint64
bill(Fixture *f, gint64 vendor_id, const gchar *number, const gchar *unit_price,
	const gchar *category, gboolean acquisition, const gchar *dated, const gchar *due,
	gboolean approve)
{
	g_autoptr(VentureEntity) record = record_new(f, "vendor_bill");
	g_autoptr(VentureEntity) line = record_new(f, "vendor_bill_line");
	g_autofree gchar *currency = g_strdup(strrchr(unit_price, ' ') + 1);
	g_object_set(record, "company-id", vendor_id, "number", number, "currency", currency,
		"status", "draft", NULL);
	field(record, "bill-date", dated);
	if (due != NULL)
		field(record, "due-date", due);
	save(f, record);
	g_object_set(line, "bill-id", venture_entity_get_id(record), "description", number,
		"quantity", "1", "category", category, "acquisition", acquisition, NULL);
	field(line, "unit-price", unit_price);
	save(f, line);
	if (approve)
	{
		g_autoptr(VentureEntity) event = record_new(f, "vendor_bill_event");
		g_object_set(event, "bill-id", venture_entity_get_id(record), "vendor-id", vendor_id,
			"kind", "approve", "state", "approved", NULL);
		field(event, "date", dated);
		save(f, event);
	}
	return venture_entity_get_id(record);
}

static void
pay_bill(Fixture *f, gint64 vendor_id, gint64 bill_id, const gchar *amount, const gchar *when)
{
	g_autoptr(VentureEntity) payment = record_new(f, "bill_payment");
	g_object_set(payment, "vendor-id", vendor_id, "bill-id", bill_id, "method", "transfer", NULL);
	field(payment, "amount", amount);
	field(payment, "date", when);
	save(f, payment);
}

static VentureEntity *
schedule(Fixture *f, const gchar *name, const gchar *kind, const gchar *frequency,
	const gchar *start, const gchar *template)
{
	VentureEntity *record = record_new(f, "recurring_schedule");
	g_object_set(record, "name", name, "timezone", "UTC", "template", template, NULL);
	field(record, "kind", kind);
	field(record, "frequency", frequency);
	field(record, "start-at", start);
	save(f, record);
	return record;
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

static gint64
money_metric(VentureReportResult *result, const gchar *key)
{
	const VentureMoney *amount = venture_metric_get_money(metric(result, key));
	g_assert_nonnull(amount);
	return venture_money_get_amount(amount);
}

static gchar *
cell(VentureReportResult *result, guint row, const gchar *key)
{
	return venture_report_result_format_cell(result, row, key);
}

static gint64
money_cell(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);
	const VentureMoney *amount;
	g_assert_nonnull(value);
	amount = g_value_get_boxed(value);
	g_assert_nonnull(amount);
	return venture_money_get_amount(amount);
}

/* The row whose @key column reads @text, or fails. */
static guint
row_named(VentureReportResult *result, const gchar *key, const gchar *text)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		g_autofree gchar *value = cell(result, i, key);
		if (g_strcmp0(value, text) == 0)
			return i;
	}
	g_error("no row with %s = %s", key, text);
	return 0;
}

static gboolean
has_row(VentureReportResult *result, const gchar *key, const gchar *text)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		g_autofree gchar *value = cell(result, i, key);
		if (g_strcmp0(value, text) == 0)
			return TRUE;
	}
	return FALSE;
}

static const gchar *
note(VentureReportResult *result)
{
	g_autoptr(JsonNode) json = venture_report_result_to_json(result);
	JsonObject *object = json_node_get_object(json);
	static gchar buffer[2048];
	const gchar *text = json_object_has_member(object, "note")
		? json_object_get_string_member(object, "note") : "";
	g_strlcpy(buffer, text != NULL ? text : "", sizeof buffer);
	return buffer;
}

/* --- 1. Revenue by customer, and by source --------------------------------- */

static void
test_revenue_by_customer(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) by_source = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(JsonObject) convert = json_object_new();
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *source = NULL;
	gint64 a = company(f, "Acme", NULL);
	gint64 b = company(f, "Bolt", "referral");
	gint64 c = company(f, "Cog", NULL);
	gint64 a_paid;
	guint row;
	(void)unused;

	/* Acme: 300 paid in February, 50 refunded in February, plus 120 issued
	 * and unpaid; a web lead converted into it. */
	a_paid = paid_invoice(f, a, "A-1", "300 USD", "2026-02-03", "2026-02-05");
	refund(f, a, a_paid, "50 USD", "2026-02-20");
	issued_invoice(f, a, "A-2", "120 USD", "2026-02-25", "2026-03-25");
	lead = record_new(f, "lead");
	g_object_set(lead, "name", "Acme inquiry", "source", "web", "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, lead);
	json_object_set_int_member(convert, "company_id", a);
	json_object_set_boolean_member(convert, "deal", FALSE);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, convert, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(converted);

	/* Bolt: 200 issued in February with a 40 credit note; paid in March,
	 * so February's paid figure is zero and March's is 200. */
	issued_invoice(f, b, "B-1", "200 USD", "2026-02-10", "2026-03-10");
	credit_note(f, b, "40 USD", "2026-02-11");
	paid_invoice(f, b, "B-2", "200 USD", "2026-03-05", "2026-03-15");

	/* Cog: January only; a draft in February is not revenue. */
	paid_invoice(f, c, "C-1", "500 USD", "2026-01-10", "2026-01-12");
	{
		g_autoptr(VentureEntity) draft = record_new(f, "invoice");
		g_object_set(draft, "number", "C-DRAFT", "company-id", c, NULL);
		field(draft, "issued-at", "2026-02-14");
		save(f, draft);
	}

	result = run_report(f, "revenue_by_customer", "2026-02", NULL);
	g_assert_cmpint(money_metric(result, "paid"), ==, 25000);
	g_assert_cmpint(money_metric(result, "issued"), ==, 58000);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "customers")), ==, 2.0);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	/* Sorted by paid revenue, descending: Acme first. */
	first = cell(result, 0, "customer");
	g_assert_cmpstr(first, ==, "Acme");
	g_assert_cmpint(money_cell(result, 0, "paid"), ==, 25000);
	g_assert_cmpint(money_cell(result, 0, "issued"), ==, 42000);
	source = cell(result, 0, "source");
	g_assert_cmpstr(source, ==, "web");
	row = row_named(result, "customer", "Bolt");
	g_assert_cmpint(money_cell(result, row, "paid"), ==, 0);
	g_assert_cmpint(money_cell(result, row, "issued"), ==, 16000);
	g_free(source);
	source = cell(result, row, "source");
	g_assert_cmpstr(source, ==, "referral");
	g_assert_false(has_row(result, "customer", "Cog"));

	/* by=source folds the same figures by where the customer came from. */
	json_object_set_string_member(options, "by", "source");
	by_source = run_report(f, "revenue_by_customer", "2026-02", options);
	g_assert_cmpuint(venture_report_result_get_row_count(by_source), ==, 2);
	row = row_named(by_source, "source", "web");
	g_assert_cmpint(money_cell(by_source, row, "paid"), ==, 25000);
	g_assert_cmpint(money_cell(by_source, row, "issued"), ==, 42000);
	row = row_named(by_source, "source", "referral");
	g_assert_cmpint(money_cell(by_source, row, "issued"), ==, 16000);
	g_assert_cmpint(money_metric(by_source, "paid"), ==, 25000);

	/* An unknown grouping is refused, not silently treated as customer. */
	{
		g_autoptr(JsonObject) bad = json_object_new();
		g_autoptr(VentureDateRange) period = NULL;
		VentureReport *report = venture_report_registry_lookup(
			venture_context_get_report_registry(f->context), "revenue_by_customer");
		json_object_set_string_member(bad, "by", "planet");
		period = venture_context_parse_period(f->context, "2026-02", &error);
		g_assert_no_error(error);
		g_assert_null(venture_report_generate(report, f->context, period, bad, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
	}
}

/* A receipt in another currency is counted and noted, never added. */
static void
test_revenue_currencies(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	gint64 a = company(f, "Acme", NULL);
	gint64 e = company(f, "Euro", NULL);
	guint row;
	(void)unused;
	paid_invoice(f, a, "A-1", "100 USD", "2026-02-03", "2026-02-05");
	paid_invoice(f, e, "E-1", "80 EUR", "2026-02-04", "2026-02-06");
	result = run_report(f, "revenue_by_customer", "2026-02", NULL);
	g_assert_cmpint(money_metric(result, "paid"), ==, 10000);
	g_assert_cmpstr(venture_money_get_currency(venture_metric_get_money(metric(result, "paid"))), ==, "USD");
	/* The euro customer keeps its own row in its own currency. */
	row = row_named(result, "customer", "Euro");
	g_assert_cmpint(money_cell(result, row, "paid"), ==, 8000);
	{
		g_autofree gchar *currency = cell(result, row, "currency");
		g_assert_cmpstr(currency, ==, "EUR");
	}
	g_assert_nonnull(strstr(note(result), "could not be included"));
}

/* --- 2. Spend by vendor, and by category ----------------------------------- */

static void
test_spend_by_vendor(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) by_category = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *top = NULL;
	gint64 printer = vendor(f, "Printer");
	gint64 host = vendor(f, "Host");
	guint row;
	(void)unused;

	/* Printer: an approved 50 flyer bill (acquisition) and a 25 paper bill;
	 * a 999 draft and a March bill do not count. */
	bill(f, printer, "P-1", "50 USD", "marketing", TRUE, "2026-02-12", "2026-03-12", TRUE);
	bill(f, printer, "P-2", "25 USD", "supplies", FALSE, "2026-02-13", "2026-03-13", TRUE);
	bill(f, printer, "P-DRAFT", "999 USD", "marketing", TRUE, "2026-02-14", NULL, FALSE);
	bill(f, printer, "P-MARCH", "999 USD", "marketing", TRUE, "2026-03-02", NULL, TRUE);
	/* Host: an approved 100 hosting bill, plus a 30 expense typed against
	 * the same name. */
	bill(f, host, "H-1", "100 USD", "hosting", FALSE, "2026-02-02", "2026-02-28", TRUE);
	expense(f, "Backups", "Host", "hosting", "30 USD", "2026-02-20", FALSE);
	/* Ads: an acquisition expense with no vendor named. */
	expense(f, "Ads", NULL, "marketing", "200 USD", "2026-02-10", TRUE);

	result = run_report(f, "spend_by_vendor", "2026-02", NULL);
	g_assert_cmpint(money_metric(result, "spend"), ==, 40500);
	g_assert_cmpint(money_metric(result, "acquisition"), ==, 25000);
	g_assert_cmpint(money_metric(result, "other"), ==, 15500);
	g_assert_cmpint(money_metric(result, "bills"), ==, 17500);
	g_assert_cmpint(money_metric(result, "expenses"), ==, 23000);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	/* Descending by total: the unnamed ads vendor first. */
	top = cell(result, 0, "vendor");
	g_assert_cmpstr(top, ==, "(no vendor)");
	g_assert_cmpint(money_cell(result, 0, "acquisition"), ==, 20000);
	row = row_named(result, "vendor", "Host");
	g_assert_cmpint(money_cell(result, row, "bills"), ==, 10000);
	g_assert_cmpint(money_cell(result, row, "expenses"), ==, 3000);
	g_assert_cmpint(money_cell(result, row, "total"), ==, 13000);
	g_assert_cmpint(money_cell(result, row, "acquisition"), ==, 0);
	row = row_named(result, "vendor", "Printer");
	g_assert_cmpint(money_cell(result, row, "total"), ==, 7500);
	g_assert_cmpint(money_cell(result, row, "acquisition"), ==, 5000);
	g_assert_cmpint(money_cell(result, row, "other"), ==, 2500);

	json_object_set_string_member(options, "by", "category");
	by_category = run_report(f, "spend_by_vendor", "2026-02", options);
	g_assert_cmpuint(venture_report_result_get_row_count(by_category), ==, 3);
	row = row_named(by_category, "category", "marketing");
	g_assert_cmpint(money_cell(by_category, row, "total"), ==, 25000);
	g_assert_cmpint(money_cell(by_category, row, "acquisition"), ==, 25000);
	row = row_named(by_category, "category", "hosting");
	g_assert_cmpint(money_cell(by_category, row, "total"), ==, 13000);
	row = row_named(by_category, "category", "supplies");
	g_assert_cmpint(money_cell(by_category, row, "total"), ==, 2500);
	g_assert_cmpint(money_metric(by_category, "spend"), ==, 40500);
}

/* An uncategorised expense lands in its own group rather than vanishing,
 * and a bill in another currency is noted rather than added. */
static void
test_spend_edges(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) by_category = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	gint64 euro = vendor(f, "Euro Host");
	guint row;
	(void)unused;
	expense(f, "Misc", "Corner shop", NULL, "10 USD", "2026-02-02", FALSE);
	bill(f, euro, "E-1", "80 EUR", "hosting", FALSE, "2026-02-03", NULL, TRUE);
	result = run_report(f, "spend_by_vendor", "2026-02", NULL);
	g_assert_cmpint(money_metric(result, "spend"), ==, 1000);
	row = row_named(result, "vendor", "Euro Host");
	g_assert_cmpint(money_cell(result, row, "total"), ==, 8000);
	g_assert_nonnull(strstr(note(result), "could not be included"));
	json_object_set_string_member(options, "by", "category");
	by_category = run_report(f, "spend_by_vendor", "2026-02", options);
	row = row_named(by_category, "category", "(uncategorised)");
	g_assert_cmpint(money_cell(by_category, row, "total"), ==, 1000);
}

/* --- 3. Recurring costs ---------------------------------------------------- */

static void
test_recurring_costs(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) rent = NULL;
	g_autoptr(VentureEntity) coffee = NULL;
	g_autoptr(VentureEntity) domain = NULL;
	g_autoptr(VentureEntity) cleaner = NULL;
	g_autoptr(VentureEntity) cancelled = NULL;
	g_autoptr(VentureEntity) paused = NULL;
	g_autoptr(VentureEntity) ended = NULL;
	g_autoptr(VentureEntity) hosting = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *bill_template = NULL;
	g_autofree gchar *next_year = NULL;
	g_autofree gchar *last_year = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) later = g_date_time_add_years(now, 1);
	g_autoptr(GDateTime) earlier = g_date_time_add_years(now, -1);
	gint64 landlord = vendor(f, "Landlord");
	guint row;
	(void)unused;

	next_year = venture_time_to_string(later);
	last_year = venture_time_to_string(earlier);
	bill_template = g_strdup_printf(
		"{\"company_id\":%" G_GINT64_FORMAT ",\"currency\":\"USD\",\"lines\":["
		"{\"description\":\"Rent\",\"quantity\":\"1\",\"unit_price\":\"1200 USD\"},"
		"{\"description\":\"Parking\",\"quantity\":\"2\",\"unit_price\":\"50 USD\"}]}",
		landlord);

	/* Monthly 1300 rent bill; weekly 4.50 coffee; yearly 24 domain;
	 * daily 10 cleaner. */
	rent = schedule(f, "Rent", "bill", "monthly", "2026-01-01", bill_template);
	coffee = schedule(f, "Coffee", "expense", "weekly", "2026-01-05",
		"{\"description\":\"Coffee\",\"amount\":\"4.50 USD\",\"vendor\":\"Cafe\"}");
	domain = schedule(f, "Domain", "expense", "yearly", "2026-03-01",
		"{\"description\":\"Domain\",\"amount\":\"24 USD\",\"vendor\":\"Registrar\"}");
	cleaner = schedule(f, "Cleaner", "expense", "daily", "2026-01-01",
		"{\"description\":\"Cleaning\",\"amount\":\"10 USD\",\"vendor\":\"Cleaners\"}");
	/* Not costs: a cancelled schedule, a paused one, one that has ended,
	 * and a customer invoice schedule. */
	cancelled = schedule(f, "Old tool", "expense", "monthly", "2026-01-01",
		"{\"description\":\"Tool\",\"amount\":\"999 USD\",\"vendor\":\"Tools\"}");
	g_assert_true(venture_database_delete(f->db, cancelled, NULL, &error));
	g_assert_no_error(error);
	paused = schedule(f, "Paused tool", "expense", "monthly", "2026-01-01",
		"{\"description\":\"Tool\",\"amount\":\"999 USD\",\"vendor\":\"Tools\"}");
	g_assert_true(venture_recurring_service_pause(venture_recurring_service_get(f->db), paused, NULL, &error));
	g_assert_no_error(error);
	ended = record_new(f, "recurring_schedule");
	g_object_set(ended, "name", "Ended tool", "timezone", "UTC",
		"template", "{\"description\":\"Tool\",\"amount\":\"999 USD\",\"vendor\":\"Tools\"}", NULL);
	field(ended, "kind", "expense");
	field(ended, "frequency", "monthly");
	field(ended, "start-at", "2025-01-01");
	field(ended, "end-at", last_year);
	save(f, ended);
	hosting = schedule(f, "Hosting", "invoice", "monthly", "2026-01-01",
		"{\"company_id\":1,\"lines\":[{\"description\":\"Hosting\",\"quantity\":1,\"unit_price\":\"40 USD\"}]}");

	result = run_report(f, "recurring_costs", "this_month", NULL);
	/* 1300 + 4.50 x 52/12 (19.50) + 24/12 (2) + 10 x 365/12 (304.17). */
	g_assert_cmpint(money_metric(result, "monthly"), ==, 162567);
	g_assert_cmpint(money_metric(result, "annual"), ==, 1950804);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "schedules")), ==, 4.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "excluded")), ==, 3.0);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 4);
	row = row_named(result, "schedule", "Rent");
	g_assert_cmpint(money_cell(result, row, "amount"), ==, 130000);
	g_assert_cmpint(money_cell(result, row, "monthly"), ==, 130000);
	{
		g_autofree gchar *who = cell(result, row, "vendor");
		g_autofree gchar *kind = cell(result, row, "kind");
		g_autofree gchar *due = cell(result, row, "next_due");
		g_assert_cmpstr(who, ==, "Landlord");
		g_assert_cmpstr(kind, ==, "bill");
		g_assert_cmpstr(due, ==, "2026-01-01");
	}
	row = row_named(result, "schedule", "Coffee");
	g_assert_cmpint(money_cell(result, row, "monthly"), ==, 1950);
	{
		g_autofree gchar *who = cell(result, row, "vendor");
		g_assert_cmpstr(who, ==, "Cafe");
	}
	row = row_named(result, "schedule", "Domain");
	g_assert_cmpint(money_cell(result, row, "monthly"), ==, 200);
	row = row_named(result, "schedule", "Cleaner");
	g_assert_cmpint(money_cell(result, row, "monthly"), ==, 30417);
	g_assert_false(has_row(result, "schedule", "Old tool"));
	g_assert_false(has_row(result, "schedule", "Paused tool"));
	g_assert_false(has_row(result, "schedule", "Ended tool"));
	g_assert_false(has_row(result, "schedule", "Hosting"));
	/* The largest monthly cost comes first. */
	{
		g_autofree gchar *top = cell(result, 0, "schedule");
		g_assert_cmpstr(top, ==, "Rent");
	}
	/* Ending a running schedule takes it out of the run-rate. */
	field(rent, "end-at", last_year);
	save(f, rent);
	g_clear_object(&result);
	result = run_report(f, "recurring_costs", "this_month", NULL);
	g_assert_cmpint(money_metric(result, "monthly"), ==, 32567);
}

/* A schedule whose template cannot be priced is listed with nothing
 * invented for it, and a euro schedule is not added into dollars. */
static void
test_recurring_costs_edges(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) broken = NULL;
	g_autoptr(VentureEntity) euro = NULL;
	g_autoptr(VentureEntity) fine = NULL;
	guint row;
	(void)unused;
	broken = schedule(f, "Mystery", "expense", "monthly", "2026-01-01",
		"{\"description\":\"No amount\",\"vendor\":\"Nobody\"}");
	euro = schedule(f, "Euro", "expense", "monthly", "2026-01-01",
		"{\"description\":\"Euro\",\"amount\":\"20 EUR\",\"vendor\":\"EU\"}");
	fine = schedule(f, "Fine", "expense", "monthly", "2026-01-01",
		"{\"description\":\"Fine\",\"amount\":\"30 USD\",\"vendor\":\"US\"}");
	result = run_report(f, "recurring_costs", "this_month", NULL);
	g_assert_cmpint(money_metric(result, "monthly"), ==, 3000);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	row = row_named(result, "schedule", "Mystery");
	g_assert_null(venture_report_result_get_cell(result, row, "monthly"));
	row = row_named(result, "schedule", "Euro");
	g_assert_cmpint(money_cell(result, row, "monthly"), ==, 2000);
	g_assert_nonnull(strstr(note(result), "could not be included"));
	g_assert_nonnull(strstr(note(result), "priced"));
}

/* --- 4. The weekly cash outlook -------------------------------------------- */

static void
bank_statement(Fixture *f, const gchar *closing)
{
	g_autoptr(VentureEntity) account = record_new(f, "bank_account");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) cash = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	g_autoptr(VentureEntity) statement = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", &error));
	cash = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cash);
	g_object_set(account, "account-id", venture_entity_get_id(cash), "name", "Main", "currency", "USD",
		"date-column", "Date", "amount-column", "Amount", "description-column", "Memo",
		"reference-column", "Ref", "external-id-column", "ID", "date-format", "%Y-%m-%d",
		"sign-convention", "normal", NULL);
	save(f, account);
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "990 USD");
	json_object_set_string_member(args, "closing_balance", closing);
	json_object_set_string_member(args, "format", "csv");
	json_object_set_string_member(args, "data", "Date,Amount,Memo,Ref,ID\n2026-01-10,10,Interest,Jan,int-1\n");
	statement = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db), "import",
		venture_entity_get_id(account), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
}

static void
test_cash_outlook(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) shorter = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	gint64 acme = company(f, "Acme", NULL);
	gint64 host = vendor(f, "Host");
	gint64 partly;
	gint64 paid;
	guint row;
	(void)unused;

	bank_statement(f, "1000 USD");

	/* Cash out: 100 due in week 1, 300 due in week 3 of which 100 is
	 * already paid, 50 overdue, 80 undated, 700 beyond eight weeks; a draft
	 * and a paid bill count for nothing. */
	bill(f, host, "W1", "100 USD", "hosting", FALSE, "2026-03-20", "2026-04-03", TRUE);
	partly = bill(f, host, "W3", "300 USD", "hosting", FALSE, "2026-03-20", "2026-04-16", TRUE);
	pay_bill(f, host, partly, "100 USD", "2026-03-25");
	bill(f, host, "LATE", "50 USD", "hosting", FALSE, "2026-03-01", "2026-03-20", TRUE);
	bill(f, host, "UNDATED", "80 USD", "hosting", FALSE, "2026-03-20", NULL, TRUE);
	bill(f, host, "FAR", "700 USD", "hosting", FALSE, "2026-03-20", "2026-07-01", TRUE);
	bill(f, host, "DRAFT", "999 USD", "hosting", FALSE, "2026-03-20", "2026-04-03", FALSE);
	paid = bill(f, host, "PAID", "999 USD", "hosting", FALSE, "2026-03-20", "2026-04-03", TRUE);
	pay_bill(f, host, paid, "999 USD", "2026-03-21");

	/* Cash in: 400 due in week 1, 250 due in week 2, 60 undated, a paid
	 * invoice and a draft that count for nothing. */
	issued_invoice(f, acme, "I1", "400 USD", "2026-03-15", "2026-04-02");
	issued_invoice(f, acme, "I2", "250 USD", "2026-03-15", "2026-04-10");
	issued_invoice(f, acme, "I-UNDATED", "60 USD", "2026-03-15", NULL);
	paid_invoice(f, acme, "I-PAID", "999 USD", "2026-03-15", "2026-03-16");
	{
		g_autoptr(VentureEntity) draft = record_new(f, "invoice");
		g_object_set(draft, "number", "I-DRAFT", "company-id", acme, NULL);
		field(draft, "issued-at", "2026-03-15");
		field(draft, "due-at", "2026-04-02");
		save(f, draft);
	}

	/* Eight weeks from 1 April: week 1 is 1-7 April, week 2 is 8-14,
	 * week 3 is 15-21; 1 July is beyond. */
	result = run_report(f, "cash_outlook", "2026-04-01", NULL);
	g_assert_cmpint(money_metric(result, "opening"), ==, 100000);
	g_assert_cmpint(money_metric(result, "cash_in"), ==, 65000);
	g_assert_cmpint(money_metric(result, "cash_out"), ==, 35000);
	g_assert_cmpint(money_metric(result, "closing"), ==, 130000);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "weeks")), ==, 8.0);
	/* Overdue, eight weeks, later, undated. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 11);
	row = row_named(result, "bucket", "Overdue");
	g_assert_cmpint(money_cell(result, row, "cash_out"), ==, 5000);
	g_assert_cmpint(money_cell(result, row, "cash_in"), ==, 0);
	g_assert_cmpint(money_cell(result, row, "balance"), ==, 95000);
	row = row_named(result, "bucket", "Week 1");
	{
		g_autofree gchar *from = cell(result, row, "from");
		g_autofree gchar *to = cell(result, row, "to");
		g_assert_cmpstr(from, ==, "2026-04-01");
		g_assert_cmpstr(to, ==, "2026-04-07");
	}
	g_assert_cmpint(money_cell(result, row, "cash_in"), ==, 40000);
	g_assert_cmpint(money_cell(result, row, "cash_out"), ==, 10000);
	g_assert_cmpint(money_cell(result, row, "net"), ==, 30000);
	g_assert_cmpint(money_cell(result, row, "balance"), ==, 125000);
	row = row_named(result, "bucket", "Week 2");
	g_assert_cmpint(money_cell(result, row, "cash_in"), ==, 25000);
	g_assert_cmpint(money_cell(result, row, "balance"), ==, 150000);
	row = row_named(result, "bucket", "Week 3");
	g_assert_cmpint(money_cell(result, row, "cash_out"), ==, 20000);
	g_assert_cmpint(money_cell(result, row, "balance"), ==, 130000);
	row = row_named(result, "bucket", "Week 8");
	g_assert_cmpint(money_cell(result, row, "balance"), ==, 130000);
	/* Beyond the horizon is shown but not in the closing figure. */
	row = row_named(result, "bucket", "Later");
	g_assert_cmpint(money_cell(result, row, "cash_out"), ==, 70000);
	g_assert_null(venture_report_result_get_cell(result, row, "balance"));
	/* Undated documents are listed with no balance invented for them. */
	row = row_named(result, "bucket", "Undated");
	g_assert_cmpint(money_cell(result, row, "cash_out"), ==, 8000);
	g_assert_cmpint(money_cell(result, row, "cash_in"), ==, 6000);
	g_assert_null(venture_report_result_get_cell(result, row, "balance"));
	g_assert_nonnull(strstr(note(result), "undated"));

	/* weeks=2 narrows the horizon: week 3's bill moves to Later. */
	json_object_set_int_member(options, "weeks", 2);
	shorter = run_report(f, "cash_outlook", "2026-04-01", options);
	g_assert_cmpfloat(venture_metric_get_number(metric(shorter, "weeks")), ==, 2.0);
	g_assert_cmpint(money_metric(shorter, "cash_out"), ==, 15000);
	g_assert_cmpint(money_metric(shorter, "closing"), ==, 150000);
	row = row_named(shorter, "bucket", "Later");
	g_assert_cmpint(money_cell(shorter, row, "cash_out"), ==, 90000);
}

/* No statement means no opening figure, and therefore no running balance:
 * the flows are still shown, the balance is n/a rather than zero. */
static void
test_cash_outlook_without_bank(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	gint64 host = vendor(f, "Host");
	guint row;
	(void)unused;
	bill(f, host, "W1", "100 USD", "hosting", FALSE, "2026-03-20", "2026-04-03", TRUE);
	result = run_report(f, "cash_outlook", "2026-04-01", NULL);
	g_assert_null(venture_metric_get_money(metric(result, "opening")));
	g_assert_cmpstr(venture_metric_get_text(metric(result, "opening")), ==, "no statements");
	g_assert_null(venture_metric_get_money(metric(result, "closing")));
	g_assert_cmpstr(venture_metric_get_text(metric(result, "closing")), ==, "n/a");
	g_assert_cmpint(money_metric(result, "cash_out"), ==, 10000);
	row = row_named(result, "bucket", "Week 1");
	g_assert_cmpint(money_cell(result, row, "cash_out"), ==, 10000);
	g_assert_null(venture_report_result_get_cell(result, row, "balance"));
	g_assert_nonnull(strstr(note(result), "statement"));
}

/* A horizon of zero or a negative number of weeks is refused. */
static void
test_cash_outlook_refusals(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report = venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "cash_outlook");
	(void)unused;
	g_assert_nonnull(report);
	period = venture_context_parse_period(f->context, "2026-04-01", &error);
	g_assert_no_error(error);
	json_object_set_int_member(options, "weeks", 0);
	g_assert_null(venture_report_generate(report, f->context, period, options, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	json_object_set_int_member(options, "weeks", 200);
	g_assert_null(venture_report_generate(report, f->context, period, options, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- 5. The P&L card ------------------------------------------------------- */

static void
test_pnl_card_links(Fixture *f, gconstpointer unused)
{
	g_autoptr(JsonNode) cards = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *array;
	JsonObject *card;
	JsonArray *links;
	JsonArray *lines;
	static const gchar *const expected[] = {
		"/reports/revenue_by_customer", "/reports/spend_by_vendor",
		"/reports/recurring_costs", "/reports/cash_outlook"
	};
	guint i;
	(void)unused;
	bank_statement(f, "1234 USD");
	period = venture_context_parse_period(f->context, "2026-01", &error);
	g_assert_no_error(error);
	cards = venture_headline_home_cards(f->context, f->org, period, &error);
	g_assert_no_error(error);
	array = json_node_get_array(cards);
	card = json_array_get_object_element(array, 0);
	g_assert_cmpstr(json_object_get_string_member(card, "key"), ==, "pnl");
	/* Bank cash is still beside the booked figures. */
	lines = json_object_get_array_member(card, "lines");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(lines, 2), "label"), ==, "Bank cash");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(lines, 2), "value"), ==, "$1,234.00");
	/* And the four cuts hang off it. */
	g_assert_true(json_object_has_member(card, "links"));
	links = json_object_get_array_member(card, "links");
	g_assert_cmpuint(json_array_get_length(links), ==, 4);
	for (i = 0; i < 4; i++)
	{
		JsonObject *link = json_array_get_object_element(links, i);
		g_assert_cmpstr(json_object_get_string_member(link, "href"), ==, expected[i]);
		g_assert_true(json_object_has_member(link, "label"));
	}
	/* The other cards carry no links; the P&L is the one with cuts. */
	card = json_array_get_object_element(array, 1);
	g_assert_false(json_object_has_member(card, "links"));
}

/* --- 6. The module --------------------------------------------------------- */

static void
test_module_switch(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureConfig) off = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	VentureReportRegistry *reports;
	(void)unused;
	reports = venture_context_get_report_registry(f->context);
	g_assert_nonnull(venture_report_registry_lookup(reports, "revenue_by_customer"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "spend_by_vendor"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "recurring_costs"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "cash_outlook"));
	/* The budgets module's own forecast is untouched. */
	g_assert_nonnull(venture_report_registry_lookup(reports, "cash_forecast"));

	venture_config_set_module_enabled(off, "pnl_cuts", FALSE);
	context = venture_context_new(off, f->db);
	reports = venture_context_get_report_registry(context);
	g_assert_null(venture_report_registry_lookup(reports, "revenue_by_customer"));
	g_assert_null(venture_report_registry_lookup(reports, "spend_by_vendor"));
	g_assert_null(venture_report_registry_lookup(reports, "recurring_costs"));
	g_assert_null(venture_report_registry_lookup(reports, "cash_outlook"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "pnl"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/pnl_cuts/revenue_by_customer", Fixture, NULL, setup, test_revenue_by_customer, teardown);
	g_test_add("/pnl_cuts/revenue_by_customer/currencies", Fixture, NULL, setup, test_revenue_currencies, teardown);
	g_test_add("/pnl_cuts/spend_by_vendor", Fixture, NULL, setup, test_spend_by_vendor, teardown);
	g_test_add("/pnl_cuts/spend_by_vendor/edges", Fixture, NULL, setup, test_spend_edges, teardown);
	g_test_add("/pnl_cuts/recurring_costs", Fixture, NULL, setup, test_recurring_costs, teardown);
	g_test_add("/pnl_cuts/recurring_costs/edges", Fixture, NULL, setup, test_recurring_costs_edges, teardown);
	g_test_add("/pnl_cuts/cash_outlook", Fixture, NULL, setup, test_cash_outlook, teardown);
	g_test_add("/pnl_cuts/cash_outlook/without_bank", Fixture, NULL, setup, test_cash_outlook_without_bank, teardown);
	g_test_add("/pnl_cuts/cash_outlook/refusals", Fixture, NULL, setup, test_cash_outlook_refusals, teardown);
	g_test_add("/pnl_cuts/pnl_card/links", Fixture, NULL, setup, test_pnl_card_links, teardown);
	g_test_add("/pnl_cuts/module", Fixture, NULL, setup, test_module_switch, teardown);
	return g_test_run();
}
