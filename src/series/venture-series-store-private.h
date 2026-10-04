/*
 * venture-series-store-private.h - What the series store's files share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The store is one handle and one SQLite connection; the market figures
 * live in venture-series-store.c and the operator's accounts in
 * venture-series-accounts.c. Both prepare statements into the same
 * per-handle cache, nest transactions on the same counter and create
 * venues and instruments the same way, so the few internals they share
 * are declared here. Never installed and never included outside
 * src/series/: a plugin reads and writes a store through the public
 * headers only.
 */

#ifndef VENTURE_SERIES_STORE_PRIVATE_H
#define VENTURE_SERIES_STORE_PRIVATE_H

#include "venture.h"

#include <sqlite3.h>

G_BEGIN_DECLS

/*
 * A cached statement goes back to the cache reset: a SELECT left
 * mid-step holds its read transaction open, and on a reader handle that
 * would pin the snapshot it sees -- the page would show the same prices
 * forever while the writer moved on.
 */
typedef sqlite3_stmt SeriesCachedStmt;

static inline void
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

static inline void
series_owned_stmt_free(SeriesOwnedStmt *stmt)
{
	if (NULL != stmt)
		sqlite3_finalize(stmt);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SeriesOwnedStmt, series_owned_stmt_free)

static inline void
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
static inline void
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

static inline gint64
series_column_figure(
	sqlite3_stmt	*stmt,
	gint		 column
){
	if (SQLITE_NULL == sqlite3_column_type(stmt, column))
		return VENTURE_SERIES_NONE;

	return sqlite3_column_int64(stmt, column);
}

static inline gchar *
series_column_strdup(
	sqlite3_stmt	*stmt,
	gint		 column
){
	const guchar *text;

	text = sqlite3_column_text(stmt, column);

	return (NULL != text) ? g_strdup((const gchar *)text) : NULL;
}

static inline void
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

/* --- The handle's internals, defined in venture-series-store.c ------------ */

/* The handle's connection. */
sqlite3 *
venture_series_store_internal_db(VentureSeriesStore *self);

/* The cached statement for @sql, a static string whose address is the key. */
SeriesCachedStmt *
venture_series_store_internal_stmt(
	VentureSeriesStore	 *self,
	const gchar		 *sql,
	GError			**error
);

void
venture_series_store_internal_sqlite_error(
	VentureSeriesStore	 *self,
	gint			  rc,
	const gchar		 *what,
	GError			**error
);

/* Steps a statement expected to finish without rows. */
gboolean
venture_series_store_internal_step_done(
	VentureSeriesStore	 *self,
	sqlite3_stmt		 *stmt,
	const gchar		 *what,
	GError			**error
);

/* One transaction level: the outermost is BEGIN IMMEDIATE, the rest are
 * savepoints. Refused on a reader handle. */
gboolean
venture_series_store_internal_begin(
	VentureSeriesStore	 *self,
	GError			**error
);

gboolean
venture_series_store_internal_commit(
	VentureSeriesStore	 *self,
	GError			**error
);

void
venture_series_store_internal_rollback_one(VentureSeriesStore *self);

/* Whether the store has passed its size cap: new instruments are refused. */
gboolean
venture_series_store_internal_over_cap(
	VentureSeriesStore	 *self,
	gboolean		 *out_over,
	GError			**error
);

/* A venue's id, creating a bare one when it is new. */
gboolean
venture_series_store_internal_venue_ensure(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			  seen_at,
	gint64			 *out_id,
	GError			**error
);

/* An instrument's id from a bare key, creating it unless @allow_new is
 * FALSE (then *out_id stays 0 and nothing is written). */
gboolean
venture_series_store_internal_instrument_ensure(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			  seen_at,
	gboolean		  allow_new,
	gint64			 *out_id,
	gboolean		 *out_created,
	GError			**error
);

gboolean
venture_series_store_internal_check_key(
	const gchar	 *key,
	const gchar	 *what,
	GError		**error
);

gboolean
venture_series_store_internal_check_text(
	const gchar	 *text,
	gsize		  limit,
	const gchar	 *what,
	GError		**error
);

gboolean
venture_series_store_internal_check_attrs(
	const gchar	 *attrs_json,
	GError		**error
);

gboolean
venture_series_store_internal_normalise_currency(
	const gchar	 *currency,
	gchar		  out[VENTURE_MONEY_CURRENCY_LEN],
	GError		**error
);

/* --- The accounts' half, defined in venture-series-accounts.c ------------- */

/*
 * Retention for the account tables, inside the purge's transaction:
 * balance history and the external ledger before day @first_day go, but
 * an account's newest balance in each currency is kept however old it is
 * -- an unchanged purse is still a purse.
 */
gboolean
venture_series_accounts_purge(
	VentureSeriesStore		 *self,
	gint64				  first_day,
	VentureSeriesPurgeResult	 *result,
	GError				**error
);

G_END_DECLS

#endif /* VENTURE_SERIES_STORE_PRIVATE_H */
