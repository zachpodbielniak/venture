/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

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

static void
test_registration(Fixture *f, gconstpointer data)
{
	VentureReport *report;
	VentureModule *module;
	gboolean financial = FALSE;
	module = venture_module_registry_lookup(venture_context_get_modules(f->context), "statements");
	g_assert_nonnull(module);
	g_assert_true(g_strv_contains(venture_module_get_requires(module), "ledger"));
	g_assert_true(g_strv_contains(venture_module_get_requires(module), "periods"));
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), data);
	g_assert_nonnull(report);
	g_object_get(report, "financial", &financial, NULL);
	g_assert_true(financial);
	venture_config_set_module_enabled(f->config, "statements", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), data));
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

static gint64
post(Fixture *f, const gchar *when, const gchar *currency, const gchar *debit,
	const gchar *credit, gint64 amount)
{
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) date = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(amount, currency);
	g_autoptr(GError) error = NULL;
	VentureJournalLine *line;
	g_object_set(header, "organization-id", f->org, "source-type", "organization",
		"source-id", f->org, "occurred-at", date, "currency", currency, NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account(f, debit), "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account(f, credit), "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), header, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	return venture_entity_get_id(VENTURE_ENTITY(posted));
}

static VentureReportResult *
report(Fixture *f, const gchar *name, const gchar *label, const gchar *currency,
	const gchar *compare)
{
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *registered;
	VentureReportResult *result;
	registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	g_assert_nonnull(registered);
	range = venture_context_parse_period(f->context, label, &error);
	g_assert_no_error(error);
	json_object_set_int_member(options, "organization_id", f->org);
	if (currency != NULL)
		json_object_set_string_member(options, "currency", currency);
	if (compare != NULL)
		json_object_set_string_member(options, "compare_to", compare);
	result = venture_report_generate(registered, f->context, range, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
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
	g_error("Missing statement row %s", key);
	return 0;
}

static void
test_statement(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(VentureReportResult) eur = NULL;
	g_autofree gchar *csv = NULL;
	const gchar *name = data;
	/* Prior income, current cash income, and an accrued invoice: cash and
	 * recognized profit differ, but both sides of the books still tie. */
	post(f, "2026-07-10T00:00:00Z", "USD", "1000", "4000", 10000);
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 20000);
	post(f, "2026-08-12T00:00:00Z", "USD", "1100", "4000", 5000);
	post(f, "2026-08-14T00:00:00Z", "USD", "6400", "1000", 3000);
	post(f, "2026-08-14T00:00:00Z", "EUR", "1000", "4000", 700);
	post(f, "2026-09-01T00:00:00Z", "USD", "1000", "4000", 999);
	r = report(f, name, "2026-08", "USD", "2026-07");
	if (g_str_equal(name, "balance_sheet"))
	{
		g_assert_cmpint(cell(r, "assets", "current"), ==, 32000);
		g_assert_cmpint(cell(r, "equity", "current"), ==, 32000);
		g_assert_cmpint(cell(r, "net_income", "current"), ==, 22000);
		g_assert_cmpint(cell(r, "difference", "current"), ==, 0);
		g_assert_cmpint(cell(r, "assets", "prior"), ==, 10000);
		g_assert_cmpint(cell(r, "assets", "delta"), ==, 22000);
	}
	else if (g_str_equal(name, "income_statement"))
	{
		g_assert_cmpint(cell(r, "income", "current"), ==, 25000);
		g_assert_cmpint(cell(r, "expenses", "current"), ==, 3000);
		g_assert_cmpint(cell(r, "net_income", "current"), ==, 22000);
		g_assert_cmpint(cell(r, "net_income", "prior"), ==, 10000);
		g_assert_cmpint(cell(r, "net_income", "delta"), ==, 12000);
		csv = venture_report_result_render(r, VENTURE_OUTPUT_FORMAT_JSON);
		g_assert_nonnull(strstr(csv, "pnl_reconciliation"));
	}
	else if (g_str_equal(name, "cash_flow"))
	{
		g_assert_cmpint(cell(r, "net_income", "current"), ==, 22000);
		g_assert_cmpint(cell(r, "receivables", "current"), ==, -5000);
		g_assert_cmpint(cell(r, "cash_movement", "current"), ==, 17000);
		g_assert_cmpint(cell(r, "cash_start", "current"), ==, 10000);
		g_assert_cmpint(cell(r, "cash_end", "current"), ==, 27000);
		g_assert_cmpint(cell(r, "difference", "current"), ==, 0);
	}
	else if (g_str_equal(name, "account_balances"))
	{
		g_assert_cmpint(cell(r, "1000", "opening"), ==, 10000);
		g_assert_cmpint(cell(r, "1000", "debits"), ==, 20000);
		g_assert_cmpint(cell(r, "1000", "credits"), ==, 3000);
		g_assert_cmpint(cell(r, "1000", "closing"), ==, 27000);
		g_assert_cmpint(cell(r, "1000", "prior"), ==, 10000);
	}
	else if (g_str_equal(name, "general_ledger"))
	{
		guint i;
		gboolean saw_cash = FALSE;
		g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 6);
		for (i = 0; i < venture_report_result_get_row_count(r); i++)
		{
			g_assert_nonnull(venture_report_result_get_cell(r, i, "journal_id"));
			g_assert_nonnull(venture_report_result_get_cell(r, i, "account_id"));
			g_assert_nonnull(venture_report_result_get_cell(r, i, "prior"));
			if (g_strcmp0(g_value_get_string(venture_report_result_get_cell(r, i, "key")), "1000") == 0)
			{
				const VentureMoney *running = g_value_get_boxed(venture_report_result_get_cell(r, i, "current"));
				g_assert_true(running->amount == 30000 || running->amount == 27000);
				saw_cash = TRUE;
			}
		}
		g_assert_true(saw_cash);
		csv = venture_report_result_render(r, VENTURE_OUTPUT_FORMAT_HTML);
		g_assert_nonnull(strstr(csv, "/e/journal/"));
	}
	else
	{
		g_assert_cmpuint(venture_report_result_get_row_count(r), >, 0);
		g_assert_nonnull(venture_report_result_get_cell(r, 0, "source_id"));
		g_assert_cmpint(cell(r, "organization:1", "difference"), ==, 22000);
	}
	g_clear_pointer(&csv, g_free);
	csv = venture_report_result_render(r, VENTURE_OUTPUT_FORMAT_CSV);
	g_assert_nonnull(strstr(csv, "Prior"));
	eur = report(f, name, "2026-08", "EUR", NULL);
	if (g_str_equal(name, "balance_sheet"))
		g_assert_cmpint(cell(eur, "assets", "current"), ==, 700);
	if (g_str_equal(name, "income_statement"))
		g_assert_cmpint(cell(eur, "net_income", "current"), ==, 700);
}

int
main(int argc, char **argv)
{
	static const gchar *const names[] = { "balance_sheet", "income_statement", "cash_flow", "general_ledger", "account_balances", "pnl_reconciliation" };
	guint i;
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autofree gchar *path = g_strdup_printf("/statements/registration/%s", names[i]);
		g_test_add(path, Fixture, names[i], setup, test_registration, teardown);
		g_free(g_steal_pointer(&path));
		path = g_strdup_printf("/statements/amounts/%s", names[i]);
		g_test_add(path, Fixture, names[i], setup, test_statement, teardown);
	}
	return g_test_run();
}
