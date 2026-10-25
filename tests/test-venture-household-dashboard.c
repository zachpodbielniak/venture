/*
 * test-venture-household-dashboard.c - the household's own dashboard
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spending by category, the cash forecast and the household dashboard
 * template, under the private-account contract: a member's unshared
 * personal account stays out of another member's figures until the owner
 * shares it with the household.
 */

#include <venture.h>
#include <string.h>

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org, owner, other, equity;
	gint64 joint, personal;
} Fixture;

static void
save(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	gboolean saved = venture_database_save(f->db, entity, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(saved);
}

static gint64
member(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", name, "active", TRUE, NULL);
	g_autoptr(VentureEntity) membership = NULL;
	save(f, user);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", venture_entity_get_id(user), "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_OWNER, NULL);
	save(f, membership);
	return venture_entity_get_id(user);
}

static gint64
ledger(Fixture *f, const gchar *code, gint kind, gint64 owner)
{
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_ACCOUNT, "organization-id", f->org,
		"code", code, "name", code, "kind", kind, "active", TRUE, "private-owner-id", owner, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

/* A checking account: the ledger account and the bank account over it. */
static gint64
checking(Fixture *f, const gchar *code, gint64 owner)
{
	gint64 id = ledger(f, code, VENTURE_ACCOUNT_KIND_ASSET, owner);
	g_autoptr(VentureEntity) bank = g_object_new(VENTURE_TYPE_BANK_ACCOUNT, "organization-id", f->org,
		"name", code, "account-id", id, "currency", "USD", "kind", VENTURE_BANK_ACCOUNT_CHECKING, NULL);
	save(f, bank);
	return id;
}

static void
deposit(Fixture *f, gint64 account, gint64 cents, const gchar *when)
{
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) date = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(cents, "USD");
	g_autoptr(GError) error = NULL;
	VentureJournalLine *line;
	g_object_set(header, "organization-id", f->org, "source-type", "organization",
		"source-id", f->org, "occurred-at", date, "currency", "USD", NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", f->equity, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), header, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

static void
spend(Fixture *f, const gchar *what, const gchar *category, gint64 cents, gint64 cash, const gchar *when)
{
	g_autoptr(VentureMoney) amount = venture_money_new(cents, "USD", 2);
	g_autoptr(GDateTime) date = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_EXPENSE, "organization-id", f->org,
		"description", what, "amount", amount, "occurred-at", date, "category", category,
		"cash-account-id", cash, NULL);
	save(f, row);
}

static void
monthly_bill(Fixture *f, const gchar *name, const gchar *start, const gchar *amount)
{
	g_autoptr(VentureEntity) row = g_object_new(VENTURE_TYPE_RECURRING_SCHEDULE, "organization-id", f->org,
		"name", name, "timezone", "UTC", NULL);
	g_autofree gchar *template = g_strdup_printf("{\"amount\":\"%s\"}", amount);
	g_autoptr(GError) error = NULL;
	g_object_set(row, "template", template, NULL);
	g_assert_true(venture_entity_set_field_from_string(row, "kind", "expense", &error));
	g_assert_true(venture_entity_set_field_from_string(row, "frequency", "monthly", &error));
	g_assert_true(venture_entity_set_field_from_string(row, "start-at", start, &error));
	g_assert_true(venture_entity_set_field_from_string(row, "projection-only", "true", &error));
	g_assert_no_error(error);
	save(f, row);
}

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
	f->owner = member(f, "household-owner");
	f->other = member(f, "household-partner");
	f->equity = ledger(f, "opening-equity", VENTURE_ACCOUNT_KIND_EQUITY, 0);
	f->joint = checking(f, "joint-checking", 0);
	f->personal = checking(f, "owner-checking", f->owner);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureReportResult *
run_as(Fixture *f, gint64 user, const gchar *name, const gchar *period_text, const gchar *as_of)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = venture_context_parse_period(f->context, period_text, &error);
	g_autoptr(JsonObject) options = json_object_new();
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	VentureAuthPrincipal actor = { 0 };
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureReportResult *result;
	g_assert_no_error(error);
	g_assert_nonnull(report);
	if (as_of != NULL)
		json_object_set_string_member(options, "as_of", as_of);
	actor.authenticated = TRUE;
	actor.role = VENTURE_USER_ROLE_EDITOR;
	actor.user_id = user;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &actor);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gint64
cents(VentureReportResult *r, const gchar *key_column, const gchar *key, const gchar *column)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
	{
		const GValue *k = venture_report_result_get_cell(r, i, key_column);
		if (k != NULL && g_strcmp0(g_value_get_string(k), key) == 0)
		{
			const GValue *v = venture_report_result_get_cell(r, i, column);
			g_assert_nonnull(v);
			return venture_money_get_amount(g_value_get_boxed(v));
		}
	}
	g_assert_not_reached();
	return 0;
}

static gboolean
has_key(VentureReportResult *r, const gchar *key_column, const gchar *key)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
	{
		const GValue *k = venture_report_result_get_cell(r, i, key_column);
		if (k != NULL && g_strcmp0(g_value_get_string(k), key) == 0)
			return TRUE;
	}
	return FALSE;
}

static void
share_personal(Fixture *f)
{
	g_autoptr(VentureEntity) row = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, f->personal, NULL);
	VentureAuthPrincipal actor = { 0 };
	g_autoptr(VentureAccessScope) scope = NULL;
	actor.authenticated = TRUE;
	actor.role = VENTURE_USER_ROLE_EDITOR;
	actor.user_id = f->owner;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &actor);
	g_object_set(row, "household-shared", TRUE, NULL);
	save(f, row);
}

/* Spending by category for the period, biggest first, never another
 * member's unshared spending. */
static void
test_spending(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) mine = NULL, theirs = NULL, shared = NULL;
	(void)unused;
	spend(f, "Weekly shop", "Groceries", 8000, f->joint, "2026-03-03T12:00:00Z");
	spend(f, "Corner shop", "Groceries", 2000, f->joint, "2026-03-09T12:00:00Z");
	spend(f, "Bus pass", "Transport", 5000, f->joint, "2026-03-04T12:00:00Z");
	spend(f, "Concert", "Fun", 9000, f->personal, "2026-03-05T12:00:00Z");
	spend(f, "February shop", "Groceries", 7000, f->joint, "2026-02-20T12:00:00Z");

	mine = run_as(f, f->owner, "household_spending", "2026-03", NULL);
	g_assert_cmpint(cents(mine, "category", "Groceries", "spent"), ==, 10000);
	g_assert_cmpint(cents(mine, "category", "Fun", "spent"), ==, 9000);
	g_assert_cmpint(cents(mine, "category", "Transport", "spent"), ==, 5000);
	g_assert_cmpstr(g_value_get_string(venture_report_result_get_cell(mine, 0, "category")), ==, "Groceries");

	theirs = run_as(f, f->other, "household_spending", "2026-03", NULL);
	g_assert_false(has_key(theirs, "category", "Fun"));
	g_assert_cmpint(cents(theirs, "category", "Groceries", "spent"), ==, 10000);

	share_personal(f);
	shared = run_as(f, f->other, "household_spending", "2026-03", NULL);
	g_assert_cmpint(cents(shared, "category", "Fun", "spent"), ==, 9000);
}

/* Today's spendable balance carried forward week by week through what is
 * due; another member's unshared checking account is not counted. */
static void
test_cash_forecast(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) mine = NULL, theirs = NULL;
	guint rows;
	(void)unused;
	deposit(f, f->joint, 200000, "2026-05-01T00:00:00Z");
	deposit(f, f->personal, 50000, "2026-05-01T00:00:00Z");
	monthly_bill(f, "Rent", "2026-05-15", "1000.00 USD");

	mine = run_as(f, f->owner, "household_cash_forecast", "this_month", "2026-05-04");
	rows = venture_report_result_get_row_count(mine);
	g_assert_cmpuint(rows, ==, 13);
	g_assert_cmpint(cents(mine, "week", "2026-05-04", "start"), ==, 250000);
	g_assert_cmpint(cents(mine, "week", "2026-05-11", "out"), ==, 100000);
	g_assert_cmpint(cents(mine, "week", "2026-05-11", "end"), ==, 150000);
	g_assert_cmpint(cents(mine, "week", "2026-06-15", "end"), ==, 50000);

	theirs = run_as(f, f->other, "household_cash_forecast", "this_month", "2026-05-04");
	g_assert_cmpint(cents(theirs, "week", "2026-05-04", "start"), ==, 200000);
	g_assert_cmpint(cents(theirs, "week", "2026-05-11", "end"), ==, 100000);
}

/* Money in, spent and left over per month in household words, from the
 * same monthly series, under the same privacy. */
static void
test_months(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureReportResult) mine = NULL, theirs = NULL;
	g_autofree gchar *columns = NULL;
	g_auto(GStrv) keys = NULL;
	(void)unused;
	spend(f, "Weekly shop", "Groceries", 8000, f->joint, "2026-03-03T12:00:00Z");
	spend(f, "Concert", "Fun", 9000, f->personal, "2026-03-05T12:00:00Z");
	mine = run_as(f, f->owner, "household_months", "2026-03", NULL);
	keys = venture_report_result_get_column_keys(mine);
	columns = g_strjoinv(",", keys);
	g_assert_cmpstr(columns, ==, "month,currency,money_in,spent,left_over");
	g_assert_cmpint(cents(mine, "month", "Mar 2026", "spent"), ==, 17000);
	g_assert_cmpint(cents(mine, "month", "Mar 2026", "left_over"), ==, -17000);
	theirs = run_as(f, f->other, "household_months", "2026-03", NULL);
	g_assert_cmpint(cents(theirs, "month", "Mar 2026", "spent"), ==, 8000);
}

/* The household dashboard is a template like any other, two columns that
 * stack on a phone, built from reports that obey the same privacy. */
static void
test_dashboard_template(Fixture *f, gconstpointer unused)
{
	gsize n = 0, i;
	const VentureDashboardTemplate *templates = venture_dashboard_get_templates(&n);
	const VentureDashboardTemplate *household = NULL;
	(void)unused; (void)f;
	for (i = 0; i < n; i++)
		if (g_strcmp0(templates[i].name, "household") == 0)
			household = &templates[i];
	g_assert_nonnull(household);
	g_assert_nonnull(strstr(household->definition, "household_spending"));
	g_assert_nonnull(strstr(household->definition, "household_cash_forecast"));
	g_assert_nonnull(strstr(household->definition, "household_months"));
	g_assert_nonnull(strstr(household->definition, "net_worth"));
	g_assert_nonnull(strstr(household->definition, "\"two_columns\""));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/household-dashboard/spending", Fixture, NULL, setup, test_spending, teardown);
	g_test_add("/household-dashboard/cash-forecast", Fixture, NULL, setup, test_cash_forecast, teardown);
	g_test_add("/household-dashboard/months", Fixture, NULL, setup, test_months, teardown);
	g_test_add("/household-dashboard/template", Fixture, NULL, setup, test_dashboard_template, teardown);
	return g_test_run();
}
