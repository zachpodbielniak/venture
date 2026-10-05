/*
 * test-series-store.c - One data source's market data, in its own file
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The series store is the one place VENTURE hand-writes SQL for data, so
 * nothing generic checks it: these tests are the whole of its safety net.
 * They drive the real store on a real file -- the schema upgrade path, a
 * file that is not a store, the reader that runs while the writer writes
 * on another thread, a snapshot applied twice, a sale read from two
 * listing sets -- because each of those is a way market history goes
 * quietly wrong and nobody notices until a trade does.
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

	fixture->root = g_dir_make_tmp("venture-series-XXXXXX", &error);
	g_assert_no_error(error);
	fixture->dir = g_build_filename(fixture->root, "series", "source-1", NULL);

	if (NULL == data)
	{
		fixture->store = venture_series_store_open(fixture->dir, &error);
		g_assert_no_error(error);
		g_assert_nonnull(fixture->store);
	}
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_clear_object(&fixture->store);
	venture_test_remove_tree(fixture->root);
	g_free(fixture->root);
	g_free(fixture->dir);
}

/* Marks a fixture that opens its own store. */
static const gchar no_store[] = "no store";

static void
add_listing(
	VentureSeriesSnapshot	*snapshot,
	const gchar		*instrument,
	guint64			 id,
	gint64			 price,
	gint64			 quantity,
	gint64			 expires
){
	VentureSeriesListing listing;
	g_autoptr(GError) error = NULL;

	memset(&listing, 0, sizeof(listing));
	listing.instrument_key = instrument;
	listing.listing_id = id;
	listing.unit_price = price;
	listing.quantity = quantity;
	listing.side = VENTURE_SERIES_SIDE_SELL;
	listing.expires_in_min = expires;

	g_assert_true(venture_series_snapshot_add_listing(snapshot, &listing, &error));
	g_assert_no_error(error);
}

static VentureSeriesSnapshot *
begin(
	VentureSeriesStore	*store,
	const gchar		*venue,
	gint64			 taken_at,
	gboolean		 complete
){
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;

	snapshot = venture_series_store_begin_snapshot(store, venue, "gold", taken_at,
	                                               taken_at + 60, complete,
	                                               &error);
	g_assert_no_error(error);
	g_assert_nonnull(snapshot);

	return snapshot;
}

static VentureSeriesCommitResult
commit(
	VentureSeriesStore	*store,
	VentureSeriesSnapshot	*snapshot
){
	VentureSeriesCommitResult result;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_series_store_commit_snapshot(store, snapshot, &result,
	                                                   &error));
	g_assert_no_error(error);

	return result;
}

/* One venue, one instrument, one listing at @price. */
static void
put_price(
	VentureSeriesStore	*store,
	const gchar		*venue,
	const gchar		*instrument,
	gint64			 taken_at,
	gint64			 price,
	gint64			 quantity
){
	VentureSeriesSnapshot *snapshot;

	snapshot = begin(store, venue, taken_at, FALSE);
	add_listing(snapshot, instrument, 0, price, quantity, -1);
	commit(store, snapshot);
}

static VentureSeriesRow *
current_of(
	VentureSeriesStore	*store,
	const gchar		*venue,
	const gchar		*instrument
){
	VentureSeriesRow *row;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_series_store_get_current(store, venue, instrument, &row,
	                                               &error));
	g_assert_no_error(error);

	return row;
}

static void
set_venue(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*name,
	const gchar		*group
){
	VentureSeriesVenue venue;
	g_autoptr(GError) error = NULL;

	memset(&venue, 0, sizeof(venue));
	venue.key = key;
	venue.name = name;
	venue.group_key = group;
	venue.currency = "GOLD";

	g_assert_true(venture_series_store_upsert_venue(store, &venue, T0, &error));
	g_assert_no_error(error);
}

static void
set_instrument(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*name,
	const gchar		*category
){
	VentureSeriesInstrument instrument;
	g_autoptr(GError) error = NULL;

	memset(&instrument, 0, sizeof(instrument));
	instrument.key = key;
	instrument.name = name;
	instrument.category = category;

	g_assert_true(venture_series_store_upsert_instrument(store, &instrument, T0,
	                                                     NULL, NULL, &error));
	g_assert_no_error(error);
}

static gint64
read_user_version(const gchar *path)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	gint64 version;

	g_assert_cmpint(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), ==,
	                SQLITE_OK);
	g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt,
	                                   NULL), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
	version = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	sqlite3_close(db);

	return version;
}

/* --- Opening ------------------------------------------------------------------ */

/*
 * A new store is created at the current schema, in WAL mode, and opening
 * it again is a no-op rather than a second schema.
 */
static void
test_create(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesStore) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *created = NULL;
	g_autofree gchar *path = NULL;

	path = g_build_filename(fixture->dir, "store.db", NULL);
	g_assert_cmpstr(venture_series_store_get_path(fixture->store), ==, path);
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(read_user_version(path), ==,
	                venture_series_store_schema_version());
	g_assert_cmpuint(venture_series_store_schema_version(), >=, 2);
	g_assert_nonnull(venture_series_store_schema_step(1));
	g_assert_null(venture_series_store_schema_step(0));
	g_assert_null(venture_series_store_schema_step(
	                  venture_series_store_schema_version() + 1));

	created = venture_series_store_get_meta(fixture->store, "created_at", &error);
	g_assert_no_error(error);
	g_assert_nonnull(created);

	again = venture_series_store_open(fixture->dir, &error);
	g_assert_no_error(error);
	g_assert_nonnull(again);
	g_assert_false(venture_series_store_is_reader(again));
}

/*
 * A store written by an older build is brought up to date on open, its
 * rows intact. Step 1 is written by hand here, as an older build would
 * have left it.
 */
static void
test_upgrade(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GPtrArray) venues = NULL;
	g_autoptr(GPtrArray) quotes = NULL;
	VentureSeriesQuote quote;
	sqlite3 *db;

	g_assert_cmpint(g_mkdir_with_parents(fixture->dir, 0700), ==, 0);
	path = g_build_filename(fixture->dir, "store.db", NULL);

	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db, "PRAGMA auto_vacuum = INCREMENTAL",
	                             NULL, NULL, NULL), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db, venture_series_store_schema_step(1),
	                             NULL, NULL, NULL), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db,
	                             "INSERT INTO venues (key, name, first_seen,"
	                             " last_seen) VALUES ('old', 'Old Realm', 1, 1);"
	                             "PRAGMA user_version = 1;",
	                             NULL, NULL, NULL), ==, SQLITE_OK);
	sqlite3_close(db);

	/* A reader does not upgrade: it is the writer's job. */
	g_assert_null(venture_series_store_open_reader(fixture->dir, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_MIGRATION);
	g_clear_error(&error);

	fixture->store = venture_series_store_open(fixture->dir, &error);
	g_assert_no_error(error);
	g_assert_cmpint(read_user_version(path), ==,
	                venture_series_store_schema_version());

	venues = venture_series_store_list_venues(fixture->store, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venues->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesVenueRow *)venues->pdata[0])->name, ==,
	                "Old Realm");

	/* Step 2's tables exist: quotes can be written. */
	memset(&quote, 0, sizeof(quote));
	quote.venue_key = "old";
	quote.instrument_key = "outcome";
	quote.side = VENTURE_SERIES_QUOTE_BACK;
	quote.value = 2150000;
	quote.liquidity = VENTURE_SERIES_NONE;
	quote.taken_at = T0;

	g_assert_true(venture_series_store_add_quotes(fixture->store, &quote, 1,
	                                              NULL, &error));
	g_assert_no_error(error);

	quotes = venture_series_store_list_quotes(fixture->store, "outcome", NULL,
	                                          &error);
	g_assert_cmpuint(quotes->len, ==, 1);
}

/*
 * A store written by a newer build is refused, not written under rules it
 * was not written to -- by the writer and by a reader.
 */
static void
test_future_version_refused(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	sqlite3 *db;

	g_assert_cmpint(g_mkdir_with_parents(fixture->dir, 0700), ==, 0);
	path = g_build_filename(fixture->dir, "store.db", NULL);

	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db, "CREATE TABLE later (x); "
	                             "PRAGMA user_version = 99;", NULL, NULL, NULL),
	                ==, SQLITE_OK);
	sqlite3_close(db);

	store = venture_series_store_open(fixture->dir, &error);
	g_assert_null(store);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_MIGRATION);
	g_assert_nonnull(strstr(error->message, "99"));
	g_clear_error(&error);

	store = venture_series_store_open_reader(fixture->dir, &error);
	g_assert_null(store);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_MIGRATION);

	/* Nothing was written into it. */
	g_assert_cmpint(read_user_version(path), ==, 99);
}

/*
 * A file that is not a store -- garbage, or some other SQLite database --
 * is refused with an error naming it, and never has a schema written
 * into it.
 */
static void
test_corrupt_refused(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *garbage = NULL;
	sqlite3 *db;

	g_assert_cmpint(g_mkdir_with_parents(fixture->dir, 0700), ==, 0);
	path = g_build_filename(fixture->dir, "store.db", NULL);

	garbage = g_strnfill(8192, 'x');
	g_assert_true(g_file_set_contents(path, garbage, -1, &error));

	store = venture_series_store_open(fixture->dir, &error);
	g_assert_null(store);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_assert_nonnull(strstr(error->message, path));
	g_clear_error(&error);

	store = venture_series_store_open_reader(fixture->dir, &error);
	g_assert_null(store);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_clear_error(&error);

	/* Somebody else's SQLite database, pointed at by mistake. */
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db, "CREATE TABLE invoices (id)", NULL, NULL,
	                             NULL), ==, SQLITE_OK);
	sqlite3_close(db);

	store = venture_series_store_open(fixture->dir, &error);
	g_assert_null(store);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_assert_nonnull(strstr(error->message, "not a series store"));
	g_assert_cmpint(read_user_version(path), ==, 0);
}

/* A reader cannot write, and a store that does not exist is not created. */
static void
test_reader_is_read_only(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(VentureSeriesStore) missing = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *nowhere = NULL;
	VentureSeriesSnapshot *snapshot;

	nowhere = g_build_filename(fixture->root, "nowhere", NULL);
	missing = venture_series_store_open_reader(nowhere, &error);
	g_assert_null(missing);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_false(g_file_test(nowhere, G_FILE_TEST_EXISTS));
	g_clear_error(&error);

	reader = venture_series_store_open_reader(fixture->dir, &error);
	g_assert_no_error(error);
	g_assert_true(venture_series_store_is_reader(reader));

	snapshot = venture_series_store_begin_snapshot(reader, "realm", "GOLD", T0,
	                                               T0, TRUE, &error);
	g_assert_null(snapshot);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

/* --- Snapshots ------------------------------------------------------------------ */

/*
 * One snapshot fills current, the hour and the day with the figures the
 * math defines, and keeps the book's tiers for bulk pricing.
 */
static void
test_snapshot_figures(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GArray) hourly = NULL;
	g_autoptr(GArray) daily = NULL;
	g_autoptr(GArray) tiers = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;
	VentureSeriesBulkCost cost;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	const VentureSeriesPoint *point;
	const VentureSeriesDay *day;

	snapshot = begin(fixture->store, "realm-1", T0 + 2 * HOUR + 120, TRUE);
	add_listing(snapshot, "herb", 1, 100, 5, 7200);
	add_listing(snapshot, "herb", 2, 120, 5, 7200);
	add_listing(snapshot, "herb", 3, 100, 10, 7200);
	add_listing(snapshot, "ore", 4, 50, 1, 7200);

	{
		VentureSeriesListing bid;

		memset(&bid, 0, sizeof(bid));
		bid.instrument_key = "ore";
		bid.unit_price = 40;
		bid.quantity = 3;
		bid.side = VENTURE_SERIES_SIDE_BUY;
		bid.expires_in_min = -1;
		g_assert_true(venture_series_snapshot_add_listing(snapshot, &bid, &error));
	}

	result = commit(fixture->store, snapshot);
	g_assert_false(result.duplicate);
	g_assert_false(result.late);
	g_assert_cmpint(result.listings, ==, 5);
	g_assert_cmpint(result.instruments, ==, 2);
	g_assert_cmpint(result.instruments_new, ==, 2);
	g_assert_cmpint(result.instruments_refused, ==, 0);
	g_assert_cmpint(result.rows_written, >, 0);

	row = current_of(fixture->store, "realm-1", "herb");
	g_assert_nonnull(row);
	g_assert_cmpstr(row->currency, ==, "GOLD");
	g_assert_cmpint(row->quantity, ==, 20);
	g_assert_cmpint(row->listings, ==, 3);
	g_assert_cmpint(row->min_price, ==, 100);
	/* 15 at 100 and 5 at 120: mean 105; median 100; market 100. */
	g_assert_cmpint(row->mean, ==, 105);
	g_assert_cmpint(row->median, ==, 100);
	g_assert_cmpint(row->market_value, ==, 100);
	g_assert_cmpint(row->p15, ==, 100);
	g_assert_true(isnan(row->pct_vs_region));
	g_assert_cmpint(row->region_median, ==, VENTURE_SERIES_NONE);
	g_clear_pointer(&row, venture_series_row_free);

	row = current_of(fixture->store, "realm-1", "ore");
	g_assert_cmpint(row->bid_price, ==, 40);
	g_assert_cmpint(row->bid_quantity, ==, 3);

	tiers = venture_series_store_get_tiers(fixture->store, "realm-1", "herb",
	                                       &error);
	g_assert_no_error(error);
	g_assert_cmpuint(tiers->len, ==, 2);
	g_assert_cmpint(g_array_index(tiers, VentureSeriesTier, 0).quantity, ==, 15);
	g_assert_cmpint(g_array_index(tiers, VentureSeriesTier, 1).price, ==, 120);

	/* Bulk: 15 at 100 then 3 at 120. */
	g_assert_true(venture_series_store_bulk_cost(fixture->store, "realm-1", "herb",
	                                             18, &cost, currency, &error));
	g_assert_cmpint(cost.cost, ==, 1860);
	g_assert_true(cost.complete);
	g_assert_cmpstr(currency, ==, "GOLD");

	g_assert_true(venture_series_store_bulk_cost(fixture->store, "realm-1", "herb",
	                                             25, &cost, currency, &error));
	g_assert_false(cost.complete);
	g_assert_cmpint(cost.filled, ==, 20);

	hourly = venture_series_store_hourly(fixture->store, "realm-1", "herb", T0,
	                                     &error);
	g_assert_no_error(error);
	g_assert_cmpuint(hourly->len, ==, 1);
	point = &g_array_index(hourly, VentureSeriesPoint, 0);
	g_assert_cmpint(point->at, ==, T0 + 2 * HOUR);
	g_assert_cmpint(point->min_price, ==, 100);
	g_assert_cmpint(point->quantity, ==, 20);
	g_assert_cmpint(point->market_value, ==, 100);
	g_assert_cmpint(point->listings, ==, 3);
	g_assert_cmpstr(point->currency, ==, "GOLD");

	daily = venture_series_store_daily(fixture->store, "realm-1", "herb", T0,
	                                   &error);
	g_assert_cmpuint(daily->len, ==, 1);
	day = &g_array_index(daily, VentureSeriesDay, 0);
	g_assert_cmpint(day->day_start, ==, T0);
	g_assert_cmpint(day->snapshots, ==, 1);
	g_assert_cmpint(day->min_price, ==, 100);
	g_assert_cmpint(day->max_quantity, ==, 20);
	g_assert_cmpint(day->sale_avg, ==, VENTURE_SERIES_NONE);
}

/*
 * The same venue and time applied twice changes nothing: a retried fetch
 * must not count a day's snapshot twice or double its sales.
 */
static void
test_idempotent(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GArray) daily = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureSeriesRow) row = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;
	guint round;

	for (round = 0; round < 2; round++)
	{
		snapshot = begin(fixture->store, "realm", T0 + HOUR, TRUE);
		add_listing(snapshot, "herb", 1, (0 == round) ? 100 : 1, 5, 7200);
		result = commit(fixture->store, snapshot);
		g_assert_cmpint(result.duplicate, ==, (1 == round));
	}

	g_assert_cmpint(result.rows_written, ==, 0);

	row = current_of(fixture->store, "realm", "herb");
	g_assert_cmpint(row->min_price, ==, 100);

	daily = venture_series_store_daily(fixture->store, "realm", "herb", T0, &error);
	g_assert_cmpuint(daily->len, ==, 1);
	g_assert_cmpint(g_array_index(daily, VentureSeriesDay, 0).snapshots, ==, 1);
}

/*
 * A snapshot older than the venue's newest adds history and changes
 * nothing current: a backfill must not roll the browse page back an hour.
 */
static void
test_late_snapshot(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GArray) hourly = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;
	VentureSeriesVenueState state;

	put_price(fixture->store, "realm", "herb", T0 + 5 * HOUR, 200, 1);

	snapshot = begin(fixture->store, "realm", T0 + 3 * HOUR, TRUE);
	add_listing(snapshot, "herb", 0, 150, 1, -1);
	result = commit(fixture->store, snapshot);
	g_assert_true(result.late);

	row = current_of(fixture->store, "realm", "herb");
	g_assert_cmpint(row->min_price, ==, 200);
	g_assert_cmpint(row->taken_at, ==, T0 + 5 * HOUR);

	hourly = venture_series_store_hourly(fixture->store, "realm", "herb", T0,
	                                     &error);
	g_assert_cmpuint(hourly->len, ==, 2);
	g_assert_cmpint(g_array_index(hourly, VentureSeriesPoint, 0).min_price, ==,
	                150);

	g_assert_true(venture_series_store_get_venue_state(fixture->store, "realm",
	                                                   &state, &error));
	g_assert_cmpint(state.last_taken_at, ==, T0 + 5 * HOUR);
	g_assert_cmpint(state.gaps, ==, 0);
}

/*
 * A complete snapshot that leaves an instrument out means it sold out at
 * that venue; an incomplete one says nothing about what it left out.
 */
static void
test_absent_out_of_stock(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	VentureSeriesSnapshot *snapshot;

	snapshot = begin(fixture->store, "realm", T0, TRUE);
	add_listing(snapshot, "herb", 0, 100, 2, -1);
	add_listing(snapshot, "ore", 0, 50, 2, -1);
	commit(fixture->store, snapshot);

	snapshot = begin(fixture->store, "realm", T0 + HOUR, FALSE);
	add_listing(snapshot, "herb", 0, 101, 2, -1);
	commit(fixture->store, snapshot);

	row = current_of(fixture->store, "realm", "ore");
	g_assert_cmpint(row->quantity, ==, 2);
	g_assert_cmpint(row->min_price, ==, 50);
	g_clear_pointer(&row, venture_series_row_free);

	snapshot = begin(fixture->store, "realm", T0 + 2 * HOUR, TRUE);
	add_listing(snapshot, "herb", 0, 102, 2, -1);
	commit(fixture->store, snapshot);

	row = current_of(fixture->store, "realm", "ore");
	g_assert_cmpint(row->quantity, ==, 0);
	g_assert_cmpint(row->min_price, ==, VENTURE_SERIES_NONE);
	g_assert_cmpint(row->seen_at, ==, T0);
	g_clear_pointer(&row, venture_series_row_free);

	row = current_of(fixture->store, "realm", "herb");
	g_assert_cmpint(row->min_price, ==, 102);
}

/*
 * stock_changed_at marks the snapshot at which a venue's quantity crossed
 * zero, either way, and nothing else: not the first sighting, not a price
 * change, not a stats row that did not say how many.
 *
 * What breaks if this regresses: the out_of_stock and back_in_stock alerts
 * read "changed at the newest snapshot" off this column; stamped on a
 * first sighting, a source's first sync says every instrument is back in
 * stock; stamped on every snapshot, every one says it again.
 */
static void
test_stock_changed_at(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesStats stats;

	/* First sighting: no change yet. */
	snapshot = begin(fixture->store, "realm", T0, TRUE);
	add_listing(snapshot, "ore", 0, 50, 2, -1);
	add_listing(snapshot, "herb", 0, 10, 1, -1);
	commit(fixture->store, snapshot);
	row = current_of(fixture->store, "realm", "ore");
	g_assert_cmpint(row->stock_changed_at, ==, VENTURE_SERIES_NONE);
	g_clear_pointer(&row, venture_series_row_free);

	/* A new price is not a change of stock. */
	snapshot = begin(fixture->store, "realm", T0 + HOUR, TRUE);
	add_listing(snapshot, "ore", 0, 55, 3, -1);
	add_listing(snapshot, "herb", 0, 10, 1, -1);
	commit(fixture->store, snapshot);
	row = current_of(fixture->store, "realm", "ore");
	g_assert_cmpint(row->stock_changed_at, ==, VENTURE_SERIES_NONE);
	g_clear_pointer(&row, venture_series_row_free);

	/* Left out of a complete snapshot: out, stamped then. */
	snapshot = begin(fixture->store, "realm", T0 + 2 * HOUR, TRUE);
	add_listing(snapshot, "herb", 0, 10, 1, -1);
	commit(fixture->store, snapshot);
	row = current_of(fixture->store, "realm", "ore");
	g_assert_cmpint(row->quantity, ==, 0);
	g_assert_cmpint(row->stock_changed_at, ==, T0 + 2 * HOUR);
	g_assert_cmpint(row->taken_at, ==, T0 + 2 * HOUR);
	g_clear_pointer(&row, venture_series_row_free);

	/* Still out an hour later: the stamp stays where it went out. */
	snapshot = begin(fixture->store, "realm", T0 + 3 * HOUR, TRUE);
	add_listing(snapshot, "herb", 0, 10, 1, -1);
	commit(fixture->store, snapshot);
	row = current_of(fixture->store, "realm", "ore");
	g_assert_cmpint(row->stock_changed_at, ==, T0 + 2 * HOUR);
	g_clear_pointer(&row, venture_series_row_free);

	/* Back: stamped at the snapshot that brought it back. */
	snapshot = begin(fixture->store, "realm", T0 + 4 * HOUR, TRUE);
	add_listing(snapshot, "ore", 0, 60, 1, -1);
	add_listing(snapshot, "herb", 0, 10, 1, -1);
	commit(fixture->store, snapshot);
	row = current_of(fixture->store, "realm", "ore");
	g_assert_cmpint(row->quantity, ==, 1);
	g_assert_cmpint(row->stock_changed_at, ==, T0 + 4 * HOUR);
	g_clear_pointer(&row, venture_series_row_free);

	/* A stats row that does not say how many is not a sell-out. */
	memset(&stats, 0, sizeof(stats));
	stats.instrument_key = "herb";
	stats.min_price = 11;
	stats.market_value = VENTURE_SERIES_NONE;
	stats.mean = VENTURE_SERIES_NONE;
	stats.median = VENTURE_SERIES_NONE;
	stats.sale_avg = VENTURE_SERIES_NONE;
	stats.quantity = VENTURE_SERIES_NONE;
	stats.listings = VENTURE_SERIES_NONE;
	stats.sold = VENTURE_SERIES_NONE;
	snapshot = begin(fixture->store, "realm", T0 + 5 * HOUR, FALSE);
	g_assert_true(venture_series_snapshot_add_stats(snapshot, &stats, &error));
	g_assert_no_error(error);
	commit(fixture->store, snapshot);
	row = current_of(fixture->store, "realm", "herb");
	g_assert_cmpint(row->stock_changed_at, ==, VENTURE_SERIES_NONE);
}

/*
 * Precomputed figures stand in for listings, and one instrument cannot be
 * both in one snapshot.
 */
static void
test_stats_snapshot(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GArray) daily = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesStats stats;
	VentureSeriesListing listing;

	memset(&stats, 0, sizeof(stats));
	stats.instrument_key = "commodity";
	stats.min_price = 30;
	stats.market_value = 35;
	stats.mean = VENTURE_SERIES_NONE;
	stats.median = VENTURE_SERIES_NONE;
	stats.sale_avg = 33;
	stats.quantity = 1000;
	stats.listings = VENTURE_SERIES_NONE;
	stats.sold = 10;

	snapshot = begin(fixture->store, "region", T0, TRUE);
	g_assert_true(venture_series_snapshot_add_stats(snapshot, &stats, &error));
	g_assert_no_error(error);

	memset(&listing, 0, sizeof(listing));
	listing.instrument_key = "commodity";
	listing.unit_price = 1;
	listing.quantity = 1;
	listing.expires_in_min = -1;
	g_assert_false(venture_series_snapshot_add_listing(snapshot, &listing, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* A set of figures with no figure in it is not a set of figures. */
	stats.min_price = VENTURE_SERIES_NONE;
	stats.market_value = VENTURE_SERIES_NONE;
	stats.sale_avg = VENTURE_SERIES_NONE;
	stats.quantity = VENTURE_SERIES_NONE;
	stats.sold = VENTURE_SERIES_NONE;
	stats.instrument_key = "nothing";
	g_assert_false(venture_series_snapshot_add_stats(snapshot, &stats, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	commit(fixture->store, snapshot);

	row = current_of(fixture->store, "region", "commodity");
	g_assert_cmpint(row->min_price, ==, 30);
	g_assert_cmpint(row->market_value, ==, 35);
	g_assert_cmpint(row->quantity, ==, 1000);
	g_assert_cmpint(row->mean, ==, VENTURE_SERIES_NONE);

	daily = venture_series_store_daily(fixture->store, "region", "commodity", T0,
	                                   &error);
	g_assert_cmpuint(daily->len, ==, 1);
	g_assert_cmpint(g_array_index(daily, VentureSeriesDay, 0).sold_estimate, ==, 10);
	g_assert_cmpint(g_array_index(daily, VentureSeriesDay, 0).sale_avg, ==, 33);
}

/*
 * Two complete snapshots an hour apart: a long auction that vanished was
 * bought, one that could have expired was not, and a stack that shrank
 * sold the difference. The sales land on the day and in the reference
 * figures.
 */
static void
test_sale_estimate(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GArray) daily = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;
	VentureSeriesReference reference;
	const VentureSeriesDay *day;

	snapshot = begin(fixture->store, "realm", T0 + 10 * HOUR, TRUE);
	add_listing(snapshot, "herb", 101, 100, 5, 7200);
	add_listing(snapshot, "herb", 102, 110, 5, 7200);
	add_listing(snapshot, "herb", 103, 90, 2, 0);
	add_listing(snapshot, "herb", 104, 95, 10, -1);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.sold_estimate, ==, 0);

	snapshot = begin(fixture->store, "realm", T0 + 11 * HOUR, TRUE);
	add_listing(snapshot, "herb", 102, 110, 5, 7200);
	add_listing(snapshot, "herb", 104, 95, 4, -1);
	add_listing(snapshot, "herb", 105, 99, 1, 7200);
	result = commit(fixture->store, snapshot);

	/* 101 sold 5 @ 100; 104 sold 6 @ 95; 103 may have expired. */
	g_assert_cmpint(result.sold_estimate, ==, 11);

	daily = venture_series_store_daily(fixture->store, "realm", "herb", T0, &error);
	g_assert_cmpuint(daily->len, ==, 1);
	day = &g_array_index(daily, VentureSeriesDay, 0);
	g_assert_cmpint(day->sold_estimate, ==, 11);
	g_assert_cmpint(day->expired_estimate, ==, 2);
	/* (500 + 570) / 11 = 97.27 -> 97. */
	g_assert_cmpint(day->sale_avg, ==, 97);
	g_assert_cmpint(day->snapshots, ==, 2);
	/* The fuller snapshot (22 units) is the one the day keeps. */
	g_assert_cmpint(day->max_quantity, ==, 22);
	g_assert_cmpint(day->price_at_max, ==, 90);
	g_assert_cmpint(day->min_price, ==, 90);

	g_assert_true(venture_series_store_reference(fixture->store, "realm", NULL,
	                                             "herb", T0 + 12 * HOUR,
	                                             &reference, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(reference.currency, ==, "GOLD");
	g_assert_cmpint(reference.sale_avg, ==, 97);
	g_assert_cmpfloat_with_epsilon(reference.sale_rate, 11.0 / 13.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(reference.sold_per_day, 11.0, 1e-9);
	g_assert_cmpint(reference.days_14, ==, 1);
}

/*
 * The sale rate and sold-per-day a browse page sorts by are columns on the
 * current row, written by the region recompute with the reference's own
 * arithmetic; the deal filter and the percent bound are precomputed
 * columns too. What breaks if this regresses: a browse page sorts "sale
 * rate" by nothing (every row NULL), a sort that runs over two weeks of
 * history per request, or a deals page listing a venue dearer than the
 * deal price.
 */
static void
test_precomputed_sales_and_deals(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureSeriesRow) row = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesReference reference;
	VentureSeriesFilter filter;
	gint64 count;

	set_venue(fixture->store, "a", "Alpha", "eu");
	set_venue(fixture->store, "b", "Beta", "eu");
	set_venue(fixture->store, "c", "Gamma", "eu");

	/* Venue a sells: the sale estimate's own case, 11 of 13 units. */
	snapshot = begin(fixture->store, "a", T0 + 10 * HOUR, TRUE);
	add_listing(snapshot, "herb", 101, 100, 5, 7200);
	add_listing(snapshot, "herb", 102, 110, 5, 7200);
	add_listing(snapshot, "herb", 103, 90, 2, 0);
	add_listing(snapshot, "herb", 104, 95, 10, -1);
	commit(fixture->store, snapshot);
	snapshot = begin(fixture->store, "a", T0 + 11 * HOUR, TRUE);
	add_listing(snapshot, "herb", 102, 110, 5, 7200);
	add_listing(snapshot, "herb", 104, 95, 4, -1);
	add_listing(snapshot, "herb", 105, 99, 1, 7200);
	commit(fixture->store, snapshot);

	/* b and c only list. */
	put_price(fixture->store, "b", "herb", T0 + 11 * HOUR, 200, 3);
	put_price(fixture->store, "c", "herb", T0 + 11 * HOUR, 150, 2);

	/* Before a recompute there is nothing to sort by. */
	row = current_of(fixture->store, "a", "herb");
	g_assert_true(isnan(row->sale_rate));
	g_clear_pointer(&row, venture_series_row_free);

	g_assert_true(venture_series_store_recompute_region(fixture->store, NULL, T0 + 12 * HOUR,
	                                                    VENTURE_SERIES_NONE, NULL, NULL, &error));
	g_assert_no_error(error);

	/* The same figures the reference computes for the venue. */
	g_assert_true(venture_series_store_reference(fixture->store, "a", NULL, "herb",
	                                             T0 + 12 * HOUR, &reference, &error));
	row = current_of(fixture->store, "a", "herb");
	g_assert_cmpfloat_with_epsilon(row->sale_rate, reference.sale_rate, 1e-9);
	g_assert_cmpfloat_with_epsilon(row->sale_rate, 11.0 / 13.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(row->sold_per_day, 11.0, 1e-9);
	g_clear_pointer(&row, venture_series_row_free);

	/* Listed but nothing sold or expired: no rate, and none a day. */
	row = current_of(fixture->store, "b", "herb");
	g_assert_true(isnan(row->sale_rate));
	g_assert_cmpfloat_with_epsilon(row->sold_per_day, 0.0, 1e-9);
	g_clear_pointer(&row, venture_series_row_free);

	/* Sorted by it, the seller first and the unknowns last either way. */
	venture_series_filter_init(&filter);
	g_assert_true(venture_series_sort_from_string("sale_rate", &filter.sort));
	g_assert_cmpstr(venture_series_sort_to_string(VENTURE_SERIES_SORT_SOLD_PER_DAY), ==,
	                "sold_per_day");
	filter.descending = TRUE;
	rows = venture_series_store_list_current(fixture->store, &filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 3);
	g_assert_cmpstr(((VentureSeriesRow *)rows->pdata[0])->venue_key, ==, "a");
	g_clear_pointer(&rows, g_ptr_array_unref);
	filter.descending = FALSE;
	rows = venture_series_store_list_current(fixture->store, &filter, &error);
	g_assert_cmpstr(((VentureSeriesRow *)rows->pdata[0])->venue_key, ==, "a");
	g_clear_pointer(&rows, g_ptr_array_unref);

	/*
	 * Minimums 95, 200 and 150: the median, and so the deal price (three
	 * venues, under the p33 threshold), is 150. a and c are at or under
	 * it; b is not. At 150 exactly is a deal: "at or under".
	 */
	venture_series_filter_init(&filter);
	filter.deals_only = TRUE;
	filter.sort = VENTURE_SERIES_SORT_PCT_VS_REGION;
	rows = venture_series_store_list_current(fixture->store, &filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	g_assert_cmpstr(((VentureSeriesRow *)rows->pdata[0])->venue_key, ==, "a");
	g_assert_cmpstr(((VentureSeriesRow *)rows->pdata[1])->venue_key, ==, "c");
	g_assert_cmpint(((VentureSeriesRow *)rows->pdata[1])->deal_price, ==, 150);
	g_clear_pointer(&rows, g_ptr_array_unref);
	g_assert_true(venture_series_store_count_current(fixture->store, &filter, &count, &error));
	g_assert_cmpint(count, ==, 2);

	/* A percent bound: 95 is 63.3% of 150, 150 is 100%. */
	filter.max_pct_vs_region = 70.0;
	rows = venture_series_store_list_current(fixture->store, &filter, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesRow *)rows->pdata[0])->venue_key, ==, "a");
	g_clear_pointer(&rows, g_ptr_array_unref);

	filter.max_pct_vs_region = -1.0;
	g_assert_null(venture_series_store_list_current(fixture->store, &filter, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Two weeks on, the history has aged out of the window: cleared. */
	g_assert_true(venture_series_store_recompute_region(fixture->store, NULL, T0 + 20 * DAY,
	                                                    VENTURE_SERIES_NONE, NULL, NULL, &error));
	g_assert_no_error(error);
	row = current_of(fixture->store, "a", "herb");
	g_assert_true(isnan(row->sale_rate));
	g_assert_true(isnan(row->sold_per_day));
}

/*
 * A snapshot whose listings carry no ids says nothing about sales, even
 * after one that did: otherwise a source that stopped sending ids would
 * read as everything selling at once.
 */
static void
test_sale_needs_ids(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;

	snapshot = begin(fixture->store, "realm", T0, TRUE);
	add_listing(snapshot, "herb", 7, 100, 5, 7200);
	commit(fixture->store, snapshot);

	snapshot = begin(fixture->store, "realm", T0 + HOUR, TRUE);
	add_listing(snapshot, "herb", 0, 100, 5, 7200);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.sold_estimate, ==, 0);
}

/*
 * An incomplete snapshot between two complete ones keeps the last
 * complete listing set, so the next complete one still finds what sold.
 * The incomplete one never loaded the set and wrote an empty one back,
 * and every sale since the last complete snapshot was lost -- for every
 * exec source, whose snapshots are incomplete unless they say otherwise.
 */
static void
test_sale_across_incomplete(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;

	snapshot = begin(fixture->store, "realm", T0, TRUE);
	add_listing(snapshot, "herb", 101, 100, 5, 4 * HOUR);
	add_listing(snapshot, "herb", 102, 110, 5, 4 * HOUR);
	commit(fixture->store, snapshot);

	snapshot = begin(fixture->store, "realm", T0 + HOUR, FALSE);
	add_listing(snapshot, "herb", 102, 110, 5, 3 * HOUR);
	commit(fixture->store, snapshot);

	snapshot = begin(fixture->store, "realm", T0 + 2 * HOUR, TRUE);
	add_listing(snapshot, "herb", 102, 110, 5, 2 * HOUR);
	result = commit(fixture->store, snapshot);

	/* 101 left with two of its four hours to run: bought. */
	g_assert_cmpint(result.sold_estimate, ==, 5);
}

/*
 * A sale is valued at the previous listing set's price, so a set is diffed
 * only against a snapshot in the currency it was priced in. A venue whose
 * snapshots switch currency starts a new set: nothing sold is booked across
 * the switch, and the next diff is within the new currency at its prices.
 *
 * What breaks if this regresses: a venue that changed currency books
 * gold prices as silver -- a sale average and a value off by the exchange
 * rate, in the daily row every reference figure and scan reads.
 */
static void
test_sale_within_one_currency(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GArray) daily = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;
	guint i;

	(void)data;

	snapshot = begin(fixture->store, "realm", T0 + 10 * HOUR, TRUE);
	add_listing(snapshot, "herb", 101, 100, 5, 7200);
	add_listing(snapshot, "herb", 102, 110, 5, 7200);
	commit(fixture->store, snapshot);

	/* The same venue, now priced in silver: 101 is gone, but the gold
	 * set is not diffed against it. */
	snapshot = venture_series_store_begin_snapshot(fixture->store, "realm", "silver",
	                                               T0 + 11 * HOUR, T0 + 11 * HOUR + 60,
	                                               TRUE, &error);
	g_assert_no_error(error);
	add_listing(snapshot, "herb", 102, 11000, 5, 7200);
	add_listing(snapshot, "herb", 103, 12000, 3, 7200);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.sold_estimate, ==, 0);

	/* Within silver, 103 sells, valued at its silver price. */
	snapshot = venture_series_store_begin_snapshot(fixture->store, "realm", "silver",
	                                               T0 + 12 * HOUR, T0 + 12 * HOUR + 60,
	                                               TRUE, &error);
	g_assert_no_error(error);
	add_listing(snapshot, "herb", 102, 11000, 5, 7200);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.sold_estimate, ==, 3);

	daily = venture_series_store_daily(fixture->store, "realm", "herb", T0, &error);
	g_assert_no_error(error);

	for (i = 0; i < daily->len; i++)
	{
		const VentureSeriesDay *day = &g_array_index(daily, VentureSeriesDay, i);

		if (0 == g_ascii_strcasecmp(day->currency, "GOLD"))
			g_assert_cmpint(day->sold_estimate, ==, 0);
		else
		{
			g_assert_cmpstr(day->currency, ==, "SILVER");
			g_assert_cmpint(day->sold_estimate, ==, 3);
			g_assert_cmpint(day->sale_avg, ==, 12000);
		}
	}
}

/*
 * A store from before the listing set's currency was kept learns it on
 * upgrade from the snapshot the set was taken from, so the first diff
 * after the upgrade still counts its sales; a set whose snapshot was
 * purged has no recorded currency and restarts once instead of guessing.
 */
static void
test_sale_currency_upgrade(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;
	sqlite3 *db;

	(void)data;

	snapshot = begin(fixture->store, "realm", T0, TRUE);
	add_listing(snapshot, "herb", 101, 100, 5, 4 * HOUR);
	add_listing(snapshot, "herb", 102, 110, 5, 4 * HOUR);
	commit(fixture->store, snapshot);
	snapshot = begin(fixture->store, "purged", T0, TRUE);
	add_listing(snapshot, "herb", 201, 100, 5, 4 * HOUR);
	add_listing(snapshot, "herb", 202, 110, 5, 4 * HOUR);
	commit(fixture->store, snapshot);
	g_clear_object(&fixture->store);

	/* As an older build left it: no column, version 4, and one venue's
	 * snapshot log purged. Steps 6 and 7 (the accounts, the logins) are
	 * undone too, since a version-4 store never had them. */
	path = g_build_filename(fixture->dir, "store.db", NULL);
	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(db,
	                             "DROP TABLE logins;"
	                             "DROP TABLE accounts; DROP TABLE account_balances;"
	                             "DROP TABLE account_holdings; DROP TABLE account_positions;"
	                             "DROP TABLE account_inbound; DROP TABLE external_txns;"
	                             "ALTER TABLE current DROP COLUMN source_historical;"
	                             "ALTER TABLE current DROP COLUMN source_sale_rate;"
	                             "ALTER TABLE current DROP COLUMN source_sold_per_day;"
	                             "ALTER TABLE venue_state DROP COLUMN listing_set_currency;"
	                             "DELETE FROM snapshots WHERE venue_id ="
	                             "  (SELECT id FROM venues WHERE key = 'purged');"
	                             "PRAGMA user_version = 4;",
	                             NULL, NULL, NULL), ==, SQLITE_OK);
	sqlite3_close(db);

	fixture->store = venture_series_store_open(fixture->dir, &error);
	g_assert_no_error(error);
	g_assert_cmpint(read_user_version(path), ==, venture_series_store_schema_version());

	snapshot = begin(fixture->store, "realm", T0 + HOUR, TRUE);
	add_listing(snapshot, "herb", 102, 110, 5, 3 * HOUR);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.sold_estimate, ==, 5);

	snapshot = begin(fixture->store, "purged", T0 + HOUR, TRUE);
	add_listing(snapshot, "herb", 202, 110, 5, 3 * HOUR);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.sold_estimate, ==, 0);

	/* And from there on it diffs as usual. */
	snapshot = begin(fixture->store, "purged", T0 + 2 * HOUR, TRUE);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.sold_estimate, ==, 5);
}

/*
 * A provider's sold count has no ceiling and the daily row saturates at
 * INT64_MAX rather than overflowing. The region recompute then summed two
 * such days with SQL SUM(), which raises "integer overflow", and the whole
 * store's sale rates and deal figures stopped updating until the day aged
 * out of the window.
 */
static void
test_saturated_sales_recompute(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesStats stats;
	guint i;

	for (i = 0; i < 2; i++)
	{
		memset(&stats, 0, sizeof(stats));
		stats.instrument_key = "commodity";
		stats.min_price = 30;
		stats.market_value = VENTURE_SERIES_NONE;
		stats.mean = VENTURE_SERIES_NONE;
		stats.median = VENTURE_SERIES_NONE;
		stats.sale_avg = VENTURE_SERIES_NONE;
		stats.quantity = 10;
		stats.listings = VENTURE_SERIES_NONE;
		stats.sold = (0 == i) ? G_MAXINT64 : 1;

		snapshot = begin(fixture->store, "region", T0 + i * DAY, TRUE);
		g_assert_true(venture_series_snapshot_add_stats(snapshot, &stats, &error));
		g_assert_no_error(error);
		commit(fixture->store, snapshot);
	}

	g_assert_true(venture_series_store_recompute_region(fixture->store, NULL, T0 + 2 * DAY,
	                                                    VENTURE_SERIES_NONE, NULL, NULL, &error));
	g_assert_no_error(error);
}

/* --- Region ---------------------------------------------------------------------- */

/*
 * Three venues in a group list one item at 100, 200 and 300: the region
 * median and deal price are 200 (fewer than 15 venues), each venue's
 * percent of it lands on current, and the venue index counts who is
 * cheap. A venue in another group is untouched.
 */
static void
test_region_small(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GPtrArray) index = NULL;
	g_autoptr(GPtrArray) others = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesRegion region;
	VentureSeriesRegionResult result;
	const VentureSeriesVenueStats *a;

	set_venue(fixture->store, "a", "Alpha", "eu");
	set_venue(fixture->store, "b", "Beta", "eu");
	set_venue(fixture->store, "c", "Gamma", "eu");
	set_venue(fixture->store, "z", "Zeta", "us");

	put_price(fixture->store, "a", "item", T0, 100, 1);
	put_price(fixture->store, "b", "item", T0, 200, 2);
	put_price(fixture->store, "c", "item", T0, 300, 3);
	put_price(fixture->store, "z", "item", T0, 1000, 1);

	g_assert_true(venture_series_store_recompute_region(fixture->store, NULL,
	                                                    T0 + 60,
	                                                    VENTURE_SERIES_NONE, NULL,
	                                                    &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result.groups, ==, 2);
	g_assert_cmpint(result.rows, ==, 2);
	g_assert_cmpint(result.current_updated, ==, 4);

	g_assert_true(venture_series_store_get_region(fixture->store, "eu", "item",
	                                              NULL, &region, &error));
	g_assert_true(region.found);
	g_assert_cmpstr(region.currency, ==, "GOLD");
	g_assert_cmpint(region.median_min, ==, 200);
	g_assert_cmpint(region.deal_price, ==, 200);
	g_assert_cmpint(region.p33, ==, 200);
	g_assert_cmpint(region.venues_offering, ==, 3);
	g_assert_cmpint(region.total_quantity, ==, 6);
	g_assert_cmpint(region.market_avg, ==, 200);
	g_assert_cmpint(region.computed_at, ==, T0 + 60);

	row = current_of(fixture->store, "a", "item");
	g_assert_cmpint(row->region_median, ==, 200);
	g_assert_cmpint(row->deal_price, ==, 200);
	g_assert_cmpfloat_with_epsilon(row->pct_vs_region, 50.0, 1e-9);
	g_clear_pointer(&row, venture_series_row_free);

	row = current_of(fixture->store, "z", "item");
	g_assert_cmpint(row->region_median, ==, 1000);
	g_clear_pointer(&row, venture_series_row_free);

	/*
	 * A new snapshot keeps the percent fresh against the stored median
	 * (400 of 200) before any recompute...
	 */
	put_price(fixture->store, "a", "item", T0 + HOUR, 400, 1);
	row = current_of(fixture->store, "a", "item");
	g_assert_cmpfloat_with_epsilon(row->pct_vs_region, 200.0, 1e-9);
	g_clear_pointer(&row, venture_series_row_free);

	/* ...and the recompute moves the median to 300 on all three rows. */
	g_assert_true(venture_series_store_recompute_region(fixture->store, "eu",
	                                                    T0 + 120,
	                                                    VENTURE_SERIES_NONE, NULL,
	                                                    &result, &error));
	g_assert_cmpint(result.groups, ==, 1);
	g_assert_cmpint(result.current_updated, ==, 3);

	row = current_of(fixture->store, "a", "item");
	g_assert_cmpint(row->region_median, ==, 300);
	g_assert_cmpfloat_with_epsilon(row->pct_vs_region, 400.0 * 100.0 / 300.0,
	                               1e-9);
	g_clear_pointer(&row, venture_series_row_free);

	/* The other group was not part of it. */
	row = current_of(fixture->store, "z", "item");
	g_assert_cmpint(row->region_median, ==, 1000);

	/* Recomputing with nothing new rewrites nothing. */
	g_assert_true(venture_series_store_recompute_region(fixture->store, "eu",
	                                                    T0 + 180,
	                                                    VENTURE_SERIES_NONE, NULL,
	                                                    &result, &error));
	g_assert_cmpint(result.current_updated, ==, 0);

	index = venture_series_store_venue_index(fixture->store, "eu", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(index->len, ==, 3);
	a = (const VentureSeriesVenueStats *)index->pdata[0];
	g_assert_cmpstr(a->venue_key, ==, "a");
	g_assert_cmpstr(a->venue_name, ==, "Alpha");
	g_assert_cmpint(a->instruments, ==, 1);
	g_assert_cmpint(a->cheaper, ==, 0);
	g_assert_cmpint(a->dearer, ==, 1);
	g_assert_cmpfloat_with_epsilon(a->avg_ratio, 400.0 / 300.0, 1e-9);
	g_assert_cmpint(a->last_taken_at, ==, T0 + HOUR);
	g_assert_cmpint(((const VentureSeriesVenueStats *)index->pdata[1])->cheaper,
	                ==, 1);
	g_assert_cmpint(((const VentureSeriesVenueStats *)index->pdata[2])->equal,
	                ==, 1);

	/* Other venues, cheapest first, within the group. */
	others = venture_series_store_other_venues(fixture->store, "item", "eu",
	                                           &error);
	g_assert_cmpuint(others->len, ==, 3);
	g_assert_cmpstr(((VentureSeriesRow *)others->pdata[0])->venue_key, ==, "b");
	g_assert_cmpstr(((VentureSeriesRow *)others->pdata[2])->venue_key, ==, "a");
}

/*
 * With fifteen venues the deal price is the one a third of the way up,
 * not the median; and below the minimum value there is no deal at all.
 */
static void
test_region_deal_price(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	VentureSeriesRegion region;
	guint i;

	for (i = 0; i < 15; i++)
	{
		g_autofree gchar *venue = g_strdup_printf("realm-%02u", i);

		set_venue(fixture->store, venue, NULL, "eu");
		put_price(fixture->store, venue, "item", T0, (gint64)(i + 1) * 10, 1);
		put_price(fixture->store, venue, "cheap", T0 + 1, 2, 1);
	}

	g_assert_true(venture_series_store_recompute_region(fixture->store, NULL, T0,
	                                                    50, "gold", NULL,
	                                                    &error));
	g_assert_no_error(error);

	g_assert_true(venture_series_store_get_region(fixture->store, "eu", "item",
	                                              "GOLD", &region, &error));
	g_assert_cmpint(region.venues_offering, ==, 15);
	g_assert_cmpint(region.median_min, ==, 80);
	g_assert_cmpint(region.deal_price, ==, 60);

	g_assert_true(venture_series_store_get_region(fixture->store, "eu", "cheap",
	                                              NULL, &region, &error));
	g_assert_true(region.found);
	g_assert_cmpint(region.median_min, ==, 2);
	g_assert_cmpint(region.deal_price, ==, VENTURE_SERIES_NONE);

	/* A bound in another currency bounds nothing here. */
	g_assert_true(venture_series_store_recompute_region(fixture->store, NULL, T0,
	                                                    50, "USD", NULL, &error));
	g_assert_true(venture_series_store_get_region(fixture->store, "eu", "cheap",
	                                              NULL, &region, &error));
	g_assert_cmpint(region.deal_price, ==, 2);

	g_assert_true(venture_series_store_get_region(fixture->store, "us", "item",
	                                              NULL, &region, &error));
	g_assert_false(region.found);
}

/* --- Browsing ------------------------------------------------------------------- */

static GPtrArray *
list(
	VentureSeriesStore		*store,
	const VentureSeriesFilter	*filter
){
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	rows = venture_series_store_list_current(store, filter, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);

	return rows;
}

#define ROW(rows, i) ((const VentureSeriesRow *)(rows)->pdata[i])

/*
 * Filters narrow, sorts order with unknowns last both ways, paging is
 * stable, and a search finds names regardless of case. Nothing from a
 * filter reaches SQL as text: a search for "%" finds a literal percent.
 */
static void
test_list_current(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	VentureSeriesFilter filter;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesSort sort;
	gint64 count;

	set_venue(fixture->store, "a", "Alpha", "eu");
	set_venue(fixture->store, "b", "Beta", "eu");
	set_instrument(fixture->store, "peacebloom", "Peacebloom", "herbs");
	set_instrument(fixture->store, "lotus", "Black Lotus", "herbs/rare");
	set_instrument(fixture->store, "herbsx", "Odd 100% Thing", "herbsx");
	set_instrument(fixture->store, "copper", "Copper Ore", "ore");

	snapshot = begin(fixture->store, "a", T0, TRUE);
	add_listing(snapshot, "peacebloom", 0, 10, 100, -1);
	add_listing(snapshot, "lotus", 0, 5000, 1, -1);
	add_listing(snapshot, "herbsx", 0, 7, 3, -1);
	add_listing(snapshot, "copper", 0, 20, 50, -1);
	commit(fixture->store, snapshot);

	snapshot = begin(fixture->store, "b", T0, TRUE);
	add_listing(snapshot, "peacebloom", 0, 12, 10, -1);
	add_listing(snapshot, "copper", 0, 18, 5, -1);
	commit(fixture->store, snapshot);

	/* Copper goes out of stock at b: its row stays, unpriced. */
	snapshot = begin(fixture->store, "b", T0 + HOUR, TRUE);
	add_listing(snapshot, "peacebloom", 0, 12, 10, -1);
	commit(fixture->store, snapshot);

	venture_series_filter_init(&filter);

	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpuint(rows->len, ==, 6);
		g_assert_cmpint(ROW(rows, 0)->min_price, ==, 7);
		g_assert_cmpint(ROW(rows, 4)->min_price, ==, 5000);
		/* Unknown last ascending... */
		g_assert_cmpint(ROW(rows, 5)->min_price, ==, VENTURE_SERIES_NONE);
	}

	filter.descending = TRUE;
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpint(ROW(rows, 0)->min_price, ==, 5000);
		/* ...and last descending. */
		g_assert_cmpint(ROW(rows, 5)->min_price, ==, VENTURE_SERIES_NONE);
	}

	/* Paging. */
	filter.descending = FALSE;
	filter.offset = 1;
	filter.count = 2;
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpuint(rows->len, ==, 2);
		g_assert_cmpint(ROW(rows, 0)->min_price, ==, 10);
		g_assert_cmpint(ROW(rows, 1)->min_price, ==, 12);
	}

	/* Category prefix: herbs and herbs/rare, never herbsx. */
	venture_series_filter_init(&filter);
	filter.category_prefix = "herbs";
	filter.in_stock_only = TRUE;
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpuint(rows->len, ==, 3);
	}
	g_assert_true(venture_series_store_count_current(fixture->store, &filter,
	                                                 &count, &error));
	g_assert_cmpint(count, ==, 3);

	/* Search, ignoring case, by name or key. */
	venture_series_filter_init(&filter);
	filter.search = "LOTUS";
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpuint(rows->len, ==, 1);
		g_assert_cmpstr(ROW(rows, 0)->instrument_name, ==, "Black Lotus");
		g_assert_cmpstr(ROW(rows, 0)->venue_name, ==, "Alpha");
		g_assert_cmpstr(ROW(rows, 0)->category, ==, "herbs/rare");
	}

	filter.search = "%";
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpuint(rows->len, ==, 1);
		g_assert_cmpstr(ROW(rows, 0)->instrument_key, ==, "herbsx");
	}

	/* Venues by key, and by group. */
	{
		const gchar *const venues[] = { "b", NULL };

		venture_series_filter_init(&filter);
		filter.venue_keys = venues;
		{
			g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

			g_assert_cmpuint(rows->len, ==, 2);
		}
	}

	/* A minimum value, in its own currency only. */
	venture_series_filter_init(&filter);
	filter.min_value = 15;
	g_assert_null(venture_series_store_list_current(fixture->store, &filter,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	filter.min_value_currency = "gold";
	filter.sort = VENTURE_SERIES_SORT_MARKET_VALUE;
	filter.descending = TRUE;
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpuint(rows->len, ==, 2);
		g_assert_cmpstr(ROW(rows, 0)->instrument_key, ==, "lotus");
		g_assert_cmpstr(ROW(rows, 1)->instrument_key, ==, "copper");
	}

	filter.min_value_currency = "USD";
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpuint(rows->len, ==, 0);
	}

	/* Sort by name, and the allowlist's spellings. */
	venture_series_filter_init(&filter);
	filter.sort = VENTURE_SERIES_SORT_NAME;
	filter.venue_keys = NULL;
	filter.group_key = "eu";
	filter.in_stock_only = TRUE;
	{
		g_autoptr(GPtrArray) rows = list(fixture->store, &filter);

		g_assert_cmpstr(ROW(rows, 0)->instrument_name, ==, "Black Lotus");
	}

	g_assert_true(venture_series_sort_from_string("pct_vs_region", &sort));
	g_assert_cmpint(sort, ==, VENTURE_SERIES_SORT_PCT_VS_REGION);
	g_assert_cmpstr(venture_series_sort_to_string(sort), ==, "pct_vs_region");
	g_assert_false(venture_series_sort_from_string("min_price; DROP TABLE x",
	                                               &sort));

	venture_series_filter_init(&filter);
	filter.count = VENTURE_SERIES_MAX_PAGE + 1;
	g_assert_null(venture_series_store_list_current(fixture->store, &filter,
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- History ---------------------------------------------------------------------- */

/*
 * Hourly points across days build the weekday-by-hour map; the last
 * snapshot in an hour is the hour's point.
 */
static void
test_heat(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GArray) hourly = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesHeat heat;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];

	/* Monday 01:10 then 01:50: the later one is the hour. */
	put_price(fixture->store, "realm", "herb", T0 + HOUR + 600, 999, 1);
	put_price(fixture->store, "realm", "herb", T0 + HOUR + 3000, 10, 1);
	/* The next Monday at 01:20, and a Tuesday at 05:00. */
	put_price(fixture->store, "realm", "herb", T0 + 7 * DAY + HOUR + 1200, 20, 1);
	put_price(fixture->store, "realm", "herb", T0 + 8 * DAY + 5 * HOUR, 40, 1);

	hourly = venture_series_store_hourly(fixture->store, "realm", "herb", T0,
	                                     &error);
	g_assert_cmpuint(hourly->len, ==, 3);
	g_assert_cmpint(g_array_index(hourly, VentureSeriesPoint, 0).min_price, ==, 10);

	g_assert_true(venture_series_store_heat(fixture->store, "realm", "herb", T0,
	                                        0, &heat, currency, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(currency, ==, "GOLD");
	g_assert_cmpuint(heat.count[0][1], ==, 2);
	g_assert_cmpint(heat.value[0][1], ==, 15);
	g_assert_cmpuint(heat.count[1][5], ==, 1);
	g_assert_cmpint(heat.value[1][5], ==, 40);

	/* Read in UTC+10, Tuesday 05:00 UTC is Tuesday 15:00. */
	g_assert_true(venture_series_store_heat(fixture->store, "realm", "herb", T0,
	                                        10 * HOUR, &heat, currency, &error));
	g_assert_cmpuint(heat.count[1][15], ==, 1);
}

/*
 * The 14-day market value weights each day's market value by 0.5^(d/2.1)
 * from today; a group's day is the mean across its venues.
 */
static void
test_reference(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	VentureSeriesReference reference;
	gint64 values[3];

	set_venue(fixture->store, "a", NULL, "eu");
	set_venue(fixture->store, "b", NULL, "eu");

	/* Today 100, two days ago 200 at venue a. */
	put_price(fixture->store, "a", "herb", T0 + 2 * DAY, 100, 1);
	put_price(fixture->store, "a", "herb", T0, 200, 1);
	/* Venue b today at 300: the group's today is (100 + 300) / 2. */
	put_price(fixture->store, "b", "herb", T0 + 2 * DAY, 300, 1);

	g_assert_true(venture_series_store_reference(fixture->store, "a", NULL,
	                                             "herb", T0 + 2 * DAY + HOUR,
	                                             &reference, &error));
	g_assert_no_error(error);

	values[0] = 100;
	values[1] = VENTURE_SERIES_NONE;
	values[2] = 200;
	g_assert_cmpint(reference.market_14d, ==,
	                venture_series_math_ewma(values, 3, NULL));
	g_assert_cmpint(reference.market_14d, ==, 134);
	g_assert_cmpint(reference.historical_60d, ==, 150);
	g_assert_cmpint(reference.days_14, ==, 2);
	g_assert_true(isnan(reference.sale_rate));

	g_assert_true(venture_series_store_reference(fixture->store, NULL, "eu",
	                                             "herb", T0 + 2 * DAY + HOUR,
	                                             &reference, &error));
	values[0] = 200;
	g_assert_cmpint(reference.market_14d, ==,
	                venture_series_math_ewma(values, 3, NULL));

	/* Nothing known is NONE, not zero. */
	g_assert_true(venture_series_store_reference(fixture->store, "a", NULL,
	                                             "unknown", T0, &reference,
	                                             &error));
	g_assert_cmpint(reference.market_14d, ==, VENTURE_SERIES_NONE);
	g_assert_cmpstr(reference.currency, ==, "");
}

/*
 * The interval is learned from the gaps between snapshots, and the cursor
 * is per venue.
 */
static void
test_venue_state(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cursor = NULL;
	VentureSeriesVenueState state;
	guint i;

	for (i = 0; i < 6; i++)
		put_price(fixture->store, "realm", "herb", T0 + (gint64)i * 1800, 10, 1);

	g_assert_true(venture_series_store_get_venue_state(fixture->store, "realm",
	                                                   &state, &error));
	g_assert_true(state.found);
	g_assert_cmpint(state.gaps, ==, 5);
	g_assert_cmpint(state.interval_seconds, ==, 1800);
	g_assert_cmpint(state.next_expected, ==, T0 + 5 * 1800 + 1800);

	g_assert_true(venture_series_store_set_venue_cursor(fixture->store, "realm",
	                                                    "page=2", &error));
	cursor = venture_series_store_get_venue_cursor(fixture->store, "realm",
	                                               &error);
	g_assert_cmpstr(cursor, ==, "page=2");

	/* The cursor survives the next snapshot. */
	put_price(fixture->store, "realm", "herb", T0 + DAY, 10, 1);
	g_clear_pointer(&cursor, g_free);
	cursor = venture_series_store_get_venue_cursor(fixture->store, "realm",
	                                               &error);
	g_assert_cmpstr(cursor, ==, "page=2");

	g_assert_false(venture_series_store_set_venue_cursor(fixture->store, "nope",
	                                                     "x", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	g_assert_true(venture_series_store_get_venue_state(fixture->store, "nope",
	                                                   &state, &error));
	g_assert_false(state.found);
}

/* --- Retention and the cap ------------------------------------------------------ */

/*
 * Purging keeps exactly N days counting today, deletes for good, and 0
 * keeps forever.
 */
static void
test_purge(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	VentureSeriesPurgeResult result;
	gint64 now;
	guint i;

	for (i = 0; i < 20; i++)
		put_price(fixture->store, "realm", "herb", T0 + (gint64)i * DAY, 10, 1);

	now = T0 + 19 * DAY + HOUR;

	g_assert_true(venture_series_store_purge(fixture->store, now, 0, 0, &result,
	                                         &error));
	g_assert_cmpint(result.hourly, ==, 0);
	g_assert_cmpint(result.daily, ==, 0);

	g_assert_true(venture_series_store_purge(fixture->store, now, 14, 0, &result,
	                                         &error));
	g_assert_no_error(error);
	g_assert_cmpint(result.hourly, ==, 6);
	g_assert_cmpint(result.snapshots, ==, 6);
	g_assert_cmpint(result.daily, ==, 0);

	{
		g_autoptr(GArray) hourly = venture_series_store_hourly(
			fixture->store, "realm", "herb", T0, &error);
		g_autoptr(GArray) daily = venture_series_store_daily(
			fixture->store, "realm", "herb", T0, &error);

		g_assert_cmpuint(hourly->len, ==, 14);
		g_assert_cmpint(g_array_index(hourly, VentureSeriesPoint, 0).at, ==,
		                T0 + 6 * DAY);
		g_assert_cmpuint(daily->len, ==, 20);
	}

	g_assert_true(venture_series_store_purge(fixture->store, now, 14, 5, &result,
	                                         &error));
	g_assert_cmpint(result.daily, ==, 15);
}

/*
 * Past the cap, an instrument never seen is refused and counted, while
 * the ones already known keep updating -- the history being paid for
 * keeps arriving.
 */
static void
test_cap(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesCommitResult result;
	gboolean refused;
	guint64 bytes;

	put_price(fixture->store, "realm", "known", T0, 10, 1);

	g_assert_true(venture_series_store_get_size(fixture->store, &bytes, &error));
	g_assert_cmpuint(bytes, >, 0);

	venture_series_store_set_max_bytes(fixture->store, 1);

	snapshot = begin(fixture->store, "realm", T0 + HOUR, TRUE);
	add_listing(snapshot, "known", 0, 11, 1, -1);
	add_listing(snapshot, "new", 0, 5, 2, -1);
	add_listing(snapshot, "new", 0, 6, 2, -1);
	result = commit(fixture->store, snapshot);

	g_assert_cmpint(result.instruments, ==, 1);
	g_assert_cmpint(result.instruments_refused, ==, 1);
	g_assert_cmpint(result.listings_refused, ==, 2);
	g_assert_cmpint(result.listings, ==, 1);

	row = current_of(fixture->store, "realm", "known");
	g_assert_cmpint(row->min_price, ==, 11);
	g_clear_pointer(&row, venture_series_row_free);
	row = current_of(fixture->store, "realm", "new");
	g_assert_null(row);

	{
		VentureSeriesInstrument instrument;

		memset(&instrument, 0, sizeof(instrument));
		instrument.key = "another";
		g_assert_true(venture_series_store_upsert_instrument(
			fixture->store, &instrument, T0, NULL, &refused, &error));
		g_assert_true(refused);
	}

	venture_series_store_set_max_bytes(fixture->store, 0);
	snapshot = begin(fixture->store, "realm", T0 + 2 * HOUR, TRUE);
	add_listing(snapshot, "new", 0, 5, 2, -1);
	result = commit(fixture->store, snapshot);
	g_assert_cmpint(result.instruments_new, ==, 1);
}

/* A listing that would overflow its instrument's total is refused alone. */
static void
test_listing_overflow(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;
	VentureSeriesListing listing;

	snapshot = begin(fixture->store, "realm", T0, TRUE);
	add_listing(snapshot, "herb", 0, 10, G_MAXINT64 - 1, -1);

	memset(&listing, 0, sizeof(listing));
	listing.instrument_key = "herb";
	listing.unit_price = 10;
	listing.quantity = 2;
	listing.expires_in_min = -1;
	g_assert_false(venture_series_snapshot_add_listing(snapshot, &listing, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	listing.unit_price = -1;
	listing.quantity = 1;
	g_assert_false(venture_series_snapshot_add_listing(snapshot, &listing, &error));
	g_clear_error(&error);

	commit(fixture->store, snapshot);
	row = current_of(fixture->store, "realm", "herb");
	g_assert_cmpint(row->quantity, ==, G_MAXINT64 - 1);
}

/* --- Quotes and entries ---------------------------------------------------------- */

/*
 * The latest quote per venue, instrument and side wins by time, an older
 * one does not overwrite it, the same one twice is written once, and an
 * event's outcomes are found through their parent.
 */
static void
test_quotes(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GPtrArray) quotes = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesQuote batch[4];
	VentureSeriesCommitResult result;
	VentureSeriesInstrument outcome;
	const VentureSeriesQuoteRow *row;

	memset(&outcome, 0, sizeof(outcome));
	outcome.key = "match-1:home";
	outcome.parent_key = "match-1";
	outcome.kind = "outcome";
	g_assert_true(venture_series_store_upsert_instrument(fixture->store, &outcome,
	                                                     T0, NULL, NULL, &error));
	outcome.key = "match-1:away";
	g_assert_true(venture_series_store_upsert_instrument(fixture->store, &outcome,
	                                                     T0, NULL, NULL, &error));

	memset(batch, 0, sizeof(batch));
	batch[0].venue_key = "book-a";
	batch[0].instrument_key = "match-1:home";
	batch[0].side = VENTURE_SERIES_QUOTE_BACK;
	batch[0].value = 2100000;
	batch[0].liquidity = VENTURE_SERIES_NONE;
	batch[0].taken_at = T0 + 60;
	batch[1] = batch[0];
	batch[1].value = 1900000;
	batch[1].taken_at = T0;
	batch[2] = batch[0];
	batch[3] = batch[0];
	batch[3].instrument_key = "match-1:away";
	batch[3].value = 1950000;

	g_assert_true(venture_series_store_add_quotes(fixture->store, batch, 4,
	                                              &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result.listings, ==, 4);

	quotes = venture_series_store_list_quotes(fixture->store, NULL, "match-1",
	                                          &error);
	g_assert_cmpuint(quotes->len, ==, 2);
	row = (const VentureSeriesQuoteRow *)quotes->pdata[1];
	g_assert_cmpstr(row->instrument_key, ==, "match-1:home");
	g_assert_cmpint(row->value, ==, 2100000);
	g_assert_cmpint(row->taken_at, ==, T0 + 60);
	g_assert_cmpstr(row->currency, ==, "");
	g_assert_cmpstr(row->parent_key, ==, "match-1");

	/* Odds of 1 or less pay nothing back, and are refused with the batch. */
	batch[0].value = VENTURE_SERIES_ODDS_SCALE;
	g_assert_false(venture_series_store_add_quotes(fixture->store, batch, 1,
	                                               NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* A price quote carries its currency. */
	batch[0].value = 420;
	batch[0].currency = "usd";
	batch[0].side = VENTURE_SERIES_QUOTE_ASK;
	batch[0].instrument_key = "sku-1";
	g_assert_true(venture_series_store_add_quotes(fixture->store, batch, 1, NULL,
	                                              &error));
	g_clear_pointer(&quotes, g_ptr_array_unref);
	quotes = venture_series_store_list_quotes(fixture->store, "sku-1", NULL,
	                                          &error);
	g_assert_cmpstr(((const VentureSeriesQuoteRow *)quotes->pdata[0])->currency,
	                ==, "USD");
}

static void
test_entries(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesEntry entry;
	gint64 fresh;

	memset(&entry, 0, sizeof(entry));
	entry.key = "guid-1";
	entry.title = "Patch notes";
	entry.url = "https://example.test/patch";
	entry.published_at = T0;
	entry.instrument_key = "herb";

	g_assert_true(venture_series_store_add_entries(fixture->store, &entry, 1,
	                                               T0 + 10, &fresh, &error));
	g_assert_no_error(error);
	g_assert_cmpint(fresh, ==, 1);

	entry.title = "Patch notes (revised)";
	g_assert_true(venture_series_store_add_entries(fixture->store, &entry, 1,
	                                               T0 + 20, &fresh, &error));
	g_assert_cmpint(fresh, ==, 0);

	entries = venture_series_store_list_entries(fixture->store, 0, 0, &error);
	g_assert_cmpuint(entries->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesEntryRow *)entries->pdata[0])->title, ==,
	                "Patch notes (revised)");
	g_assert_cmpstr(((VentureSeriesEntryRow *)entries->pdata[0])->instrument_key,
	                ==, "herb");

	/* New is when it first arrived, however old it was published: the
	 * revised copy fetched at T0 + 20 is not new since T0 + 15. */
	g_clear_pointer(&entries, g_ptr_array_unref);
	entries = venture_series_store_list_new_entries(fixture->store, T0 + 10, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(entries->len, ==, 1);
	g_clear_pointer(&entries, g_ptr_array_unref);
	entries = venture_series_store_list_new_entries(fixture->store, T0 + 15, 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(entries->len, ==, 0);
	g_clear_pointer(&entries, g_ptr_array_unref);

	/* Only a web address is a link. */
	entry.key = "guid-2";
	entry.url = "javascript:alert(1)";
	g_assert_false(venture_series_store_add_entries(fixture->store, &entry, 1,
	                                                T0, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- Transactions and concurrency --------------------------------------------- */

/*
 * An operator's batch: a snapshot inside it that fails takes back only
 * its own writes, and nothing is visible to a reader until the batch
 * commits.
 */
static void
test_batch_and_reader_isolation(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snapshot;

	put_price(fixture->store, "realm", "herb", T0, 100, 1);

	reader = venture_series_store_open_reader(fixture->dir, &error);
	g_assert_no_error(error);

	g_assert_true(venture_series_store_begin(fixture->store, &error));
	put_price(fixture->store, "realm", "herb", T0 + HOUR, 200, 1);

	/* A snapshot from a different handle is refused inside the batch. */
	snapshot = begin(fixture->store, "realm", T0 + 2 * HOUR, TRUE);
	g_assert_false(venture_series_store_commit_snapshot(reader, snapshot, NULL,
	                                                    &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	row = current_of(reader, "realm", "herb");
	g_assert_cmpint(row->min_price, ==, 100);
	g_clear_pointer(&row, venture_series_row_free);

	g_assert_true(venture_series_store_commit(fixture->store, &error));

	row = current_of(reader, "realm", "herb");
	g_assert_cmpint(row->min_price, ==, 200);
	g_clear_pointer(&row, venture_series_row_free);

	/* A rolled-back batch leaves nothing behind. */
	g_assert_true(venture_series_store_begin(fixture->store, &error));
	put_price(fixture->store, "realm", "herb", T0 + 3 * HOUR, 300, 1);
	venture_series_store_rollback(fixture->store);

	row = current_of(fixture->store, "realm", "herb");
	g_assert_cmpint(row->min_price, ==, 200);
}

typedef struct
{
	const gchar	*dir;
	guint		 rounds;
	gint		 done;
	gchar		*failure;
} WriterJob;

/*
 * The writer, on its own thread with its own handle, the way the feeds
 * worker will run it: every snapshot sets two instruments to the same
 * price in one transaction.
 */
static gpointer
writer_thread(gpointer data)
{
	WriterJob *job;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	job = (WriterJob *)data;
	store = venture_series_store_open(job->dir, &error);

	for (i = 0; (NULL != store) && (i < job->rounds); i++)
	{
		VentureSeriesSnapshot *snapshot;
		VentureSeriesListing listing;

		snapshot = venture_series_store_begin_snapshot(
			store, "realm", "GOLD", T0 + (gint64)(i + 1) * 60, T0, TRUE, &error);
		if (NULL == snapshot)
			break;

		memset(&listing, 0, sizeof(listing));
		listing.unit_price = (gint64)(i + 1) * 10;
		listing.quantity = 1;
		listing.expires_in_min = -1;

		listing.instrument_key = "left";
		listing.listing_id = i * 2 + 1;
		venture_series_snapshot_add_listing(snapshot, &listing, NULL);
		listing.instrument_key = "right";
		listing.listing_id = i * 2 + 2;
		venture_series_snapshot_add_listing(snapshot, &listing, NULL);

		if (!venture_series_store_commit_snapshot(store, snapshot, NULL, &error))
			break;
	}

	if (NULL != error)
		job->failure = g_strdup(error->message);

	g_atomic_int_set(&job->done, 1);
	return NULL;
}

/*
 * WAL lets a reader on the main thread read while the writer commits on
 * another, and every read sees one whole snapshot: the two instruments a
 * snapshot writes together are never seen at different prices.
 */
static void
test_wal_reader_during_write(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GError) error = NULL;
	WriterJob job;
	GThread *thread;
	guint reads;

	{
		VentureSeriesSnapshot *snapshot;

		snapshot = begin(fixture->store, "realm", T0, TRUE);
		add_listing(snapshot, "left", 0, 0, 1, -1);
		add_listing(snapshot, "right", 0, 0, 1, -1);
		commit(fixture->store, snapshot);
	}
	g_clear_object(&fixture->store);

	reader = venture_series_store_open_reader(fixture->dir, &error);
	g_assert_no_error(error);

	memset(&job, 0, sizeof(job));
	job.dir = fixture->dir;
	job.rounds = 200;

	thread = g_thread_new("series-writer", writer_thread, &job);
	reads = 0;

	while (0 == g_atomic_int_get(&job.done))
	{
		VentureSeriesFilter filter;
		g_autoptr(GPtrArray) rows = NULL;

		venture_series_filter_init(&filter);
		filter.sort = VENTURE_SERIES_SORT_NAME;

		rows = venture_series_store_list_current(reader, &filter, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(rows->len, ==, 2);
		g_assert_cmpint(((VentureSeriesRow *)rows->pdata[0])->min_price, ==,
		                ((VentureSeriesRow *)rows->pdata[1])->min_price);
		reads++;
	}

	g_thread_join(thread);
	g_assert_null(job.failure);
	g_assert_cmpuint(reads, >, 0);

	{
		g_autoptr(VentureSeriesRow) row = current_of(reader, "realm", "left");

		g_assert_cmpint(row->min_price, ==, 2000);
	}
}

/*
 * A modest smoke test of scale: one venue, twenty thousand listings over
 * two thousand instruments, twice (the second diffing the first for
 * sales). The bound is generous on purpose -- a debug build on a slow
 * machine must pass -- and exists to catch a statement that stopped being
 * prepared once or a transaction per row, which is minutes, not seconds.
 */
static void
test_scale_smoke(
	Fixture		*fixture,
	gconstpointer	 data
){
	VentureSeriesCommitResult result;
	gint64 started;
	guint round;

	started = g_get_monotonic_time();

	for (round = 0; round < 2; round++)
	{
		VentureSeriesSnapshot *snapshot;
		guint i;

		snapshot = begin(fixture->store, "realm", T0 + (gint64)round * HOUR, TRUE);

		for (i = 0; i < 20000; i++)
		{
			gchar key[32];

			/* The second round drops each instrument's first listing. */
			if ((1 == round) && (i < 2000))
				continue;

			g_snprintf(key, sizeof(key), "item-%u", i % 2000);
			add_listing(snapshot, key, i + 1, 100 + (gint64)(i % 97), 1 + i % 5,
			            7200);
		}

		result = commit(fixture->store, snapshot);
	}

	g_assert_cmpint(result.instruments, ==, 2000);
	g_assert_cmpint(result.listings, ==, 18000);
	g_assert_cmpint(result.sold_estimate, >, 0);
	g_assert_cmpint(g_get_monotonic_time() - started, <,
	                G_GINT64_CONSTANT(60) * G_USEC_PER_SEC);
}

/* Writes an instrument with only the fields given; NULLs leave stored ones. */
static void
put_instrument(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*name,
	const gchar		*parent_key
){
	g_autoptr(GError) error = NULL;
	VentureSeriesInstrument instrument;

	memset(&instrument, 0, sizeof(instrument));
	instrument.key = key;
	instrument.name = name;
	instrument.parent_key = parent_key;
	instrument.kind = "item";
	g_assert_true(venture_series_store_upsert_instrument(store, &instrument, 1000, NULL, NULL, &error));
	g_assert_no_error(error);
}

static gchar *
stored_name(
	VentureSeriesStore	*store,
	const gchar		*key
){
	g_autoptr(VentureSeriesInstrumentRow) row = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_series_store_get_instrument(store, key, &row, &error));
	g_assert_no_error(error);
	g_assert_nonnull(row);

	return g_strdup(row->name);
}

/*
 * A variant's name follows its plain item's: a variant stored before the
 * item is named takes the name when it arrives, one stored after starts
 * with it, and a variant a provider named itself keeps its own.
 */
static void
test_variant_names(
	Fixture		*fixture,
	gconstpointer	 data
){
	g_autofree gchar *early = NULL;
	g_autofree gchar *late = NULL;
	g_autofree gchar *own = NULL;
	g_autofree gchar *still = NULL;

	(void)data;

	put_instrument(fixture->store, "19019:b1", NULL, "19019");
	put_instrument(fixture->store, "19019:b2", "Thunderfury (heroic)", "19019");
	still = stored_name(fixture->store, "19019:b1");
	g_assert_null(still);

	put_instrument(fixture->store, "19019", "Thunderfury", NULL);
	put_instrument(fixture->store, "19019:b3", NULL, "19019");

	early = stored_name(fixture->store, "19019:b1");
	late = stored_name(fixture->store, "19019:b3");
	own = stored_name(fixture->store, "19019:b2");
	g_assert_cmpstr(early, ==, "Thunderfury");
	g_assert_cmpstr(late, ==, "Thunderfury");
	g_assert_cmpstr(own, ==, "Thunderfury (heroic)");
}

#define ADD(path, func) \
	g_test_add("/series-store/" path, Fixture, NULL, fixture_set_up, func, \
	           fixture_tear_down)
#define ADD_BARE(path, func) \
	g_test_add("/series-store/" path, Fixture, no_store, fixture_set_up, func, \
	           fixture_tear_down)

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	ADD("create", test_create);
	ADD("variant-names", test_variant_names);
	ADD_BARE("upgrade", test_upgrade);
	ADD_BARE("future-version-refused", test_future_version_refused);
	ADD_BARE("corrupt-refused", test_corrupt_refused);
	ADD("reader-is-read-only", test_reader_is_read_only);
	ADD("snapshot-figures", test_snapshot_figures);
	ADD("idempotent", test_idempotent);
	ADD("late-snapshot", test_late_snapshot);
	ADD("absent-out-of-stock", test_absent_out_of_stock);
	ADD("stock-changed-at", test_stock_changed_at);
	ADD("stats-snapshot", test_stats_snapshot);
	ADD("sale-estimate", test_sale_estimate);
	ADD("sale-needs-ids", test_sale_needs_ids);
	ADD("sale-across-incomplete", test_sale_across_incomplete);
	ADD("sale-within-one-currency", test_sale_within_one_currency);
	ADD("sale-currency-upgrade", test_sale_currency_upgrade);
	ADD("saturated-sales-recompute", test_saturated_sales_recompute);
	ADD("precomputed-sales-and-deals", test_precomputed_sales_and_deals);
	ADD("region-small", test_region_small);
	ADD("region-deal-price", test_region_deal_price);
	ADD("list-current", test_list_current);
	ADD("heat", test_heat);
	ADD("reference", test_reference);
	ADD("venue-state", test_venue_state);
	ADD("purge", test_purge);
	ADD("cap", test_cap);
	ADD("listing-overflow", test_listing_overflow);
	ADD("quotes", test_quotes);
	ADD("entries", test_entries);
	ADD("batch-and-reader-isolation", test_batch_and_reader_isolation);
	ADD("wal-reader-during-write", test_wal_reader_during_write);
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
