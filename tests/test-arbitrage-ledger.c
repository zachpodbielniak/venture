/*
 * test-arbitrage-ledger.c - An arbitrage attempt, and what it does to the books
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Recording an attempt is only worth anything if the books it reaches are
 * right. Every movement of a trade passes through one account, arbitrage
 * positions, so that what is left there is exactly the trade's profit or
 * loss; closing moves it to gains. These tests hold the parts of that
 * which a plausible implementation gets wrong: a journal that does not
 * balance, a leg that posts while planned or twice when saved again,
 * currencies added together, a purse spent before it was funded, a unit of
 * stock posted twice or into goods-received-not-invoiced, a close that
 * cannot be taken back, a staged record that writes before it is approved,
 * and a report that disagrees with the page.
 */

#include <venture.h>
#include "arbitrage/venture-arbitrage-private.h"

#include <libsoup/soup.h>
#include <string.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*db;
	VentureContext	*context;
	gint64		 org;
	gint64		 venture;
	gint64		 aria;
	gint64		 bram;
	/* Venues: two auction houses whose money is a character's purse, a
	 * fair paid in tickets, two dollar venues with no location (the
	 * organization's cash) and a euro shop. */
	gint64		 ah_aria;
	gint64		 ah_bram;
	gint64		 fair;
	gint64		 market;
	gint64		 supplier;
	gint64		 eurshop;
} Fixture;

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static void
save_refused(Fixture *f, gpointer record, const gchar *fragment)
{
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error);
	g_assert_nonnull(error);
	g_assert_false(saved);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
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

static VentureEntity *
reread(Fixture *f, GType type, gint64 id)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *entity;

	entity = venture_database_get(f->db, type, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(entity);
	return entity;
}

static gint64
int_of(Fixture *f, GType type, gint64 id, const gchar *property)
{
	g_autoptr(VentureEntity) row = reread(f, type, id);
	gint64 value = 0;

	g_object_get(row, property, &value, NULL);
	return value;
}

static gint
enum_of(Fixture *f, GType type, gint64 id, const gchar *property)
{
	g_autoptr(VentureEntity) row = reread(f, type, id);
	gint value = 0;

	g_object_get(row, property, &value, NULL);
	return value;
}

static void
define_currency(Fixture *f, const gchar *code, gint64 exponent, const gchar *treatment)
{
	g_autoptr(VentureEntity) currency = record(f, "currency");

	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", treatment);
	save(f, currency);
}

static gint64
location(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) place = record(f, "location");

	g_object_set(place, "name", name, "kind", "character", NULL);
	save(f, place);
	return ID(place);
}

static gint64
venue(Fixture *f, const gchar *name, gint64 location_id, const gchar *currency)
{
	g_autoptr(VentureEntity) row = record(f, "venue");

	g_object_set(row, "name", name, "location-id", location_id, "currency", currency, NULL);
	save(f, row);
	return ID(row);
}

static gint64
holding(Fixture *f, gint64 location_id)
{
	g_autoptr(GError) error = NULL;
	gint64 account = 0;

	g_assert_true(venture_holdings_account_for_location(f->db, f->org, location_id, TRUE,
	                                                    NULL, &account, &error));
	g_assert_no_error(error);
	return account;
}

static gint64
held(Fixture *f, gint64 location_id, const gchar *currency)
{
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;

	balance = venture_holdings_balance(f->db, holding(f, location_id), currency, 0, &error);
	g_assert_no_error(error);
	return venture_money_get_amount(balance);
}

/* A hand adjustment of a memo currency into @location_id's holding. */
static void
adjust(Fixture *f, gint64 location_id, const gchar *amount, const gchar *date)
{
	g_autoptr(VentureEntity) movement = record(f, "holding_txn");

	g_object_set(movement, "account-id", holding(f, location_id), NULL);
	field(movement, "amount", amount);
	field(movement, "occurred-at", date);
	save(f, movement);
}

/* Funds @location_id's purse with a posted currency: a session's money
 * yield, posted on 1 March. */
static void
fund(Fixture *f, gint64 location_id, const gchar *amount)
{
	g_autoptr(VentureEntity) session = record(f, "session");
	g_autoptr(VentureEntity) yield = record(f, "session_yield");
	g_autoptr(GError) error = NULL;
	guint posted = 0;

	g_object_set(session, "name", "Farming", "activity", "farm", "location-id", location_id, NULL);
	field(session, "started-at", "2026-03-01T08:00:00Z");
	field(session, "ended-at", "2026-03-01T09:00:00Z");
	save(f, session);
	g_object_set(yield, "session-id", ID(session), NULL);
	field(yield, "amount", amount);
	save(f, yield);
	g_assert_true(venture_sessions_post(f->db, ID(session), NULL, &posted, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(posted, ==, 1);
}

static gint64
trade(Fixture *f, const gchar *name, const gchar *strategy)
{
	g_autoptr(VentureEntity) row = record(f, "arbitrage_trade");

	g_object_set(row, "name", name, "strategy", strategy, "venture-id", f->venture, NULL);
	save(f, row);
	return ID(row);
}

/* An unsaved leg; @status NULL leaves it planned. */
static VentureEntity *
leg_new(Fixture *f, gint64 trade_id, gint64 venue_id, const gchar *kind, const gchar *amount,
	const gchar *fees, const gchar *status, const gchar *when)
{
	VentureEntity *row = record(f, "arbitrage_leg");

	g_object_set(row, "trade-id", trade_id, "venue-id", venue_id, NULL);
	field(row, "kind", kind);

	if (NULL != amount)
		field(row, "amount", amount);

	if (NULL != fees)
		field(row, "fees", fees);

	if (NULL != status)
		field(row, "status", status);

	if (NULL != when)
		field(row, "occurred-at", when);

	return row;
}

static gint64
leg(Fixture *f, gint64 trade_id, gint64 venue_id, const gchar *kind, const gchar *amount,
	const gchar *fees, const gchar *status, const gchar *when)
{
	g_autoptr(VentureEntity) row = leg_new(f, trade_id, venue_id, kind, amount, fees, status, when);

	save(f, row);
	return ID(row);
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

static guint
posted_journals(Fixture *f, const gchar *type, gint64 id)
{
	g_autoptr(GPtrArray) journals = journals_for(f, type, id);
	guint count = 0;
	guint i;

	for (i = 0; i < journals->len; i++)
	{
		VentureJournalState state;

		g_object_get(g_ptr_array_index(journals, i), "state", &state, NULL);
		count += (VENTURE_JOURNAL_POSTED == state) ? 1 : 0;
	}

	return count;
}

static GPtrArray *
lines_of(Fixture *f, gint64 journal_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ, journal_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static guint
count_rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;

	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows->len;
}

/* Every journal that is not a draft balances in its book amounts. */
static void
assert_balanced(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	guint j;

	venture_query_set_limit(query, 0);
	journals = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);

	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(GPtrArray) lines = NULL;
		VentureJournalState state;
		gint64 net = 0;

		g_object_get(journal, "state", &state, NULL);

		if (VENTURE_JOURNAL_DRAFT == state)
			continue;

		lines = lines_of(f, ID(journal));
		g_assert_cmpuint(lines->len, >=, 2);

		for (j = 0; j < lines->len; j++)
		{
			g_autoptr(VentureMoney) book = NULL;
			VentureLedgerSide side;

			g_object_get(g_ptr_array_index(lines, j), "book-amount", &book, "side", &side, NULL);
			g_assert_nonnull(book);
			net += (VENTURE_LEDGER_SIDE_DEBIT == side) ? venture_money_get_amount(book)
			                                           : -venture_money_get_amount(book);
		}

		g_assert_cmpint(net, ==, 0);
	}
}

/* What @account holds in book @currency across the books, debits positive. */
static gint64
account_net(Fixture *f, gint64 account, const gchar *currency)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GError) error = NULL;
	gint64 net = 0;
	guint i;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "account-id", VENTURE_FILTER_OP_EQ, account, NULL);
	lines = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);

	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureMoney) book = NULL;
		VentureLedgerSide side;
		gint64 journal_id = 0;
		VentureJournalState state;

		g_object_get(line, "book-amount", &book, "side", &side, "journal-id", &journal_id, NULL);
		state = (VentureJournalState)enum_of(f, VENTURE_TYPE_JOURNAL, journal_id, "state");

		if ((VENTURE_JOURNAL_DRAFT == state) || (NULL == book) ||
		    (0 != g_strcmp0(venture_money_get_currency(book), currency)))
			continue;

		net += (VENTURE_LEDGER_SIDE_DEBIT == side) ? venture_money_get_amount(book)
		                                           : -venture_money_get_amount(book);
	}

	return net;
}

static gint64
arb_account(Fixture *f, const gchar *classification)
{
	g_autoptr(GError) error = NULL;
	gint64 id;

	id = venture_arbitrage_account(f->db, f->org, classification, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

/* The chart account with @code (plain or organization-scoped), or 0. */
static gint64
account_by_code(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) found = NULL;
	g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", f->org, code);

	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	found = venture_database_find_one(f->db, query, NULL);

	if (NULL == found)
	{
		g_clear_object(&query);
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped, NULL);
		found = venture_database_find_one(f->db, query, NULL);
	}

	return (NULL != found) ? ID(found) : 0;
}

/* The trade's positions balance in @currency, in minor units; 0 when it
 * has none there. */
static gint64
position(Fixture *f, gint64 trade_id, const gchar *currency)
{
	g_autoptr(GPtrArray) balances = NULL;
	g_autoptr(GError) error = NULL;
	const VentureMoney *found;

	balances = venture_arbitrage_position(f->db, trade_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balances);
	found = venture_money_totals_lookup(balances, currency);
	return (NULL != found) ? venture_money_get_amount(found) : 0;
}

/* The trade summary's realised figure in @currency, minor units. */
static gint64
realised(Fixture *f, gint64 trade_id, const gchar *currency)
{
	g_autoptr(JsonNode) summary = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *figures;
	guint i;

	summary = venture_arbitrage_trade_summary(f->db, trade_id, &error);
	g_assert_no_error(error);
	figures = json_object_get_array_member(json_node_get_object(summary), "figures");

	for (i = 0; i < json_array_get_length(figures); i++)
	{
		JsonObject *row = json_array_get_object_element(figures, i);

		if (0 == g_strcmp0(json_object_get_string_member(row, "currency"), currency))
		{
			g_autoptr(VentureMoney) money = venture_money_from_json(
				json_object_get_member(row, "realised"), NULL, &error);

			g_assert_no_error(error);
			return venture_money_get_amount(money);
		}
	}

	g_error("no figures in %s", currency);
	return 0;
}

static void
execute(Fixture *f, gint64 leg_id)
{
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_arbitrage_execute_leg(f->db, leg_id, NULL, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

static GDateTime *
time_of(const gchar *text)
{
	GDateTime *when = venture_time_from_string(text, NULL);

	g_assert_nonnull(when);
	return when;
}

static void
close_at(Fixture *f, gint64 trade_id, const gchar *when)
{
	g_autoptr(GDateTime) at = time_of(when);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) result = NULL;

	g_assert_true(venture_arbitrage_close(f->db, trade_id, at, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

static gint64
item_for(Fixture *f, const gchar *product_name)
{
	g_autoptr(VentureEntity) product = record(f, "product");
	g_autoptr(VentureEntity) item = record(f, "inventory_item");

	g_object_set(product, "name", product_name, NULL);
	save(f, product);
	g_object_set(item, "product-id", ID(product), NULL);
	save(f, item);
	return ID(item);
}

static gint64
on_hand(Fixture *f, gint64 item_id)
{
	g_autoptr(GError) error = NULL;
	gint64 units;

	units = venture_inventory_service_on_hand(venture_inventory_service_get(f->db), item_id, NULL,
	                                          &error);
	g_assert_no_error(error);
	return units;
}

/* A planned stock leg of @quantity units for @amount. */
static gint64
stock_leg(Fixture *f, gint64 trade_id, gint64 venue_id, const gchar *kind, gint64 item_id,
	gint64 quantity, const gchar *amount, const gchar *fees, const gchar *when)
{
	g_autoptr(VentureEntity) row = leg_new(f, trade_id, venue_id, kind, amount, fees, NULL, when);

	g_object_set(row, "inventory-item-id", item_id, "quantity", quantity, NULL);
	save(f, row);
	return ID(row);
}

static gchar *
money_text(VentureEntity *entity, const gchar *property)
{
	g_autoptr(VentureMoney) money = NULL;

	g_object_get(entity, property, &money, NULL);
	return (NULL != money) ? venture_money_to_string(money) : NULL;
}

/* ==========================================================================
 * Fixture
 * ========================================================================== */

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();
	g_autoptr(VentureEntity) rate = NULL;

	(void)data;
	venture_currency_clear_registered();
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(venture, "name", "Flips", "organization-id", f->org, NULL);
	save(f, venture);
	f->venture = ID(venture);

	/* The books are in dollars. Gold is a game's currency kept in a book
	 * of its own, tickets are counted and never posted, and euros are
	 * valued in dollars at a recorded rate. */
	define_currency(f, "GOLD", 4, "separate_book");
	define_currency(f, "TICKET", 0, "memo");
	rate = record(f, "exchange_rate");
	g_object_set(rate, "from-currency", "EUR", "to-currency", "USD",
	             "rate-numerator", (gint64)11, "rate-denominator", (gint64)10, NULL);
	field(rate, "effective-at", "2026-01-01");
	save(f, rate);

	f->aria = location(f, "Aria");
	f->bram = location(f, "Bram");
	f->ah_aria = venue(f, "Argent Dawn AH", f->aria, "GOLD");
	f->ah_bram = venue(f, "Silvermoon AH", f->bram, "GOLD");
	f->fair = venue(f, "Darkmoon Faire", f->aria, "TICKET");
	f->market = venue(f, "Market", 0, "USD");
	f->supplier = venue(f, "Supplier", 0, "USD");
	f->eurshop = venue(f, "Euro shop", 0, "EUR");
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_currency_clear_registered();

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* ==========================================================================
 * Posting legs
 * ========================================================================== */

/*
 * A dollar flip at two venues with no location (the organization's cash).
 * A planned leg posts nothing; executing it posts the position and the
 * fees; saving it again unchanged posts nothing more; changing its amount
 * reverses and reposts; cancelling an executed leg takes it back out. If
 * this regresses, a plan shows up in the books, a note edit doubles a
 * purchase, or a cancelled stake stays spent.
 */
static void
test_cash_legs(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) buy = NULL;
	g_autoptr(VentureEntity) stake = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	gint64 id;
	gint64 sell;
	gint64 buy_id;
	gint64 positions;
	gint64 fees;
	gint64 cash;

	(void)data;
	id = trade(f, "Herb flip", "spread");
	positions = arb_account(f, "arbitrage_positions");
	fees = arb_account(f, "arbitrage_fees");

	buy = leg_new(f, id, f->market, "buy", "100.00 USD", "2.00 USD", NULL, "2026-03-05T10:00:00Z");
	save(f, buy);
	g_assert_cmpuint(journals_for(f, "arbitrage_leg", ID(buy))->len, ==, 0);

	field(buy, "status", "executed");
	save(f, buy);
	journals = journals_for(f, "arbitrage_leg", ID(buy));
	g_assert_cmpuint(journals->len, ==, 1);
	lines = lines_of(f, ID(g_ptr_array_index(journals, 0)));
	/* Principal first, then fees: Dr positions / Cr cash, Dr fees / Cr cash. */
	g_assert_cmpuint(lines->len, ==, 4);
	g_object_get(g_ptr_array_index(lines, 1), "account-id", &cash, NULL);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_JOURNAL_LINE, ID(g_ptr_array_index(lines, 0)), "account-id"), ==, positions);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_JOURNAL_LINE, ID(g_ptr_array_index(lines, 2)), "account-id"), ==, fees);
	g_assert_cmpint(account_net(f, positions, "USD"), ==, 10000);
	g_assert_cmpint(account_net(f, fees, "USD"), ==, 200);
	g_assert_cmpint(account_net(f, cash, "USD"), ==, -10200);

	sell = leg(f, id, f->supplier, "sell", "130.00 USD", "3.00 USD", "executed", "2026-03-06T10:00:00Z");
	g_assert_cmpuint(posted_journals(f, "arbitrage_leg", sell), ==, 1);
	g_assert_cmpint(position(f, id, "USD"), ==, -3000);

	/* Saved again with only its notes changed: nothing reposts. */
	buy_id = ID(buy);
	g_clear_object(&buy);
	buy = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, buy_id);
	g_object_set(buy, "notes", "bought before the reset", NULL);
	save(f, buy);
	g_assert_cmpuint(journals_for(f, "arbitrage_leg", ID(buy))->len, ==, 1);

	/* A changed amount: the old journal is reversed and the new one posted. */
	field(buy, "amount", "110.00 USD");
	save(f, buy);
	g_assert_cmpuint(journals_for(f, "arbitrage_leg", ID(buy))->len, ==, 3);
	g_assert_cmpuint(posted_journals(f, "arbitrage_leg", ID(buy)), ==, 2);
	g_assert_cmpint(position(f, id, "USD"), ==, -2000);

	/* A stake executed and then cancelled is taken back out of the books. */
	stake = leg_new(f, id, f->market, "stake", "40.00 USD", NULL, "executed", "2026-03-06T11:00:00Z");
	save(f, stake);
	g_assert_cmpint(position(f, id, "USD"), ==, 2000);
	field(stake, "status", "cancelled");
	save(f, stake);
	g_assert_cmpuint(journals_for(f, "arbitrage_leg", ID(stake))->len, ==, 2);
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_JOURNAL,
		ID(g_ptr_array_index(journals_for(f, "arbitrage_leg", ID(stake)), 0)), "state"),
		==, VENTURE_JOURNAL_REVERSED);
	g_assert_cmpint(position(f, id, "USD"), ==, -2000);

	/* A failed leg posts nothing either. */
	g_assert_cmpuint(journals_for(f, "arbitrage_leg",
		leg(f, id, f->market, "buy", "5.00 USD", NULL, "failed", NULL))->len, ==, 0);

	/* Realised: 130 back, 110 out, 5 of fees. */
	g_assert_cmpint(realised(f, id, "USD"), ==, 1500);
	assert_balanced(f);
}

/*
 * Close moves the position to gains and stamps the trade; the trade is
 * then frozen; reopen reverses the close; closing again posts a new close
 * under a new key. If this regresses, a closed trade's books can be
 * edited under its closing journal, a close cannot be taken back, or a
 * second close collides with the first's posting key.
 */
static void
test_close_reopen(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) late = NULL;
	g_autoptr(GDateTime) opened = NULL;
	g_autoptr(GDateTime) expected_open = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) closes = NULL;
	gint64 id;
	gint64 sell;
	gint64 gains;
	gint64 positions;

	(void)data;
	id = trade(f, "Herb flip", "spread");
	gains = arb_account(f, "arbitrage_gains");
	positions = arb_account(f, "arbitrage_positions");
	leg(f, id, f->market, "buy", "100.00 USD", NULL, "executed", "2026-03-05T10:00:00Z");
	sell = leg(f, id, f->supplier, "sell", "125.00 USD", NULL, "executed", "2026-03-06T10:00:00Z");

	/* Closed is the action's: by hand it is refused. */
	row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	field(row, "status", "closed");
	save_refused(f, row, "Close action");
	g_clear_object(&row);

	/* Never before the last leg. */
	{
		g_autoptr(GDateTime) early = time_of("2026-03-05T12:00:00Z");

		g_assert_false(venture_arbitrage_close(f->db, id, early, NULL, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

	close_at(f, id, "2026-03-07T10:00:00Z");
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "status"), ==,
	                VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED);
	g_assert_cmpint(position(f, id, "USD"), ==, 0);
	g_assert_cmpint(account_net(f, gains, "USD"), ==, -2500);
	g_assert_cmpint(account_net(f, positions, "USD"), ==, 0);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "close-journal-id"), >, 0);

	/* Opened at its first leg, though nobody set it open. */
	row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	g_object_get(row, "opened-at", &opened, NULL);
	expected_open = time_of("2026-03-05T10:00:00Z");
	g_assert_true(g_date_time_equal(opened, expected_open));

	/* Frozen: an executed leg, a new executed leg, the status by hand,
	 * the stamp by hand. A planned leg is still fine. */
	g_clear_object(&row);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, sell);
	field(row, "amount", "200.00 USD");
	save_refused(f, row, "Reopen it first");
	late = leg_new(f, id, f->market, "sell", "1.00 USD", NULL, "executed", "2026-03-08T10:00:00Z");
	save_refused(f, late, "Reopen it first");
	leg(f, id, f->market, "buy", "1.00 USD", NULL, NULL, NULL);
	g_clear_object(&row);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	field(row, "status", "open");
	save_refused(f, row, "Reopen action");
	g_clear_object(&row);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	g_object_set(row, "close-journal-id", (gint64)0, NULL);
	save_refused(f, row, "Close action");

	/* Closing twice is refused, not posted twice. */
	g_assert_false(venture_arbitrage_close(f->db, id, NULL, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);

	/* Reopen: the close is reversed, the position is back. */
	g_assert_true(venture_arbitrage_reopen(f->db, id, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "status"), ==,
	                VENTURE_ARBITRAGE_TRADE_STATUS_OPEN);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "close-journal-id"), ==, 0);
	g_assert_cmpint(position(f, id, "USD"), ==, -2500);
	g_assert_cmpint(account_net(f, gains, "USD"), ==, 0);

	/* Now editable, and closed again under a new key. */
	g_clear_object(&row);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, sell);
	field(row, "amount", "130.00 USD");
	save(f, row);
	close_at(f, id, "2026-03-08T10:00:00Z");
	g_assert_cmpint(position(f, id, "USD"), ==, 0);
	g_assert_cmpint(account_net(f, gains, "USD"), ==, -3000);
	closes = journals_for(f, "arbitrage_trade", id);
	/* First close, its reversal, second close. */
	g_assert_cmpuint(closes->len, ==, 3);
	{
		g_autofree gchar *first = NULL;
		g_autofree gchar *second = NULL;
		g_autofree gchar *key0 = g_strdup_printf("arbitrage_close:%" G_GINT64_FORMAT ":0:USD", id);
		g_autofree gchar *key1 = g_strdup_printf("arbitrage_close:%" G_GINT64_FORMAT ":1:USD", id);

		g_object_get(g_ptr_array_index(closes, 0), "posting-key", &first, NULL);
		g_object_get(g_ptr_array_index(closes, 2), "posting-key", &second, NULL);
		g_assert_cmpstr(first, ==, key0);
		g_assert_cmpstr(second, ==, key1);
	}

	assert_balanced(f);
}

/*
 * Gold is a separate book, tickets are memo, euros are converted at a
 * rate. Each leg posts where its currency's rule says -- a gold journal, a
 * ticket movement on the purse and no journal, a dollar journal valuing
 * euros -- and the close posts one journal per book section, never adding
 * gold to dollars. If this regresses, gold profit lands in the dollar
 * books, tickets reach a journal, or the euro section is left behind.
 */
static void
test_currencies(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) closes = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autofree gchar *currency = NULL;
	gint64 id;
	gint64 buy_gold;
	gint64 buy_tickets;
	gint64 buy_euros;
	gint64 gains;
	guint movements;
	guint i;

	(void)data;
	fund(f, f->aria, "500 GOLD");
	adjust(f, f->aria, "50 TICKET", "2026-03-01T00:00:00Z");
	gains = arb_account(f, "arbitrage_gains");
	id = trade(f, "Everything", "spread");
	movements = count_rows(f, VENTURE_TYPE_HOLDING_TXN);

	buy_gold = leg(f, id, f->ah_aria, "buy", "200 GOLD", NULL, "executed", "2026-03-02T10:00:00Z");
	leg(f, id, f->ah_bram, "sell", "260 GOLD", NULL, "executed", "2026-03-03T10:00:00Z");
	journals = journals_for(f, "arbitrage_leg", buy_gold);
	g_assert_cmpuint(journals->len, ==, 1);
	g_object_get(g_ptr_array_index(journals, 0), "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "GOLD");
	g_assert_cmpint(held(f, f->aria, "GOLD"), ==, 3000000);
	g_assert_cmpint(held(f, f->bram, "GOLD"), ==, 2600000);

	/* Tickets: a movement on the purse, never a journal. */
	buy_tickets = leg(f, id, f->fair, "buy", "30 TICKET", NULL, "executed", "2026-03-02T11:00:00Z");
	leg(f, id, f->fair, "payout", "45 TICKET", NULL, "executed", "2026-03-04T10:00:00Z");
	g_assert_cmpuint(journals_for(f, "arbitrage_leg", buy_tickets)->len, ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_HOLDING_TXN), ==, movements + 2);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 65);

	/* Euros: one dollar journal, the line keeping its euros. */
	buy_euros = leg(f, id, f->eurshop, "buy", "100.00 EUR", NULL, "executed", "2026-03-02T12:00:00Z");
	leg(f, id, f->eurshop, "sell", "150.00 EUR", NULL, "executed", "2026-03-03T12:00:00Z");
	g_clear_pointer(&journals, g_ptr_array_unref);
	journals = journals_for(f, "arbitrage_leg", buy_euros);
	g_clear_pointer(&currency, g_free);
	g_object_get(g_ptr_array_index(journals, 0), "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "USD");
	lines = lines_of(f, ID(g_ptr_array_index(journals, 0)));
	{
		g_autoptr(VentureMoney) original = NULL;
		g_autoptr(VentureMoney) book = NULL;

		g_object_get(g_ptr_array_index(lines, 0), "amount", &original, "book-amount", &book, NULL);
		g_assert_cmpstr(venture_money_get_currency(original), ==, "EUR");
		g_assert_cmpint(venture_money_get_amount(original), ==, 10000);
		g_assert_cmpstr(venture_money_get_currency(book), ==, "USD");
		g_assert_cmpint(venture_money_get_amount(book), ==, 11000);
	}

	g_assert_cmpint(position(f, id, "GOLD"), ==, -600000);
	g_assert_cmpint(position(f, id, "USD"), ==, -5500);
	g_assert_cmpint(position(f, id, "TICKET"), ==, 0);

	/* What each currency made, kept apart. */
	g_assert_cmpint(realised(f, id, "GOLD"), ==, 600000);
	g_assert_cmpint(realised(f, id, "TICKET"), ==, 15);
	g_assert_cmpint(realised(f, id, "EUR"), ==, 5000);

	/* Two closing journals, one per book section, never added together. */
	close_at(f, id, "2026-03-05T10:00:00Z");
	closes = journals_for(f, "arbitrage_trade", id);
	g_assert_cmpuint(closes->len, ==, 2);

	for (i = 0; i < closes->len; i++)
	{
		g_autofree gchar *code = NULL;
		g_autofree gchar *key = NULL;
		g_autofree gchar *wanted = NULL;

		g_object_get(g_ptr_array_index(closes, i), "currency", &code, "posting-key", &key, NULL);
		wanted = g_strdup_printf("arbitrage_close:%" G_GINT64_FORMAT ":0:%s", id, code);
		g_assert_cmpstr(key, ==, wanted);
		g_assert_true((0 == g_strcmp0(code, "GOLD")) || (0 == g_strcmp0(code, "USD")));
	}

	g_assert_cmpint(position(f, id, "GOLD"), ==, 0);
	g_assert_cmpint(position(f, id, "USD"), ==, 0);
	g_assert_cmpint(account_net(f, gains, "GOLD"), ==, -600000);
	g_assert_cmpint(account_net(f, gains, "USD"), ==, -5500);
	assert_balanced(f);
}

/*
 * A purse cannot be spent before it was funded: a back-dated ticket buy
 * is refused although the purse holds enough now, and a gold stake from
 * an empty purse is refused by the posting guard. If this regresses, a
 * character spends money it did not have yet.
 */
static void
test_holding_floor(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) early = NULL;
	g_autoptr(VentureEntity) stake = NULL;
	gint64 id;
	gint64 early_id;

	(void)data;
	adjust(f, f->aria, "10 TICKET", "2026-03-10T00:00:00Z");
	id = trade(f, "Faire", "deal");

	early = leg_new(f, id, f->fair, "buy", "8 TICKET", NULL, NULL, "2026-03-01T10:00:00Z");
	save(f, early);
	field(early, "status", "executed");
	save_refused(f, early, "below zero");
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_LEG, ID(early), "status"), ==,
	                VENTURE_ARBITRAGE_LEG_STATUS_PLANNED);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 10);

	early_id = ID(early);
	g_clear_object(&early);
	early = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, early_id);
	field(early, "occurred-at", "2026-03-12T10:00:00Z");
	field(early, "status", "executed");
	save(f, early);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 2);

	/* Bram has no gold at all. */
	stake = leg_new(f, id, f->ah_bram, "stake", "5 GOLD", NULL, "executed", "2026-03-05T10:00:00Z");
	save_refused(f, stake, "below zero");
	assert_balanced(f);
}

/* ==========================================================================
 * Stock
 * ========================================================================== */

/*
 * A stock flip: seven units bought for 100.00 and sold for 130.00. The
 * buy is received paid from the venue's cash -- goods received not
 * invoiced (2010) never touched -- at an exact split of 100.00 over seven
 * units; the sell issues them at that cost into the positions account,
 * not cost of goods sold (5000); the positions residual is the profit.
 * Executing a stock leg by hand, writing its stamps, and cancelling it
 * once executed are refused. If this regresses, a cent goes missing per
 * flip, a liability waits for a bill that never comes, or a unit's cost
 * is posted twice.
 */
static void
test_stock_flip(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cost = NULL;
	gint64 id;
	gint64 item;
	gint64 buy;
	gint64 sell;
	gint64 inventory;
	gint64 layered;
	guint i;

	(void)data;
	item = item_for(f, "Peacebloom");
	id = trade(f, "Peacebloom", "spread");
	buy = stock_leg(f, id, f->supplier, "buy", item, 7, "100.00 USD", "1.00 USD", "2026-03-05T10:00:00Z");

	/* By hand: executing, or stamping the movement, is refused. */
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, buy);
	field(row, "status", "executed");
	save_refused(f, row, "Execute action");
	g_clear_object(&row);
	/* (A movement id is checked as a reference first; the cost is not.) */
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, buy);
	field(row, "cost", "1.00 USD");
	save_refused(f, row, "Execute action");
	g_clear_object(&row);

	execute(f, buy);
	g_assert_cmpint(on_hand(f, item), ==, 7);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_ARBITRAGE_LEG, buy, "inventory-txn-id"), >, 0);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, buy);
	cost = money_text(row, "cost");
	g_assert_cmpstr(cost, ==, "100.00 USD");
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "status"), ==,
	                VENTURE_ARBITRAGE_TRADE_STATUS_OPEN);

	/* An exact split: 4 at 14.29 and 3 at 14.28 is 100.00 to the cent. */
	query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "inventory-item-id", VENTURE_FILTER_OP_EQ, item, NULL);
	layers = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	layered = 0;

	for (i = 0; i < layers->len; i++)
	{
		g_autoptr(VentureMoney) unit = NULL;
		gint64 units = 0;

		g_object_get(g_ptr_array_index(layers, i), "unit-cost", &unit, "original-qty", &units, NULL);
		layered += venture_money_get_amount(unit) * units;
	}

	g_assert_cmpuint(layers->len, ==, 2);
	g_assert_cmpint(layered, ==, 10000);

	/* Inventory up 100, the venue's cash down 101, GRNI untouched; the
	 * buy's own journal is its fee alone. */
	inventory = account_by_code(f, "1200");
	g_assert_cmpint(account_net(f, inventory, "USD"), ==, 10000);
	g_assert_cmpint(account_net(f, account_by_code(f, "2010"), "USD"), ==, 0);
	g_assert_cmpuint(lines_of(f, ID(g_ptr_array_index(journals_for(f, "arbitrage_leg", buy), 0)))->len, ==, 2);
	g_assert_cmpint(position(f, id, "USD"), ==, 0);

	sell = stock_leg(f, id, f->market, "sell", item, 7, "130.00 USD", NULL, "2026-03-06T10:00:00Z");
	execute(f, sell);
	g_assert_cmpint(on_hand(f, item), ==, 0);
	g_clear_object(&row);
	g_clear_pointer(&cost, g_free);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, sell);
	cost = money_text(row, "cost");
	g_assert_cmpstr(cost, ==, "100.00 USD");
	g_assert_cmpint(account_net(f, inventory, "USD"), ==, 0);
	g_assert_cmpint(account_net(f, account_by_code(f, "5000"), "USD"), ==, 0);

	/* What is left on the position is the profit before fees. */
	g_assert_cmpint(position(f, id, "USD"), ==, -3000);
	g_assert_cmpint(realised(f, id, "USD"), ==, 2900);

	/* Executed stock legs are for good. */
	g_clear_object(&row);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, buy);
	field(row, "status", "cancelled");
	save_refused(f, row, "counter-leg");

	close_at(f, id, "2026-03-07T10:00:00Z");
	g_assert_cmpint(account_net(f, arb_account(f, "arbitrage_gains"), "USD"), ==, -3000);
	assert_balanced(f);
}

/* Ten units bought for 50.00, four sold for 30.00, the trade abandoned. */
static gint64
half_sold(Fixture *f, gint64 *out_item)
{
	gint64 id;
	gint64 item;

	item = item_for(f, "Silkweave");
	id = trade(f, "Silkweave", "spread");
	execute(f, stock_leg(f, id, f->supplier, "buy", item, 10, "50.00 USD", NULL, "2026-03-05T10:00:00Z"));
	execute(f, stock_leg(f, id, f->market, "sell", item, 4, "30.00 USD", NULL, "2026-03-06T10:00:00Z"));
	*out_item = item;
	return id;
}

/*
 * Abandoned with the stock written off, the six unsold units leave at
 * their cost (30.00) through a write_off leg into the position, and the
 * close takes the whole loss (20.00) to gains. Kept, they stay on the
 * shelf at cost and the close takes the profit on the four sold (10.00).
 * A write_off leg typed by hand is refused. If this regresses, an
 * abandoned trade's loss is hidden in inventory or counted twice.
 */
static void
test_abandon(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) by_hand = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GError) error = NULL;
	gint64 item;
	gint64 kept_item;
	gint64 id;
	gint64 kept;
	gint64 gains;
	guint i;
	guint write_offs;

	(void)data;
	gains = arb_account(f, "arbitrage_gains");
	id = half_sold(f, &item);

	by_hand = leg_new(f, id, 0, "write_off", NULL, NULL, NULL, NULL);
	save_refused(f, by_hand, "Abandon action");

	g_assert_true(venture_arbitrage_abandon(f->db, id, TRUE, NULL, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "status"), ==,
	                VENTURE_ARBITRAGE_TRADE_STATUS_ABANDONED);
	g_assert_cmpint(on_hand(f, item), ==, 0);
	g_assert_cmpint(position(f, id, "USD"), ==, 0);
	g_assert_cmpint(account_net(f, gains, "USD"), ==, 2000);
	g_assert_cmpint(realised(f, id, "USD"), ==, -2000);

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
	venture_query_set_limit(query, 0);
	legs = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	write_offs = 0;

	for (i = 0; i < legs->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(legs, i);
		gint kind = 0;
		gint64 units = 0;

		g_object_get(row, "kind", &kind, "quantity", &units, NULL);

		if (VENTURE_ARBITRAGE_LEG_KIND_WRITE_OFF == kind)
		{
			g_autofree gchar *cost = money_text(row, "cost");

			write_offs++;
			g_assert_cmpint(units, ==, 6);
			g_assert_cmpstr(cost, ==, "30.00 USD");
		}
	}

	g_assert_cmpuint(write_offs, ==, 1);

	/* Kept: the units stay, at their cost, and only the sale is closed. */
	kept = half_sold(f, &kept_item);
	g_clear_object(&result);
	g_assert_true(venture_arbitrage_abandon(f->db, kept, FALSE, NULL, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(on_hand(f, kept_item), ==, 6);
	g_assert_cmpint(position(f, kept, "USD"), ==, 0);
	g_assert_cmpint(account_net(f, gains, "USD"), ==, 1000);
	g_assert_cmpint(realised(f, kept, "USD"), ==, 1000);

	/* Abandoned is the action's too. */
	assert_balanced(f);
}

/*
 * Abandoning with a write-off counts a deleted leg, as close does: a
 * deleted sell's units left the shelf all the same. Ten bought, four
 * sold and that sell deleted, five more on the shelf from another trade:
 * six are this trade's to write off, and five stay. If this regresses,
 * the write-off takes units that belong to other stock as this trade's
 * loss.
 */
static void
test_abandon_deleted_leg(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sell = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 item;
	gint64 id;
	gint64 other;
	gint64 sold;

	(void)data;
	item = item_for(f, "Runecloth");
	id = trade(f, "Runecloth", "spread");
	execute(f, stock_leg(f, id, f->supplier, "buy", item, 10, "50.00 USD", NULL, "2026-03-05T10:00:00Z"));
	sold = stock_leg(f, id, f->market, "sell", item, 4, "30.00 USD", NULL, "2026-03-06T10:00:00Z");
	execute(f, sold);
	sell = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, sold);
	g_assert_true(venture_database_delete(f->db, sell, NULL, &error));
	g_assert_no_error(error);

	other = trade(f, "Someone else's runecloth", "spread");
	execute(f, stock_leg(f, other, f->supplier, "buy", item, 5, "25.00 USD", NULL, "2026-03-07T10:00:00Z"));
	g_assert_cmpint(on_hand(f, item), ==, 11);

	g_assert_true(venture_arbitrage_abandon(f->db, id, TRUE, NULL, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(on_hand(f, item), ==, 5);
	g_assert_cmpint(position(f, id, "USD"), ==, 0);
	assert_balanced(f);
}

/*
 * A closed trade's closing time is the action's: edited by hand it is put
 * back, because the close journals carry that date and the P&L counts the
 * trade in its period. If this regresses, the result lands in one period
 * and the gains in another.
 */
static void
test_closed_at_kept(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(GDateTime) closed = NULL;
	g_autoptr(GDateTime) expected = NULL;
	gint64 id;

	(void)data;
	id = trade(f, "Dated", "spread");
	leg(f, id, f->market, "buy", "10.00 USD", NULL, "executed", "2026-03-05T10:00:00Z");
	leg(f, id, f->supplier, "sell", "12.00 USD", NULL, "executed", "2026-03-06T10:00:00Z");
	close_at(f, id, "2026-03-07T10:00:00Z");

	row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	field(row, "closed-at", "2026-04-15T10:00:00Z");
	g_object_set(row, "notes", "Moved the close by hand", NULL);
	save(f, row);

	again = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	g_object_get(again, "closed-at", &closed, NULL);
	expected = time_of("2026-03-07T10:00:00Z");
	g_assert_nonnull(closed);
	g_assert_true(g_date_time_equal(closed, expected));
}

/* Receives @quantity units of @item_id at @unit_cost each, outside any
 * trade: stock already on the shelf. */
static void
receive_stock(Fixture *f, gint64 item_id, gint64 quantity, const gchar *unit_cost)
{
	g_autoptr(VentureMoney) cost = venture_money_from_string(unit_cost, NULL, NULL);
	g_autoptr(GDateTime) when = time_of("2026-03-01T09:00:00Z");
	g_autoptr(GError) error = NULL;

	g_assert_nonnull(cost);
	g_assert_true(venture_inventory_service_receive(venture_inventory_service_get(f->db), item_id,
	                                                quantity, cost, when, 0, "seed", NULL, &error));
	g_assert_no_error(error);
}

/*
 * Units made from gold and tickets cost both: a craft gives them sibling
 * layers in one lot, and selling two of them through a trade issues both
 * currencies' shares at once. The leg keeps every part in cost_detail,
 * and the trade's figures count each in its own currency -- so the gold
 * result is what the positions account holds and the close takes to
 * gains, and the tickets' cost is a loss in tickets, as the memo currency
 * it is. Writing the detail by hand is refused like the cost.
 *
 * What breaks if this regresses: a crafted unit cannot be sold through a
 * trade at all (it was refused as "one currency"), or one currency's cost
 * vanishes from the result while the books carry it.
 */
static void
test_cost_in_two_currencies(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GPtrArray) made = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) detail = NULL;
	g_autofree gchar *detail_text = NULL;
	g_autofree gchar *cost = NULL;
	VentureInventoryDraw draws[2];
	JsonObject *parts;
	gint64 dust;
	gint64 token;
	gint64 charm;
	gint64 id;
	gint64 sell;

	(void)data;
	dust = item_for(f, "Dust");
	token = item_for(f, "Token");
	charm = item_for(f, "Charm");
	receive_stock(f, dust, 4, "0.2500 GOLD");
	receive_stock(f, token, 10, "5 TICKET");

	/* 1 GOLD and 50 TICKET into three charms: units of 0.3334 + 17,
	 * 0.3333 + 17 and 0.3333 + 16, one lot. */
	draws[0].inventory_item_id = dust;
	draws[0].quantity = 4;
	draws[1].inventory_item_id = token;
	draws[1].quantity = 10;
	g_assert_true(venture_inventory_service_produce(venture_inventory_service_get(f->db), draws, 2,
		charm, 3, NULL, "recipe:1", NULL, &txn, &made, &error));
	g_assert_no_error(error);

	id = trade(f, "Charms", "transform");
	sell = stock_leg(f, id, f->ah_aria, "sell", charm, 2, "3.0000 GOLD", NULL, "2026-03-06T10:00:00Z");
	execute(f, sell);
	g_assert_cmpint(on_hand(f, charm), ==, 1);

	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, sell);
	cost = money_text(row, "cost");
	g_assert_cmpstr(cost, ==, "0.6667 GOLD");
	g_object_get(row, "cost-detail", &detail_text, NULL);
	detail = json_from_string(detail_text, &error);
	g_assert_no_error(error);
	parts = json_node_get_object(detail);
	g_assert_cmpuint(json_object_get_size(parts), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(parts, "GOLD"), ==, "0.6667");
	g_assert_cmpstr(json_object_get_string_member(parts, "TICKET"), ==, "34");

	/* Each currency's result: gold 3.0000 - 0.6667, tickets -34. */
	g_assert_cmpint(realised(f, id, "GOLD"), ==, 23333);
	g_assert_cmpint(realised(f, id, "TICKET"), ==, -34);

	/* The books agree in gold: the issue's cost in, the proceeds out. */
	g_assert_cmpint(position(f, id, "GOLD"), ==, -23333);
	g_assert_cmpint(position(f, id, "TICKET"), ==, 0);
	close_at(f, id, "2026-03-07T10:00:00Z");
	g_assert_cmpint(account_net(f, arb_account(f, "arbitrage_gains"), "GOLD"), ==, -23333);
	g_assert_cmpint(position(f, id, "GOLD"), ==, 0);
	assert_balanced(f);

	/* The detail is the action's to write, like the cost. */
	g_clear_object(&row);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, sell);
	g_object_set(row, "cost-detail", "{\"GOLD\":\"0.0001\"}", NULL);
	save_refused(f, row, "Execute action");
}

/*
 * A trade's expected profit counts only amounts that name their currency,
 * at their full precision. A bare "12.00" used to be read in the install's
 * default currency -- dollars in a gold organization -- and an object
 * member's 12.75 was read as an integer, 12.
 */
static void
test_expected_profit(Fixture *f, gconstpointer data)
{
	g_autoptr(GPtrArray) bare = NULL;
	g_autoptr(GPtrArray) named = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	g_autoptr(GPtrArray) object = NULL;
	g_autofree gchar *text = NULL;

	(void)f;
	(void)data;

	bare = venture_arbitrage_expected_profit("{\"profit\":\"12.00\"}");
	g_assert_cmpuint(bare->len, ==, 0);

	named = venture_arbitrage_expected_profit("{\"profit\":\"12.0000 GOLD\"}");
	g_assert_cmpuint(named->len, ==, 1);
	text = venture_money_to_string(g_ptr_array_index(named, 0));
	g_assert_cmpstr(text, ==, "12.0000 GOLD");
	g_clear_pointer(&text, g_free);

	listed = venture_arbitrage_expected_profit("{\"profit\":[\"3.00 USD\",\"5\"]}");
	g_assert_cmpuint(listed->len, ==, 1);

	object = venture_arbitrage_expected_profit("{\"profit\":{\"GOLD\":12.75}}");
	g_assert_cmpuint(object->len, ==, 1);
	text = venture_money_to_string(g_ptr_array_index(object, 0));
	g_assert_cmpstr(text, ==, "12.7500 GOLD");
}

/*
 * Deleting a leg, and then the trade, removes nothing from the books: a
 * deletion is not a correction. The position still counts the deleted
 * leg's journals. If this regresses, deleting a record silently rewrites
 * a closed period.
 */
static void
test_delete_keeps_journals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 buy;
	guint journals;

	(void)data;
	id = trade(f, "Doomed", "spread");
	buy = leg(f, id, f->market, "buy", "10.00 USD", NULL, "executed", "2026-03-05T10:00:00Z");
	leg(f, id, f->supplier, "sell", "12.00 USD", NULL, "executed", "2026-03-06T10:00:00Z");
	journals = count_rows(f, VENTURE_TYPE_JOURNAL);

	row = reread(f, VENTURE_TYPE_ARBITRAGE_LEG, buy);
	g_assert_true(venture_database_delete(f->db, row, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, journals);
	g_assert_cmpuint(posted_journals(f, "arbitrage_leg", buy), ==, 1);
	g_assert_cmpint(position(f, id, "USD"), ==, -200);

	g_clear_object(&row);
	row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	g_assert_true(venture_database_delete(f->db, row, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, journals);
	assert_balanced(f);
}

/*
 * What a leg refuses. If this regresses, a negative purchase posts as a
 * sale, fees in another currency are added to the amount, a venue that
 * trades in gold takes tickets, or an executed leg posts nothing.
 */
static void
test_leg_rules(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *amount = NULL;
	gint64 id;

	(void)data;
	id = trade(f, "Rules", "spread");

	row = leg_new(f, id, f->market, "buy", "-5.00 USD", NULL, NULL, NULL);
	save_refused(f, row, "cannot be negative");
	g_clear_object(&row);

	row = leg_new(f, id, f->market, "buy", "5.00 USD", "1.00 EUR", NULL, NULL);
	save_refused(f, row, "fee leg of their own");
	g_clear_object(&row);

	row = leg_new(f, id, f->ah_aria, "buy", "5 TICKET", NULL, NULL, NULL);
	save_refused(f, row, "trades in GOLD");
	g_clear_object(&row);

	row = leg_new(f, id, f->market, "buy", NULL, NULL, "executed", NULL);
	save_refused(f, row, "needs the money");
	g_clear_object(&row);

	row = leg_new(f, id, f->market, "fee", "1.00 USD", NULL, NULL, NULL);
	g_object_set(row, "inventory-item-id", item_for(f, "Wrong"), "quantity", (gint64)1, NULL);
	save_refused(f, row, "only a buy or a sell moves stock");
	g_clear_object(&row);

	/* Stock given to a buy that already executed as cash: the re-save
	 * would hand the cash back with no units arriving. */
	row = leg_new(f, id, f->market, "buy", "7.00 USD", NULL, "executed", NULL);
	save(f, row);
	g_object_set(row, "inventory-item-id", item_for(f, "Late"), "quantity", (gint64)1, NULL);
	save_refused(f, row, "only Execute moves");
	g_clear_object(&row);

	/* A transfer arriving is negative, and that is fine. */
	leg(f, id, f->market, "transfer", "-5.00 USD", NULL, NULL, NULL);

	/* A unit price and a quantity fill an empty amount. */
	row = leg_new(f, id, f->market, "buy", NULL, NULL, NULL, NULL);
	field(row, "unit-price", "2.50 USD");
	g_object_set(row, "quantity", (gint64)4, NULL);
	save(f, row);
	amount = money_text(row, "amount");
	g_assert_cmpstr(amount, ==, "10.00 USD");
}

/* ==========================================================================
 * Recording an attempt
 * ========================================================================== */

static JsonObject *
request_for(Fixture *f, const gchar *name)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;

	/* Bare amounts: read in the venue's currency, gold. */
	text = g_strdup_printf(
		"{\"name\":\"%s\",\"strategy\":\"spread\",\"venture_id\":%" G_GINT64_FORMAT ","
		"\"expected\":{\"profit\":[\"40 GOLD\"]},"
		"\"legs\":["
		"{\"kind\":\"buy\",\"venue_id\":%" G_GINT64_FORMAT ",\"amount\":\"100\","
		"\"status\":\"executed\",\"occurred_at\":\"2026-03-02T10:00:00Z\"},"
		"{\"kind\":\"sell\",\"venue_id\":%" G_GINT64_FORMAT ",\"amount\":\"150\","
		"\"status\":\"executed\",\"occurred_at\":\"2026-03-03T10:00:00Z\"},"
		"{\"kind\":\"fee\",\"venue_id\":%" G_GINT64_FORMAT ",\"amount\":\"5\"}]}",
		name, f->venture, f->ah_aria, f->ah_bram, f->ah_bram);
	node = venture_json_parse(text, &error);
	g_assert_no_error(error);
	return json_object_ref(json_node_get_object(node));
}

/*
 * Recording creates the trade and its legs in one transaction and
 * executes the legs marked executed in order, reading bare amounts in the
 * venue's currency. A leg naming a stamp is refused. Staged, nothing is
 * written until a second person approves, and approval performs it
 * afresh. If this regresses, a proposed trade reaches the books before
 * anybody approved it, or "100" at a gold venue is recorded as dollars.
 */
static void
test_record(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) request = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) placeholder = NULL;
	g_autofree gchar *confirmation_id = NULL;
	VentureActionRegistry *registry;
	VentureConfirmation *confirmation;
	guint trades;
	guint i;

	(void)data;
	fund(f, f->aria, "500 GOLD");
	request = request_for(f, "Recorded flip");
	result = venture_arbitrage_record(f->db, f->org, request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, ID(result), "status"), ==,
	                VENTURE_ARBITRAGE_TRADE_STATUS_OPEN);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);

		venture_query_set_limit(query, 0);
		venture_query_add_filter_int(query, "trade-id", VENTURE_FILTER_OP_EQ, ID(result), NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		legs = venture_database_find(f->db, query, &error);
		g_assert_no_error(error);
	}
	g_assert_cmpuint(legs->len, ==, 3);

	for (i = 0; i < legs->len; i++)
	{
		g_autofree gchar *amount = money_text(g_ptr_array_index(legs, i), "amount");

		g_assert_true(g_str_has_suffix(amount, " GOLD"));
	}

	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_LEG, ID(g_ptr_array_index(legs, 2)), "status"),
	                ==, VENTURE_ARBITRAGE_LEG_STATUS_PLANNED);
	g_assert_cmpint(position(f, ID(result), "GOLD"), ==, -500000);
	g_assert_cmpint(held(f, f->aria, "GOLD"), ==, 4000000);

	/* A stamp in a leg is refused, and nothing of the request is kept. */
	trades = count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE);
	{
		g_autoptr(JsonObject) bad = request_for(f, "Bad");
		JsonObject *first = json_array_get_object_element(json_object_get_array_member(bad, "legs"), 0);

		json_object_set_int_member(first, "inventory_txn_id", 4);
		g_clear_object(&result);
		result = venture_arbitrage_record(f->db, f->org, bad, NULL, &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "inventory_txn_id"));
		g_clear_error(&error);
	}
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, trades);

	/* Staged: nothing until approved, then performed. */
	registry = venture_database_get_action_registry(f->db);
	g_clear_pointer(&request, json_object_unref);
	request = request_for(f, "Staged flip");
	{
		g_autoptr(JsonNode) body = json_node_new(JSON_NODE_OBJECT);

		json_node_set_object(body, request);
		json_object_set_int_member(request, "organization_id", f->org);
		params = venture_action_parameters_from_json(body, &error);
		g_assert_no_error(error);
	}
	placeholder = VENTURE_ENTITY(venture_arbitrage_trade_new());
	confirmation = venture_confirmation_store_stage_action(venture_context_get_confirmations(f->context),
		venture_action_registry_lookup(registry, "arbitrage_trade", "record"), placeholder, params,
		NULL, VENTURE_USER_ROLE_OWNER, "test", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	confirmation_id = g_strdup(venture_confirmation_get_id(confirmation));
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, trades);
	g_assert_true(venture_confirmation_store_approve_as(venture_context_get_confirmations(f->context),
		confirmation_id, "reviewer", VENTURE_USER_ROLE_OWNER, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, trades + 1);
	g_assert_cmpint(held(f, f->aria, "GOLD"), ==, 3000000);
	assert_balanced(f);
}

/* A member of the organization with @role, as the principal a request carries. */
static void
member(Fixture *f, gint role, const gchar *username, VentureAuthPrincipal *principal)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", username,
		"active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) membership = NULL;

	save(f, user);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", ID(user),
		"organization-id", f->org, "role", role, "active", TRUE, NULL);
	save(f, membership);
	principal->authenticated = TRUE;
	principal->user_id = ID(user);
	principal->token_id = 0;
	principal->role = VENTURE_USER_ROLE_EDITOR;
	principal->name = NULL;
}

/* Performs `record` as @principal inside its request scope. */
static VentureEntity *
record_as(Fixture *f, VentureAuthPrincipal *principal, gint64 organization_id, GError **error)
{
	g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(
		venture_database_get_access_policy(f->db), principal);
	g_autoptr(JsonObject) request = request_for(f, "As a member");
	g_autoptr(JsonNode) body = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GHashTable) params = NULL;
	VentureActor actor;

	/* No executed legs: the role check is the question, not the purse. */
	json_object_remove_member(request, "legs");
	{
		g_autofree gchar *text = g_strdup_printf("[{\"kind\":\"buy\",\"venue_id\":%"
			G_GINT64_FORMAT ",\"amount\":\"10\"}]", f->ah_aria);
		json_object_set_member(request, "legs", venture_json_parse(text, NULL));
	}

	if (0 != organization_id)
		json_object_set_int_member(request, "organization_id", organization_id);

	json_node_set_object(body, request);
	params = venture_action_parameters_from_json(body, error);
	g_assert_nonnull(params);
	venture_auth_to_actor(principal, &actor);
	return venture_action_registry_perform(venture_database_get_action_registry(f->db),
		"arbitrage_trade", 0, "record", params, &actor, principal->role, error);
}

/*
 * `record` is judged in the organization it names. Its finance member
 * records; an editor member does not, because arbitrage moves money; a
 * member who names no organization is told to name one. If this
 * regresses, an organization's own trader cannot record a trade, or
 * anybody with an account in it can post to its books.
 */
static void
test_record_roles(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal finance;
	VentureAuthPrincipal editor;

	(void)data;
	member(f, VENTURE_ORGANIZATION_ROLE_FINANCE, "trader", &finance);
	member(f, VENTURE_ORGANIZATION_ROLE_EDITOR, "editor", &editor);

	result = record_as(f, &finance, f->org, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(venture_entity_get_organization_id(result), ==, f->org);
	g_clear_object(&result);

	result = record_as(f, &editor, f->org, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	result = record_as(f, &finance, 0, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "organization_id"));
}

/* Performs a record action on @type_name #@id as @actor (an owner). */
static VentureEntity *
act(Fixture *f, const gchar *type_name, gint64 id, const gchar *name, const VentureActor *actor,
	GError **error)
{
	g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)json_node_unref);

	return venture_action_registry_perform(venture_database_get_action_registry(f->db), type_name,
		id, name, params, actor, VENTURE_USER_ROLE_OWNER, error);
}

/*
 * With a second-actor rule on posting, executing a leg and closing a trade
 * through the action framework are proposals the first time and performed
 * by a second person -- not refused for beginning consent inside their own
 * transaction, which is what an action that opened the posting boundary
 * after its transaction would do. If this regresses, an organization that
 * requires a second approver cannot execute or close a trade at all, or a
 * single person posts to its books alone.
 */
static void
test_second_actor(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(VentureQuery) approvals = venture_query_new(VENTURE_TYPE_ACCOUNTING_APPROVAL);
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor alice;
	VentureActor bob;
	gint64 id;
	gint64 buy;

	(void)data;
	id = trade(f, "Guarded", "spread");
	buy = leg(f, id, f->market, "buy", "10.00 USD", NULL, NULL, "2026-03-05T10:00:00Z");
	leg(f, id, f->supplier, "sell", "12.00 USD", NULL, "executed", "2026-03-06T10:00:00Z");
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	save(f, rule);

	alice.kind = VENTURE_ACTOR_KIND_USER; alice.name = "alice";
	alice.prompt = NULL; alice.request_id = NULL; alice.approved_by = NULL;
	bob.kind = VENTURE_ACTOR_KIND_USER; bob.name = "bob";
	bob.prompt = NULL; bob.request_id = NULL; bob.approved_by = NULL;

	result = act(f, "arbitrage_leg", buy, "execute", &alice, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(venture_database_count(f->db, approvals, NULL), ==, 1);
	g_assert_cmpuint(journals_for(f, "arbitrage_leg", buy)->len, ==, 0);

	result = act(f, "arbitrage_leg", buy, "execute", &bob, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(posted_journals(f, "arbitrage_leg", buy), ==, 1);
	g_clear_object(&result);

	result = act(f, "arbitrage_trade", id, "close", &alice, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "status"), ==,
	                VENTURE_ARBITRAGE_TRADE_STATUS_OPEN);

	result = act(f, "arbitrage_trade", id, "close", &bob, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_TRADE, id, "status"), ==,
	                VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED);
	g_assert_cmpint(position(f, id, "USD"), ==, 0);
	assert_balanced(f);
}

/* ==========================================================================
 * The report
 * ========================================================================== */

static VentureReportResult *
performance(Fixture *f, const gchar *group_by, const gchar *strategy, GError **error)
{
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) period = venture_date_range_new_month(2026, 3, NULL);
	VentureReport *report;

	if (NULL != group_by)
		json_object_set_string_member(options, "group_by", group_by);

	if (NULL != strategy)
		json_object_set_string_member(options, "strategy", strategy);

	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context),
	                                        "arbitrage_performance");
	g_assert_nonnull(report);
	return venture_report_generate(report, f->context, period, options, error);
}

static const gchar *
cell_text(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);

	return ((NULL != value) && G_VALUE_HOLDS_STRING(value)) ? g_value_get_string(value) : NULL;
}

static gchar *
cell_money(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);

	if ((NULL == value) || !G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY) || (NULL == g_value_get_boxed(value)))
		return NULL;

	return venture_money_to_string(g_value_get_boxed(value));
}

static gdouble
cell_number(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);

	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS_DOUBLE(value));
	return g_value_get_double(value);
}

static guint
row_of(VentureReportResult *result, const gchar *group, const gchar *currency)
{
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
		if ((0 == g_strcmp0(cell_text(result, i, "group"), group)) &&
		    (0 == g_strcmp0(cell_text(result, i, "currency"), currency)))
			return i;

	g_error("no row for %s in %s", group, currency);
	return 0;
}

#define ASSERT_MONEY(result, row, key, expected) G_STMT_START { \
	g_autofree gchar *assert_money_text = cell_money(result, row, key); \
	g_assert_cmpstr(assert_money_text, ==, expected); \
} G_STMT_END

/* A closed dollar flip: buy @out, sell @in, fees @fees. */
static gint64
closed_flip(Fixture *f, const gchar *name, const gchar *strategy, const gchar *out,
	const gchar *in, const gchar *fees, const gchar *expected, const gchar *closed)
{
	g_autoptr(VentureEntity) row = NULL;
	gint64 id;

	id = trade(f, name, strategy);

	if (NULL != expected)
	{
		row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
		g_object_set(row, "expected", expected, NULL);
		save(f, row);
	}

	leg(f, id, f->market, "buy", out, fees, "executed", "2026-03-05T10:00:00Z");
	leg(f, id, f->supplier, "sell", in, NULL, "executed", "2026-03-06T10:00:00Z");
	close_at(f, id, closed);
	return id;
}

/*
 * The report's figures are the page's: realised, capital, ROI, fees, hit
 * rate and expected against realised, per group and currency, the book
 * currency first, never a sum across currencies; open trades and trades
 * finished outside the period are left out. If this regresses, a gold
 * profit is added to dollars, a losing trade counts as a hit, or the
 * expected figure of one trade is compared with the result of two.
 */
static void
test_performance(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 cover;
	guint row;

	(void)data;
	fund(f, f->aria, "500 GOLD");
	closed_flip(f, "Win", "spread", "100.00 USD", "120.00 USD", "2.00 USD",
	            "{\"profit\":\"15.00 USD\"}", "2026-03-10T10:00:00Z");
	closed_flip(f, "Loss", "spread", "100.00 USD", "90.00 USD", NULL, NULL, "2026-03-11T10:00:00Z");
	closed_flip(f, "April", "spread", "100.00 USD", "200.00 USD", NULL, NULL, "2026-04-02T10:00:00Z");

	cover = trade(f, "Cover", "cover");
	leg(f, cover, f->ah_aria, "stake", "50 GOLD", NULL, "executed", "2026-03-05T10:00:00Z");
	leg(f, cover, f->ah_aria, "payout", "60 GOLD", NULL, "executed", "2026-03-07T10:00:00Z");
	close_at(f, cover, "2026-03-12T10:00:00Z");

	/* Still open: not in the report. */
	leg(f, trade(f, "Open", "spread"), f->market, "buy", "1.00 USD", NULL, "executed",
	    "2026-03-05T10:00:00Z");

	result = performance(f, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);

	row = row_of(result, "spread", "USD");
	g_assert_cmpfloat(cell_number(result, row, "trades"), ==, 2.0);
	g_assert_cmpfloat(cell_number(result, row, "wins"), ==, 1.0);
	g_assert_cmpfloat(cell_number(result, row, "hit_rate"), ==, 0.5);
	ASSERT_MONEY(result, row, "realised", "8.00 USD");
	ASSERT_MONEY(result, row, "capital", "200.00 USD");
	ASSERT_MONEY(result, row, "fees", "2.00 USD");
	g_assert_cmpfloat_with_epsilon(cell_number(result, row, "roi"), 0.04, 1e-9);
	ASSERT_MONEY(result, row, "expected", "15.00 USD");
	ASSERT_MONEY(result, row, "slippage", "3.00 USD");

	row = row_of(result, "cover", "GOLD");
	ASSERT_MONEY(result, row, "realised", "10.0000 GOLD");
	g_assert_cmpfloat(cell_number(result, row, "hit_rate"), ==, 1.0);
	g_assert_null(cell_money(result, row, "expected"));
	g_clear_object(&result);

	/* One strategy, narrowed in the query. */
	result = performance(f, NULL, "cover", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_clear_object(&result);

	/* By month: the book currency first. */
	result = performance(f, "month", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	g_assert_cmpstr(cell_text(result, 0, "group"), ==, "2026-03");
	g_assert_cmpstr(cell_text(result, 0, "currency"), ==, "USD");
	g_assert_cmpstr(cell_text(result, 1, "currency"), ==, "GOLD");
	g_clear_object(&result);

	/* By venue pair: where the money went out, and where it came back. */
	result = performance(f, "venue_pair", NULL, &error);
	g_assert_no_error(error);
	row = row_of(result, "Market \xe2\x86\x92 Supplier", "USD");
	ASSERT_MONEY(result, row, "realised", "8.00 USD");
	row = row_of(result, "Argent Dawn AH", "GOLD");
	g_clear_object(&result);

	result = performance(f, "colour", NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* Runs report @name over @period_text with an optional venture. */
static VentureReportResult *
operational(Fixture *f, const gchar *name, const gchar *period_text, gint64 venture_id)
{
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;
	VentureReport *report;

	if (venture_id > 0)
		json_object_set_int_member(options, "venture_id", venture_id);

	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	g_assert_nonnull(report);
	period = venture_context_parse_period(f->context, period_text, &error);
	g_assert_no_error(error);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

/* A P&L line's amount in @currency, as text; NULL when there is no such line. */
static gchar *
pnl_line(VentureReportResult *result, const gchar *line, const gchar *currency)
{
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
		if ((0 == g_strcmp0(cell_text(result, i, "line"), line)) &&
		    (0 == g_strcmp0(cell_text(result, i, "currency"), currency)))
			return cell_money(result, i, "amount");

	return NULL;
}

/* A metric's money as text, or NULL. */
static gchar *
metric_text(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric = g_ptr_array_index(metrics, i);

		if ((0 == g_strcmp0(venture_metric_get_key(metric), key)) &&
		    (NULL != venture_metric_get_money(metric)))
			return venture_money_to_string(venture_metric_get_money(metric));
	}

	return NULL;
}

/* The row of @report whose @key column reads @value in @currency. */
static guint
row_where(VentureReportResult *result, const gchar *key, const gchar *value, const gchar *currency)
{
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
		if ((0 == g_strcmp0(cell_text(result, i, key), value)) &&
		    (0 == g_strcmp0(cell_text(result, i, "currency"), currency)))
			return i;

	g_error("no %s row %s in %s", key, value, currency);
	return 0;
}

#define ASSERT_LINE(result, line, currency, expected) G_STMT_START { \
	g_autofree gchar *assert_line_text = pnl_line(result, line, currency); \
	g_assert_cmpstr(assert_line_text, ==, expected); \
} G_STMT_END

#define ASSERT_METRIC(result, key, expected) G_STMT_START { \
	g_autofree gchar *assert_metric_text = metric_text(result, key); \
	g_assert_cmpstr(assert_metric_text, ==, expected); \
} G_STMT_END

/*
 * The P&L asked "as of" a date counts the arbitrage trades visible then,
 * as it does the sales beside them: a trade deleted since still counts,
 * and the live P&L leaves it out. The arbitrage lines ignored as_of, so a
 * P&L reproduced as of a past date moved with every later deletion.
 */
static void
test_pnl_as_of(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) live = NULL;
	g_autoptr(VentureReportResult) then = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *gone = NULL;
	VentureReport *report;
	gint64 id;

	(void)data;
	id = closed_flip(f, "Deleted later", "spread", "100.00 USD", "120.00 USD", NULL, NULL,
	                 "2026-03-10T10:00:00Z");
	row = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, id);
	g_assert_true(venture_database_delete(f->db, row, NULL, &error));
	g_assert_no_error(error);

	live = operational(f, "pnl", "2026-03", 0);
	gone = pnl_line(live, "Arbitrage result", "USD");
	g_assert_null(gone);

	json_object_set_string_member(options, "as_of", "2026-03-31");
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "pnl");
	period = venture_context_parse_period(f->context, "2026-03", &error);
	g_assert_no_error(error);
	then = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	ASSERT_LINE(then, "Arbitrage result", "USD", "20.00 USD");
}

/*
 * The operational reports count realised arbitrage, which the ledger's
 * income statement always did. March finishes two dollar flips under
 * Flips (18.00 made on 2.00 of fees, 10.00 lost: 10.00 of gains before
 * fees, a result of 8.00), one under Other (1.00) and a gold cover (10
 * GOLD); April finishes one more (100.00), outside March. Beside them a
 * 50.00 sale and a 5.00 expense: the P&L's profit is 50.00 - 5.00 +
 * 9.00, the gold block's is the trade's 10 GOLD, the venture filter
 * keeps Other's dollar out, and `ventures` and `monthly` carry the same
 * result in a column of their own. A month that finished no trade and
 * the module switched off read as they always did, without an error.
 * What breaks if this regresses: report pnl and the income statement
 * disagree by every trade's result, a gold profit is added to dollars,
 * or a sale's leg is counted twice.
 */
static void
test_operational_reports(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(VentureEntity) spent = NULL;
	g_autoptr(VentureVenture) other = venture_venture_new();
	g_autoptr(VentureEntity) side = NULL;
	gint64 cover;
	gint64 small;
	guint row;

	(void)data;
	fund(f, f->aria, "500 GOLD");
	closed_flip(f, "Win", "spread", "100.00 USD", "120.00 USD", "2.00 USD", NULL, "2026-03-10T10:00:00Z");
	closed_flip(f, "Loss", "spread", "100.00 USD", "90.00 USD", NULL, NULL, "2026-03-11T10:00:00Z");
	closed_flip(f, "April", "spread", "100.00 USD", "200.00 USD", NULL, NULL, "2026-04-02T10:00:00Z");

	cover = trade(f, "Cover", "cover");
	leg(f, cover, f->ah_aria, "stake", "50 GOLD", NULL, "executed", "2026-03-05T10:00:00Z");
	leg(f, cover, f->ah_aria, "payout", "60 GOLD", NULL, "executed", "2026-03-07T10:00:00Z");
	close_at(f, cover, "2026-03-12T10:00:00Z");

	g_object_set(other, "name", "Other", "organization-id", f->org, NULL);
	save(f, other);
	small = trade(f, "Side flip", "spread");
	side = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, small);
	g_object_set(side, "venture-id", ID(other), NULL);
	save(f, side);
	leg(f, small, f->market, "buy", "10.00 USD", NULL, "executed", "2026-03-05T10:00:00Z");
	leg(f, small, f->supplier, "sell", "11.00 USD", NULL, "executed", "2026-03-06T10:00:00Z");
	close_at(f, small, "2026-03-13T10:00:00Z");

	/* Still open: not finished, not counted. */
	leg(f, trade(f, "Open", "spread"), f->market, "buy", "1.00 USD", NULL, "executed",
	    "2026-03-05T10:00:00Z");

	sale = record(f, "sale");
	g_object_set(sale, "venture-id", f->venture, NULL);
	field(sale, "occurred-at", "2026-03-08");
	field(sale, "gross", "50.00 USD");
	save(f, sale);
	spent = record(f, "expense");
	g_object_set(spent, "venture-id", f->venture, "description", "Bags", NULL);
	field(spent, "occurred-at", "2026-03-09");
	field(spent, "amount", "5.00 USD");
	save(f, spent);

	/* The whole organization: every venture's trades. */
	result = operational(f, "pnl", "2026-03", 0);
	ASSERT_LINE(result, "Net revenue", "USD", "50.00 USD");
	ASSERT_LINE(result, "Expenses", "USD", "5.00 USD");
	ASSERT_LINE(result, "Arbitrage gains", "USD", "11.00 USD");
	ASSERT_LINE(result, "Less arbitrage fees", "USD", "2.00 USD");
	ASSERT_LINE(result, "Arbitrage result", "USD", "9.00 USD");
	ASSERT_LINE(result, "Profit", "USD", "54.00 USD");
	ASSERT_LINE(result, "Arbitrage result", "GOLD", "10.0000 GOLD");
	ASSERT_LINE(result, "Profit", "GOLD", "10.0000 GOLD");
	g_assert_cmpstr(cell_text(result, 0, "currency"), ==, "USD");
	ASSERT_METRIC(result, "arbitrage", "9.00 USD");
	ASSERT_METRIC(result, "arbitrage_GOLD", "10.0000 GOLD");
	ASSERT_METRIC(result, "profit", "54.00 USD");
	g_clear_object(&result);

	/* One venture: Other's dollar stays out. */
	result = operational(f, "pnl", "2026-03", f->venture);
	ASSERT_LINE(result, "Arbitrage result", "USD", "8.00 USD");
	ASSERT_LINE(result, "Profit", "USD", "53.00 USD");
	g_clear_object(&result);

	/* Ventures: a column of its own, in the venture's rows. */
	result = operational(f, "ventures", "2026-03", 0);
	row = row_where(result, "venture", "Flips", "USD");
	ASSERT_MONEY(result, row, "arbitrage", "8.00 USD");
	ASSERT_MONEY(result, row, "profit", "53.00 USD");
	row = row_where(result, "venture", "Flips", "GOLD");
	ASSERT_MONEY(result, row, "arbitrage", "10.0000 GOLD");
	row = row_where(result, "venture", "Other", "USD");
	ASSERT_MONEY(result, row, "arbitrage", "1.00 USD");
	ASSERT_MONEY(result, row, "profit", "1.00 USD");
	g_clear_object(&result);

	/* Monthly: each trade in the month it finished. Rows run month by
	 * month, the book currency first in each: March's dollars, March's
	 * gold, April's dollars. */
	result = operational(f, "monthly", "2026-03-01..2026-04-30", 0);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	g_assert_cmpstr(cell_text(result, 0, "currency"), ==, "USD");
	ASSERT_MONEY(result, 0, "arbitrage", "9.00 USD");
	ASSERT_MONEY(result, 0, "profit", "54.00 USD");
	g_assert_cmpstr(cell_text(result, 1, "currency"), ==, "GOLD");
	ASSERT_MONEY(result, 1, "arbitrage", "10.0000 GOLD");
	g_assert_cmpstr(cell_text(result, 2, "currency"), ==, "USD");
	ASSERT_MONEY(result, 2, "arbitrage", "100.00 USD");
	ASSERT_MONEY(result, 2, "profit", "100.00 USD");
	g_clear_object(&result);

	/* A month that finished no trade reads as it always did. */
	result = operational(f, "pnl", "2026-02", 0);
	g_assert_null(pnl_line(result, "Arbitrage result", "USD"));
	g_assert_null(metric_text(result, "arbitrage"));
	g_clear_object(&result);
	result = operational(f, "ventures", "2026-02", 0);
	g_assert_null(cell_money(result, 0, "arbitrage"));
	g_clear_object(&result);

	/* Off: no lines, no error, the profit of sales and expenses alone. */
	venture_config_set_module_enabled(f->config, "arbitrage", FALSE);
	result = operational(f, "pnl", "2026-03", 0);
	g_assert_null(pnl_line(result, "Arbitrage result", "USD"));
	ASSERT_LINE(result, "Profit", "USD", "45.00 USD");
	g_clear_object(&result);
	result = operational(f, "monthly", "2026-03", 0);
	g_assert_null(cell_money(result, 0, "arbitrage"));
	ASSERT_MONEY(result, 0, "profit", "45.00 USD");
	g_clear_object(&result);
	venture_config_set_module_enabled(f->config, "arbitrage", TRUE);
}

/*
 * The module switches off cleanly: its types, report and actions are
 * hidden, and come back when it is on; marketdata off takes it off too,
 * because a leg names venues. If this regresses, a hidden module still
 * offers a Close button, or its report answers about a type that is off.
 */
static void
test_module_off(Fixture *f, gconstpointer data)
{
	VentureEntityRegistry *types;
	VentureReportRegistry *reports;
	VentureActionRegistry *actions;

	(void)data;
	types = venture_context_get_entity_registry(f->context);
	reports = venture_context_get_report_registry(f->context);
	actions = venture_database_get_action_registry(f->db);
	g_assert_nonnull(venture_action_registry_lookup(actions, "arbitrage_trade", "close"));

	venture_config_set_module_enabled(f->config, "arbitrage", FALSE);
	g_assert_false(venture_context_module_enabled(f->context, "arbitrage"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "arbitrage_trade"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "arbitrage_leg"));
	g_assert_null(venture_report_registry_lookup(reports, "arbitrage_performance"));
	g_assert_null(venture_action_registry_lookup(actions, "arbitrage_trade", "close"));

	venture_config_set_module_enabled(f->config, "arbitrage", TRUE);
	g_assert_nonnull(venture_report_registry_lookup(reports, "arbitrage_performance"));
	g_assert_nonnull(venture_action_registry_lookup(actions, "arbitrage_leg", "execute"));

	venture_config_set_module_enabled(f->config, "marketdata", FALSE);
	g_assert_false(venture_context_module_enabled(f->context, "arbitrage"));
	venture_config_set_module_enabled(f->config, "marketdata", TRUE);
	g_assert_true(venture_context_module_enabled(f->context, "arbitrage"));
}

/* ==========================================================================
 * The page and the web doors
 * ========================================================================== */

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/* One request on this thread's main context, which the server answering
 * it runs on too; redirects are not followed. */
static guint
http(VentureWebServer *server, SoupSession *session, const gchar *method, const gchar *path,
	const gchar *form, gchar **out_body)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new(method, url);
	g_assert_nonnull(message);

	if (NULL != form)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));

	g_bytes_unref(reply.body);
	return soup_message_get_status(message);
}

/*
 * The trade page draws its own block -- the result per currency, the
 * position, the journals, the legs with Execute beside the ones that can
 * be -- with every name escaped and every button named, and a deleted
 * leg marked rather than hidden. Execute from the page performs the
 * action. The report's new option reaches the web API and the report
 * page. If this regresses, a venue named like a script runs on the trade
 * page, the page shows figures the report disagrees with, or the
 * strategy filter is silently dropped on the web door.
 */
static void
test_trade_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(VentureEntity) hostile = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *form_action = NULL;
	g_autofree gchar *answer = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	const gchar *cursor;
	gint64 id;
	gint64 sell;
	gint64 cover;

	(void)data;
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
	             "security-require-auth", FALSE, NULL);

	hostile = record(f, "venue");
	g_object_set(hostile, "name", "<script>alert(1)</script>", "currency", "USD", NULL);
	save(f, hostile);

	id = trade(f, "Page flip", "spread");
	leg(f, id, ID(hostile), "buy", "100.00 USD", "2.00 USD", "executed", "2026-03-05T10:00:00Z");
	sell = leg(f, id, f->supplier, "sell", "130.00 USD", NULL, NULL, "2026-03-06T10:00:00Z");
	fee = leg_new(f, id, f->market, "fee", "1.00 USD", NULL, "executed", "2026-03-05T11:00:00Z");
	save(f, fee);
	g_assert_true(venture_database_delete(f->db, fee, NULL, &error));
	g_assert_no_error(error);

	cover = trade(f, "Cover", "cover");
	leg(f, cover, f->market, "stake", "10.00 USD", NULL, "executed", "2026-03-05T10:00:00Z");
	close_at(f, cover, "2026-03-09T10:00:00Z");

	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	session = soup_session_new_with_options("timeout", 30, NULL);

	path = g_strdup_printf("/e/arbitrage_trade/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(http(server, session, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "arbitrage-figures"));
	g_assert_nonnull(strstr(page, "arbitrage-legs"));
	g_assert_null(strstr(page, "<script>alert(1)"));
	g_assert_nonnull(strstr(page, "&lt;script&gt;"));
	g_assert_nonnull(strstr(page, "(deleted)"));
	g_assert_nonnull(strstr(page, "href=\"/e/journal/"));
	/* Only the buy is on the position: 100.00 still out. */
	g_assert_nonnull(strstr(page, " still out"));
	form_action = g_strdup_printf("/api/v1/arbitrage_leg/%" G_GINT64_FORMAT "/actions/execute", sell);
	g_assert_nonnull(strstr(page, form_action));

	/* Every button is named. */
	for (cursor = strstr(page, "<button"); NULL != cursor; cursor = strstr(cursor + 1, "<button"))
	{
		const gchar *open_end = strchr(cursor, '>');
		const gchar *close = strstr(cursor, "</button>");

		g_assert_nonnull(open_end);
		g_assert_nonnull(close);
		g_assert_true((close - open_end > 1) || (NULL != g_strstr_len(cursor, open_end - cursor, "aria-label")));
	}

	/* Execute from the page performs the action. */
	g_assert_cmpuint(http(server, session, "POST", form_action, "occurred_at=2026-03-06T10:00:00Z", NULL),
	                 ==, 303);
	g_assert_cmpint(enum_of(f, VENTURE_TYPE_ARBITRAGE_LEG, sell, "status"), ==,
	                VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED);
	g_assert_cmpint(position(f, id, "USD"), ==, -3000);

	/* The report's strategy option reaches the web API... */
	close_at(f, id, "2026-03-10T10:00:00Z");
	g_assert_cmpuint(http(server, session, "GET",
		"/api/v1/reports/arbitrage_performance?period=2026-03&strategy=cover", NULL, &answer), ==, 200);
	parsed = venture_json_parse(answer, &error);
	g_assert_no_error(error);
	{
		JsonArray *rows = json_object_get_array_member(json_node_get_object(parsed), "rows");

		g_assert_cmpuint(json_array_get_length(rows), ==, 1);
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "group"),
		                ==, "cover");
	}

	/* ...and the report page offers it and keeps it. */
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(http(server, session, "GET",
		"/reports/arbitrage_performance?period=2026-03&strategy=spread&group_by=month", NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "name=\"strategy\" value=\"spread\""));
	g_assert_nonnull(strstr(page, "<option value=\"month\" selected>"));
	g_assert_nonnull(strstr(page, "2026-03"));

	venture_web_server_stop(server);
}

/*
 * Reading a trade never adds to the chart. An organization that has never
 * executed a leg has no arbitrage accounts; looking at a trade -- its
 * summary, its position, its page -- and running arbitrage_performance or
 * the P&L over it must leave it with none. If this regresses, a viewer
 * opening a planned trade writes `<org>:1460` into the chart (an audited
 * write under a reader's name, and a GET that is not safe).
 */
static void
test_reads_make_no_accounts(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) pnl = NULL;
	g_autoptr(JsonNode) summary = NULL;
	g_autoptr(GPtrArray) balances = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	guint accounts;
	gint64 id;
	gint64 found = -1;

	(void)data;
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
	             "security-require-auth", FALSE, NULL);

	id = trade(f, "Only a plan", "spread");
	leg(f, id, f->market, "buy", "100.00 USD", "2.00 USD", NULL, "2026-03-05T10:00:00Z");
	accounts = count_rows(f, VENTURE_TYPE_ACCOUNT);
	g_assert_cmpint(account_by_code(f, "1460"), ==, 0);

	/* The lookup itself answers "none" without an error. */
	g_assert_true(venture_arbitrage_find_account(f->db, f->org, "arbitrage_positions", NULL,
	                                             &found, &error));
	g_assert_no_error(error);
	g_assert_cmpint(found, ==, 0);

	summary = venture_arbitrage_trade_summary(f->db, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(summary);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		json_node_get_object(summary), "position")), ==, 0);

	balances = venture_arbitrage_position(f->db, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balances);
	g_assert_cmpuint(balances->len, ==, 0);

	result = performance(f, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	pnl = operational(f, "pnl", "2026-03", 0);

	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	session = soup_session_new_with_options("timeout", 30, NULL);
	path = g_strdup_printf("/e/arbitrage_trade/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(http(server, session, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "arbitrage-legs"));

	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ACCOUNT), ==, accounts);
	g_assert_cmpint(account_by_code(f, "1460"), ==, 0);

	/* Executing the leg is a write, and makes the account it posts to. */
	execute(f, leg(f, id, f->market, "fee", "1.00 USD", NULL, NULL, "2026-03-05T11:00:00Z"));
	g_assert_cmpint(account_by_code(f, "6960"), >, 0);
	venture_web_server_stop(server);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/arbitrage-ledger/cash-legs", Fixture, NULL, setup, test_cash_legs, teardown);
	g_test_add("/arbitrage-ledger/close-reopen", Fixture, NULL, setup, test_close_reopen, teardown);
	g_test_add("/arbitrage-ledger/currencies", Fixture, NULL, setup, test_currencies, teardown);
	g_test_add("/arbitrage-ledger/holding-floor", Fixture, NULL, setup, test_holding_floor, teardown);
	g_test_add("/arbitrage-ledger/stock-flip", Fixture, NULL, setup, test_stock_flip, teardown);
	g_test_add("/arbitrage-ledger/abandon", Fixture, NULL, setup, test_abandon, teardown);
	g_test_add("/arbitrage-ledger/delete-keeps-journals", Fixture, NULL, setup,
	           test_delete_keeps_journals, teardown);
	g_test_add("/arbitrage-ledger/leg-rules", Fixture, NULL, setup, test_leg_rules, teardown);
	g_test_add("/arbitrage-ledger/abandon-deleted-leg", Fixture, NULL, setup, test_abandon_deleted_leg,
	           teardown);
	g_test_add("/arbitrage-ledger/closed-at-kept", Fixture, NULL, setup, test_closed_at_kept, teardown);
	g_test_add("/arbitrage-ledger/expected-profit", Fixture, NULL, setup, test_expected_profit, teardown);
	g_test_add("/arbitrage-ledger/cost-in-two-currencies", Fixture, NULL, setup,
	           test_cost_in_two_currencies, teardown);
	g_test_add("/arbitrage-ledger/record", Fixture, NULL, setup, test_record, teardown);
	g_test_add("/arbitrage-ledger/record-roles", Fixture, NULL, setup, test_record_roles, teardown);
	g_test_add("/arbitrage-ledger/second-actor", Fixture, NULL, setup, test_second_actor, teardown);
	g_test_add("/arbitrage-ledger/performance", Fixture, NULL, setup, test_performance, teardown);
	g_test_add("/arbitrage-ledger/pnl-as-of", Fixture, NULL, setup, test_pnl_as_of, teardown);
	g_test_add("/arbitrage-ledger/operational-reports", Fixture, NULL, setup, test_operational_reports,
	           teardown);
	g_test_add("/arbitrage-ledger/trade-page", Fixture, NULL, setup, test_trade_page, teardown);
	g_test_add("/arbitrage-ledger/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/arbitrage-ledger/reads-make-no-accounts", Fixture, NULL, setup,
	           test_reads_make_no_accounts, teardown);

	return g_test_run();
}
