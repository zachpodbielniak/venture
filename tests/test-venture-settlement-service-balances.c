/*
 * test-venture-settlement-service-balances.c - Many customers' balances in
 * one read are each exactly the balance of that customer alone.
 *
 * The staff billing overview needs the balance of every customer at once.
 * Counted one customer at a time that is four statements per customer, on
 * the server's one loop. What breaks if this regresses: a customer's owed
 * amount in the overview differs from its own page, or the overview's
 * statements grow with the number of customers again.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>

#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	gint64 organization_id;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->database, record, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	(void)data;
	f->config = venture_config_new();
	f->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->organization_id = venture_context_get_default_organization_id(f->context);
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->database);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
}

static VentureEntity *
record_new(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	VentureEntity *record;

	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	record = g_object_new(type, NULL);
	venture_entity_set_organization_id(record, f->organization_id);
	return record;
}

static void
field(VentureEntity *record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(record, name, value, &error));
	g_assert_no_error(error);
}

static gint64
customer_new(Fixture *f, const gchar *name)
{
	g_autoptr(VentureCompany) customer = venture_company_new();

	g_object_set(customer, "name", name, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->organization_id);
	save(f, VENTURE_ENTITY(customer));
	return venture_entity_get_id(VENTURE_ENTITY(customer));
}

/* An invoice for @amount, sent, so that it moves the balance. */
static gint64
invoice_new(Fixture *f, gint64 customer, const gchar *number, const gchar *amount)
{
	g_autoptr(VentureEntity) invoice = record_new(f, "invoice");
	g_autoptr(VentureEntity) line = NULL;

	g_object_set(invoice, "number", number, "company-id", customer, NULL);
	field(invoice, "issued-at", "2026-07-01");
	field(invoice, "due-at", "2026-07-01");
	save(f, invoice);
	line = record_new(f, "invoice_line");
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work", "quantity", 1.0, NULL);
	field(line, "unit-price", amount);
	save(f, line);
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	save(f, invoice);
	return venture_entity_get_id(invoice);
}

static void
payment_new(Fixture *f, gint64 customer, gint64 invoice, const gchar *amount)
{
	g_autoptr(VentureEntity) payment = record_new(f, "payment");

	g_object_set(payment, "customer-id", customer, "invoice-id", invoice, "method", "manual", NULL);
	field(payment, "amount", amount);
	field(payment, "date", "2026-07-15");
	save(f, payment);
}

static GHashTable *
currencies_new(void)
{
	return g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
}

static void
currency_add(GHashTable *currencies, gint64 customer, const gchar *currency)
{
	g_hash_table_insert(currencies, g_memdup2(&customer, sizeof customer), g_strdup(currency));
}

/* Each customer's balance from the batch equals its balance alone, and
 * the batch reads each kind of record once, whatever the customer count. */
static void
test_balances_match(Fixture *f, gconstpointer data)
{
	VentureSettlementService *settlement = venture_settlement_service_get(f->database);
	g_autoptr(GHashTable) currencies = currencies_new(), balances = NULL, few = NULL;
	g_autoptr(GError) error = NULL;
	gint64 paid = customer_new(f, "Paid in part");
	gint64 owing = customer_new(f, "Owing");
	gint64 over = customer_new(f, "Paid over");
	gint64 quiet = customer_new(f, "Nothing yet");
	gint64 euro = customer_new(f, "Counted in euros");
	gint64 customers[] = { paid, owing, over, quiet, euro };
	guint64 before, many_statements, few_statements;
	guint i;

	(void)data;
	payment_new(f, paid, invoice_new(f, paid, "B-1", "100 USD"), "40 USD");
	invoice_new(f, owing, "B-2", "250 USD");
	invoice_new(f, owing, "B-3", "5 USD");
	payment_new(f, over, invoice_new(f, over, "B-4", "10 USD"), "30 USD");
	invoice_new(f, euro, "B-5", "70 USD");

	currency_add(currencies, paid, "USD");
	currency_add(currencies, owing, "USD");
	currency_add(currencies, over, "USD");
	currency_add(currencies, quiet, "USD");
	currency_add(currencies, euro, "EUR");
	before = venture_database_get_statement_count(f->database);
	balances = venture_settlement_service_customer_balances(settlement, f->organization_id, currencies, &error);
	many_statements = venture_database_get_statement_count(f->database) - before;
	g_assert_no_error(error);
	g_assert_nonnull(balances);
	g_assert_cmpuint(g_hash_table_size(balances), ==, G_N_ELEMENTS(customers));

	for (i = 0; i < G_N_ELEMENTS(customers); i++) {
		const gchar *currency = g_hash_table_lookup(currencies, &customers[i]);
		VentureMoney *batch = g_hash_table_lookup(balances, &customers[i]);
		g_autoptr(VentureMoney) alone = venture_settlement_service_customer_balance(settlement,
			f->organization_id, customers[i], NULL, currency, &error);

		g_assert_no_error(error);
		g_assert_nonnull(batch);
		g_assert_nonnull(alone);
		g_assert_cmpstr(venture_money_get_currency(batch), ==, venture_money_get_currency(alone));
		g_assert_cmpint(venture_money_get_amount(batch), ==, venture_money_get_amount(alone));
	}
	/* The fixture covers what it claims to. */
	g_assert_cmpint(venture_money_get_amount(g_hash_table_lookup(balances, &paid)), ==, 6000);
	g_assert_cmpint(venture_money_get_amount(g_hash_table_lookup(balances, &owing)), ==, 25500);
	g_assert_cmpint(venture_money_get_amount(g_hash_table_lookup(balances, &over)), ==, -2000);
	g_assert_cmpint(venture_money_get_amount(g_hash_table_lookup(balances, &quiet)), ==, 0);
	g_assert_cmpint(venture_money_get_amount(g_hash_table_lookup(balances, &euro)), ==, 0);

	{
		g_autoptr(GHashTable) none = currencies_new();
		few = venture_settlement_service_customer_balances(settlement, f->organization_id, none, &error);
		g_assert_no_error(error);
	}
	g_assert_cmpuint(g_hash_table_size(few), ==, 0);
	g_clear_pointer(&few, g_hash_table_unref);
	{
		g_autoptr(GHashTable) one = currencies_new();
		currency_add(one, paid, "USD");
		before = venture_database_get_statement_count(f->database);
		few = venture_settlement_service_customer_balances(settlement, f->organization_id, one, &error);
		few_statements = venture_database_get_statement_count(f->database) - before;
		g_assert_no_error(error);
	}
	g_test_message("statements: %" G_GUINT64_FORMAT " for one customer, %" G_GUINT64_FORMAT " for five",
		few_statements, many_statements);
	g_assert_cmpuint(many_statements, ==, few_statements);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/settlement/balances-match", Fixture, NULL, set_up, test_balances_match, tear_down);
	return g_test_run();
}
