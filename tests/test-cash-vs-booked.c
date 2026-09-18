/*
 * test-cash-vs-booked.c - Revenue booked beside cash received, per bucket
 * and per currency, and the same figures on the home card.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>

#include <string.h>
#include <unistd.h>
#include <libsoup/soup.h>

#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	gint64 organization_id;
	gint64 customer_id;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->database, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static VentureEntity *
record_new(Fixture *f, const gchar *name)
{
	VentureEntity *record;
	GType type;

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	record = g_object_new(type, NULL);
	venture_entity_set_organization_id(record, f->organization_id);
	return record;
}

static gint64
company_new(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) company = NULL;

	company = record_new(f, "company");
	g_object_set(company, "name", name, NULL);
	save(f, company);
	return venture_entity_get_id(company);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	f->config = venture_config_new();
	f->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->organization_id = venture_context_get_default_organization_id(f->context);
	f->customer_id = company_new(f, "Customer");
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->database);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
}

static void
field(VentureEntity *record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(record, name, value, &error));
	g_assert_no_error(error);
}

/* An issued invoice for @customer, dated @issued, one line of @amount. */
static VentureEntity *
invoice_for(Fixture *f, gint64 customer, gint64 venture_id, const gchar *number,
	const gchar *issued, const gchar *amount)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) line = NULL;

	invoice = record_new(f, "invoice");
	g_object_set(invoice, "number", number, "company-id", customer, NULL);
	if (venture_id != 0)
		g_object_set(invoice, "venture-id", venture_id, NULL);
	field(invoice, "issued-at", issued);
	field(invoice, "due-at", issued);
	save(f, invoice);
	line = record_new(f, "invoice_line");
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice),
		"description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", amount);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	return g_steal_pointer(&invoice);
}

static VentureEntity *
invoice_new(Fixture *f, const gchar *number, const gchar *issued, const gchar *amount)
{
	return invoice_for(f, f->customer_id, 0, number, issued, amount);
}

/* A receipt from @customer on @date, applied to @invoice_id when nonzero. */
static VentureEntity *
payment_for(Fixture *f, gint64 customer, gint64 invoice_id, const gchar *amount, const gchar *date)
{
	VentureEntity *payment;

	payment = record_new(f, "payment");
	g_object_set(payment, "customer-id", customer, "invoice-id", invoice_id,
		"method", "manual", NULL);
	field(payment, "amount", amount);
	field(payment, "date", date);
	save(f, payment);
	return payment;
}

static VentureEntity *
payment_new(Fixture *f, gint64 invoice_id, const gchar *amount, const gchar *date)
{
	return payment_for(f, f->customer_id, invoice_id, amount, date);
}

static VentureDateRange *
period_new(const gchar *text)
{
	g_autoptr(GTimeZone) utc = NULL;
	g_autoptr(GError) error = NULL;
	VentureDateRange *period;

	/* Bare record dates are UTC; keep the buckets in that timezone rather
	 * than the machine running the test. */
	utc = g_time_zone_new_utc();
	period = venture_date_range_parse(text, utc, 1, &error);
	g_assert_no_error(error);
	g_assert_nonnull(period);
	return period;
}

static VentureReportResult *
run_report(Fixture *f, const gchar *period_text, JsonObject *options, GError **error)
{
	g_autoptr(VentureDateRange) period = NULL;
	VentureReport *report;

	period = period_new(period_text);
	report = venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "cash_vs_booked");
	g_assert_nonnull(report);
	return venture_report_generate(report, f->context, period, options, error);
}

static VentureReportResult *
report(Fixture *f, const gchar *period_text, JsonObject *options)
{
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;

	result = run_report(f, period_text, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

/* A cell as canonical text: money as "10.00 USD", text as itself. */
static gchar *
cell(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	if (G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY))
		return venture_money_to_string(g_value_get_boxed(value));
	return g_value_dup_string(value);
}

static void
assert_cell(VentureReportResult *result, guint row, const gchar *key, const gchar *expected)
{
	g_autofree gchar *text = cell(result, row, key);

	g_assert_cmpstr(text, ==, expected);
}

static void
assert_row(VentureReportResult *result, guint row, const gchar *bucket,
	const gchar *currency, const gchar *booked, const gchar *received,
	const gchar *gap, const gchar *cumulative)
{
	assert_cell(result, row, "bucket", bucket);
	assert_cell(result, row, "currency", currency);
	assert_cell(result, row, "booked", booked);
	assert_cell(result, row, "received", received);
	assert_cell(result, row, "gap", gap);
	assert_cell(result, row, "cumulative_gap", cumulative);
}

static VentureMetric *
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics;
	guint i;

	metrics = venture_report_result_get_metrics(result);
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *found = g_ptr_array_index(metrics, i);

		if (g_strcmp0(venture_metric_get_key(found), key) == 0)
			return found;
	}
	g_assert_not_reached();
}

static void
assert_money_metric(VentureReportResult *result, const gchar *key, const gchar *expected)
{
	g_autofree gchar *text = NULL;

	text = venture_money_to_string(venture_metric_get_money(metric(result, key)));
	g_assert_cmpstr(text, ==, expected);
}

/* Registered under its own name, with the columns the issue asks for, and
 * a usable result on an empty period. */
static void
test_registered(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_auto(GStrv) keys = NULL;

	result = report(f, "2026-07", NULL);
	g_assert_cmpstr(venture_report_result_get_title(result), ==, "Cash vs booked");
	keys = venture_report_result_get_column_keys(result);
	g_assert_cmpuint(g_strv_length(keys), ==, 6);
	g_assert_cmpstr(keys[0], ==, "bucket");
	g_assert_cmpstr(keys[1], ==, "currency");
	g_assert_cmpstr(keys[2], ==, "booked");
	g_assert_cmpstr(keys[3], ==, "received");
	g_assert_cmpstr(keys[4], ==, "gap");
	g_assert_cmpstr(keys[5], ==, "cumulative_gap");
	/* Nothing booked and nothing received is a zero row per month, not an
	 * empty table: the reader sees the months they asked about. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	assert_row(result, 0, "Jul 2026", "USD", "0.00 USD", "0.00 USD", "0.00 USD", "0.00 USD");
	assert_money_metric(result, "booked", "0.00 USD");
	assert_money_metric(result, "received", "0.00 USD");
	assert_money_metric(result, "gap", "0.00 USD");
}

/*
 * July: a 100 USD invoice, 60 USD received. August: a 50 USD invoice with
 * a 20 USD credit note against it, a 30 USD invoice voided, the July
 * invoice's remaining 40 USD received, and 10 USD of July's receipt
 * refunded. Booked is issues less voids and credits by date; received is
 * receipts less refunds by date; the gap accumulates across the buckets.
 */
static void
test_month_buckets(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) july = NULL;
	g_autoptr(VentureEntity) august = NULL;
	g_autoptr(VentureEntity) voided = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GDateTime) void_date = NULL;
	g_autoptr(GError) error = NULL;

	july = invoice_new(f, "JUL", "2026-07-05", "100 USD");
	first = payment_new(f, venture_entity_get_id(july), "60 USD", "2026-07-20");
	august = invoice_new(f, "AUG", "2026-08-03", "50 USD");
	voided = invoice_new(f, "VOID", "2026-08-04", "30 USD");
	void_date = g_date_time_new_utc(2026, 8, 6, 0, 0, 0);
	g_assert_true(venture_settlement_service_transition(
		venture_settlement_service_get(f->database), VENTURE_INVOICE(voided),
		"void", void_date, NULL, &error));
	g_assert_no_error(error);
	credit = record_new(f, "customer_credit");
	g_object_set(credit, "customer-id", f->customer_id, "kind", "credit_note", NULL);
	field(credit, "amount", "20 USD");
	field(credit, "date", "2026-08-15");
	save(f, credit);
	allocation = record_new(f, "payment_allocation");
	g_object_set(allocation, "credit-id", venture_entity_get_id(credit),
		"invoice-id", venture_entity_get_id(august), NULL);
	field(allocation, "amount", "20 USD");
	field(allocation, "date", "2026-08-16");
	save(f, allocation);
	second = payment_new(f, venture_entity_get_id(july), "40 USD", "2026-08-10");

	query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	g_assert_true(venture_query_add_filter_int(query, "payment-id",
		VENTURE_FILTER_OP_EQ, venture_entity_get_id(first), &error));
	allocations = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(allocations->len, ==, 1);
	refund = record_new(f, "refund");
	g_object_set(refund, "customer-id", f->customer_id, "allocation-id",
		venture_entity_get_id(g_ptr_array_index(allocations, 0)), NULL);
	field(refund, "amount", "10 USD");
	field(refund, "date", "2026-08-20");
	save(f, refund);

	result = report(f, "2026-07-01..2026-08-31", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	assert_row(result, 0, "Jul 2026", "USD", "100.00 USD", "60.00 USD", "40.00 USD", "40.00 USD");
	assert_row(result, 1, "Aug 2026", "USD", "30.00 USD", "30.00 USD", "0.00 USD", "40.00 USD");
	assert_money_metric(result, "booked", "130.00 USD");
	assert_money_metric(result, "received", "90.00 USD");
	assert_money_metric(result, "gap", "40.00 USD");
	g_assert_cmpstr(venture_metric_get_text(metric(result, "booked_vs_received")), ==,
		"$130.00 booked, $90.00 received");

	/* The period is what is asked about: July alone carries no August
	 * receipt, and the cumulative gap starts at the period start. */
	g_clear_object(&result);
	result = report(f, "2026-08", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	assert_row(result, 0, "Aug 2026", "USD", "30.00 USD", "30.00 USD", "0.00 USD", "0.00 USD");
}

/* A period that starts on a Wednesday: the first and last weeks are
 * clipped to the period, and every bucket is labelled by its Monday. */
static void
test_week_buckets(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) third = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;

	first = invoice_new(f, "W1", "2026-07-02", "10 USD");
	second = invoice_new(f, "W2", "2026-07-06", "20 USD");
	third = invoice_new(f, "W3", "2026-07-14", "30 USD");
	payment = payment_new(f, venture_entity_get_id(first), "10 USD", "2026-07-12");

	options = json_object_new();
	json_object_set_string_member(options, "bucket", "week");
	result = report(f, "2026-07-01..2026-07-14", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	assert_row(result, 0, "Week of 29 Jun 2026", "USD", "10.00 USD", "0.00 USD", "10.00 USD", "10.00 USD");
	assert_row(result, 1, "Week of 6 Jul 2026", "USD", "20.00 USD", "10.00 USD", "10.00 USD", "20.00 USD");
	assert_row(result, 2, "Week of 13 Jul 2026", "USD", "30.00 USD", "0.00 USD", "30.00 USD", "50.00 USD");
	assert_money_metric(result, "gap", "50.00 USD");

	/* Month is the default and is also accepted by name. */
	json_object_set_string_member(options, "bucket", "month");
	g_clear_object(&result);
	result = report(f, "2026-07-01..2026-07-14", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	assert_row(result, 0, "Jul 2026", "USD", "60.00 USD", "10.00 USD", "50.00 USD", "50.00 USD");
}

/* The boxed splitter behind the week option, on its own. */
static void
test_split_by_week(void)
{
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GPtrArray) weeks = NULL;
	g_autofree gchar *first_start = NULL;
	g_autofree gchar *first_end = NULL;
	g_autofree gchar *last_end = NULL;
	VentureDateRange *week;

	period = period_new("2026-07-01..2026-07-19");
	weeks = venture_date_range_split_by_week(period);
	g_assert_cmpuint(weeks->len, ==, 3);
	week = g_ptr_array_index(weeks, 0);
	first_start = g_date_time_format_iso8601(venture_date_range_get_start(week));
	first_end = g_date_time_format_iso8601(venture_date_range_get_end(week));
	g_assert_cmpstr(first_start, ==, "2026-07-01T00:00:00Z");
	g_assert_cmpstr(first_end, ==, "2026-07-06T00:00:00Z");
	g_assert_cmpstr(venture_date_range_get_label(week), ==, "Week of 29 Jun 2026");
	week = g_ptr_array_index(weeks, 1);
	g_assert_cmpstr(venture_date_range_get_label(week), ==, "Week of 6 Jul 2026");
	week = g_ptr_array_index(weeks, 2);
	last_end = g_date_time_format_iso8601(venture_date_range_get_end(week));
	g_assert_cmpstr(last_end, ==, "2026-07-20T00:00:00Z");
	g_assert_cmpstr(venture_date_range_get_label(week), ==, "Week of 13 Jul 2026");

	g_clear_pointer(&period, venture_date_range_free);
	g_clear_pointer(&weeks, g_ptr_array_unref);
	period = venture_date_range_new_all_time();
	weeks = venture_date_range_split_by_week(period);
	g_assert_cmpuint(weeks->len, ==, 0);
}

/* Two currencies in one month stay two rows; the totals name one currency
 * and say what they left out. */
static void
test_currencies(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) dollars = NULL;
	g_autoptr(VentureEntity) euros = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;

	dollars = invoice_new(f, "USD", "2026-07-05", "100 USD");
	euros = invoice_new(f, "EUR", "2026-07-06", "80 EUR");
	payment = payment_new(f, venture_entity_get_id(euros), "80 EUR", "2026-08-02");

	result = report(f, "2026-07-01..2026-08-31", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 4);
	assert_row(result, 0, "Jul 2026", "EUR", "80.00 EUR", "0.00 EUR", "80.00 EUR", "80.00 EUR");
	assert_row(result, 1, "Jul 2026", "USD", "100.00 USD", "0.00 USD", "100.00 USD", "100.00 USD");
	assert_row(result, 2, "Aug 2026", "EUR", "0.00 EUR", "80.00 EUR", "-80.00 EUR", "0.00 EUR");
	assert_row(result, 3, "Aug 2026", "USD", "0.00 USD", "0.00 USD", "0.00 USD", "100.00 USD");
	assert_money_metric(result, "booked", "100.00 USD");
	assert_money_metric(result, "received", "0.00 USD");
	{
		g_autoptr(JsonNode) json = venture_metric_to_json(metric(result, "booked"));
		const gchar *note;

		note = venture_json_object_get_string(json_node_get_object(json), "note", NULL);
		g_assert_nonnull(note);
		g_assert_nonnull(strstr(note, "EUR"));
	}

	options = json_object_new();
	json_object_set_string_member(options, "currency", "EUR");
	g_clear_object(&result);
	result = report(f, "2026-07-01..2026-08-31", options);
	assert_money_metric(result, "booked", "80.00 EUR");
	assert_money_metric(result, "received", "80.00 EUR");
	assert_money_metric(result, "gap", "0.00 EUR");
}

/* customer_id keeps one customer's invoices and receipts. venture_id keeps
 * the venture's invoices, and the receipts applied to them: a receipt
 * belongs to a customer, so cash reaches a venture through allocation. */
static void
test_filters(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) venture = NULL;
	g_autoptr(VentureEntity) mine = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(VentureEntity) paid_mine = NULL;
	g_autoptr(VentureEntity) paid_theirs = NULL;
	g_autoptr(VentureEntity) deposit = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	gint64 stranger;

	venture = record_new(f, "venture");
	g_object_set(venture, "name", "Comics", "slug", "comics", NULL);
	save(f, venture);
	stranger = company_new(f, "Stranger");
	mine = invoice_for(f, f->customer_id, venture_entity_get_id(venture), "MINE", "2026-07-03", "100 USD");
	theirs = invoice_for(f, stranger, 0, "THEIRS", "2026-07-04", "70 USD");
	other = invoice_for(f, f->customer_id, 0, "OTHER", "2026-07-05", "50 USD");
	paid_mine = payment_new(f, venture_entity_get_id(mine), "60 USD", "2026-07-10");
	paid_theirs = payment_for(f, stranger, venture_entity_get_id(theirs), "70 USD", "2026-07-11");
	/* An unapplied deposit is cash received from the customer, but not
	 * cash for any venture. */
	deposit = payment_new(f, 0, "25 USD", "2026-07-12");

	options = json_object_new();
	json_object_set_int_member(options, "customer_id", f->customer_id);
	result = report(f, "2026-07", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	assert_row(result, 0, "Jul 2026", "USD", "150.00 USD", "85.00 USD", "65.00 USD", "65.00 USD");

	json_object_remove_member(options, "customer_id");
	json_object_set_int_member(options, "venture_id", venture_entity_get_id(venture));
	g_clear_object(&result);
	result = report(f, "2026-07", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	assert_row(result, 0, "Jul 2026", "USD", "100.00 USD", "60.00 USD", "40.00 USD", "40.00 USD");

	g_clear_object(&result);
	result = report(f, "2026-07", NULL);
	assert_row(result, 0, "Jul 2026", "USD", "220.00 USD", "155.00 USD", "65.00 USD", "65.00 USD");
}

/* A migrated invoice is not revenue booked here and its receipt through
 * the cutover is not cash received: both carry the opening stamp. */
static void
test_opening_excluded(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) clearing = NULL;
	g_autoptr(VentureEntity) real = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GDateTime) cutover = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GError) error = NULL;
	VentureSettlementService *service;
	gboolean ok;

	invoice = record_new(f, "invoice");
	g_object_set(invoice, "number", "OLD-1", "company-id", f->customer_id, NULL);
	field(invoice, "issued-at", "2026-07-02");
	field(invoice, "due-at", "2026-07-20");
	line = record_new(f, "invoice_line");
	g_object_set(line, "description", "Migrated", "quantity", 1.0, NULL);
	field(line, "unit-price", "300 USD");
	lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(lines, g_object_ref(line));
	cutover = g_date_time_new_utc(2026, 7, 31, 0, 0, 0);
	clearing = record_new(f, "account");
	g_object_set(clearing, "code", "3900", "name", "Opening balance clearing",
		"kind", VENTURE_ACCOUNT_KIND_EQUITY, "active", TRUE, NULL);
	save(f, clearing);
	service = venture_settlement_service_get(f->database);
	ok = venture_settlement_service_issue_opening(service, VENTURE_INVOICE(invoice),
		lines, cutover, venture_entity_get_id(clearing), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	real = invoice_new(f, "NEW-1", "2026-07-09", "40 USD");

	result = report(f, "2026-07", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	assert_row(result, 0, "Jul 2026", "USD", "40.00 USD", "0.00 USD", "40.00 USD", "40.00 USD");
}

/* An option the report does not know is refused rather than ignored, and
 * so is a bucket it cannot cut. */
static void
test_refuse_options(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(GError) error = NULL;

	options = json_object_new();
	json_object_set_string_member(options, "granularity", "week");
	result = run_report(f, "2026-07", options, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "granularity"));
	g_clear_error(&error);

	json_object_remove_member(options, "granularity");
	json_object_set_string_member(options, "bucket", "day");
	result = run_report(f, "2026-07", options, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "month"));
	g_assert_nonnull(strstr(error->message, "week"));
	g_clear_error(&error);

	/* The options every report surface adds are not unknown. */
	json_object_remove_member(options, "bucket");
	json_object_set_int_member(options, "organization_id", f->organization_id);
	json_object_set_string_member(options, "currency", "USD");
	json_object_set_string_member(options, "as_of", "2026-07-31");
	json_object_set_int_member(options, "customer_id", f->customer_id);
	json_object_set_int_member(options, "venture_id", 0);
	result = run_report(f, "2026-07", options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

/* as_of cuts the evidence off, so a receipt after it is not yet cash. */
static void
test_as_of(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = NULL;

	invoice = invoice_new(f, "ASOF", "2026-07-05", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "100 USD", "2026-07-25");
	options = json_object_new();
	json_object_set_string_member(options, "as_of", "2026-07-20");
	result = report(f, "2026-07", options);
	assert_row(result, 0, "Jul 2026", "USD", "100.00 USD", "0.00 USD", "100.00 USD", "100.00 USD");
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

static JsonObject *
pnl_card(Fixture *f, const gchar *period_text)
{
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *cards;
	JsonArray *array;
	JsonObject *card;

	period = period_new(period_text);
	cards = venture_headline_home_cards(f->context, f->organization_id, period, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cards);
	array = json_node_get_array(cards);
	card = json_array_get_object_element(array, 0);
	g_assert_cmpstr(json_object_get_string_member(card, "key"), ==, "pnl");
	return json_object_ref(card);
}

/* The Money in card carries the report's own headline figure, so the two
 * cannot disagree; with receivables off there is no figure, not a zero. */
static void
test_home_card_line(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) card = NULL;

	invoice = invoice_new(f, "CARD", "2026-07-05", "100 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "60 USD", "2026-07-20");

	card = pnl_card(f, "2026-07");
	g_assert_cmpstr(line_value(card, "Booked vs received"), ==, "$100.00 booked, $60.00 received");
	result = report(f, "2026-07", NULL);
	g_assert_cmpstr(line_value(card, "Booked vs received"), ==,
		venture_metric_get_text(metric(result, "booked_vs_received")));

	venture_config_set_module_enabled(f->config, "receivables", FALSE);
	g_clear_pointer(&card, json_object_unref);
	card = pnl_card(f, "2026-07");
	g_assert_cmpstr(line_value(card, "Booked vs received"), ==, "n/a");
	venture_config_set_module_enabled(f->config, "receivables", TRUE);
}

/* --- Surfaces: the API, its CSV export and venturectl -------------------- */

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
	gchar *out;
	gchar *err;
} SurfaceResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response = data;

	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *path, gchar **out)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SurfaceResult response;
	guint status;

	memset(&response, 0, sizeof(response));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new("GET", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	status = soup_message_get_status(message);
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return status;
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response = data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &response->out, &response->err, &response->error);
	response->done = TRUE;
}

static gboolean
cli_timeout(gpointer data)
{
	g_subprocess_force_exit(G_SUBPROCESS(data));
	return G_SOURCE_CONTINUE;
}

static gchar *
run_cli(const gchar *const *argv, gboolean success)
{
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	SurfaceResult response;
	guint timeout;

	memset(&response, 0, sizeof(response));
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "cash-vs-booked-test-only", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(response.error);
	if (g_subprocess_get_successful(process) != success)
		g_test_message("CLI stdout: %s; stderr: %s", response.out, response.err);
	g_assert_cmpint(g_subprocess_get_successful(process), ==, success);
	if (!success)
	{
		g_free(response.out);
		return response.err;
	}
	g_free(response.err);
	return response.out;
}

static VentureWebServer *
start_server(Fixture *f, gchar **state_dir)
{
	g_autoptr(GSocketListener) listener = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	guint16 port;

	*state_dir = g_dir_make_tmp("venture-cash-vs-booked-XXXXXX", &error);
	g_assert_no_error(error);
	listener = g_socket_listener_new();
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", *state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

/* The bucket option reaches the report through the API and venturectl,
 * the CSV export carries the week rows, and a bad bucket is refused at
 * the door with the report's own message. */
static void
test_surfaces(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(JsonNode) expected = NULL;
	g_autoptr(JsonNode) actual = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *csv = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *response = NULL;
	g_autofree gchar *cli = g_canonicalize_filename("build/debug/venturectl", NULL);
	const gchar *argv[] = { NULL, "--server", NULL, "-f", "json", "report", "cash_vs_booked",
		"2026-07-01..2026-07-14", "bucket=week", NULL };
	const gchar *bad[] = { NULL, "--server", NULL, "report", "cash_vs_booked",
		"2026-07", "bucket=day", NULL };

	invoice = invoice_new(f, "API", "2026-07-02", "10 USD");
	payment = payment_new(f, venture_entity_get_id(invoice), "10 USD", "2026-07-08");
	server = start_server(f, &state_dir);

	g_assert_cmpuint(http_request(server,
		"/api/v1/reports/cash_vs_booked?period=2026-07-01..2026-07-14&bucket=week", &body), ==, 200);
	g_assert_nonnull(strstr(body, "Week of 29 Jun 2026"));
	g_assert_nonnull(strstr(body, "Week of 6 Jul 2026"));
	expected = venture_json_parse(body, NULL);
	g_assert_nonnull(expected);

	argv[0] = cli;
	argv[2] = venture_web_server_get_base_url(server);
	response = run_cli(argv, TRUE);
	actual = venture_json_parse(response, NULL);
	g_assert_nonnull(actual);
	g_assert_true(json_node_equal(expected, actual));

	g_assert_cmpuint(http_request(server,
		"/api/v1/reports/cash_vs_booked?period=2026-07-01..2026-07-14&bucket=week&format=csv", &csv), ==, 200);
	g_assert_nonnull(strstr(csv, "Week,Currency,Booked,Received,Gap,Cumulative gap"));
	/* A negative gap starts with a minus, which the CSV writer quotes and
	 * prefixes so a spreadsheet does not read it as a formula. */
	g_assert_nonnull(strstr(csv, "Week of 29 Jun 2026,USD,$10.00,$0.00,$10.00,$10.00"));
	g_assert_nonnull(strstr(csv, "Week of 6 Jul 2026,USD,$0.00,$10.00,\"'-$10.00\",$0.00"));

	g_assert_cmpuint(http_request(server,
		"/reports/cash_vs_booked?period=2026-07-01..2026-07-14&bucket=week", &page), ==, 200);
	g_assert_nonnull(strstr(page, "Week of 6 Jul 2026"));
	g_assert_nonnull(strstr(page, "bucket=week"));

	g_assert_cmpuint(http_request(server,
		"/api/v1/reports/cash_vs_booked?period=2026-07&bucket=day", &refused), ==, 422);
	g_assert_nonnull(strstr(refused, "month"));

	bad[0] = cli;
	bad[2] = venture_web_server_get_base_url(server);
	g_clear_pointer(&response, g_free);
	response = run_cli(bad, FALSE);
	g_assert_nonnull(strstr(response, "month"));

	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(state_dir);
}

static void
test_docs(void)
{
	g_autofree gchar *reporting = NULL;
	g_autofree gchar *receivables = NULL;

	g_assert_true(g_file_get_contents("docs/reporting.org", &reporting, NULL, NULL));
	g_assert_nonnull(strstr(reporting, "=cash_vs_booked="));
	g_assert_nonnull(strstr(reporting, "bucket=week"));
	g_assert_true(g_file_get_contents("docs/receivables.org", &receivables, NULL, NULL));
	g_assert_nonnull(strstr(receivables, "cash_vs_booked"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(name, func) g_test_add("/cash-vs-booked/" name, Fixture, NULL, set_up, func, tear_down)
	ADD("registered", test_registered);
	ADD("month-buckets", test_month_buckets);
	ADD("week-buckets", test_week_buckets);
	ADD("currencies", test_currencies);
	ADD("filters", test_filters);
	ADD("opening-excluded", test_opening_excluded);
	ADD("refuse-options", test_refuse_options);
	ADD("as-of", test_as_of);
	ADD("home-card-line", test_home_card_line);
	ADD("surfaces", test_surfaces);
#undef ADD
	g_test_add_func("/cash-vs-booked/split-by-week", test_split_by_week);
	g_test_add_func("/cash-vs-booked/docs", test_docs);
	return g_test_run();
}
