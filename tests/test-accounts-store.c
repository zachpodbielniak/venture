/*
 * test-accounts-store.c - The operator's accounts in a series store
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The second half of the series store: accounts, their balances,
 * holdings, positions and inbound, and the external ledger. Like the
 * market half it is hand-written SQL that nothing generic checks, and its
 * mistakes are the quiet kind -- a snapshot that erases another account's
 * bags, a ledger row counted twice because the source merged it, a purse
 * whose history grows a row every five minutes -- so these tests drive
 * the real store on a real file and say, each, what goes wrong without
 * them.
 */

#include <venture.h>

#ifdef VENTURE_HAVE_SQLITE

#include <math.h>
#include <string.h>

#include "venture-test-util.h"

/* Monday 2026-09-28 00:00:00 UTC. */
#define T0 (G_GINT64_CONSTANT(1790553600))
#define HOUR (G_GINT64_CONSTANT(3600))
#define DAY (G_GINT64_CONSTANT(86400))

typedef struct
{
	gchar			*root;
	gchar			*dir;
	VentureSeriesStore	*store;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;

	(void)data;

	fixture->root = g_dir_make_tmp("venture-accounts-XXXXXX", &error);
	g_assert_no_error(error);
	fixture->dir = g_build_filename(fixture->root, "series", "source-1", NULL);
	fixture->store = venture_series_store_open(fixture->dir, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->store);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 data
){
	(void)data;

	g_clear_object(&fixture->store);
	venture_test_remove_tree(fixture->root);
	g_free(fixture->root);
	g_free(fixture->dir);
}

/* --- Building batches ------------------------------------------------------------ */

static VentureSeriesAccount
account(
	const gchar	*key,
	const gchar	*name,
	const gchar	*kind
){
	VentureSeriesAccount row;

	memset(&row, 0, sizeof(row));
	row.key = key;
	row.name = name;
	row.kind = kind;
	row.last_seen = VENTURE_SERIES_NONE;

	return row;
}

static VentureSeriesHolding
holding(
	const gchar	*account_key,
	const gchar	*place,
	const gchar	*instrument,
	gint64		 quantity
){
	VentureSeriesHolding row;

	row.account_key = account_key;
	row.place = place;
	row.instrument_key = instrument;
	row.quantity = quantity;
	row.at = VENTURE_SERIES_NONE;

	return row;
}

static VentureSeriesPosition
position(
	const gchar	*key,
	const gchar	*account_key,
	const gchar	*instrument,
	gint64		 price,
	gint64		 expires_at
){
	VentureSeriesPosition row;

	row.key = key;
	row.account_key = account_key;
	row.venue_key = "realm-a";
	row.instrument_key = instrument;
	row.quantity = 1;
	row.unit_price = price;
	row.bid = VENTURE_SERIES_NONE;
	row.expires_at = expires_at;
	row.posted_at = VENTURE_SERIES_NONE;
	row.at = VENTURE_SERIES_NONE;

	return row;
}

static VentureSeriesInbound
inbound(
	const gchar	*key,
	const gchar	*account_key,
	gint64		 money,
	gint64		 expires_at
){
	VentureSeriesInbound row;

	memset(&row, 0, sizeof(row));
	row.key = key;
	row.account_key = account_key;
	row.sender = "Auction House";
	row.subject = "Auction successful";
	row.money = money;
	row.cod = VENTURE_SERIES_NONE;
	row.quantity = VENTURE_SERIES_NONE;
	row.expires_at = expires_at;
	row.at = VENTURE_SERIES_NONE;

	return row;
}

static VentureSeriesTxn
txn(
	const gchar	*key,
	const gchar	*account_key,
	const gchar	*kind,
	const gchar	*instrument,
	gint64		 quantity,
	gint64		 amount,
	gint64		 at
){
	VentureSeriesTxn row;

	memset(&row, 0, sizeof(row));
	row.key = key;
	row.account_key = account_key;
	row.venue_key = "realm-a";
	row.kind = kind;
	row.instrument_key = instrument;
	row.quantity = quantity;
	row.unit_price = VENTURE_SERIES_NONE;
	row.amount = amount;
	row.source = "Auction";
	row.at = at;

	return row;
}

static VentureSeriesBalance
balance(
	const gchar	*account_key,
	gint64		 amount,
	gint64		 at
){
	VentureSeriesBalance row;

	row.account_key = account_key;
	row.currency = "GOLD";
	row.amount = amount;
	row.at = at;

	return row;
}

static VentureSeriesAccountBatch
batch_new(void)
{
	VentureSeriesAccountBatch batch;

	memset(&batch, 0, sizeof(batch));
	batch.currency = "GOLD";

	return batch;
}

static VentureSeriesAccountResult
apply(
	Fixture				*fixture,
	const VentureSeriesAccountBatch	*batch,
	gint64				 fetched_at
){
	VentureSeriesAccountResult result;
	g_autoptr(GError) error = NULL;

	memset(&result, 0, sizeof(result));
	g_assert_true(venture_series_store_apply_accounts(fixture->store, batch, fetched_at,
	                                                  &result, &error));
	g_assert_no_error(error);

	return result;
}

/* The quantity an account holds of an instrument at a place, or -1. */
static gint64
held(
	Fixture		*fixture,
	const gchar	*account_key,
	const gchar	*place,
	const gchar	*instrument
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesHoldingFilter filter;

	venture_series_holding_filter_init(&filter);
	filter.account_key = account_key;
	filter.place = place;
	filter.instrument_key = instrument;
	rows = venture_series_store_list_holdings(fixture->store, &filter, &error);
	g_assert_no_error(error);

	if (0 == rows->len)
		return -1;

	g_assert_cmpuint(rows->len, ==, 1);

	return ((VentureSeriesHoldingRow *)g_ptr_array_index(rows, 0))->quantity;
}

static guint
count_positions(
	Fixture		*fixture,
	const gchar	*account_key
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesPositionFilter filter;

	venture_series_position_filter_init(&filter);
	filter.account_key = account_key;
	rows = venture_series_store_list_positions(fixture->store, &filter, &error);
	g_assert_no_error(error);

	return rows->len;
}

static GArray *
history(
	Fixture		*fixture,
	const gchar	*account_key
){
	g_autoptr(GError) error = NULL;
	GArray *points;

	points = venture_series_store_balance_history(fixture->store, account_key, NULL,
	                                              VENTURE_SERIES_NONE, VENTURE_SERIES_NONE,
	                                              &error);
	g_assert_no_error(error);
	g_assert_nonnull(points);

	return points;
}

/* --- Accounts and their rows -------------------------------------------------------- */

/*
 * One batch of everything, read back through every reader.
 *
 * What breaks if this regresses: the accounts page has nothing to show,
 * or shows it under the wrong character.
 */
static void
test_apply_and_read(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesAccount accounts[2];
	VentureSeriesHolding holdings[3];
	VentureSeriesPosition positions[2];
	VentureSeriesInbound mail[2];
	VentureSeriesBalance balances[1];
	VentureSeriesTxn txns[2];
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;
	g_autoptr(VentureSeriesAccountRow) row = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;

	accounts[0] = account("Drgold-Thorium", "Drgold", "character");
	accounts[0].group_key = "us";
	accounts[0].venue_key = "realm-a";
	accounts[0].attrs_json = "{\"class\":\"WARRIOR\",\"level\":80}";
	accounts[0].last_seen = T0;
	accounts[1] = account("warbank:ZAK", "Warband bank", "shared");

	holdings[0] = holding("Drgold-Thorium", "bag", "2770", 12);
	holdings[1] = holding("Drgold-Thorium", "bank", "2770", 3);
	holdings[2] = holding("warbank:ZAK", "warbank", "2447", 40);
	positions[0] = position("1637378752", "Drgold-Thorium", "2770", 8179800, T0 + 12 * HOUR);
	positions[1] = position("1637378753", "Drgold-Thorium", "2447", 100, T0 + 2 * HOUR);
	mail[0] = inbound("m1", "Drgold-Thorium", 1234500, T0 + 30 * DAY);
	mail[1] = inbound("m2", "Drgold-Thorium", VENTURE_SERIES_NONE, T0 + 3 * DAY);
	mail[1].instrument_key = "2770";
	mail[1].quantity = 5;
	mail[1].cod = 5000;
	balances[0] = balance("Drgold-Thorium", 27249501812, T0);
	txns[0] = txn("t1", "Drgold-Thorium", "sale", "2770", 3, 7977, T0 - DAY);
	txns[1] = txn("t2", "Drgold-Thorium", "expired", "2447", 1, VENTURE_SERIES_NONE, T0 - HOUR);

	batch = batch_new();
	batch.accounts = accounts;
	batch.n_accounts = 2;
	batch.holdings = holdings;
	batch.n_holdings = 3;
	batch.positions = positions;
	batch.n_positions = 2;
	batch.inbound = mail;
	batch.n_inbound = 2;
	batch.balances = balances;
	batch.n_balances = 1;
	batch.txns = txns;
	batch.n_txns = 2;

	result = apply(fixture, &batch, T0 + 60);
	g_assert_cmpint(result.accounts, ==, 2);
	g_assert_cmpint(result.accounts_new, ==, 2);
	g_assert_cmpint(result.holdings, ==, 3);
	g_assert_cmpint(result.positions, ==, 2);
	g_assert_cmpint(result.inbound, ==, 2);
	g_assert_cmpint(result.balances, ==, 1);
	g_assert_cmpint(result.txns_new, ==, 2);
	/* The instruments the rows named exist now, bare, for the market
	 * pages and promotion to find. */
	g_assert_cmpint(result.instruments_new, ==, 2);

	g_assert_true(venture_series_store_get_account(fixture->store, "Drgold-Thorium", T0 + 3 * HOUR,
	                                               &row, &error));
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_assert_cmpstr(row->name, ==, "Drgold");
	g_assert_cmpstr(row->kind, ==, "character");
	g_assert_cmpstr(row->group_key, ==, "us");
	g_assert_cmpstr(row->venue_key, ==, "realm-a");
	g_assert_nonnull(strstr(row->attrs_json, "WARRIOR"));
	g_assert_cmpint(row->last_seen, ==, T0);
	g_assert_cmpint(row->synced_at, ==, T0 + 60);
	g_assert_cmpint(row->holdings, ==, 2);
	g_assert_cmpint(row->positions, ==, 2);
	/* At three hours in, the two-hour auction has run out. */
	g_assert_cmpint(row->positions_expired, ==, 1);
	g_assert_cmpint(row->soonest_position_expiry, ==, T0 + 2 * HOUR);
	g_assert_cmpint(row->inbound, ==, 2);
	g_assert_cmpint(row->inbound_expired, ==, 0);
	g_assert_cmpint(row->soonest_inbound_expiry, ==, T0 + 3 * DAY);
	g_assert_cmpuint(row->balances->len, ==, 1);
	g_assert_cmpstr(g_array_index(row->balances, VentureSeriesAmount, 0).currency, ==, "GOLD");
	g_assert_cmpint(g_array_index(row->balances, VentureSeriesAmount, 0).amount, ==, 27249501812);
	g_assert_cmpuint(row->inbound_money->len, ==, 1);
	g_assert_cmpint(g_array_index(row->inbound_money, VentureSeriesAmount, 0).amount, ==, 1234500);
	g_assert_cmpuint(row->inbound_cod->len, ==, 1);
	g_assert_cmpint(g_array_index(row->inbound_cod, VentureSeriesAmount, 0).amount, ==, 5000);

	/* An unknown key is no account, and no error. */
	g_clear_pointer(&row, venture_series_account_row_free);
	g_assert_true(venture_series_store_get_account(fixture->store, "nobody", T0, &row, &error));
	g_assert_null(row);

	all = venture_series_store_list_accounts(fixture->store, NULL, NULL, T0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(all->len, ==, 2);
	g_clear_pointer(&all, g_ptr_array_unref);

	all = venture_series_store_list_accounts(fixture->store, "shared", NULL, T0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(all->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesAccountRow *)g_ptr_array_index(all, 0))->key, ==, "warbank:ZAK");
	g_assert_cmpint(((VentureSeriesAccountRow *)g_ptr_array_index(all, 0))->soonest_position_expiry,
	                ==, VENTURE_SERIES_NONE);
}

/*
 * A snapshot replaces only what it covers, only for its own account.
 *
 * What breaks if this regresses: syncing one character's bags erases the
 * bank of another, or a push of positions alone empties the bags.
 */
static void
test_snapshot_replacement(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesHolding holdings[4];
	VentureSeriesPosition positions[2];
	VentureSeriesAccountSnapshot snapshot;
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;

	(void)data;

	holdings[0] = holding("a", "bag", "ore", 10);
	holdings[1] = holding("a", "bank", "herb", 5);
	holdings[2] = holding("b", "bag", "ore", 7);
	positions[0] = position("p1", "a", "ore", 100, T0 + DAY);
	positions[1] = position("p2", "a", "herb", 50, T0 + DAY);

	batch = batch_new();
	batch.holdings = holdings;
	batch.n_holdings = 3;
	batch.positions = positions;
	batch.n_positions = 2;
	apply(fixture, &batch, T0);

	/* Account a's holdings are restated: only the bag's ore remains,
	 * now 12. Its positions are not covered; account b is not in it. */
	snapshot.account_key = "a";
	snapshot.at = T0 + HOUR;
	snapshot.covers = VENTURE_SERIES_COVERS_HOLDINGS;
	holdings[0] = holding("a", "bag", "ore", 12);
	batch = batch_new();
	batch.snapshots = &snapshot;
	batch.n_snapshots = 1;
	batch.holdings = holdings;
	batch.n_holdings = 1;
	result = apply(fixture, &batch, T0 + HOUR);

	g_assert_cmpint(result.removed, ==, 1);
	g_assert_cmpint(held(fixture, "a", "bag", "ore"), ==, 12);
	g_assert_cmpint(held(fixture, "a", "bank", "herb"), ==, -1);
	g_assert_cmpint(held(fixture, "b", "bag", "ore"), ==, 7);
	g_assert_cmpuint(count_positions(fixture, "a"), ==, 2);

	/* Covering positions with none restated: they are all gone. */
	snapshot.at = T0 + 2 * HOUR;
	snapshot.covers = VENTURE_SERIES_COVERS_POSITIONS;
	batch = batch_new();
	batch.snapshots = &snapshot;
	batch.n_snapshots = 1;
	result = apply(fixture, &batch, T0 + 2 * HOUR);
	g_assert_cmpint(result.removed, ==, 2);
	g_assert_cmpuint(count_positions(fixture, "a"), ==, 0);
	g_assert_cmpint(held(fixture, "a", "bag", "ore"), ==, 12);

	/* Outside a snapshot, zero is "no longer held". */
	holdings[0] = holding("b", "bag", "ore", 0);
	batch = batch_new();
	batch.holdings = holdings;
	batch.n_holdings = 1;
	apply(fixture, &batch, T0 + 3 * HOUR);
	g_assert_cmpint(held(fixture, "b", "bag", "ore"), ==, -1);
}

/*
 * A position restated keeps when it was first seen and when it was
 * posted, however often it is restated.
 *
 * What breaks if this regresses: every listing is "posted just now" after
 * each sync, and an undercut alert or an "up for days" warning never fires.
 */
static void
test_position_age(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesPosition positions[1];
	VentureSeriesAccountSnapshot snapshot;
	VentureSeriesAccountBatch batch;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesPositionRow *row;
	guint i;

	(void)data;

	for (i = 0; i < 3; i++)
	{
		positions[0] = position("p1", "a", "ore", 100 + i, T0 + DAY);

		/* The source says when it was posted only the first time. */
		if (0 == i)
			positions[0].posted_at = T0 - HOUR;

		snapshot.account_key = "a";
		snapshot.at = T0 + (gint64)i * HOUR;
		snapshot.covers = VENTURE_SERIES_COVERS_POSITIONS;
		batch = batch_new();
		batch.snapshots = &snapshot;
		batch.n_snapshots = 1;
		batch.positions = positions;
		batch.n_positions = 1;
		apply(fixture, &batch, T0 + (gint64)i * HOUR);
	}

	rows = venture_series_store_list_positions(fixture->store, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	row = g_ptr_array_index(rows, 0);
	g_assert_cmpstr(row->key, ==, "p1");
	g_assert_cmpstr(row->account_key, ==, "a");
	g_assert_cmpint(row->unit_price, ==, 102);
	g_assert_cmpstr(row->currency, ==, "GOLD");
	g_assert_cmpint(row->first_seen, ==, T0);
	g_assert_cmpint(row->last_seen, ==, T0 + 2 * HOUR);
	g_assert_cmpint(row->posted_at, ==, T0 - HOUR);
}

/*
 * A snapshot older than the one that last replaced a kind changes nothing
 * of that kind, and one at the same time applies again idempotently.
 *
 * What breaks if this regresses: an export read late -- the laptop that
 * synced after the desktop -- rolls a character's bags back an hour.
 */
static void
test_stale_snapshot(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesHolding holdings[1];
	VentureSeriesAccountSnapshot snapshot;
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;

	(void)data;

	snapshot.account_key = "a";
	snapshot.at = T0 + 2 * HOUR;
	snapshot.covers = VENTURE_SERIES_COVERS_HOLDINGS;
	holdings[0] = holding("a", "bag", "ore", 20);
	batch = batch_new();
	batch.snapshots = &snapshot;
	batch.n_snapshots = 1;
	batch.holdings = holdings;
	batch.n_holdings = 1;
	apply(fixture, &batch, T0 + 2 * HOUR);

	snapshot.at = T0 + HOUR;
	holdings[0] = holding("a", "bag", "herb", 9);
	result = apply(fixture, &batch, T0 + 3 * HOUR);
	g_assert_cmpint(result.stale, ==, 1);
	g_assert_cmpint(result.holdings, ==, 0);
	g_assert_cmpint(held(fixture, "a", "bag", "ore"), ==, 20);
	g_assert_cmpint(held(fixture, "a", "bag", "herb"), ==, -1);

	/* The same snapshot again: the same state, nothing stale. */
	snapshot.at = T0 + 2 * HOUR;
	holdings[0] = holding("a", "bag", "ore", 20);
	result = apply(fixture, &batch, T0 + 4 * HOUR);
	g_assert_cmpint(result.stale, ==, 0);
	g_assert_cmpint(result.removed, ==, 0);
	g_assert_cmpint(held(fixture, "a", "bag", "ore"), ==, 20);
}

/*
 * Balance history keeps changes only: a repeat of the value before is not
 * stored, a late point between two others leaves the history as if it
 * had come in order, and a covered currency left out drops to zero.
 *
 * What breaks if this regresses: a purse read every five minutes is a
 * row every five minutes forever, and the chart is a flat line made of
 * a hundred thousand points; or a character who spent all their gold
 * keeps showing it.
 */
static void
test_balance_history(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesBalance balances[2];
	VentureSeriesAccountSnapshot snapshot;
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;
	g_autoptr(GArray) points = NULL;
	guint i;

	(void)data;

	/* 100, 100, 100, 150, 150 at hourly steps: two points. */
	for (i = 0; i < 5; i++)
	{
		balances[0] = balance("a", (i < 3) ? 100 : 150, T0 + (gint64)i * HOUR);
		batch = batch_new();
		batch.balances = balances;
		batch.n_balances = 1;
		apply(fixture, &batch, T0 + (gint64)i * HOUR);
	}

	points = history(fixture, "a");
	g_assert_cmpuint(points->len, ==, 2);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 0).at, ==, T0);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 0).amount, ==, 100);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 1).at, ==, T0 + 3 * HOUR);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 1).amount, ==, 150);
	g_clear_pointer(&points, g_array_unref);

	/* A late point saying 150 at hour 2 moves the change earlier: the
	 * point at hour 3 now repeats it and goes. */
	balances[0] = balance("a", 150, T0 + 2 * HOUR);
	batch = batch_new();
	batch.balances = balances;
	batch.n_balances = 1;
	result = apply(fixture, &batch, T0 + 5 * HOUR);
	g_assert_cmpint(result.balances, ==, 1);
	points = history(fixture, "a");
	g_assert_cmpuint(points->len, ==, 2);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 1).at, ==, T0 + 2 * HOUR);
	g_clear_pointer(&points, g_array_unref);

	/* The same point again is unchanged. */
	result = apply(fixture, &batch, T0 + 6 * HOUR);
	g_assert_cmpint(result.balances, ==, 0);
	g_assert_cmpint(result.balances_unchanged, ==, 1);

	/* A snapshot covering balances that restates none: the purse is
	 * empty now, appended, not erased. */
	snapshot.account_key = "a";
	snapshot.at = T0 + 7 * HOUR;
	snapshot.covers = VENTURE_SERIES_COVERS_BALANCES;
	batch = batch_new();
	batch.snapshots = &snapshot;
	batch.n_snapshots = 1;
	apply(fixture, &batch, T0 + 7 * HOUR);
	points = history(fixture, "a");
	g_assert_cmpuint(points->len, ==, 3);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 2).at, ==, T0 + 7 * HOUR);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 2).amount, ==, 0);
	g_clear_pointer(&points, g_array_unref);

	/* Restating it with money keeps it: no zero is appended. */
	snapshot.at = T0 + 8 * HOUR;
	balances[0] = balance("a", 40, T0 + 8 * HOUR);
	batch.balances = balances;
	batch.n_balances = 1;
	apply(fixture, &batch, T0 + 8 * HOUR);
	points = history(fixture, "a");
	g_assert_cmpuint(points->len, ==, 4);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 3).amount, ==, 40);
}

/*
 * The ledger is upserted on the source's id: a row sent again unchanged
 * is unchanged, one whose merged quantity grew is updated in place, never
 * a second row.
 *
 * What breaks if this regresses: TSM merges a stack into an earlier sale
 * row, tsmctl re-sends it, and the profit and loss counts the sale twice.
 */
static void
test_txn_upsert(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesTxn txns[2];
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesTxnRow *row;
	gint64 count;

	(void)data;

	txns[0] = txn("sale-1", "a", "sale", "ore", 3, 7977, T0);
	txns[1] = txn("sale-2", "a", "sale", "herb", 1, 100, T0 + HOUR);
	batch = batch_new();
	batch.txns = txns;
	batch.n_txns = 2;
	result = apply(fixture, &batch, T0 + 2 * HOUR);
	g_assert_cmpint(result.txns_new, ==, 2);

	result = apply(fixture, &batch, T0 + 3 * HOUR);
	g_assert_cmpint(result.txns_new, ==, 0);
	g_assert_cmpint(result.txns_updated, ==, 0);
	g_assert_cmpint(result.txns_unchanged, ==, 2);

	txns[0].quantity = 5;
	txns[0].amount = 13295;
	result = apply(fixture, &batch, T0 + 4 * HOUR);
	g_assert_cmpint(result.txns_updated, ==, 1);
	g_assert_cmpint(result.txns_unchanged, ==, 1);

	g_assert_true(venture_series_store_count_txns(fixture->store, NULL, &count, &error));
	g_assert_cmpint(count, ==, 2);

	rows = venture_series_store_list_txns(fixture->store, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	/* Newest first by default. */
	row = g_ptr_array_index(rows, 1);
	g_assert_cmpstr(row->key, ==, "sale-1");
	g_assert_cmpint(row->quantity, ==, 5);
	g_assert_cmpint(row->amount, ==, 13295);
	g_assert_cmpstr(row->currency, ==, "GOLD");
	g_assert_cmpint(row->first_seen, ==, T0 + 2 * HOUR);
	g_assert_cmpint(row->updated_at, ==, T0 + 4 * HOUR);
}

/*
 * Malformed rows fail the whole batch, and nothing of it is kept.
 *
 * What breaks if this regresses: half a snapshot is applied -- the
 * replacement ran, the rows did not -- and the bags read empty.
 */
static void
test_refusals(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesHolding holdings[2];
	VentureSeriesTxn txns[1];
	VentureSeriesPosition positions[1];
	VentureSeriesAccountSnapshot snapshot;
	VentureSeriesAccountBatch batch;
	g_autoptr(GError) error = NULL;

	(void)data;

	holdings[0] = holding("a", "bag", "ore", 10);
	batch = batch_new();
	batch.holdings = holdings;
	batch.n_holdings = 1;
	apply(fixture, &batch, T0);

	/* A replacement followed by a row with a place nobody knows. */
	snapshot.account_key = "a";
	snapshot.at = T0 + HOUR;
	snapshot.covers = VENTURE_SERIES_COVERS_HOLDINGS;
	holdings[0] = holding("a", "bag", "herb", 1);
	holdings[1] = holding("a", "pocket", "ore", 1);
	batch.snapshots = &snapshot;
	batch.n_snapshots = 1;
	batch.n_holdings = 2;
	g_assert_false(venture_series_store_apply_accounts(fixture->store, &batch, T0 + HOUR, NULL,
	                                                   &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_cmpint(held(fixture, "a", "bag", "ore"), ==, 10);
	g_assert_cmpint(held(fixture, "a", "bag", "herb"), ==, -1);

	/* A ledger kind outside the list. */
	txns[0] = txn("x", "a", "gift", NULL, VENTURE_SERIES_NONE, 1, T0);
	batch = batch_new();
	batch.txns = txns;
	batch.n_txns = 1;
	g_assert_false(venture_series_store_apply_accounts(fixture->store, &batch, T0, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Money with no currency to be in. */
	positions[0] = position("p", "a", "ore", 1, T0);
	batch = batch_new();
	batch.currency = NULL;
	batch.positions = positions;
	batch.n_positions = 1;
	g_assert_false(venture_series_store_apply_accounts(fixture->store, &batch, T0, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Two snapshots of one account in one batch. */
	{
		VentureSeriesAccountSnapshot twice[2];

		twice[0] = snapshot;
		twice[1] = snapshot;
		batch = batch_new();
		batch.snapshots = twice;
		batch.n_snapshots = 2;
		g_assert_false(venture_series_store_apply_accounts(fixture->store, &batch, T0, NULL,
		                                                   &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}
}

/* --- Readers --------------------------------------------------------------------------- */

/*
 * Holdings summed across accounts and places, each with its breakdown;
 * positions and inbound expiring before a moment.
 *
 * What breaks if this regresses: the inventory page counts an item twice
 * or misses the guild bank, and "log in before it expires" lists the
 * wrong auctions.
 */
static void
test_readers(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesHolding holdings[4];
	VentureSeriesPosition positions[3];
	VentureSeriesInbound mail[2];
	VentureSeriesAccountBatch batch;
	g_autoptr(GPtrArray) totals = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesHoldingFilter holding_filter;
	VentureSeriesPositionFilter position_filter;
	VentureSeriesInboundFilter inbound_filter;
	VentureSeriesHoldingTotal *total;

	(void)data;

	holdings[0] = holding("a", "bag", "ore", 10);
	holdings[1] = holding("a", "bank", "ore", 5);
	holdings[2] = holding("b", "guild", "ore", 1);
	holdings[3] = holding("b", "currency", "currency:2032", 900);
	positions[0] = position("p1", "a", "ore", 100, T0 + 2 * HOUR);
	positions[1] = position("p2", "b", "ore", 100, T0 + 30 * HOUR);
	positions[2] = position("p3", "b", "ore", 100, VENTURE_SERIES_NONE);
	mail[0] = inbound("m1", "a", 10, T0 + HOUR);
	mail[1] = inbound("m2", "b", 10, T0 + 10 * DAY);

	batch = batch_new();
	batch.holdings = holdings;
	batch.n_holdings = 4;
	batch.positions = positions;
	batch.n_positions = 3;
	batch.inbound = mail;
	batch.n_inbound = 2;
	apply(fixture, &batch, T0);

	venture_series_holding_filter_init(&holding_filter);
	holding_filter.exclude_place = "currency";
	totals = venture_series_store_holdings_by_instrument(fixture->store, &holding_filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(totals->len, ==, 1);
	total = g_ptr_array_index(totals, 0);
	g_assert_cmpstr(total->instrument_key, ==, "ore");
	g_assert_cmpint(total->quantity, ==, 16);
	g_assert_cmpint(total->accounts, ==, 2);
	g_assert_cmpuint(total->places->len, ==, 3);
	g_clear_pointer(&totals, g_ptr_array_unref);

	/* With the currencies, two instruments; a search finds one. */
	venture_series_holding_filter_init(&holding_filter);
	totals = venture_series_store_holdings_by_instrument(fixture->store, &holding_filter, &error);
	g_assert_cmpuint(totals->len, ==, 2);
	g_clear_pointer(&totals, g_ptr_array_unref);
	holding_filter.search = "CURRENCY";
	totals = venture_series_store_holdings_by_instrument(fixture->store, &holding_filter, &error);
	g_assert_cmpuint(totals->len, ==, 1);
	g_assert_cmpint(((VentureSeriesHoldingTotal *)g_ptr_array_index(totals, 0))->quantity, ==, 900);

	/* Expiring within the next day: p1, not p2, never the one with no
	 * expiry. Soonest first. */
	venture_series_position_filter_init(&position_filter);
	position_filter.expires_before = T0 + DAY;
	rows = venture_series_store_list_positions(fixture->store, &position_filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesPositionRow *)g_ptr_array_index(rows, 0))->key, ==, "p1");
	g_clear_pointer(&rows, g_ptr_array_unref);

	venture_series_position_filter_init(&position_filter);
	rows = venture_series_store_list_positions(fixture->store, &position_filter, &error);
	g_assert_cmpuint(rows->len, ==, 3);
	g_assert_cmpstr(((VentureSeriesPositionRow *)g_ptr_array_index(rows, 2))->key, ==, "p3");
	g_clear_pointer(&rows, g_ptr_array_unref);

	venture_series_inbound_filter_init(&inbound_filter);
	inbound_filter.expires_before = T0 + DAY;
	rows = venture_series_store_list_inbound(fixture->store, &inbound_filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesInboundRow *)g_ptr_array_index(rows, 0))->key, ==, "m1");
	g_assert_cmpstr(((VentureSeriesInboundRow *)g_ptr_array_index(rows, 0))->sender, ==,
	                "Auction House");
}

/*
 * The ledger summed for a profit and loss: integer money, one bucket per
 * group and currency, net = sales + income - buys - expenses; periods are
 * UTC days, Monday weeks and calendar months.
 *
 * What breaks if this regresses: the P&L page and tsmctl pnl disagree, or
 * a sale on Sunday night lands in next week.
 */
static void
test_txn_totals(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesTxn txns[6];
	VentureSeriesAccountBatch batch;
	VentureSeriesTxnFilter filter;
	g_autoptr(GPtrArray) totals = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesTxnTotal *bucket;
	VentureSeriesTxnGroup group;
	gint64 count;

	(void)data;

	/* T0 is Monday 28 September. Sunday before it, then Monday,
	 * Tuesday, and Thursday 1 October: two weeks, two months. */
	txns[0] = txn("s1", "a", "sale", "ore", 3, 300, T0 - HOUR);
	txns[1] = txn("s2", "a", "sale", "ore", 2, 200, T0 + HOUR);
	txns[2] = txn("b1", "b", "buy", "ore", 4, 160, T0 + DAY);
	txns[3] = txn("i1", "a", "income", NULL, VENTURE_SERIES_NONE, 50, T0 + DAY);
	txns[4] = txn("e1", "b", "expense", NULL, VENTURE_SERIES_NONE, 30, T0 + 3 * DAY);
	txns[5] = txn("x1", "a", "expired", "herb", 7, VENTURE_SERIES_NONE, T0 + DAY);
	txns[1].source = "Trade";

	batch = batch_new();
	batch.txns = txns;
	batch.n_txns = 6;
	apply(fixture, &batch, T0 + 2 * DAY);

	venture_series_txn_filter_init(&filter);
	totals = venture_series_store_txn_totals(fixture->store, &filter, VENTURE_SERIES_TXN_GROUP_WEEK,
	                                         &error);
	g_assert_no_error(error);
	g_assert_cmpuint(totals->len, ==, 2);
	bucket = g_ptr_array_index(totals, 0);
	g_assert_null(bucket->key);
	g_assert_cmpint(bucket->period_start, ==, T0 - 7 * DAY);
	g_assert_cmpint(bucket->sales_amount, ==, 300);
	bucket = g_ptr_array_index(totals, 1);
	g_assert_cmpint(bucket->period_start, ==, T0);
	g_assert_cmpint(bucket->sales, ==, 1);
	g_assert_cmpint(bucket->sold_units, ==, 2);
	g_assert_cmpint(bucket->sales_amount, ==, 200);
	g_assert_cmpint(bucket->buys_amount, ==, 160);
	g_assert_cmpint(bucket->bought_units, ==, 4);
	g_assert_cmpint(bucket->income, ==, 50);
	g_assert_cmpint(bucket->expense, ==, 30);
	g_assert_cmpint(bucket->expired_units, ==, 7);
	g_assert_cmpint(bucket->net, ==, 200 + 50 - 160 - 30);
	g_clear_pointer(&totals, g_ptr_array_unref);

	totals = venture_series_store_txn_totals(fixture->store, &filter, VENTURE_SERIES_TXN_GROUP_DAY,
	                                         &error);
	g_assert_cmpuint(totals->len, ==, 4);
	g_clear_pointer(&totals, g_ptr_array_unref);

	totals = venture_series_store_txn_totals(fixture->store, &filter,
	                                         VENTURE_SERIES_TXN_GROUP_MONTH, &error);
	g_assert_cmpuint(totals->len, ==, 2);
	g_assert_cmpint(((VentureSeriesTxnTotal *)g_ptr_array_index(totals, 1))->period_start, ==,
	                G_GINT64_CONSTANT(1790812800));
	g_clear_pointer(&totals, g_ptr_array_unref);

	totals = venture_series_store_txn_totals(fixture->store, &filter,
	                                         VENTURE_SERIES_TXN_GROUP_ACCOUNT, &error);
	g_assert_cmpuint(totals->len, ==, 2);
	bucket = g_ptr_array_index(totals, 0);
	g_assert_cmpstr(bucket->key, ==, "a");
	g_assert_cmpint(bucket->net, ==, 300 + 200 + 50);
	bucket = g_ptr_array_index(totals, 1);
	g_assert_cmpstr(bucket->key, ==, "b");
	g_assert_cmpint(bucket->net, ==, -190);
	g_clear_pointer(&totals, g_ptr_array_unref);

	g_assert_true(venture_series_txn_group_from_string("source", &group));
	totals = venture_series_store_txn_totals(fixture->store, &filter, group, &error);
	g_assert_cmpuint(totals->len, ==, 2);
	g_clear_pointer(&totals, g_ptr_array_unref);

	totals = venture_series_store_txn_totals(fixture->store, &filter,
	                                         VENTURE_SERIES_TXN_GROUP_INSTRUMENT, &error);
	/* "", herb, ore */
	g_assert_cmpuint(totals->len, ==, 3);
	g_clear_pointer(&totals, g_ptr_array_unref);

	/* Filters: one account's sales in a window, [since, until). */
	filter.account_key = "a";
	filter.kind = "sale";
	filter.since = T0;
	filter.until = T0 + DAY;
	g_assert_true(venture_series_store_count_txns(fixture->store, &filter, &count, &error));
	g_assert_cmpint(count, ==, 1);
	rows = venture_series_store_list_txns(fixture->store, &filter, &error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesTxnRow *)g_ptr_array_index(rows, 0))->key, ==, "s2");
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* Paging, oldest first. */
	venture_series_txn_filter_init(&filter);
	filter.count = 2;
	filter.offset = 1;
	rows = venture_series_store_list_txns(fixture->store, &filter, &error);
	g_assert_cmpuint(rows->len, ==, 2);
	g_assert_cmpstr(((VentureSeriesTxnRow *)g_ptr_array_index(rows, 0))->key, ==, "s2");
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* A kind that is not one is refused, not matched against nothing. */
	filter.kind = "gift";
	g_assert_false(venture_series_store_count_txns(fixture->store, &filter, &count, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * Retention: the ledger and balance history follow series.daily_days, but
 * an account's newest balance survives however old it is.
 *
 * What breaks if this regresses: a character untouched for a year shows
 * no gold at all after the purge.
 */
static void
test_retention(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesBalance balances[3];
	VentureSeriesTxn txns[2];
	VentureSeriesAccountBatch batch;
	VentureSeriesPurgeResult purged;
	g_autoptr(GArray) points = NULL;
	g_autoptr(GError) error = NULL;
	gint64 count;

	(void)data;

	balances[0] = balance("a", 10, T0 - 100 * DAY);
	balances[1] = balance("a", 20, T0 - 90 * DAY);
	balances[2] = balance("b", 5, T0 - 100 * DAY);
	txns[0] = txn("old", "a", "sale", "ore", 1, 1, T0 - 100 * DAY);
	txns[1] = txn("new", "a", "sale", "ore", 1, 1, T0 - DAY);

	batch = batch_new();
	batch.balances = balances;
	batch.n_balances = 3;
	batch.txns = txns;
	batch.n_txns = 2;
	apply(fixture, &batch, T0);

	/* Zero days keeps everything. */
	g_assert_true(venture_series_store_purge(fixture->store, T0, 14, 0, &purged, &error));
	g_assert_no_error(error);
	g_assert_cmpint(purged.txns, ==, 0);
	g_assert_cmpint(purged.balances, ==, 0);

	g_assert_true(venture_series_store_purge(fixture->store, T0, 14, 30, &purged, &error));
	g_assert_no_error(error);
	g_assert_cmpint(purged.txns, ==, 1);
	g_assert_cmpint(purged.balances, ==, 1);

	g_assert_true(venture_series_store_count_txns(fixture->store, NULL, &count, &error));
	g_assert_cmpint(count, ==, 1);

	points = history(fixture, "a");
	g_assert_cmpuint(points->len, ==, 1);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 0).amount, ==, 20);
	g_clear_pointer(&points, g_array_unref);

	points = history(fixture, "b");
	g_assert_cmpuint(points->len, ==, 1);
	g_assert_cmpint(g_array_index(points, VentureSeriesAmount, 0).amount, ==, 5);
}

/*
 * A source's own historical price and sale rate fill the reference only
 * where the store has no figure of its own, and say so.
 *
 * What breaks if this regresses: a TSM-only source has no sale rate on any
 * page, or the source's figure overrides what this install observed.
 */
static void
test_source_figures(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesSnapshot *snapshot;
	VentureSeriesStats stats;
	VentureSeriesReference reference;
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;

	snapshot = venture_series_store_begin_snapshot(fixture->store, "region-us", "gold", T0,
	                                               T0 + 60, FALSE, &error);
	g_assert_no_error(error);
	venture_series_stats_init(&stats);
	stats.instrument_key = "ore";
	stats.historical = 1500;
	stats.sale_rate = 0.25;
	stats.sold_per_day = 120.5;
	g_assert_true(venture_series_snapshot_add_stats(snapshot, &stats, &error));
	g_assert_no_error(error);

	/* A rate above one is not a rate. */
	stats.instrument_key = "herb";
	stats.sale_rate = 1.5;
	g_assert_false(venture_series_snapshot_add_stats(snapshot, &stats, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_true(venture_series_store_commit_snapshot(fixture->store, snapshot, NULL, &error));
	g_assert_no_error(error);

	g_assert_true(venture_series_store_get_current(fixture->store, "region-us", "ore", &row,
	                                               &error));
	g_assert_nonnull(row);
	g_assert_cmpint(row->source_historical, ==, 1500);
	g_assert_cmpfloat(row->source_sale_rate, ==, 0.25);
	g_assert_cmpfloat(row->source_sold_per_day, ==, 120.5);
	/* The store's own estimate is a different member, and has nothing. */
	g_assert_true(isnan(row->sale_rate));

	/* Only a stat (no sold figure) the day before: no history of the
	 * store's own, so the source's figures answer, flagged. */
	memset(&reference, 0, sizeof(reference));
	g_assert_true(venture_series_store_reference(fixture->store, "region-us", NULL, "ore",
	                                             T0 + HOUR, &reference, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(reference.currency, ==, "GOLD");
	g_assert_cmpfloat(reference.sale_rate, ==, 0.25);
	g_assert_true(reference.sale_rate_from_source);
	g_assert_cmpfloat(reference.sold_per_day, ==, 120.5);
	g_assert_true(reference.sold_per_day_from_source);
	/* A market value per day exists (the stat had none), so the
	 * historical is the source's too. */
	g_assert_cmpint(reference.historical_60d, ==, 1500);
	g_assert_true(reference.historical_from_source);

	/* Asked about a moment before the stat was taken: not this
	 * morning's figure for last week's question. */
	memset(&reference, 0, sizeof(reference));
	g_assert_true(venture_series_store_reference(fixture->store, "region-us", NULL, "ore",
	                                             T0 - DAY, &reference, &error));
	g_assert_true(isnan(reference.sale_rate));
	g_assert_false(reference.sale_rate_from_source);
}

/*
 * Fifty thousand ledger rows in one batch -- a first sync -- in a bounded
 * time, and a second application of the same rows writes nothing.
 *
 * What breaks if this regresses: a first sync of a real TSM ledger holds
 * the feeds worker for minutes, or a re-export rewrites every row.
 */
static void
test_scale_smoke(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GArray) rows = NULL;
	g_autoptr(GPtrArray) keys = NULL;
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;
	gint64 started;
	guint i;

	(void)data;

	rows = g_array_sized_new(FALSE, FALSE, sizeof(VentureSeriesTxn), 50000);
	keys = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < 50000; i++)
	{
		gchar *key = g_strdup_printf("txn-%u", i);
		gchar *instrument = g_strdup_printf("%u", 1000 + (i % 2000));
		VentureSeriesTxn row;

		g_ptr_array_add(keys, key);
		g_ptr_array_add(keys, instrument);
		row = txn(key, (0 == (i % 2)) ? "a" : "b", (0 == (i % 3)) ? "buy" : "sale", instrument,
		          1 + (i % 20), 100 + i, T0 - (gint64)(i % 400) * HOUR);
		g_array_append_val(rows, row);
	}

	batch = batch_new();
	batch.txns = (const VentureSeriesTxn *)(gpointer)rows->data;
	batch.n_txns = rows->len;

	started = g_get_monotonic_time();
	result = apply(fixture, &batch, T0);
	g_test_message("50000 new ledger rows in %" G_GINT64_FORMAT " ms",
	               (g_get_monotonic_time() - started) / 1000);
	g_assert_cmpint(result.txns_new, ==, 50000);
	g_assert_cmpint(result.instruments_new, ==, 2000);
	g_assert_cmpint(g_get_monotonic_time() - started, <, G_GINT64_CONSTANT(30) * G_USEC_PER_SEC);

	/* Again: only the two accounts' sync times move. */
	started = g_get_monotonic_time();
	result = apply(fixture, &batch, T0 + HOUR);
	g_test_message("50000 unchanged ledger rows in %" G_GINT64_FORMAT " ms",
	               (g_get_monotonic_time() - started) / 1000);
	g_assert_cmpint(result.txns_unchanged, ==, 50000);
	g_assert_cmpint(result.rows_written, ==, 2);
	g_assert_cmpint(g_get_monotonic_time() - started, <, G_GINT64_CONSTANT(30) * G_USEC_PER_SEC);
}

#define ADD(path, func) \
	g_test_add("/accounts-store/" path, Fixture, NULL, fixture_set_up, func, \
	           fixture_tear_down)

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	ADD("apply-and-read", test_apply_and_read);
	ADD("snapshot-replacement", test_snapshot_replacement);
	ADD("position-age", test_position_age);
	ADD("stale-snapshot", test_stale_snapshot);
	ADD("balance-history", test_balance_history);
	ADD("txn-upsert", test_txn_upsert);
	ADD("refusals", test_refusals);
	ADD("readers", test_readers);
	ADD("txn-totals", test_txn_totals);
	ADD("retention", test_retention);
	ADD("source-figures", test_source_figures);
	ADD("scale-smoke", test_scale_smoke);

	return g_test_run();
}

#else /* !VENTURE_HAVE_SQLITE */

gint
main(
	gint	 argc,
	gchar	**argv
){
	/* No store to test: an empty plan, which passes. */
	g_test_init(&argc, &argv, NULL);
	return g_test_run();
}

#endif /* VENTURE_HAVE_SQLITE */
