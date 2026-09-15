/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
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

static gint64
posted_balance(Fixture *f, const gchar *code)
{
	g_autoptr(GDateTime) as_of = g_date_time_new_utc(2026, 8, 31, 23, 59, 59);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		account(f, code), f->org, "USD", as_of, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balance);
	return balance->amount;
}

static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"equity_transaction"), !=, G_TYPE_INVALID);
}

static void
test_contribution_and_draw(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMoney) in_amount = venture_money_new_for_currency(100000, "USD");
	g_autoptr(VentureMoney) draw_amount = venture_money_new_for_currency(25000, "USD");
	g_autoptr(GDateTime) when = g_date_time_new_utc(2026, 8, 1, 0, 0, 0);
	g_autoptr(VentureEntity) contribution = NULL;
	g_autoptr(VentureEntity) draw = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	contribution = venture_capital_service_post(venture_capital_service_get(f->db), f->org,
		VENTURE_EQUITY_KIND_CONTRIBUTION, in_amount, when, "Owner contribution", 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(contribution);
	draw = venture_capital_service_post(venture_capital_service_get(f->db), f->org,
		VENTURE_EQUITY_KIND_DRAW, draw_amount, when, "Owner draw", 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(draw);
	g_assert_cmpint(posted_balance(f, "1000"), ==, 75000);
	g_assert_cmpint(posted_balance(f, "3000"), ==, -100000);
	g_assert_cmpint(posted_balance(f, "3100"), ==, 25000);
}

static void
test_loan_and_transfer(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMoney) proceed = venture_money_new_for_currency(40000, "USD");
	g_autoptr(VentureMoney) payment = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureMoney) move = venture_money_new_for_currency(5000, "USD");
	g_autoptr(GDateTime) when = g_date_time_new_utc(2026, 8, 2, 0, 0, 0);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	gint64 cash = account(f, "1000");
	(void)data;
	row = venture_capital_service_post(venture_capital_service_get(f->db), f->org,
		VENTURE_EQUITY_KIND_LOAN_PROCEED, proceed, when, "Loan", 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_clear_object(&row);
	row = venture_capital_service_post(venture_capital_service_get(f->db), f->org,
		VENTURE_EQUITY_KIND_LOAN_PAYMENT, payment, when, "Loan payment", 0, 0, NULL, &error);
	g_assert_no_error(error);
	g_clear_object(&row);
	row = venture_capital_service_post(venture_capital_service_get(f->db), f->org,
		VENTURE_EQUITY_KIND_TRANSFER, move, when, "Cash transfer", cash, cash, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(posted_balance(f, "1000"), ==, 30000);
	g_assert_cmpint(posted_balance(f, "2500"), ==, -30000);
}

static void
test_refuses_expense_dump(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GDateTime) when = g_date_time_new_utc(2026, 8, 3, 0, 0, 0);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	gint64 expense = account(f, "6900");
	(void)data;
	row = venture_capital_service_post(venture_capital_service_get(f->db), f->org,
		VENTURE_EQUITY_KIND_CONTRIBUTION, amount, when, "Dump", expense, 0, NULL, &error);
	g_assert_null(row);
	g_assert_nonnull(strstr(error->message, "expense"));
}

static void
test_generic_write_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_equity_transaction_new());
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(row, "organization-id", f->org, "kind", VENTURE_EQUITY_KIND_CONTRIBUTION,
		"amount", amount, "memo", "bypass", NULL);
	g_assert_false(venture_database_save(f->db, row, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureCapitalService"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/equity/records", test_records);
	g_test_add("/equity/contribution-draw", Fixture, NULL, setup, test_contribution_and_draw, teardown);
	g_test_add("/equity/loan-transfer", Fixture, NULL, setup, test_loan_and_transfer, teardown);
	g_test_add("/equity/refuse-expense", Fixture, NULL, setup, test_refuses_expense_dump, teardown);
	g_test_add("/equity/generic-write", Fixture, NULL, setup, test_generic_write_refused, teardown);
	return g_test_run();
}
