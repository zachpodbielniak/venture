/*
 * test-inventory-currencies.c - FIFO stock whose cost is in several currencies
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One organization keeps its books in GOLD and also buys stock with TICKET
 * (no decimals), which may be valued, a separate book, or memo. Cost
 * layers carry one currency each; everything that sums them -- an issue, a
 * craft, a transfer, the valuation and the inventory report -- keeps one
 * total per currency and never adds across. An issue posts each currency
 * by its own treatment in one call; a made unit whose inputs cost two
 * currencies gets sibling layers sharing a lot, so it leaves with both.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*db;
	VentureContext	*context;
	gint64		 org;
	gint64		 venture;
} Fixture;

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(gpointer record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *entity;

	entity = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(entity);
	venture_entity_set_organization_id(entity, f->org);
	return entity;
}

static void
define_currency(Fixture *f, const gchar *code, gint64 exponent, const gchar *treatment)
{
	g_autoptr(VentureEntity) currency = record(f, "currency");

	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", treatment);
	save(f, currency);
}

/* One @from is worth @numerator/@denominator of @to from 1 January 2026. */
static void
rate(Fixture *f, const gchar *from, const gchar *to, gint64 numerator, gint64 denominator)
{
	g_autoptr(VentureEntity) row = record(f, "exchange_rate");

	g_object_set(row, "from-currency", from, "to-currency", to,
		"rate-numerator", numerator, "rate-denominator", denominator, NULL);
	field(row, "effective-at", "2026-01-01");
	save(f, row);
}

/* The organization, its venture and its currencies, over @f->config. */
static void
seed(Fixture *f, const gchar *ticket)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();
	g_autoptr(VentureEntity) organization = NULL;

	venture_currency_clear_registered();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(venture, "name", "Evermoor", "venture-type", "books", "organization-id", f->org, NULL);
	save(f, venture);
	f->venture = venture_entity_get_id(VENTURE_ENTITY(venture));
	define_currency(f, "GOLD", 4, "valued");
	define_currency(f, "TICKET", 0, ticket != NULL ? ticket : "valued");
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, &error);
	g_assert_no_error(error);
	g_object_set(organization, "default-currency", "GOLD", NULL);
	save(f, organization);
}

static void
setup(Fixture *f, gconstpointer data)
{
	f->config = venture_config_new();
	seed(f, data);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_currency_clear_registered();
}

/* --- Stock -------------------------------------------------------------- */

static gint64
product(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) row = record(f, "product");

	g_object_set(row, "name", name, "venture-id", f->venture, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

static gint64
item(Fixture *f, gint64 product_id, const gchar *sku)
{
	g_autoptr(VentureEntity) row = record(f, "inventory_item");

	g_object_set(row, "product-id", product_id, "sku", sku, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

static VentureMoney *
money_of(const gchar *text)
{
	VentureMoney *money = venture_money_from_string(text, NULL, NULL);

	g_assert_nonnull(money);
	return money;
}

static VentureInventoryService *
inventory(Fixture *f)
{
	return venture_inventory_service_get(f->db);
}

/* @quantity units at @unit_cost, arriving on @date. */
static void
receive(Fixture *f, gint64 item_id, gint64 quantity, const gchar *unit_cost, const gchar *date)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) cost = money_of(unit_cost);
	g_autoptr(GDateTime) when = venture_time_from_string(date, NULL);

	g_assert_true(venture_inventory_service_receive(inventory(f), item_id, quantity, cost, when,
		0, "seed", NULL, &error));
	g_assert_no_error(error);
}

static GPtrArray *
issue(Fixture *f, gint64 item_id, gint64 quantity)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) when = venture_time_from_string("2026-03-10", NULL);
	GPtrArray *costs = NULL;

	g_assert_true(venture_inventory_service_issue(inventory(f), item_id, quantity, when,
		"inventory_txn", 0, NULL, &costs, &error));
	g_assert_no_error(error);
	g_assert_nonnull(costs);
	return costs;
}

/* The amount in @currency, in minor units; -1 when there is none. */
static gint64
amount_in(GPtrArray *totals, const gchar *currency)
{
	const VentureMoney *money = venture_money_totals_lookup(totals, currency);

	return money != NULL ? venture_money_get_amount(money) : -1;
}

static GPtrArray *
layers_of(Fixture *f, gint64 item_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "inventory-item-id", VENTURE_FILTER_OP_EQ, item_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

/* --- The books ---------------------------------------------------------- */

/* The inventory transactions an item has had, newest last. */
static GPtrArray *
txns_of(Fixture *f, gint64 item_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_TXN);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "inventory-item-id", VENTURE_FILTER_OP_EQ, item_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static GPtrArray *
journals_for(Fixture *f, const gchar *type, gint64 id)
{
	g_autoptr(GError) error = NULL;
	GPtrArray *journals;

	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		type, id, f->org, &error);
	g_assert_no_error(error);
	g_assert_nonnull(journals);
	return journals;
}

static GPtrArray *
lines_of(Fixture *f, gpointer journal)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(VENTURE_ENTITY(journal)), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static gchar *
currency_of(gpointer journal)
{
	gchar *currency = NULL;

	g_object_get(journal, "currency", &currency, NULL);
	return currency;
}

/* What the debit lines of @journal come to, in its book amounts. */
static gint64
debits_of(Fixture *f, gpointer journal)
{
	g_autoptr(GPtrArray) lines = lines_of(f, journal);
	gint64 total = 0;
	guint i;

	for (i = 0; i < lines->len; i++)
	{
		g_autoptr(VentureMoney) book = NULL;
		gint side = 0;

		g_object_get(g_ptr_array_index(lines, i), "side", &side, "book-amount", &book, NULL);
		if (side == VENTURE_LEDGER_SIDE_DEBIT)
			total += venture_money_get_amount(book);
	}
	return total;
}

/*
 * Every currency's books balance: over every journal line the organization
 * has, debits equal credits in each book currency. This is the trial
 * balance per currency, read straight off the lines.
 */
static void
assert_books_balance(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GHashTable) sums = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GError) error = NULL;
	GHashTableIter iter;
	gpointer value;
	guint i;

	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	lines = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < lines->len; i++)
	{
		g_autoptr(VentureMoney) book = NULL;
		gint side = 0;
		gint64 *sum;

		g_object_get(g_ptr_array_index(lines, i), "side", &side, "book-amount", &book, NULL);
		g_assert_nonnull(book);
		sum = g_hash_table_lookup(sums, venture_money_get_currency(book));
		if (sum == NULL)
		{
			sum = g_new0(gint64, 1);
			g_hash_table_insert(sums, g_strdup(venture_money_get_currency(book)), sum);
		}
		*sum += side == VENTURE_LEDGER_SIDE_DEBIT ? venture_money_get_amount(book)
			: -venture_money_get_amount(book);
	}
	g_hash_table_iter_init(&iter, sums);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_assert_cmpint(*(gint64 *)value, ==, 0);
}

/* The issue transaction an item's last movement wrote. */
static gint64
last_txn(Fixture *f, gint64 item_id)
{
	g_autoptr(GPtrArray) txns = txns_of(f, item_id);

	g_assert_cmpuint(txns->len, >, 0);
	return venture_entity_get_id(g_ptr_array_index(txns, txns->len - 1));
}

/* ------------------------------------------------------------------------ */

/*
 * Finding 3d: an issue that crosses a GOLD layer into a TICKET layer. It
 * used to be refused ("Cannot combine GOLD and TICKET") because the slices
 * were added into one amount. Now it costs 2 GOLD and 5 TICKET, and posts
 * a GOLD pair and a TICKET pair (no rate: a journal of its own), each
 * balanced by itself.
 */
static void
test_issue_across_currencies(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) layers = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	gint64 prize;
	gint64 remaining = 0;

	(void)data;
	prize = item(f, product(f, "Prize"), "PRIZE");
	receive(f, prize, 2, "1.0000 GOLD", "2026-03-01");
	receive(f, prize, 2, "5 TICKET", "2026-03-02");

	costs = issue(f, prize, 3);
	g_assert_cmpuint(costs->len, ==, 2);
	g_assert_cmpint(amount_in(costs, "GOLD"), ==, 20000);
	g_assert_cmpint(amount_in(costs, "TICKET"), ==, 5);

	/* The GOLD layer is used up, one TICKET unit is left. */
	layers = layers_of(f, prize);
	g_object_get(g_ptr_array_index(layers, 1), "remaining-qty", &remaining, NULL);
	g_assert_cmpint(remaining, ==, 1);

	journals = journals_for(f, "inventory_txn", last_txn(f, prize));
	g_assert_cmpuint(journals->len, ==, 2);
	first = currency_of(g_ptr_array_index(journals, 0));
	second = currency_of(g_ptr_array_index(journals, 1));
	g_assert_cmpstr(first, ==, "GOLD");
	g_assert_cmpstr(second, ==, "TICKET");
	g_assert_cmpint(debits_of(f, g_ptr_array_index(journals, 0)), ==, 20000);
	g_assert_cmpint(debits_of(f, g_ptr_array_index(journals, 1)), ==, 5);
	assert_books_balance(f);
}

/*
 * FIFO is by arrival, whatever the currency: TICKET, then GOLD, then
 * TICKET again, and an issue of three takes one of each in that order.
 * What breaks: grouping layers by currency before walking them, which
 * sells the newest gold before the oldest tickets.
 */
static void
test_fifo_order_across_currencies(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(GPtrArray) more = NULL;
	gint64 prize;

	(void)data;
	prize = item(f, product(f, "Prize"), "PRIZE");
	receive(f, prize, 2, "4 TICKET", "2026-03-01");
	receive(f, prize, 1, "3.0000 GOLD", "2026-03-02");
	receive(f, prize, 5, "7 TICKET", "2026-03-03");

	costs = issue(f, prize, 2);
	g_assert_cmpuint(costs->len, ==, 1);
	g_assert_cmpint(amount_in(costs, "TICKET"), ==, 8);

	more = issue(f, prize, 2);
	g_assert_cmpint(amount_in(more, "GOLD"), ==, 30000);
	g_assert_cmpint(amount_in(more, "TICKET"), ==, 7);
}

/*
 * Finding 3e: a GOLD sale of a unit bought with tickets. The sale posts
 * GOLD cash and sales; the issue posts TICKET cost of goods sold against
 * TICKET inventory -- nothing refused, nothing silently missing. With a
 * TICKET-to-GOLD rate the same cost is valued into a GOLD journal, so the
 * gold books carry the cost of what they sold.
 */
static void
test_gold_sale_ticket_unit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GPtrArray) sale_journals = NULL;
	g_autoptr(GPtrArray) issue_journals = NULL;
	g_autoptr(GPtrArray) valued = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autofree gchar *sale_currency = NULL;
	g_autofree gchar *issue_currency = NULL;
	g_autofree gchar *valued_currency = NULL;
	gint64 prize_product;
	gint64 prize;

	(void)data;
	prize_product = product(f, "Prize");
	prize = item(f, prize_product, "PRIZE");
	receive(f, prize, 2, "50 TICKET", "2026-03-01");

	sale = record(f, "sale");
	g_object_set(sale, "venture-id", f->venture, "product-id", prize_product, "quantity", (gint64)1, NULL);
	field(sale, "occurred-at", "2026-03-05");
	field(sale, "gross", "15 GOLD");
	save(f, sale);

	sale_journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(sale_journals->len, ==, 1);
	sale_currency = currency_of(g_ptr_array_index(sale_journals, 0));
	g_assert_cmpstr(sale_currency, ==, "GOLD");
	g_assert_cmpint(debits_of(f, g_ptr_array_index(sale_journals, 0)), ==, 150000);

	issue_journals = journals_for(f, "inventory_txn", last_txn(f, prize));
	g_assert_cmpuint(issue_journals->len, ==, 1);
	issue_currency = currency_of(g_ptr_array_index(issue_journals, 0));
	g_assert_cmpstr(issue_currency, ==, "TICKET");
	g_assert_cmpint(debits_of(f, g_ptr_array_index(issue_journals, 0)), ==, 50);
	assert_books_balance(f);

	/* With a rate, 50 TICKET at 1/10 GOLD each is 5 GOLD of cost. */
	rate(f, "TICKET", "GOLD", 1, 10);
	second = record(f, "sale");
	g_object_set(second, "venture-id", f->venture, "product-id", prize_product, "quantity", (gint64)1, NULL);
	field(second, "occurred-at", "2026-03-06");
	field(second, "gross", "15 GOLD");
	save(f, second);
	valued = journals_for(f, "inventory_txn", last_txn(f, prize));
	g_assert_cmpuint(valued->len, ==, 1);
	valued_currency = currency_of(g_ptr_array_index(valued, 0));
	g_assert_cmpstr(valued_currency, ==, "GOLD");
	g_assert_cmpint(debits_of(f, g_ptr_array_index(valued, 0)), ==, 50000);
	lines = lines_of(f, g_ptr_array_index(valued, 0));
	{
		g_autoptr(VentureMoney) original = NULL;

		g_object_get(g_ptr_array_index(lines, 0), "amount", &original, NULL);
		g_assert_cmpstr(venture_money_get_currency(original), ==, "TICKET");
		g_assert_cmpint(venture_money_get_amount(original), ==, 50);
	}
	assert_books_balance(f);
}

/*
 * Finding 3f: a posted session gives its goods a zero GOLD layer; bought
 * units of the same item cost TICKET. An issue across both, and a craft
 * drawing both, used to be refused though the gold part was zero. Now the
 * zero is a zero: nothing posts for it, and the made unit carries only
 * TICKET cost -- no zero GOLD layer beside it.
 */
static void
test_zero_cost_beside_costed(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) made = NULL;
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureMoney) zero_unit = NULL;
	g_autofree gchar *currency = NULL;
	VentureInventoryDraw draw;
	gint64 herb;
	gint64 potion;
	guint i;

	(void)data;
	herb = item(f, product(f, "Herb"), "HERB");
	potion = item(f, product(f, "Potion"), "POTION");

	/* What a session post does: produce with no draws. */
	g_assert_true(venture_inventory_service_produce(inventory(f), NULL, 0, herb, 2, NULL,
		"session:1", NULL, NULL, NULL, &error));
	g_assert_no_error(error);
	layers = layers_of(f, herb);
	g_object_get(g_ptr_array_index(layers, 0), "unit-cost", &zero_unit, NULL);
	g_assert_cmpstr(venture_money_get_currency(zero_unit), ==, "GOLD");
	g_assert_true(venture_money_is_zero(zero_unit));
	receive(f, herb, 4, "3 TICKET", "2027-01-01");

	costs = issue(f, herb, 3);
	g_assert_cmpint(amount_in(costs, "GOLD"), ==, 0);
	g_assert_cmpint(amount_in(costs, "TICKET"), ==, 3);
	journals = journals_for(f, "inventory_txn", last_txn(f, herb));
	g_assert_cmpuint(journals->len, ==, 1);
	currency = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(currency, ==, "TICKET");

	/* A fresh zero-cost batch, then a craft drawing it and three TICKET
	 * herbs: the potion costs 9 TICKET and nothing in GOLD. */
	g_assert_true(venture_inventory_service_produce(inventory(f), NULL, 0, herb, 1, NULL,
		"session:2", NULL, NULL, NULL, &error));
	g_assert_no_error(error);
	draw.inventory_item_id = herb;
	draw.quantity = 4;
	g_assert_true(venture_inventory_service_produce(inventory(f), &draw, 1, potion, 1, NULL,
		"recipe:1", NULL, &txn, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(amount_in(made, "TICKET"), ==, 9);
	g_clear_pointer(&layers, g_ptr_array_unref);
	layers = layers_of(f, potion);
	g_assert_cmpuint(layers->len, ==, 1);
	for (i = 0; i < layers->len; i++)
	{
		g_autoptr(VentureMoney) unit = NULL;

		g_object_get(g_ptr_array_index(layers, i), "unit-cost", &unit, NULL);
		g_assert_cmpstr(venture_money_get_currency(unit), ==, "TICKET");
	}
	assert_books_balance(f);
}

/*
 * A craft whose inputs cost GOLD and TICKET, then the output sold: the
 * made units carry sibling layers, one set per currency, all naming the
 * craft's transaction as their lot, each currency split exactly. Selling
 * one unit gives up both currencies' shares at once, and selling all
 * three gives up exactly what went in -- 1 GOLD and 50 TICKET to the
 * minor unit. What breaks: a craft refused for two currencies (as it
 * was), a rounded layer that loses a copper or a ticket, or siblings
 * consumed as separate units so the first sale takes only gold.
 */
static void
test_craft_two_currencies_then_sell(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) made = NULL;
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GPtrArray) one = NULL;
	g_autoptr(GPtrArray) rest = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	VentureInventoryDraw draws[2];
	gint64 dust;
	gint64 token;
	gint64 charm;
	guint i;

	(void)data;
	dust = item(f, product(f, "Dust"), "DUST");
	token = item(f, product(f, "Token"), "TOKEN");
	charm = item(f, product(f, "Charm"), "CHARM");
	receive(f, dust, 4, "0.2500 GOLD", "2026-03-01");
	receive(f, token, 10, "5 TICKET", "2026-03-01");

	draws[0].inventory_item_id = dust;
	draws[0].quantity = 4;
	draws[1].inventory_item_id = token;
	draws[1].quantity = 10;
	g_assert_true(venture_inventory_service_produce(inventory(f), draws, 2, charm, 3, NULL,
		"recipe:9", NULL, &txn, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(amount_in(made, "GOLD"), ==, 10000);
	g_assert_cmpint(amount_in(made, "TICKET"), ==, 50);

	/* 1 GOLD over 3 is 3334 + 3333 + 3333; 50 TICKET is 17 + 17 + 16. */
	layers = layers_of(f, charm);
	g_assert_cmpuint(layers->len, ==, 4);
	{
		gint64 gold = 0, ticket = 0;

		for (i = 0; i < layers->len; i++)
		{
			g_autoptr(VentureMoney) unit = NULL;
			gint64 lot = 0, quantity = 0;

			g_object_get(g_ptr_array_index(layers, i), "unit-cost", &unit, "lot-txn-id", &lot,
				"original-qty", &quantity, NULL);
			g_assert_cmpint(lot, ==, venture_entity_get_id(txn));
			if (g_strcmp0(venture_money_get_currency(unit), "GOLD") == 0)
				gold += venture_money_get_amount(unit) * quantity;
			else
				ticket += venture_money_get_amount(unit) * quantity;
		}
		g_assert_cmpint(gold, ==, 10000);
		g_assert_cmpint(ticket, ==, 50);
	}

	one = issue(f, charm, 1);
	g_assert_cmpuint(one->len, ==, 2);
	g_assert_cmpint(amount_in(one, "GOLD"), ==, 3334);
	g_assert_cmpint(amount_in(one, "TICKET"), ==, 17);
	rest = issue(f, charm, 2);
	g_assert_cmpint(amount_in(rest, "GOLD"), ==, 6666);
	g_assert_cmpint(amount_in(rest, "TICKET"), ==, 33);
	assert_books_balance(f);
}

/*
 * A transfer keeps a lot's currencies together: a two-currency unit moved
 * to another location arrives with sibling layers there and sells with
 * both. What breaks: the old transfer wrote one layer at the rounded
 * average of a single currency.
 */
static void
test_transfer_keeps_currencies(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) sold = NULL;
	g_autoptr(GPtrArray) txns = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	VentureInventoryDraw draws[2];
	gint64 dust, token, charm_product, charm, charm_bank;

	(void)data;
	dust = item(f, product(f, "Dust"), "DUST");
	token = item(f, product(f, "Token"), "TOKEN");
	charm_product = product(f, "Charm");
	charm = item(f, charm_product, "CHARM-BAG");
	charm_bank = item(f, charm_product, "CHARM-BANK");
	receive(f, dust, 2, "0.5000 GOLD", "2026-03-01");
	receive(f, token, 3, "7 TICKET", "2026-03-01");
	draws[0].inventory_item_id = dust;
	draws[0].quantity = 2;
	draws[1].inventory_item_id = token;
	draws[1].quantity = 3;
	g_assert_true(venture_inventory_service_produce(inventory(f), draws, 2, charm, 2, NULL,
		"recipe:9", NULL, &txn, NULL, &error));
	g_assert_no_error(error);

	g_assert_true(venture_inventory_service_transfer(inventory(f), charm, charm_bank, 2, NULL, NULL, &error));
	g_assert_no_error(error);
	/* No one unit cost for a two-currency move: the notes say what it was. */
	txns = txns_of(f, charm_bank);
	g_object_get(g_ptr_array_index(txns, 0), "unit-cost", &unit, "notes", &notes, NULL);
	g_assert_null(unit);
	g_assert_nonnull(strstr(notes, "GOLD"));
	g_assert_nonnull(strstr(notes, "TICKET"));

	sold = issue(f, charm_bank, 2);
	g_assert_cmpint(amount_in(sold, "GOLD"), ==, 10000);
	g_assert_cmpint(amount_in(sold, "TICKET"), ==, 21);
}

/*
 * A memo currency's cost is tracked on its layer and posts nothing: the
 * receipt and the issue leave no journal, the issue still reports what
 * the units cost, and the valuation counts them, saying they are memo.
 */
static void
test_memo_cost_layer(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(GError) error = NULL;
	gint64 prize;

	(void)data;
	prize = item(f, product(f, "Prize"), "PRIZE");
	receive(f, prize, 3, "5 TICKET", "2026-03-01");
	journals = journals_for(f, "inventory_txn", last_txn(f, prize));
	g_assert_cmpuint(journals->len, ==, 0);

	costs = issue(f, prize, 1);
	g_assert_cmpint(amount_in(costs, "TICKET"), ==, 5);
	g_clear_pointer(&journals, g_ptr_array_unref);
	journals = journals_for(f, "inventory_txn", last_txn(f, prize));
	g_assert_cmpuint(journals->len, ==, 0);

	values = venture_inventory_service_valuation(inventory(f), f->org, NULL, &error);
	g_assert_no_error(error);
	/* Only TICKET is on hand; a zero is added only when nothing is. */
	g_assert_cmpuint(values->len, ==, 1);
	g_assert_cmpint(amount_in(values, "TICKET"), ==, 10);
	{
		VentureReport *report = venture_report_registry_lookup(
			venture_context_get_report_registry(f->context), "inventory_valuation");
		g_autoptr(VentureReportResult) result = venture_report_generate(report, f->context, NULL, NULL, &error);
		const GValue *books;

		g_assert_no_error(error);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
		books = venture_report_result_get_cell(result, 0, "books");
		g_assert_cmpstr(g_value_get_string(books), ==, "memo, not posted");
	}
}

/* The metric under @key, or NULL. */
static VentureMetric *
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;

	for (i = 0; i < metrics->len; i++)
	{
		if (g_strcmp0(venture_metric_get_key(g_ptr_array_index(metrics, i)), key) == 0)
			return g_ptr_array_index(metrics, i);
	}
	return NULL;
}

/*
 * Finding 3g: once layers were in two currencies -- even of two different
 * products -- the valuation failed for the whole organization, and an
 * empty one fell back to 0 USD. Now it is one figure per currency, book
 * currency first, and an empty GOLD organization is worth 0 GOLD. The
 * report gives a row per currency saying how it reaches the books.
 */
static void
test_valuation_per_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) empty = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	VentureReport *report;
	gint64 gold_goods, ticket_goods;

	(void)data;
	empty = venture_inventory_service_valuation(inventory(f), f->org, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(empty->len, ==, 1);
	g_assert_cmpint(amount_in(empty, "GOLD"), ==, 0);

	gold_goods = item(f, product(f, "Ore"), "ORE");
	ticket_goods = item(f, product(f, "Prize"), "PRIZE");
	receive(f, ticket_goods, 3, "5 TICKET", "2026-03-01");
	receive(f, gold_goods, 2, "1.5000 GOLD", "2026-03-01");

	values = venture_inventory_service_valuation(inventory(f), f->org, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(values->len, ==, 2);
	g_assert_cmpstr(venture_money_get_currency(g_ptr_array_index(values, 0)), ==, "GOLD");
	g_assert_cmpint(amount_in(values, "GOLD"), ==, 30000);
	g_assert_cmpint(amount_in(values, "TICKET"), ==, 15);

	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context),
		"inventory_valuation");
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	g_assert_cmpstr(g_value_get_string(venture_report_result_get_cell(result, 0, "books")), ==,
		"book currency");
	g_assert_cmpstr(g_value_get_string(venture_report_result_get_cell(result, 1, "books")), ==,
		"separate book");
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(metric(result, "valuation"))), ==, 30000);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(metric(result, "valuation_TICKET"))), ==, 15);
}

/*
 * The inventory report in a GOLD organization: the location column
 * shows the location's path, one row per currency the stock cost with
 * the average unit cost of those units, and totals per currency. It used
 * to show "$0.00 USD", a blank location and a blank unit cost for stock
 * that came in through layers.
 */
static void
test_inventory_report(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) bank = NULL;
	g_autoptr(VentureEntity) vault = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureReportResult) empty = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	VentureReport *report;
	gint64 prize;
	guint i;

	(void)data;
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "inventory");
	empty = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_money_get_currency(venture_metric_get_money(metric(empty, "value"))), ==, "GOLD");

	bank = record(f, "location");
	g_object_set(bank, "name", "Bank", NULL);
	save(f, bank);
	vault = record(f, "location");
	g_object_set(vault, "name", "Vault", "parent-id", venture_entity_get_id(bank), NULL);
	save(f, vault);
	prize = item(f, product(f, "Prize"), "PRIZE");
	row = venture_database_get(f->db, VENTURE_TYPE_INVENTORY_ITEM, prize, &error);
	g_object_set(row, "location-id", venture_entity_get_id(vault), NULL);
	save(f, row);
	receive(f, prize, 2, "1.0000 GOLD", "2026-03-01");
	receive(f, prize, 4, "5 TICKET", "2026-03-02");

	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	for (i = 0; i < 2; i++)
	{
		const GValue *currency = venture_report_result_get_cell(result, i, "currency");
		const GValue *unit = venture_report_result_get_cell(result, i, "unit_cost");
		const GValue *value = venture_report_result_get_cell(result, i, "value");
		gboolean gold = g_strcmp0(g_value_get_string(currency), "GOLD") == 0;

		g_assert_cmpstr(g_value_get_string(venture_report_result_get_cell(result, i, "location")), ==,
			"Bank / Vault");
		g_assert_cmpfloat(g_value_get_double(venture_report_result_get_cell(result, i, "on_hand")), ==, 6.0);
		g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(unit)), ==, gold ? 10000 : 5);
		g_assert_cmpint(venture_money_get_amount(g_value_get_boxed(value)), ==, gold ? 20000 : 20);
	}
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(metric(result, "value"))), ==, 20000);
	g_assert_cmpint(venture_money_get_amount(venture_metric_get_money(metric(result, "value_TICKET"))), ==, 20);
}

/* ==========================================================================
 * Through the REST API, as venturectl sends it
 * ========================================================================== */

typedef struct
{
	Fixture		 base;
	VentureWebServer	*server;
	SoupSession	*session;
	gchar		*state_dir;
	gchar		*cookie;
	guint16		 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;

	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
		&outcome->error);
	outcome->done = TRUE;
}

/* Sends @body with @content_type; returns the status. */
static guint
server_request(ServerFixture *sf, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out_body)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", sf->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (sf->cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie",
			sf->cookie);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	soup_session_send_and_read_async(sf->session, message, G_PRIORITY_DEFAULT, NULL,
		request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	if (outcome.error != NULL)
		g_error("%s %s: %s", method, path, outcome.error->message);
	if (out_body != NULL)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL), g_bytes_get_size(outcome.body));
	g_clear_pointer(&outcome.body, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
server_setup(ServerFixture *sf, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "inventory-currencies-secret", TRUE);
	sf->state_dir = g_dir_make_tmp("venture-inventory-currencies-XXXXXX", NULL);
	sf->port = (guint16)(20000 + ((getpid() + 17377) % 20000));
	sf->base.config = venture_config_new();
	g_object_set(sf->base.config, "state-dir", sf->state_dir,
		"server-bind-address", "127.0.0.1", "server-port", (gint64)sf->port,
		"security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
		"security-password-iterations", (gint64)100000, NULL);
	seed(&sf->base, data);
	sf->server = venture_web_server_new(sf->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(sf->server, &error));
	g_assert_no_error(error);
	sf->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000, NULL));
	save(&sf->base, user);
	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/login", sf->port);
		g_autoptr(GBytes) bytes = g_bytes_new_static("username=owner&password=owner-password-1",
			strlen("username=owner&password=owner-password-1"));
		RequestResult outcome = { FALSE, NULL, NULL };

		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(sf->session, message, G_PRIORITY_DEFAULT, NULL,
			request_done, &outcome);
		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);
		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);
		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');
	if (semicolon != NULL)
		*semicolon = '\0';
	sf->cookie = g_steal_pointer(&set_cookie);
}

static void
server_teardown(ServerFixture *sf, gconstpointer data)
{
	if (sf->server != NULL)
		venture_web_server_stop(sf->server);
	g_clear_pointer(&sf->cookie, g_free);
	g_clear_object(&sf->session);
	g_clear_object(&sf->server);
	teardown(&sf->base, data);
	if (sf->state_dir != NULL)
	{
		venture_test_remove_tree(sf->state_dir);
		g_clear_pointer(&sf->state_dir, g_free);
	}
	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
}

/* POSTs a goods action, asserting it answers 200. */
static void
goods_action(ServerFixture *sf, gint64 id, const gchar *action, const gchar *body)
{
	g_autofree gchar *path = g_strdup_printf("/api/v1/purchase_order/%" G_GINT64_FORMAT "/%s",
		id, action);
	g_autofree gchar *answer = NULL;
	guint status;

	status = server_request(sf, "POST", path, "application/json", body != NULL ? body : "{}",
		&answer);
	if (status != SOUP_STATUS_OK)
		g_error("%s: %u %s", path, status, answer);
}

/*
 * Stock bought with tickets arrives without the web page: a purchase order
 * in TICKET is approved, sent and received through the REST route that
 * `venturectl purchase` drives -- quantity sent as a string, the way the
 * CLI sends every value -- and lands as a TICKET cost layer. Sold for gold,
 * the revenue posts in GOLD and the cost of goods in TICKET, each in its
 * own book. If this regresses, a demo or an agent has no way to bring in
 * ticket-priced stock, or the sale folds the ticket cost into gold.
 */
static void
test_http_ticket_purchase(ServerFixture *sf, gconstpointer data)
{
	Fixture *f = &sf->base;
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) po = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *revenue_currency = NULL;
	g_autofree gchar *cost_currency = NULL;
	gint64 prize_product;
	gint64 prize;
	gint64 po_id;

	(void)data;
	prize_product = product(f, "Whistle of the Faire");
	prize = item(f, prize_product, "FAIRE-WHISTLE");
	vendor = record(f, "company");
	g_object_set(vendor, "name", "Faire prize booth", NULL);
	field(vendor, "kind", "supplier");
	save(f, vendor);

	po = record(f, "purchase_order");
	g_object_set(po, "number", "FAIRE-1", "vendor-id", venture_entity_get_id(vendor),
		"currency", "TICKET", "status", "draft", NULL);
	field(po, "ordered-at", "2026-03-01");
	save(f, po);
	po_id = venture_entity_get_id(po);
	line = record(f, "purchase_order_line");
	g_object_set(line, "purchase-order-id", po_id, "product-id", prize_product,
		"inventory-item-id", prize, "description", "Whistle", "quantity", (gint64)2,
		"position", (gint64)1, NULL);
	field(line, "unit-price", "20 TICKET");
	save(f, line);

	goods_action(sf, po_id, "approve", NULL);
	goods_action(sf, po_id, "send", NULL);
	body = g_strdup_printf("{\"line_id\": \"%" G_GINT64_FORMAT "\", \"quantity\": \"2\"}",
		venture_entity_get_id(line));
	goods_action(sf, po_id, "receive", body);

	layers = layers_of(f, prize);
	g_assert_cmpuint(layers->len, ==, 1);
	{
		g_autoptr(VentureMoney) unit = NULL;

		g_object_get(g_ptr_array_index(layers, 0), "unit-cost", &unit, NULL);
		g_assert_cmpstr(venture_money_get_currency(unit), ==, "TICKET");
		g_assert_cmpint(venture_money_get_amount(unit), ==, 20);
	}

	sale = record(f, "sale");
	g_object_set(sale, "venture-id", f->venture, "product-id", prize_product,
		"quantity", (gint64)1, NULL);
	field(sale, "occurred-at", "2026-03-05");
	field(sale, "gross", "45 GOLD");
	save(f, sale);

	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(journals->len, ==, 1);
	revenue_currency = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(revenue_currency, ==, "GOLD");
	g_clear_pointer(&journals, g_ptr_array_unref);
	journals = journals_for(f, "inventory_txn", last_txn(f, prize));
	g_assert_cmpuint(journals->len, ==, 1);
	cost_currency = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(cost_currency, ==, "TICKET");
	g_assert_cmpint(debits_of(f, g_ptr_array_index(journals, 0)), ==, 20);
	assert_books_balance(f);

	/* One whistle left, valued in tickets. */
	values = venture_inventory_service_valuation(inventory(f), f->org, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(amount_in(values, "TICKET"), ==, 20);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/inventory-currencies/issue-across-currencies", Fixture, "valued", setup,
		test_issue_across_currencies, teardown);
	g_test_add("/inventory-currencies/fifo-order", Fixture, "valued", setup,
		test_fifo_order_across_currencies, teardown);
	g_test_add("/inventory-currencies/gold-sale-ticket-unit", Fixture, "valued", setup,
		test_gold_sale_ticket_unit, teardown);
	g_test_add("/inventory-currencies/zero-cost-beside-costed", Fixture, "separate_book", setup,
		test_zero_cost_beside_costed, teardown);
	g_test_add("/inventory-currencies/craft-two-currencies-then-sell", Fixture, "separate_book", setup,
		test_craft_two_currencies_then_sell, teardown);
	g_test_add("/inventory-currencies/transfer-keeps-currencies", Fixture, "valued", setup,
		test_transfer_keeps_currencies, teardown);
	g_test_add("/inventory-currencies/memo-cost-layer", Fixture, "memo", setup,
		test_memo_cost_layer, teardown);
	g_test_add("/inventory-currencies/valuation-per-currency", Fixture, "separate_book", setup,
		test_valuation_per_currency, teardown);
	g_test_add("/inventory-currencies/inventory-report", Fixture, "valued", setup,
		test_inventory_report, teardown);
	g_test_add("/inventory-currencies/http-ticket-purchase", ServerFixture, "separate_book",
		server_setup, test_http_ticket_purchase, server_teardown);
	return g_test_run();
}
