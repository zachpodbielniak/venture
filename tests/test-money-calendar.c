/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-money-calendar.c - the money calendar (issue #93)
 *
 * Every dated money event on one grid: recurring schedules expanded
 * forward, bills and invoices on their due dates, dunning steps, payroll
 * runs and tax packs, with a per-day and per-ISO-week net per currency.
 */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org, customer, vendor, period;
	gint64 invoice, bill;
} Fixture;

/* The tax filing service prepares a pack through an adapter; the suite's
 * fake one is enough to give the pack a period and a tax amount. */
typedef struct { GObject parent; } FakeAdapter;
typedef struct { GObjectClass parent; } FakeAdapterClass;
GType fake_adapter_get_type(void);
static void fake_iface(VentureTaxFilingAdapterInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeAdapter, fake_adapter, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_TAX_FILING_ADAPTER, fake_iface))
static void fake_adapter_init(FakeAdapter *self) { (void)self; }
static void fake_adapter_class_init(FakeAdapterClass *klass) { (void)klass; }
static const gchar *fake_name(VentureTaxFilingAdapter *self) { (void)self; return "XX"; }
static const gchar *fake_rule(VentureTaxFilingAdapter *self) { (void)self; return "fake:1"; }
static const gchar *fake_country(VentureTaxFilingAdapter *self) { (void)self; return "XX"; }
static gboolean
fake_prepare(VentureTaxFilingAdapter *self, VentureDatabase *database, VentureEntity *filing, GError **error)
{
	g_autoptr(VentureMoney) tax = venture_money_from_string("75 USD", NULL, NULL);
	(void)self; (void)database; (void)error;
	g_object_set(filing, "json-pack", "{}", "csv-pack", "code,tax\n", "rule-id", "fake:1", "tax", tax, NULL);
	return TRUE;
}
static void
fake_iface(VentureTaxFilingAdapterInterface *iface)
{
	iface->get_name = fake_name;
	iface->get_rule_id = fake_rule;
	iface->get_country = fake_country;
	iface->prepare = fake_prepare;
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
static GDateTime *day(const gchar *text)
{
	GDateTime *at = venture_time_from_string(text, NULL);
	g_assert_nonnull(at);
	return at;
}

/* A sent invoice for the customer: 40 USD due on @due. */
static gint64
invoice(Fixture *f, const gchar *number, const gchar *due, const gchar *price)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) row = record(f, "invoice");
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(GDateTime) issued = day("2026-01-01");
	g_object_set(row, "number", number, "company-id", f->customer, NULL);
	field(row, "issued-at", "2026-01-01");
	field(row, "due-at", due);
	save(f, row);
	line = record(f, "invoice_line");
	g_object_set(line, "invoice-id", venture_entity_get_id(row), "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", price);
	save(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(row), "sent", issued, NULL, &error));
	g_assert_no_error(error);
	return venture_entity_get_id(row);
}

/* An approved bill for the vendor: 100 USD due on @due. */
static gint64
bill(Fixture *f, const gchar *number, const gchar *due, const gchar *currency, const gchar *price)
{
	g_autoptr(VentureEntity) row = record(f, "vendor_bill");
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_object_set(row, "number", number, "company-id", f->vendor, "currency", currency, "status", "draft", NULL);
	field(row, "bill-date", "2026-01-01");
	field(row, "due-date", due);
	save(f, row);
	line = record(f, "vendor_bill_line");
	g_object_set(line, "bill-id", venture_entity_get_id(row), "description", "Hosting", "quantity", "1", "category", "hosting", NULL);
	field(line, "unit-price", price);
	save(f, line);
	event = record(f, "vendor_bill_event");
	g_object_set(event, "bill-id", venture_entity_get_id(row), "vendor-id", f->vendor, "kind", "approve", "state", "approved", NULL);
	field(event, "date", "2026-01-01");
	save(f, event);
	return venture_entity_get_id(row);
}

static gint64
schedule(Fixture *f, const gchar *name, const gchar *kind, const gchar *frequency, const gchar *timezone,
	const gchar *start, const gchar *template)
{
	g_autoptr(VentureEntity) row = record(f, "recurring_schedule");
	g_object_set(row, "name", name, "timezone", timezone, "template", template, NULL);
	field(row, "kind", kind);
	field(row, "frequency", frequency);
	field(row, "start-at", start);
	save(f, row);
	return venture_entity_get_id(row);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) customer = NULL, vendor = NULL, year = NULL, period = NULL;
	g_autoptr(VentureQuery) query = NULL;
	(void)unused;
	f->config = venture_config_new();
	/* Payroll is opt-in; the calendar shows it only when it is on. */
	g_object_set(f->config, "payroll-enabled", TRUE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	customer = record(f, "company");
	g_object_set(customer, "name", "Acme & Sons", "email", "acme@example.test", NULL);
	save(f, customer); f->customer = venture_entity_get_id(customer);
	vendor = record(f, "company");
	g_object_set(vendor, "name", "Cloud Host", NULL);
	field(vendor, "kind", "supplier");
	save(f, vendor); f->vendor = venture_entity_get_id(vendor);
	year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "organization-id", f->org, "name", "2026", NULL);
	field(year, "start-at", "2026-01-01");
	save(f, year);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	f->period = venture_entity_get_id(period);
	f->invoice = invoice(f, "INV-1", "2026-01-10", "40 USD");
	f->bill = bill(f, "BILL-1", "2026-01-20", "USD", "100 USD");
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static GPtrArray *
events(Fixture *f, const gchar *from, const gchar *to, const gchar *today)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = day(from);
	g_autoptr(GDateTime) end = day(to);
	g_autoptr(GDateTime) as_of = day(today);
	g_autoptr(VentureDateRange) range = venture_date_range_new(start, end);
	GPtrArray *rows = venture_money_calendar_events(f->context, f->org, range, as_of, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	return rows;
}

/* The events of @kind, in order; NULL kind means all of them. */
static guint
count(GPtrArray *rows, const gchar *kind)
{
	guint i, n = 0;
	for (i = 0; i < rows->len; i++)
		if (kind == NULL || g_strcmp0(venture_money_calendar_event_get_kind(g_ptr_array_index(rows, i)), kind) == 0)
			n++;
	return n;
}

static VentureMoneyCalendarEvent *
find(GPtrArray *rows, const gchar *kind, const gchar *date)
{
	guint i;
	for (i = 0; i < rows->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(rows, i);
		if (g_strcmp0(venture_money_calendar_event_get_kind(event), kind) == 0 &&
			g_strcmp0(venture_money_calendar_event_get_day(event), date) == 0)
			return event;
	}
	return NULL;
}

static void
assert_amount(VentureMoneyCalendarEvent *event, const gchar *expected)
{
	g_autofree gchar *text = venture_money_to_string(venture_money_calendar_event_get_amount(event));
	g_assert_cmpstr(text, ==, expected);
}

static VentureReportResult *
run_report(Fixture *f, const gchar *period, JsonObject *options)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "money_calendar");
	VentureReportResult *result;
	g_assert_nonnull(report);
	range = venture_context_parse_period(f->context, period, &error);
	g_assert_no_error(error);
	result = venture_report_generate(report, f->context, range, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gchar *
cell(VentureReportResult *result, guint row, const gchar *key)
{
	return venture_report_result_format_cell(result, row, key);
}

static gint
row_for(VentureReportResult *result, const gchar *kind, const gchar *date)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		g_autofree gchar *k = cell(result, i, "kind");
		g_autofree gchar *d = cell(result, i, "date");
		if (g_strcmp0(k, kind) == 0 && g_strcmp0(d, date) == 0)
			return (gint)i;
	}
	return -1;
}

/* --- Recurring schedules ---------------------------------------------------- */

/* A monthly schedule anchored on the 31st lands on the last day of shorter
 * months and returns to the 31st afterwards; a daily one appears each day. */
static void
test_recurring_month_end(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) rows = NULL;
	VentureMoneyCalendarEvent *event;
	(void)unused;
	schedule(f, "Hosting", "expense", "monthly", "UTC", "2026-01-31",
		"{\"description\":\"Hosting\",\"amount\":\"12.50 EUR\"}");
	rows = events(f, "2026-01-01", "2026-05-01", "2026-01-01");
	g_assert_cmpuint(count(rows, "recurring"), ==, 4);
	g_assert_nonnull(find(rows, "recurring", "2026-01-31"));
	g_assert_nonnull(find(rows, "recurring", "2026-02-28"));
	g_assert_nonnull(find(rows, "recurring", "2026-03-31"));
	g_assert_nonnull(find(rows, "recurring", "2026-04-30"));
	event = find(rows, "recurring", "2026-02-28");
	assert_amount(event, "12.50 EUR");
	g_assert_cmpstr(venture_money_calendar_event_get_direction(event), ==, "out");
	g_assert_cmpstr(venture_money_calendar_event_get_record_type(event), ==, "recurring_schedule");
	g_assert_true(venture_money_calendar_event_get_counts_in_net(event));
}

/* Calendar days come from the schedule's own timezone. 23:30 on 1 September
 * in Auckland is 11:30Z; adding a month in UTC crosses the NZ daylight-saving
 * change and lands on 2 October local time. The schedule says the 1st. */
static void
test_recurring_dst(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) rows = NULL;
	VentureMoneyCalendarEvent *event;
	(void)unused;
	schedule(f, "Retainer", "invoice", "monthly", "Pacific/Auckland", "2026-09-01T11:30:00Z",
		"{\"company_id\":1,\"lines\":[{\"description\":\"Retainer\",\"quantity\":\"2\",\"unit_price\":\"30 NZD\"}]}");
	rows = events(f, "2026-09-01", "2026-11-01", "2026-09-01");
	g_assert_cmpuint(count(rows, "recurring"), ==, 2);
	g_assert_nonnull(find(rows, "recurring", "2026-09-01"));
	g_assert_null(find(rows, "recurring", "2026-10-02"));
	event = find(rows, "recurring", "2026-10-01");
	g_assert_nonnull(event);
	assert_amount(event, "60.00 NZD");
	g_assert_cmpstr(venture_money_calendar_event_get_direction(event), ==, "in");
}

/* A paused schedule, one past its end and one whose occurrence was already
 * generated do not project; journals have no direction and are not money.
 * The generated occurrence is written by the recurring service, the only
 * writer of occurrences. */
static void
test_recurring_exclusions(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) paused = NULL;
	g_autoptr(VentureEntity) ended = NULL;
	g_autoptr(GDateTime) run_day = day("2026-01-08");
	(void)unused;
	schedule(f, "Generated", "expense", "monthly", "UTC", "2026-01-08",
		"{\"description\":\"Coffee\",\"amount\":\"1 USD\",\"vendor\":\"Cafe\"}");
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db), f->org, run_day, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	paused = venture_database_get(f->db, VENTURE_TYPE_RECURRING_SCHEDULE,
		schedule(f, "Paused", "expense", "monthly", "UTC", "2026-01-05", "{\"amount\":\"1 USD\"}"), NULL);
	g_object_set(paused, "paused", TRUE, NULL);
	save(f, paused);
	ended = venture_database_get(f->db, VENTURE_TYPE_RECURRING_SCHEDULE,
		schedule(f, "Ended", "expense", "monthly", "UTC", "2026-01-06", "{\"amount\":\"1 USD\"}"), NULL);
	field(ended, "end-at", "2026-01-31");
	save(f, ended);
	schedule(f, "Journal", "journal", "monthly", "UTC", "2026-01-07", "{\"lines\":[]}");
	rows = events(f, "2026-01-01", "2026-03-01", "2026-01-01");
	g_assert_null(find(rows, "recurring", "2026-01-05"));
	g_assert_null(find(rows, "recurring", "2026-02-05"));
	g_assert_nonnull(find(rows, "recurring", "2026-01-06"));
	g_assert_null(find(rows, "recurring", "2026-02-06"));
	g_assert_null(find(rows, "recurring", "2026-01-07"));
	g_assert_null(find(rows, "recurring", "2026-01-08"));
	g_assert_nonnull(find(rows, "recurring", "2026-02-08"));
}

/* --- Bills and invoices ------------------------------------------------------ */

/* An approved unpaid bill sits on its due date; an issued unpaid invoice on
 * its. Both carry the open balance, not the face value. */
static void
test_bills_and_invoices(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) rows = NULL;
	VentureMoneyCalendarEvent *event;
	(void)unused;
	rows = events(f, "2026-01-01", "2026-02-01", "2026-01-01");
	event = find(rows, "bill", "2026-01-20");
	g_assert_nonnull(event);
	assert_amount(event, "100.00 USD");
	g_assert_cmpstr(venture_money_calendar_event_get_direction(event), ==, "out");
	g_assert_cmpstr(venture_money_calendar_event_get_record_type(event), ==, "vendor_bill");
	g_assert_cmpint(venture_money_calendar_event_get_record_id(event), ==, f->bill);
	g_assert_cmpstr(venture_money_calendar_event_get_counterparty(event), ==, "Cloud Host");
	g_assert_false(venture_money_calendar_event_get_overdue(event));
	event = find(rows, "invoice", "2026-01-10");
	g_assert_nonnull(event);
	assert_amount(event, "40.00 USD");
	g_assert_cmpstr(venture_money_calendar_event_get_direction(event), ==, "in");
	g_assert_cmpint(venture_money_calendar_event_get_record_id(event), ==, f->invoice);
	g_assert_cmpstr(venture_money_calendar_event_get_counterparty(event), ==, "Acme & Sons");
}

/* Past due and still open, a bill or invoice is carried to today and flagged;
 * its own due date shows nothing. */
static void
test_overdue_carried(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) rows = NULL;
	VentureMoneyCalendarEvent *event;
	(void)unused;
	rows = events(f, "2026-02-01", "2026-03-01", "2026-02-10");
	g_assert_null(find(rows, "bill", "2026-01-20"));
	event = find(rows, "bill", "2026-02-10");
	g_assert_nonnull(event);
	g_assert_true(venture_money_calendar_event_get_overdue(event));
	event = find(rows, "invoice", "2026-02-10");
	g_assert_nonnull(event);
	g_assert_true(venture_money_calendar_event_get_overdue(event));
	g_assert_cmpstr(venture_money_calendar_event_get_anchor_day(event), ==, "2026-01-10");
}

/* Drafts, paid and void documents are not money due. */
static void
test_settled_excluded(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) draft = record(f, "vendor_bill");
	g_autoptr(GDateTime) paid_on = day("2026-01-05");
	(void)unused;
	g_object_set(draft, "number", "DRAFT", "company-id", f->vendor, "currency", "USD", "status", "draft", NULL);
	field(draft, "bill-date", "2026-01-01"); field(draft, "due-date", "2026-01-25");
	save(f, draft);
	g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db), f->invoice, paid_on, NULL, &error));
	g_assert_no_error(error);
	rows = events(f, "2026-01-01", "2026-02-01", "2026-01-01");
	g_assert_null(find(rows, "bill", "2026-01-25"));
	g_assert_null(find(rows, "invoice", "2026-01-10"));
	g_assert_cmpuint(count(rows, "invoice"), ==, 0);
}

/* --- Dunning, payroll, tax ----------------------------------------------------- */

/* Each policy step that has not yet been recorded is a dated event on due +
 * offset; a recorded step is not repeated. Reminders are not cash. */
static void
test_dunning_steps(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) template = record(f, "mail_template");
	g_autoptr(VentureEntity) policy = record(f, "dunning_policy");
	g_autoptr(GDateTime) sweep_day = day("2026-01-07");
	g_autofree gchar *steps = NULL;
	VentureMoneyCalendarEvent *event;
	(void)unused;
	g_object_set(template, "name", "Reminder", "subject", "{number}", "text-body", "{number}", "html-body", "<p>{number}</p>", NULL);
	save(f, template);
	steps = g_strdup_printf("[{\"offset\":-3,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":7,\"template_id\":%" G_GINT64_FORMAT "}]",
		venture_entity_get_id(template), venture_entity_get_id(template));
	g_object_set(policy, "name", "Standard", "steps", steps, "is-default", TRUE, NULL);
	field(policy, "adopted-at", "2026-01-01");
	save(f, policy);
	rows = events(f, "2026-01-01", "2026-02-01", "2026-01-01");
	g_assert_cmpuint(count(rows, "dunning"), ==, 2);
	event = find(rows, "dunning", "2026-01-07");
	g_assert_nonnull(event);
	g_assert_false(venture_money_calendar_event_get_counts_in_net(event));
	g_assert_cmpstr(venture_money_calendar_event_get_record_type(event), ==, "invoice");
	g_assert_nonnull(find(rows, "dunning", "2026-01-17"));
	g_clear_pointer(&rows, g_ptr_array_unref);
	g_assert_cmpint(venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, sweep_day, 100, NULL, &error), ==, 1);
	g_assert_no_error(error);
	rows = events(f, "2026-01-01", "2026-02-01", "2026-01-08");
	g_assert_cmpuint(count(rows, "dunning"), ==, 1);
	g_assert_null(find(rows, "dunning", "2026-01-07"));
	g_assert_nonnull(find(rows, "dunning", "2026-01-17"));
}

/* An imported, undisbursed pay run is money out on its period end: net pay
 * and the liabilities as two events. Disbursing removes them. */
static void
test_payroll(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonNode) payload = venture_json_parse(
		"{\"run_key\":\"2026-01\",\"period_start\":\"2026-01-01T00:00:00Z\",\"period_end\":\"2026-01-31T00:00:00Z\","
		"\"currency\":\"USD\",\"lines\":[{\"employee\":\"Ada\",\"gross\":\"5000.00 USD\",\"employer_cost\":\"400.00 USD\","
		"\"deductions\":\"1000.00 USD\",\"net\":\"4000.00 USD\",\"liabilities\":\"1400.00 USD\"}]}", NULL);
	g_autoptr(VentureEntity) run = NULL;
	VentureMoneyCalendarEvent *net, *tax;
	guint i;
	(void)unused;
	run = venture_payroll_service_import_json(venture_payroll_service_get(f->db), f->org, json_node_get_object(payload), NULL, &error);
	g_assert_no_error(error);
	rows = events(f, "2026-01-01", "2026-02-01", "2026-01-01");
	g_assert_cmpuint(count(rows, "payroll"), ==, 2);
	net = NULL; tax = NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(rows, i);
		if (g_strcmp0(venture_money_calendar_event_get_kind(event), "payroll") != 0)
			continue;
		g_assert_cmpstr(venture_money_calendar_event_get_day(event), ==, "2026-01-31");
		g_assert_cmpstr(venture_money_calendar_event_get_direction(event), ==, "out");
		g_assert_cmpstr(venture_money_calendar_event_get_record_type(event), ==, "payroll_run");
		if (strstr(venture_money_calendar_event_get_title(event), "liabilities"))
			tax = event;
		else
			net = event;
	}
	g_assert_nonnull(net); g_assert_nonnull(tax);
	assert_amount(net, "4000.00 USD");
	assert_amount(tax, "1400.00 USD");
}

/* A draft or reviewed tax pack is money out on its period end for the tax it
 * carries; a submitted one has left the calendar. */
static void
test_tax(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	FakeAdapter *fake = g_object_new(fake_adapter_get_type(), NULL);
	VentureMoneyCalendarEvent *event;
	VentureActor actor = { VENTURE_ACTOR_KIND_USER, "clerk", NULL, NULL, NULL };
	(void)unused;
	venture_tax_filing_adapter_registry_add(venture_tax_filing_service_get_adapters(venture_tax_filing_service_get(f->db)),
		VENTURE_TAX_FILING_ADAPTER(fake));
	filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db), f->org, "XX", "XX", f->period, NULL, NULL, &actor, &error);
	g_assert_no_error(error);
	rows = events(f, "2026-01-01", "2026-03-01", "2026-01-01");
	g_assert_cmpuint(count(rows, "tax"), ==, 1);
	event = find(rows, "tax", "2026-02-01");
	g_assert_nonnull(event);
	assert_amount(event, "75.00 USD");
	g_assert_cmpstr(venture_money_calendar_event_get_direction(event), ==, "out");
	g_assert_cmpstr(venture_money_calendar_event_get_record_type(event), ==, "tax_filing");
	g_clear_pointer(&rows, g_ptr_array_unref);
	g_assert_true(venture_tax_filing_service_review(venture_tax_filing_service_get(f->db), filing, &actor, &error));
	g_assert_no_error(error);
	g_assert_true(venture_tax_filing_service_submit(venture_tax_filing_service_get(f->db), filing, &actor, &error));
	g_assert_no_error(error);
	rows = events(f, "2026-01-01", "2026-03-01", "2026-01-01");
	g_assert_cmpuint(count(rows, "tax"), ==, 0);
}

/* --- The report ----------------------------------------------------------------- */

/* Rows carry kind, amount, direction and the record; per-day and per-ISO-week
 * nets are computed per currency and never folded across them. */
static void
test_report(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	gint row;
	(void)unused;
	bill(f, "BILL-EUR", "2026-01-10", "EUR", "30 EUR");
	invoice(f, "INV-2", "2026-01-12", "10 USD");
	json_object_set_string_member(options, "as_of", "2026-01-01");
	result = run_report(f, "2026-01", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 4);
	row = row_for(result, "invoice", "2026-01-10");
	g_assert_cmpint(row, >=, 0);
	{
		g_autofree gchar *amount = cell(result, row, "amount");
		g_autofree gchar *direction = cell(result, row, "direction");
		g_autofree gchar *record = cell(result, row, "record");
		g_autofree gchar *week = cell(result, row, "week");
		g_autofree gchar *day_net = cell(result, row, "day_net");
		g_autofree gchar *week_net = cell(result, row, "week_net");
		g_assert_nonnull(strstr(amount, "40.00"));
		g_assert_cmpstr(direction, ==, "in");
		g_assert_true(g_str_has_prefix(record, "invoice:"));
		g_assert_cmpstr(week, ==, "2026-W02");
		/* 10 Jan: +40 USD in and a 30 EUR bill out, shown side by side;
		 * never the folded 10.00 that adding them would give. */
		g_assert_nonnull(strstr(day_net, "40.00 USD"));
		g_assert_nonnull(strstr(day_net, "-30.00 EUR"));
		g_assert_null(strstr(day_net, "10.00"));
		/* ISO week 2 (5-11 Jan) in USD: +40. Week 3 holds the 10 USD invoice and the 100 USD bill. */
		g_assert_nonnull(strstr(week_net, "40.00"));
	}
	row = row_for(result, "bill", "2026-01-10");
	{
		g_autofree gchar *day_net = cell(result, row, "day_net");
		g_assert_nonnull(strstr(day_net, "-30.00 EUR"));
		g_assert_nonnull(strstr(day_net, "40.00 USD"));
	}
	row = row_for(result, "bill", "2026-01-20");
	{
		g_autofree gchar *week_net = cell(result, row, "week_net");
		g_autofree gchar *week = cell(result, row, "week");
		g_assert_cmpstr(week, ==, "2026-W04");
		g_assert_nonnull(strstr(week_net, "-100.00"));
	}
	row = row_for(result, "invoice", "2026-01-12");
	{
		g_autofree gchar *week_net = cell(result, row, "week_net");
		g_assert_nonnull(strstr(week_net, "10.00"));
		g_assert_null(strstr(week_net, "-90.00"));
	}
}

/* from/to and kind options narrow the report; an unknown kind is refused. */
static void
test_report_options(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonObject) bad = json_object_new();
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	(void)unused;
	json_object_set_string_member(options, "as_of", "2026-01-01");
	json_object_set_string_member(options, "from", "2026-01-15");
	json_object_set_string_member(options, "to", "2026-01-25");
	json_object_set_string_member(options, "kind", "bill");
	result = run_report(f, "2026", options);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_cmpint(row_for(result, "bill", "2026-01-20"), ==, 0);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "money_calendar");
	range = venture_context_parse_period(f->context, "2026-01", NULL);
	json_object_set_string_member(bad, "kind", "lottery");
	g_assert_null(venture_report_generate(report, f->context, range, bad, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* The module off hides the report and yields no events; sources whose module
 * is off contribute nothing while the rest stay. */
static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = day("2026-01-01");
	g_autoptr(GDateTime) end = day("2026-02-01");
	g_autoptr(VentureDateRange) range = venture_date_range_new(start, end);
	(void)unused;
	schedule(f, "Hosting", "expense", "monthly", "UTC", "2026-01-15", "{\"amount\":\"1 USD\"}");
	venture_config_set_module_enabled(f->config, "recurring", FALSE);
	rows = events(f, "2026-01-01", "2026-02-01", "2026-01-01");
	g_assert_cmpuint(count(rows, "recurring"), ==, 0);
	g_assert_cmpuint(count(rows, "bill"), ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);
	venture_config_set_module_enabled(f->config, "recurring", TRUE);
	venture_config_set_module_enabled(f->config, "money_calendar", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "money_calendar"));
	g_assert_null(venture_money_calendar_events(f->context, f->org, range, start, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- .ics -------------------------------------------------------------------------- */

/* All-day VEVENTs with a UID that survives the overdue carry. */
static void
test_ics(Fixture *f, gconstpointer unused)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) later = NULL;
	g_autofree gchar *ics = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *uid = NULL;
	(void)unused;
	rows = events(f, "2026-01-01", "2026-02-01", "2026-01-01");
	ics = venture_money_calendar_ics(rows);
	g_assert_true(g_str_has_prefix(ics, "BEGIN:VCALENDAR\r\n"));
	g_assert_nonnull(strstr(ics, "DTSTART;VALUE=DATE:20260120\r\n"));
	g_assert_nonnull(strstr(ics, "DTEND;VALUE=DATE:20260121\r\n"));
	g_assert_nonnull(strstr(ics, "SUMMARY:Bill BILL-1 Cloud Host"));
	g_assert_nonnull(strstr(ics, "END:VCALENDAR\r\n"));
	uid = g_strdup_printf("UID:money-bill-vendor_bill-%" G_GINT64_FORMAT "-2026-01-20@venture\r\n", f->bill);
	g_assert_nonnull(strstr(ics, uid));
	later = events(f, "2026-02-01", "2026-03-01", "2026-02-10");
	again = venture_money_calendar_ics(later);
	g_assert_nonnull(strstr(again, uid));
	g_assert_nonnull(strstr(again, "DTSTART;VALUE=DATE:20260210\r\n"));
	g_assert_nonnull(strstr(again, "overdue"));
}

/* --- HTTP and CLI ------------------------------------------------------------------ */

typedef struct { gboolean done; GError *error; GBytes *bytes; gchar *out, *err; } Result;
typedef struct { Fixture base; VentureWebServer *server; gchar *directory; } ServerFixture;

static void
server_setup(ServerFixture *s, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	s->directory = g_dir_make_tmp("venture-money-calendar-XXXXXX", &error);
	g_assert_no_error(error);
	setup(&s->base, unused);
	g_object_set(s->base.config, "state-dir", s->directory, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	s->server = venture_web_server_new(s->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
}

static void
server_teardown(ServerFixture *s, gconstpointer unused)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->server);
	teardown(&s->base, unused);
	venture_test_remove_tree(s->directory);
	g_free(s->directory);
}

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}

static guint
request(ServerFixture *s, const gchar *path, gchar **out, gchar **content_type)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(s->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Result result;
	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out)
		*out = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	if (content_type)
		*content_type = g_strdup(soup_message_headers_get_content_type(soup_message_get_response_headers(message), NULL));
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}

static gboolean
cli_timeout(gpointer process)
{
	g_subprocess_force_exit(process);
	return G_SOURCE_CONTINUE;
}

static gchar *
cli(ServerFixture *s, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Result result;
	guint i, timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(s->server)));
	for (i = 0; args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "calendar-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	if (g_subprocess_get_successful(process) != expect_success)
		g_test_message("CLI: %s%s", result.out, result.err);
	g_assert_true(g_subprocess_get_successful(process) == expect_success);
	g_free(result.err);
	return result.out;
}

/* Month, week and agenda views render server-side with the events linked to
 * their records; filters narrow them; the module off hides the page. */
static void
test_web_page(ServerFixture *s, gconstpointer unused)
{
	g_autofree gchar *month = NULL, *week = NULL, *agenda = NULL, *filtered = NULL, *off = NULL, *link = NULL;
	(void)unused;
	g_assert_cmpuint(request(s, "/money/calendar?period=2026-01&as_of=2026-01-01", &month, NULL), ==, 200);
	g_assert_nonnull(strstr(month, "money-calendar"));
	g_assert_nonnull(strstr(month, "class=\"cal-month\""));
	link = g_strdup_printf("href=\"/e/vendor_bill/%" G_GINT64_FORMAT "\"", s->base.bill);
	g_assert_nonnull(strstr(month, link));
	g_assert_nonnull(strstr(month, "INV-1"));
	g_assert_null(strstr(month, "draggable"));
	g_assert_cmpuint(request(s, "/money/calendar?view=week&period=2026-01-05..2026-01-11&as_of=2026-01-01", &week, NULL), ==, 200);
	g_assert_nonnull(strstr(week, "class=\"cal-week\""));
	g_assert_nonnull(strstr(week, "INV-1"));
	g_assert_null(strstr(week, "BILL-1"));
	g_assert_cmpuint(request(s, "/money/calendar?view=agenda&period=2026-01&as_of=2026-01-01", &agenda, NULL), ==, 200);
	g_assert_nonnull(strstr(agenda, "class=\"cal-agenda\""));
	g_assert_nonnull(strstr(agenda, "BILL-1"));
	g_assert_nonnull(strstr(agenda, "2026-W02"));
	g_assert_nonnull(strstr(agenda, "2026-W04"));
	g_assert_null(strstr(agenda, "2026-W03"));
	g_assert_cmpuint(request(s, "/money/calendar?view=agenda&period=2026-01&as_of=2026-01-01&kind=invoice", &filtered, NULL), ==, 200);
	g_assert_nonnull(strstr(filtered, "INV-1"));
	g_assert_null(strstr(filtered, "BILL-1"));
	g_clear_pointer(&filtered, g_free);
	{
		g_autofree gchar *path = g_strdup_printf("/money/calendar?view=agenda&period=2026-01&as_of=2026-01-01&company_id=%" G_GINT64_FORMAT, s->base.vendor);
		g_assert_cmpuint(request(s, path, &filtered, NULL), ==, 200);
		g_assert_nonnull(strstr(filtered, "BILL-1"));
		g_assert_null(strstr(filtered, "INV-1"));
	}
	g_clear_pointer(&filtered, g_free);
	g_assert_cmpuint(request(s, "/money/calendar?view=agenda&period=2026-01&as_of=2026-01-01&currency=EUR", &filtered, NULL), ==, 200);
	g_assert_null(strstr(filtered, "BILL-1"));
	g_assert_null(strstr(filtered, "INV-1"));
	venture_config_set_module_enabled(s->base.config, "money_calendar", FALSE);
	g_assert_cmpuint(request(s, "/money/calendar?period=2026-01", &off, NULL), ==, 404);
	g_assert_cmpuint(request(s, "/money/calendar.ics?period=2026-01", NULL, NULL), ==, 404);
	venture_config_set_module_enabled(s->base.config, "money_calendar", TRUE);
}

/* The feed is text/calendar with the same events as the page. */
static void
test_web_ics(ServerFixture *s, gconstpointer unused)
{
	g_autofree gchar *body = NULL, *type = NULL;
	(void)unused;
	g_assert_cmpuint(request(s, "/money/calendar.ics?period=2026-01&as_of=2026-01-01", &body, &type), ==, 200);
	g_assert_cmpstr(type, ==, "text/calendar");
	g_assert_nonnull(strstr(body, "BEGIN:VEVENT\r\n"));
	g_assert_nonnull(strstr(body, "SUMMARY:Invoice INV-1 Acme & Sons"));
	g_assert_nonnull(strstr(body, "DTSTART;VALUE=DATE:20260110\r\n"));
	g_assert_nonnull(strstr(body, "BILL-1"));
}

/* venturectl money calendar prints the agenda through the report API. */
static void
test_cli(ServerFixture *s, gconstpointer unused)
{
	const gchar *agenda_args[] = { "money", "calendar", "--from", "2026-01-01", "--to", "2026-01-31", "--as-of", "2026-01-01", NULL };
	const gchar *kind_args[] = { "money", "calendar", "--from", "2026-01-01", "--to", "2026-01-31", "--as-of", "2026-01-01", "--kind", "bill", NULL };
	const gchar *json_args[] = { "-f", "json", "money", "calendar", "--from", "2026-01-01", "--to", "2026-01-31", "--as-of", "2026-01-01", NULL };
	const gchar *bad_args[] = { "money", "calendar", NULL };
	const gchar *bad_verb[] = { "money", "burn", "--from", "2026-01-01", "--to", "2026-01-31", NULL };
	g_autofree gchar *out = NULL, *kind = NULL, *json = NULL, *bad = NULL, *verb = NULL;
	(void)unused;
	out = cli(s, agenda_args, TRUE);
	g_assert_nonnull(strstr(out, "2026-01-10"));
	g_assert_nonnull(strstr(out, "INV-1"));
	g_assert_nonnull(strstr(out, "BILL-1"));
	g_assert_nonnull(strstr(out, "net"));
	kind = cli(s, kind_args, TRUE);
	g_assert_nonnull(strstr(kind, "BILL-1"));
	g_assert_null(strstr(kind, "INV-1"));
	json = cli(s, json_args, TRUE);
	g_assert_nonnull(strstr(json, "\"money_calendar\"") ? strstr(json, "\"money_calendar\"") : strstr(json, "\"rows\""));
	bad = cli(s, bad_args, FALSE);
	verb = cli(s, bad_verb, FALSE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/money-calendar/recurring/month-end", Fixture, NULL, setup, test_recurring_month_end, teardown);
	g_test_add("/money-calendar/recurring/dst", Fixture, NULL, setup, test_recurring_dst, teardown);
	g_test_add("/money-calendar/recurring/exclusions", Fixture, NULL, setup, test_recurring_exclusions, teardown);
	g_test_add("/money-calendar/bills-and-invoices", Fixture, NULL, setup, test_bills_and_invoices, teardown);
	g_test_add("/money-calendar/overdue-carried", Fixture, NULL, setup, test_overdue_carried, teardown);
	g_test_add("/money-calendar/settled-excluded", Fixture, NULL, setup, test_settled_excluded, teardown);
	g_test_add("/money-calendar/dunning-steps", Fixture, NULL, setup, test_dunning_steps, teardown);
	g_test_add("/money-calendar/payroll", Fixture, NULL, setup, test_payroll, teardown);
	g_test_add("/money-calendar/tax", Fixture, NULL, setup, test_tax, teardown);
	g_test_add("/money-calendar/report", Fixture, NULL, setup, test_report, teardown);
	g_test_add("/money-calendar/report-options", Fixture, NULL, setup, test_report_options, teardown);
	g_test_add("/money-calendar/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/money-calendar/ics", Fixture, NULL, setup, test_ics, teardown);
	g_test_add("/money-calendar/web/page", ServerFixture, NULL, server_setup, test_web_page, server_teardown);
	g_test_add("/money-calendar/web/ics", ServerFixture, NULL, server_setup, test_web_ics, server_teardown);
	g_test_add("/money-calendar/cli", ServerFixture, NULL, server_setup, test_cli, server_teardown);
	return g_test_run();
}
