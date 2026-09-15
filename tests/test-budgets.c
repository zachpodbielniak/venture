/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "locale-timezone", "UTC", NULL);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->config);
	g_clear_object(&f->db);
}

static gint64
account(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, q, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

static void
save(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, entity, NULL, &error));
	g_assert_no_error(error);
}

static void
post(Fixture *f, const gchar *when, const gchar *debit, const gchar *credit, gint64 amount)
{
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) date = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(amount, "USD");
	g_autoptr(GError) error = NULL;
	VentureJournalLine *line;
	g_object_set(header, "organization-id", f->org, "source-type", "organization",
		"source-id", f->org, "occurred-at", date, "currency", "USD", NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account(f, debit), "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account(f, credit), "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), header, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

static gint64
cell(VentureReportResult *r, const gchar *key, const gchar *column)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
	{
		const GValue *name = venture_report_result_get_cell(r, i, "key");
		if (name != NULL && g_strcmp0(g_value_get_string(name), key) == 0)
		{
			const GValue *value = venture_report_result_get_cell(r, i, column);
			g_assert_nonnull(value);
			g_assert_true(G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY));
			return ((const VentureMoney *)g_value_get_boxed(value))->amount;
		}
	}
	g_error("Missing budget row %s", key);
	return 0;
}

static gint64
metric_money(VentureReportResult *r, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(r);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(metric), key) == 0)
		{
			const VentureMoney *money = venture_metric_get_money(metric);
			g_assert_nonnull(money);
			return money->amount;
		}
	}
	g_error("Missing metric %s", key);
	return 0;
}

static void
test_records(void)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "budget"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "budget_line"), !=, G_TYPE_INVALID);
}

static void
test_vs_actual(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) budget = VENTURE_ENTITY(venture_budget_new());
	g_autoptr(VentureEntity) line = VENTURE_ENTITY(venture_budget_line_new());
	g_autoptr(VentureMoney) planned = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(budget, "organization-id", f->org, "name", "August plan", "period", "2026-08",
		"currency", "USD", "status", "active", NULL);
	save(f, budget);
	g_object_set(line, "organization-id", f->org, "budget-id", venture_entity_get_id(budget),
		"account-id", account(f, "6900"), "period", "2026-08", "amount", planned, "dimension", "ops", NULL);
	save(f, line);
	post(f, "2026-08-10T00:00:00Z", "6900", "1000", 4000);
	result = venture_budget_service_vs_actual(venture_budget_service_get(f->db), f->org, "2026-08", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(cell(result, "6900", "budget"), ==, 10000);
	g_assert_cmpint(cell(result, "6900", "actual"), ==, 4000);
	g_assert_cmpint(cell(result, "6900", "variance"), ==, -6000);
}

static void
test_cash_forecast(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) budget = VENTURE_ENTITY(venture_budget_new());
	g_autoptr(VentureEntity) income = VENTURE_ENTITY(venture_budget_line_new());
	g_autoptr(VentureEntity) expense = VENTURE_ENTITY(venture_budget_line_new());
	g_autoptr(VentureMoney) in_amount = venture_money_new_for_currency(50000, "USD");
	g_autoptr(VentureMoney) out_amount = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(budget, "organization-id", f->org, "name", "August plan", "period", "2026-08",
		"currency", "USD", "status", "active", NULL);
	save(f, budget);
	g_object_set(income, "organization-id", f->org, "budget-id", venture_entity_get_id(budget),
		"account-id", account(f, "4000"), "period", "2026-08", "amount", in_amount, NULL);
	save(f, income);
	g_object_set(expense, "organization-id", f->org, "budget-id", venture_entity_get_id(budget),
		"account-id", account(f, "6900"), "period", "2026-08", "amount", out_amount, NULL);
	save(f, expense);
	post(f, "2026-08-02T00:00:00Z", "1100", "4000", 15000);
	post(f, "2026-08-03T00:00:00Z", "6900", "2000", 2000);
	result = venture_budget_service_cash_forecast(venture_budget_service_get(f->db), f->org, "2026-08", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(metric_money(result, "receivables"), ==, 15000);
	g_assert_cmpint(metric_money(result, "payables"), ==, 2000);
	g_assert_cmpint(metric_money(result, "budget_in"), ==, 35000);
	g_assert_cmpint(metric_money(result, "budget_out"), ==, 8000);
	g_assert_cmpint(metric_money(result, "forecast"), ==, 15000 - 2000 + 35000 - 8000);
}

static void
test_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	venture_config_set_module_enabled(f->config, "budgets", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "budget_vs_actual"));
	g_assert_false(venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "budget"));
	g_assert_null(venture_budget_service_vs_actual(venture_budget_service_get(f->db), f->org, "2026-08", NULL, &error));
	g_assert_nonnull(strstr(error->message, "budgets"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/budgets/records", test_records);
	g_test_add("/budgets/vs-actual", Fixture, NULL, setup, test_vs_actual, teardown);
	g_test_add("/budgets/cash-forecast", Fixture, NULL, setup, test_cash_forecast, teardown);
	g_test_add("/budgets/module-off", Fixture, NULL, setup, test_module_off, teardown);
	return g_test_run();
}
