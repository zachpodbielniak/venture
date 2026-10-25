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
	g_assert_true(json_object_has_member(properties, "basis"));
	g_assert_true(json_object_has_member(properties, "dimension"));
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
	g_clear_object(&r);
	r = report(f, "net_worth", "2026-08", "USD", NULL);
	g_assert_cmpint(cell(r, "2026-08", "assets"), ==, 10000);
	f->org = venture_entity_get_id(VENTURE_ENTITY(other));
	g_clear_object(&r);
	r = report(f, "net_worth", "2026-08", "USD", NULL);
	g_assert_cmpint(cell(r, "2026-08", "assets"), ==, 777);
	f->org = original;
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
	/* Isolate local requests from idle keep-alive reuse, and bound failures. */
	g_object_set(session, "timeout", 20u, NULL);
	soup_message_headers_replace(soup_message_get_request_headers(message), "Connection", "close");
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
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureGoal) savings = venture_goal_new();
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-statements-XXXXXX", NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *body = NULL;
	const gchar *args[14];
	Response response = { FALSE, NULL, NULL, NULL, NULL };
	guint port = 0, wealth;
	(void)data;
	g_assert_no_error(error);
	g_object_set(f->config, "state-dir", dir, "security-require-auth", FALSE,
		"server-bind-address", "127.0.0.1", "server-port", (gint64)port, NULL);
	post(f, "2026-07-10T00:00:00Z", "USD", "1000", "4000", 10000);
	post(f, "2026-08-10T00:00:00Z", "USD", "1000", "4000", 20000);
	g_object_set(savings, "name", "Household reserve", "organization-id", f->org,
		"savings-account-id", account(f, "1000"), NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(savings), "savings-target", "500 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(savings), "savings-monthly", "100 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(savings), "due-on", "2026-10-31", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(savings), NULL, &error));
	g_assert_no_error(error);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	port = venture_web_server_get_port(server);
	base = g_strdup_printf("http://127.0.0.1:%u", port);
	body = http_get(session, base, "/reports", 200);
	g_assert_nonnull(strstr(body, "<h2>Statements</h2>"));
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/reports/balance_sheet?period=2026-08&compare_to=2026-07&as_of=2026-08-31&currency=USD&organization_id=1", 200);
	g_assert_nonnull(strstr(body, "name=\"compare_to\""));
	g_assert_nonnull(strstr(body, "Prior"));
	g_assert_nonnull(strstr(body, "compare_to=2026-07"));
	g_assert_nonnull(strstr(body, "name=\"basis\""));
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/reports/income_statement?period=2026-08&basis=cash&currency=USD&organization_id=1", 200);
	g_assert_nonnull(strstr(body, "value=\"cash\" selected"));
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/api/v1/reports/general_ledger?period=2026-08&compare_to=2026-07&account_id=1&currency=USD&organization_id=1", 200);
	g_assert_nonnull(strstr(body, "prior"));
	args[0] = VENTURE_TEST_BIN_DIR "/venturectl";
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
	/* Filtering survives both web parsers and CLI option forwarding. */
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/reports/income_statement?period=2026-08&basis=cash&dimension=missing&currency=USD", 200);
	g_assert_null(strstr(body, "200.00"));
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/api/v1/reports/general_ledger?period=2026-08&basis=cash&dimension=missing&currency=USD", 200);
	g_assert_null(strstr(body, "20000"));
	g_clear_object(&child);
	args[11] = "basis=cash";
	args[12] = "dimension=missing";
	args[13] = NULL;
	response.done = FALSE;
	response.out = NULL;
	response.err = NULL;
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_null(strstr(response.out, "300.00"));
	g_assert_null(strstr(response.out, "200.00"));
	g_free(response.out);
	g_free(response.err);
	for (wealth = 0; wealth < 3; wealth++)
	{
		const gchar *name = wealth == 0 ? "net_worth" : wealth == 1 ? "debt_payoff" : "savings_goals";
		g_autofree gchar *page = g_strdup_printf("/reports/%s?period=2026-08&currency=USD", name);
		g_autofree gchar *api = g_strdup_printf("/api/v1/reports/%s?period=2026-08&currency=USD", name);
		g_clear_pointer(&body, g_free);
		body = http_get(session, base, page, 200);
		g_assert_nonnull(strstr(body, wealth == 0 ? "Net worth" : wealth == 1 ? "Debt payoff" : "Savings goals"));
		/* A month-only report must never offer a quarter/year that it refuses. */
		if (wealth == 2)
		{
			g_assert_null(strstr(body, "href=\"/reports/savings_goals?period=this_year"));
			g_assert_null(strstr(body, "href=\"/reports/savings_goals?period=this_quarter"));
			g_assert_null(strstr(body, "href=\"/reports/savings_goals?period=ytd"));
			g_assert_nonnull(strstr(body, "href=\"/reports/savings_goals?period=last_month"));
		}
		g_clear_pointer(&body, g_free);
		body = http_get(session, base, api, 200);
		g_assert_nonnull(strstr(body, wealth == 0 ? "net_worth" : wealth == 1 ? "interest_saved" : "monthly_needed"));
		g_clear_object(&child);
		args[6] = name;
		args[10] = NULL;
		response.done = FALSE;
		response.out = NULL;
		response.err = NULL;
		child = g_subprocess_launcher_spawnv(launcher, args, &error);
		g_assert_no_error(error);
		g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &response);
		while (!response.done) g_main_context_iteration(NULL, TRUE);
		g_assert_no_error(response.error);
		g_assert_true(g_subprocess_get_successful(child));
		g_assert_nonnull(strstr(response.out, wealth == 0 ? "Net worth" : wealth == 1 ? "Interest saved" : "Monthly amount needed"));
		if (wealth == 2)
		{
			g_assert_nonnull(strstr(response.out, "200.00"));
			g_assert_nonnull(strstr(response.out, "2026-10-31"));
		}
		g_free(response.out);
		g_free(response.err);
	}
	/* The saved goal must read the same posted money as its report, not
	 * its unused manual Start/Current/Target zeroes. */
	{
		g_autofree gchar *path = g_strdup_printf("/e/goal/%" G_GINT64_FORMAT,
			venture_entity_get_id(VENTURE_ENTITY(savings)));
		g_clear_pointer(&body, g_free);
		body = http_get(session, base, path, 200);
		g_assert_nonnull(strstr(body, "class=\"card savings-progress\""));
		g_assert_null(strstr(body, "class=\"card goal-progress\""));
		g_assert_nonnull(strstr(body, "300.00"));
		g_assert_nonnull(strstr(body, "500.00"));
		g_assert_nonnull(strstr(body, "60.0%"));
		g_assert_nonnull(strstr(body, "goal-steps"));
	}
	venture_config_set_module_enabled(f->config, "statements", FALSE);
	g_clear_pointer(&body, g_free);
	body = http_get(session, base, "/reports/balance_sheet", 404);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
}

/* Accrual recognizes the invoice in January; cash recognizes the receipt in
 * February. Both bases keep one ledger authority. */
static void
test_cash_basis(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(VentureReportResult) accrual = NULL;
	g_autoptr(VentureReportResult) cash_jan = NULL;
	g_autoptr(VentureReportResult) cash_feb = NULL;
	g_autoptr(JsonObject) cash = json_object_new();
	g_autoptr(VentureDateRange) january = NULL;
	g_autoptr(VentureDateRange) february = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *income;
	(void)data;
	g_object_set(customer, "name", "Cash customer", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	g_object_set(invoice, "number", "CASH-1", "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", "2026-01-10", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, "tax-percent", (gint64)5,
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", "100 USD", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(payment, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "method", "transfer",
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "amount", "105 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "date", "2026-02-05", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(payment), NULL, &error));
	g_assert_no_error(error);
	income = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "income_statement");
	january = venture_context_parse_period(f->context, "2026-01", NULL);
	february = venture_context_parse_period(f->context, "2026-02", NULL);
	json_object_set_int_member(cash, "organization_id", f->org);
	json_object_set_string_member(cash, "currency", "USD");
	accrual = venture_report_generate(income, f->context, january, cash, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(accrual, "income", "current"), ==, 10000);
	json_object_set_string_member(cash, "basis", "cash");
	cash_jan = venture_report_generate(income, f->context, january, cash, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(cash_jan, "income", "current"), ==, 0);
	cash_feb = venture_report_generate(income, f->context, february, cash, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(cash_feb, "income", "current"), ==, 10000);
	{
		VentureReport *sheet = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "balance_sheet");
		g_autoptr(VentureReportResult) cash_sheet = NULL;
		cash_sheet = venture_report_generate(sheet, f->context, february, cash, &error);
		g_assert_no_error(error);
		g_assert_cmpint(cell(cash_sheet, "difference", "current"), ==, 0);
		g_assert_cmpint(cell(cash_sheet, "assets", "current"), ==, 10500);
	}
	{
		VentureReport *sheet = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "balance_sheet");
		g_autoptr(VentureDateRange) march = venture_context_parse_period(f->context, "2026-03", NULL);
		g_autoptr(VentureReportResult) cash_sheet = NULL;
		cash_sheet = venture_report_generate(sheet, f->context, march, cash, &error);
		g_assert_no_error(error);
		g_assert_cmpint(cell(cash_sheet, "difference", "current"), ==, 0);
		g_assert_cmpint(cell(cash_sheet, "assets", "current"), ==, 10500);
	}
}

static void
test_cash_basis_write_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VentureReportResult) cash_feb = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) february = NULL;
	g_autoptr(GDateTime) when = venture_time_from_string("2026-02-05", NULL);
	g_autoptr(GError) error = NULL;
	VentureReport *income;
	VentureActor actor;

	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "bookkeeper";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_object_set(customer, "name", "Writeoff customer", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	g_object_set(invoice, "number", "WO-1", "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", "2026-01-10", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, "organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", "100 USD", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_assert_true(venture_settlement_service_write_off(venture_settlement_service_get(f->db),
		venture_entity_get_id(VENTURE_ENTITY(invoice)), when, &actor, &error));
	g_assert_no_error(error);
	income = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "income_statement");
	february = venture_context_parse_period(f->context, "2026-02", NULL);
	json_object_set_int_member(options, "organization_id", f->org);
	json_object_set_string_member(options, "currency", "USD");
	json_object_set_string_member(options, "basis", "cash");
	cash_feb = venture_report_generate(income, f->context, february, options, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(cash_feb, "income", "current"), ==, 0);
}

static void
test_cash_basis_refund(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(VentureRefund) refund = venture_refund_new();
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION);
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(VentureReportResult) cash_mar = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) march = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *income;

	(void)data;
	g_object_set(customer, "name", "Refund customer", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	g_object_set(invoice, "number", "RF-1", "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", "2026-01-10", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
		"description", "Work", "quantity", 1.0, "organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", "100 USD", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(payment, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "method", "transfer",
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "amount", "100 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "date", "2026-02-05", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(payment), NULL, &error));
	venture_query_set_organization(q, f->org);
	allocations = venture_database_find(f->db, q, &error);
	g_assert_cmpuint(allocations->len, ==, 1);
	g_object_set(refund, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)),
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(refund), "amount", "100 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(refund), "date", "2026-03-05", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(refund), NULL, &error));
	income = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "income_statement");
	march = venture_context_parse_period(f->context, "2026-03", NULL);
	json_object_set_int_member(options, "organization_id", f->org);
	json_object_set_string_member(options, "currency", "USD");
	json_object_set_string_member(options, "basis", "cash");
	cash_mar = venture_report_generate(income, f->context, march, options, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(cash_mar, "income", "current"), ==, -10000);
}
/* Each side must apportion one rounded cumulative total: half of a two-cent
 * invoice with equal net/tax legs is one cent, never a one-cent imbalance. */
static void
test_cash_split_rounding(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) sheet = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;

	(void)data;
	g_object_set(customer, "name", "Rounding customer", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	g_object_set(invoice, "number", "CASH-CENTS", "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", "2026-01-01", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "description", "Cent",
		"organization-id", f->org, "quantity", 1.0, "tax-percent", (gint64)100, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", "0.01 USD", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_object_set(payment, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "method", "manual", "organization-id", f->org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "amount", "0.01 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "date", "2026-02-01", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(payment), NULL, &error));
	g_assert_no_error(error);
	period = venture_context_parse_period(f->context, "2026-02", &error);
	json_object_set_string_member(options, "basis", "cash");
	json_object_set_string_member(options, "currency", "USD");
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "balance_sheet");
	sheet = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell(sheet, "difference", "current"), ==, 0);
	g_assert_cmpint(cell(sheet, "assets", "current"), ==, 1);
}

static void
test_net_worth(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) r = NULL;
	(void)data;
	post(f, "2025-12-10T00:00:00Z", "USD", "1000", "4000", 10000);
	post(f, "2026-01-10T00:00:00Z", "USD", "1000", "2000", 5000);
	post(f, "2026-02-10T00:00:00Z", "USD", "6400", "1000", 2000);
	post(f, "2026-02-10T00:00:00Z", "EUR", "1000", "4000", 700);
	post(f, "2027-01-01T00:00:00Z", "USD", "1000", "4000", 99999);
	r = report(f, "net_worth", "2026", "USD", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 12);
	g_assert_cmpint(cell(r, "2026-01", "assets"), ==, 15000);
	g_assert_cmpint(cell(r, "2026-01", "liabilities"), ==, 5000);
	g_assert_cmpint(cell(r, "2026-01", "net_worth"), ==, 10000);
	g_assert_cmpint(cell(r, "2026-02", "net_worth"), ==, 8000);
	g_assert_cmpint(cell(r, "2026-12", "net_worth"), ==, 8000);
	g_clear_object(&r);
	r = report(f, "net_worth", "2026", "EUR", NULL);
	g_assert_cmpint(cell(r, "2026-02", "net_worth"), ==, 700);
}

static VentureBankAccount *
plan_debt(Fixture *f, const gchar *code, const gchar *name, gint64 apr, const gchar *minimum, const gchar *extra)
{
	VentureBankAccount *bank = venture_bank_account_new();
	g_autoptr(GError) error = NULL;
	g_object_set(bank, "name", name, "organization-id", f->org, "account-id", account(f, code),
		"currency", "USD", "kind", VENTURE_BANK_ACCOUNT_LOAN, "payoff-enabled", TRUE,
		"payoff-apr-bps", apr, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(bank), "payoff-minimum", minimum, &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(bank), "payoff-extra", extra, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(bank), NULL, &error));
	g_assert_no_error(error);
	return bank;
}

static const gchar *
text_cell(VentureReportResult *r, const gchar *key, const gchar *column)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
		if (g_strcmp0(g_value_get_string(venture_report_result_get_cell(r, i, "key")), key) == 0)
			return g_value_get_string(venture_report_result_get_cell(r, i, column));
	g_error("Missing row %s", key);
	return NULL;
}

static void
test_debt_payoff(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBankAccount) first = NULL, second = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	(void)data;
	g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "debt_payoff"));
	first = plan_debt(f, "2000", "Small loan", 0, "20 USD", "10 USD");
	second = plan_debt(f, "2100", "Large loan", 2400, "20 USD", "0 USD");
	post(f, "2026-01-10T00:00:00Z", "USD", "1000", "2000", 10000);
	post(f, "2026-01-10T00:00:00Z", "USD", "1000", "2100", 20000);
	r = report(f, "debt_payoff", "2026-01", "USD", NULL);
	g_assert_cmpint(cell(r, "minimum", "interest"), ==, 2540);
	g_assert_cmpint(cell(r, "snowball", "interest"), ==, 1801);
	g_assert_cmpint(cell(r, "avalanche", "interest"), ==, 1631);
	g_assert_cmpint(cell(r, "snowball", "interest_saved"), ==, 739);
	g_assert_cmpint(cell(r, "avalanche", "interest_saved"), ==, 909);
	g_assert_cmpstr(text_cell(r, "minimum", "payoff_date"), ==, "2027-01-31");
	g_assert_cmpstr(text_cell(r, "snowball", "payoff_date"), ==, "2026-08-31");
	g_assert_cmpstr(text_cell(r, "avalanche", "payoff_date"), ==, "2026-08-31");
}

static void
test_wealth_cutoff(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureAccount) parent = venture_account_new();
	g_autoptr(VentureEntity) cash = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(VentureDateRange) range = venture_context_parse_period(f->context, "2026", NULL);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReport *registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "net_worth");
	(void)data;
	g_object_set(parent, "code", "0900", "name", "Asset group", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(parent), NULL, &error));
	cash = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account(f, "1000"), &error);
	g_object_set(cash, "parent-id", venture_entity_get_id(VENTURE_ENTITY(parent)), NULL);
	g_assert_true(venture_database_save(f->db, cash, NULL, &error));
	post(f, "2026-01-10T00:00:00Z", "USD", "1000", "4000", 10000);
	post(f, "2026-02-10T00:00:00Z", "USD", "1000", "4000", 5000);
	post(f, "2026-02-12T00:00:00Z", "USD", "1000", "4000", 99999);
	json_object_set_string_member(options, "currency", "USD");
	json_object_set_string_member(options, "as_of", "2026-02-11");
	r = venture_report_generate(registered, f->context, range, options, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 2);
	g_assert_cmpint(cell(r, "2026-02", "assets"), ==, 15000);
	g_clear_object(&r);
	json_object_set_string_member(options, "basis", "cash");
	r = venture_report_generate(registered, f->context, range, options, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static void
test_debt_horizon(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBankAccount) bank = plan_debt(f, "2000", "Slow loan", 1200, "0.01 USD", "0 USD");
	g_autoptr(VentureReportResult) r = NULL;
	guint i;
	(void)data;
	post(f, "2026-01-10T00:00:00Z", "USD", "1000", "2000", 10000);
	r = report(f, "debt_payoff", "2026-01", "USD", NULL);
	g_assert_cmpint(cell(r, "avalanche", "remaining"), >, 10000);
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
	{
		g_assert_null(venture_report_result_get_cell(r, i, "payoff_date"));
		g_assert_null(venture_report_result_get_cell(r, i, "interest_saved"));
	}
}

static void
test_debt_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBankAccount) bank = plan_debt(f, "2000", "Loan", 0, "1 USD", "0 USD");
	g_autoptr(VentureBankAccount) duplicate = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(VentureDateRange) range = venture_context_parse_period(f->context, "2026-01", NULL);
	VentureReport *registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "debt_payoff");
	(void)data;
	g_object_set(bank, "payoff-apr-bps", (gint64)-1, NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(bank), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(bank, "payoff-apr-bps", (gint64)0, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(bank), "payoff-extra", "1 EUR", &error));
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(bank), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	duplicate = plan_debt(f, "2000", "Duplicate loan", 0, "1 USD", "0 USD");
	r = venture_report_generate(registered, f->context, range, NULL, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_debt_rounding(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBankAccount) first = plan_debt(f, "2000", "Half-cent loan", 1200, "0.25 USD", "0 USD");
	g_autoptr(VentureBankAccount) second = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	second = venture_bank_account_new();
	g_object_set(second, "name", "Euro loan", "organization-id", f->org, "account-id", account(f, "2100"),
		"currency", "EUR", "kind", VENTURE_BANK_ACCOUNT_LOAN, "payoff-enabled", TRUE,
		"payoff-apr-bps", (gint64)1200, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(second), "payoff-minimum", "1.52 EUR", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(second), NULL, &error));
	g_assert_no_error(error);
	post(f, "2026-01-10T00:00:00Z", "USD", "1000", "2000", 50);
	post(f, "2026-01-10T00:00:00Z", "EUR", "1000", "2100", 150);
	r = report(f, "debt_payoff", "2026-01", "USD", NULL);
	g_assert_cmpint(cell(r, "minimum", "interest"), ==, 0);
	g_assert_cmpstr(text_cell(r, "minimum", "payoff_date"), ==, "2026-03-31");
	g_clear_object(&r);
	r = report(f, "debt_payoff", "2026-01", "EUR", NULL);
	g_assert_cmpint(cell(r, "minimum", "interest"), ==, 2);
	g_assert_cmpstr(text_cell(r, "minimum", "payoff_date"), ==, "2026-02-28");
	g_clear_object(&r);
	r = report(f, "debt_payoff", "2026-01", NULL, NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 12);
}

static void
test_debt_registration(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) r = NULL;
	(void)data;
	r = report(f, "debt_payoff", "2026-01", "USD", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 0);
	venture_config_set_module_enabled(f->config, "statements", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "net_worth"));
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "debt_payoff"));
}

static void
test_savings_account(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureGoal) goal = venture_goal_new();
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	(void)data;
	g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals"));
	g_object_set(goal, "name", "Rainy day", "organization-id", f->org,
		"savings-account-id", account(f, "1000"), NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "savings-target", "100 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "savings-monthly", "20 USD", &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "due-on", "2026-04-30", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(goal), NULL, &error));
	g_assert_no_error(error);
	post(f, "2026-01-10T00:00:00Z", "USD", "1000", "4000", 4000);
	post(f, "2026-02-10T00:00:00Z", "USD", "1000", "4000", 99999);
	key = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(goal)));
	r = report(f, "savings_goals", "2026-01", "USD", NULL);
	g_assert_cmpint(cell(r, key, "saved"), ==, 4000);
	g_assert_cmpint(cell(r, key, "remaining"), ==, 6000);
	g_assert_cmpint(cell(r, key, "monthly_needed"), ==, 2000);
	g_assert_cmpstr(text_cell(r, key, "forecast"), ==, "2026-04-30");
	g_clear_object(&r);
	r = report(f, "goal_progress", "2026-01", NULL, NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 0);
}

static VentureGoal *
savings_goal(Fixture *f, const gchar *field, gint64 id, const gchar *target, const gchar *monthly)
{
	VentureGoal *goal = venture_goal_new();
	g_autoptr(GError) error = NULL;
	g_object_set(goal, "name", "Household reserve", "organization-id", f->org, field, id, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "savings-target", target, &error));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "savings-monthly", monthly, &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(goal), NULL, &error));
	g_assert_no_error(error);
	return goal;
}

/* Currency operands stay bound in both SQL dialects; arbitrary fields do not. */
static void
test_savings_currency_query(void)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_GOAL);
	g_autoptr(GError) error = NULL;
	const gchar *operand = "USD' OR 1=1 --";
	guint i;
	g_assert_false(venture_query_add_money_currency(query, "name", "USD", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_query_add_money_currency(query, "missing", "USD", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_true(venture_query_add_money_currency(query, "savings_target", operand, &error));
	g_assert_no_error(error);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *sql = NULL;
		GList *params = NULL;
		sql = venture_query_to_sql(query, i == 0 ? ORM_DIALECT_SQLITE : ORM_DIALECT_POSTGRES, FALSE, &params);
		g_assert_nonnull(strstr(sql, "\"savings_target_currency\" = "));
		g_assert_null(strstr(sql, operand));
		g_assert_cmpuint(g_list_length(params), ==, 1);
		g_assert_cmpstr(orm_value_get_string(params->data), ==, operand);
		g_list_free_full(params, (GDestroyNotify)orm_value_free);
	}
}

/* Unrelated goals must not exhaust a narrowed savings report's row bound. */
static void
test_savings_bound(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureGoal) ordinary = venture_goal_new();
	g_autoptr(VentureGoal) usd = NULL, eur = NULL, second = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) range = venture_context_parse_period(f->context, "2026-01", NULL);
	VentureReport *registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals");
	guint old_limit = venture_aggregate_get_max_rows();
	(void)data;
	g_object_set(ordinary, "name", "Learn a craft", "organization-id", f->org, "target-value", 1.0, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(ordinary), NULL, &error));
	usd = savings_goal(f, "savings-account-id", account(f, "1000"), "100 USD", "20 USD");
	eur = savings_goal(f, "savings-account-id", account(f, "1000"), "100 EUR", "20 EUR");
	venture_aggregate_set_max_rows(1);
	r = report(f, "savings_goals", "2026-01", "USD", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 1);
	g_assert_cmpstr(g_value_get_string(venture_report_result_get_cell(r, 0, "currency")), ==, "USD");
	g_clear_object(&r);
	r = report(f, "savings_goals", "2026-01", "EUR", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 1);
	g_clear_object(&r);
	/* Without currency, only the two savings goals count, not the ordinary goal. */
	venture_aggregate_set_max_rows(2);
	r = report(f, "savings_goals", "2026-01", NULL, NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 2);
	g_clear_object(&r);
	second = savings_goal(f, "savings-account-id", account(f, "1000"), "50 USD", "10 USD");
	venture_aggregate_set_max_rows(1);
	json_object_set_string_member(options, "currency", "USD");
	r = venture_report_generate(registered, f->context, range, options, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	json_object_set_int_member(options, "goal_id", venture_entity_get_id(VENTURE_ENTITY(usd)));
	r = venture_report_generate(registered, f->context, range, options, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(r), ==, 1);
	venture_aggregate_set_max_rows(old_limit);
}

static void
test_savings_envelope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureBudget) budget = venture_budget_new();
	g_autoptr(VentureBudgetLine) line = venture_budget_line_new();
	g_autoptr(VentureGoal) goal = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	(void)data;
	g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals"));
	g_object_set(budget, "name", "Reserve plan", "organization-id", f->org, "period", "2026", "currency", "USD", "status", "active", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(budget), NULL, &error));
	g_object_set(line, "organization-id", f->org, "budget-id", venture_entity_get_id(VENTURE_ENTITY(budget)),
		"account-id", account(f, "6400"), "period", "2026-01", "envelope", TRUE, "rollover", TRUE, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "amount", "30 USD", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), NULL, &error));
	goal = savings_goal(f, "savings-envelope-id", venture_entity_get_id(VENTURE_ENTITY(line)), "100 USD", "30 USD");
	post(f, "2026-01-10T00:00:00Z", "USD", "6400", "1000", 500);
	post(f, "2026-02-10T00:00:00Z", "USD", "6400", "1000", 1000);
	key = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(goal)));
	r = report(f, "savings_goals", "2026-02", "USD", NULL);
	g_assert_cmpint(cell(r, key, "saved"), ==, 4500);
	g_assert_cmpint(cell(r, key, "remaining"), ==, 5500);
	g_assert_cmpstr(text_cell(r, key, "forecast"), ==, "2026-04-28");
	g_assert_null(venture_report_result_get_cell(r, 0, "monthly_needed"));
}

static void
test_savings_rounding(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureGoal) goal = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = NULL;
	(void)data;
	g_assert_nonnull(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals"));
	goal = savings_goal(f, "savings-account-id", account(f, "1000"), "1 USD", "0 USD");
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "due-on", "2026-04-30", &error));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(goal), NULL, &error));
	key = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(goal)));
	r = report(f, "savings_goals", "2026-01", "USD", NULL);
	g_assert_cmpint(cell(r, key, "monthly_needed"), ==, 34);
	g_assert_null(venture_report_result_get_cell(r, 0, "forecast"));
	g_clear_object(&r);
	r = report(f, "savings_goals", "2026-04", "USD", NULL);
	g_assert_cmpint(cell(r, key, "monthly_needed"), ==, 100);
	post(f, "2026-05-10T00:00:00Z", "USD", "1000", "4000", 150);
	g_clear_object(&r);
	r = report(f, "savings_goals", "2026-05", "USD", NULL);
	g_assert_cmpint(cell(r, key, "remaining"), ==, 0);
	g_assert_cmpstr(text_cell(r, key, "forecast"), ==, "2026-05-31");
}

static void
test_savings_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureGoal) goal = NULL;
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureAccount) foreign = venture_account_new();
	g_autoptr(VentureDateRange) range = venture_context_parse_period(f->context, "2026-01", NULL);
	VentureReport *registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals");
	(void)data;
	g_assert_nonnull(registered);
	goal = savings_goal(f, "savings-account-id", account(f, "1000"), "100 USD", "20 USD");
	g_object_set(goal, "savings-account-id", account(f, "2000"), NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(goal), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(goal, "savings-account-id", account(f, "1000"), NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "savings-monthly", "20 EUR", &error));
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(goal), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(goal), "savings-monthly", "20 USD", &error));
	g_object_set(other, "name", "Other household", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(other), NULL, &error));
	g_object_set(foreign, "name", "Other reserve", "code", "OTHER-RESERVE", "organization-id", venture_entity_get_id(VENTURE_ENTITY(other)), NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(foreign), NULL, &error));
	g_object_set(goal, "savings-account-id", venture_entity_get_id(VENTURE_ENTITY(foreign)), NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(goal), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "ledger", FALSE);
	registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals");
	g_assert_nonnull(registered);
	r = venture_report_generate(registered, f->context, range, NULL, &error);
	g_assert_null(r);
	g_assert_nonnull(error);
	g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "goals", FALSE);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals"));
	venture_config_set_module_enabled(f->config, "ledger", TRUE);
	venture_config_set_module_enabled(f->config, "goals", TRUE);
}

static void
test_savings_shortfall(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureGoal) goal = savings_goal(f, "savings-account-id", account(f, "1000"), "100 USD", "25 USD");
	g_autoptr(VentureReportResult) r = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(goal)));
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) range = venture_context_parse_period(f->context, "2026-01", NULL);
	VentureReport *registered = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "savings_goals");
	(void)data;
	post(f, "2026-01-10T00:00:00Z", "USD", "6400", "1000", 2500);
	post(f, "2026-01-10T00:00:00Z", "EUR", "1000", "4000", 99999);
	r = report(f, "savings_goals", "2026-01", "USD", NULL);
	g_assert_cmpint(cell(r, key, "remaining"), ==, 12500);
	g_assert_cmpstr(text_cell(r, key, "forecast"), ==, "2026-06-30");
	g_clear_object(&r);
	g_object_set(goal, "status", VENTURE_GOAL_STATUS_PAUSED, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(goal), NULL, &error));
	r = report(f, "savings_goals", "2026-01", "USD", NULL);
	g_assert_null(venture_report_result_get_cell(r, 0, "forecast"));
	g_clear_object(&r);
	json_object_set_string_member(options, "as_of", "2026-01-15");
	r = venture_report_generate(registered, f->context, range, options, &error);
	g_assert_null(r);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

int
main(int argc, char **argv)
{
	static const gchar *const names[] = { "balance_sheet", "income_statement", "cash_flow", "general_ledger", "account_balances", "pnl_reconciliation" };
	guint i;
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/statements/savings-currency-query", test_savings_currency_query);
	g_test_add("/statements/savings-bound", Fixture, NULL, setup, test_savings_bound, teardown);
	g_test_add("/statements/savings-envelope", Fixture, NULL, setup, test_savings_envelope, teardown);
	g_test_add("/statements/savings-rounding", Fixture, NULL, setup, test_savings_rounding, teardown);
	g_test_add("/statements/savings-validation", Fixture, NULL, setup, test_savings_validation, teardown);
	g_test_add("/statements/savings-shortfall", Fixture, NULL, setup, test_savings_shortfall, teardown);
	g_test_add("/statements/savings-account", Fixture, NULL, setup, test_savings_account, teardown);
	g_test_add("/statements/net-worth", Fixture, NULL, setup, test_net_worth, teardown);
	g_test_add("/statements/debt-payoff", Fixture, NULL, setup, test_debt_payoff, teardown);
	g_test_add("/statements/wealth-cutoff", Fixture, NULL, setup, test_wealth_cutoff, teardown);
	g_test_add("/statements/debt-horizon", Fixture, NULL, setup, test_debt_horizon, teardown);
	g_test_add("/statements/debt-validation", Fixture, NULL, setup, test_debt_validation, teardown);
	g_test_add("/statements/debt-rounding", Fixture, NULL, setup, test_debt_rounding, teardown);
	g_test_add("/statements/debt-registration", Fixture, NULL, setup, test_debt_registration, teardown);
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
	g_test_add("/statements/cash-split-rounding", Fixture, NULL, setup, test_cash_split_rounding, teardown);
	g_test_add("/statements/cash-basis", Fixture, NULL, setup, test_cash_basis, teardown);
	g_test_add("/statements/cash-basis-write-off", Fixture, NULL, setup, test_cash_basis_write_off, teardown);
	g_test_add("/statements/cash-basis-refund", Fixture, NULL, setup, test_cash_basis_refund, teardown);
	return g_test_run();
}
