/*
 * test-line-accounts.c - Invoice lines and expenses that name their accounts.
 *
 * A migrated ledger credits several income accounts, keeps discounts on
 * their own account and debits each expense where the source booked it.
 * These are the posting options that let a replayed history reproduce the
 * source trial balance account for account.
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
	gint64 customer;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(VentureEntity *record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(record, name, value, &error));
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
account(Fixture *f, const gchar *code, const gchar *name, const gchar *kind)
{
	g_autoptr(VentureEntity) a = record(f, "account");
	g_object_set(a, "code", code, "name", name, "active", TRUE, NULL);
	field(a, "kind", kind);
	save(f, a);
	return venture_entity_get_id(a);
}

static gint64
account_by_code(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) found = NULL;
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, code, NULL));
	found = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(found);
	if (found->len == 0)
		return 0;
	return venture_entity_get_id(g_ptr_array_index(found, 0));
}

/* Signed balance in cents, debit positive; zero for a missing account. */
static gint64
balance(Fixture *f, gint64 account_id)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2030-01-01T00:00:00Z", NULL);
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GError) error = NULL;
	if (account_id == 0)
		return 0;
	value = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		account_id, f->org, "USD", when, &error);
	g_assert_no_error(error);
	g_assert_nonnull(value);
	return venture_money_get_amount(value);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	customer = record(f, "company");
	g_object_set(customer, "name", "Acme Widgets", NULL);
	save(f, customer);
	f->customer = venture_entity_get_id(customer);
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

static VentureEntity *
draft(Fixture *f, const gchar *number)
{
	VentureEntity *invoice = record(f, "invoice");
	g_object_set(invoice, "number", number, "company-id", f->customer, NULL);
	field(invoice, "issued-at", "2025-03-04");
	field(invoice, "due-at", "2025-04-03");
	save(f, invoice);
	return invoice;
}

static void
line(Fixture *f, VentureEntity *invoice, const gchar *price, gint64 discount_percent,
	gint64 income_account, gint64 discount_account)
{
	g_autoptr(VentureEntity) l = record(f, "invoice_line");
	g_object_set(l, "invoice-id", venture_entity_get_id(invoice), "description", "Work",
		"quantity", 1.0, "discount-percent", discount_percent,
		"income-account-id", income_account, "discount-account-id", discount_account, NULL);
	field(l, "unit-price", price);
	save(f, l);
}

static void
issue(Fixture *f, VentureEntity *invoice, const gchar *state, const gchar *date)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601(date, NULL);
	g_autoptr(VentureEntity) stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE,
		venture_entity_get_id(invoice), NULL);
	g_autoptr(GError) error = NULL;
	g_assert_nonnull(stored);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(stored), state, when, NULL, &error));
	g_assert_no_error(error);
}

/* Without either account a line posts exactly as before: the whole net to
 * the organization's income account. */
static void
test_default_unchanged(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = draft(f, "A-1");
	(void)data;
	line(f, invoice, "120 USD", 0, 0, 0);
	issue(f, invoice, "sent", "2025-03-04T00:00:00Z");
	g_assert_cmpint(balance(f, account_by_code(f, "4000")), ==, -12000);
	g_assert_cmpint(balance(f, account_by_code(f, "1100")), ==, 12000);
}

/* Each line credits the account it names; the rest goes to income. */
static void
test_income_accounts(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = draft(f, "A-2");
	gint64 fees = account(f, "4600", "Late fee income", "income");
	gint64 rebilled = account(f, "6150", "Advertising", "expense");
	(void)data;
	line(f, invoice, "500 USD", 0, 0, 0);
	line(f, invoice, "7 USD", 0, fees, 0);
	line(f, invoice, "40 USD", 0, rebilled, 0);
	issue(f, invoice, "sent", "2025-03-04T00:00:00Z");
	g_assert_cmpint(balance(f, account_by_code(f, "4000")), ==, -50000);
	g_assert_cmpint(balance(f, fees), ==, -700);
	g_assert_cmpint(balance(f, rebilled), ==, -4000);
	g_assert_cmpint(balance(f, account_by_code(f, "1100")), ==, 54700);
}

/* A discount account keeps the discount apart: income is credited before
 * the discount and the discount is a debit of its own. A void reverses
 * every leg. */
static void
test_discount_account_and_void(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = draft(f, "A-3");
	gint64 sales = account(f, "4111", "Services", "income");
	gint64 discount = account(f, "4911", "Discounts given", "income");
	(void)data;
	line(f, invoice, "800 USD", 25, sales, discount);
	issue(f, invoice, "sent", "2025-03-04T00:00:00Z");
	g_assert_cmpint(balance(f, sales), ==, -80000);
	g_assert_cmpint(balance(f, discount), ==, 20000);
	g_assert_cmpint(balance(f, account_by_code(f, "1100")), ==, 60000);
	g_assert_cmpint(balance(f, account_by_code(f, "4000")), ==, 0);
	issue(f, invoice, "void", "2025-03-10T00:00:00Z");
	g_assert_cmpint(balance(f, sales), ==, 0);
	g_assert_cmpint(balance(f, discount), ==, 0);
	g_assert_cmpint(balance(f, account_by_code(f, "1100")), ==, 0);
}

/* An inactive account, or one from another organization, is refused and
 * the invoice stays a draft. */
static void
test_inactive_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) invoice = draft(f, "A-4");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) closed = record(f, "account");
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2025-03-04T00:00:00Z", NULL);
	g_autoptr(GError) error = NULL;
	gint status = -1;
	(void)data;
	g_object_set(closed, "code", "4701", "name", "Closed income", "active", FALSE, NULL);
	field(closed, "kind", "income");
	save(f, closed);
	line(f, invoice, "10 USD", 0, venture_entity_get_id(closed), 0);
	stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), NULL);
	g_assert_false(venture_settlement_service_transition(venture_settlement_service_get(f->db),
		VENTURE_INVOICE(stored), "sent", when, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "active"));
	g_clear_object(&stored);
	stored = venture_database_get(f->db, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), NULL);
	g_object_get(stored, "status", &status, NULL);
	g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_DRAFT);
	g_assert_cmpint(balance(f, account_by_code(f, "1100")), ==, 0);
}

/* An expense naming its account debits it, whatever its category maps to. */
static void
test_expense_account(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) expense = record(f, "expense");
	gint64 software = account(f, "6421", "Software", "expense");
	gint64 checking = account(f, "1011", "Checking", "asset");
	(void)data;
	g_object_set(expense, "description", "Editor licence", "vendor", "Toolsmith",
		"category", "office", "expense-account-id", software, "cash-account-id", checking, NULL);
	field(expense, "amount", "64.25 USD");
	field(expense, "occurred-at", "2025-05-06");
	save(f, expense);
	g_assert_cmpint(balance(f, software), ==, 6425);
	g_assert_cmpint(balance(f, checking), ==, -6425);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/line-accounts/default-unchanged", Fixture, NULL, setup, test_default_unchanged, teardown);
	g_test_add("/line-accounts/income-accounts", Fixture, NULL, setup, test_income_accounts, teardown);
	g_test_add("/line-accounts/discount-account-and-void", Fixture, NULL, setup, test_discount_account_and_void, teardown);
	g_test_add("/line-accounts/inactive-refused", Fixture, NULL, setup, test_inactive_refused, teardown);
	g_test_add("/line-accounts/expense-account", Fixture, NULL, setup, test_expense_account, teardown);
	return g_test_run();
}
