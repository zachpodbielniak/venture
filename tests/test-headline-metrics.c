/*
 * test-headline-metrics.c - CAC, churn, LTV, LTV:CAC and the five cards
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every formula here has a refusal: a zero it will not divide by, a
 * threshold below which it will not project. Each rule gets a fixture that
 * makes the number checkable by hand, and each refusal a case that shows
 * "n/a" where a zero would have been a lie.
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
company(Fixture *f, const gchar *name, const gchar *source)
{
	g_autoptr(VentureEntity) record = record_new(f, "company");
	g_object_set(record, "name", name, "source", source, NULL);
	save(f, record);
	return venture_entity_get_id(record);
}

/* An issued invoice settled in full by a receipt on @paid: the customer's
 * "first paid invoice" is what CAC counts, and the receipt is what LTV
 * counts, so both come from the settlement service rather than a status. */
static gint64
paid_invoice(Fixture *f, gint64 company_id, const gchar *number, const gchar *amount, const gchar *paid)
{
	g_autoptr(VentureEntity) invoice = record_new(f, "invoice");
	g_autoptr(VentureEntity) line = record_new(f, "invoice_line");
	g_autoptr(VentureEntity) payment = record_new(f, "payment");
	g_autoptr(VentureEntity) stored = NULL;
	gint status = 0;
	g_object_set(invoice, "number", number, "company-id", company_id, NULL);
	field(invoice, "issued-at", paid);
	field(invoice, "due-at", paid);
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", amount);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	g_object_set(payment, "customer-id", company_id, "invoice-id", venture_entity_get_id(invoice), "method", "manual", NULL);
	field(payment, "amount", amount);
	field(payment, "date", paid);
	save(f, payment);
	stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_PAID);
	return venture_entity_get_id(invoice);
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

static gint64
campaign(Fixture *f, const gchar *name, const gchar *spend, const gchar *started)
{
	g_autoptr(VentureEntity) record = record_new(f, "campaign");
	g_object_set(record, "name", name, NULL);
	field(record, "spend", spend);
	field(record, "started-at", started);
	save(f, record);
	return venture_entity_get_id(record);
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

/* --- 1. The acquisition flag ---------------------------------------------- */

/* Classification must survive the same metadata and save path as money;
 * changing a category later must not silently rewrite historical spend. */
static void
test_acquisition(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureVendorBillLine) line = venture_vendor_bill_line_new();
	g_autoptr(VentureTaxCategory) category = venture_tax_category_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(2500, "USD");
	g_autoptr(GDateTime) date = venture_time_from_string("2026-01-15", NULL);
	gboolean acquisition = TRUE;
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(expense), "acquisition"));
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(line), "acquisition"));
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(category), "acquisition"));
	g_object_get(expense, "acquisition", &acquisition, NULL);
	g_assert_false(acquisition);
	g_object_get(line, "acquisition", &acquisition, NULL);
	g_assert_false(acquisition);
	g_object_set(category, "organization-id", f->org, "name", "Customer acquisition",
		"code", "headline-acquisition", "acquisition", TRUE, NULL);
	save(f, VENTURE_ENTITY(category));
	g_object_set(expense, "organization-id", f->org, "description", "Acquisition tooling",
		"amount", amount, "occurred-at", date,
		"tax-category-id", venture_entity_get_id(VENTURE_ENTITY(category)), NULL);
	save(f, VENTURE_ENTITY(expense));
	stored = venture_database_get(f->db, VENTURE_TYPE_EXPENSE,
		venture_entity_get_id(VENTURE_ENTITY(expense)), NULL);
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(stored), FALSE);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(json), "acquisition"));
	g_object_set(stored, "acquisition", FALSE, NULL);
	save(f, stored);
	g_clear_object(&stored);
	stored = venture_database_get(f->db, VENTURE_TYPE_EXPENSE,
		venture_entity_get_id(VENTURE_ENTITY(expense)), NULL);
	g_object_get(stored, "acquisition", &acquisition, NULL);
	g_assert_false(acquisition);

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
		g_object_get(stored_line, "acquisition", &acquisition, NULL);
		g_assert_true(acquisition);
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
	g_autoptr(VentureReportResult) empty = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *row_campaign = NULL;
	g_autofree gchar *row_cac = NULL;
	gint64 a = company(f, "Acme", NULL);
	gint64 b = company(f, "Bolt", NULL);
	gint64 c = company(f, "Cog", "referral");
	gint64 spring = campaign(f, "Spring", "300 USD", "2026-02-03");
	guint web_row = 0;

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

	result = run_report(f, "cac", "2026-02", NULL);
	g_assert_cmpint(money_metric(result, "spend"), ==, 55000);
	g_assert_cmpint(money_metric(result, "campaign_spend"), ==, 30000);
	g_assert_cmpint(money_metric(result, "expense_spend"), ==, 25000);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "new_customers")), ==, 2.0);
	g_assert_cmpint(money_metric(result, "cac"), ==, 27500);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	source = cell(result, 0, "source");
	if (g_strcmp0(source, "web") != 0)
	{
		g_free(source);
		web_row = 1;
		source = cell(result, 1, "source");
	}
	g_assert_cmpstr(source, ==, "web");
	row_campaign = cell(result, web_row, "campaign");
	g_assert_cmpstr(row_campaign, ==, "Spring");
	row_cac = venture_report_result_format_cell(result, web_row, "new_customers");
	g_assert_cmpstr(row_cac, ==, "1");
	g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(venture_report_result_get_cell(result, web_row, "cac"))), ==, 30000);
	g_free(source);
	source = cell(result, 1 - web_row, "source");
	g_assert_cmpstr(source, ==, "referral");

	/* March: spend, but nobody new. Reported, never divided. */
	empty = run_report(f, "cac", "2026-03", NULL);
	g_assert_cmpint(money_metric(empty, "spend"), ==, 99900);
	g_assert_cmpfloat(venture_metric_get_number(metric(empty, "new_customers")), ==, 0.0);
	g_assert_null(venture_metric_get_money(metric(empty, "cac")));
	g_assert_cmpstr(venture_metric_get_text(metric(empty, "cac")), ==, "n/a");
}

/* --- 3. Churn -------------------------------------------------------------- */

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
	g_autoptr(VentureEntity) cancelled = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *long_ago = days_ago(400);
	g_autofree gchar *quiet_since = days_ago(200);
	g_autofree gchar *last_month = days_ago(30);
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
	cancelled = venture_database_get(f->db, VENTURE_TYPE_RECURRING_SCHEDULE, gone, NULL);
	g_assert_true(venture_database_delete(f->db, cancelled, NULL, &error));
	g_assert_no_error(error);

	/* Cash: X paid 200 days ago only, Y a month ago, Z today. */
	paid_invoice(f, x, "X-1", "80 USD", quiet_since);
	paid_invoice(f, y, "Y-1", "80 USD", last_month);
	paid_invoice(f, z, "Z-1", "80 USD", just_now);

	result = run_report(f, "customer_churn", "this_month", NULL);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "active_at_start")), ==, 3.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "churned")), ==, 2.0);
	/* 2 of 3, in basis points, rendered: 6666 / 10000. */
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "recurring_churn")), ==, 0.6666);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "active_customers")), ==, 3.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "inactive_customers")), ==, 1.0);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "activity_churn")), ==, 0.3333);
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
	g_assert_cmpfloat(venture_metric_get_number(metric(loose, "days")), ==, 300.0);
}

/* --- 4. LTV ---------------------------------------------------------------- */

static void
test_ltv(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) projected = NULL;
	g_autoptr(VentureReportResult) quiet = NULL;
	g_autoptr(VentureEntity) setting = NULL;
	g_autoptr(VentureEntity) twin = NULL;
	g_autoptr(VentureEntity) refund = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) worklog = NULL;
	g_autoptr(VentureQuery) query = NULL;
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

	query = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, first, &error));
	allocation = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(allocation);
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
	setting = record_new(f, "headline_setting");
	g_object_set(setting, "minimum-customer-months", (gint64)3, NULL);
	field(setting, "hourly-rate", "20 USD");
	save(f, setting);
	twin = record_new(f, "headline_setting");
	g_assert_false(venture_database_save(f->db, twin, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	projected = run_report(f, "ltv", "2026-06", NULL);
	/* Support cost: 2.5 h x 20 = 50. Trailing-year revenue 490 (the P&L's
	 * cash sales are the same receipts less the refund), no expenses:
	 * margin (490 - 50) / 490 = 8979 bps. ARPU 490 / 4 months = 122.50.
	 * Monthly churn: 1 quiet of 3 over 12 months, so lifetime is
	 * 122.50 x 0.8979 = 109.99 (rounded once), x 36 = 3959.64. */
	g_assert_cmpint(money_metric(projected, "support_cost"), ==, 5000);
	g_assert_cmpint(money_metric(projected, "arpu"), ==, 12250);
	g_assert_cmpfloat(venture_metric_get_number(metric(projected, "gross_margin")), ==, 0.8979);
	g_assert_cmpfloat(venture_metric_get_number(metric(projected, "monthly_churn")), ==, 0.0277);
	g_assert_cmpint(money_metric(projected, "projected"), ==, 395964);

	/* With nobody quiet, lifetime is unbounded: n/a, not a number. */
	{
		g_autoptr(JsonObject) options = json_object_new();
		json_object_set_int_member(options, "days", 400);
		quiet = run_report(f, "ltv", "2026-06", options);
		g_assert_cmpstr(venture_metric_get_text(metric(quiet, "projected")), ==, "n/a");
	}
}

/* --- 5. LTV:CAC ------------------------------------------------------------ */

static void
test_ltv_cac(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) bare = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) setting = NULL;
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

	setting = record_new(f, "headline_setting");
	g_object_set(setting, "minimum-customer-months", (gint64)3, NULL);
	save(f, setting);

	/* Revenue 540 against the 100 of ads: margin 8148 bps. ARPU 540/4 =
	 * 135, x 0.8148 = 110.00. Churn 1 of 3 over 12 months: LTV 110 x 36
	 * = 3960. Over CAC 100 = 39.6. */
	result = run_report(f, "ltv_cac", "2026-06", NULL);
	g_assert_cmpint(money_metric(result, "projected_ltv"), ==, 396000);
	g_assert_cmpfloat(venture_metric_get_number(metric(result, "ltv_cac")), ==, 39.6);

	/* No new customer in May: CAC n/a, so the ratio is too. */
	{
		g_autoptr(VentureReportResult) may = run_report(f, "ltv_cac", "2026-05", NULL);
		g_assert_cmpstr(venture_metric_get_text(metric(may, "cac")), ==, "n/a");
		g_assert_cmpstr(venture_metric_get_text(metric(may, "ltv_cac")), ==, "n/a");
	}
}

/* --- 6. The five cards ----------------------------------------------------- */

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
	g_assert_cmpuint(json_array_get_length(array), ==, 5);
	for (i = 0; i < 5; i++)
	{
		card = json_array_get_object_element(array, i);
		g_assert_cmpstr(json_object_get_string_member(card, "key"), ==, keys[i]);
		g_assert_true(json_object_has_member(card, "value"));
		g_assert_true(json_object_has_member(card, "trend"));
		g_assert_true(g_str_has_prefix(json_object_get_string_member(card, "link"), "/reports/"));
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(card, "lines")), >=, 2);
	}
	/* July's profit is above June's: the arrow points up. */
	card = json_array_get_object_element(array, 0);
	g_assert_cmpint(json_object_get_int_member(card, "trend"), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(json_object_get_array_member(card, "lines"), 2), "value"), ==, "$1,234.00");
	/* One open ticket on the support card. */
	card = json_array_get_object_element(array, 4);
	g_assert_cmpstr(json_object_get_string_member(card, "value"), ==, "1");
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

static void
server_setup(ServerFixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;
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
	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/login", f->port);
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1", strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);
		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);
		set_cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	}
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');
	if (semicolon != NULL)
		*semicolon = '\0';
	f->cookie = g_steal_pointer(&set_cookie);
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
	g_autoptr(VentureEntity) setting = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	guint status;

	status = server_request(f, "GET", "/", NULL, &body);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(body, "headline-cards"));
	g_assert_nonnull(strstr(body, "data-card=\"pnl\""));
	g_assert_nonnull(strstr(body, "data-card=\"support\""));
	g_assert_nonnull(strstr(body, "href=\"/reports/cac\""));

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

	/* The module off hides the cards and the API; reports stay registered
	 * until the mask is applied, and / falls through to the built-in. */
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/headline/acquisition", Fixture, NULL, setup, test_acquisition, teardown);
	g_test_add("/headline/cac", Fixture, NULL, setup, test_cac, teardown);
	g_test_add("/headline/churn", Fixture, NULL, setup, test_churn, teardown);
	g_test_add("/headline/ltv", Fixture, NULL, setup, test_ltv, teardown);
	g_test_add("/headline/ltv_cac", Fixture, NULL, setup, test_ltv_cac, teardown);
	g_test_add("/headline/home/cards", Fixture, NULL, setup, test_home_cards, teardown);
	g_test_add("/headline/home/page", ServerFixture, NULL, server_setup, test_home_page, server_teardown);
	return g_test_run();
}
