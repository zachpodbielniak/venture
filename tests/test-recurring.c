/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
	gint64 vendor;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static void
field(VentureEntity *e, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(e, name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *e = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(e);
	venture_entity_set_organization_id(e, f->org);
	return e;
}

static gint64
count(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static gint64
account(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	company = record(f, "company");
	g_object_set(company, "name", "Acme", "email", "billing@acme.test", NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
	vendor = record(f, "company");
	g_object_set(vendor, "name", "Supplier", "email", "ap@supplier.test", NULL);
	field(vendor, "kind", "supplier");
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
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
test_catalog(void)
{
	static const gchar *const names[] = {
		"recurring_schedule", "recurring_occurrence", "collection_policy",
		"collection_step", "collection_case", "collection_notice", "financial_batch"
	};
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}

static gchar *
invoice_template(Fixture *f)
{
	return g_strdup_printf(
		"{\"company_id\":%" G_GINT64_FORMAT ",\"lines\":[{\"description\":\"Hosting\",\"quantity\":1,\"unit_price\":\"40 USD\"}]}",
		f->company);
}

static VentureEntity *
monthly_invoice(Fixture *f, const gchar *start)
{
	VentureEntity *schedule = record(f, "recurring_schedule");
	g_autofree gchar *template = invoice_template(f);
	g_object_set(schedule, "name", "Hosting", "timezone", "UTC", "auto-post", TRUE,
		"template", template, NULL);
	field(schedule, "kind", "invoice");
	field(schedule, "frequency", "monthly");
	field(schedule, "start-at", start);
	save(f, schedule);
	return schedule;
}

/* Each due month issues through settlement exactly once. */
static void
test_invoice_idempotent(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) schedule = monthly_invoice(f, "2026-01-15");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = venture_time_from_string("2026-03-15", NULL);
	gint created;
	(void)unused;
	created = venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, as_of, FALSE, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(created, ==, 3);
	g_assert_cmpint(count(f, "invoice"), ==, 3);
	g_assert_cmpint(count(f, "recurring_occurrence"), ==, 3);
	created = venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, as_of, FALSE, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(created, ==, 0);
	g_assert_cmpint(count(f, "invoice"), ==, 3);
}

/* A closed period is recorded and skipped; later open months still generate. */
static void
test_skip_closed_period(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) year = NULL;
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(GPtrArray) skipped = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GDateTime) as_of = venture_time_from_string("2026-02-15", NULL);
	VentureActor actor;
	(void)unused;
	year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "name", "FY2026",
		"organization-id", f->org, "start-at", start, "period-length", 0, NULL);
	save(f, year);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	periods = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(periods->len, >=, 2);
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "closer";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_object_set(g_ptr_array_index(periods, 0), "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->db, g_ptr_array_index(periods, 0), &actor, &error));
	g_assert_no_error(error);
	schedule = monthly_invoice(f, "2026-01-15");
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, as_of, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count(f, "invoice"), ==, 1);
	g_clear_object(&query);
	query = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "recurring_occurrence"));
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, 1, NULL);
	skipped = venture_database_find(f->db, query, NULL);
	g_assert_cmpuint(skipped->len, ==, 1);
}

/* January 31 retains its day after February. */
static void
test_month_end_and_leap(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) schedule = monthly_invoice(f, "2024-01-31");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = venture_time_from_string("2024-03-31", NULL);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GDateTime) issued = NULL;
	g_autofree gchar *day = NULL;
	(void)unused;
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, as_of, FALSE, NULL, &error), ==, 3);
	g_assert_no_error(error);
	query = venture_query_new(VENTURE_TYPE_INVOICE);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "issued-at", VENTURE_SORT_ASCENDING, NULL);
	invoices = venture_database_find(f->db, query, NULL);
	g_assert_cmpuint(invoices->len, ==, 3);
	g_object_get(g_ptr_array_index(invoices, 1), "issued-at", &issued, NULL);
	day = g_date_time_format(issued, "%F");
	g_assert_cmpstr(day, ==, "2024-02-29");
	g_clear_pointer(&day, g_free);
	g_clear_pointer(&issued, g_date_time_unref);
	g_object_get(g_ptr_array_index(invoices, 2), "issued-at", &issued, NULL);
	day = g_date_time_format(issued, "%F");
	g_assert_cmpstr(day, ==, "2024-03-31");
}

static void
test_pause_resume(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) schedule = monthly_invoice(f, "2026-01-01");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = venture_time_from_string("2026-01-01", NULL);
	(void)unused;
	g_assert_true(venture_recurring_service_pause(venture_recurring_service_get(f->db), schedule, NULL, &error));
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, as_of, FALSE, NULL, &error), ==, 0);
	g_assert_true(venture_recurring_service_resume(venture_recurring_service_get(f->db), schedule, NULL, &error));
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, as_of, FALSE, NULL, &error), ==, 1);
}

/* Bills go through payables, journals through posting, never raw invoice journals. */
static void
test_bill_expense_journal(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) bill = record(f, "recurring_schedule");
	g_autoptr(VentureEntity) expense = record(f, "recurring_schedule");
	g_autoptr(VentureEntity) journal = record(f, "recurring_schedule");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = venture_time_from_string("2026-01-01", NULL);
	g_autofree gchar *bill_template = NULL;
	g_autofree gchar *journal_template = NULL;
	(void)unused;
	bill_template = g_strdup_printf(
		"{\"company_id\":%" G_GINT64_FORMAT ",\"currency\":\"USD\",\"lines\":[{\"description\":\"Rent\",\"quantity\":\"1\",\"unit_price\":\"100 USD\"}]}",
		f->vendor);
	g_object_set(bill, "name", "Rent", "auto-post", TRUE, "template", bill_template, NULL);
	field(bill, "kind", "bill");
	field(bill, "start-at", "2026-01-01");
	save(f, bill);
	g_object_set(expense, "name", "Coffee", "auto-post", TRUE,
		"template", "{\"description\":\"Coffee\",\"amount\":\"4.50 USD\",\"vendor\":\"Cafe\"}", NULL);
	field(expense, "kind", "expense");
	field(expense, "start-at", "2026-01-01");
	save(f, expense);
	journal_template = g_strdup_printf(
		"{\"memo\":\"Payroll\",\"currency\":\"USD\",\"lines\":[{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"debit\",\"amount\":\"10 USD\"},{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"credit\",\"amount\":\"10 USD\"}]}",
		account(f, "4000"), account(f, "1000"));
	g_object_set(journal, "name", "Payroll", "auto-post", TRUE, "template", journal_template, NULL);
	field(journal, "kind", "journal");
	field(journal, "start-at", "2026-01-01");
	save(f, journal);
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, as_of, FALSE, NULL, &error), ==, 3);
	g_assert_no_error(error);
	g_assert_cmpint(count(f, "vendor_bill"), ==, 1);
	g_assert_cmpint(count(f, "expense"), ==, 1);
	g_assert_cmpint(count(f, "journal"), >=, 1);
}

static void
test_template_change_is_prospective(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) schedule = monthly_invoice(f, "2026-01-01");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) first = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(GDateTime) second = venture_time_from_string("2026-02-01", NULL);
	g_autofree gchar *updated = NULL;
	(void)unused;
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, first, FALSE, NULL, &error), ==, 1);
	g_clear_object(&schedule);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_RECURRING_SCHEDULE);
		venture_query_set_organization(query, f->org);
		schedule = venture_database_find_one(f->db, query, NULL);
	}
	updated = g_strdup_printf(
		"{\"company_id\":%" G_GINT64_FORMAT ",\"lines\":[{\"description\":\"Hosting plus\",\"quantity\":1,\"unit_price\":\"50 USD\"}]}",
		f->company);
	g_object_set(schedule, "template", updated, NULL);
	save(f, schedule);
	g_assert_cmpint(venture_recurring_service_run(venture_recurring_service_get(f->db),
		f->org, second, FALSE, NULL, &error), ==, 1);
	g_assert_cmpint(count(f, "invoice"), ==, 2);
	g_assert_cmpint(count(f, "invoice_line"), ==, 2);
}

static void
test_actions_registered(Fixture *f, gconstpointer unused)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(f->db);
	(void)unused;
	g_assert_nonnull(venture_action_registry_lookup(registry, "recurring_schedule", "pause"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "recurring_schedule", "run"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "collection_policy", "run"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "financial_batch", "apply"));
	g_assert_nonnull(venture_action_registry_lookup(registry, "invoice", "batch_create"));
}

static void
issue_overdue(Fixture *f)
{
	g_autoptr(VentureEntity) invoice = record(f, "invoice");
	g_autoptr(VentureEntity) line = record(f, "invoice_line");
	g_autoptr(GError) error = NULL;
	g_object_set(invoice, "number", "INV-DUE", "company-id", f->company, NULL);
	field(invoice, "issued-at", "2026-01-01");
	field(invoice, "due-at", "2026-01-10");
	save(f, invoice);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work",
		"quantity", 1.0, NULL);
	field(line, "unit-price", "40 USD");
	save(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(invoice), "sent", venture_time_from_string("2026-01-01", NULL), NULL, &error));
	g_assert_no_error(error);
}

static void
test_collection_reminders(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) policy = record(f, "collection_policy");
	g_autoptr(VentureEntity) step = record(f, "collection_step");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = venture_time_from_string("2026-01-17", NULL);
	gint notices;
	(void)unused;
	g_object_set(policy, "name", "Standard", "active", TRUE, "statement-day", (gint64)1, NULL);
	save(f, policy);
	g_object_set(step, "policy-id", venture_entity_get_id(policy), "day-offset", (gint64)7,
		"active", TRUE, NULL);
	field(step, "action", "reminder");
	save(f, step);
	issue_overdue(f);
	notices = venture_collection_service_run(venture_collection_service_get(f->db),
		f->context, f->org, as_of, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(notices, >=, 1);
	g_assert_cmpint(count(f, "collection_notice"), >=, 1);
	g_assert_cmpint(count(f, "mail_message"), >=, 1);
	notices = venture_collection_service_run(venture_collection_service_get(f->db),
		f->context, f->org, as_of, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(notices, ==, 0);
}

static void
test_collection_stops_on_payment(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) policy = record(f, "collection_policy");
	g_autoptr(VentureEntity) step = record(f, "collection_step");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) as_of = venture_time_from_string("2026-01-17", NULL);
	(void)unused;
	g_object_set(policy, "name", "Paid", "active", TRUE, NULL);
	save(f, policy);
	g_object_set(step, "policy-id", venture_entity_get_id(policy), "day-offset", (gint64)0,
		"active", TRUE, NULL);
	field(step, "action", "reminder");
	save(f, step);
	issue_overdue(f);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
		g_autoptr(VentureEntity) invoice = NULL;
		venture_query_set_organization(query, f->org);
		venture_query_add_filter_string(query, "number", VENTURE_FILTER_OP_EQ, "INV-DUE", NULL);
		invoice = venture_database_find_one(f->db, query, NULL);
		g_assert_nonnull(invoice);
		g_assert_true(venture_settlement_service_settle_invoice(venture_settlement_service_get(f->db),
			venture_entity_get_id(invoice), as_of, NULL, &error));
	}
	g_assert_cmpint(venture_collection_service_run(venture_collection_service_get(f->db),
		f->context, f->org, as_of, NULL, &error), ==, 0);
}

static void
test_worklist_report(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	(void)unused;
	issue_overdue(f);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "collections_worklist");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_report_result_get_row_count(result), >=, 1);
}

static void
test_batch_invoices_atomic(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(JsonNode) result = NULL;
	(void)unused;
	json = g_strdup_printf(
		"{\"documents\":[{\"number\":\"B-1\",\"company_id\":%" G_GINT64_FORMAT ",\"issued_at\":\"2026-01-01\",\"lines\":[{\"description\":\"A\",\"quantity\":1,\"unit_price\":\"10 USD\"}]},{\"number\":\"B-2\",\"company_id\":%" G_GINT64_FORMAT ",\"issued_at\":\"2026-01-01\",\"lines\":[{\"description\":\"B\",\"quantity\":1,\"unit_price\":\"20 USD\"}]}]}",
		f->company, f->company);
	result = venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"invoice", "json", json, TRUE, FALSE, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(count(f, "invoice"), ==, 2);
}

static void
test_batch_all_or_nothing(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	(void)unused;
	json = g_strdup_printf(
		"{\"documents\":[{\"number\":\"C-1\",\"company_id\":%" G_GINT64_FORMAT ",\"issued_at\":\"2026-01-01\",\"lines\":[{\"description\":\"A\",\"quantity\":1,\"unit_price\":\"10 USD\"}]},{\"number\":\"C-2\",\"company_id\":0,\"issued_at\":\"2026-01-01\",\"lines\":[{\"description\":\"B\",\"quantity\":1,\"unit_price\":\"20 USD\"}]}]}",
		f->company);
	g_assert_null(venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"invoice", "json", json, TRUE, FALSE, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(count(f, "invoice"), ==, 0);
}

static void
test_batch_csv_expenses(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	const gchar *csv = "description,amount,occurred_at,vendor,external_id\n"
		"Coffee,4.50 USD,2026-01-01,Cafe,exp-1\n"
		"Paper,12 USD,2026-01-02,Office,exp-2\n";
	(void)unused;
	result = venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"expense", "csv", csv, FALSE, FALSE, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count(f, "expense"), ==, 2);
	g_assert_null(venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"expense", "csv", csv, FALSE, FALSE, NULL, &error));
	g_assert_nonnull(error);
	g_assert_cmpint(count(f, "expense"), ==, 2);
}

static void
test_batch_dry_run(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(JsonNode) result = NULL;
	(void)unused;
	json = g_strdup_printf(
		"{\"documents\":[{\"number\":\"D-1\",\"company_id\":%" G_GINT64_FORMAT ",\"issued_at\":\"2026-01-01\",\"lines\":[{\"description\":\"A\",\"quantity\":1,\"unit_price\":\"10 USD\"}]}]}",
		f->company);
	result = venture_recurring_service_batch(venture_recurring_service_get(f->db),
		"invoice", "json", json, TRUE, TRUE, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count(f, "invoice"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/recurring/catalog", test_catalog);
	g_test_add("/recurring/invoice_idempotent", Fixture, NULL, setup, test_invoice_idempotent, teardown);
	g_test_add("/recurring/skip_closed", Fixture, NULL, setup, test_skip_closed_period, teardown);
	g_test_add("/recurring/month_end_leap", Fixture, NULL, setup, test_month_end_and_leap, teardown);
	g_test_add("/recurring/pause_resume", Fixture, NULL, setup, test_pause_resume, teardown);
	g_test_add("/recurring/bill_expense_journal", Fixture, NULL, setup, test_bill_expense_journal, teardown);
	g_test_add("/recurring/template_prospective", Fixture, NULL, setup, test_template_change_is_prospective, teardown);
	g_test_add("/recurring/actions", Fixture, NULL, setup, test_actions_registered, teardown);
	g_test_add("/collections/reminders", Fixture, NULL, setup, test_collection_reminders, teardown);
	g_test_add("/collections/stop_on_payment", Fixture, NULL, setup, test_collection_stops_on_payment, teardown);
	g_test_add("/collections/worklist", Fixture, NULL, setup, test_worklist_report, teardown);
	g_test_add("/batch/invoices", Fixture, NULL, setup, test_batch_invoices_atomic, teardown);
	g_test_add("/batch/all_or_nothing", Fixture, NULL, setup, test_batch_all_or_nothing, teardown);
	g_test_add("/batch/csv_expenses", Fixture, NULL, setup, test_batch_csv_expenses, teardown);
	g_test_add("/batch/dry_run", Fixture, NULL, setup, test_batch_dry_run, teardown);
	return g_test_run();
}
