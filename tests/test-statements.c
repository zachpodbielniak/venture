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

static void
test_registration(Fixture *f, gconstpointer data)
{
	VentureReport *report;
	VentureModule *module;
	gboolean financial = FALSE;
	g_autoptr(JsonNode) parameters = NULL;
	JsonObject *properties;
	module = venture_module_registry_lookup(venture_context_get_modules(f->context), "statements");
	g_assert_nonnull(module);
	g_assert_true(g_strv_contains(venture_module_get_requires(module), "ledger"));
	g_assert_true(g_strv_contains(venture_module_get_requires(module), "periods"));
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), data);
	g_assert_nonnull(report);
	g_object_get(report, "financial", &financial, NULL);
	g_assert_true(financial);
	parameters = venture_report_describe_parameters(report);
	properties = json_object_get_object_member(json_node_get_object(parameters), "properties");
	g_assert_true(json_object_has_member(properties, "compare_to"));
	g_assert_true(json_object_has_member(properties, "currency"));
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

static void
test_balances(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureAccount) parent = venture_account_new();
	g_autoptr(VentureAccount) grandparent = venture_account_new();
	g_autoptr(VentureEntity) cash = NULL;
	g_autoptr(VentureLedgerBalances) query = venture_ledger_balances_new(f->db);
	g_autoptr(VentureDateRange) period = venture_context_parse_period(f->context, "2026-08", NULL);
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GDateTime) cutoff = venture_time_from_string("2026-08-11T00:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureJournal) draft = venture_journal_new();
	g_autoptr(VentureJournalLine) draft_line = venture_journal_line_new();
	g_autoptr(VentureMoney) fake = venture_money_new_for_currency(99999, "USD");
	gint64 parent_id;
	(void)data;
	g_object_set(grandparent, "code", "0800", "name", "All assets", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(grandparent), NULL, &error));
	g_object_set(parent, "code", "0900", "name", "Cash group", "organization-id", f->org,
		"parent-id", venture_entity_get_id(VENTURE_ENTITY(grandparent)), NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(parent), NULL, &error));
	parent_id = venture_entity_get_id(VENTURE_ENTITY(parent));
	cash = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account(f, "1000"), &error);
	g_object_set(cash, "parent-id", parent_id, "opening-balance", fake, NULL);
	g_assert_true(venture_database_save(f->db, cash, NULL, &error));
	post(f, "2026-07-10T00:00:00Z", "USD", "1000", "4000", 10000);
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 20000);
	post(f, "2026-08-12T00:00:00Z", "USD", "6400", "1000", 3000);
	post(f, "2026-08-10T00:00:00Z", "EUR", "1000", "4000", 700);
	g_object_set(draft, "organization-id", f->org, "occurred-at", cutoff, "currency", "USD", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(draft), NULL, &error));
	g_object_set(draft_line, "organization-id", f->org, "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)),
		"account-id", account(f, "1000"), "side", VENTURE_LEDGER_SIDE_DEBIT,
		"amount", fake, "book-amount", fake, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(draft_line), NULL, &error));
	g_assert_no_error(error);
	r = venture_ledger_balances_query(query, f->org, "USD", period, NULL, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(r, "0800", "closing"), ==, 27000);
	g_assert_cmpint(cell(r, "0900", "debits"), ==, 20000);
	g_assert_cmpint(cell(r, "1000", "opening"), ==, 10000);
	g_clear_object(&r);
	r = venture_ledger_balances_query(query, f->org, "EUR", period, NULL, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(r, "0800", "closing"), ==, 700);
	g_clear_object(&r);
	r = venture_ledger_balances_query(query, f->org, NULL, period, NULL, TRUE, &error);
	g_assert_no_error(error);
	{
		guint i, currencies = 0;
		for (i = 0; i < venture_report_result_get_row_count(r); i++)
		{
			const gchar *key = g_value_get_string(venture_report_result_get_cell(r, i, "key"));
			if (g_str_equal(key, "0800"))
			{
				const gchar *currency = g_value_get_string(venture_report_result_get_cell(r, i, "currency"));
				const VentureMoney *value = g_value_get_boxed(venture_report_result_get_cell(r, i, "closing"));
				g_assert_cmpint(value->amount, ==, g_str_equal(currency, "EUR") ? 700 : 27000);
				currencies++;
			}
		}
		g_assert_cmpuint(currencies, ==, 2);
	}
	g_clear_object(&r);
	r = venture_ledger_balances_query(query, f->org, "USD", period, cutoff, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(r, "1000", "closing"), ==, 30000);
	g_clear_object(&r);
	r = venture_ledger_balances_query(query, f->org, "USD", period, NULL, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(r, "0900", "closing"), ==, 0);
	g_clear_object(&r);
	r = report(f, "balance_sheet", "2026-08", "USD", NULL);
	g_assert_cmpint(cell(r, "assets", "current"), ==, 27000);
	/* The report must refuse a cycle instead of amplifying descendant money. */
	g_object_set(grandparent, "parent-id", parent_id, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(grandparent), NULL, &error));
	g_clear_object(&r);
	r = venture_ledger_balances_query(query, f->org, "USD", period, NULL, TRUE, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_snapshots(Fixture *f, gconstpointer data)
{
	g_autoptr(GDateTime) start = venture_time_from_string("2026-08-01", NULL);
	g_autoptr(GDateTime) later = venture_time_from_string("2026-09-02", NULL);
	g_autoptr(VentureFiscalYear) year = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureJournal) reversed = NULL;
	g_autoptr(VentureReportResult) drift = NULL;
	g_autoptr(GPtrArray) snapshots = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	gint64 journal_id;
	guint i, statements = 0;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "closer";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	year = venture_period_service_generate(venture_period_service_get(f->db), f->org,
		"FY2026", start, VENTURE_PERIOD_MONTHLY, &actor, &error);
	g_assert_no_error(error);
	journal_id = post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 10000);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(f->db, query, &error);
	g_object_set(period, "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->db, period, &actor, &error));
	g_assert_no_error(error);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_REPORT_SNAPSHOT);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, f->org);
	snapshots = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < snapshots->len; i++)
	{
		g_autofree gchar *name = NULL;
		VentureModule *module = venture_module_registry_lookup(venture_context_get_modules(f->context), "statements");
		g_object_get(g_ptr_array_index(snapshots, i), "report", &name, NULL);
		if (g_strv_contains(venture_module_get_reports(module), name))
			statements++;
	}
	g_assert_cmpuint(statements, ==, 6);
	reversed = venture_posting_service_reverse(venture_database_get_posting_service(f->db), journal_id,
		later, "September correction", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reversed);
	drift = report(f, "snapshot_vs_live", "2026-08", NULL, NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(drift), ==, 0);
}

static void
test_controls(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) r = NULL;
	(void)data;
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 20000);
	post(f, "2026-08-11T00:00:00Z", "USD", "1200", "2000", 4000);
	post(f, "2026-08-12T00:00:00Z", "USD", "1100", "4000", 1000);
	post(f, "2026-08-13T00:00:00Z", "USD", "2000", "1000", 2000);
	post(f, "2026-08-14T00:00:00Z", "USD", "1000", "2100", 500);
	post(f, "2026-08-15T00:00:00Z", "USD", "1000", "3000", 7000);
	r = report(f, "cash_flow", "2026-08", "USD", NULL);
	g_assert_cmpint(cell(r, "net_income", "current"), ==, 21000);
	g_assert_cmpint(cell(r, "receivables", "current"), ==, -1000);
	g_assert_cmpint(cell(r, "payables", "current"), ==, 2000);
	g_assert_cmpint(cell(r, "inventory", "current"), ==, -4000);
	g_assert_cmpint(cell(r, "tax_payable", "current"), ==, 500);
	g_assert_cmpint(cell(r, "cash_movement", "current"), ==, 25500);
	g_assert_cmpint(cell(r, "difference", "current"), ==, 0);
}

static void
test_source_reconciliation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureVenture) venture = venture_venture_new();
	g_autoptr(VentureSale) sale = venture_sale_new();
	g_autoptr(VentureMoney) gross = venture_money_new_for_currency(1234, "USD");
	g_autoptr(GDateTime) date = venture_time_from_string("2026-08-10", NULL);
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	(void)data;
	g_object_set(venture, "name", "Books", "venture-type", "books", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(venture), NULL, &error));
	g_object_set(sale, "venture-id", venture_entity_get_id(VENTURE_ENTITY(venture)),
		"organization-id", f->org, "gross", gross, "occurred-at", date, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(sale), NULL, &error));
	g_assert_no_error(error);
	r = report(f, "balance_sheet", "2026-08", "USD", NULL);
	g_assert_cmpint(cell(r, "assets", "current"), ==, 1234);
	g_assert_cmpint(cell(r, "equity", "current"), ==, 1234);
	g_assert_cmpint(cell(r, "difference", "current"), ==, 0);
	g_clear_object(&r);
	r = report(f, "pnl_reconciliation", "2026-08", "USD", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 0);
	g_assert_true(venture_database_delete(f->db, VENTURE_ENTITY(sale), NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&r);
	r = report(f, "pnl_reconciliation", "2026-08", "USD", NULL);
	key = g_strdup_printf("sale:%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(sale)));
	g_assert_cmpint(cell(r, key, "difference"), ==, 1234);
	g_assert_cmpint(cell(r, key, "ledger"), ==, 1234);
	g_assert_cmpint(cell(r, key, "operational"), ==, 0);
}

static void
test_prior_only(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) r = NULL;
	(void)data;
	post(f, "2026-07-10T00:00:00Z", "USD", "1000", "4000", 10000);
	r = report(f, "pnl_reconciliation", "2026-08", "USD", "2026-07");
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 1);
	g_assert_cmpint(cell(r, "organization:1", "current"), ==, 0);
	g_assert_cmpint(cell(r, "organization:1", "prior"), ==, 10000);
	g_assert_cmpint(cell(r, "organization:1", "delta"), ==, -10000);
}

static void
test_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureLedgerBalances) query = venture_ledger_balances_new(f->db);
	g_autoptr(VentureDateRange) period = venture_context_parse_period(f->context, "2026-08", NULL);
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReport *registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "balance_sheet");
	(void)data;
	r = venture_ledger_balances_query(query, 0, "USD", period, NULL, TRUE, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	r = venture_ledger_balances_query(query, f->org, "usd", period, NULL, TRUE, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	json_object_set_string_member(options, "as_of", "not-a-date");
	r = venture_report_generate(registered, f->context, period, options, &error);
	g_assert_null(r);
	g_assert_nonnull(error);
	g_clear_error(&error);
	json_object_remove_member(options, "as_of");
	json_object_set_string_member(options, "compare_to", "2026-09");
	r = venture_report_generate(registered, f->context, period, options, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", G_MAXINT64);
	post(f, "2026-08-11T00:00:00Z", "USD", "1000", "4000", 1);
	r = venture_ledger_balances_query(query, f->org, "USD", period, NULL, TRUE, &error);
	g_assert_null(r);
	g_assert_nonnull(error);
}

static void
test_organization(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureAccount) cash = venture_account_new();
	g_autoptr(VentureAccount) income = venture_account_new();
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	gint64 original = f->org;
	(void)data;
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 10000);
	g_object_set(other, "name", "Other legal entity", "parent-id", original, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	f->org = venture_entity_get_id(VENTURE_ENTITY(other));
	g_object_set(cash, "name", "Cash", "code", "1000", "active", TRUE, "organization-id", f->org, NULL);
	g_object_set(income, "name", "Income", "code", "4000", "kind", VENTURE_ACCOUNT_KIND_INCOME,
		"active", TRUE, "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(cash), NULL, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(income), NULL, &error));
	g_assert_no_error(error);
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 777);
	r = report(f, "balance_sheet", "2026-08", "USD", NULL);
	g_assert_cmpint(cell(r, "assets", "current"), ==, 777);
	f->org = original;
	g_clear_object(&r);
	r = report(f, "balance_sheet", "2026-08", "USD", NULL);
	g_assert_cmpint(cell(r, "assets", "current"), ==, 10000);
}

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
	gchar *out;
	gchar *err;
} Response;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Response *response = data;
	response->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Response *response = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &response->out, &response->err, &response->error);
	response->done = TRUE;
}

static gchar *
http_get(SoupSession *session, const gchar *base, const gchar *path, guint expected)
{
	g_autofree gchar *url = g_strconcat(base, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Response response = { FALSE, NULL, NULL, NULL, NULL };
	gchar *body;
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, expected);
	body = g_strndup(g_bytes_get_data(response.body, NULL), g_bytes_get_size(response.body));
	g_bytes_unref(response.body);
	return body;
}

static void
test_surfaces(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-statements-XXXXXX", NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *body = NULL;
	const gchar *args[12];
	Response response = { FALSE, NULL, NULL, NULL, NULL };
	guint port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	(void)data;
	g_assert_no_error(error);
	g_clear_object(&probe);
	g_object_set(f->config, "state-dir", dir, "security-require-auth", FALSE,
		"server-bind-address", "127.0.0.1", "server-port", (gint64)port, NULL);
	post(f, "2026-07-10T00:00:00Z", "USD", "1000", "4000", 10000);
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 20000);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	base = g_strdup_printf("http://127.0.0.1:%u", port);
	body = http_get(session, base, "/reports", 200);
	g_assert_nonnull(strstr(body, "<h2>Statements</h2>"));
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/reports/balance_sheet?period=2026-08&compare_to=2026-07&as_of=2026-08-31&currency=USD&organization_id=1", 200);
	g_assert_nonnull(strstr(body, "name=\"compare_to\""));
	g_assert_nonnull(strstr(body, "Prior"));
	g_assert_nonnull(strstr(body, "compare_to=2026-07"));
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/api/v1/reports/general_ledger?period=2026-08&compare_to=2026-07&account_id=1&currency=USD&organization_id=1", 200);
	g_assert_nonnull(strstr(body, "prior"));
	args[0] = "build/debug/venturectl";
	args[1] = "--server";
	args[2] = base;
	args[3] = "-f";
	args[4] = "csv";
	args[5] = "report";
	args[6] = "balance_sheet";
	args[7] = "2026-08";
	args[8] = "organization_id=1";
	args[9] = "currency=USD";
	args[10] = "compare_to=2026-07";
	args[11] = NULL;
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	g_test_message("CLI stderr: %s", response.err);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(strstr(response.out, "Prior"));
	g_assert_nonnull(strstr(response.out, "300.00"));
	g_free(response.out);
	g_free(response.err);
	venture_config_set_module_enabled(f->config, "statements", FALSE);
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/reports/balance_sheet", 404);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
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
	g_test_add("/statements/balances", Fixture, NULL, setup, test_balances, teardown);
	g_test_add("/statements/snapshots", Fixture, NULL, setup, test_snapshots, teardown);
	g_test_add("/statements/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	g_test_add("/statements/refusals", Fixture, NULL, setup, test_refusals, teardown);
	g_test_add("/statements/organization", Fixture, NULL, setup, test_organization, teardown);
	g_test_add("/statements/prior-only", Fixture, NULL, setup, test_prior_only, teardown);
	g_test_add("/statements/controls", Fixture, NULL, setup, test_controls, teardown);
	g_test_add("/statements/source-reconciliation", Fixture, NULL, setup, test_source_reconciliation, teardown);
	return g_test_run();
}
