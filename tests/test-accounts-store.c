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

#include <sqlite3.h>

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

	all = venture_series_store_list_accounts(fixture->store, NULL, NULL, NULL, T0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(all->len, ==, 2);
	g_clear_pointer(&all, g_ptr_array_unref);

	all = venture_series_store_list_accounts(fixture->store, "shared", NULL, NULL, T0, &error);
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

/*
 * A balance with no time of its own inside a snapshot is as of the
 * snapshot. Two snapshots, both taken before the pushes that carried them,
 * the second leaving SILVER out: SILVER's newest point is zero.
 *
 * What breaks if this regresses: the restated balance is stamped with the
 * push's time, which is later than the next snapshot's, so the sweep
 * judges it newer than that snapshot and never empties it -- a purse the
 * character spent reads full for ever.
 */
static void
test_balance_takes_snapshot_time(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesBalance balances[2];
	VentureSeriesAccountSnapshot snapshot;
	VentureSeriesAccountBatch batch;
	g_autoptr(VentureSeriesAccountRow) row = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesAmount *silver;
	VentureSeriesAmount *gold;

	(void)data;

	snapshot.account_key = "a";
	snapshot.at = T0 + HOUR;
	snapshot.covers = VENTURE_SERIES_COVERS_BALANCES;
	balances[0] = balance("a", 100, VENTURE_SERIES_NONE);
	balances[1] = balance("a", 50, VENTURE_SERIES_NONE);
	balances[1].currency = "SILVER";
	batch = batch_new();
	batch.snapshots = &snapshot;
	batch.n_snapshots = 1;
	batch.balances = balances;
	batch.n_balances = 2;
	apply(fixture, &batch, T0 + 10 * HOUR);

	/* Read an hour after the first, pushed later still. */
	snapshot.at = T0 + 2 * HOUR;
	batch.n_balances = 1;
	apply(fixture, &batch, T0 + 11 * HOUR);

	g_assert_true(venture_series_store_get_account(fixture->store, "a", T0, &row, &error));
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_assert_cmpuint(row->balances->len, ==, 2);

	/* By currency: GOLD, then SILVER. The emptied purse first. */
	silver = &g_array_index(row->balances, VentureSeriesAmount, 1);
	gold = &g_array_index(row->balances, VentureSeriesAmount, 0);
	g_assert_cmpstr(silver->currency, ==, "SILVER");
	g_assert_cmpint(silver->amount, ==, 0);
	g_assert_cmpint(silver->at, ==, T0 + 2 * HOUR);
	g_assert_cmpstr(gold->currency, ==, "GOLD");
	g_assert_cmpint(gold->amount, ==, 100);
	g_assert_cmpint(gold->at, ==, T0 + HOUR);
}

/*
 * A read of every position, inbound row, holding, holding line or account
 * that finds more than the bound is refused, saying to narrow it; a page
 * under the bound and a narrower read still answer.
 *
 * What breaks if this regresses: the read stops at the bound in silence,
 * and the mirror, which closes the listing of every position it does not
 * see, closes the listings of every position past it.
 */
static void
test_reads_refuse_past_the_bound(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesAccount accounts[3];
	VentureSeriesHolding holdings[3];
	VentureSeriesPosition positions[3];
	VentureSeriesInbound mail[3];
	VentureSeriesAccountBatch batch;
	VentureSeriesPositionFilter position_filter;
	VentureSeriesInboundFilter inbound_filter;
	VentureSeriesHoldingFilter holding_filter;
	VentureSeriesValueFilter value_filter;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;

	accounts[0] = account("a", "Alpha", "character");
	accounts[1] = account("b", "Beta", "character");
	accounts[2] = account("c", "Gamma", "character");
	accounts[2].group_key = "eu";
	holdings[0] = holding("a", "bag", "ore", 1);
	holdings[1] = holding("a", "bank", "ore", 2);
	holdings[2] = holding("b", "bag", "ore", 3);
	positions[0] = position("p1", "a", "ore", 100, T0 + HOUR);
	positions[1] = position("p2", "a", "ore", 100, T0 + 2 * HOUR);
	positions[2] = position("p3", "b", "ore", 100, T0 + 3 * HOUR);
	mail[0] = inbound("m1", "a", 10, T0 + HOUR);
	mail[1] = inbound("m2", "a", 10, T0 + 2 * HOUR);
	mail[2] = inbound("m3", "b", 10, T0 + 3 * HOUR);

	batch = batch_new();
	batch.accounts = accounts;
	batch.n_accounts = 3;
	batch.holdings = holdings;
	batch.n_holdings = 3;
	batch.positions = positions;
	batch.n_positions = 3;
	batch.inbound = mail;
	batch.n_inbound = 3;
	apply(fixture, &batch, T0);

	venture_series_accounts_set_max_rows(2);
	venture_series_accounts_set_max_accounts(2);

	/* Positions: all of them is three, past two. */
	rows = venture_series_store_list_positions(fixture->store, NULL, &error);
	g_assert_null(rows);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "narrow"));
	g_clear_error(&error);

	/* A page of two is a page, and one account's two are all of them. */
	venture_series_position_filter_init(&position_filter);
	position_filter.count = 2;
	rows = venture_series_store_list_positions(fixture->store, &position_filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	g_clear_pointer(&rows, g_ptr_array_unref);
	g_assert_cmpuint(count_positions(fixture, "a"), ==, 2);

	venture_series_inbound_filter_init(&inbound_filter);
	rows = venture_series_store_list_inbound(fixture->store, &inbound_filter, &error);
	g_assert_null(rows);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	inbound_filter.account_key = "a";
	rows = venture_series_store_list_inbound(fixture->store, &inbound_filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	g_clear_pointer(&rows, g_ptr_array_unref);

	venture_series_holding_filter_init(&holding_filter);
	rows = venture_series_store_list_holdings(fixture->store, &holding_filter, &error);
	g_assert_null(rows);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_cmpint(held(fixture, "a", "bank", "ore"), ==, 2);

	venture_series_value_filter_init(&value_filter);
	value_filter.currency = "GOLD";
	value_filter.now = T0;
	rows = venture_series_store_value_lines(fixture->store, &value_filter, NULL, &error);
	g_assert_null(rows);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	value_filter.account_key = "b";
	rows = venture_series_store_value_lines(fixture->store, &value_filter, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* Three accounts past a bound of two; the ungrouped two are not. */
	rows = venture_series_store_list_accounts(fixture->store, NULL, NULL, NULL, T0, &error);
	g_assert_null(rows);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "accounts"));
	g_clear_error(&error);
	rows = venture_series_store_list_accounts(fixture->store, NULL, "", NULL, T0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	g_clear_pointer(&rows, g_ptr_array_unref);

	venture_series_accounts_set_max_rows(0);
	venture_series_accounts_set_max_accounts(0);
	g_assert_cmpint(venture_series_accounts_get_max_rows(), ==, VENTURE_SERIES_MAX_ACCOUNT_ROWS);
	g_assert_cmpint(venture_series_accounts_get_max_accounts(), ==, VENTURE_SERIES_MAX_ACCOUNTS);

	rows = venture_series_store_list_positions(fixture->store, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 3);
}

/* --- Logins ------------------------------------------------------------------------ */

static VentureSeriesLogin
login(
	const gchar	*key,
	const gchar	*name,
	const gchar	*kind
){
	VentureSeriesLogin row;

	memset(&row, 0, sizeof(row));
	row.key = key;
	row.name = name;
	row.kind = kind;

	return row;
}

/* The login row with @key, or NULL. */
static VentureSeriesLoginRow *
find_login(
	GPtrArray	*rows,
	const gchar	*key
){
	guint i;

	for (i = 0; i < rows->len; i++)
	{
		VentureSeriesLoginRow *row = g_ptr_array_index(rows, i);

		if (0 == g_strcmp0(row->key, key))
			return row;
	}

	return NULL;
}

/*
 * Logins are described by their own lines or named bare by an account,
 * an account keeps its login when a later line leaves it out and loses it
 * on an empty one, and a malformed login fails the whole batch.
 *
 * What breaks if this regresses: a second WoW licence's characters read
 * as the first's, a re-export that omits `login` (an older tsmctl)
 * strands every character outside its login, or a shared warband that
 * moved to `warbank:<group>` keeps pointing at the licence it left.
 */
static void
test_logins(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesLogin logins[2];
	VentureSeriesAccount accounts[4];
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureSeriesAccountRow) row = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesLoginRow *found;

	(void)data;

	logins[0] = login("ZAKMANN", "Main", "game_account");
	logins[0].group_key = "bnet-1";
	logins[0].attrs_json = "{\"region\":\"us\"}";
	accounts[0] = account("Drgold-Thorium", "Drgold", "character");
	accounts[0].login_key = "ZAKMANN";
	accounts[1] = account("Alt-Thorium", "Alt", "character");
	accounts[1].login_key = "53141745#1";
	accounts[2] = account("warbank:bnet-1", "Warband bank", "shared");
	accounts[3] = account("guild:Treasury", "Guild bank", "guild");
	accounts[3].login_key = "";

	batch = batch_new();
	batch.logins = logins;
	batch.n_logins = 1;
	batch.accounts = accounts;
	batch.n_accounts = 4;
	result = apply(fixture, &batch, T0);
	g_assert_cmpint(result.accounts, ==, 4);
	g_assert_cmpint(result.logins, ==, 2);
	g_assert_cmpint(result.logins_new, ==, 2);

	rows = venture_series_store_list_logins(fixture->store, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	/* By name, else key: "53141745#1" sorts before "Main". */
	g_assert_cmpstr(((VentureSeriesLoginRow *)g_ptr_array_index(rows, 0))->key, ==, "53141745#1");
	found = find_login(rows, "ZAKMANN");
	g_assert_cmpstr(found->name, ==, "Main");
	g_assert_cmpstr(found->kind, ==, "game_account");
	g_assert_cmpstr(found->group_key, ==, "bnet-1");
	g_assert_cmpstr(found->attrs_json, ==, "{\"region\":\"us\"}");
	g_assert_cmpint(found->accounts, ==, 1);
	g_assert_cmpint(found->characters, ==, 1);
	g_assert_cmpint(found->first_seen, ==, T0);

	/* Named only by an account: bare, as a row's unknown account is. */
	found = find_login(rows, "53141745#1");
	g_assert_null(found->name);
	g_assert_cmpstr(found->kind, ==, "other");
	g_assert_cmpstr(found->group_key, ==, "");
	g_assert_cmpint(found->accounts, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);

	g_assert_true(venture_series_store_get_account(fixture->store, "Drgold-Thorium", T0, &row, &error));
	g_assert_cmpstr(row->login_key, ==, "ZAKMANN");
	g_assert_cmpstr(row->login_name, ==, "Main");
	g_clear_pointer(&row, venture_series_account_row_free);
	g_assert_true(venture_series_store_get_account(fixture->store, "warbank:bnet-1", T0, &row, &error));
	g_assert_cmpstr(row->login_key, ==, "");
	g_assert_null(row->login_name);
	g_clear_pointer(&row, venture_series_account_row_free);

	/* A later export: the bare login described, Drgold sent without a
	 * login (an older exporter) and the alt sent with an empty one. */
	logins[0] = login("53141745#1", "Alt licence", NULL);
	accounts[0] = account("Drgold-Thorium", NULL, "character");
	accounts[1] = account("Alt-Thorium", NULL, "character");
	accounts[1].login_key = "";
	batch = batch_new();
	batch.logins = logins;
	batch.n_logins = 1;
	batch.accounts = accounts;
	batch.n_accounts = 2;
	result = apply(fixture, &batch, T0 + HOUR);
	g_assert_cmpint(result.logins, ==, 1);
	g_assert_cmpint(result.logins_new, ==, 0);

	g_assert_true(venture_series_store_get_account(fixture->store, "Drgold-Thorium", T0, &row, &error));
	g_assert_cmpstr(row->login_key, ==, "ZAKMANN");
	g_clear_pointer(&row, venture_series_account_row_free);
	g_assert_true(venture_series_store_get_account(fixture->store, "Alt-Thorium", T0, &row, &error));
	g_assert_cmpstr(row->login_key, ==, "");
	g_clear_pointer(&row, venture_series_account_row_free);

	rows = venture_series_store_list_logins(fixture->store, &error);
	found = find_login(rows, "53141745#1");
	g_assert_cmpstr(found->name, ==, "Alt licence");
	g_assert_cmpint(found->accounts, ==, 0);
	g_assert_cmpint(found->first_seen, ==, T0);
	g_assert_cmpint(found->last_seen, ==, T0 + HOUR);
	/* Not named this time: its sighting does not move. */
	g_assert_cmpint(find_login(rows, "ZAKMANN")->last_seen, ==, T0);
	g_clear_pointer(&rows, g_ptr_array_unref);

	rows = venture_series_store_list_accounts(fixture->store, NULL, NULL, "ZAKMANN", T0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = venture_series_store_list_accounts(fixture->store, NULL, NULL, "", T0, &error);
	g_assert_cmpuint(rows->len, ==, 3);
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* A kind outside the contract fails the batch: nothing of it lands. */
	logins[0] = login("Other", "Other", "character");
	accounts[0] = account("New-Thorium", "New", "character");
	accounts[0].login_key = "Other";
	batch = batch_new();
	batch.logins = logins;
	batch.n_logins = 1;
	batch.accounts = accounts;
	batch.n_accounts = 1;
	g_assert_false(venture_series_store_apply_accounts(fixture->store, &batch, T0 + DAY, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_true(venture_series_store_get_account(fixture->store, "New-Thorium", T0, &row, &error));
	g_assert_null(row);
	rows = venture_series_store_list_logins(fixture->store, &error);
	g_assert_null(find_login(rows, "Other"));
}

/* Two instruments' market figures at a venue, in GOLD, as one snapshot
 * sets them. */
static void
market_values(
	Fixture		*fixture,
	const gchar	*venue,
	const gchar	*first,
	gint64		 first_value,
	const gchar	*second,
	gint64		 second_value
){
	VentureSeriesSnapshot *snapshot;
	VentureSeriesStats stats;
	g_autoptr(GError) error = NULL;

	snapshot = venture_series_store_begin_snapshot(fixture->store, venue, "gold", T0, T0 + 60, FALSE,
	                                               &error);
	g_assert_no_error(error);
	venture_series_stats_init(&stats);
	stats.instrument_key = first;
	stats.market_value = first_value;
	g_assert_true(venture_series_snapshot_add_stats(snapshot, &stats, &error));
	venture_series_stats_init(&stats);
	stats.instrument_key = second;
	stats.market_value = second_value;
	g_assert_true(venture_series_snapshot_add_stats(snapshot, &stats, &error));
	g_assert_true(venture_series_store_commit_snapshot(fixture->store, snapshot, NULL, &error));
	g_assert_no_error(error);
}

/*
 * Every reader narrows to a login, and the ledger and the valued holdings
 * group by it: the logins' sums are exact and add up to the whole, the
 * accounts reached through no login (a shared warband) are one group of
 * their own, counted once.
 *
 * What breaks if this regresses: the per-login cards on /accounts add up
 * to more or less than the headline, or the warband several licences
 * share is counted under each of them.
 */
static void
test_login_filters(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesLogin logins[2];
	VentureSeriesAccount accounts[4];
	VentureSeriesHolding holdings[4];
	VentureSeriesPosition positions[2];
	VentureSeriesInbound mail[2];
	VentureSeriesTxn txns[4];
	VentureSeriesAccountBatch batch;
	VentureSeriesHoldingFilter holding_filter;
	VentureSeriesPositionFilter position_filter;
	VentureSeriesInboundFilter inbound_filter;
	VentureSeriesTxnFilter txn_filter;
	VentureSeriesValueFilter value_filter;
	VentureSeriesValueTotals totals;
	VentureSeriesTxnGroup group_by;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint64 sum;
	gint64 count;
	guint i;

	(void)data;

	market_values(fixture, "realm-a", "ore", 100, "herb", 250);

	logins[0] = login("L1", "Main", "game_account");
	logins[1] = login("L2", "Alt", "game_account");
	accounts[0] = account("A", "Anna", "character");
	accounts[0].login_key = "L1";
	accounts[1] = account("B", "Bert", "character");
	accounts[1].login_key = "L1";
	accounts[2] = account("C", "Cora", "character");
	accounts[2].login_key = "L2";
	accounts[3] = account("warbank:bnet", "Warband bank", "shared");

	for (i = 0; i < G_N_ELEMENTS(accounts); i++)
		accounts[i].venue_key = "realm-a";

	holdings[0] = holding("A", "bag", "ore", 10);
	holdings[1] = holding("B", "bag", "herb", 2);
	holdings[2] = holding("C", "bank", "ore", 3);
	holdings[3] = holding("warbank:bnet", "warbank", "herb", 4);
	positions[0] = position("p1", "A", "ore", 120, T0 + DAY);
	positions[1] = position("p2", "C", "ore", 130, T0 + DAY);
	mail[0] = inbound("m1", "B", 500, T0 + DAY);
	mail[1] = inbound("m2", "C", 700, T0 + DAY);
	txns[0] = txn("t1", "A", "sale", "ore", 5, 1000, T0);
	txns[1] = txn("t2", "C", "sale", "ore", 2, 500, T0 + 1);
	txns[2] = txn("t3", "C", "buy", "ore", 1, 200, T0 + 2);
	txns[3] = txn("t4", "warbank:bnet", "income", NULL, VENTURE_SERIES_NONE, 50, T0 + 3);

	batch = batch_new();
	batch.logins = logins;
	batch.n_logins = 2;
	batch.accounts = accounts;
	batch.n_accounts = 4;
	batch.holdings = holdings;
	batch.n_holdings = 4;
	batch.positions = positions;
	batch.n_positions = 2;
	batch.inbound = mail;
	batch.n_inbound = 2;
	batch.txns = txns;
	batch.n_txns = 4;
	apply(fixture, &batch, T0);

	venture_series_holding_filter_init(&holding_filter);
	holding_filter.login_key = "L1";
	rows = venture_series_store_list_holdings(fixture->store, &holding_filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	g_clear_pointer(&rows, g_ptr_array_unref);
	holding_filter.login_key = "";
	rows = venture_series_store_list_holdings(fixture->store, &holding_filter, &error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesHoldingRow *)g_ptr_array_index(rows, 0))->account_key, ==,
	                "warbank:bnet");
	g_clear_pointer(&rows, g_ptr_array_unref);

	venture_series_position_filter_init(&position_filter);
	position_filter.login_key = "L2";
	rows = venture_series_store_list_positions(fixture->store, &position_filter, &error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesPositionRow *)g_ptr_array_index(rows, 0))->key, ==, "p2");
	g_clear_pointer(&rows, g_ptr_array_unref);

	venture_series_inbound_filter_init(&inbound_filter);
	inbound_filter.login_key = "L1";
	rows = venture_series_store_list_inbound(fixture->store, &inbound_filter, &error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesInboundRow *)g_ptr_array_index(rows, 0))->key, ==, "m1");
	g_clear_pointer(&rows, g_ptr_array_unref);

	venture_series_txn_filter_init(&txn_filter);
	txn_filter.login_key = "L2";
	g_assert_true(venture_series_store_count_txns(fixture->store, &txn_filter, &count, &error));
	g_assert_cmpint(count, ==, 2);

	/* The ledger by login: net per login, the shared bank's on its own. */
	g_assert_true(venture_series_txn_group_from_string("login", &group_by));
	g_assert_cmpint(group_by, ==, VENTURE_SERIES_TXN_GROUP_LOGIN);
	venture_series_txn_filter_init(&txn_filter);
	rows = venture_series_store_txn_totals(fixture->store, &txn_filter, group_by, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 3);
	g_assert_cmpstr(((VentureSeriesTxnTotal *)g_ptr_array_index(rows, 0))->key, ==, "");
	g_assert_cmpint(((VentureSeriesTxnTotal *)g_ptr_array_index(rows, 0))->net, ==, 50);
	g_assert_cmpstr(((VentureSeriesTxnTotal *)g_ptr_array_index(rows, 1))->key, ==, "L1");
	g_assert_cmpstr(((VentureSeriesTxnTotal *)g_ptr_array_index(rows, 1))->label, ==, "Main");
	g_assert_cmpint(((VentureSeriesTxnTotal *)g_ptr_array_index(rows, 1))->net, ==, 1000);
	g_assert_cmpstr(((VentureSeriesTxnTotal *)g_ptr_array_index(rows, 2))->key, ==, "L2");
	g_assert_cmpint(((VentureSeriesTxnTotal *)g_ptr_array_index(rows, 2))->net, ==, 300);
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* The holdings valued per login add up to the instruments' total. */
	venture_series_value_filter_init(&value_filter);
	value_filter.currency = "GOLD";
	value_filter.basis = VENTURE_SERIES_VALUE_MARKET;
	value_filter.now = T0 + HOUR;
	rows = venture_series_store_value_instruments(fixture->store, &value_filter, &totals, &error);
	g_assert_no_error(error);
	g_assert_cmpint(totals.value, ==, 10 * 100 + 2 * 250 + 3 * 100 + 4 * 250);
	g_clear_pointer(&rows, g_ptr_array_unref);

	rows = venture_series_store_value_totals(fixture->store, &value_filter, VENTURE_SERIES_VALUE_GROUP_LOGIN,
	                                         &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 3);
	sum = 0;

	for (i = 0; i < rows->len; i++)
	{
		VentureSeriesValueGroupTotal *group = g_ptr_array_index(rows, i);

		sum += group->value;

		if (0 == g_strcmp0(group->key, ""))
		{
			g_assert_cmpint(group->value, ==, 4 * 250);
			g_assert_cmpint(group->lines, ==, 1);
			g_assert_null(group->label);
		}
		else if (0 == g_strcmp0(group->key, "L1"))
		{
			g_assert_cmpint(group->value, ==, 10 * 100 + 2 * 250);
			g_assert_cmpstr(group->label, ==, "Main");
		}
		else
		{
			g_assert_cmpstr(group->key, ==, "L2");
			g_assert_cmpint(group->value, ==, 3 * 100);
			g_assert_cmpint(group->quantity, ==, 3);
		}
	}

	g_assert_cmpint(sum, ==, totals.value);
	g_clear_pointer(&rows, g_ptr_array_unref);

	rows = venture_series_store_value_totals(fixture->store, &value_filter,
	                                         VENTURE_SERIES_VALUE_GROUP_ACCOUNT, &error);
	g_assert_cmpuint(rows->len, ==, 4);
	g_clear_pointer(&rows, g_ptr_array_unref);

	/* One login's holdings, valued alone, are its group's sum. */
	value_filter.login_key = "L1";
	rows = venture_series_store_value_instruments(fixture->store, &value_filter, &totals, &error);
	g_assert_cmpint(totals.value, ==, 10 * 100 + 2 * 250);
	g_assert_cmpint(totals.lines, ==, 2);
}

/*
 * A store from before logins (schema 6) upgrades to one where every
 * account it held reads as reached through no login, and the next batch
 * can name logins at once.
 *
 * What breaks if this regresses: the first push after the upgrade fails
 * on a missing column, or old accounts read a NULL login and vanish from
 * the "no login" group.
 */
static void
test_login_upgrade(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesAccount accounts[1];
	VentureSeriesAccountBatch batch;
	g_autoptr(VentureSeriesAccountRow) row = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	sqlite3 *db;

	(void)data;

	accounts[0] = account("Old-Thorium", "Old", "character");
	batch = batch_new();
	batch.accounts = accounts;
	batch.n_accounts = 1;
	apply(fixture, &batch, T0);
	g_clear_object(&fixture->store);

	/* As the build before logins left it. */
	path = g_build_filename(fixture->dir, "store.db", NULL);
	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db,
	                             "DROP INDEX accounts_login; DROP TABLE logins;"
	                             "ALTER TABLE accounts DROP COLUMN login;"
	                             "PRAGMA user_version = 6;",
	                             NULL, NULL, NULL), ==, SQLITE_OK);
	sqlite3_close(db);

	fixture->store = venture_series_store_open(fixture->dir, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_series_store_schema_version(), ==, 7);

	g_assert_true(venture_series_store_get_account(fixture->store, "Old-Thorium", T0, &row, &error));
	g_assert_cmpstr(row->login_key, ==, "");
	g_assert_null(row->login_name);
	g_clear_pointer(&row, venture_series_account_row_free);

	rows = venture_series_store_list_accounts(fixture->store, NULL, NULL, "", T0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_clear_pointer(&rows, g_ptr_array_unref);

	accounts[0] = account("Old-Thorium", NULL, "character");
	accounts[0].login_key = "ZAKMANN";
	batch = batch_new();
	batch.accounts = accounts;
	batch.n_accounts = 1;
	apply(fixture, &batch, T0 + HOUR);
	g_assert_true(venture_series_store_get_account(fixture->store, "Old-Thorium", T0, &row, &error));
	g_assert_cmpstr(row->login_key, ==, "ZAKMANN");
	g_assert_null(row->login_name);
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
	ADD("balance-takes-snapshot-time", test_balance_takes_snapshot_time);
	ADD("reads-refuse-past-the-bound", test_reads_refuse_past_the_bound);
	ADD("logins", test_logins);
	ADD("login-filters", test_login_filters);
	ADD("login-upgrade", test_login_upgrade);

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
