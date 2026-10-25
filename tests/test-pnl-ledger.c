/*
 * test-pnl-ledger.c - Profit and loss from the posted books.
 *
 * The operational P&L counts sale and expense records: a sale when it is
 * paid, an expense when one is recorded. A business whose books live in the
 * ledger -- invoices and bills, as a migrated history has -- needs the
 * accrual figure the income statement gives. basis=ledger asks for it, and
 * an organization's pnl-basis makes it the default for its reports and its
 * dashboard card.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *e = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(e);
	venture_entity_set_organization_id(e, f->org);
	return e;
}

static void
field(VentureEntity *e, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(e, name, value, &error));
	g_assert_no_error(error);
}

static gint64
account(Fixture *f, const gchar *code, const gchar *name, const gchar *kind)
{
	g_autoptr(VentureEntity) a = record(f, "account");
	g_object_set(a, "code", code, "name", name, "active", TRUE, NULL);
	field(a, "kind", kind);
	save(f, a);
	return venture_entity_get_id(a);
}

/* A journal moving @amount from @credit to @debit on @date. */
static void
journal(Fixture *f, const gchar *date, gint64 debit, gint64 credit, const gchar *amount)
{
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601(date, NULL);
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GError) error = NULL;
	VentureJournalLine *line;
	money = venture_money_from_string(amount, "USD", &error);
	g_assert_no_error(error);
	g_object_set(header, "organization-id", f->org, "occurred-at", when, "currency", "USD", "memo", "Test",
		"source-type", "organization", "source-id", f->org, NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", debit, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", credit, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), header, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 bank, services, hosting;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	bank = account(f, "1012", "Operating account", "asset");
	services = account(f, "4112", "Services", "income");
	hosting = account(f, "6422", "Hosting", "expense");
	/* An invoice-like income and a bill-like expense, posted, never paid. */
	journal(f, "2025-03-04T00:00:00Z", bank, services, "300.00 USD");
	journal(f, "2025-03-10T00:00:00Z", hosting, bank, "45.00 USD");
	/* Outside the period. */
	journal(f, "2025-04-02T00:00:00Z", bank, services, "999.00 USD");
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureReportResult *
pnl(Fixture *f, const gchar *basis)
{
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "pnl");
	g_assert_nonnull(report);
	period = venture_context_parse_period(f->context, "2025-03", &error);
	g_assert_no_error(error);
	json_object_set_int_member(options, "organization_id", f->org);
	if (basis != NULL)
		json_object_set_string_member(options, "basis", basis);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gint64
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(m), key) == 0)
			return venture_money_get_amount(venture_metric_get_money(m));
	}
	g_assert_not_reached();
	return 0;
}

/* The operational P&L is unchanged: no sale and no expense record. */
static void
test_records_default(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = pnl(f, NULL);
	(void)data;
	g_assert_cmpint(metric(result, "revenue"), ==, 0);
	g_assert_cmpint(metric(result, "expenses"), ==, 0);
}

/* basis=ledger reads the posted income and expense accounts. */
static void
test_ledger_basis(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = pnl(f, "ledger");
	(void)data;
	g_assert_cmpint(metric(result, "revenue"), ==, 30000);
	g_assert_cmpint(metric(result, "expenses"), ==, 4500);
	g_assert_cmpint(metric(result, "profit"), ==, 25500);
}

/* The organization's pnl-basis is the default; an explicit basis wins. */
static void
test_organization_default(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, NULL);
	g_autoptr(VentureReportResult) ledger = NULL;
	g_autoptr(VentureReportResult) records = NULL;
	(void)data;
	g_assert_nonnull(organization);
	g_object_set(organization, "pnl-basis", "ledger", NULL);
	save(f, organization);
	ledger = pnl(f, NULL);
	g_assert_cmpint(metric(ledger, "revenue"), ==, 30000);
	records = pnl(f, "records");
	g_assert_cmpint(metric(records, "revenue"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/pnl-ledger/records-default", Fixture, NULL, setup, test_records_default, teardown);
	g_test_add("/pnl-ledger/ledger-basis", Fixture, NULL, setup, test_ledger_basis, teardown);
	g_test_add("/pnl-ledger/organization-default", Fixture, NULL, setup, test_organization_default, teardown);
	return g_test_run();
}
