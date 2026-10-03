/*
 * venture-series-store.c - One data source's market data, in its own file
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The SQL in this file is hand-written on purpose: see the header and
 * docs/market-data.org for why the series store is the one exception to
 * the field table. Every statement that runs per row is prepared once per
 * handle and kept, and every write happens inside one transaction per
 * snapshot or batch.
 *
 * Nothing here may touch VentureDatabase, the entity registry or the
 * configuration: the writer runs on the feeds worker thread. Nothing here
 * may call g_warning() either, which the test harness makes fatal from any
 * thread; a failure is a GError for the caller to record.
 */

#include "venture.h"

#ifndef VENTURE_HAVE_SQLITE
#error "The series store needs SQLite; the Makefile leaves it out of SQLITE=0 builds."
#endif

#include <errno.h>
#include <string.h>
#include <math.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>

/* --- Schema ---------------------------------------------------------------- */

/*
 * Append-only. A step that has shipped is never edited: a store that ran it
 * would never run the edit, and two stores at the same version would hold
 * different schemas. Each step runs in its own transaction with the
 * user_version bump, so a crash between steps leaves a store at a version
 * that exists.
 *
 * Times are Unix seconds and days are days since 1970-01-01 UTC. Prices
 * are integer minor units; a NULL price is "none", never zero. The tables
 * that are written once per instrument per snapshot keep their rows small
 * and their keys integer; the ones whose rows carry a blob (current's
 * tiers, hourly's points) are rowid tables with a unique index, because a
 * WITHOUT ROWID row past a few hundred bytes is the slow case.
 */
static const gchar series_schema_step_1[] =
	"CREATE TABLE meta ("
	"  key TEXT PRIMARY KEY,"
	"  value TEXT"
	") WITHOUT ROWID;"
	"INSERT INTO meta (key, value) VALUES ('created_at', strftime('%s', 'now'));"
	"CREATE TABLE venues ("
	"  id INTEGER PRIMARY KEY,"
	"  key TEXT NOT NULL UNIQUE,"
	"  namespace TEXT,"
	"  name TEXT,"
	"  kind TEXT,"
	"  group_key TEXT NOT NULL DEFAULT '',"
	"  currency TEXT,"
	"  attrs TEXT,"
	"  first_seen INTEGER NOT NULL,"
	"  last_seen INTEGER NOT NULL"
	");"
	"CREATE INDEX venues_group ON venues (group_key);"
	"CREATE TABLE instruments ("
	"  id INTEGER PRIMARY KEY,"
	"  key TEXT NOT NULL UNIQUE,"
	"  namespace TEXT,"
	"  name TEXT,"
	"  name_fold TEXT,"
	"  kind TEXT,"
	"  category TEXT,"
	"  parent_key TEXT,"
	"  attrs TEXT,"
	"  first_seen INTEGER NOT NULL,"
	"  last_seen INTEGER NOT NULL"
	");"
	"CREATE INDEX instruments_category ON instruments (category);"
	"CREATE INDEX instruments_parent ON instruments (parent_key)"
	"  WHERE parent_key IS NOT NULL;"
	"CREATE TABLE snapshots ("
	"  venue_id INTEGER NOT NULL,"
	"  taken_at INTEGER NOT NULL,"
	"  fetched_at INTEGER NOT NULL,"
	"  currency TEXT NOT NULL,"
	"  listings INTEGER NOT NULL,"
	"  instruments INTEGER NOT NULL,"
	"  complete INTEGER NOT NULL,"
	"  late INTEGER NOT NULL,"
	"  PRIMARY KEY (venue_id, taken_at)"
	") WITHOUT ROWID;"
	"CREATE INDEX snapshots_taken ON snapshots (taken_at);"
	"CREATE TABLE current ("
	"  id INTEGER PRIMARY KEY,"
	"  venue_id INTEGER NOT NULL,"
	"  instrument_id INTEGER NOT NULL,"
	"  currency TEXT NOT NULL,"
	"  taken_at INTEGER NOT NULL,"
	"  seen_at INTEGER NOT NULL,"
	"  quantity INTEGER,"
	"  listings INTEGER NOT NULL DEFAULT 0,"
	"  min_price INTEGER,"
	"  market_value INTEGER,"
	"  median INTEGER,"
	"  p15 INTEGER,"
	"  mean INTEGER,"
	"  stddev INTEGER,"
	"  bid_price INTEGER,"
	"  bid_quantity INTEGER NOT NULL DEFAULT 0,"
	"  tiers BLOB,"
	"  region_median INTEGER,"
	"  deal_price INTEGER,"
	"  pct_vs_region REAL,"
	"  UNIQUE (venue_id, instrument_id)"
	");"
	"CREATE INDEX current_instrument ON current (instrument_id, min_price);"
	"CREATE INDEX current_venue_min ON current (venue_id, min_price);"
	"CREATE INDEX current_venue_market ON current (venue_id, market_value);"
	"CREATE INDEX current_venue_pct ON current (venue_id, pct_vs_region);"
	"CREATE INDEX current_pct ON current (pct_vs_region)"
	"  WHERE pct_vs_region IS NOT NULL;"
	"CREATE INDEX current_deal ON current (deal_price)"
	"  WHERE deal_price IS NOT NULL;"
	"CREATE TABLE hourly ("
	"  id INTEGER PRIMARY KEY,"
	"  venue_id INTEGER NOT NULL,"
	"  instrument_id INTEGER NOT NULL,"
	"  day INTEGER NOT NULL,"
	"  currency TEXT NOT NULL,"
	"  points BLOB NOT NULL,"
	"  UNIQUE (venue_id, instrument_id, day)"
	");"
	"CREATE INDEX hourly_day ON hourly (day);"
	"CREATE TABLE daily ("
	"  venue_id INTEGER NOT NULL,"
	"  instrument_id INTEGER NOT NULL,"
	"  day INTEGER NOT NULL,"
	"  currency TEXT NOT NULL,"
	"  snapshots INTEGER NOT NULL DEFAULT 0,"
	"  min_price INTEGER,"
	"  max_quantity INTEGER NOT NULL DEFAULT 0,"
	"  price_at_max INTEGER,"
	"  market_value INTEGER,"
	"  mean INTEGER,"
	"  listings INTEGER NOT NULL DEFAULT 0,"
	"  sold_estimate INTEGER NOT NULL DEFAULT 0,"
	"  sold_value INTEGER NOT NULL DEFAULT 0,"
	"  expired_estimate INTEGER NOT NULL DEFAULT 0,"
	"  sale_avg INTEGER,"
	"  PRIMARY KEY (venue_id, instrument_id, day, currency)"
	") WITHOUT ROWID;"
	"CREATE INDEX daily_day ON daily (day);"
	"CREATE TABLE region ("
	"  group_key TEXT NOT NULL,"
	"  instrument_id INTEGER NOT NULL,"
	"  currency TEXT NOT NULL,"
	"  computed_at INTEGER NOT NULL,"
	"  venues_offering INTEGER NOT NULL,"
	"  total_quantity INTEGER NOT NULL,"
	"  median_min INTEGER,"
	"  p33 INTEGER,"
	"  deal_price INTEGER,"
	"  market_avg INTEGER,"
	"  PRIMARY KEY (group_key, instrument_id, currency)"
	") WITHOUT ROWID;"
	"CREATE INDEX region_instrument ON region (instrument_id);"
	"CREATE TABLE venue_state ("
	"  venue_id INTEGER PRIMARY KEY,"
	"  last_taken_at INTEGER,"
	"  last_fetched_at INTEGER,"
	"  gaps BLOB,"
	"  interval_seconds INTEGER,"
	"  listing_set BLOB,"
	"  listing_set_at INTEGER,"
	"  cursor TEXT"
	");";

static const gchar series_schema_step_2[] =
	"CREATE TABLE quotes ("
	"  id INTEGER PRIMARY KEY,"
	"  venue_id INTEGER NOT NULL,"
	"  instrument_id INTEGER NOT NULL,"
	"  side INTEGER NOT NULL,"
	"  taken_at INTEGER NOT NULL,"
	"  value INTEGER NOT NULL,"
	"  currency TEXT,"
	"  liquidity INTEGER,"
	"  UNIQUE (venue_id, instrument_id, side)"
	");"
	"CREATE INDEX quotes_instrument ON quotes (instrument_id);"
	"CREATE TABLE quote_history ("
	"  venue_id INTEGER NOT NULL,"
	"  instrument_id INTEGER NOT NULL,"
	"  side INTEGER NOT NULL,"
	"  taken_at INTEGER NOT NULL,"
	"  value INTEGER NOT NULL,"
	"  currency TEXT,"
	"  liquidity INTEGER,"
	"  PRIMARY KEY (venue_id, instrument_id, side, taken_at)"
	") WITHOUT ROWID;"
	"CREATE INDEX quote_history_taken ON quote_history (taken_at);"
	"CREATE TABLE entries ("
	"  id INTEGER PRIMARY KEY,"
	"  key TEXT NOT NULL UNIQUE,"
	"  venue_id INTEGER,"
	"  instrument_id INTEGER,"
	"  published_at INTEGER,"
	"  fetched_at INTEGER NOT NULL,"
	"  title TEXT NOT NULL,"
	"  url TEXT,"
	"  summary TEXT,"
	"  attrs TEXT"
	");"
	"CREATE INDEX entries_when ON entries (COALESCE(published_at, fetched_at));";

static const gchar *const series_schema_steps[] = {
	series_schema_step_1,
	series_schema_step_2
};

#define SERIES_SCHEMA_VERSION (G_N_ELEMENTS(series_schema_steps))

/* Blob format versions: the first byte of every packed blob. */
#define SERIES_BLOB_POINTS (1)
#define SERIES_BLOB_TIERS (1)
#define SERIES_BLOB_GAPS (1)
#define SERIES_BLOB_LISTINGS (1)

/* How long a writer or reader waits on the other's lock, in milliseconds. */
#define SERIES_BUSY_TIMEOUT_MS (10000)

/* The most venue keys one filter may name. */
#define SERIES_MAX_FILTER_VENUES (512)

/* --- The handle -------------------------------------------------------------- */

struct _VentureSeriesStore
{
	GObject		 parent_instance;

	sqlite3		*db;
	gchar		*directory;
	gchar		*path;
	gboolean	 read_only;

	/* Prepared statements, keyed by the address of their static SQL. */
	GHashTable	*statements;

	guint64		 max_bytes;
	guint		 depth;
};

G_DEFINE_FINAL_TYPE(VentureSeriesStore, venture_series_store, G_TYPE_OBJECT)

/*
 * A cached statement goes back to the cache reset: a SELECT left
 * mid-step holds its read transaction open, and on a reader handle that
 * would pin the snapshot it sees -- the page would show the same prices
 * forever while the writer moved on.
 */
typedef sqlite3_stmt SeriesCachedStmt;

static void
series_cached_stmt_release(SeriesCachedStmt *stmt)
{
	if (NULL != stmt)
	{
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SeriesCachedStmt, series_cached_stmt_release)

/* A statement built for one call (a filtered listing) and then finalised. */
typedef sqlite3_stmt SeriesOwnedStmt;

static void
series_owned_stmt_free(SeriesOwnedStmt *stmt)
{
	if (NULL != stmt)
		sqlite3_finalize(stmt);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SeriesOwnedStmt, series_owned_stmt_free)

static void
venture_series_store_finalize(GObject *object)
{
	VentureSeriesStore *self;

	self = VENTURE_SERIES_STORE(object);

	if ((NULL != self->db) && (self->depth > 0))
		sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);

	g_clear_pointer(&self->statements, g_hash_table_unref);

	if (NULL != self->db)
		sqlite3_close_v2(self->db);

	g_free(self->directory);
	g_free(self->path);

	G_OBJECT_CLASS(venture_series_store_parent_class)->finalize(object);
}

static void
venture_series_store_class_init(VentureSeriesStoreClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_series_store_finalize;
}

static void
series_stmt_finalize(gpointer stmt)
{
	sqlite3_finalize((sqlite3_stmt *)stmt);
}

static void
venture_series_store_init(VentureSeriesStore *self)
{
	self->statements = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                         NULL, series_stmt_finalize);
}

/* --- Errors and statements --------------------------------------------------- */

static void
series_set_sqlite_error(
	VentureSeriesStore	 *self,
	gint			  rc,
	const gchar		 *what,
	GError			**error
){
	gint primary;

	primary = rc & 0xff;

	if ((SQLITE_NOTADB == primary) || (SQLITE_CORRUPT == primary))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "%s is not a usable series store (%s) while %s",
		            self->path, sqlite3_errstr(rc), what);
		return;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
	            "Series store %s: %s while %s", self->path,
	            (NULL != self->db) ? sqlite3_errmsg(self->db) : sqlite3_errstr(rc),
	            what);
}

/*
 * The cached statement for @sql, reset and unbound. @sql must be a static
 * string: its address is the key.
 */
static SeriesCachedStmt *
series_stmt(
	VentureSeriesStore	 *self,
	const gchar		 *sql,
	GError			**error
){
	sqlite3_stmt *stmt;
	gint rc;

	stmt = (sqlite3_stmt *)g_hash_table_lookup(self->statements, sql);

	if (NULL != stmt)
		return stmt;

	rc = sqlite3_prepare_v3(self->db, sql, -1, SQLITE_PREPARE_PERSISTENT,
	                        &stmt, NULL);

	if (SQLITE_OK != rc)
	{
		series_set_sqlite_error(self, rc, "preparing a statement", error);
		sqlite3_finalize(stmt);
		return NULL;
	}

	g_hash_table_insert(self->statements, (gpointer)sql, stmt);
	return stmt;
}

static gboolean
series_exec(
	VentureSeriesStore	 *self,
	const gchar		 *sql,
	const gchar		 *what,
	GError			**error
){
	gint rc;

	rc = sqlite3_exec(self->db, sql, NULL, NULL, NULL);

	if (SQLITE_OK != rc)
	{
		series_set_sqlite_error(self, rc, what, error);
		return FALSE;
	}

	return TRUE;
}

/* Steps a statement expected to finish without rows. */
static gboolean
series_step_done(
	VentureSeriesStore	 *self,
	sqlite3_stmt		 *stmt,
	const gchar		 *what,
	GError			**error
){
	gint rc;

	rc = sqlite3_step(stmt);

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, what, error);
		return FALSE;
	}

	return TRUE;
}

static void
series_bind_text(
	sqlite3_stmt	*stmt,
	gint		 index,
	const gchar	*text
){
	if (NULL == text)
		sqlite3_bind_null(stmt, index);
	else
		sqlite3_bind_text(stmt, index, text, -1, SQLITE_TRANSIENT);
}

/* Binds a figure, or NULL for VENTURE_SERIES_NONE. */
static void
series_bind_figure(
	sqlite3_stmt	*stmt,
	gint		 index,
	gint64		 value
){
	if (VENTURE_SERIES_NONE == value)
		sqlite3_bind_null(stmt, index);
	else
		sqlite3_bind_int64(stmt, index, value);
}

static void
series_bind_blob(
	sqlite3_stmt	*stmt,
	gint		 index,
	GByteArray	*blob
){
	if ((NULL == blob) || (0 == blob->len))
		sqlite3_bind_null(stmt, index);
	else
		sqlite3_bind_blob(stmt, index, blob->data, (gint)blob->len,
		                  SQLITE_TRANSIENT);
}

static gint64
series_column_figure(
	sqlite3_stmt	*stmt,
	gint		 column
){
	if (SQLITE_NULL == sqlite3_column_type(stmt, column))
		return VENTURE_SERIES_NONE;

	return sqlite3_column_int64(stmt, column);
}

static gchar *
series_column_strdup(
	sqlite3_stmt	*stmt,
	gint		 column
){
	const guchar *text;

	text = sqlite3_column_text(stmt, column);

	return (NULL != text) ? g_strdup((const gchar *)text) : NULL;
}

static void
series_column_currency(
	sqlite3_stmt	*stmt,
	gint		 column,
	gchar		 out[VENTURE_MONEY_CURRENCY_LEN]
){
	const guchar *text;

	text = sqlite3_column_text(stmt, column);
	g_strlcpy(out, (NULL != text) ? (const gchar *)text : "",
	          VENTURE_MONEY_CURRENCY_LEN);
}

static gboolean
series_require_writer(
	VentureSeriesStore	 *self,
	GError			**error
){
	if (self->read_only)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		            "Series store %s was opened read-only", self->path);
		return FALSE;
	}

	return TRUE;
}

/* --- Transactions -------------------------------------------------------------- */

/*
 * The outermost level is a real transaction taken for writing up front
 * (IMMEDIATE, so a reader's lock is never upgraded mid-way into a busy
 * error); inner levels are savepoints, so a snapshot that fails inside an
 * operator's batch takes back only its own writes.
 */
static gboolean
series_txn_begin(
	VentureSeriesStore	 *self,
	GError			**error
){
	g_autofree gchar *sql = NULL;

	if (!series_require_writer(self, error))
		return FALSE;

	if (0 == self->depth)
	{
		if (!series_exec(self, "BEGIN IMMEDIATE", "beginning a transaction",
		                 error))
			return FALSE;
	}
	else
	{
		sql = g_strdup_printf("SAVEPOINT series_%u", self->depth);

		if (!series_exec(self, sql, "opening a savepoint", error))
			return FALSE;
	}

	self->depth++;
	return TRUE;
}

static gboolean
series_txn_commit(
	VentureSeriesStore	 *self,
	GError			**error
){
	g_autofree gchar *sql = NULL;

	g_return_val_if_fail(self->depth > 0, FALSE);

	self->depth--;

	if (0 == self->depth)
	{
		if (!series_exec(self, "COMMIT", "committing", error))
		{
			sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		return TRUE;
	}

	sql = g_strdup_printf("RELEASE series_%u", self->depth);
	return series_exec(self, sql, "releasing a savepoint", error);
}

static void
series_txn_rollback_one(VentureSeriesStore *self)
{
	g_autofree gchar *sql = NULL;

	if (0 == self->depth)
		return;

	self->depth--;

	if (0 == self->depth)
	{
		sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
		return;
	}

	sql = g_strdup_printf("ROLLBACK TO series_%u; RELEASE series_%u",
	                      self->depth, self->depth);
	sqlite3_exec(self->db, sql, NULL, NULL, NULL);
}

gboolean
venture_series_store_begin(
	VentureSeriesStore	 *self,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);

	return series_txn_begin(self, error);
}

gboolean
venture_series_store_commit(
	VentureSeriesStore	 *self,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);

	if (0 == self->depth)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "No series store transaction is open");
		return FALSE;
	}

	return series_txn_commit(self, error);
}

void
venture_series_store_rollback(VentureSeriesStore *self)
{
	g_return_if_fail(VENTURE_IS_SERIES_STORE(self));

	if (self->depth > 0)
	{
		sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
		self->depth = 0;
	}
}

/* --- Packed blobs ---------------------------------------------------------------- */

/*
 * Unsigned LEB128: seven bits a byte, low first. A quantity of 20 is one
 * byte and a price of 300 000 000 is five, which is most of why an hour of
 * one instrument costs a dozen bytes rather than forty.
 */
static void
series_put_varint(
	GByteArray	*out,
	guint64		 value
){
	guint8 byte;

	do
	{
		byte = (guint8)(value & 0x7f);
		value >>= 7;

		if (0 != value)
			byte |= 0x80;

		g_byte_array_append(out, &byte, 1);
	}
	while (0 != value);
}

/* A figure that may be NONE, as 0 for none and value + 1 otherwise. */
static void
series_put_figure(
	GByteArray	*out,
	gint64		 value
){
	if ((VENTURE_SERIES_NONE == value) || (value < 0))
		series_put_varint(out, 0);
	else
		series_put_varint(out, (guint64)value + 1);
}

typedef struct
{
	const guint8	*data;
	gsize		 length;
	gsize		 at;
	gboolean	 broken;
} SeriesReader;

static guint64
series_get_varint(SeriesReader *reader)
{
	guint64 value;
	guint shift;

	value = 0;
	shift = 0;

	while (!reader->broken)
	{
		guint8 byte;

		if ((reader->at >= reader->length) || (shift > 63))
		{
			reader->broken = TRUE;
			return 0;
		}

		byte = reader->data[reader->at++];
		value |= ((guint64)(byte & 0x7f)) << shift;

		if (0 == (byte & 0x80))
			return value;

		shift += 7;
	}

	return 0;
}

/* A value read back must fit a signed figure, or the blob is corrupt. */
static gint64
series_get_int(SeriesReader *reader)
{
	guint64 value;

	value = series_get_varint(reader);

	if (value > (guint64)G_MAXINT64)
	{
		reader->broken = TRUE;
		return 0;
	}

	return (gint64)value;
}

static gint64
series_get_figure(SeriesReader *reader)
{
	guint64 value;

	value = series_get_varint(reader);

	if (0 == value)
		return VENTURE_SERIES_NONE;

	if (value - 1 > (guint64)G_MAXINT64)
	{
		reader->broken = TRUE;
		return 0;
	}

	return (gint64)(value - 1);
}

static void
series_reader_init(
	SeriesReader	*reader,
	sqlite3_stmt	*stmt,
	gint		 column
){
	reader->data = (const guint8 *)sqlite3_column_blob(stmt, column);
	reader->length = (gsize)sqlite3_column_bytes(stmt, column);
	reader->at = 0;
	reader->broken = FALSE;
}

static gboolean
series_reader_version(
	SeriesReader	*reader,
	guint8		 version
){
	if ((reader->length < 1) || (version != reader->data[0]))
	{
		reader->broken = TRUE;
		return FALSE;
	}

	reader->at = 1;
	return TRUE;
}

static void
series_set_corrupt(
	VentureSeriesStore	 *self,
	const gchar		 *what,
	GError			**error
){
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
	            "Series store %s holds a corrupt %s", self->path, what);
}

/* One hour of the hourly blob. */
typedef struct
{
	gboolean	present;
	gint64		offset;
	gint64		min_price;
	gint64		quantity;
	gint64		market_value;
	gint64		listings;
} SeriesHour;

static gboolean
series_decode_points(
	SeriesReader	*reader,
	SeriesHour	 hours[24]
){
	memset(hours, 0, sizeof(SeriesHour) * 24);

	if (0 == reader->length)
		return TRUE;

	if (!series_reader_version(reader, SERIES_BLOB_POINTS))
		return FALSE;

	while (reader->at < reader->length)
	{
		guint64 hour;

		hour = series_get_varint(reader);

		if (reader->broken || (hour > 23))
			return FALSE;

		hours[hour].present = TRUE;
		hours[hour].offset = series_get_int(reader);
		hours[hour].min_price = series_get_figure(reader);
		hours[hour].quantity = series_get_figure(reader);
		hours[hour].market_value = series_get_figure(reader);
		hours[hour].listings = series_get_int(reader);

		if (reader->broken)
			return FALSE;
	}

	return TRUE;
}

static void
series_encode_points(
	const SeriesHour	 hours[24],
	GByteArray		*out
){
	guint8 version;
	guint hour;

	version = SERIES_BLOB_POINTS;
	g_byte_array_append(out, &version, 1);

	for (hour = 0; hour < 24; hour++)
	{
		if (!hours[hour].present)
			continue;

		series_put_varint(out, hour);
		series_put_varint(out, (guint64)hours[hour].offset);
		series_put_figure(out, hours[hour].min_price);
		series_put_figure(out, hours[hour].quantity);
		series_put_figure(out, hours[hour].market_value);
		series_put_varint(out, (guint64)MAX(hours[hour].listings, (gint64)0));
	}
}

static void
series_encode_tiers(
	const VentureSeriesTier	*tiers,
	gsize			 n_tiers,
	GByteArray		*out
){
	guint8 version;
	gint64 previous;
	gsize i;

	if (0 == n_tiers)
		return;

	version = SERIES_BLOB_TIERS;
	g_byte_array_append(out, &version, 1);
	series_put_varint(out, n_tiers);

	/* Prices ascend, so each is stored as the rise from the one before. */
	previous = 0;

	for (i = 0; i < n_tiers; i++)
	{
		series_put_varint(out, (guint64)(tiers[i].price - previous));
		series_put_varint(out, (guint64)tiers[i].quantity);
		previous = tiers[i].price;
	}
}

static GArray *
series_decode_tiers(SeriesReader *reader)
{
	g_autoptr(GArray) tiers = NULL;
	guint64 count;
	gint64 price;
	guint64 i;

	tiers = g_array_new(FALSE, TRUE, sizeof(VentureSeriesTier));

	if (0 == reader->length)
		return g_steal_pointer(&tiers);

	if (!series_reader_version(reader, SERIES_BLOB_TIERS))
		return NULL;

	count = series_get_varint(reader);

	if (reader->broken || (count > VENTURE_SERIES_MAX_TIERS))
		return NULL;

	price = 0;

	for (i = 0; i < count; i++)
	{
		VentureSeriesTier tier;
		gint64 rise;

		rise = series_get_int(reader);
		tier.quantity = series_get_int(reader);

		if (reader->broken || !venture_series_math_add(price, rise, &price))
			return NULL;

		tier.price = price;
		g_array_append_val(tiers, tier);
	}

	return g_steal_pointer(&tiers);
}

static GArray *
series_decode_gaps(SeriesReader *reader)
{
	g_autoptr(GArray) gaps = NULL;
	guint64 count;
	guint64 i;

	gaps = g_array_new(FALSE, TRUE, sizeof(gint64));

	if (0 == reader->length)
		return g_steal_pointer(&gaps);

	if (!series_reader_version(reader, SERIES_BLOB_GAPS))
		return NULL;

	count = series_get_varint(reader);

	if (reader->broken || (count > VENTURE_SERIES_GAP_HISTORY))
		return NULL;

	for (i = 0; i < count; i++)
	{
		gint64 gap;

		gap = series_get_int(reader);

		if (reader->broken)
			return NULL;

		g_array_append_val(gaps, gap);
	}

	return g_steal_pointer(&gaps);
}

static void
series_encode_gaps(
	GArray		*gaps,
	GByteArray	*out
){
	guint8 version;
	guint i;

	version = SERIES_BLOB_GAPS;
	g_byte_array_append(out, &version, 1);
	series_put_varint(out, gaps->len);

	for (i = 0; i < gaps->len; i++)
		series_put_varint(out, (guint64)g_array_index(gaps, gint64, i));
}

/*
 * The listing set: every listing with an id from a venue's last complete
 * snapshot, sorted by id, ids stored as rises. At a hundred thousand
 * listings it is a megabyte or two per venue, rewritten once per
 * snapshot, which is the price of a sale estimate.
 */
static void
series_encode_listings(
	GArray		*marks,
	GByteArray	*out
){
	guint8 version;
	guint64 previous;
	guint i;

	version = SERIES_BLOB_LISTINGS;
	g_byte_array_append(out, &version, 1);
	series_put_varint(out, marks->len);

	previous = 0;

	for (i = 0; i < marks->len; i++)
	{
		const VentureSeriesListingMark *mark;

		mark = &g_array_index(marks, VentureSeriesListingMark, i);
		series_put_varint(out, mark->id - previous);
		series_put_varint(out, (guint64)mark->instrument);
		series_put_varint(out, (guint64)mark->price);
		series_put_varint(out, (guint64)mark->quantity);
		series_put_varint(out, (guint64)(mark->expires_in_min + 1));
		previous = mark->id;
	}
}

static GArray *
series_decode_listings(SeriesReader *reader)
{
	g_autoptr(GArray) marks = NULL;
	guint64 count;
	guint64 previous;
	guint64 i;

	marks = g_array_new(FALSE, TRUE, sizeof(VentureSeriesListingMark));

	if (0 == reader->length)
		return g_steal_pointer(&marks);

	if (!series_reader_version(reader, SERIES_BLOB_LISTINGS))
		return NULL;

	count = series_get_varint(reader);

	/* Each listing takes at least five bytes; more is a lie. */
	if (reader->broken || (count > reader->length))
		return NULL;

	g_array_set_size(marks, (guint)count);
	previous = 0;

	for (i = 0; i < count; i++)
	{
		VentureSeriesListingMark *mark;
		guint64 rise;

		mark = &g_array_index(marks, VentureSeriesListingMark, i);
		rise = series_get_varint(reader);
		mark->id = previous + rise;
		mark->instrument = series_get_int(reader);
		mark->price = series_get_int(reader);
		mark->quantity = series_get_int(reader);
		mark->expires_in_min = series_get_int(reader) - 1;

		if (reader->broken || ((i > 0) && (0 == rise)))
			return NULL;

		previous = mark->id;
	}

	return g_steal_pointer(&marks);
}

/* --- Validation -------------------------------------------------------------------- */

static gboolean
series_check_key(
	const gchar	 *key,
	const gchar	 *what,
	GError		**error
){
	gsize length;

	if ((NULL == key) || ('\0' == key[0]))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A %s key is required", what);
		return FALSE;
	}

	length = strlen(key);

	if ((length > VENTURE_SERIES_MAX_KEY_LENGTH) ||
	    !g_utf8_validate(key, (gssize)length, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A %s key must be UTF-8 of at most %d bytes", what,
		            VENTURE_SERIES_MAX_KEY_LENGTH);
		return FALSE;
	}

	return TRUE;
}

/* Optional text: NULL is fine, anything else must be bounded UTF-8. */
static gboolean
series_check_text(
	const gchar	 *text,
	gsize		  limit,
	const gchar	 *what,
	GError		**error
){
	gsize length;

	if (NULL == text)
		return TRUE;

	length = strlen(text);

	if ((length > limit) || !g_utf8_validate(text, (gssize)length, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The %s must be UTF-8 of at most %" G_GSIZE_FORMAT " bytes",
		            what, limit);
		return FALSE;
	}

	return TRUE;
}

static gboolean
series_check_attrs(
	const gchar	 *attrs_json,
	GError		**error
){
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) parse_error = NULL;
	JsonNode *root;

	if (NULL == attrs_json)
		return TRUE;

	if (!series_check_text(attrs_json, 65536, "attributes", error))
		return FALSE;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, attrs_json, -1, &parse_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The attributes are not JSON: %s", parse_error->message);
		return FALSE;
	}

	root = json_parser_get_root(parser);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The attributes must be a JSON object");
		return FALSE;
	}

	return TRUE;
}

/*
 * Copies a currency into @out upper-cased, refusing anything outside
 * VENTURE's grammar. Pure string work: the currency registry is not
 * consulted, so this is safe on the worker thread.
 */
static gboolean
series_normalise_currency(
	const gchar	 *currency,
	gchar		  out[VENTURE_MONEY_CURRENCY_LEN],
	GError		**error
){
	gsize i;

	if (!venture_currency_is_valid(currency))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a currency code (a letter, then 1 to 14 "
		            "letters, digits or underscores)",
		            (NULL != currency) ? currency : "");
		return FALSE;
	}

	for (i = 0; ('\0' != currency[i]) && (i < VENTURE_MONEY_CURRENCY_LEN - 1); i++)
		out[i] = g_ascii_toupper(currency[i]);
	out[i] = '\0';

	return TRUE;
}

static gint64
series_day_of(gint64 at)
{
	gint64 day;

	day = at / 86400;

	if ((at % 86400 != 0) && (at < 0))
		day--;

	return day;
}

/* --- Opening ------------------------------------------------------------------------ */

static gboolean
series_read_int(
	VentureSeriesStore	 *self,
	const gchar		 *sql,
	gint64			 *out,
	GError			**error
){
	sqlite3_stmt *stmt;
	gint rc;

	stmt = NULL;
	rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);

	if (SQLITE_OK == rc)
	{
		rc = sqlite3_step(stmt);

		if (SQLITE_ROW == rc)
		{
			*out = sqlite3_column_int64(stmt, 0);
			rc = SQLITE_OK;
		}
	}

	if (SQLITE_OK != rc)
	{
		series_set_sqlite_error(self, rc, "reading the store's header", error);
		sqlite3_finalize(stmt);
		return FALSE;
	}

	sqlite3_finalize(stmt);
	return TRUE;
}

static gboolean
series_migrate(
	VentureSeriesStore	 *self,
	gint64			  version,
	GError			**error
){
	guint step;

	if (0 == version)
	{
		/*
		 * Incremental vacuum lets a purge hand space back to the file
		 * system; it can only be chosen before the first table exists.
		 */
		if (!series_exec(self, "PRAGMA auto_vacuum = INCREMENTAL",
		                 "choosing the vacuum mode", error))
			return FALSE;
	}

	for (step = (guint)version + 1; step <= SERIES_SCHEMA_VERSION; step++)
	{
		g_autofree gchar *bump = NULL;

		if (!series_exec(self, "BEGIN IMMEDIATE", "beginning an upgrade", error))
			return FALSE;

		bump = g_strdup_printf("PRAGMA user_version = %u", step);

		if (!series_exec(self, series_schema_steps[step - 1],
		                 "upgrading the schema", error) ||
		    !series_exec(self, bump, "recording the schema version", error))
		{
			sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}

		if (!series_exec(self, "COMMIT", "committing an upgrade", error))
		{
			sqlite3_exec(self->db, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}

		g_debug("Series store %s upgraded to schema %u", self->path, step);
	}

	return TRUE;
}

static VentureSeriesStore *
series_open(
	const gchar	 *directory,
	gboolean	  read_only,
	GError		**error
){
	g_autoptr(VentureSeriesStore) self = NULL;
	gint64 version;
	gint flags;
	gint rc;

	if ((NULL == directory) || ('\0' == directory[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A series store needs a directory");
		return NULL;
	}

	self = (VentureSeriesStore *)g_object_new(VENTURE_TYPE_SERIES_STORE, NULL);
	self->directory = g_strdup(directory);
	self->path = g_build_filename(directory, VENTURE_SERIES_STORE_FILENAME, NULL);
	self->read_only = read_only;

	if (read_only)
	{
		if (!g_file_test(self->path, G_FILE_TEST_IS_REGULAR))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "There is no series store at %s", self->path);
			return NULL;
		}
	}
	else if (0 != g_mkdir_with_parents(directory, 0700))
	{
		gint saved;

		saved = errno;
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "Could not create the series store directory %s: %s",
		            directory, g_strerror(saved));
		return NULL;
	}

	/*
	 * One connection per handle and one thread per connection, so
	 * SQLite's own mutexes buy nothing. A reader opens read-write without
	 * CREATE and is then made query-only: a WAL database opened strictly
	 * read-only cannot create its shared-memory index, and the first
	 * reader after the writer closed would fail for that alone.
	 */
	flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX |
	        SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE;

	if (!read_only)
		flags |= SQLITE_OPEN_CREATE;

	rc = sqlite3_open_v2(self->path, &self->db, flags, NULL);

	if (SQLITE_OK != rc)
	{
		series_set_sqlite_error(self, rc, "opening", error);
		return NULL;
	}

	sqlite3_busy_timeout(self->db, SERIES_BUSY_TIMEOUT_MS);

	/* The first read of the header is what finds a file that is not one. */
	if (!series_read_int(self, "PRAGMA user_version", &version, error))
		return NULL;

	if ((version < 0) || (version > (gint64)SERIES_SCHEMA_VERSION))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_MIGRATION,
		            "Series store %s is at schema version %" G_GINT64_FORMAT
		            ", newer than this build's %u; upgrade VENTURE before "
		            "opening it", self->path, version,
		            (guint)SERIES_SCHEMA_VERSION);
		return NULL;
	}

	if (read_only)
	{
		if (version != (gint64)SERIES_SCHEMA_VERSION)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_MIGRATION,
			            "Series store %s is at schema version %" G_GINT64_FORMAT
			            "; its writer has not upgraded it to %u yet",
			            self->path, version, (guint)SERIES_SCHEMA_VERSION);
			return NULL;
		}

		if (!series_exec(self, "PRAGMA query_only = ON",
		                 "making the handle read-only", error))
			return NULL;

		return g_steal_pointer(&self);
	}

	/*
	 * A file at version 0 that already has tables is some other SQLite
	 * database: pointed at by mistake, it must be refused before anything
	 * -- a schema, or even the journal mode in its header -- is written
	 * into it.
	 */
	if (0 == version)
	{
		gint64 tables;

		if (!series_read_int(self, "SELECT count(*) FROM sqlite_schema",
		                     &tables, error))
			return NULL;

		if (tables > 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
			            "%s is an SQLite database but not a series store",
			            self->path);
			return NULL;
		}
	}

	/*
	 * WAL so readers never wait on the writer; NORMAL synchronisation
	 * because everything here can be fetched or derived again, and a
	 * power cut costing the last snapshot is the right trade for not
	 * syncing a hundred times an hour.
	 */
	if (!series_exec(self, "PRAGMA journal_mode = WAL", "choosing WAL", error) ||
	    !series_exec(self, "PRAGMA synchronous = NORMAL",
	                 "choosing the sync level", error) ||
	    !series_exec(self, "PRAGMA cache_size = -16384",
	                 "sizing the cache", error) ||
	    !series_exec(self, "PRAGMA temp_store = MEMORY",
	                 "choosing the temp store", error))
		return NULL;

	if (!series_migrate(self, version, error))
		return NULL;

	return g_steal_pointer(&self);
}

VentureSeriesStore *
venture_series_store_open(
	const gchar	 *directory,
	GError		**error
){
	return series_open(directory, FALSE, error);
}

VentureSeriesStore *
venture_series_store_open_reader(
	const gchar	 *directory,
	GError		**error
){
	return series_open(directory, TRUE, error);
}

gboolean
venture_series_store_is_reader(VentureSeriesStore *self)
{
	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);

	return self->read_only;
}

const gchar *
venture_series_store_get_path(VentureSeriesStore *self)
{
	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	return self->path;
}

guint
venture_series_store_schema_version(void)
{
	return (guint)SERIES_SCHEMA_VERSION;
}

const gchar *
venture_series_store_schema_step(guint version)
{
	if ((version < 1) || (version > SERIES_SCHEMA_VERSION))
		return NULL;

	return series_schema_steps[version - 1];
}

void
venture_series_store_set_max_bytes(
	VentureSeriesStore	*self,
	guint64			 max_bytes
){
	g_return_if_fail(VENTURE_IS_SERIES_STORE(self));

	self->max_bytes = max_bytes;
}

gboolean
venture_series_store_get_size(
	VentureSeriesStore	 *self,
	guint64			 *out_bytes,
	GError			**error
){
	gint64 pages;
	gint64 free_pages;
	gint64 page_size;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out_bytes, FALSE);

	if (!series_read_int(self, "PRAGMA page_count", &pages, error) ||
	    !series_read_int(self, "PRAGMA freelist_count", &free_pages, error) ||
	    !series_read_int(self, "PRAGMA page_size", &page_size, error))
		return FALSE;

	*out_bytes = (guint64)MAX(pages - free_pages, (gint64)0) * (guint64)page_size;
	return TRUE;
}

/* Whether the cap now refuses new instruments. */
static gboolean
series_over_cap(
	VentureSeriesStore	 *self,
	gboolean		 *out_over,
	GError			**error
){
	guint64 bytes;

	*out_over = FALSE;

	if (0 == self->max_bytes)
		return TRUE;

	if (!venture_series_store_get_size(self, &bytes, error))
		return FALSE;

	*out_over = (bytes >= self->max_bytes);
	return TRUE;
}

/* --- Venues and instruments ------------------------------------------------------- */

static const gchar series_sql_venue_id[] =
	"SELECT id, currency FROM venues WHERE key = ?1";

/*
 * Finds a venue's id, or 0 when there is none. @out_currency, when
 * given, receives its stored currency ("" without one).
 */
static gboolean
series_venue_lookup(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			 *out_id,
	gchar			  out_currency[VENTURE_MONEY_CURRENCY_LEN],
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint rc;

	*out_id = 0;
	if (NULL != out_currency)
		out_currency[0] = '\0';

	stmt = series_stmt(self, series_sql_venue_id, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, key);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW == rc)
	{
		*out_id = sqlite3_column_int64(stmt, 0);
		if (NULL != out_currency)
			series_column_currency(stmt, 1, out_currency);
		return TRUE;
	}

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, "finding a venue", error);
		return FALSE;
	}

	return TRUE;
}

static const gchar series_sql_upsert_venue[] =
	"INSERT INTO venues (key, namespace, name, kind, group_key, currency, attrs,"
	"                    first_seen, last_seen)"
	" VALUES (?1, ?2, ?3, ?4, COALESCE(?5, ''), ?6, ?7, ?8, ?8)"
	" ON CONFLICT (key) DO UPDATE SET"
	"  namespace = COALESCE(?2, namespace),"
	"  name = COALESCE(?3, name),"
	"  kind = COALESCE(?4, kind),"
	"  group_key = COALESCE(?5, group_key),"
	"  currency = COALESCE(?6, currency),"
	"  attrs = COALESCE(?7, attrs),"
	"  first_seen = min(first_seen, ?8),"
	"  last_seen = max(last_seen, ?8)";

static gboolean
series_write_venue(
	VentureSeriesStore		 *self,
	const VentureSeriesVenue	 *venue,
	gint64				  seen_at,
	GError				**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];

	if (!series_check_key(venue->key, "venue", error) ||
	    !series_check_text(venue->namespace_, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "venue namespace", error) ||
	    !series_check_text(venue->name, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "venue name", error) ||
	    !series_check_text(venue->kind, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "venue kind", error) ||
	    !series_check_text(venue->group_key, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "venue group", error) ||
	    !series_check_attrs(venue->attrs_json, error))
		return FALSE;

	if ((NULL != venue->currency) &&
	    !series_normalise_currency(venue->currency, currency, error))
		return FALSE;

	stmt = series_stmt(self, series_sql_upsert_venue, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, venue->key);
	series_bind_text(stmt, 2, venue->namespace_);
	series_bind_text(stmt, 3, venue->name);
	series_bind_text(stmt, 4, venue->kind);
	series_bind_text(stmt, 5, venue->group_key);
	series_bind_text(stmt, 6, (NULL != venue->currency) ? currency : NULL);
	series_bind_text(stmt, 7, venue->attrs_json);
	sqlite3_bind_int64(stmt, 8, seen_at);

	return series_step_done(self, stmt, "writing a venue", error);
}

/* A venue's id, creating a bare one when it is new. */
static gboolean
series_venue_ensure(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			  seen_at,
	gint64			 *out_id,
	GError			**error
){
	VentureSeriesVenue venue;

	memset(&venue, 0, sizeof(venue));
	venue.key = key;

	if (!series_write_venue(self, &venue, seen_at, error))
		return FALSE;

	if (!series_venue_lookup(self, key, out_id, NULL, error))
		return FALSE;

	if (0 == *out_id)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE,
		            "Series store %s lost the venue it just wrote", self->path);
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_series_store_upsert_venue(
	VentureSeriesStore		 *self,
	const VentureSeriesVenue	 *venue,
	gint64				  seen_at,
	GError				**error
){
	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != venue, FALSE);

	if (!series_txn_begin(self, error))
		return FALSE;

	if (!series_write_venue(self, venue, seen_at, error))
	{
		series_txn_rollback_one(self);
		return FALSE;
	}

	return series_txn_commit(self, error);
}

static const gchar series_sql_instrument_id[] =
	"SELECT id, last_seen FROM instruments WHERE key = ?1";

static gboolean
series_instrument_lookup(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			 *out_id,
	gint64			 *out_last_seen,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint rc;

	*out_id = 0;

	stmt = series_stmt(self, series_sql_instrument_id, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, key);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW == rc)
	{
		*out_id = sqlite3_column_int64(stmt, 0);
		if (NULL != out_last_seen)
			*out_last_seen = sqlite3_column_int64(stmt, 1);
		return TRUE;
	}

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, "finding an instrument", error);
		return FALSE;
	}

	return TRUE;
}

static const gchar series_sql_insert_instrument[] =
	"INSERT INTO instruments (key, namespace, name, name_fold, kind, category,"
	"                         parent_key, attrs, first_seen, last_seen)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?9)";

static const gchar series_sql_update_instrument[] =
	"UPDATE instruments SET"
	"  namespace = COALESCE(?2, namespace),"
	"  name = COALESCE(?3, name),"
	"  name_fold = COALESCE(?4, name_fold),"
	"  kind = COALESCE(?5, kind),"
	"  category = COALESCE(?6, category),"
	"  parent_key = COALESCE(?7, parent_key),"
	"  attrs = COALESCE(?8, attrs),"
	"  first_seen = min(first_seen, ?9),"
	"  last_seen = max(last_seen, ?9)"
	" WHERE id = ?1";

static const gchar series_sql_touch_instrument[] =
	"UPDATE instruments SET last_seen = ?2 WHERE id = ?1 AND last_seen < ?2";

/*
 * Creates or updates an instrument. @allow_new FALSE refuses a new one:
 * *out_id stays 0 and nothing is written.
 */
static gboolean
series_write_instrument(
	VentureSeriesStore		 *self,
	const VentureSeriesInstrument	 *instrument,
	gint64				  seen_at,
	gboolean			  allow_new,
	gint64				 *out_id,
	gboolean			 *out_created,
	GError				**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	g_autofree gchar *fold = NULL;
	gint64 id;
	gint64 last_seen;

	*out_id = 0;
	*out_created = FALSE;

	if (!series_check_key(instrument->key, "instrument", error) ||
	    !series_check_text(instrument->namespace_, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "instrument namespace", error) ||
	    !series_check_text(instrument->name, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "instrument name", error) ||
	    !series_check_text(instrument->kind, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "instrument kind", error) ||
	    !series_check_text(instrument->category, 2048,
	                       "instrument category", error) ||
	    !series_check_text(instrument->parent_key, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "parent key", error) ||
	    !series_check_attrs(instrument->attrs_json, error))
		return FALSE;

	if (NULL != instrument->name)
		fold = g_utf8_casefold(instrument->name, -1);

	if (!series_instrument_lookup(self, instrument->key, &id, &last_seen, error))
		return FALSE;

	if (0 == id)
	{
		if (!allow_new)
			return TRUE;

		stmt = series_stmt(self, series_sql_insert_instrument, error);
		if (NULL == stmt)
			return FALSE;

		series_bind_text(stmt, 1, instrument->key);
		series_bind_text(stmt, 2, instrument->namespace_);
		series_bind_text(stmt, 3, instrument->name);
		series_bind_text(stmt, 4, fold);
		series_bind_text(stmt, 5, instrument->kind);
		series_bind_text(stmt, 6, instrument->category);
		series_bind_text(stmt, 7, instrument->parent_key);
		series_bind_text(stmt, 8, instrument->attrs_json);
		sqlite3_bind_int64(stmt, 9, seen_at);

		if (!series_step_done(self, stmt, "creating an instrument", error))
			return FALSE;

		*out_id = sqlite3_last_insert_rowid(self->db);
		*out_created = TRUE;
		return TRUE;
	}

	*out_id = id;

	/*
	 * A bare key from a listing only moves last_seen, and only forward:
	 * ten thousand instruments a snapshot must not be ten thousand
	 * rewrites of identical rows.
	 */
	if ((NULL == instrument->namespace_) && (NULL == instrument->name) &&
	    (NULL == instrument->kind) && (NULL == instrument->category) &&
	    (NULL == instrument->parent_key) && (NULL == instrument->attrs_json))
	{
		if (last_seen >= seen_at)
			return TRUE;

		stmt = series_stmt(self, series_sql_touch_instrument, error);
		if (NULL == stmt)
			return FALSE;

		sqlite3_bind_int64(stmt, 1, id);
		sqlite3_bind_int64(stmt, 2, seen_at);
		return series_step_done(self, stmt, "touching an instrument", error);
	}

	stmt = series_stmt(self, series_sql_update_instrument, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, id);
	series_bind_text(stmt, 2, instrument->namespace_);
	series_bind_text(stmt, 3, instrument->name);
	series_bind_text(stmt, 4, fold);
	series_bind_text(stmt, 5, instrument->kind);
	series_bind_text(stmt, 6, instrument->category);
	series_bind_text(stmt, 7, instrument->parent_key);
	series_bind_text(stmt, 8, instrument->attrs_json);
	sqlite3_bind_int64(stmt, 9, seen_at);

	return series_step_done(self, stmt, "updating an instrument", error);
}

gboolean
venture_series_store_upsert_instrument(
	VentureSeriesStore		 *self,
	const VentureSeriesInstrument	 *instrument,
	gint64				  seen_at,
	gboolean			 *out_created,
	gboolean			 *out_refused,
	GError				**error
){
	gboolean over;
	gboolean created;
	gint64 id;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != instrument, FALSE);

	if (NULL != out_created)
		*out_created = FALSE;
	if (NULL != out_refused)
		*out_refused = FALSE;

	if (!series_txn_begin(self, error))
		return FALSE;

	if (!series_over_cap(self, &over, error) ||
	    !series_write_instrument(self, instrument, seen_at, !over, &id, &created,
	                             error))
	{
		series_txn_rollback_one(self);
		return FALSE;
	}

	if (!series_txn_commit(self, error))
		return FALSE;

	if (NULL != out_created)
		*out_created = created;
	if (NULL != out_refused)
		*out_refused = (0 == id);

	return TRUE;
}

/* An instrument's id from a bare key, creating it unless refused. */
static gboolean
series_instrument_ensure(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			  seen_at,
	gboolean		  allow_new,
	gint64			 *out_id,
	gboolean		 *out_created,
	GError			**error
){
	VentureSeriesInstrument instrument;

	memset(&instrument, 0, sizeof(instrument));
	instrument.key = key;

	return series_write_instrument(self, &instrument, seen_at, allow_new,
	                               out_id, out_created, error);
}

/* --- Meta and cursors -------------------------------------------------------------- */

static const gchar series_sql_set_meta[] =
	"INSERT INTO meta (key, value) VALUES (?1, ?2)"
	" ON CONFLICT (key) DO UPDATE SET value = excluded.value";
static const gchar series_sql_delete_meta[] =
	"DELETE FROM meta WHERE key = ?1";
static const gchar series_sql_get_meta[] =
	"SELECT value FROM meta WHERE key = ?1";

gboolean
venture_series_store_set_meta(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	const gchar		 *value,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);

	if (!series_require_writer(self, error) ||
	    !series_check_key(key, "meta", error) ||
	    !series_check_text(value, 65536, "meta value", error))
		return FALSE;

	stmt = series_stmt(self, (NULL != value) ? series_sql_set_meta :
	                                           series_sql_delete_meta, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, key);
	if (NULL != value)
		series_bind_text(stmt, 2, value);

	return series_step_done(self, stmt, "writing meta", error);
}

gchar *
venture_series_store_get_meta(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = series_stmt(self, series_sql_get_meta, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, key);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW == rc)
		return series_column_strdup(stmt, 0);

	if (SQLITE_DONE != rc)
		series_set_sqlite_error(self, rc, "reading meta", error);

	return NULL;
}

static const gchar series_sql_set_cursor[] =
	"INSERT INTO venue_state (venue_id, cursor) VALUES (?1, ?2)"
	" ON CONFLICT (venue_id) DO UPDATE SET cursor = excluded.cursor";
static const gchar series_sql_get_cursor[] =
	"SELECT s.cursor FROM venue_state s JOIN venues v ON v.id = s.venue_id"
	" WHERE v.key = ?1";

gboolean
venture_series_store_set_venue_cursor(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *cursor,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint64 id;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);

	if (!series_require_writer(self, error) ||
	    !series_check_key(venue_key, "venue", error) ||
	    !series_check_text(cursor, 65536, "cursor", error))
		return FALSE;

	if (!series_venue_lookup(self, venue_key, &id, NULL, error))
		return FALSE;

	if (0 == id)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The series store has no venue \"%s\"", venue_key);
		return FALSE;
	}

	stmt = series_stmt(self, series_sql_set_cursor, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, id);
	series_bind_text(stmt, 2, cursor);

	return series_step_done(self, stmt, "writing a cursor", error);
}

gchar *
venture_series_store_get_venue_cursor(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = series_stmt(self, series_sql_get_cursor, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, venue_key);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW == rc)
		return series_column_strdup(stmt, 0);

	if (SQLITE_DONE != rc)
		series_set_sqlite_error(self, rc, "reading a cursor", error);

	return NULL;
}

/* --- Snapshots: assembly ----------------------------------------------------------- */

/*
 * Everything one snapshot says about one instrument. Listings append to
 * the tier arrays as they arrive; the arrays are sorted and merged once,
 * at the commit.
 */
typedef struct
{
	gchar			*key;
	GArray			*sell;
	GArray			*buy;
	gint64			 listings;
	gint64			 bid_listings;
	gint64			 sell_units;
	gint64			 buy_units;
	gboolean		 has_stats;
	VentureSeriesStats	 stats;
	gint64			 id;
} SeriesAccumulator;

static void
series_accumulator_free(SeriesAccumulator *acc)
{
	if (NULL == acc)
		return;

	g_free(acc->key);
	if (NULL != acc->sell)
		g_array_unref(acc->sell);
	if (NULL != acc->buy)
		g_array_unref(acc->buy);
	g_free(acc);
}

struct _VentureSeriesSnapshot
{
	VentureSeriesStore	*store;
	gchar			*venue_key;
	gchar			 currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64			 taken_at;
	gint64			 fetched_at;
	gboolean		 complete;

	GHashTable		*by_key;
	GPtrArray		*accumulators;

	/* Sell listings with ids; instrument is the accumulator's index. */
	GArray			*marks;
	gint64			 listings;
};

void
venture_series_snapshot_free(VentureSeriesSnapshot *snapshot)
{
	if (NULL == snapshot)
		return;

	g_clear_object(&snapshot->store);
	g_free(snapshot->venue_key);
	g_clear_pointer(&snapshot->by_key, g_hash_table_unref);
	g_clear_pointer(&snapshot->accumulators, g_ptr_array_unref);
	g_clear_pointer(&snapshot->marks, g_array_unref);
	g_free(snapshot);
}

guint64
venture_series_listing_id_from_string(const gchar *id)
{
	guint64 hash;
	const guchar *p;

	g_return_val_if_fail(NULL != id, 1);

	/* FNV-1a, 64-bit. */
	hash = G_GUINT64_CONSTANT(14695981039346656037);

	for (p = (const guchar *)id; '\0' != *p; p++)
	{
		hash ^= (guint64)*p;
		hash *= G_GUINT64_CONSTANT(1099511628211);
	}

	return (0 == hash) ? 1 : hash;
}

VentureSeriesSnapshot *
venture_series_store_begin_snapshot(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *currency,
	gint64			  taken_at,
	gint64			  fetched_at,
	gboolean		  complete,
	GError			**error
){
	VentureSeriesSnapshot *snapshot;
	gchar stored[VENTURE_MONEY_CURRENCY_LEN];
	gchar normalised[VENTURE_MONEY_CURRENCY_LEN];
	gint64 id;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (!series_require_writer(self, error) ||
	    !series_check_key(venue_key, "venue", error))
		return NULL;

	if ((taken_at < 0) || (fetched_at < 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A snapshot's times must not be before 1970");
		return NULL;
	}

	/* Without a currency of its own the snapshot is in the venue's. */
	if (NULL == currency)
	{
		if (!series_venue_lookup(self, venue_key, &id, stored, error))
			return NULL;

		if ('\0' == stored[0])
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "The snapshot of \"%s\" names no currency and the "
			            "venue has none", venue_key);
			return NULL;
		}

		currency = stored;
	}

	if (!series_normalise_currency(currency, normalised, error))
		return NULL;

	snapshot = g_new0(VentureSeriesSnapshot, 1);
	snapshot->store = (VentureSeriesStore *)g_object_ref(self);
	snapshot->venue_key = g_strdup(venue_key);
	g_strlcpy(snapshot->currency, normalised, sizeof(snapshot->currency));
	snapshot->taken_at = taken_at;
	snapshot->fetched_at = fetched_at;
	snapshot->complete = complete;
	snapshot->by_key = g_hash_table_new(g_str_hash, g_str_equal);
	snapshot->accumulators =
		g_ptr_array_new_with_free_func((GDestroyNotify)series_accumulator_free);
	snapshot->marks = g_array_new(FALSE, TRUE, sizeof(VentureSeriesListingMark));

	return snapshot;
}

static SeriesAccumulator *
series_accumulator_for(
	VentureSeriesSnapshot	 *snapshot,
	const gchar		 *key,
	guint			 *out_index
){
	SeriesAccumulator *acc;
	gpointer found;

	if (g_hash_table_lookup_extended(snapshot->by_key, key, NULL, &found))
	{
		*out_index = GPOINTER_TO_UINT(found);
		return (SeriesAccumulator *)g_ptr_array_index(snapshot->accumulators,
		                                              *out_index);
	}

	acc = g_new0(SeriesAccumulator, 1);
	acc->key = g_strdup(key);
	g_ptr_array_add(snapshot->accumulators, acc);
	*out_index = snapshot->accumulators->len - 1;
	g_hash_table_insert(snapshot->by_key, acc->key, GUINT_TO_POINTER(*out_index));

	return acc;
}

gboolean
venture_series_snapshot_add_listing(
	VentureSeriesSnapshot		 *snapshot,
	const VentureSeriesListing	 *listing,
	GError				**error
){
	SeriesAccumulator *acc;
	VentureSeriesTier tier;
	gint64 total;
	guint index;

	g_return_val_if_fail(NULL != snapshot, FALSE);
	g_return_val_if_fail(NULL != listing, FALSE);

	if (!series_check_key(listing->instrument_key, "instrument", error))
		return FALSE;

	if (listing->unit_price < 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A listing of \"%s\" has a negative price",
		            listing->instrument_key);
		return FALSE;
	}

	if (listing->quantity < 1)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A listing of \"%s\" has a quantity below one",
		            listing->instrument_key);
		return FALSE;
	}

	if ((VENTURE_SERIES_SIDE_SELL != listing->side) &&
	    (VENTURE_SERIES_SIDE_BUY != listing->side))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A listing of \"%s\" is on no known side",
		            listing->instrument_key);
		return FALSE;
	}

	if (listing->expires_in_min < -1)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A listing of \"%s\" expires in a negative time",
		            listing->instrument_key);
		return FALSE;
	}

	acc = series_accumulator_for(snapshot, listing->instrument_key, &index);

	if (acc->has_stats)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" has both figures and listings in one snapshot",
		            listing->instrument_key);
		return FALSE;
	}

	/*
	 * The running total is checked before anything is appended, so a
	 * refused listing leaves the snapshot exactly as it was.
	 */
	if (!venture_series_math_add((VENTURE_SERIES_SIDE_SELL == listing->side) ?
	                             acc->sell_units : acc->buy_units,
	                             listing->quantity, &total))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The quantity of \"%s\" no longer fits in a 64-bit integer",
		            listing->instrument_key);
		return FALSE;
	}

	tier.price = listing->unit_price;
	tier.quantity = listing->quantity;

	if (VENTURE_SERIES_SIDE_SELL == listing->side)
	{
		if (NULL == acc->sell)
			acc->sell = g_array_new(FALSE, FALSE, sizeof(VentureSeriesTier));
		g_array_append_val(acc->sell, tier);
		acc->sell_units = total;
		acc->listings++;

		if (0 != listing->listing_id)
		{
			VentureSeriesListingMark mark;

			mark.id = listing->listing_id;
			mark.instrument = (gint64)index;
			mark.price = listing->unit_price;
			mark.quantity = listing->quantity;
			mark.expires_in_min = listing->expires_in_min;
			g_array_append_val(snapshot->marks, mark);
		}
	}
	else
	{
		if (NULL == acc->buy)
			acc->buy = g_array_new(FALSE, FALSE, sizeof(VentureSeriesTier));
		g_array_append_val(acc->buy, tier);
		acc->buy_units = total;
		acc->bid_listings++;
	}

	snapshot->listings++;
	return TRUE;
}

gboolean
venture_series_snapshot_add_stats(
	VentureSeriesSnapshot		 *snapshot,
	const VentureSeriesStats	 *stats,
	GError				**error
){
	SeriesAccumulator *acc;
	const gint64 *figures[8];
	gboolean any;
	guint index;
	guint i;

	g_return_val_if_fail(NULL != snapshot, FALSE);
	g_return_val_if_fail(NULL != stats, FALSE);

	if (!series_check_key(stats->instrument_key, "instrument", error))
		return FALSE;

	figures[0] = &stats->min_price;
	figures[1] = &stats->market_value;
	figures[2] = &stats->mean;
	figures[3] = &stats->median;
	figures[4] = &stats->sale_avg;
	figures[5] = &stats->quantity;
	figures[6] = &stats->listings;
	figures[7] = &stats->sold;

	any = FALSE;

	for (i = 0; i < G_N_ELEMENTS(figures); i++)
	{
		if (VENTURE_SERIES_NONE == *figures[i])
			continue;

		if (*figures[i] < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A figure for \"%s\" is negative", stats->instrument_key);
			return FALSE;
		}

		any = TRUE;
	}

	if (!any)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The figures for \"%s\" give no figure at all",
		            stats->instrument_key);
		return FALSE;
	}

	acc = series_accumulator_for(snapshot, stats->instrument_key, &index);

	if ((NULL != acc->sell) || (NULL != acc->buy))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" has both figures and listings in one snapshot",
		            stats->instrument_key);
		return FALSE;
	}

	acc->has_stats = TRUE;
	acc->stats = *stats;
	acc->stats.instrument_key = acc->key;

	return TRUE;
}

/* --- Snapshots: commit -------------------------------------------------------------- */

typedef struct
{
	gint64	last_taken_at;
	gint64	last_fetched_at;
	GArray	*gaps;
	GArray	*listing_set;
	gint64	listing_set_at;
} SeriesVenueState;

static void
series_venue_state_clear(SeriesVenueState *state)
{
	g_clear_pointer(&state->gaps, g_array_unref);
	g_clear_pointer(&state->listing_set, g_array_unref);
}

static const gchar series_sql_load_state[] =
	"SELECT last_taken_at, last_fetched_at, gaps, listing_set, listing_set_at"
	" FROM venue_state WHERE venue_id = ?1";

static gboolean
series_load_state(
	VentureSeriesStore	 *self,
	gint64			  venue_id,
	gboolean		  with_listings,
	SeriesVenueState	 *state,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	SeriesReader reader;
	gint rc;

	memset(state, 0, sizeof(*state));
	state->last_taken_at = VENTURE_SERIES_NONE;
	state->last_fetched_at = VENTURE_SERIES_NONE;
	state->listing_set_at = VENTURE_SERIES_NONE;

	stmt = series_stmt(self, series_sql_load_state, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, venue_id);
	rc = sqlite3_step(stmt);

	if (SQLITE_DONE == rc)
	{
		state->gaps = g_array_new(FALSE, TRUE, sizeof(gint64));
		state->listing_set = g_array_new(FALSE, TRUE,
		                                 sizeof(VentureSeriesListingMark));
		return TRUE;
	}

	if (SQLITE_ROW != rc)
	{
		series_set_sqlite_error(self, rc, "reading a venue's state", error);
		return FALSE;
	}

	state->last_taken_at = series_column_figure(stmt, 0);
	state->last_fetched_at = series_column_figure(stmt, 1);
	state->listing_set_at = series_column_figure(stmt, 4);

	series_reader_init(&reader, stmt, 2);
	state->gaps = series_decode_gaps(&reader);

	if (with_listings)
	{
		series_reader_init(&reader, stmt, 3);
		state->listing_set = series_decode_listings(&reader);
	}
	else
	{
		state->listing_set = g_array_new(FALSE, TRUE,
		                                 sizeof(VentureSeriesListingMark));
	}

	if ((NULL == state->gaps) || (NULL == state->listing_set))
	{
		series_set_corrupt(self, "venue state", error);
		return FALSE;
	}

	return TRUE;
}

static const gchar series_sql_snapshot_exists[] =
	"SELECT 1 FROM snapshots WHERE venue_id = ?1 AND taken_at = ?2";

static const gchar series_sql_insert_snapshot[] =
	"INSERT INTO snapshots (venue_id, taken_at, fetched_at, currency, listings,"
	"                       instruments, complete, late)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)";

/*
 * pct_vs_region is kept fresh against the stored region median at every
 * snapshot, so browsing by it is right between region recomputes; the
 * median itself only moves when the region is recomputed.
 */
static const gchar series_sql_upsert_current[] =
	"INSERT INTO current (venue_id, instrument_id, currency, taken_at, seen_at,"
	"                     quantity, listings, min_price, market_value, median,"
	"                     p15, mean, stddev, bid_price, bid_quantity, tiers)"
	" VALUES (?1, ?2, ?3, ?4, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14,"
	"         ?15)"
	" ON CONFLICT (venue_id, instrument_id) DO UPDATE SET"
	"  currency = excluded.currency,"
	"  taken_at = excluded.taken_at,"
	"  seen_at = excluded.seen_at,"
	"  quantity = excluded.quantity,"
	"  listings = excluded.listings,"
	"  min_price = excluded.min_price,"
	"  market_value = excluded.market_value,"
	"  median = excluded.median,"
	"  p15 = excluded.p15,"
	"  mean = excluded.mean,"
	"  stddev = excluded.stddev,"
	"  bid_price = excluded.bid_price,"
	"  bid_quantity = excluded.bid_quantity,"
	"  tiers = excluded.tiers,"
	"  pct_vs_region = CASE"
	"    WHEN excluded.min_price IS NULL OR region_median IS NULL"
	"         OR region_median = 0 OR excluded.quantity = 0"
	"         OR excluded.currency <> currency THEN NULL"
	"    ELSE excluded.min_price * 100.0 / region_median END";

/*
 * Whatever a complete snapshot left out is out of stock now. Rows already
 * at nothing are left alone, so an instrument that sold out a week ago is
 * not rewritten every hour since.
 */
static const gchar series_sql_mark_absent[] =
	"UPDATE current SET taken_at = ?2, quantity = 0, listings = 0,"
	"  min_price = NULL, market_value = NULL, median = NULL, p15 = NULL,"
	"  mean = NULL, stddev = NULL, bid_price = NULL, bid_quantity = 0,"
	"  tiers = NULL, pct_vs_region = NULL"
	" WHERE venue_id = ?1 AND taken_at < ?2"
	"   AND (quantity IS NULL OR quantity <> 0 OR bid_quantity <> 0"
	"        OR min_price IS NOT NULL)";

static const gchar series_sql_load_hourly[] =
	"SELECT currency, points FROM hourly"
	" WHERE venue_id = ?1 AND instrument_id = ?2 AND day = ?3";

static const gchar series_sql_store_hourly[] =
	"INSERT INTO hourly (venue_id, instrument_id, day, currency, points)"
	" VALUES (?1, ?2, ?3, ?4, ?5)"
	" ON CONFLICT (venue_id, instrument_id, day) DO UPDATE SET"
	"  currency = excluded.currency, points = excluded.points";

/*
 * SQLite turns an integer sum that overflows into a REAL, which for money
 * is the one thing that must never happen: sums here saturate instead.
 */
#define SERIES_SAT_ADD(a, b) \
	"CASE WHEN " b " > 9223372036854775807 - " a \
	" THEN 9223372036854775807 ELSE " a " + " b " END"

/*
 * The day keeps the snapshot with the most units on offer: a strictly
 * larger quantity takes over its price, market value, mean and listings,
 * and a tie keeps the first. The minimum is the day's lowest whatever the
 * quantity. Every right-hand side reads the row as it was.
 */
static const gchar series_sql_upsert_daily[] =
	"INSERT INTO daily (venue_id, instrument_id, day, currency, snapshots,"
	"                   min_price, max_quantity, price_at_max, market_value,"
	"                   mean, listings, sold_estimate, sold_value,"
	"                   expired_estimate, sale_avg)"
	" VALUES (?1, ?2, ?3, ?4, 1, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, 0, ?13)"
	" ON CONFLICT (venue_id, instrument_id, day, currency) DO UPDATE SET"
	"  snapshots = snapshots + 1,"
	"  min_price = CASE WHEN excluded.min_price IS NULL THEN min_price"
	"                   WHEN min_price IS NULL THEN excluded.min_price"
	"                   ELSE min(min_price, excluded.min_price) END,"
	"  price_at_max = CASE WHEN snapshots = 0"
	"                        OR excluded.max_quantity > max_quantity"
	"                   THEN excluded.price_at_max ELSE price_at_max END,"
	"  market_value = CASE WHEN snapshots = 0"
	"                        OR excluded.max_quantity > max_quantity"
	"                   THEN excluded.market_value ELSE market_value END,"
	"  mean = CASE WHEN snapshots = 0 OR excluded.max_quantity > max_quantity"
	"              THEN excluded.mean ELSE mean END,"
	"  listings = CASE WHEN snapshots = 0"
	"                    OR excluded.max_quantity > max_quantity"
	"               THEN excluded.listings ELSE listings END,"
	"  max_quantity = max(max_quantity, excluded.max_quantity),"
	"  sold_estimate = " SERIES_SAT_ADD("sold_estimate", "excluded.sold_estimate") ","
	"  sold_value = " SERIES_SAT_ADD("sold_value", "excluded.sold_value") ","
	"  sale_avg = COALESCE(excluded.sale_avg, sale_avg)";

static const gchar series_sql_daily_sales[] =
	"INSERT INTO daily (venue_id, instrument_id, day, currency, sold_estimate,"
	"                   sold_value, expired_estimate)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
	" ON CONFLICT (venue_id, instrument_id, day, currency) DO UPDATE SET"
	"  sold_estimate = " SERIES_SAT_ADD("sold_estimate", "excluded.sold_estimate") ","
	"  sold_value = " SERIES_SAT_ADD("sold_value", "excluded.sold_value") ","
	"  expired_estimate = " SERIES_SAT_ADD("expired_estimate",
	                                       "excluded.expired_estimate");

static const gchar series_sql_store_state[] =
	"INSERT INTO venue_state (venue_id, last_taken_at, last_fetched_at, gaps,"
	"                         interval_seconds, listing_set, listing_set_at)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
	" ON CONFLICT (venue_id) DO UPDATE SET"
	"  last_taken_at = excluded.last_taken_at,"
	"  last_fetched_at = excluded.last_fetched_at,"
	"  gaps = excluded.gaps,"
	"  interval_seconds = excluded.interval_seconds,"
	"  listing_set = excluded.listing_set,"
	"  listing_set_at = excluded.listing_set_at";

/* What one instrument's part of a snapshot comes to. */
typedef struct
{
	VentureSeriesSummary	 summary;
	gint64			 quantity;
	gint64			 listings;
	gint64			 bid_price;
	gint64			 bid_quantity;
	gint64			 sold;
	gint64			 sold_value;
	gint64			 sale_avg;
	GByteArray		*tiers;
} SeriesFigures;

static gboolean
series_figures_compute(
	SeriesAccumulator	 *acc,
	SeriesFigures		 *out,
	GError			**error
){
	memset(out, 0, sizeof(*out));
	out->bid_price = VENTURE_SERIES_NONE;
	out->sold = 0;
	out->sold_value = 0;
	out->sale_avg = VENTURE_SERIES_NONE;
	out->tiers = g_byte_array_new();

	if (acc->has_stats)
	{
		const VentureSeriesStats *stats;

		stats = &acc->stats;

		if (!venture_series_math_summarise(NULL, 0, &out->summary, error))
			return FALSE;

		out->summary.min = stats->min_price;
		out->summary.market_value = stats->market_value;
		out->summary.mean = stats->mean;
		out->summary.median = stats->median;
		out->quantity = stats->quantity;
		out->listings = (VENTURE_SERIES_NONE != stats->listings) ?
		                stats->listings : 0;
		out->sale_avg = stats->sale_avg;

		if (VENTURE_SERIES_NONE != stats->sold)
		{
			out->sold = stats->sold;

			/* A source's sold count with its average sale is a value. */
			if ((VENTURE_SERIES_NONE != stats->sale_avg) &&
			    !venture_series_math_mul(stats->sold, stats->sale_avg,
			                             &out->sold_value))
				out->sold_value = G_MAXINT64;
		}

		return TRUE;
	}

	if (NULL != acc->sell)
	{
		if (!venture_series_math_normalise_tiers(acc->sell, error) ||
		    !venture_series_math_summarise(
		            (const VentureSeriesTier *)(gpointer)acc->sell->data,
		            acc->sell->len, &out->summary, error))
			return FALSE;

		series_encode_tiers((const VentureSeriesTier *)(gpointer)acc->sell->data,
		                    MIN(acc->sell->len, (guint)VENTURE_SERIES_MAX_TIERS),
		                    out->tiers);
	}
	else if (!venture_series_math_summarise(NULL, 0, &out->summary, error))
		return FALSE;

	out->quantity = acc->sell_units;
	out->listings = acc->listings;

	if (NULL != acc->buy)
	{
		if (!venture_series_math_normalise_tiers(acc->buy, error))
			return FALSE;

		/* The best bid is the highest: the last tier once sorted. */
		out->bid_price = g_array_index(acc->buy, VentureSeriesTier,
		                               acc->buy->len - 1).price;
	}
	out->bid_quantity = acc->buy_units;

	return TRUE;
}

static gboolean
series_write_hour(
	VentureSeriesStore	 *self,
	gint64			  venue_id,
	gint64			  instrument_id,
	const gchar		 *currency,
	gint64			  taken_at,
	const SeriesFigures	 *figures,
	GError			**error
){
	g_autoptr(GByteArray) blob = NULL;
	SeriesHour hours[24];
	SeriesReader reader;
	gint64 day;
	gint64 hour;
	gint64 offset;
	gint rc;

	day = series_day_of(taken_at);
	hour = (taken_at - day * 86400) / 3600;
	offset = (taken_at - day * 86400) % 3600;

	memset(hours, 0, sizeof(hours));

	{
		g_autoptr(SeriesCachedStmt) load = NULL;

		load = series_stmt(self, series_sql_load_hourly, error);
		if (NULL == load)
			return FALSE;

		sqlite3_bind_int64(load, 1, venue_id);
		sqlite3_bind_int64(load, 2, instrument_id);
		sqlite3_bind_int64(load, 3, day);
		rc = sqlite3_step(load);

		if (SQLITE_ROW == rc)
		{
			const guchar *stored;

			/*
			 * A day that changed currency starts again: two currencies'
			 * prices in one series would read as a price movement.
			 */
			stored = sqlite3_column_text(load, 0);

			if (0 == g_strcmp0((const gchar *)stored, currency))
			{
				series_reader_init(&reader, load, 1);

				if (!series_decode_points(&reader, hours))
				{
					series_set_corrupt(self, "hourly series", error);
					return FALSE;
				}
			}
		}
		else if (SQLITE_DONE != rc)
		{
			series_set_sqlite_error(self, rc, "reading an hourly series", error);
			return FALSE;
		}
	}

	/* The last snapshot within the hour is the hour's point. */
	if (hours[hour].present && (hours[hour].offset > offset))
		return TRUE;

	hours[hour].present = TRUE;
	hours[hour].offset = offset;
	hours[hour].min_price = figures->summary.min;
	hours[hour].quantity = figures->quantity;
	hours[hour].market_value = figures->summary.market_value;
	hours[hour].listings = figures->listings;

	blob = g_byte_array_new();
	series_encode_points(hours, blob);

	{
		g_autoptr(SeriesCachedStmt) store = NULL;

		store = series_stmt(self, series_sql_store_hourly, error);
		if (NULL == store)
			return FALSE;

		sqlite3_bind_int64(store, 1, venue_id);
		sqlite3_bind_int64(store, 2, instrument_id);
		sqlite3_bind_int64(store, 3, day);
		series_bind_text(store, 4, currency);
		series_bind_blob(store, 5, blob);

		return series_step_done(self, store, "writing an hourly series", error);
	}
}

static gboolean
series_write_day(
	VentureSeriesStore	 *self,
	gint64			  venue_id,
	gint64			  instrument_id,
	const gchar		 *currency,
	gint64			  taken_at,
	const SeriesFigures	 *figures,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	stmt = series_stmt(self, series_sql_upsert_daily, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, venue_id);
	sqlite3_bind_int64(stmt, 2, instrument_id);
	sqlite3_bind_int64(stmt, 3, series_day_of(taken_at));
	series_bind_text(stmt, 4, currency);
	series_bind_figure(stmt, 5, figures->summary.min);
	sqlite3_bind_int64(stmt, 6, (VENTURE_SERIES_NONE != figures->quantity) ?
	                            figures->quantity : 0);
	series_bind_figure(stmt, 7, figures->summary.min);
	series_bind_figure(stmt, 8, figures->summary.market_value);
	series_bind_figure(stmt, 9, figures->summary.mean);
	sqlite3_bind_int64(stmt, 10, figures->listings);
	sqlite3_bind_int64(stmt, 11, figures->sold);
	sqlite3_bind_int64(stmt, 12, figures->sold_value);
	series_bind_figure(stmt, 13, figures->sale_avg);

	return series_step_done(self, stmt, "writing a daily row", error);
}

static gboolean
series_write_current(
	VentureSeriesStore	 *self,
	gint64			  venue_id,
	gint64			  instrument_id,
	const gchar		 *currency,
	gint64			  taken_at,
	const SeriesFigures	 *figures,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	stmt = series_stmt(self, series_sql_upsert_current, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, venue_id);
	sqlite3_bind_int64(stmt, 2, instrument_id);
	series_bind_text(stmt, 3, currency);
	sqlite3_bind_int64(stmt, 4, taken_at);
	series_bind_figure(stmt, 5, figures->quantity);
	sqlite3_bind_int64(stmt, 6, figures->listings);
	series_bind_figure(stmt, 7, figures->summary.min);
	series_bind_figure(stmt, 8, figures->summary.market_value);
	series_bind_figure(stmt, 9, figures->summary.median);
	series_bind_figure(stmt, 10, figures->summary.p15);
	series_bind_figure(stmt, 11, figures->summary.mean);
	series_bind_figure(stmt, 12, figures->summary.stddev);
	series_bind_figure(stmt, 13, figures->bid_price);
	sqlite3_bind_int64(stmt, 14, figures->bid_quantity);
	series_bind_blob(stmt, 15, figures->tiers);

	return series_step_done(self, stmt, "writing a current row", error);
}

static gint
series_mark_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureSeriesListingMark *left;
	const VentureSeriesListingMark *right;

	left = (const VentureSeriesListingMark *)a;
	right = (const VentureSeriesListingMark *)b;

	if (left->id < right->id)
		return -1;
	if (left->id > right->id)
		return 1;
	return 0;
}

/*
 * The snapshot's listings with ids, as the venue's new listing set:
 * instruments mapped from accumulator indexes to row ids, refused ones
 * dropped, sorted by id, and a repeated id kept once (a source that
 * repeats one is wrong, and the first is as good as any).
 */
static GArray *
series_build_listing_set(VentureSeriesSnapshot *snapshot)
{
	GArray *set;
	guint i;

	set = g_array_sized_new(FALSE, TRUE, sizeof(VentureSeriesListingMark),
	                        snapshot->marks->len);

	for (i = 0; i < snapshot->marks->len; i++)
	{
		VentureSeriesListingMark mark;
		SeriesAccumulator *acc;

		mark = g_array_index(snapshot->marks, VentureSeriesListingMark, i);
		acc = (SeriesAccumulator *)g_ptr_array_index(snapshot->accumulators,
		                                             (guint)mark.instrument);

		if (0 == acc->id)
			continue;

		mark.instrument = acc->id;
		g_array_append_val(set, mark);
	}

	g_array_sort(set, series_mark_compare);

	if (set->len > 1)
	{
		VentureSeriesListingMark *data;
		guint write;

		data = (VentureSeriesListingMark *)(gpointer)set->data;
		write = 0;

		for (i = 1; i < set->len; i++)
		{
			if (data[i].id == data[write].id)
				continue;

			data[++write] = data[i];
		}

		g_array_set_size(set, write + 1);
	}

	return set;
}

static gboolean
series_apply_sales(
	VentureSeriesStore		 *self,
	gint64				  venue_id,
	const gchar			 *currency,
	gint64				  taken_at,
	GArray				 *previous,
	GArray				 *current,
	gint64				  elapsed,
	VentureSeriesCommitResult	 *result,
	GError				**error
){
	g_autoptr(GArray) sales = NULL;
	guint i;

	sales = venture_series_math_sale_estimate(
		(const VentureSeriesListingMark *)(gpointer)previous->data,
		previous->len,
		(const VentureSeriesListingMark *)(gpointer)current->data,
		current->len, elapsed, error);

	if (NULL == sales)
		return FALSE;

	for (i = 0; i < sales->len; i++)
	{
		g_autoptr(SeriesCachedStmt) stmt = NULL;
		const VentureSeriesSale *sale;

		sale = &g_array_index(sales, VentureSeriesSale, i);

		stmt = series_stmt(self, series_sql_daily_sales, error);
		if (NULL == stmt)
			return FALSE;

		sqlite3_bind_int64(stmt, 1, venue_id);
		sqlite3_bind_int64(stmt, 2, sale->instrument);
		sqlite3_bind_int64(stmt, 3, series_day_of(taken_at));
		series_bind_text(stmt, 4, currency);
		sqlite3_bind_int64(stmt, 5, sale->sold_units);
		sqlite3_bind_int64(stmt, 6, sale->sold_value);
		sqlite3_bind_int64(stmt, 7, sale->expired_units);

		if (!series_step_done(self, stmt, "writing estimated sales", error))
			return FALSE;

		result->rows_written++;

		if (!venture_series_math_add(result->sold_estimate, sale->sold_units,
		                             &result->sold_estimate))
			result->sold_estimate = G_MAXINT64;
	}

	return TRUE;
}

static gboolean
series_snapshot_apply(
	VentureSeriesStore		 *self,
	VentureSeriesSnapshot		 *snapshot,
	VentureSeriesCommitResult	 *result,
	GError				**error
){
	SeriesVenueState state;
	g_autoptr(GArray) listing_set = NULL;
	g_autoptr(GByteArray) gaps_blob = NULL;
	g_autoptr(GByteArray) set_blob = NULL;
	gint64 venue_id;
	gint64 interval;
	gboolean over;
	gboolean ok;
	guint i;

	/* A snapshot applied before is found before anything is written. */
	if (!series_venue_lookup(self, snapshot->venue_key, &venue_id, NULL, error))
		return FALSE;

	if (0 != venue_id)
	{
		g_autoptr(SeriesCachedStmt) stmt = NULL;
		gint rc;

		stmt = series_stmt(self, series_sql_snapshot_exists, error);
		if (NULL == stmt)
			return FALSE;

		sqlite3_bind_int64(stmt, 1, venue_id);
		sqlite3_bind_int64(stmt, 2, snapshot->taken_at);
		rc = sqlite3_step(stmt);

		if (SQLITE_ROW == rc)
		{
			result->duplicate = TRUE;
			return TRUE;
		}

		if (SQLITE_DONE != rc)
		{
			series_set_sqlite_error(self, rc, "finding a snapshot", error);
			return FALSE;
		}
	}

	if (!series_venue_ensure(self, snapshot->venue_key, snapshot->taken_at,
	                         &venue_id, error))
		return FALSE;

	if (!series_load_state(self, venue_id, snapshot->complete, &state, error))
	{
		series_venue_state_clear(&state);
		return FALSE;
	}

	ok = FALSE;

	result->late = (VENTURE_SERIES_NONE != state.last_taken_at) &&
	               (snapshot->taken_at < state.last_taken_at);

	if (!series_over_cap(self, &over, error))
		goto out;

	/* Every instrument: resolve, compute, write. */
	for (i = 0; i < snapshot->accumulators->len; i++)
	{
		SeriesAccumulator *acc;
		SeriesFigures figures;
		gboolean created;
		gboolean wrote;

		acc = (SeriesAccumulator *)g_ptr_array_index(snapshot->accumulators, i);

		if (!series_instrument_ensure(self, acc->key, snapshot->taken_at, !over,
		                              &acc->id, &created, error))
			goto out;

		if (0 == acc->id)
		{
			result->instruments_refused++;
			result->listings_refused += acc->listings + acc->bid_listings;
			continue;
		}

		if (created)
			result->instruments_new++;

		result->instruments++;
		result->listings += acc->listings + acc->bid_listings;

		wrote = series_figures_compute(acc, &figures, error);

		if (wrote && !result->late)
		{
			wrote = series_write_current(self, venue_id, acc->id,
			                             snapshot->currency, snapshot->taken_at,
			                             &figures, error);
			result->rows_written++;
		}

		if (wrote)
		{
			wrote = series_write_hour(self, venue_id, acc->id,
			                          snapshot->currency, snapshot->taken_at,
			                          &figures, error) &&
			        series_write_day(self, venue_id, acc->id,
			                         snapshot->currency, snapshot->taken_at,
			                         &figures, error);
			result->rows_written += 2;
		}

		g_byte_array_unref(figures.tiers);

		if (!wrote)
			goto out;
	}

	if (snapshot->complete && !result->late)
	{
		g_autoptr(SeriesCachedStmt) stmt = NULL;

		stmt = series_stmt(self, series_sql_mark_absent, error);
		if (NULL == stmt)
			goto out;

		sqlite3_bind_int64(stmt, 1, venue_id);
		sqlite3_bind_int64(stmt, 2, snapshot->taken_at);

		if (!series_step_done(self, stmt, "marking instruments out of stock",
		                      error))
			goto out;

		result->rows_written += sqlite3_changes(self->db);
	}

	{
		g_autoptr(SeriesCachedStmt) stmt = NULL;

		stmt = series_stmt(self, series_sql_insert_snapshot, error);
		if (NULL == stmt)
			goto out;

		sqlite3_bind_int64(stmt, 1, venue_id);
		sqlite3_bind_int64(stmt, 2, snapshot->taken_at);
		sqlite3_bind_int64(stmt, 3, snapshot->fetched_at);
		series_bind_text(stmt, 4, snapshot->currency);
		sqlite3_bind_int64(stmt, 5, result->listings);
		sqlite3_bind_int64(stmt, 6, result->instruments);
		sqlite3_bind_int(stmt, 7, snapshot->complete ? 1 : 0);
		sqlite3_bind_int(stmt, 8, result->late ? 1 : 0);

		if (!series_step_done(self, stmt, "logging a snapshot", error))
			goto out;

		result->rows_written++;
	}

	/*
	 * A late snapshot changes nothing a newer one decided: not the
	 * listing set (its listings are older than the set's), not the gaps.
	 */
	if (result->late)
	{
		ok = TRUE;
		goto out;
	}

	/*
	 * Sales are estimated between two complete snapshots only -- an
	 * incomplete one's missing listings say nothing -- and only when this
	 * one carries ids at all: a source that stopped sending them would
	 * otherwise read as every listing at the venue selling at once.
	 */
	if (snapshot->complete)
	{
		listing_set = series_build_listing_set(snapshot);

		if ((VENTURE_SERIES_NONE != state.listing_set_at) &&
		    (state.listing_set->len > 0) &&
		    ((listing_set->len > 0) || (0 == snapshot->listings)) &&
		    !series_apply_sales(self, venue_id, snapshot->currency,
		                        snapshot->taken_at, state.listing_set,
		                        listing_set,
		                        snapshot->taken_at - state.listing_set_at,
		                        result, error))
			goto out;
	}

	/* The gap since the last snapshot teaches the interval. */
	if (VENTURE_SERIES_NONE != state.last_taken_at)
	{
		gint64 gap;

		gap = snapshot->taken_at - state.last_taken_at;
		g_array_append_val(state.gaps, gap);

		if (state.gaps->len > VENTURE_SERIES_GAP_HISTORY)
			g_array_remove_range(state.gaps, 0,
			                     state.gaps->len - VENTURE_SERIES_GAP_HISTORY);
	}

	interval = venture_series_math_learn_interval(
		(const gint64 *)(gpointer)state.gaps->data, state.gaps->len);

	gaps_blob = g_byte_array_new();
	series_encode_gaps(state.gaps, gaps_blob);

	{
		g_autoptr(SeriesCachedStmt) stmt = NULL;

		stmt = series_stmt(self, series_sql_store_state, error);
		if (NULL == stmt)
			goto out;

		sqlite3_bind_int64(stmt, 1, venue_id);
		sqlite3_bind_int64(stmt, 2, snapshot->taken_at);
		sqlite3_bind_int64(stmt, 3, snapshot->fetched_at);
		series_bind_blob(stmt, 4, gaps_blob);
		sqlite3_bind_int64(stmt, 5, interval);

		if (NULL != listing_set)
		{
			set_blob = g_byte_array_new();
			series_encode_listings(listing_set, set_blob);
			series_bind_blob(stmt, 6, set_blob);
			sqlite3_bind_int64(stmt, 7, snapshot->taken_at);
		}
		else
		{
			/*
			 * An incomplete snapshot keeps the last complete set, so
			 * the next complete one diffs against it over the longer
			 * gap.
			 */
			set_blob = g_byte_array_new();
			if (state.listing_set->len > 0)
				series_encode_listings(state.listing_set, set_blob);
			series_bind_blob(stmt, 6, set_blob);
			series_bind_figure(stmt, 7, state.listing_set_at);
		}

		if (!series_step_done(self, stmt, "writing a venue's state", error))
			goto out;

		result->rows_written++;
	}

	ok = TRUE;

out:
	series_venue_state_clear(&state);
	return ok;
}

gboolean
venture_series_store_commit_snapshot(
	VentureSeriesStore		 *self,
	VentureSeriesSnapshot		 *snapshot,
	VentureSeriesCommitResult	 *out,
	GError				**error
){
	g_autoptr(VentureSeriesSnapshot) owned = snapshot;
	VentureSeriesCommitResult result;
	gint64 started;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != snapshot, FALSE);

	memset(&result, 0, sizeof(result));

	if (owned->store != self)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A snapshot must be committed on the handle that "
		                    "began it");
		return FALSE;
	}

	started = g_get_monotonic_time();

	if (!series_txn_begin(self, error))
		return FALSE;

	if (!series_snapshot_apply(self, owned, &result, error))
	{
		series_txn_rollback_one(self);
		return FALSE;
	}

	if (!series_txn_commit(self, error))
		return FALSE;

	g_debug("Series store %s: venue %s at %" G_GINT64_FORMAT ": %"
	        G_GINT64_FORMAT " listings, %" G_GINT64_FORMAT " instruments (%"
	        G_GINT64_FORMAT " new, %" G_GINT64_FORMAT " refused)%s%s in %"
	        G_GINT64_FORMAT " ms",
	        self->path, owned->venue_key, owned->taken_at, result.listings,
	        result.instruments, result.instruments_new,
	        result.instruments_refused,
	        result.duplicate ? ", duplicate" : "", result.late ? ", late" : "",
	        (g_get_monotonic_time() - started) / 1000);

	if (NULL != out)
		*out = result;

	return TRUE;
}

/* --- Quotes and entries ----------------------------------------------------------------- */

static const gchar series_sql_upsert_quote[] =
	"INSERT INTO quotes (venue_id, instrument_id, side, taken_at, value,"
	"                    currency, liquidity)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
	" ON CONFLICT (venue_id, instrument_id, side) DO UPDATE SET"
	"  taken_at = excluded.taken_at, value = excluded.value,"
	"  currency = excluded.currency, liquidity = excluded.liquidity"
	" WHERE excluded.taken_at >= quotes.taken_at";

static const gchar series_sql_quote_history[] =
	"INSERT OR IGNORE INTO quote_history (venue_id, instrument_id, side,"
	"                                     taken_at, value, currency, liquidity)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";

static gboolean
series_check_quote(
	const VentureSeriesQuote	 *quote,
	gchar				  currency[VENTURE_MONEY_CURRENCY_LEN],
	GError				**error
){
	currency[0] = '\0';

	if (!series_check_key(quote->venue_key, "venue", error) ||
	    !series_check_key(quote->instrument_key, "instrument", error))
		return FALSE;

	if ((quote->side < VENTURE_SERIES_QUOTE_BACK) ||
	    (quote->side > VENTURE_SERIES_QUOTE_ASK))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A quote for \"%s\" is on no known side",
		            quote->instrument_key);
		return FALSE;
	}

	if (NULL != quote->currency)
	{
		if (!series_normalise_currency(quote->currency, currency, error))
			return FALSE;

		if (quote->value < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A price quoted for \"%s\" is negative",
			            quote->instrument_key);
			return FALSE;
		}
	}
	else if (quote->value <= VENTURE_SERIES_ODDS_SCALE)
	{
		/* Decimal odds of 1 or less pay nothing back: not odds. */
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Odds quoted for \"%s\" must exceed 1",
		            quote->instrument_key);
		return FALSE;
	}

	if ((VENTURE_SERIES_NONE != quote->liquidity) && (quote->liquidity < 0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The liquidity quoted for \"%s\" is negative",
		            quote->instrument_key);
		return FALSE;
	}

	if (quote->taken_at < 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A quote for \"%s\" is from before 1970",
		            quote->instrument_key);
		return FALSE;
	}

	return TRUE;
}

static gboolean
series_quotes_apply(
	VentureSeriesStore		 *self,
	const VentureSeriesQuote	 *quotes,
	gsize				  n_quotes,
	VentureSeriesCommitResult	 *result,
	GError				**error
){
	gboolean over;
	gsize i;

	if (!series_over_cap(self, &over, error))
		return FALSE;

	for (i = 0; i < n_quotes; i++)
	{
		const VentureSeriesQuote *quote;
		gchar currency[VENTURE_MONEY_CURRENCY_LEN];
		gint64 venue_id;
		gint64 instrument_id;
		gboolean created;
		const gchar *sql[2];
		guint s;

		quote = &quotes[i];

		if (!series_check_quote(quote, currency, error) ||
		    !series_venue_ensure(self, quote->venue_key, quote->taken_at,
		                         &venue_id, error) ||
		    !series_instrument_ensure(self, quote->instrument_key,
		                              quote->taken_at, !over, &instrument_id,
		                              &created, error))
			return FALSE;

		if (0 == instrument_id)
		{
			result->instruments_refused++;
			result->listings_refused++;
			continue;
		}

		if (created)
			result->instruments_new++;

		result->listings++;

		sql[0] = series_sql_upsert_quote;
		sql[1] = series_sql_quote_history;

		for (s = 0; s < G_N_ELEMENTS(sql); s++)
		{
			g_autoptr(SeriesCachedStmt) stmt = NULL;

			stmt = series_stmt(self, sql[s], error);
			if (NULL == stmt)
				return FALSE;

			sqlite3_bind_int64(stmt, 1, venue_id);
			sqlite3_bind_int64(stmt, 2, instrument_id);
			sqlite3_bind_int(stmt, 3, (gint)quote->side);
			sqlite3_bind_int64(stmt, 4, quote->taken_at);
			sqlite3_bind_int64(stmt, 5, quote->value);
			series_bind_text(stmt, 6, ('\0' != currency[0]) ? currency : NULL);
			series_bind_figure(stmt, 7, quote->liquidity);

			if (!series_step_done(self, stmt, "writing a quote", error))
				return FALSE;

			result->rows_written += sqlite3_changes(self->db);
		}
	}

	return TRUE;
}

gboolean
venture_series_store_add_quotes(
	VentureSeriesStore		 *self,
	const VentureSeriesQuote	 *quotes,
	gsize				  n_quotes,
	VentureSeriesCommitResult	 *out,
	GError				**error
){
	VentureSeriesCommitResult result;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail((NULL != quotes) || (0 == n_quotes), FALSE);

	memset(&result, 0, sizeof(result));

	if (!series_txn_begin(self, error))
		return FALSE;

	if (!series_quotes_apply(self, quotes, n_quotes, &result, error))
	{
		series_txn_rollback_one(self);
		return FALSE;
	}

	if (!series_txn_commit(self, error))
		return FALSE;

	if (NULL != out)
		*out = result;

	return TRUE;
}

static const gchar series_sql_entry_exists[] =
	"SELECT 1 FROM entries WHERE key = ?1";

static const gchar series_sql_upsert_entry[] =
	"INSERT INTO entries (key, venue_id, instrument_id, published_at,"
	"                     fetched_at, title, url, summary, attrs)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
	" ON CONFLICT (key) DO UPDATE SET"
	"  venue_id = COALESCE(excluded.venue_id, venue_id),"
	"  instrument_id = COALESCE(excluded.instrument_id, instrument_id),"
	"  published_at = COALESCE(excluded.published_at, published_at),"
	"  title = excluded.title,"
	"  url = COALESCE(excluded.url, url),"
	"  summary = COALESCE(excluded.summary, summary),"
	"  attrs = COALESCE(excluded.attrs, attrs)";

static gboolean
series_entries_apply(
	VentureSeriesStore		 *self,
	const VentureSeriesEntry	 *entries,
	gsize				  n_entries,
	gint64				  fetched_at,
	gint64				 *out_new,
	GError				**error
){
	gboolean over;
	gsize i;

	if (!series_over_cap(self, &over, error))
		return FALSE;

	for (i = 0; i < n_entries; i++)
	{
		g_autoptr(SeriesCachedStmt) stmt = NULL;
		const VentureSeriesEntry *entry;
		gint64 venue_id;
		gint64 instrument_id;
		gboolean created;
		gint rc;

		entry = &entries[i];
		venue_id = 0;
		instrument_id = 0;

		if (!series_check_key(entry->key, "entry", error) ||
		    !series_check_text(entry->title, 16384, "entry title", error) ||
		    !series_check_text(entry->summary, 16384, "entry summary", error) ||
		    !series_check_text(entry->url, 4096, "entry link", error) ||
		    !series_check_attrs(entry->attrs_json, error))
			return FALSE;

		if (NULL == entry->title)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "The entry \"%s\" has no title", entry->key);
			return FALSE;
		}

		/* Only a web address is a link; anything else is refused here. */
		if ((NULL != entry->url) &&
		    !g_str_has_prefix(entry->url, "https://") &&
		    !g_str_has_prefix(entry->url, "http://"))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "The entry \"%s\" links to something that is not a "
			            "web address", entry->key);
			return FALSE;
		}

		if ((NULL != entry->venue_key) &&
		    !series_venue_ensure(self, entry->venue_key, fetched_at, &venue_id,
		                         error))
			return FALSE;

		if ((NULL != entry->instrument_key) &&
		    !series_instrument_ensure(self, entry->instrument_key, fetched_at,
		                              !over, &instrument_id, &created, error))
			return FALSE;

		stmt = series_stmt(self, series_sql_entry_exists, error);
		if (NULL == stmt)
			return FALSE;

		series_bind_text(stmt, 1, entry->key);
		rc = sqlite3_step(stmt);

		if ((SQLITE_ROW != rc) && (SQLITE_DONE != rc))
		{
			series_set_sqlite_error(self, rc, "finding an entry", error);
			return FALSE;
		}

		if ((SQLITE_DONE == rc) && (NULL != out_new))
			(*out_new)++;

		series_cached_stmt_release(stmt);
		stmt = series_stmt(self, series_sql_upsert_entry, error);
		if (NULL == stmt)
			return FALSE;

		series_bind_text(stmt, 1, entry->key);
		series_bind_figure(stmt, 2, (0 != venue_id) ? venue_id :
		                                              VENTURE_SERIES_NONE);
		series_bind_figure(stmt, 3, (0 != instrument_id) ? instrument_id :
		                                                   VENTURE_SERIES_NONE);
		series_bind_figure(stmt, 4, entry->published_at);
		sqlite3_bind_int64(stmt, 5, fetched_at);
		series_bind_text(stmt, 6, entry->title);
		series_bind_text(stmt, 7, entry->url);
		series_bind_text(stmt, 8, entry->summary);
		series_bind_text(stmt, 9, entry->attrs_json);

		if (!series_step_done(self, stmt, "writing an entry", error))
			return FALSE;
	}

	return TRUE;
}

gboolean
venture_series_store_add_entries(
	VentureSeriesStore		 *self,
	const VentureSeriesEntry	 *entries,
	gsize				  n_entries,
	gint64				  fetched_at,
	gint64				 *out_new,
	GError				**error
){
	gint64 fresh;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail((NULL != entries) || (0 == n_entries), FALSE);

	fresh = 0;

	if (!series_txn_begin(self, error))
		return FALSE;

	if (!series_entries_apply(self, entries, n_entries, fetched_at, &fresh,
	                          error))
	{
		series_txn_rollback_one(self);
		return FALSE;
	}

	if (!series_txn_commit(self, error))
		return FALSE;

	if (NULL != out_new)
		*out_new = fresh;

	return TRUE;
}

/* --- Region ---------------------------------------------------------------------------- */

static const gchar series_sql_region_rows_all[] =
	"SELECT v.group_key, c.instrument_id, c.currency, c.min_price, c.quantity,"
	"       c.market_value"
	" FROM current c JOIN venues v ON v.id = c.venue_id"
	" ORDER BY v.group_key, c.instrument_id, c.currency";

static const gchar series_sql_region_rows_group[] =
	"SELECT v.group_key, c.instrument_id, c.currency, c.min_price, c.quantity,"
	"       c.market_value"
	" FROM current c JOIN venues v ON v.id = c.venue_id"
	" WHERE v.group_key = ?1"
	" ORDER BY v.group_key, c.instrument_id, c.currency";

static const gchar series_sql_region_clear_all[] =
	"DELETE FROM region";
static const gchar series_sql_region_clear_group[] =
	"DELETE FROM region WHERE group_key = ?1";

static const gchar series_sql_region_insert[] =
	"INSERT INTO region (group_key, instrument_id, currency, computed_at,"
	"                    venues_offering, total_quantity, median_min, p33,"
	"                    deal_price, market_avg)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)";

#define SERIES_PCT_EXPR \
	"CASE WHEN c.min_price IS NULL OR r.median_min IS NULL" \
	"       OR r.median_min = 0 OR c.quantity = 0 THEN NULL" \
	"     ELSE c.min_price * 100.0 / r.median_min END"

static const gchar series_sql_region_apply[] =
	"UPDATE current AS c SET"
	"  region_median = r.median_min,"
	"  deal_price = r.deal_price,"
	"  pct_vs_region = " SERIES_PCT_EXPR
	" FROM venues v, region r"
	" WHERE v.id = c.venue_id AND r.group_key = v.group_key"
	"   AND r.instrument_id = c.instrument_id AND r.currency = c.currency"
	"   AND (?1 IS NULL OR v.group_key = ?1)"
	"   AND (c.region_median IS NOT r.median_min"
	"        OR c.deal_price IS NOT r.deal_price"
	"        OR c.pct_vs_region IS NOT (" SERIES_PCT_EXPR "))";

static const gchar series_sql_region_orphans[] =
	"UPDATE current SET region_median = NULL, deal_price = NULL,"
	"  pct_vs_region = NULL"
	" WHERE (region_median IS NOT NULL OR deal_price IS NOT NULL"
	"        OR pct_vs_region IS NOT NULL)"
	"   AND (?1 IS NULL OR venue_id IN (SELECT id FROM venues"
	"                                   WHERE group_key = ?1))"
	"   AND NOT EXISTS (SELECT 1 FROM region r, venues v"
	"                   WHERE v.id = current.venue_id"
	"                     AND r.group_key = v.group_key"
	"                     AND r.instrument_id = current.instrument_id"
	"                     AND r.currency = current.currency)";

/* One group, instrument and currency, gathered from the sorted stream. */
typedef struct
{
	gchar	*group_key;
	gint64	 instrument_id;
	gchar	 currency[VENTURE_MONEY_CURRENCY_LEN];
	GArray	*mins;
	GArray	*markets;
	gint64	 total_quantity;
} SeriesRegionBatch;

static gboolean
series_region_flush(
	VentureSeriesStore		 *self,
	SeriesRegionBatch		 *batch,
	gint64				  now,
	gint64				  deal_min_value,
	const gchar			 *deal_min_currency,
	VentureSeriesRegionResult	 *result,
	GError				**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	g_autoptr(GError) mean_error = NULL;
	gint64 median;
	gint64 deal;
	gint64 market_avg;
	gint64 *mins;

	if ((NULL == batch->group_key) || (0 == batch->mins->len))
		return TRUE;

	mins = (gint64 *)(gpointer)batch->mins->data;
	median = venture_series_math_median(mins, batch->mins->len);
	deal = venture_series_math_deal_price(mins, batch->mins->len);

	/*
	 * Undermine leaves cheap items out of its deals: a 40% discount on
	 * something worth a copper is not a deal anyone acts on. The bound is
	 * in one currency and says nothing about another.
	 */
	if ((VENTURE_SERIES_NONE != deal_min_value) &&
	    (0 == g_strcmp0(deal_min_currency, batch->currency)) &&
	    (median < deal_min_value))
		deal = VENTURE_SERIES_NONE;

	market_avg = venture_series_math_mean(
		(const gint64 *)(gpointer)batch->markets->data, batch->markets->len,
		&mean_error);

	stmt = series_stmt(self, series_sql_region_insert, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, batch->group_key);
	sqlite3_bind_int64(stmt, 2, batch->instrument_id);
	series_bind_text(stmt, 3, batch->currency);
	sqlite3_bind_int64(stmt, 4, now);
	sqlite3_bind_int64(stmt, 5, (gint64)batch->mins->len);
	sqlite3_bind_int64(stmt, 6, batch->total_quantity);
	series_bind_figure(stmt, 7, median);
	series_bind_figure(stmt, 8, venture_series_math_p33(mins, batch->mins->len));
	series_bind_figure(stmt, 9, deal);
	series_bind_figure(stmt, 10, market_avg);

	if (!series_step_done(self, stmt, "writing a region row", error))
		return FALSE;

	result->rows++;
	return TRUE;
}

static gboolean
series_region_apply(
	VentureSeriesStore		 *self,
	const gchar			 *group_key,
	gint64				  now,
	gint64				  deal_min_value,
	const gchar			 *deal_min_currency,
	VentureSeriesRegionResult	 *result,
	GError				**error
){
	g_autoptr(GHashTable) groups = NULL;
	SeriesRegionBatch batch;
	gboolean ok;
	gint rc;

	memset(&batch, 0, sizeof(batch));
	batch.mins = g_array_new(FALSE, FALSE, sizeof(gint64));
	batch.markets = g_array_new(FALSE, FALSE, sizeof(gint64));
	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ok = FALSE;

	{
		g_autoptr(SeriesCachedStmt) clear = NULL;

		clear = series_stmt(self, (NULL != group_key) ?
		                    series_sql_region_clear_group :
		                    series_sql_region_clear_all, error);
		if (NULL == clear)
			goto out;

		if (NULL != group_key)
			series_bind_text(clear, 1, group_key);

		if (!series_step_done(self, clear, "clearing the region", error))
			goto out;
	}

	/*
	 * The stream reads current and writes region, a different table, so
	 * the write cannot disturb the read. Rows arrive grouped; each group,
	 * instrument and currency is flushed when the next one starts.
	 */
	{
		g_autoptr(SeriesCachedStmt) rows = NULL;

		rows = series_stmt(self, (NULL != group_key) ?
		                   series_sql_region_rows_group :
		                   series_sql_region_rows_all, error);
		if (NULL == rows)
			goto out;

		if (NULL != group_key)
			series_bind_text(rows, 1, group_key);

		while (SQLITE_ROW == (rc = sqlite3_step(rows)))
		{
			const gchar *group;
			const gchar *currency;
			gint64 instrument_id;
			gint64 min_price;
			gint64 quantity;
			gint64 market;

			group = (const gchar *)sqlite3_column_text(rows, 0);
			instrument_id = sqlite3_column_int64(rows, 1);
			currency = (const gchar *)sqlite3_column_text(rows, 2);
			min_price = series_column_figure(rows, 3);
			quantity = series_column_figure(rows, 4);
			market = series_column_figure(rows, 5);

			if (NULL == group)
				group = "";
			if (NULL == currency)
				currency = "";

			if ((NULL == batch.group_key) ||
			    (0 != strcmp(batch.group_key, group)) ||
			    (batch.instrument_id != instrument_id) ||
			    (0 != strcmp(batch.currency, currency)))
			{
				if (!series_region_flush(self, &batch, now, deal_min_value,
				                         deal_min_currency, result, error))
					goto out;

				if (!g_hash_table_contains(groups, group))
					g_hash_table_add(groups, g_strdup(group));

				g_free(batch.group_key);
				batch.group_key = g_strdup(group);
				batch.instrument_id = instrument_id;
				g_strlcpy(batch.currency, currency, sizeof(batch.currency));
				g_array_set_size(batch.mins, 0);
				g_array_set_size(batch.markets, 0);
				batch.total_quantity = 0;
			}

			/* Offered: a price, and a quantity that is not known to be 0. */
			if ((VENTURE_SERIES_NONE == min_price) || (0 == quantity))
				continue;

			g_array_append_val(batch.mins, min_price);

			if (VENTURE_SERIES_NONE != market)
				g_array_append_val(batch.markets, market);

			if ((VENTURE_SERIES_NONE != quantity) &&
			    !venture_series_math_add(batch.total_quantity, quantity,
			                             &batch.total_quantity))
				batch.total_quantity = G_MAXINT64;
		}

		if (SQLITE_DONE != rc)
		{
			series_set_sqlite_error(self, rc, "reading current prices", error);
			goto out;
		}
	}

	if (!series_region_flush(self, &batch, now, deal_min_value,
	                         deal_min_currency, result, error))
		goto out;

	result->groups = g_hash_table_size(groups);

	/* Then current, from region, in two set-based statements. */
	{
		g_autoptr(SeriesCachedStmt) apply = NULL;

		apply = series_stmt(self, series_sql_region_apply, error);
		if (NULL == apply)
			goto out;

		series_bind_text(apply, 1, group_key);

		if (!series_step_done(self, apply, "applying the region", error))
			goto out;

		result->current_updated += sqlite3_changes(self->db);
	}

	{
		g_autoptr(SeriesCachedStmt) orphans = NULL;

		orphans = series_stmt(self, series_sql_region_orphans, error);
		if (NULL == orphans)
			goto out;

		series_bind_text(orphans, 1, group_key);

		if (!series_step_done(self, orphans, "clearing stale region figures",
		                      error))
			goto out;

		result->current_updated += sqlite3_changes(self->db);
	}

	ok = TRUE;

out:
	g_free(batch.group_key);
	g_array_unref(batch.mins);
	g_array_unref(batch.markets);
	return ok;
}

gboolean
venture_series_store_recompute_region(
	VentureSeriesStore		 *self,
	const gchar			 *group_key,
	gint64				  now,
	gint64				  deal_min_value,
	const gchar			 *deal_min_currency,
	VentureSeriesRegionResult	 *out,
	GError				**error
){
	VentureSeriesRegionResult result;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64 started;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);

	memset(&result, 0, sizeof(result));
	currency[0] = '\0';

	if (VENTURE_SERIES_NONE != deal_min_value)
	{
		if (deal_min_value < 0)
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A deal's minimum value must not be negative");
			return FALSE;
		}

		if (!series_normalise_currency(deal_min_currency, currency, error))
			return FALSE;
	}

	if ((NULL != group_key) &&
	    !series_check_text(group_key, VENTURE_SERIES_MAX_KEY_LENGTH,
	                       "venue group", error))
		return FALSE;

	started = g_get_monotonic_time();

	if (!series_txn_begin(self, error))
		return FALSE;

	if (!series_region_apply(self, group_key, now, deal_min_value,
	                         ('\0' != currency[0]) ? currency : NULL, &result,
	                         error))
	{
		series_txn_rollback_one(self);
		return FALSE;
	}

	if (!series_txn_commit(self, error))
		return FALSE;

	g_debug("Series store %s: region recomputed, %" G_GINT64_FORMAT
	        " rows, %" G_GINT64_FORMAT " current rows changed in %"
	        G_GINT64_FORMAT " ms", self->path, result.rows,
	        result.current_updated, (g_get_monotonic_time() - started) / 1000);

	if (NULL != out)
		*out = result;

	return TRUE;
}

/* --- Retention ------------------------------------------------------------------------- */

static const gchar series_sql_purge_hourly[] =
	"DELETE FROM hourly WHERE day < ?1";
static const gchar series_sql_purge_quotes[] =
	"DELETE FROM quote_history WHERE taken_at < ?1";
static const gchar series_sql_purge_snapshots[] =
	"DELETE FROM snapshots WHERE taken_at < ?1";
static const gchar series_sql_purge_daily[] =
	"DELETE FROM daily WHERE day < ?1";
static const gchar series_sql_purge_entries[] =
	"DELETE FROM entries WHERE COALESCE(published_at, fetched_at) < ?1";

static gboolean
series_purge_one(
	VentureSeriesStore	 *self,
	const gchar		 *sql,
	gint64			  bound,
	gint64			 *out_count,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	stmt = series_stmt(self, sql, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, bound);

	if (!series_step_done(self, stmt, "purging history", error))
		return FALSE;

	*out_count = sqlite3_changes(self->db);
	return TRUE;
}

gboolean
venture_series_store_purge(
	VentureSeriesStore		 *self,
	gint64				  now,
	guint				  hourly_days,
	guint				  daily_days,
	VentureSeriesPurgeResult	 *out,
	GError				**error
){
	VentureSeriesPurgeResult result;
	gint64 today;
	gboolean ok;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);

	memset(&result, 0, sizeof(result));
	today = series_day_of(now);

	if (!series_txn_begin(self, error))
		return FALSE;

	/*
	 * Keeping N days means today and the N - 1 before it: everything
	 * before day today - N + 1 goes.
	 */
	ok = TRUE;

	if (hourly_days > 0)
	{
		gint64 first;

		first = today - (gint64)hourly_days + 1;
		ok = series_purge_one(self, series_sql_purge_hourly, first,
		                      &result.hourly, error) &&
		     series_purge_one(self, series_sql_purge_quotes, first * 86400,
		                      &result.quotes, error) &&
		     series_purge_one(self, series_sql_purge_snapshots, first * 86400,
		                      &result.snapshots, error);
	}

	if (ok && (daily_days > 0))
	{
		gint64 first;

		first = today - (gint64)daily_days + 1;
		ok = series_purge_one(self, series_sql_purge_daily, first,
		                      &result.daily, error) &&
		     series_purge_one(self, series_sql_purge_entries, first * 86400,
		                      &result.entries, error);
	}

	if (!ok)
	{
		series_txn_rollback_one(self);
		return FALSE;
	}

	if (!series_txn_commit(self, error))
		return FALSE;

	/*
	 * Hand the freed pages back to the file system. Outside the
	 * transaction, and only at the outermost level: an operator's batch
	 * still open would make this a no-op anyway.
	 */
	if ((0 == self->depth) &&
	    !series_exec(self, "PRAGMA incremental_vacuum", "vacuuming", error))
		return FALSE;

	if (NULL != out)
		*out = result;

	return TRUE;
}

/* --- Reading: rows ------------------------------------------------------------------------ */

#define SERIES_ROW_COLUMNS \
	"v.key, v.name, v.group_key, i.key, i.name, i.category, i.kind," \
	" c.currency, c.taken_at, c.seen_at, c.quantity, c.listings, c.min_price," \
	" c.market_value, c.median, c.p15, c.mean, c.stddev, c.bid_price," \
	" c.bid_quantity, c.region_median, c.deal_price, c.pct_vs_region"

#define SERIES_ROW_FROM \
	" FROM current c JOIN venues v ON v.id = c.venue_id" \
	" JOIN instruments i ON i.id = c.instrument_id"

static VentureSeriesRow *
series_row_from(sqlite3_stmt *stmt)
{
	VentureSeriesRow *row;

	row = g_new0(VentureSeriesRow, 1);
	row->venue_key = series_column_strdup(stmt, 0);
	row->venue_name = series_column_strdup(stmt, 1);
	row->group_key = series_column_strdup(stmt, 2);
	row->instrument_key = series_column_strdup(stmt, 3);
	row->instrument_name = series_column_strdup(stmt, 4);
	row->category = series_column_strdup(stmt, 5);
	row->kind = series_column_strdup(stmt, 6);
	series_column_currency(stmt, 7, row->currency);
	row->taken_at = sqlite3_column_int64(stmt, 8);
	row->seen_at = sqlite3_column_int64(stmt, 9);
	row->quantity = series_column_figure(stmt, 10);
	row->listings = sqlite3_column_int64(stmt, 11);
	row->min_price = series_column_figure(stmt, 12);
	row->market_value = series_column_figure(stmt, 13);
	row->median = series_column_figure(stmt, 14);
	row->p15 = series_column_figure(stmt, 15);
	row->mean = series_column_figure(stmt, 16);
	row->stddev = series_column_figure(stmt, 17);
	row->bid_price = series_column_figure(stmt, 18);
	row->bid_quantity = sqlite3_column_int64(stmt, 19);
	row->region_median = series_column_figure(stmt, 20);
	row->deal_price = series_column_figure(stmt, 21);
	row->pct_vs_region = (SQLITE_NULL == sqlite3_column_type(stmt, 22)) ?
	                     NAN : sqlite3_column_double(stmt, 22);

	if (NULL == row->group_key)
		row->group_key = g_strdup("");

	return row;
}

void
venture_series_row_free(VentureSeriesRow *row)
{
	if (NULL == row)
		return;

	g_free(row->venue_key);
	g_free(row->venue_name);
	g_free(row->group_key);
	g_free(row->instrument_key);
	g_free(row->instrument_name);
	g_free(row->category);
	g_free(row->kind);
	g_free(row);
}

void
venture_series_filter_init(VentureSeriesFilter *filter)
{
	g_return_if_fail(NULL != filter);

	memset(filter, 0, sizeof(*filter));
	filter->min_value = VENTURE_SERIES_NONE;
	filter->sort = VENTURE_SERIES_SORT_MIN_PRICE;
}

static const gchar *const series_sort_names[] = {
	"min_price", "market_value", "quantity", "listings", "pct_vs_region",
	"deal_price", "region_median", "name", "updated", "venue"
};

/* The SQL each sort orders by: fixed text, never built from input. */
static const gchar *const series_sort_columns[] = {
	"c.min_price", "c.market_value", "c.quantity", "c.listings",
	"c.pct_vs_region", "c.deal_price", "c.region_median",
	"COALESCE(i.name_fold, i.key)", "c.taken_at", "v.key"
};

gboolean
venture_series_sort_from_string(
	const gchar		*name,
	VentureSeriesSort	*out
){
	guint i;

	g_return_val_if_fail(NULL != out, FALSE);

	for (i = 0; (NULL != name) && (i < G_N_ELEMENTS(series_sort_names)); i++)
	{
		if (0 == g_strcmp0(name, series_sort_names[i]))
		{
			*out = (VentureSeriesSort)i;
			return TRUE;
		}
	}

	return FALSE;
}

const gchar *
venture_series_sort_to_string(VentureSeriesSort sort)
{
	if (((guint)sort) >= G_N_ELEMENTS(series_sort_names))
		return "min_price";

	return series_sort_names[sort];
}

/* Escapes LIKE's wildcards and its escape character. */
static gchar *
series_like_escape(const gchar *text)
{
	GString *out;
	const gchar *p;

	out = g_string_new(NULL);

	for (p = text; '\0' != *p; p++)
	{
		if (('%' == *p) || ('_' == *p) || ('\\' == *p))
			g_string_append_c(out, '\\');
		g_string_append_c(out, *p);
	}

	return g_string_free(out, FALSE);
}

typedef struct
{
	gboolean	 is_text;
	gchar		*text;
	gint64		 value;
} SeriesBinding;

static void
series_binding_clear(gpointer data)
{
	g_free(((SeriesBinding *)data)->text);
}

static void
series_bind_add_text(
	GArray		*bindings,
	const gchar	*text
){
	SeriesBinding binding;

	binding.is_text = TRUE;
	binding.text = g_strdup(text);
	binding.value = 0;
	g_array_append_val(bindings, binding);
}

static void
series_bind_add_int(
	GArray	*bindings,
	gint64	 value
){
	SeriesBinding binding;

	binding.is_text = FALSE;
	binding.text = NULL;
	binding.value = value;
	g_array_append_val(bindings, binding);
}

/*
 * The WHERE clause of a filter, with its bindings. Every value reaches
 * SQL as a parameter; the only text spliced in is fixed.
 */
static gboolean
series_filter_where(
	const VentureSeriesFilter	 *filter,
	GString				 *sql,
	GArray				 *bindings,
	gchar				  currency[VENTURE_MONEY_CURRENCY_LEN],
	GError				**error
){
	g_string_append(sql, " WHERE 1");

	if (NULL == filter)
		return TRUE;

	if (NULL != filter->venue_keys)
	{
		guint n;
		guint i;

		n = g_strv_length((gchar **)filter->venue_keys);

		if (n > SERIES_MAX_FILTER_VENUES)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A filter may name at most %d venues",
			            SERIES_MAX_FILTER_VENUES);
			return FALSE;
		}

		/* An empty set of venues matches nothing, not everything. */
		if (0 == n)
			g_string_append(sql, " AND 0");
		else
		{
			g_string_append(sql, " AND v.key IN (");
			for (i = 0; i < n; i++)
			{
				g_string_append(sql, (0 == i) ? "?" : ", ?");
				series_bind_add_text(bindings, filter->venue_keys[i]);
			}
			g_string_append(sql, ")");
		}
	}

	if (NULL != filter->group_key)
	{
		g_string_append(sql, " AND v.group_key = ?");
		series_bind_add_text(bindings, filter->group_key);
	}

	if (NULL != filter->instrument_key)
	{
		g_string_append(sql, " AND i.key = ?");
		series_bind_add_text(bindings, filter->instrument_key);
	}

	if ((NULL != filter->search) && ('\0' != filter->search[0]))
	{
		g_autofree gchar *fold = NULL;
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *pattern = NULL;

		if (!g_utf8_validate(filter->search, -1, NULL))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A search must be UTF-8");
			return FALSE;
		}

		fold = g_utf8_casefold(filter->search, -1);
		escaped = series_like_escape(fold);
		pattern = g_strdup_printf("%%%s%%", escaped);

		g_string_append(sql, " AND (i.name_fold LIKE ? ESCAPE '\\'"
		                     " OR i.key LIKE ? ESCAPE '\\')");
		series_bind_add_text(bindings, pattern);
		series_bind_add_text(bindings, pattern);
	}

	if ((NULL != filter->category_prefix) && ('\0' != filter->category_prefix[0]))
	{
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *pattern = NULL;

		escaped = series_like_escape(filter->category_prefix);
		pattern = g_strdup_printf("%s/%%", escaped);

		g_string_append(sql, " AND (i.category = ?"
		                     " OR i.category LIKE ? ESCAPE '\\')");
		series_bind_add_text(bindings, filter->category_prefix);
		series_bind_add_text(bindings, pattern);
	}

	if (VENTURE_SERIES_NONE != filter->min_value)
	{
		if (NULL == filter->min_value_currency)
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A minimum value needs its currency");
			return FALSE;
		}

		if (!series_normalise_currency(filter->min_value_currency, currency,
		                               error))
			return FALSE;

		g_string_append(sql, " AND c.currency = ?"
		                     " AND COALESCE(c.market_value, c.min_price) >= ?");
		series_bind_add_text(bindings, currency);
		series_bind_add_int(bindings, filter->min_value);
	}

	if (filter->in_stock_only)
		g_string_append(sql, " AND c.min_price IS NOT NULL"
		                     " AND (c.quantity IS NULL OR c.quantity > 0)");

	return TRUE;
}

static SeriesOwnedStmt *
series_prepare_bound(
	VentureSeriesStore	 *self,
	const gchar		 *sql,
	GArray			 *bindings,
	GError			**error
){
	sqlite3_stmt *stmt;
	guint i;
	gint rc;

	stmt = NULL;
	rc = sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);

	if (SQLITE_OK != rc)
	{
		series_set_sqlite_error(self, rc, "preparing a listing", error);
		sqlite3_finalize(stmt);
		return NULL;
	}

	for (i = 0; i < bindings->len; i++)
	{
		SeriesBinding *binding;

		binding = &g_array_index(bindings, SeriesBinding, i);

		if (binding->is_text)
			series_bind_text(stmt, (gint)i + 1, binding->text);
		else
			sqlite3_bind_int64(stmt, (gint)i + 1, binding->value);
	}

	return stmt;
}

/* Collects every row of a statement through @row_from. */
static GPtrArray *
series_collect(
	VentureSeriesStore	 *self,
	sqlite3_stmt		 *stmt,
	gpointer		(*row_from) (sqlite3_stmt *),
	GDestroyNotify		  free_row,
	const gchar		 *what,
	GError			**error
){
	g_autoptr(GPtrArray) rows = NULL;
	gint rc;

	rows = g_ptr_array_new_with_free_func(free_row);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
		g_ptr_array_add(rows, row_from(stmt));

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, what, error);
		return NULL;
	}

	return g_steal_pointer(&rows);
}

static gpointer
series_row_from_any(sqlite3_stmt *stmt)
{
	return series_row_from(stmt);
}

GPtrArray *
venture_series_store_list_current(
	VentureSeriesStore		 *self,
	const VentureSeriesFilter	 *filter,
	GError				**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	VentureSeriesFilter defaults;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	guint count;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (NULL == filter)
	{
		venture_series_filter_init(&defaults);
		filter = &defaults;
	}

	if (((guint)filter->sort) >= G_N_ELEMENTS(series_sort_columns))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "That is not a sort the series store offers");
		return NULL;
	}

	count = (0 == filter->count) ? VENTURE_SERIES_DEFAULT_PAGE : filter->count;

	if (count > VENTURE_SERIES_MAX_PAGE)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A page holds at most %d rows", VENTURE_SERIES_MAX_PAGE);
		return NULL;
	}

	sql = g_string_new("SELECT " SERIES_ROW_COLUMNS SERIES_ROW_FROM);
	bindings = g_array_new(FALSE, TRUE, sizeof(SeriesBinding));
	g_array_set_clear_func(bindings, series_binding_clear);

	if (!series_filter_where(filter, sql, bindings, currency, error))
		return NULL;

	/*
	 * Unknown values last whichever way the sort runs, said outright: a
	 * page of instruments with no price ahead of the cheap ones is a page
	 * of nothing. Ties break on venue and instrument so paging is stable.
	 */
	g_string_append_printf(sql, " ORDER BY %s %s NULLS LAST, v.key, i.key"
	                            " LIMIT ? OFFSET ?",
	                       series_sort_columns[filter->sort],
	                       filter->descending ? "DESC" : "ASC");
	series_bind_add_int(bindings, count);
	series_bind_add_int(bindings, filter->offset);

	stmt = series_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	return series_collect(self, stmt, series_row_from_any,
	                      (GDestroyNotify)venture_series_row_free,
	                      "listing current prices", error);
}

gboolean
venture_series_store_count_current(
	VentureSeriesStore		 *self,
	const VentureSeriesFilter	 *filter,
	gint64				 *out_count,
	GError				**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out_count, FALSE);

	sql = g_string_new("SELECT count(*)" SERIES_ROW_FROM);
	bindings = g_array_new(FALSE, TRUE, sizeof(SeriesBinding));
	g_array_set_clear_func(bindings, series_binding_clear);

	if (!series_filter_where(filter, sql, bindings, currency, error))
		return FALSE;

	stmt = series_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return FALSE;

	rc = sqlite3_step(stmt);

	if (SQLITE_ROW != rc)
	{
		series_set_sqlite_error(self, rc, "counting current prices", error);
		return FALSE;
	}

	*out_count = sqlite3_column_int64(stmt, 0);
	return TRUE;
}

static const gchar series_sql_get_current[] =
	"SELECT " SERIES_ROW_COLUMNS SERIES_ROW_FROM
	" WHERE v.key = ?1 AND i.key = ?2";

gboolean
venture_series_store_get_current(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	VentureSeriesRow	**out,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	*out = NULL;

	stmt = series_stmt(self, series_sql_get_current, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, venue_key);
	series_bind_text(stmt, 2, instrument_key);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW == rc)
	{
		*out = series_row_from(stmt);
		return TRUE;
	}

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, "reading a current price", error);
		return FALSE;
	}

	return TRUE;
}

static const gchar series_sql_other_venues[] =
	"SELECT " SERIES_ROW_COLUMNS SERIES_ROW_FROM
	" WHERE i.key = ?1 AND (?2 IS NULL OR v.group_key = ?2)"
	" ORDER BY c.min_price ASC NULLS LAST, v.key";

GPtrArray *
venture_series_store_other_venues(
	VentureSeriesStore	 *self,
	const gchar		 *instrument_key,
	const gchar		 *group_key,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = series_stmt(self, series_sql_other_venues, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, instrument_key);
	series_bind_text(stmt, 2, group_key);

	return series_collect(self, stmt, series_row_from_any,
	                      (GDestroyNotify)venture_series_row_free,
	                      "listing other venues", error);
}

/* --- Reading: series ------------------------------------------------------------------- */

static const gchar series_sql_pair_ids[] =
	"SELECT v.id, i.id FROM venues v, instruments i"
	" WHERE v.key = ?1 AND i.key = ?2";

/* The ids of a venue and an instrument; both 0 when either is unknown. */
static gboolean
series_pair_ids(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			 *out_venue,
	gint64			 *out_instrument,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint rc;

	*out_venue = 0;
	*out_instrument = 0;

	stmt = series_stmt(self, series_sql_pair_ids, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, venue_key);
	series_bind_text(stmt, 2, instrument_key);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW == rc)
	{
		*out_venue = sqlite3_column_int64(stmt, 0);
		*out_instrument = sqlite3_column_int64(stmt, 1);
		return TRUE;
	}

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, "finding a venue and instrument",
		                        error);
		return FALSE;
	}

	return TRUE;
}

static const gchar series_sql_hourly[] =
	"SELECT day, currency, points FROM hourly"
	" WHERE venue_id = ?1 AND instrument_id = ?2 AND day >= ?3 ORDER BY day";

GArray *
venture_series_store_hourly(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  since,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	g_autoptr(GArray) points = NULL;
	gint64 venue_id;
	gint64 instrument_id;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	points = g_array_new(FALSE, TRUE, sizeof(VentureSeriesPoint));

	if (!series_pair_ids(self, venue_key, instrument_key, &venue_id,
	                     &instrument_id, error))
		return NULL;

	if (0 == venue_id)
		return g_steal_pointer(&points);

	stmt = series_stmt(self, series_sql_hourly, error);
	if (NULL == stmt)
		return NULL;

	sqlite3_bind_int64(stmt, 1, venue_id);
	sqlite3_bind_int64(stmt, 2, instrument_id);
	sqlite3_bind_int64(stmt, 3, series_day_of(since));

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		SeriesHour hours[24];
		SeriesReader reader;
		gint64 day;
		guint hour;

		day = sqlite3_column_int64(stmt, 0);
		series_reader_init(&reader, stmt, 2);

		if (!series_decode_points(&reader, hours))
		{
			series_set_corrupt(self, "hourly series", error);
			return NULL;
		}

		for (hour = 0; hour < 24; hour++)
		{
			VentureSeriesPoint point;

			if (!hours[hour].present)
				continue;

			point.at = day * 86400 + (gint64)hour * 3600;

			if (point.at + 3599 < since)
				continue;

			series_column_currency(stmt, 1, point.currency);
			point.min_price = hours[hour].min_price;
			point.quantity = hours[hour].quantity;
			point.market_value = hours[hour].market_value;
			point.listings = hours[hour].listings;
			g_array_append_val(points, point);
		}
	}

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, "reading an hourly series", error);
		return NULL;
	}

	return g_steal_pointer(&points);
}

static const gchar series_sql_daily[] =
	"SELECT day, currency, snapshots, min_price, max_quantity, price_at_max,"
	"       market_value, mean, listings, sold_estimate, sold_value,"
	"       expired_estimate, sale_avg"
	" FROM daily WHERE venue_id = ?1 AND instrument_id = ?2 AND day >= ?3"
	" ORDER BY day, currency";

/* What units sold for on average: from the sales, else as the source said. */
static gint64
series_sale_avg(
	gint64	sold,
	gint64	sold_value,
	gint64	stated
){
	if ((sold > 0) && (sold_value >= 0))
		return venture_series_math_div_round(sold_value, sold);

	return stated;
}

static void
series_day_from(
	sqlite3_stmt		*stmt,
	VentureSeriesDay	*day
){
	day->day_start = sqlite3_column_int64(stmt, 0) * 86400;
	series_column_currency(stmt, 1, day->currency);
	day->snapshots = sqlite3_column_int64(stmt, 2);
	day->min_price = series_column_figure(stmt, 3);
	day->max_quantity = sqlite3_column_int64(stmt, 4);
	day->price_at_max = series_column_figure(stmt, 5);
	day->market_value = series_column_figure(stmt, 6);
	day->mean = series_column_figure(stmt, 7);
	day->listings = sqlite3_column_int64(stmt, 8);
	day->sold_estimate = sqlite3_column_int64(stmt, 9);
	day->expired_estimate = sqlite3_column_int64(stmt, 11);
	day->sale_avg = series_sale_avg(day->sold_estimate,
	                                sqlite3_column_int64(stmt, 10),
	                                series_column_figure(stmt, 12));
}

GArray *
venture_series_store_daily(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  since,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	g_autoptr(GArray) days = NULL;
	gint64 venue_id;
	gint64 instrument_id;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	days = g_array_new(FALSE, TRUE, sizeof(VentureSeriesDay));

	if (!series_pair_ids(self, venue_key, instrument_key, &venue_id,
	                     &instrument_id, error))
		return NULL;

	if (0 == venue_id)
		return g_steal_pointer(&days);

	stmt = series_stmt(self, series_sql_daily, error);
	if (NULL == stmt)
		return NULL;

	sqlite3_bind_int64(stmt, 1, venue_id);
	sqlite3_bind_int64(stmt, 2, instrument_id);
	sqlite3_bind_int64(stmt, 3, series_day_of(since));

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesDay day;

		series_day_from(stmt, &day);
		g_array_append_val(days, day);
	}

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, "reading a daily series", error);
		return NULL;
	}

	return g_steal_pointer(&days);
}

gboolean
venture_series_store_heat(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  since,
	gint64			  zone_offset,
	VentureSeriesHeat	 *out,
	gchar			  out_currency[VENTURE_MONEY_CURRENCY_LEN],
	GError			**error
){
	g_autoptr(GArray) points = NULL;
	g_autoptr(GArray) times = NULL;
	g_autoptr(GArray) values = NULL;
	const gchar *currency;
	guint i;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);
	g_return_val_if_fail(NULL != out_currency, FALSE);

	out_currency[0] = '\0';

	points = venture_series_store_hourly(self, venue_key, instrument_key, since,
	                                     error);
	if (NULL == points)
		return FALSE;

	times = g_array_new(FALSE, FALSE, sizeof(gint64));
	values = g_array_new(FALSE, FALSE, sizeof(gint64));

	/* The newest point's currency is the matrix's; others are left out. */
	currency = (points->len > 0) ?
	           g_array_index(points, VentureSeriesPoint, points->len - 1).currency :
	           "";

	for (i = 0; i < points->len; i++)
	{
		const VentureSeriesPoint *point;

		point = &g_array_index(points, VentureSeriesPoint, i);

		if (0 != strcmp(point->currency, currency))
			continue;

		g_array_append_val(times, point->at);
		g_array_append_val(values, point->min_price);
	}

	if (!venture_series_math_heat((const gint64 *)(gpointer)times->data,
	                              (const gint64 *)(gpointer)values->data,
	                              times->len, zone_offset, out, error))
		return FALSE;

	g_strlcpy(out_currency, currency, VENTURE_MONEY_CURRENCY_LEN);
	return TRUE;
}

static const gchar series_sql_tiers[] =
	"SELECT c.currency, c.tiers FROM current c"
	" JOIN venues v ON v.id = c.venue_id"
	" JOIN instruments i ON i.id = c.instrument_id"
	" WHERE v.key = ?1 AND i.key = ?2";

static GArray *
series_tiers_for(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gchar			  out_currency[VENTURE_MONEY_CURRENCY_LEN],
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	SeriesReader reader;
	GArray *tiers;
	gint rc;

	out_currency[0] = '\0';

	stmt = series_stmt(self, series_sql_tiers, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, venue_key);
	series_bind_text(stmt, 2, instrument_key);
	rc = sqlite3_step(stmt);

	if (SQLITE_DONE == rc)
		return g_array_new(FALSE, TRUE, sizeof(VentureSeriesTier));

	if (SQLITE_ROW != rc)
	{
		series_set_sqlite_error(self, rc, "reading a book", error);
		return NULL;
	}

	series_reader_init(&reader, stmt, 1);
	tiers = series_decode_tiers(&reader);

	if (NULL == tiers)
	{
		series_set_corrupt(self, "book", error);
		return NULL;
	}

	if (tiers->len > 0)
		series_column_currency(stmt, 0, out_currency);

	return tiers;
}

GArray *
venture_series_store_get_tiers(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	GError			**error
){
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	return series_tiers_for(self, venue_key, instrument_key, currency, error);
}

gboolean
venture_series_store_bulk_cost(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  units,
	VentureSeriesBulkCost	 *out,
	gchar			  out_currency[VENTURE_MONEY_CURRENCY_LEN],
	GError			**error
){
	g_autoptr(GArray) tiers = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);
	g_return_val_if_fail(NULL != out_currency, FALSE);

	tiers = series_tiers_for(self, venue_key, instrument_key, out_currency,
	                         error);
	if (NULL == tiers)
		return FALSE;

	return venture_series_math_bulk_cost(
		(const VentureSeriesTier *)(gpointer)tiers->data, tiers->len, units,
		out, error);
}

/* --- Reading: region, venues, reference ----------------------------------------------- */

static const gchar series_sql_get_region[] =
	"SELECT r.group_key, r.currency, r.computed_at, r.venues_offering,"
	"       r.total_quantity, r.median_min, r.p33, r.deal_price, r.market_avg"
	" FROM region r JOIN instruments i ON i.id = r.instrument_id"
	" WHERE r.group_key = ?1 AND i.key = ?2 AND (?3 IS NULL OR r.currency = ?3)"
	" ORDER BY r.venues_offering DESC, r.currency LIMIT 1";

gboolean
venture_series_store_get_region(
	VentureSeriesStore	 *self,
	const gchar		 *group_key,
	const gchar		 *instrument_key,
	const gchar		 *currency,
	VentureSeriesRegion	 *out,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gchar normalised[VENTURE_MONEY_CURRENCY_LEN];
	const guchar *group;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));

	if ((NULL != currency) &&
	    !series_normalise_currency(currency, normalised, error))
		return FALSE;

	stmt = series_stmt(self, series_sql_get_region, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, (NULL != group_key) ? group_key : "");
	series_bind_text(stmt, 2, instrument_key);
	series_bind_text(stmt, 3, (NULL != currency) ? normalised : NULL);
	rc = sqlite3_step(stmt);

	if (SQLITE_DONE == rc)
		return TRUE;

	if (SQLITE_ROW != rc)
	{
		series_set_sqlite_error(self, rc, "reading a region row", error);
		return FALSE;
	}

	out->found = TRUE;
	group = sqlite3_column_text(stmt, 0);
	g_strlcpy(out->group_key, (NULL != group) ? (const gchar *)group : "",
	          sizeof(out->group_key));
	series_column_currency(stmt, 1, out->currency);
	out->computed_at = sqlite3_column_int64(stmt, 2);
	out->venues_offering = sqlite3_column_int64(stmt, 3);
	out->total_quantity = sqlite3_column_int64(stmt, 4);
	out->median_min = series_column_figure(stmt, 5);
	out->p33 = series_column_figure(stmt, 6);
	out->deal_price = series_column_figure(stmt, 7);
	out->market_avg = series_column_figure(stmt, 8);

	return TRUE;
}

/*
 * Cheaper, equal and dearer are counted only where the comparison exists:
 * a percent of the region median is set only for an instrument in stock
 * with a median above zero.
 */
static const gchar series_sql_venue_index[] =
	"SELECT v.key, v.name, v.group_key,"
	"  count(c.pct_vs_region),"
	"  sum(CASE WHEN c.pct_vs_region IS NOT NULL"
	"            AND c.min_price < c.region_median THEN 1 ELSE 0 END),"
	"  sum(CASE WHEN c.pct_vs_region IS NOT NULL"
	"            AND c.min_price = c.region_median THEN 1 ELSE 0 END),"
	"  sum(CASE WHEN c.pct_vs_region IS NOT NULL"
	"            AND c.min_price > c.region_median THEN 1 ELSE 0 END),"
	"  avg(c.pct_vs_region) / 100.0,"
	"  sum(c.listings),"
	"  s.last_taken_at, s.interval_seconds"
	" FROM venues v"
	" LEFT JOIN current c ON c.venue_id = v.id"
	" LEFT JOIN venue_state s ON s.venue_id = v.id"
	" WHERE (?1 IS NULL OR v.group_key = ?1)"
	" GROUP BY v.id ORDER BY v.key";

static gpointer
series_venue_stats_from(sqlite3_stmt *stmt)
{
	VentureSeriesVenueStats *stats;

	stats = g_new0(VentureSeriesVenueStats, 1);
	stats->venue_key = series_column_strdup(stmt, 0);
	stats->venue_name = series_column_strdup(stmt, 1);
	stats->group_key = series_column_strdup(stmt, 2);
	stats->instruments = sqlite3_column_int64(stmt, 3);
	stats->cheaper = sqlite3_column_int64(stmt, 4);
	stats->equal = sqlite3_column_int64(stmt, 5);
	stats->dearer = sqlite3_column_int64(stmt, 6);
	stats->avg_ratio = (SQLITE_NULL == sqlite3_column_type(stmt, 7)) ?
	                   NAN : sqlite3_column_double(stmt, 7);
	stats->listings = sqlite3_column_int64(stmt, 8);
	stats->last_taken_at = series_column_figure(stmt, 9);
	stats->interval_seconds = (SQLITE_NULL == sqlite3_column_type(stmt, 10)) ?
	                          VENTURE_SERIES_INTERVAL_DEFAULT :
	                          sqlite3_column_int64(stmt, 10);

	if (NULL == stats->group_key)
		stats->group_key = g_strdup("");

	return stats;
}

void
venture_series_venue_stats_free(VentureSeriesVenueStats *stats)
{
	if (NULL == stats)
		return;

	g_free(stats->venue_key);
	g_free(stats->venue_name);
	g_free(stats->group_key);
	g_free(stats);
}

GPtrArray *
venture_series_store_venue_index(
	VentureSeriesStore	 *self,
	const gchar		 *group_key,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = series_stmt(self, series_sql_venue_index, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, group_key);

	return series_collect(self, stmt, series_venue_stats_from,
	                      (GDestroyNotify)venture_series_venue_stats_free,
	                      "indexing venues", error);
}

static const gchar series_sql_reference_venue[] =
	"SELECT d.day, d.currency, d.market_value, d.sold_estimate, d.sold_value,"
	"       d.expired_estimate"
	" FROM daily d JOIN venues v ON v.id = d.venue_id"
	" JOIN instruments i ON i.id = d.instrument_id"
	" WHERE v.key = ?1 AND i.key = ?2 AND d.day >= ?3 AND d.day <= ?4";

static const gchar series_sql_reference_group[] =
	"SELECT d.day, d.currency, d.market_value, d.sold_estimate, d.sold_value,"
	"       d.expired_estimate"
	" FROM daily d JOIN venues v ON v.id = d.venue_id"
	" JOIN instruments i ON i.id = d.instrument_id"
	" WHERE v.group_key = ?1 AND i.key = ?2 AND d.day >= ?3 AND d.day <= ?4";

/* One day of history across whatever rows it had, in one currency. */
typedef struct
{
	GArray	*markets;
	gint64	 sold;
	gint64	 sold_value;
	gint64	 expired;
	gboolean present;
} SeriesRefDay;

gboolean
venture_series_store_reference(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *group_key,
	const gchar		 *instrument_key,
	gint64			  now,
	VentureSeriesReference	 *out,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	g_autoptr(GArray) rows = NULL;
	g_autoptr(GError) math_error = NULL;
	SeriesRefDay days[VENTURE_SERIES_HISTORICAL_DAYS];
	gint64 dailies[VENTURE_SERIES_HISTORICAL_DAYS];
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64 today;
	gint64 newest;
	gint64 sold;
	gint64 sold_value;
	gint64 expired;
	gboolean ok;
	guint i;
	gint rc;

	typedef struct
	{
		gint64	day;
		gchar	currency[VENTURE_MONEY_CURRENCY_LEN];
		gint64	market;
		gint64	sold;
		gint64	sold_value;
		gint64	expired;
	} RefRow;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	out->market_14d = VENTURE_SERIES_NONE;
	out->historical_60d = VENTURE_SERIES_NONE;
	out->sale_avg = VENTURE_SERIES_NONE;
	out->sale_rate = NAN;
	out->sold_per_day = NAN;

	if ((NULL == venue_key) && (NULL == group_key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A reference price needs a venue or a group");
		return FALSE;
	}

	today = series_day_of(now);

	stmt = series_stmt(self, (NULL != venue_key) ? series_sql_reference_venue :
	                                               series_sql_reference_group,
	                   error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, (NULL != venue_key) ? venue_key : group_key);
	series_bind_text(stmt, 2, instrument_key);
	sqlite3_bind_int64(stmt, 3, today - VENTURE_SERIES_HISTORICAL_DAYS + 1);
	sqlite3_bind_int64(stmt, 4, today);

	/* Read everything first: the newest day decides the currency. */
	rows = g_array_new(FALSE, TRUE, sizeof(RefRow));
	newest = VENTURE_SERIES_NONE;
	currency[0] = '\0';

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		RefRow row;

		row.day = sqlite3_column_int64(stmt, 0);
		series_column_currency(stmt, 1, row.currency);
		row.market = series_column_figure(stmt, 2);
		row.sold = sqlite3_column_int64(stmt, 3);
		row.sold_value = sqlite3_column_int64(stmt, 4);
		row.expired = sqlite3_column_int64(stmt, 5);
		g_array_append_val(rows, row);

		if ((VENTURE_SERIES_NONE == newest) || (row.day > newest) ||
		    ((row.day == newest) && (strcmp(row.currency, currency) < 0)))
		{
			newest = row.day;
			g_strlcpy(currency, row.currency, sizeof(currency));
		}
	}

	if (SQLITE_DONE != rc)
	{
		series_set_sqlite_error(self, rc, "reading daily history", error);
		return FALSE;
	}

	if (0 == rows->len)
		return TRUE;

	g_strlcpy(out->currency, currency, sizeof(out->currency));

	/* Fold the rows into days; a group's day has one row per venue. */
	memset(days, 0, sizeof(days));
	ok = FALSE;

	for (i = 0; i < rows->len; i++)
	{
		RefRow *row;
		SeriesRefDay *day;

		row = &g_array_index(rows, RefRow, i);

		if (0 != strcmp(row->currency, currency))
			continue;

		day = &days[today - row->day];
		day->present = TRUE;

		if (NULL == day->markets)
			day->markets = g_array_new(FALSE, FALSE, sizeof(gint64));

		if (VENTURE_SERIES_NONE != row->market)
			g_array_append_val(day->markets, row->market);

		if (!venture_series_math_add(day->sold, row->sold, &day->sold) ||
		    !venture_series_math_add(day->sold_value, row->sold_value,
		                             &day->sold_value) ||
		    !venture_series_math_add(day->expired, row->expired, &day->expired))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A day's sales do not fit in a 64-bit integer");
			goto out;
		}
	}

	/* Each day's market value: the venue's, or the group's mean. */
	for (i = 0; i < VENTURE_SERIES_HISTORICAL_DAYS; i++)
	{
		dailies[i] = VENTURE_SERIES_NONE;

		if ((NULL == days[i].markets) || (0 == days[i].markets->len))
			continue;

		dailies[i] = venture_series_math_mean(
			(const gint64 *)(gpointer)days[i].markets->data,
			days[i].markets->len, &math_error);

		if (NULL != math_error)
		{
			g_propagate_error(error, g_steal_pointer(&math_error));
			goto out;
		}
	}

	out->market_14d = venture_series_math_ewma(dailies, VENTURE_SERIES_EWMA_DAYS,
	                                           &math_error);
	if (NULL == math_error)
		out->historical_60d = venture_series_math_mean(
			dailies, VENTURE_SERIES_HISTORICAL_DAYS, &math_error);

	if (NULL != math_error)
	{
		g_propagate_error(error, g_steal_pointer(&math_error));
		goto out;
	}

	sold = 0;
	sold_value = 0;
	expired = 0;

	for (i = 0; i < VENTURE_SERIES_HISTORICAL_DAYS; i++)
	{
		if (!days[i].present)
			continue;

		out->days_60++;

		if (i >= VENTURE_SERIES_EWMA_DAYS)
			continue;

		out->days_14++;

		if (!venture_series_math_add(sold, days[i].sold, &sold) ||
		    !venture_series_math_add(sold_value, days[i].sold_value,
		                             &sold_value) ||
		    !venture_series_math_add(expired, days[i].expired, &expired))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Two weeks of sales do not fit in a 64-bit "
			                    "integer");
			goto out;
		}
	}

	if (sold > 0)
		out->sale_avg = venture_series_math_div_round(sold_value, sold);

	if (sold + expired > 0)
		out->sale_rate = (gdouble)sold / ((gdouble)sold + (gdouble)expired);

	if (out->days_14 > 0)
		out->sold_per_day = (gdouble)sold / (gdouble)out->days_14;

	ok = TRUE;

out:
	for (i = 0; i < VENTURE_SERIES_HISTORICAL_DAYS; i++)
	{
		if (NULL != days[i].markets)
			g_array_unref(days[i].markets);
	}

	return ok;
}

static const gchar series_sql_venue_state[] =
	"SELECT s.last_taken_at, s.last_fetched_at, s.interval_seconds, s.gaps"
	" FROM venues v LEFT JOIN venue_state s ON s.venue_id = v.id"
	" WHERE v.key = ?1";

gboolean
venture_series_store_get_venue_state(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	VentureSeriesVenueState	 *out,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	g_autoptr(GArray) gaps = NULL;
	SeriesReader reader;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	out->last_taken_at = VENTURE_SERIES_NONE;
	out->last_fetched_at = VENTURE_SERIES_NONE;
	out->interval_seconds = VENTURE_SERIES_INTERVAL_DEFAULT;
	out->next_expected = VENTURE_SERIES_NONE;

	stmt = series_stmt(self, series_sql_venue_state, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, venue_key);
	rc = sqlite3_step(stmt);

	if (SQLITE_DONE == rc)
		return TRUE;

	if (SQLITE_ROW != rc)
	{
		series_set_sqlite_error(self, rc, "reading a venue's state", error);
		return FALSE;
	}

	out->found = TRUE;
	out->last_taken_at = series_column_figure(stmt, 0);
	out->last_fetched_at = series_column_figure(stmt, 1);

	if (SQLITE_NULL != sqlite3_column_type(stmt, 2))
		out->interval_seconds = sqlite3_column_int64(stmt, 2);

	series_reader_init(&reader, stmt, 3);
	gaps = series_decode_gaps(&reader);

	if (NULL == gaps)
	{
		series_set_corrupt(self, "venue state", error);
		return FALSE;
	}

	out->gaps = gaps->len;

	if (VENTURE_SERIES_NONE != out->last_taken_at)
		out->next_expected = out->last_taken_at + out->interval_seconds;

	return TRUE;
}

static const gchar series_sql_list_venues[] =
	"SELECT key, namespace, name, kind, group_key, currency, attrs,"
	"       first_seen, last_seen"
	" FROM venues ORDER BY key";

static gpointer
series_venue_row_from(sqlite3_stmt *stmt)
{
	VentureSeriesVenueRow *row;

	row = g_new0(VentureSeriesVenueRow, 1);
	row->key = series_column_strdup(stmt, 0);
	row->namespace_ = series_column_strdup(stmt, 1);
	row->name = series_column_strdup(stmt, 2);
	row->kind = series_column_strdup(stmt, 3);
	row->group_key = series_column_strdup(stmt, 4);
	row->currency = series_column_strdup(stmt, 5);
	row->attrs_json = series_column_strdup(stmt, 6);
	row->first_seen = sqlite3_column_int64(stmt, 7);
	row->last_seen = sqlite3_column_int64(stmt, 8);

	if (NULL == row->group_key)
		row->group_key = g_strdup("");

	return row;
}

void
venture_series_venue_row_free(VentureSeriesVenueRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->namespace_);
	g_free(row->name);
	g_free(row->kind);
	g_free(row->group_key);
	g_free(row->currency);
	g_free(row->attrs_json);
	g_free(row);
}

GPtrArray *
venture_series_store_list_venues(
	VentureSeriesStore	 *self,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = series_stmt(self, series_sql_list_venues, error);
	if (NULL == stmt)
		return NULL;

	return series_collect(self, stmt, series_venue_row_from,
	                      (GDestroyNotify)venture_series_venue_row_free,
	                      "listing venues", error);
}

static const gchar series_sql_get_instrument[] =
	"SELECT key, namespace, name, kind, category, parent_key, attrs,"
	"       first_seen, last_seen"
	" FROM instruments WHERE key = ?1";

void
venture_series_instrument_row_free(VentureSeriesInstrumentRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->namespace_);
	g_free(row->name);
	g_free(row->kind);
	g_free(row->category);
	g_free(row->parent_key);
	g_free(row->attrs_json);
	g_free(row);
}

gboolean
venture_series_store_get_instrument(
	VentureSeriesStore		 *self,
	const gchar			 *instrument_key,
	VentureSeriesInstrumentRow	**out,
	GError				**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	VentureSeriesInstrumentRow *row;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	*out = NULL;

	stmt = series_stmt(self, series_sql_get_instrument, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, instrument_key);
	rc = sqlite3_step(stmt);

	if (SQLITE_DONE == rc)
		return TRUE;

	if (SQLITE_ROW != rc)
	{
		series_set_sqlite_error(self, rc, "reading an instrument", error);
		return FALSE;
	}

	row = g_new0(VentureSeriesInstrumentRow, 1);
	row->key = series_column_strdup(stmt, 0);
	row->namespace_ = series_column_strdup(stmt, 1);
	row->name = series_column_strdup(stmt, 2);
	row->kind = series_column_strdup(stmt, 3);
	row->category = series_column_strdup(stmt, 4);
	row->parent_key = series_column_strdup(stmt, 5);
	row->attrs_json = series_column_strdup(stmt, 6);
	row->first_seen = sqlite3_column_int64(stmt, 7);
	row->last_seen = sqlite3_column_int64(stmt, 8);

	*out = row;
	return TRUE;
}

static const gchar series_sql_list_quotes[] =
	"SELECT v.key, v.name, i.key, i.parent_key, q.side, q.value, q.currency,"
	"       q.liquidity, q.taken_at"
	" FROM quotes q JOIN venues v ON v.id = q.venue_id"
	" JOIN instruments i ON i.id = q.instrument_id"
	" WHERE (?1 IS NULL OR i.key = ?1) AND (?2 IS NULL OR i.parent_key = ?2)"
	" ORDER BY i.key, v.key, q.side";

static gpointer
series_quote_row_from(sqlite3_stmt *stmt)
{
	VentureSeriesQuoteRow *row;

	row = g_new0(VentureSeriesQuoteRow, 1);
	row->venue_key = series_column_strdup(stmt, 0);
	row->venue_name = series_column_strdup(stmt, 1);
	row->instrument_key = series_column_strdup(stmt, 2);
	row->parent_key = series_column_strdup(stmt, 3);
	row->side = (VentureSeriesQuoteSide)sqlite3_column_int(stmt, 4);
	row->value = sqlite3_column_int64(stmt, 5);
	series_column_currency(stmt, 6, row->currency);
	row->liquidity = series_column_figure(stmt, 7);
	row->taken_at = sqlite3_column_int64(stmt, 8);

	return row;
}

void
venture_series_quote_row_free(VentureSeriesQuoteRow *row)
{
	if (NULL == row)
		return;

	g_free(row->venue_key);
	g_free(row->venue_name);
	g_free(row->instrument_key);
	g_free(row->parent_key);
	g_free(row);
}

GPtrArray *
venture_series_store_list_quotes(
	VentureSeriesStore	 *self,
	const gchar		 *instrument_key,
	const gchar		 *parent_key,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = series_stmt(self, series_sql_list_quotes, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, instrument_key);
	series_bind_text(stmt, 2, parent_key);

	return series_collect(self, stmt, series_quote_row_from,
	                      (GDestroyNotify)venture_series_quote_row_free,
	                      "listing quotes", error);
}

static const gchar series_sql_list_entries[] =
	"SELECT e.key, e.title, e.url, e.summary, e.published_at, e.fetched_at,"
	"       v.key, i.key"
	" FROM entries e LEFT JOIN venues v ON v.id = e.venue_id"
	" LEFT JOIN instruments i ON i.id = e.instrument_id"
	" WHERE COALESCE(e.published_at, e.fetched_at) >= ?1"
	" ORDER BY COALESCE(e.published_at, e.fetched_at) DESC, e.id DESC"
	" LIMIT ?2";

static gpointer
series_entry_row_from(sqlite3_stmt *stmt)
{
	VentureSeriesEntryRow *row;

	row = g_new0(VentureSeriesEntryRow, 1);
	row->key = series_column_strdup(stmt, 0);
	row->title = series_column_strdup(stmt, 1);
	row->url = series_column_strdup(stmt, 2);
	row->summary = series_column_strdup(stmt, 3);
	row->published_at = series_column_figure(stmt, 4);
	row->fetched_at = sqlite3_column_int64(stmt, 5);
	row->venue_key = series_column_strdup(stmt, 6);
	row->instrument_key = series_column_strdup(stmt, 7);

	return row;
}

void
venture_series_entry_row_free(VentureSeriesEntryRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->title);
	g_free(row->url);
	g_free(row->summary);
	g_free(row->venue_key);
	g_free(row->instrument_key);
	g_free(row);
}

GPtrArray *
venture_series_store_list_entries(
	VentureSeriesStore	 *self,
	gint64			  since,
	guint			  count,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (0 == count)
		count = VENTURE_SERIES_DEFAULT_PAGE;

	if (count > VENTURE_SERIES_MAX_PAGE)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A page holds at most %d rows", VENTURE_SERIES_MAX_PAGE);
		return NULL;
	}

	stmt = series_stmt(self, series_sql_list_entries, error);
	if (NULL == stmt)
		return NULL;

	sqlite3_bind_int64(stmt, 1, since);
	sqlite3_bind_int64(stmt, 2, count);

	return series_collect(self, stmt, series_entry_row_from,
	                      (GDestroyNotify)venture_series_entry_row_free,
	                      "listing entries", error);
}
