/*
 * venture-series-accounts.c - The operator's accounts, in a series store
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The second half of a series store: what the operator owns rather than
 * what the market offers. Hand-written SQL, like the first half, for the
 * same reason (docs/market-data.org, "Why it is not entity tables"), on
 * the same connection, statement cache and transaction counter -- the
 * private header lends them.
 *
 * Nothing here may touch VentureDatabase, the entity registry or the
 * configuration (the writer runs on the feeds worker thread), and nothing
 * may call g_warning(), which the test harness makes fatal from any
 * thread: a failure is a GError for the caller to record.
 */

#include "venture.h"

#ifndef VENTURE_HAVE_SQLITE
#error "The series store needs SQLite; the Makefile leaves it out of SQLITE=0 builds."
#endif

#include <string.h>
#include <math.h>

#include <sqlite3.h>

#include "series/venture-series-store-private.h"

static const gchar *const accounts_kinds[] = {
	"character", "shared", "guild", "other", NULL
};
static const gchar *const accounts_login_kinds[] = {
	"game_account", "platform_account", "other", NULL
};
static const gchar *const accounts_places[] = {
	"bag", "bank", "reagent_bank", "warbank", "guild", "mail", "auction",
	"void", "equipped", "currency", "other", NULL
};
static const gchar *const accounts_txn_kinds[] = {
	"sale", "buy", "income", "expense", "expired", "cancelled", NULL
};
static const gchar *const accounts_txn_groups[] = {
	"day", "week", "month", "account", "venue", "instrument", "source", "login", NULL
};

gboolean
venture_series_txn_group_from_string(
	const gchar		*name,
	VentureSeriesTxnGroup	*out
){
	guint i;

	g_return_val_if_fail(NULL != out, FALSE);

	if (NULL == name)
		return FALSE;

	for (i = 0; NULL != accounts_txn_groups[i]; i++)
	{
		if (0 == strcmp(accounts_txn_groups[i], name))
		{
			*out = (VentureSeriesTxnGroup)i;
			return TRUE;
		}
	}

	return FALSE;
}

/* --- Small helpers ------------------------------------------------------------ */

static gboolean
accounts_check_choice(
	const gchar		 *value,
	const gchar *const	 *choices,
	const gchar		 *what,
	GError			**error
){
	if ((NULL != value) && g_strv_contains(choices, value))
		return TRUE;

	{
		g_autofree gchar *joined = g_strjoinv(", ", (gchar **)choices);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A %s must be one of %s", what, joined);
	}

	return FALSE;
}

/* A time a row may carry: present and not before 1970. */
static gboolean
accounts_check_time(
	gint64		  at,
	const gchar	 *what,
	GError		**error
){
	if ((VENTURE_SERIES_NONE == at) || (at < 0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A %s needs a time, not before 1970", what);
		return FALSE;
	}

	return TRUE;
}

/* An optional amount: none, or zero and up. */
static gboolean
accounts_check_money(
	gint64		  amount,
	const gchar	 *what,
	GError		**error
){
	if ((VENTURE_SERIES_NONE != amount) && (amount < 0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A %s is never negative: the kind says which way money went", what);
		return FALSE;
	}

	return TRUE;
}

/* Escapes LIKE's wildcards and its escape character. */
static gchar *
accounts_like_escape(const gchar *text)
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

/* --- Bound statements built per call ------------------------------------------ */

typedef struct
{
	gboolean	 is_text;
	gchar		*text;
	gint64		 value;
} AccountsBinding;

static void
accounts_binding_clear(gpointer data)
{
	g_free(((AccountsBinding *)data)->text);
}

static GArray *
accounts_bindings_new(void)
{
	GArray *bindings;

	bindings = g_array_new(FALSE, TRUE, sizeof(AccountsBinding));
	g_array_set_clear_func(bindings, accounts_binding_clear);

	return bindings;
}

static void
accounts_bind_add_text(
	GArray		*bindings,
	const gchar	*text
){
	AccountsBinding binding;

	memset(&binding, 0, sizeof(binding));
	binding.is_text = TRUE;
	binding.text = g_strdup(text);
	g_array_append_val(bindings, binding);
}

static void
accounts_bind_add_int(
	GArray	*bindings,
	gint64	 value
){
	AccountsBinding binding;

	memset(&binding, 0, sizeof(binding));
	binding.value = value;
	g_array_append_val(bindings, binding);
}

static SeriesOwnedStmt *
accounts_prepare_bound(
	VentureSeriesStore	 *self,
	const gchar		 *sql,
	GArray			 *bindings,
	GError			**error
){
	sqlite3_stmt *stmt;
	guint i;
	gint rc;

	stmt = NULL;
	rc = sqlite3_prepare_v2(venture_series_store_internal_db(self), sql, -1, &stmt, NULL);

	if (SQLITE_OK != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "preparing a read", error);
		sqlite3_finalize(stmt);
		return NULL;
	}

	for (i = 0; i < bindings->len; i++)
	{
		AccountsBinding *binding = &g_array_index(bindings, AccountsBinding, i);

		if (binding->is_text)
			series_bind_text(stmt, (gint)i + 1, binding->text);
		else
			sqlite3_bind_int64(stmt, (gint)i + 1, binding->value);
	}

	return stmt;
}

/* Zero is the macros' bound; only a test sets anything else. Atomic
 * because the worker's readers and the main thread's read them. */
static gint accounts_max_rows = 0;
static gint accounts_max_accounts = 0;

gint
venture_series_accounts_get_max_rows(void)
{
	gint max_rows = g_atomic_int_get(&accounts_max_rows);

	return (max_rows > 0) ? max_rows : VENTURE_SERIES_MAX_ACCOUNT_ROWS;
}

void
venture_series_accounts_set_max_rows(gint max_rows)
{
	g_atomic_int_set(&accounts_max_rows, MAX(max_rows, 0));
}

gint
venture_series_accounts_get_max_accounts(void)
{
	gint max_accounts = g_atomic_int_get(&accounts_max_accounts);

	return (max_accounts > 0) ? max_accounts : VENTURE_SERIES_MAX_ACCOUNTS;
}

void
venture_series_accounts_set_max_accounts(gint max_accounts)
{
	g_atomic_int_set(&accounts_max_accounts, MAX(max_accounts, 0));
}

/* The page a read asks for: 0 is @fallback, and nothing past the most. */
static guint
accounts_page(
	guint	count,
	guint	fallback
){
	if (0 == count)
		return fallback;

	return MIN(count, (guint)venture_series_accounts_get_max_rows());
}

/*
 * The LIMIT of a read that may be asked for everything. A count under the
 * bound is a page, and a page ends where it ends. No count, or one past
 * the bound, is a read of all of them: it asks for one row more than the
 * bound, so that accounts_check_whole() can tell a whole answer from a
 * cut one. A caller that judges what is missing -- the mirror closes the
 * listing of a position it does not see -- must never be handed a cut
 * answer as if it were whole.
 */
static gint64
accounts_limit(
	guint		 count,
	guint		 bound,
	gboolean	*out_all
){
	if ((0 == count) || (count > bound))
	{
		*out_all = TRUE;
		return (gint64)bound + 1;
	}

	*out_all = FALSE;
	return count;
}

/* Refuses a read of everything that found more than @bound rows. */
static gboolean
accounts_check_whole(
	GPtrArray	 *rows,
	gboolean	  all,
	guint		  bound,
	const gchar	 *what,
	const gchar	 *narrow,
	GError		**error
){
	if (!all || (rows->len <= bound))
		return TRUE;

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "The store has more than %u %s; narrow the read to %s, or page it", bound, what,
	            narrow);
	return FALSE;
}

/* ==========================================================================
 * Writing
 * ========================================================================== */

/* One account the batch touched, and what its snapshot asked for. */
typedef struct
{
	gint64		 id;
	guint		 replace;	/* kinds this batch restates in full */
	guint		 stale;		/* kinds whose snapshot was too old */
	gint64		 at;		/* the snapshot's time */
	GHashTable	*currencies;	/* restated balance currencies */
} AccountsTouched;

static void
accounts_touched_free(gpointer data)
{
	AccountsTouched *touched = data;

	g_clear_pointer(&touched->currencies, g_hash_table_unref);
	g_free(touched);
}

typedef struct
{
	VentureSeriesStore		*store;
	gint64				 fetched_at;
	gchar				 currency[VENTURE_MONEY_CURRENCY_LEN];
	gboolean			 has_currency;
	gboolean			 over_cap;
	GHashTable			*accounts;	/* key -> AccountsTouched */
	GHashTable			*venues;	/* keys ensured */
	GHashTable			*instruments;	/* keys ensured */
	GHashTable			*logins;	/* keys written this batch */
	VentureSeriesAccountResult	*result;
} AccountsApply;

static void
accounts_apply_clear(AccountsApply *apply)
{
	g_clear_pointer(&apply->accounts, g_hash_table_unref);
	g_clear_pointer(&apply->venues, g_hash_table_unref);
	g_clear_pointer(&apply->instruments, g_hash_table_unref);
	g_clear_pointer(&apply->logins, g_hash_table_unref);
}

static gint64
accounts_changes(AccountsApply *apply)
{
	return sqlite3_changes(venture_series_store_internal_db(apply->store));
}

static gboolean
accounts_venue(
	AccountsApply	 *apply,
	const gchar	 *key,
	GError		**error
){
	gint64 id;

	if ((NULL == key) || g_hash_table_contains(apply->venues, key))
		return TRUE;

	if (!venture_series_store_internal_venue_ensure(apply->store, key, apply->fetched_at, &id,
	                                                error))
		return FALSE;

	g_hash_table_add(apply->venues, g_strdup(key));

	return TRUE;
}

/*
 * The instrument a row names, created bare when new so the market pages
 * and promotion know it. One past the size cap is refused and counted;
 * the row is kept all the same -- it names its instrument by key.
 */
static gboolean
accounts_instrument(
	AccountsApply	 *apply,
	const gchar	 *key,
	GError		**error
){
	gboolean created;
	gint64 id;

	if ((NULL == key) || g_hash_table_contains(apply->instruments, key))
		return TRUE;

	if (!venture_series_store_internal_check_key(key, "instrument", error) ||
	    !venture_series_store_internal_instrument_ensure(apply->store, key, apply->fetched_at,
	                                                     !apply->over_cap, &id, &created,
	                                                     error))
		return FALSE;

	if (created)
		apply->result->instruments_new++;
	else if (0 == id)
		apply->result->instruments_refused++;

	g_hash_table_add(apply->instruments, g_strdup(key));

	return TRUE;
}

/* --- Logins ------------------------------------------------------------------- */

static const gchar accounts_sql_login_exists[] =
	"SELECT 1 FROM logins WHERE key = ?1";

static const gchar accounts_sql_upsert_login[] =
	"INSERT INTO logins (key, name, kind, group_key, attrs, first_seen, last_seen)"
	" VALUES (?1, ?2, COALESCE(?3, 'other'), COALESCE(?4, ''), ?5, ?6, ?6)"
	" ON CONFLICT (key) DO UPDATE SET"
	"  name = COALESCE(?2, name),"
	"  kind = COALESCE(?3, kind),"
	"  group_key = COALESCE(?4, group_key),"
	"  attrs = COALESCE(?5, attrs),"
	"  first_seen = min(first_seen, ?6),"
	"  last_seen = max(last_seen, ?6)";

/*
 * Writes a login: described by a login line, or bare when an account
 * names one the store has not heard of -- the way a row naming an unknown
 * account creates it. %NULL members keep what is stored, so an account's
 * bare mention never blanks the name a login line gave. Each key is
 * written once a batch; a second mention would only bump last_seen to
 * the same moment.
 */
static gboolean
accounts_write_login(
	AccountsApply			 *apply,
	const VentureSeriesLogin	 *login,
	GError				**error
){
	g_autoptr(SeriesCachedStmt) lookup = NULL;
	g_autoptr(SeriesCachedStmt) upsert = NULL;
	gboolean known;
	gint rc;

	if (!venture_series_store_internal_check_key(login->key, "login", error) ||
	    !venture_series_store_internal_check_text(login->name, VENTURE_SERIES_MAX_KEY_LENGTH,
	                                              "login name", error) ||
	    !venture_series_store_internal_check_text(login->group_key,
	                                              VENTURE_SERIES_MAX_KEY_LENGTH,
	                                              "login group", error) ||
	    !venture_series_store_internal_check_attrs(login->attrs_json, error))
		return FALSE;

	if ((NULL != login->kind) &&
	    !accounts_check_choice(login->kind, accounts_login_kinds, "login kind", error))
		return FALSE;

	/* A bare mention after the login was written this batch adds
	 * nothing; a described one always writes. */
	if ((NULL == login->name) && (NULL == login->kind) && (NULL == login->group_key) &&
	    (NULL == login->attrs_json) && g_hash_table_contains(apply->logins, login->key))
		return TRUE;

	lookup = venture_series_store_internal_stmt(apply->store, accounts_sql_login_exists, error);
	if (NULL == lookup)
		return FALSE;

	series_bind_text(lookup, 1, login->key);
	rc = sqlite3_step(lookup);
	known = (SQLITE_ROW == rc);

	if ((SQLITE_ROW != rc) && (SQLITE_DONE != rc))
	{
		venture_series_store_internal_sqlite_error(apply->store, rc, "finding a login", error);
		return FALSE;
	}

	g_clear_pointer(&lookup, series_cached_stmt_release);

	upsert = venture_series_store_internal_stmt(apply->store, accounts_sql_upsert_login, error);
	if (NULL == upsert)
		return FALSE;

	series_bind_text(upsert, 1, login->key);
	series_bind_text(upsert, 2, login->name);
	series_bind_text(upsert, 3, login->kind);
	series_bind_text(upsert, 4, login->group_key);
	series_bind_text(upsert, 5, login->attrs_json);
	sqlite3_bind_int64(upsert, 6, apply->fetched_at);

	if (!venture_series_store_internal_step_done(apply->store, upsert, "writing a login", error))
		return FALSE;

	apply->result->rows_written++;

	if (!g_hash_table_contains(apply->logins, login->key))
	{
		apply->result->logins++;
		g_hash_table_add(apply->logins, g_strdup(login->key));
	}

	if (!known)
		apply->result->logins_new++;

	return TRUE;
}

static const gchar accounts_sql_account_id[] =
	"SELECT id FROM accounts WHERE key = ?1";

static const gchar accounts_sql_upsert_account[] =
	"INSERT INTO accounts (key, name, kind, group_key, venue_key, attrs, last_seen,"
	"                      first_seen, synced_at, login)"
	" VALUES (?1, ?2, COALESCE(?3, 'other'), COALESCE(?4, ''), ?5, ?6, ?7, ?8, ?8,"
	"         COALESCE(?9, ''))"
	" ON CONFLICT (key) DO UPDATE SET"
	"  name = COALESCE(?2, name),"
	"  kind = COALESCE(?3, kind),"
	"  group_key = COALESCE(?4, group_key),"
	"  venue_key = COALESCE(?5, venue_key),"
	"  attrs = COALESCE(?6, attrs),"
	"  login = COALESCE(?9, login),"
	"  last_seen = CASE WHEN ?7 IS NULL THEN last_seen"
	"                   WHEN last_seen IS NULL OR ?7 > last_seen THEN ?7"
	"                   ELSE last_seen END,"
	"  first_seen = min(first_seen, ?8),"
	"  synced_at = max(synced_at, ?8)";

/*
 * Writes an account (a bare one when only its key is known) and returns
 * the batch's record of it. last_seen only moves forward: a batch read
 * from an older export must not make a character look idle.
 */
static AccountsTouched *
accounts_write_account(
	AccountsApply			 *apply,
	const VentureSeriesAccount	 *account,
	GError				**error
){
	g_autoptr(SeriesCachedStmt) lookup = NULL;
	g_autoptr(SeriesCachedStmt) upsert = NULL;
	AccountsTouched *touched;
	gint64 id;
	gint rc;

	if (!venture_series_store_internal_check_key(account->key, "account", error) ||
	    !venture_series_store_internal_check_text(account->name, VENTURE_SERIES_MAX_KEY_LENGTH,
	                                              "account name", error) ||
	    !venture_series_store_internal_check_text(account->group_key,
	                                              VENTURE_SERIES_MAX_KEY_LENGTH,
	                                              "account group", error) ||
	    !venture_series_store_internal_check_attrs(account->attrs_json, error) ||
	    !venture_series_store_internal_check_text(account->login_key,
	                                              VENTURE_SERIES_MAX_KEY_LENGTH,
	                                              "account login", error))
		return NULL;

	if ((NULL != account->kind) &&
	    !accounts_check_choice(account->kind, accounts_kinds, "account kind", error))
		return NULL;

	/* The login it names exists before the account points at it, bare
	 * when no login line described it. "" is no login, not a key. */
	if (!venture_string_is_empty(account->login_key))
	{
		VentureSeriesLogin bare;

		memset(&bare, 0, sizeof(bare));
		bare.key = account->login_key;

		if (!accounts_write_login(apply, &bare, error))
			return NULL;
	}

	if ((NULL != account->venue_key) &&
	    (!venture_series_store_internal_check_key(account->venue_key, "venue", error) ||
	     !accounts_venue(apply, account->venue_key, error)))
		return NULL;

	lookup = venture_series_store_internal_stmt(apply->store, accounts_sql_account_id, error);
	if (NULL == lookup)
		return NULL;

	series_bind_text(lookup, 1, account->key);
	rc = sqlite3_step(lookup);
	id = (SQLITE_ROW == rc) ? sqlite3_column_int64(lookup, 0) : 0;

	if ((SQLITE_ROW != rc) && (SQLITE_DONE != rc))
	{
		venture_series_store_internal_sqlite_error(apply->store, rc, "finding an account", error);
		return NULL;
	}

	g_clear_pointer(&lookup, series_cached_stmt_release);

	upsert = venture_series_store_internal_stmt(apply->store, accounts_sql_upsert_account, error);
	if (NULL == upsert)
		return NULL;

	series_bind_text(upsert, 1, account->key);
	series_bind_text(upsert, 2, account->name);
	series_bind_text(upsert, 3, account->kind);
	series_bind_text(upsert, 4, account->group_key);
	series_bind_text(upsert, 5, account->venue_key);
	series_bind_text(upsert, 6, account->attrs_json);
	series_bind_figure(upsert, 7, account->last_seen);
	sqlite3_bind_int64(upsert, 8, apply->fetched_at);
	series_bind_text(upsert, 9, account->login_key);

	if (!venture_series_store_internal_step_done(apply->store, upsert, "writing an account", error))
		return NULL;

	apply->result->rows_written++;

	if (0 == id)
	{
		id = sqlite3_last_insert_rowid(venture_series_store_internal_db(apply->store));
		apply->result->accounts_new++;
	}

	touched = g_hash_table_lookup(apply->accounts, account->key);

	if (NULL == touched)
	{
		touched = g_new0(AccountsTouched, 1);
		touched->at = VENTURE_SERIES_NONE;
		g_hash_table_insert(apply->accounts, g_strdup(account->key), touched);
	}

	touched->id = id;

	return touched;
}

/* The batch's record of an account a row names: written bare when the
 * batch did not describe it. */
static AccountsTouched *
accounts_touch(
	AccountsApply	 *apply,
	const gchar	 *key,
	GError		**error
){
	VentureSeriesAccount bare;
	AccountsTouched *touched;

	touched = (NULL != key) ? g_hash_table_lookup(apply->accounts, key) : NULL;

	if (NULL != touched)
		return touched;

	memset(&bare, 0, sizeof(bare));
	bare.key = key;
	bare.last_seen = VENTURE_SERIES_NONE;

	return accounts_write_account(apply, &bare, error);
}

/* --- Snapshots ---------------------------------------------------------------- */

static const gchar accounts_sql_snapshot_times[] =
	"SELECT holdings_at, positions_at, inbound_at, balances_at FROM accounts WHERE id = ?1";

static const gchar accounts_sql_set_holdings_at[] =
	"UPDATE accounts SET holdings_at = ?2 WHERE id = ?1";
static const gchar accounts_sql_set_positions_at[] =
	"UPDATE accounts SET positions_at = ?2 WHERE id = ?1";
static const gchar accounts_sql_set_inbound_at[] =
	"UPDATE accounts SET inbound_at = ?2 WHERE id = ?1";
static const gchar accounts_sql_set_balances_at[] =
	"UPDATE accounts SET balances_at = ?2 WHERE id = ?1";

/*
 * Mark and sweep. A replaced kind's rows are stamped with -1, a time no
 * real row takes (times are refused before 1970); each row the batch
 * restates is written over its stamp; whatever still carries -1 after
 * the rows is what the source no longer has. A delete-then-insert would
 * be simpler and would lose a listing's first_seen -- its age.
 */
static const gchar accounts_sql_mark_holdings[] =
	"UPDATE account_holdings SET at = -1 WHERE account_id = ?1";
static const gchar accounts_sql_mark_positions[] =
	"UPDATE account_positions SET last_seen = -1 WHERE account_id = ?1";
static const gchar accounts_sql_mark_inbound[] =
	"UPDATE account_inbound SET last_seen = -1 WHERE account_id = ?1";

static const gchar accounts_sql_sweep_holdings[] =
	"DELETE FROM account_holdings WHERE account_id = ?1 AND at = -1";
static const gchar accounts_sql_sweep_positions[] =
	"DELETE FROM account_positions WHERE account_id = ?1 AND last_seen = -1";
static const gchar accounts_sql_sweep_inbound[] =
	"DELETE FROM account_inbound WHERE account_id = ?1 AND last_seen = -1";

static gboolean
accounts_exec_id(
	AccountsApply	 *apply,
	const gchar	 *sql,
	gint64		  id,
	gint64		  value,
	gboolean	  with_value,
	const gchar	 *what,
	GError		**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	stmt = venture_series_store_internal_stmt(apply->store, sql, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, id);

	if (with_value)
		sqlite3_bind_int64(stmt, 2, value);

	return venture_series_store_internal_step_done(apply->store, stmt, what, error);
}

/*
 * Judges one snapshot against the times each kind was last replaced:
 * a kind it covers is replaced unless the snapshot is older than the one
 * that last replaced it -- an export read late must not roll an account
 * back. The kinds it will replace are marked now; the rows that follow
 * clear the mark, and the sweep after them removes what kept it.
 */
static gboolean
accounts_begin_snapshot(
	AccountsApply				 *apply,
	const VentureSeriesAccountSnapshot	 *snapshot,
	GError					**error
){
	static const guint kinds[] = {
		VENTURE_SERIES_COVERS_HOLDINGS, VENTURE_SERIES_COVERS_POSITIONS,
		VENTURE_SERIES_COVERS_INBOUND, VENTURE_SERIES_COVERS_BALANCES
	};
	static const gchar *const set_sql[] = {
		accounts_sql_set_holdings_at, accounts_sql_set_positions_at,
		accounts_sql_set_inbound_at, accounts_sql_set_balances_at
	};
	static const gchar *const mark_sql[] = {
		accounts_sql_mark_holdings, accounts_sql_mark_positions,
		accounts_sql_mark_inbound, NULL
	};
	g_autoptr(SeriesCachedStmt) times = NULL;
	AccountsTouched *touched;
	gint64 stored[4];
	guint i;
	gint rc;

	if (!accounts_check_time(snapshot->at, "account snapshot", error))
		return FALSE;

	if ((0 == (snapshot->covers & 0xf)) || (0 != (snapshot->covers & ~0xfu)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An account snapshot covers one or more of holdings, positions, "
		                    "inbound and balances");
		return FALSE;
	}

	touched = accounts_touch(apply, snapshot->account_key, error);

	if (NULL == touched)
		return FALSE;

	if (0 != (touched->replace | touched->stale))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A batch snapshots account \"%s\" at most once", snapshot->account_key);
		return FALSE;
	}

	times = venture_series_store_internal_stmt(apply->store, accounts_sql_snapshot_times, error);
	if (NULL == times)
		return FALSE;

	sqlite3_bind_int64(times, 1, touched->id);
	rc = sqlite3_step(times);

	if (SQLITE_ROW != rc)
	{
		venture_series_store_internal_sqlite_error(apply->store, rc, "reading an account", error);
		return FALSE;
	}

	for (i = 0; i < 4; i++)
		stored[i] = series_column_figure(times, (gint)i);

	g_clear_pointer(&times, series_cached_stmt_release);
	touched->at = snapshot->at;

	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		if (0 == (snapshot->covers & kinds[i]))
			continue;

		if ((VENTURE_SERIES_NONE != stored[i]) && (snapshot->at < stored[i]))
		{
			touched->stale |= kinds[i];
			continue;
		}

		touched->replace |= kinds[i];

		if (!accounts_exec_id(apply, set_sql[i], touched->id, snapshot->at, TRUE,
		                      "recording a snapshot", error))
			return FALSE;

		if ((NULL != mark_sql[i]) &&
		    !accounts_exec_id(apply, mark_sql[i], touched->id, 0, FALSE,
		                      "marking what a snapshot replaces", error))
			return FALSE;
	}

	if (0 != touched->stale)
		apply->result->stale++;

	if (0 != (touched->replace & VENTURE_SERIES_COVERS_BALANCES))
		touched->currencies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	return TRUE;
}

/*
 * Whether a row of @kind for an account goes in: not when its account's
 * snapshot of that kind was stale. @out_replaced says whether it is part
 * of a replacement (TRUE) or an upsert on its own (FALSE).
 */
static AccountsTouched *
accounts_row_account(
	AccountsApply	 *apply,
	const gchar	 *account_key,
	guint		  kind,
	gboolean	 *out_skip,
	GError		**error
){
	AccountsTouched *touched;

	*out_skip = FALSE;
	touched = accounts_touch(apply, account_key, error);

	if ((NULL != touched) && (0 != (touched->stale & kind)))
		*out_skip = TRUE;

	return touched;
}

/* --- Balances ------------------------------------------------------------------- */

static const gchar accounts_sql_balance_prev[] =
	"SELECT amount FROM account_balances"
	" WHERE account_id = ?1 AND currency = ?2 AND at < ?3"
	" ORDER BY at DESC LIMIT 1";
static const gchar accounts_sql_balance_same[] =
	"SELECT amount FROM account_balances"
	" WHERE account_id = ?1 AND currency = ?2 AND at = ?3";
static const gchar accounts_sql_balance_next[] =
	"SELECT at, amount FROM account_balances"
	" WHERE account_id = ?1 AND currency = ?2 AND at > ?3"
	" ORDER BY at ASC LIMIT 1";
static const gchar accounts_sql_balance_insert[] =
	"INSERT INTO account_balances (account_id, currency, at, amount)"
	" VALUES (?1, ?2, ?3, ?4)";
static const gchar accounts_sql_balance_update[] =
	"UPDATE account_balances SET amount = ?4"
	" WHERE account_id = ?1 AND currency = ?2 AND at = ?3";
static const gchar accounts_sql_balance_delete[] =
	"DELETE FROM account_balances WHERE account_id = ?1 AND currency = ?2 AND at = ?3";

/* Reads one neighbour: TRUE with *found FALSE when there is none. */
static gboolean
accounts_balance_neighbour(
	AccountsApply	 *apply,
	const gchar	 *sql,
	gint64		  account_id,
	const gchar	 *currency,
	gint64		  at,
	gboolean	 *out_found,
	gint64		 *out_at,
	gint64		 *out_amount,
	GError		**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint rc;

	*out_found = FALSE;

	stmt = venture_series_store_internal_stmt(apply->store, sql, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, account_id);
	series_bind_text(stmt, 2, currency);
	sqlite3_bind_int64(stmt, 3, at);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW == rc)
	{
		*out_found = TRUE;

		if (2 == sqlite3_column_count(stmt))
		{
			*out_at = sqlite3_column_int64(stmt, 0);
			*out_amount = sqlite3_column_int64(stmt, 1);
		}
		else
		{
			*out_at = at;
			*out_amount = sqlite3_column_int64(stmt, 0);
		}

		return TRUE;
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(apply->store, rc, "reading a balance", error);
		return FALSE;
	}

	return TRUE;
}

static gboolean
accounts_balance_write(
	AccountsApply	 *apply,
	const gchar	 *sql,
	gint64		  account_id,
	const gchar	 *currency,
	gint64		  at,
	gint64		  amount,
	GError		**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;

	stmt = venture_series_store_internal_stmt(apply->store, sql, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, account_id);
	series_bind_text(stmt, 2, currency);
	sqlite3_bind_int64(stmt, 3, at);

	if (sql != accounts_sql_balance_delete)
		sqlite3_bind_int64(stmt, 4, amount);

	if (!venture_series_store_internal_step_done(apply->store, stmt, "writing a balance", error))
		return FALSE;

	apply->result->rows_written += accounts_changes(apply);

	return TRUE;
}

/*
 * One point of balance history, kept only where it is a change: a value
 * equal to the one before it is not stored, and a later point it makes a
 * repeat of is removed. A balance read every five minutes for a month is
 * then as many rows as times the purse actually moved -- and a point
 * that arrives late, between two others, leaves the history as it would
 * have been had it arrived in order.
 */
static gboolean
accounts_balance_put(
	AccountsApply	 *apply,
	gint64		  account_id,
	const gchar	 *currency,
	gint64		  at,
	gint64		  amount,
	GError		**error
){
	gboolean has_prev;
	gboolean has_same;
	gboolean has_next;
	gint64 prev_amount = 0;
	gint64 same_amount = 0;
	gint64 next_amount = 0;
	gint64 next_at = 0;
	gint64 ignored = 0;

	if (!accounts_balance_neighbour(apply, accounts_sql_balance_prev, account_id, currency, at,
	                                &has_prev, &ignored, &prev_amount, error) ||
	    !accounts_balance_neighbour(apply, accounts_sql_balance_same, account_id, currency, at,
	                                &has_same, &ignored, &same_amount, error) ||
	    !accounts_balance_neighbour(apply, accounts_sql_balance_next, account_id, currency, at,
	                                &has_next, &next_at, &next_amount, error))
		return FALSE;

	if (has_same && (same_amount == amount))
	{
		apply->result->balances_unchanged++;
		return TRUE;
	}

	if (has_prev && (prev_amount == amount))
	{
		/* A repeat of what came before. A point already at this
		 * moment saying otherwise was the change; now it is not. */
		if (has_same &&
		    !accounts_balance_write(apply, accounts_sql_balance_delete, account_id, currency,
		                            at, 0, error))
			return FALSE;

		if (!has_same)
		{
			apply->result->balances_unchanged++;
			return TRUE;
		}
	}
	else if (!accounts_balance_write(apply, has_same ? accounts_sql_balance_update
	                                                 : accounts_sql_balance_insert,
	                                 account_id, currency, at, amount, error))
		return FALSE;

	/* The next point now repeats this one: it is no longer a change. */
	if (has_next && (next_amount == amount) &&
	    !accounts_balance_write(apply, accounts_sql_balance_delete, account_id, currency,
	                            next_at, 0, error))
		return FALSE;

	apply->result->balances++;

	return TRUE;
}

static const gchar accounts_sql_latest_balances[] =
	"SELECT b.currency, b.amount, b.at FROM account_balances b"
	" WHERE b.account_id = ?1"
	"   AND b.at = (SELECT MAX(b2.at) FROM account_balances b2"
	"               WHERE b2.account_id = b.account_id AND b2.currency = b.currency)"
	" ORDER BY b.currency";

/*
 * A snapshot covering balances restates every currency the account has:
 * one it had money in and did not restate is empty now. History is not
 * deleted -- a zero is appended at the snapshot's time.
 */
static gboolean
accounts_sweep_balances(
	AccountsApply		 *apply,
	AccountsTouched		 *touched,
	GError			**error
){
	g_autoptr(GPtrArray) emptied = NULL;
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	guint i;
	gint rc;

	emptied = g_ptr_array_new_with_free_func(g_free);
	stmt = venture_series_store_internal_stmt(apply->store, accounts_sql_latest_balances, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, touched->id);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		const gchar *currency = (const gchar *)sqlite3_column_text(stmt, 0);

		if ((NULL != currency) && (0 != sqlite3_column_int64(stmt, 1)) &&
		    (sqlite3_column_int64(stmt, 2) <= touched->at) &&
		    !g_hash_table_contains(touched->currencies, currency))
			g_ptr_array_add(emptied, g_strdup(currency));
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(apply->store, rc, "reading balances", error);
		return FALSE;
	}

	g_clear_pointer(&stmt, series_cached_stmt_release);

	for (i = 0; i < emptied->len; i++)
	{
		if (!accounts_balance_put(apply, touched->id, g_ptr_array_index(emptied, i),
		                          touched->at, 0, error))
			return FALSE;

		apply->result->removed++;
	}

	return TRUE;
}

static gboolean
accounts_apply_balances(
	AccountsApply			 *apply,
	const VentureSeriesAccountBatch	 *batch,
	GError				**error
){
	guint i;

	for (i = 0; i < batch->n_balances; i++)
	{
		const VentureSeriesBalance *balance = &batch->balances[i];
		gchar currency[VENTURE_MONEY_CURRENCY_LEN];
		AccountsTouched *touched;
		gboolean skip;
		gint64 at;

		if (!venture_series_store_internal_normalise_currency(balance->currency, currency,
		                                                      error))
			return FALSE;

		if ((VENTURE_SERIES_NONE == balance->amount) || (balance->amount < 0))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A balance is an amount of zero or more");
			return FALSE;
		}

		touched = accounts_row_account(apply, balance->account_key,
		                               VENTURE_SERIES_COVERS_BALANCES, &skip, error);

		if (NULL == touched)
			return FALSE;

		if (skip)
			continue;

		/*
		 * A balance with no time of its own is as of the snapshot that
		 * restates it, like a position or a holding: the sweep judges a
		 * currency's newest point against the snapshot's time, and a
		 * restated balance stamped with the later push time would look
		 * newer than every later snapshot that leaves its currency out --
		 * so an emptied purse would never read zero.
		 */
		if (VENTURE_SERIES_NONE != balance->at)
			at = balance->at;
		else if (0 != (touched->replace & VENTURE_SERIES_COVERS_BALANCES))
			at = touched->at;
		else
			at = apply->fetched_at;

		if (!accounts_check_time(at, "balance", error))
			return FALSE;

		if (0 != (touched->replace & VENTURE_SERIES_COVERS_BALANCES))
			g_hash_table_add(touched->currencies, g_strdup(currency));

		if (!accounts_balance_put(apply, touched->id, currency, at, balance->amount, error))
			return FALSE;
	}

	return TRUE;
}

/* --- Holdings ------------------------------------------------------------------- */

static const gchar accounts_sql_upsert_holding[] =
	"INSERT INTO account_holdings (account_id, place, instrument_key, quantity, at)"
	" VALUES (?1, ?2, ?3, ?4, ?5)"
	" ON CONFLICT (account_id, place, instrument_key) DO UPDATE SET"
	"  quantity = excluded.quantity, at = excluded.at";
static const gchar accounts_sql_delete_holding[] =
	"DELETE FROM account_holdings"
	" WHERE account_id = ?1 AND place = ?2 AND instrument_key = ?3";

static gboolean
accounts_apply_holdings(
	AccountsApply			 *apply,
	const VentureSeriesAccountBatch	 *batch,
	GError				**error
){
	guint i;

	for (i = 0; i < batch->n_holdings; i++)
	{
		const VentureSeriesHolding *holding = &batch->holdings[i];
		g_autoptr(SeriesCachedStmt) stmt = NULL;
		AccountsTouched *touched;
		gboolean replacing;
		gboolean skip;
		gint64 at;

		if (!accounts_check_choice(holding->place, accounts_places, "place", error) ||
		    !venture_series_store_internal_check_key(holding->instrument_key, "instrument",
		                                             error))
			return FALSE;

		if (holding->quantity < 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A holding's quantity is zero or more");
			return FALSE;
		}

		touched = accounts_row_account(apply, holding->account_key,
		                               VENTURE_SERIES_COVERS_HOLDINGS, &skip, error);

		if (NULL == touched)
			return FALSE;

		if (skip)
			continue;

		replacing = (0 != (touched->replace & VENTURE_SERIES_COVERS_HOLDINGS));
		at = (VENTURE_SERIES_NONE != holding->at) ? holding->at
		     : (replacing ? touched->at : apply->fetched_at);

		if (!accounts_check_time(at, "holding", error))
			return FALSE;

		/* Zero is "not held": inside a replacement it is simply not
		 * restated, outside one it removes the line. */
		if (0 == holding->quantity)
		{
			if (replacing)
				continue;

			stmt = venture_series_store_internal_stmt(apply->store, accounts_sql_delete_holding,
			                                          error);
			if (NULL == stmt)
				return FALSE;

			sqlite3_bind_int64(stmt, 1, touched->id);
			series_bind_text(stmt, 2, holding->place);
			series_bind_text(stmt, 3, holding->instrument_key);

			if (!venture_series_store_internal_step_done(apply->store, stmt,
			                                             "removing a holding", error))
				return FALSE;

			apply->result->removed += accounts_changes(apply);
			apply->result->rows_written += accounts_changes(apply);
			continue;
		}

		if (!accounts_instrument(apply, holding->instrument_key, error))
			return FALSE;

		stmt = venture_series_store_internal_stmt(apply->store, accounts_sql_upsert_holding, error);
		if (NULL == stmt)
			return FALSE;

		sqlite3_bind_int64(stmt, 1, touched->id);
		series_bind_text(stmt, 2, holding->place);
		series_bind_text(stmt, 3, holding->instrument_key);
		sqlite3_bind_int64(stmt, 4, holding->quantity);
		sqlite3_bind_int64(stmt, 5, at);

		if (!venture_series_store_internal_step_done(apply->store, stmt, "writing a holding",
		                                             error))
			return FALSE;

		apply->result->holdings++;
		apply->result->rows_written++;
	}

	return TRUE;
}

/* --- Positions -------------------------------------------------------------------- */

/*
 * first_seen only moves back and posted_at is kept when a later restating
 * leaves it out: a listing's age is the one fact about it the source may
 * stop repeating.
 */
static const gchar accounts_sql_upsert_position[] =
	"INSERT INTO account_positions (key, account_id, venue_key, instrument_key, quantity,"
	"                               unit_price, bid, currency, expires_at, posted_at,"
	"                               first_seen, last_seen)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?11)"
	" ON CONFLICT (key) DO UPDATE SET"
	"  account_id = excluded.account_id,"
	"  venue_key = excluded.venue_key,"
	"  instrument_key = excluded.instrument_key,"
	"  quantity = excluded.quantity,"
	"  unit_price = excluded.unit_price,"
	"  bid = excluded.bid,"
	"  currency = excluded.currency,"
	"  expires_at = excluded.expires_at,"
	"  posted_at = COALESCE(excluded.posted_at, posted_at),"
	"  first_seen = min(first_seen, excluded.first_seen),"
	"  last_seen = max(last_seen, excluded.last_seen)";

static gboolean
accounts_apply_positions(
	AccountsApply			 *apply,
	const VentureSeriesAccountBatch	 *batch,
	GError				**error
){
	guint i;

	if ((batch->n_positions > 0) && !apply->has_currency)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Positions carry prices, and the batch names no currency for them");
		return FALSE;
	}

	for (i = 0; i < batch->n_positions; i++)
	{
		const VentureSeriesPosition *position = &batch->positions[i];
		g_autoptr(SeriesCachedStmt) stmt = NULL;
		AccountsTouched *touched;
		gboolean skip;
		gint64 at;

		if (!venture_series_store_internal_check_key(position->key, "position", error) ||
		    !venture_series_store_internal_check_key(position->venue_key, "venue", error) ||
		    !venture_series_store_internal_check_key(position->instrument_key, "instrument",
		                                             error))
			return FALSE;

		if ((position->quantity < 1) || (VENTURE_SERIES_NONE == position->unit_price) ||
		    (position->unit_price < 0) || !accounts_check_money(position->bid, "bid", error))
		{
			if ((NULL != error) && (NULL == *error))
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				                    "A position needs a quantity of one or more and a "
				                    "price of zero or more");
			return FALSE;
		}

		touched = accounts_row_account(apply, position->account_key,
		                               VENTURE_SERIES_COVERS_POSITIONS, &skip, error);

		if (NULL == touched)
			return FALSE;

		if (skip)
			continue;

		at = (VENTURE_SERIES_NONE != position->at) ? position->at
		     : ((0 != (touched->replace & VENTURE_SERIES_COVERS_POSITIONS)) ? touched->at
		                                                                   : apply->fetched_at);

		if (!accounts_check_time(at, "position", error) ||
		    !accounts_venue(apply, position->venue_key, error) ||
		    !accounts_instrument(apply, position->instrument_key, error))
			return FALSE;

		stmt = venture_series_store_internal_stmt(apply->store, accounts_sql_upsert_position,
		                                          error);
		if (NULL == stmt)
			return FALSE;

		series_bind_text(stmt, 1, position->key);
		sqlite3_bind_int64(stmt, 2, touched->id);
		series_bind_text(stmt, 3, position->venue_key);
		series_bind_text(stmt, 4, position->instrument_key);
		sqlite3_bind_int64(stmt, 5, position->quantity);
		sqlite3_bind_int64(stmt, 6, position->unit_price);
		series_bind_figure(stmt, 7, position->bid);
		series_bind_text(stmt, 8, apply->currency);
		series_bind_figure(stmt, 9, position->expires_at);
		series_bind_figure(stmt, 10, position->posted_at);
		sqlite3_bind_int64(stmt, 11, at);

		if (!venture_series_store_internal_step_done(apply->store, stmt, "writing a position",
		                                             error))
			return FALSE;

		apply->result->positions++;
		apply->result->rows_written++;
	}

	return TRUE;
}

/* --- Inbound ------------------------------------------------------------------------ */

static const gchar accounts_sql_upsert_inbound[] =
	"INSERT INTO account_inbound (key, account_id, sender, subject, money, cod, currency,"
	"                             instrument_key, quantity, expires_at, returned,"
	"                             first_seen, last_seen)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?12)"
	" ON CONFLICT (key) DO UPDATE SET"
	"  account_id = excluded.account_id,"
	"  sender = excluded.sender,"
	"  subject = excluded.subject,"
	"  money = excluded.money,"
	"  cod = excluded.cod,"
	"  currency = excluded.currency,"
	"  instrument_key = excluded.instrument_key,"
	"  quantity = excluded.quantity,"
	"  expires_at = excluded.expires_at,"
	"  returned = excluded.returned,"
	"  first_seen = min(first_seen, excluded.first_seen),"
	"  last_seen = max(last_seen, excluded.last_seen)";

static gboolean
accounts_apply_inbound(
	AccountsApply			 *apply,
	const VentureSeriesAccountBatch	 *batch,
	GError				**error
){
	guint i;

	for (i = 0; i < batch->n_inbound; i++)
	{
		const VentureSeriesInbound *inbound = &batch->inbound[i];
		g_autoptr(SeriesCachedStmt) stmt = NULL;
		AccountsTouched *touched;
		gboolean has_money;
		gboolean skip;
		gint64 at;

		if (!venture_series_store_internal_check_key(inbound->key, "inbound", error) ||
		    !venture_series_store_internal_check_text(inbound->sender,
		                                              VENTURE_SERIES_MAX_KEY_LENGTH,
		                                              "sender", error) ||
		    !venture_series_store_internal_check_text(inbound->subject,
		                                              VENTURE_SERIES_MAX_KEY_LENGTH,
		                                              "subject", error) ||
		    !accounts_check_money(inbound->money, "mail's money", error) ||
		    !accounts_check_money(inbound->cod, "cash on delivery", error))
			return FALSE;

		if ((NULL != inbound->instrument_key) &&
		    (!venture_series_store_internal_check_key(inbound->instrument_key, "instrument",
		                                              error) ||
		     (inbound->quantity < 1)))
		{
			if ((NULL != error) && (NULL == *error))
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				                    "An item in the mail needs a quantity of one or more");
			return FALSE;
		}

		has_money = (VENTURE_SERIES_NONE != inbound->money) ||
		            (VENTURE_SERIES_NONE != inbound->cod);

		if (has_money && !apply->has_currency)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Money in the mail needs a currency, and the batch names none");
			return FALSE;
		}

		touched = accounts_row_account(apply, inbound->account_key,
		                               VENTURE_SERIES_COVERS_INBOUND, &skip, error);

		if (NULL == touched)
			return FALSE;

		if (skip)
			continue;

		at = (VENTURE_SERIES_NONE != inbound->at) ? inbound->at
		     : ((0 != (touched->replace & VENTURE_SERIES_COVERS_INBOUND)) ? touched->at
		                                                                 : apply->fetched_at);

		if (!accounts_check_time(at, "inbound row", error) ||
		    !accounts_instrument(apply, inbound->instrument_key, error))
			return FALSE;

		stmt = venture_series_store_internal_stmt(apply->store, accounts_sql_upsert_inbound,
		                                          error);
		if (NULL == stmt)
			return FALSE;

		series_bind_text(stmt, 1, inbound->key);
		sqlite3_bind_int64(stmt, 2, touched->id);
		series_bind_text(stmt, 3, inbound->sender);
		series_bind_text(stmt, 4, inbound->subject);
		series_bind_figure(stmt, 5, inbound->money);
		series_bind_figure(stmt, 6, inbound->cod);
		series_bind_text(stmt, 7, apply->has_currency ? apply->currency : NULL);
		series_bind_text(stmt, 8, inbound->instrument_key);
		series_bind_figure(stmt, 9, (NULL != inbound->instrument_key) ? inbound->quantity
		                                                              : VENTURE_SERIES_NONE);
		series_bind_figure(stmt, 10, inbound->expires_at);
		sqlite3_bind_int(stmt, 11, inbound->returned ? 1 : 0);
		sqlite3_bind_int64(stmt, 12, at);

		if (!venture_series_store_internal_step_done(apply->store, stmt, "writing inbound",
		                                             error))
			return FALSE;

		apply->result->inbound++;
		apply->result->rows_written++;
	}

	return TRUE;
}

/* --- The external ledger -------------------------------------------------------------- */

static const gchar accounts_sql_insert_txn[] =
	"INSERT INTO external_txns (key, account_id, venue_key, kind, instrument_key, quantity,"
	"                           unit_price, amount, currency, counterparty, source, at,"
	"                           first_seen, updated_at)"
	" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?13)"
	" ON CONFLICT (key) DO NOTHING";

/* Only a row that differs is rewritten, so a full re-export of a ledger
 * the store already has is a few thousand index probes and no writes. */
static const gchar accounts_sql_update_txn[] =
	"UPDATE external_txns SET"
	"  account_id = ?2, venue_key = ?3, kind = ?4, instrument_key = ?5, quantity = ?6,"
	"  unit_price = ?7, amount = ?8, currency = ?9, counterparty = ?10, source = ?11,"
	"  at = ?12, updated_at = ?13"
	" WHERE key = ?1"
	"   AND (account_id IS NOT ?2 OR venue_key IS NOT ?3 OR kind IS NOT ?4"
	"        OR instrument_key IS NOT ?5 OR quantity IS NOT ?6 OR unit_price IS NOT ?7"
	"        OR amount IS NOT ?8 OR currency IS NOT ?9 OR counterparty IS NOT ?10"
	"        OR source IS NOT ?11 OR at IS NOT ?12)";

static void
accounts_bind_txn(
	sqlite3_stmt		*stmt,
	const VentureSeriesTxn	*txn,
	gint64			 account_id,
	const gchar		*currency,
	gint64			 fetched_at
){
	series_bind_text(stmt, 1, txn->key);
	sqlite3_bind_int64(stmt, 2, account_id);
	series_bind_text(stmt, 3, txn->venue_key);
	series_bind_text(stmt, 4, txn->kind);
	series_bind_text(stmt, 5, txn->instrument_key);
	series_bind_figure(stmt, 6, txn->quantity);
	series_bind_figure(stmt, 7, txn->unit_price);
	series_bind_figure(stmt, 8, txn->amount);
	series_bind_text(stmt, 9, currency);
	series_bind_text(stmt, 10, txn->counterparty);
	series_bind_text(stmt, 11, txn->source);
	sqlite3_bind_int64(stmt, 12, txn->at);
	sqlite3_bind_int64(stmt, 13, fetched_at);
}

static gboolean
accounts_apply_txns(
	AccountsApply			 *apply,
	const VentureSeriesAccountBatch	 *batch,
	GError				**error
){
	guint i;

	for (i = 0; i < batch->n_txns; i++)
	{
		const VentureSeriesTxn *txn = &batch->txns[i];
		g_autoptr(SeriesCachedStmt) stmt = NULL;
		AccountsTouched *touched;
		gboolean has_money;

		if (!venture_series_store_internal_check_key(txn->key, "ledger row", error) ||
		    !accounts_check_choice(txn->kind, accounts_txn_kinds, "ledger row's kind", error) ||
		    !accounts_check_time(txn->at, "ledger row", error) ||
		    !accounts_check_money(txn->amount, "ledger amount", error) ||
		    !accounts_check_money(txn->unit_price, "ledger unit price", error) ||
		    !venture_series_store_internal_check_text(txn->counterparty,
		                                              VENTURE_SERIES_MAX_KEY_LENGTH,
		                                              "counterparty", error) ||
		    !venture_series_store_internal_check_text(txn->source,
		                                              VENTURE_SERIES_MAX_KEY_LENGTH,
		                                              "ledger source", error))
			return FALSE;

		if (((NULL != txn->venue_key) &&
		     !venture_series_store_internal_check_key(txn->venue_key, "venue", error)) ||
		    ((NULL != txn->instrument_key) &&
		     !venture_series_store_internal_check_key(txn->instrument_key, "instrument", error)))
			return FALSE;

		if ((VENTURE_SERIES_NONE != txn->quantity) && (txn->quantity < 1))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A ledger row's quantity is one or more");
			return FALSE;
		}

		has_money = (VENTURE_SERIES_NONE != txn->amount) ||
		            (VENTURE_SERIES_NONE != txn->unit_price);

		if (has_money && !apply->has_currency)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Ledger money needs a currency, and the batch names none");
			return FALSE;
		}

		touched = accounts_touch(apply, txn->account_key, error);

		if ((NULL == touched) ||
		    !accounts_venue(apply, txn->venue_key, error) ||
		    !accounts_instrument(apply, txn->instrument_key, error))
			return FALSE;

		stmt = venture_series_store_internal_stmt(apply->store, accounts_sql_insert_txn, error);
		if (NULL == stmt)
			return FALSE;

		/* Every row of a source with a currency carries it, money or
		 * not: an expired auction belongs in the same bucket as the
		 * sales beside it, not in one of its own. */
		accounts_bind_txn(stmt, txn, touched->id, apply->has_currency ? apply->currency : NULL,
		                  apply->fetched_at);

		if (!venture_series_store_internal_step_done(apply->store, stmt, "writing the ledger",
		                                             error))
			return FALSE;

		if (accounts_changes(apply) > 0)
		{
			apply->result->txns_new++;
			apply->result->rows_written++;
			continue;
		}

		g_clear_pointer(&stmt, series_cached_stmt_release);
		stmt = venture_series_store_internal_stmt(apply->store, accounts_sql_update_txn, error);
		if (NULL == stmt)
			return FALSE;

		accounts_bind_txn(stmt, txn, touched->id, apply->has_currency ? apply->currency : NULL,
		                  apply->fetched_at);

		if (!venture_series_store_internal_step_done(apply->store, stmt, "updating the ledger",
		                                             error))
			return FALSE;

		if (accounts_changes(apply) > 0)
		{
			apply->result->txns_updated++;
			apply->result->rows_written++;
		}
		else
			apply->result->txns_unchanged++;
	}

	return TRUE;
}

/* --- Applying a batch ------------------------------------------------------------- */

static gboolean
accounts_sweep(
	AccountsApply	 *apply,
	GError		**error
){
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, apply->accounts);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		static const guint kinds[] = {
			VENTURE_SERIES_COVERS_HOLDINGS, VENTURE_SERIES_COVERS_POSITIONS,
			VENTURE_SERIES_COVERS_INBOUND
		};
		static const gchar *const sweep_sql[] = {
			accounts_sql_sweep_holdings, accounts_sql_sweep_positions,
			accounts_sql_sweep_inbound
		};
		AccountsTouched *touched = value;
		guint i;

		for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		{
			if (0 == (touched->replace & kinds[i]))
				continue;

			if (!accounts_exec_id(apply, sweep_sql[i], touched->id, 0, FALSE,
			                      "removing what a snapshot did not restate", error))
				return FALSE;

			apply->result->removed += accounts_changes(apply);
			apply->result->rows_written += accounts_changes(apply);
		}

		if ((0 != (touched->replace & VENTURE_SERIES_COVERS_BALANCES)) &&
		    !accounts_sweep_balances(apply, touched, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
venture_series_store_apply_accounts(
	VentureSeriesStore			 *self,
	const VentureSeriesAccountBatch		 *batch,
	gint64					  fetched_at,
	VentureSeriesAccountResult		 *out,
	GError					**error
){
	VentureSeriesAccountResult result;
	AccountsApply apply;
	gboolean ok;
	guint i;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != batch, FALSE);

	memset(&result, 0, sizeof(result));
	memset(&apply, 0, sizeof(apply));

	if (!accounts_check_time(fetched_at, "batch", error))
		return FALSE;

	apply.store = self;
	apply.fetched_at = fetched_at;
	apply.result = &result;

	if (NULL != batch->currency)
	{
		if (!venture_series_store_internal_normalise_currency(batch->currency, apply.currency,
		                                                      error))
			return FALSE;

		apply.has_currency = TRUE;
	}

	if (!venture_series_store_internal_begin(self, error))
		return FALSE;

	apply.accounts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, accounts_touched_free);
	apply.venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	apply.instruments = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	apply.logins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	/* Read once: a batch of fifty thousand rows must not ask the file
	 * system for its size fifty thousand times. */
	ok = venture_series_store_internal_over_cap(self, &apply.over_cap, error);

	/* Logins before the accounts that name them, so a described login is
	 * written with its name rather than bare and then filled in. */
	for (i = 0; ok && (i < batch->n_logins); i++)
		ok = accounts_write_login(&apply, &batch->logins[i], error);

	for (i = 0; ok && (i < batch->n_accounts); i++)
	{
		ok = (NULL != accounts_write_account(&apply, &batch->accounts[i], error));

		if (ok)
			result.accounts++;
	}

	for (i = 0; ok && (i < batch->n_snapshots); i++)
		ok = accounts_begin_snapshot(&apply, &batch->snapshots[i], error);

	ok = ok &&
	     accounts_apply_balances(&apply, batch, error) &&
	     accounts_apply_holdings(&apply, batch, error) &&
	     accounts_apply_positions(&apply, batch, error) &&
	     accounts_apply_inbound(&apply, batch, error) &&
	     accounts_sweep(&apply, error) &&
	     accounts_apply_txns(&apply, batch, error);

	accounts_apply_clear(&apply);

	if (!ok)
	{
		venture_series_store_internal_rollback_one(self);
		return FALSE;
	}

	if (!venture_series_store_internal_commit(self, error))
		return FALSE;

	if (NULL != out)
		*out = result;

	return TRUE;
}

/* --- Retention -------------------------------------------------------------------- */

static const gchar accounts_sql_purge_txns[] =
	"DELETE FROM external_txns WHERE at < ?1";

/* Never an account's newest point in a currency, however old. */
static const gchar accounts_sql_purge_balances[] =
	"DELETE FROM account_balances WHERE at < ?1"
	"   AND at < (SELECT MAX(b2.at) FROM account_balances b2"
	"             WHERE b2.account_id = account_balances.account_id"
	"               AND b2.currency = account_balances.currency)";

gboolean
venture_series_accounts_purge(
	VentureSeriesStore		 *self,
	gint64				  first_day,
	VentureSeriesPurgeResult	 *result,
	GError				**error
){
	static const gchar *const sql[] = { accounts_sql_purge_txns, accounts_sql_purge_balances };
	gint64 *counts[2];
	guint i;

	counts[0] = &result->txns;
	counts[1] = &result->balances;

	for (i = 0; i < G_N_ELEMENTS(sql); i++)
	{
		g_autoptr(SeriesCachedStmt) stmt = NULL;

		stmt = venture_series_store_internal_stmt(self, sql[i], error);
		if (NULL == stmt)
			return FALSE;

		sqlite3_bind_int64(stmt, 1, first_day * 86400);

		if (!venture_series_store_internal_step_done(self, stmt, "purging account history",
		                                             error))
			return FALSE;

		*counts[i] = sqlite3_changes(venture_series_store_internal_db(self));
	}

	return TRUE;
}

/* ==========================================================================
 * Reading
 * ========================================================================== */

/* --- Accounts ---------------------------------------------------------------------- */

void
venture_series_account_row_free(VentureSeriesAccountRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->name);
	g_free(row->kind);
	g_free(row->group_key);
	g_free(row->venue_key);
	g_free(row->attrs_json);
	g_clear_pointer(&row->balances, g_array_unref);
	g_clear_pointer(&row->inbound_money, g_array_unref);
	g_clear_pointer(&row->inbound_cod, g_array_unref);
	g_free(row->login_key);
	g_free(row->login_name);
	g_free(row);
}

#define ACCOUNTS_ROW_SQL \
	"SELECT a.id, a.key, a.name, a.kind, a.group_key, a.venue_key, a.attrs," \
	"  a.last_seen, a.first_seen, a.synced_at," \
	"  a.holdings_at, a.positions_at, a.inbound_at, a.balances_at," \
	"  (SELECT COUNT(*) FROM account_holdings h WHERE h.account_id = a.id)," \
	"  (SELECT COUNT(*) FROM account_positions p WHERE p.account_id = a.id)," \
	"  (SELECT COUNT(*) FROM account_positions p WHERE p.account_id = a.id" \
	"     AND p.expires_at <= ?1)," \
	"  (SELECT MIN(p.expires_at) FROM account_positions p WHERE p.account_id = a.id)," \
	"  (SELECT COUNT(*) FROM account_inbound n WHERE n.account_id = a.id)," \
	"  (SELECT COUNT(*) FROM account_inbound n WHERE n.account_id = a.id" \
	"     AND n.expires_at <= ?1)," \
	"  (SELECT MIN(n.expires_at) FROM account_inbound n WHERE n.account_id = a.id)," \
	"  a.login, (SELECT l.name FROM logins l WHERE l.key = a.login)" \
	" FROM accounts a"

static const gchar accounts_sql_list[] =
	ACCOUNTS_ROW_SQL
	" WHERE (?2 IS NULL OR a.kind = ?2) AND (?3 IS NULL OR a.group_key = ?3)"
	"  AND (?5 IS NULL OR a.login = ?5)"
	" ORDER BY a.group_key, COALESCE(a.name, a.key), a.key"
	" LIMIT ?4";

static const gchar accounts_sql_get[] =
	ACCOUNTS_ROW_SQL
	" WHERE a.key = ?2";

static const gchar accounts_sql_inbound_money[] =
	"SELECT currency, SUM(COALESCE(money, 0)), SUM(COALESCE(cod, 0))"
	" FROM account_inbound WHERE account_id = ?1 AND currency IS NOT NULL"
	" GROUP BY currency ORDER BY currency";

/* The amounts beside an account: its newest balances, its mail's money. */
static gboolean
accounts_row_amounts(
	VentureSeriesStore	 *self,
	gint64			  id,
	VentureSeriesAccountRow	 *row,
	GError			**error
){
	g_autoptr(SeriesCachedStmt) balances = NULL;
	g_autoptr(SeriesCachedStmt) mail = NULL;
	gint rc;

	row->balances = g_array_new(FALSE, TRUE, sizeof(VentureSeriesAmount));
	row->inbound_money = g_array_new(FALSE, TRUE, sizeof(VentureSeriesAmount));
	row->inbound_cod = g_array_new(FALSE, TRUE, sizeof(VentureSeriesAmount));

	balances = venture_series_store_internal_stmt(self, accounts_sql_latest_balances, error);
	if (NULL == balances)
		return FALSE;

	sqlite3_bind_int64(balances, 1, id);

	while (SQLITE_ROW == (rc = sqlite3_step(balances)))
	{
		VentureSeriesAmount amount;

		memset(&amount, 0, sizeof(amount));
		series_column_currency(balances, 0, amount.currency);
		amount.amount = sqlite3_column_int64(balances, 1);
		amount.at = sqlite3_column_int64(balances, 2);
		g_array_append_val(row->balances, amount);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "reading balances", error);
		return FALSE;
	}

	mail = venture_series_store_internal_stmt(self, accounts_sql_inbound_money, error);
	if (NULL == mail)
		return FALSE;

	sqlite3_bind_int64(mail, 1, id);

	/* SUM of integers: SQLite refuses an overflow rather than round. */
	while (SQLITE_ROW == (rc = sqlite3_step(mail)))
	{
		VentureSeriesAmount money;
		VentureSeriesAmount cod;

		memset(&money, 0, sizeof(money));
		series_column_currency(mail, 0, money.currency);
		cod = money;
		money.amount = sqlite3_column_int64(mail, 1);
		cod.amount = sqlite3_column_int64(mail, 2);

		if (0 != money.amount)
			g_array_append_val(row->inbound_money, money);

		if (0 != cod.amount)
			g_array_append_val(row->inbound_cod, cod);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "reading the mail's money", error);
		return FALSE;
	}

	return TRUE;
}

static VentureSeriesAccountRow *
accounts_row_from(sqlite3_stmt *stmt)
{
	g_autoptr(VentureSeriesAccountRow) row = NULL;

	row = g_new0(VentureSeriesAccountRow, 1);
	row->key = series_column_strdup(stmt, 1);
	row->name = series_column_strdup(stmt, 2);
	row->kind = series_column_strdup(stmt, 3);
	row->group_key = series_column_strdup(stmt, 4);
	row->venue_key = series_column_strdup(stmt, 5);
	row->attrs_json = series_column_strdup(stmt, 6);
	row->last_seen = series_column_figure(stmt, 7);
	row->first_seen = sqlite3_column_int64(stmt, 8);
	row->synced_at = sqlite3_column_int64(stmt, 9);
	row->holdings_at = series_column_figure(stmt, 10);
	row->positions_at = series_column_figure(stmt, 11);
	row->inbound_at = series_column_figure(stmt, 12);
	row->balances_at = series_column_figure(stmt, 13);
	row->holdings = sqlite3_column_int64(stmt, 14);
	row->positions = sqlite3_column_int64(stmt, 15);
	row->positions_expired = sqlite3_column_int64(stmt, 16);
	row->soonest_position_expiry = series_column_figure(stmt, 17);
	row->inbound = sqlite3_column_int64(stmt, 18);
	row->inbound_expired = sqlite3_column_int64(stmt, 19);
	row->soonest_inbound_expiry = series_column_figure(stmt, 20);
	row->login_key = series_column_strdup(stmt, 21);
	row->login_name = series_column_strdup(stmt, 22);

	if (NULL == row->group_key)
		row->group_key = g_strdup("");

	if (NULL == row->login_key)
		row->login_key = g_strdup("");

	return g_steal_pointer(&row);
}

GPtrArray *
venture_series_store_list_accounts(
	VentureSeriesStore	 *self,
	const gchar		 *kind,
	const gchar		 *group_key,
	const gchar		 *login_key,
	gint64			  now,
	GError			**error
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	guint bound;
	guint i;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = venture_series_store_internal_stmt(self, accounts_sql_list, error);
	if (NULL == stmt)
		return NULL;

	sqlite3_bind_int64(stmt, 1, now);
	series_bind_text(stmt, 2, kind);
	series_bind_text(stmt, 3, group_key);
	bound = (guint)venture_series_accounts_get_max_accounts();
	sqlite3_bind_int64(stmt, 4, (gint64)bound + 1);
	series_bind_text(stmt, 5, login_key);

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_account_row_free);
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		gint64 id = sqlite3_column_int64(stmt, 0);

		g_ptr_array_add(rows, accounts_row_from(stmt));
		g_array_append_val(ids, id);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "listing accounts", error);
		return NULL;
	}

	g_clear_pointer(&stmt, series_cached_stmt_release);

	/* Every account or a refusal: a caller that judges the accounts it
	 * does not see would take the rest for gone. */
	if (rows->len > bound)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The store has more than %u accounts there; narrow the read to a group, "
		            "a login or a kind", bound);
		return NULL;
	}

	/* The amounts after the walk: two cached statements stepping at once
	 * would share one connection's cursor bookkeeping for nothing. */
	for (i = 0; i < rows->len; i++)
	{
		if (!accounts_row_amounts(self, g_array_index(ids, gint64, i),
		                          g_ptr_array_index(rows, i), error))
			return NULL;
	}

	return g_steal_pointer(&rows);
}

gboolean
venture_series_store_get_account(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			  now,
	VentureSeriesAccountRow	**out,
	GError			**error
){
	g_autoptr(VentureSeriesAccountRow) row = NULL;
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	gint64 id;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	*out = NULL;

	if (NULL == key)
		return TRUE;

	stmt = venture_series_store_internal_stmt(self, accounts_sql_get, error);
	if (NULL == stmt)
		return FALSE;

	sqlite3_bind_int64(stmt, 1, now);
	series_bind_text(stmt, 2, key);
	rc = sqlite3_step(stmt);

	if (SQLITE_DONE == rc)
		return TRUE;

	if (SQLITE_ROW != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "reading an account", error);
		return FALSE;
	}

	id = sqlite3_column_int64(stmt, 0);
	row = accounts_row_from(stmt);
	g_clear_pointer(&stmt, series_cached_stmt_release);

	if (!accounts_row_amounts(self, id, row, error))
		return FALSE;

	*out = g_steal_pointer(&row);

	return TRUE;
}

/* --- Logins ------------------------------------------------------------------------ */

void
venture_series_login_row_free(VentureSeriesLoginRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->name);
	g_free(row->kind);
	g_free(row->group_key);
	g_free(row->attrs_json);
	g_free(row);
}

static const gchar accounts_sql_logins[] =
	"SELECT l.key, l.name, l.kind, l.group_key, l.attrs, l.first_seen, l.last_seen,"
	"  (SELECT COUNT(*) FROM accounts a WHERE a.login = l.key),"
	"  (SELECT COUNT(*) FROM accounts a WHERE a.login = l.key AND a.kind = 'character')"
	" FROM logins l ORDER BY COALESCE(l.name, l.key), l.key LIMIT ?1";

GPtrArray *
venture_series_store_list_logins(
	VentureSeriesStore	 *self,
	GError			**error
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	guint bound;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	stmt = venture_series_store_internal_stmt(self, accounts_sql_logins, error);
	if (NULL == stmt)
		return NULL;

	bound = (guint)venture_series_accounts_get_max_accounts();
	sqlite3_bind_int64(stmt, 1, (gint64)bound + 1);
	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_login_row_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesLoginRow *row = g_new0(VentureSeriesLoginRow, 1);

		g_ptr_array_add(rows, row);
		row->key = series_column_strdup(stmt, 0);
		row->name = series_column_strdup(stmt, 1);
		row->kind = series_column_strdup(stmt, 2);
		row->group_key = series_column_strdup(stmt, 3);
		row->attrs_json = series_column_strdup(stmt, 4);
		row->first_seen = sqlite3_column_int64(stmt, 5);
		row->last_seen = sqlite3_column_int64(stmt, 6);
		row->accounts = sqlite3_column_int64(stmt, 7);
		row->characters = sqlite3_column_int64(stmt, 8);

		if (NULL == row->group_key)
			row->group_key = g_strdup("");
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "listing logins", error);
		return NULL;
	}

	if (rows->len > bound)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The store has more than %u logins", bound);
		return NULL;
	}

	return g_steal_pointer(&rows);
}

/* --- Holdings ---------------------------------------------------------------------- */

void
venture_series_holding_filter_init(VentureSeriesHoldingFilter *filter)
{
	g_return_if_fail(NULL != filter);

	memset(filter, 0, sizeof(*filter));
}

void
venture_series_holding_row_free(VentureSeriesHoldingRow *row)
{
	if (NULL == row)
		return;

	g_free(row->account_key);
	g_free(row->account_name);
	g_free(row->place);
	g_free(row->instrument_key);
	g_free(row->instrument_name);
	g_free(row);
}

void
venture_series_holding_total_free(VentureSeriesHoldingTotal *total)
{
	if (NULL == total)
		return;

	g_free(total->instrument_key);
	g_free(total->instrument_name);
	g_free(total->category);
	g_clear_pointer(&total->places, g_ptr_array_unref);
	g_free(total);
}

/* The WHERE of a holdings read; every value a bound parameter. */
static gboolean
accounts_holding_where(
	const VentureSeriesHoldingFilter	 *filter,
	GString					 *sql,
	GArray					 *bindings,
	GError					**error
){
	g_string_append(sql, " WHERE 1");

	if (NULL == filter)
		return TRUE;

	if (NULL != filter->account_key)
	{
		g_string_append(sql, " AND h.account_id = (SELECT id FROM accounts WHERE key = ?)");
		accounts_bind_add_text(bindings, filter->account_key);
	}

	if (NULL != filter->login_key)
	{
		g_string_append(sql, " AND h.account_id IN (SELECT id FROM accounts WHERE login = ?)");
		accounts_bind_add_text(bindings, filter->login_key);
	}

	if (NULL != filter->instrument_key)
	{
		g_string_append(sql, " AND h.instrument_key = ?");
		accounts_bind_add_text(bindings, filter->instrument_key);
	}

	if (NULL != filter->place)
	{
		g_string_append(sql, " AND h.place = ?");
		accounts_bind_add_text(bindings, filter->place);
	}

	if (NULL != filter->exclude_place)
	{
		g_string_append(sql, " AND h.place <> ?");
		accounts_bind_add_text(bindings, filter->exclude_place);
	}

	if ((NULL != filter->search) && ('\0' != filter->search[0]))
	{
		g_autofree gchar *fold = NULL;
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *pattern = NULL;

		if (!g_utf8_validate(filter->search, -1, NULL))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A search must be UTF-8");
			return FALSE;
		}

		fold = g_utf8_casefold(filter->search, -1);
		escaped = accounts_like_escape(fold);
		pattern = g_strdup_printf("%%%s%%", escaped);

		g_string_append(sql, " AND (i.name_fold LIKE ? ESCAPE '\\'"
		                     " OR h.instrument_key LIKE ? ESCAPE '\\')");
		accounts_bind_add_text(bindings, pattern);
		accounts_bind_add_text(bindings, pattern);
	}

	return TRUE;
}

GPtrArray *
venture_series_store_list_holdings(
	VentureSeriesStore			 *self,
	const VentureSeriesHoldingFilter	 *filter,
	GError					**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean all;
	guint bound;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	sql = g_string_new("SELECT a.key, a.name, h.place, h.instrument_key, i.name, h.quantity, h.at"
	                   " FROM account_holdings h JOIN accounts a ON a.id = h.account_id"
	                   " LEFT JOIN instruments i ON i.key = h.instrument_key");
	bindings = accounts_bindings_new();

	if (!accounts_holding_where(filter, sql, bindings, error))
		return NULL;

	g_string_append(sql, " ORDER BY a.group_key, COALESCE(a.name, a.key), a.key, h.place,"
	                     " COALESCE(i.name_fold, h.instrument_key), h.instrument_key"
	                     " LIMIT ? OFFSET ?");
	bound = (guint)venture_series_accounts_get_max_rows();
	accounts_bind_add_int(bindings, accounts_limit((NULL != filter) ? filter->count : 0, bound,
	                                               &all));
	accounts_bind_add_int(bindings, (NULL != filter) ? filter->offset : 0);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_holding_row_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesHoldingRow *row = g_new0(VentureSeriesHoldingRow, 1);

		row->account_key = series_column_strdup(stmt, 0);
		row->account_name = series_column_strdup(stmt, 1);
		row->place = series_column_strdup(stmt, 2);
		row->instrument_key = series_column_strdup(stmt, 3);
		row->instrument_name = series_column_strdup(stmt, 4);
		row->quantity = sqlite3_column_int64(stmt, 5);
		row->at = sqlite3_column_int64(stmt, 6);
		g_ptr_array_add(rows, row);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "listing holdings", error);
		return NULL;
	}

	if (!accounts_check_whole(rows, all, bound, "holdings", "an account, an instrument or a place",
	                          error))
		return NULL;

	return g_steal_pointer(&rows);
}

GPtrArray *
venture_series_store_holdings_by_instrument(
	VentureSeriesStore			 *self,
	const VentureSeriesHoldingFilter	 *filter,
	GError					**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) totals = NULL;
	VentureSeriesHoldingFilter places;
	guint i;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	sql = g_string_new("SELECT h.instrument_key, i.name, i.category, SUM(h.quantity),"
	                   "  COUNT(DISTINCT h.account_id)"
	                   " FROM account_holdings h"
	                   " LEFT JOIN instruments i ON i.key = h.instrument_key");
	bindings = accounts_bindings_new();

	if (!accounts_holding_where(filter, sql, bindings, error))
		return NULL;

	g_string_append(sql, " GROUP BY h.instrument_key"
	                     " ORDER BY COALESCE(i.name_fold, h.instrument_key), h.instrument_key"
	                     " LIMIT ? OFFSET ?");
	accounts_bind_add_int(bindings, accounts_page((NULL != filter) ? filter->count : 0,
	                                              VENTURE_SERIES_DEFAULT_PAGE));
	accounts_bind_add_int(bindings, (NULL != filter) ? filter->offset : 0);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	totals = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_holding_total_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesHoldingTotal *total = g_new0(VentureSeriesHoldingTotal, 1);

		total->instrument_key = series_column_strdup(stmt, 0);
		total->instrument_name = series_column_strdup(stmt, 1);
		total->category = series_column_strdup(stmt, 2);
		total->quantity = sqlite3_column_int64(stmt, 3);
		total->accounts = sqlite3_column_int64(stmt, 4);
		g_ptr_array_add(totals, total);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "summing holdings", error);
		return NULL;
	}

	/* Each instrument's breakdown under the same filter, unpaged. */
	if (NULL != filter)
		places = *filter;
	else
		venture_series_holding_filter_init(&places);

	places.offset = 0;
	places.count = 0;
	places.search = NULL;

	for (i = 0; i < totals->len; i++)
	{
		VentureSeriesHoldingTotal *total = g_ptr_array_index(totals, i);

		places.instrument_key = total->instrument_key;
		total->places = venture_series_store_list_holdings(self, &places, error);

		if (NULL == total->places)
			return NULL;
	}

	return g_steal_pointer(&totals);
}

/* --- Positions ---------------------------------------------------------------------- */

void
venture_series_position_filter_init(VentureSeriesPositionFilter *filter)
{
	g_return_if_fail(NULL != filter);

	memset(filter, 0, sizeof(*filter));
	filter->expires_before = VENTURE_SERIES_NONE;
}

void
venture_series_position_row_free(VentureSeriesPositionRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->account_key);
	g_free(row->venue_key);
	g_free(row->instrument_key);
	g_free(row->instrument_name);
	g_free(row);
}

GPtrArray *
venture_series_store_list_positions(
	VentureSeriesStore			 *self,
	const VentureSeriesPositionFilter	 *filter,
	GError					**error
){
	VentureSeriesPositionFilter everything;
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean all;
	guint bound;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (NULL == filter)
	{
		venture_series_position_filter_init(&everything);
		filter = &everything;
	}

	sql = g_string_new("SELECT p.key, a.key, p.venue_key, p.instrument_key, i.name, p.quantity,"
	                   "  p.unit_price, p.bid, p.currency, p.expires_at, p.posted_at,"
	                   "  p.first_seen, p.last_seen"
	                   " FROM account_positions p JOIN accounts a ON a.id = p.account_id"
	                   " LEFT JOIN instruments i ON i.key = p.instrument_key WHERE 1");
	bindings = accounts_bindings_new();

	/* Only the conditions asked for are written, so the planner sees a
	 * bare range on expires_at and takes its index. */
	if (NULL != filter->account_key)
	{
		g_string_append(sql, " AND p.account_id = (SELECT id FROM accounts WHERE key = ?)");
		accounts_bind_add_text(bindings, filter->account_key);
	}

	if (NULL != filter->login_key)
	{
		g_string_append(sql, " AND a.login = ?");
		accounts_bind_add_text(bindings, filter->login_key);
	}

	if (NULL != filter->venue_key)
	{
		g_string_append(sql, " AND p.venue_key = ?");
		accounts_bind_add_text(bindings, filter->venue_key);
	}

	if (NULL != filter->instrument_key)
	{
		g_string_append(sql, " AND p.instrument_key = ?");
		accounts_bind_add_text(bindings, filter->instrument_key);
	}

	if (VENTURE_SERIES_NONE != filter->expires_before)
	{
		g_string_append(sql, " AND p.expires_at < ?");
		accounts_bind_add_int(bindings, filter->expires_before);
	}

	g_string_append(sql, " ORDER BY p.expires_at IS NULL, p.expires_at, p.key LIMIT ? OFFSET ?");
	bound = (guint)venture_series_accounts_get_max_rows();
	accounts_bind_add_int(bindings, accounts_limit(filter->count, bound, &all));
	accounts_bind_add_int(bindings, filter->offset);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_position_row_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesPositionRow *row = g_new0(VentureSeriesPositionRow, 1);

		row->key = series_column_strdup(stmt, 0);
		row->account_key = series_column_strdup(stmt, 1);
		row->venue_key = series_column_strdup(stmt, 2);
		row->instrument_key = series_column_strdup(stmt, 3);
		row->instrument_name = series_column_strdup(stmt, 4);
		row->quantity = sqlite3_column_int64(stmt, 5);
		row->unit_price = sqlite3_column_int64(stmt, 6);
		row->bid = series_column_figure(stmt, 7);
		series_column_currency(stmt, 8, row->currency);
		row->expires_at = series_column_figure(stmt, 9);
		row->posted_at = series_column_figure(stmt, 10);
		row->first_seen = sqlite3_column_int64(stmt, 11);
		row->last_seen = sqlite3_column_int64(stmt, 12);
		g_ptr_array_add(rows, row);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "listing positions", error);
		return NULL;
	}

	if (!accounts_check_whole(rows, all, bound, "positions", "an account, a venue or an instrument",
	                          error))
		return NULL;

	return g_steal_pointer(&rows);
}

/* --- Inbound ------------------------------------------------------------------------- */

void
venture_series_inbound_filter_init(VentureSeriesInboundFilter *filter)
{
	g_return_if_fail(NULL != filter);

	memset(filter, 0, sizeof(*filter));
	filter->expires_before = VENTURE_SERIES_NONE;
}

void
venture_series_inbound_row_free(VentureSeriesInboundRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->account_key);
	g_free(row->sender);
	g_free(row->subject);
	g_free(row->instrument_key);
	g_free(row->instrument_name);
	g_free(row);
}

GPtrArray *
venture_series_store_list_inbound(
	VentureSeriesStore			 *self,
	const VentureSeriesInboundFilter	 *filter,
	GError					**error
){
	VentureSeriesInboundFilter everything;
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean all;
	guint bound;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (NULL == filter)
	{
		venture_series_inbound_filter_init(&everything);
		filter = &everything;
	}

	sql = g_string_new("SELECT n.key, a.key, n.sender, n.subject, n.money, n.cod, n.currency,"
	                   "  n.instrument_key, i.name, n.quantity, n.expires_at, n.returned,"
	                   "  n.first_seen, n.last_seen"
	                   " FROM account_inbound n JOIN accounts a ON a.id = n.account_id"
	                   " LEFT JOIN instruments i ON i.key = n.instrument_key WHERE 1");
	bindings = accounts_bindings_new();

	if (NULL != filter->account_key)
	{
		g_string_append(sql, " AND n.account_id = (SELECT id FROM accounts WHERE key = ?)");
		accounts_bind_add_text(bindings, filter->account_key);
	}

	if (NULL != filter->login_key)
	{
		g_string_append(sql, " AND a.login = ?");
		accounts_bind_add_text(bindings, filter->login_key);
	}

	if (VENTURE_SERIES_NONE != filter->expires_before)
	{
		g_string_append(sql, " AND n.expires_at < ?");
		accounts_bind_add_int(bindings, filter->expires_before);
	}

	g_string_append(sql, " ORDER BY n.expires_at IS NULL, n.expires_at, n.key LIMIT ? OFFSET ?");
	bound = (guint)venture_series_accounts_get_max_rows();
	accounts_bind_add_int(bindings, accounts_limit(filter->count, bound, &all));
	accounts_bind_add_int(bindings, filter->offset);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_inbound_row_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesInboundRow *row = g_new0(VentureSeriesInboundRow, 1);

		row->key = series_column_strdup(stmt, 0);
		row->account_key = series_column_strdup(stmt, 1);
		row->sender = series_column_strdup(stmt, 2);
		row->subject = series_column_strdup(stmt, 3);
		row->money = series_column_figure(stmt, 4);
		row->cod = series_column_figure(stmt, 5);
		series_column_currency(stmt, 6, row->currency);
		row->instrument_key = series_column_strdup(stmt, 7);
		row->instrument_name = series_column_strdup(stmt, 8);
		row->quantity = series_column_figure(stmt, 9);
		row->expires_at = series_column_figure(stmt, 10);
		row->returned = (0 != sqlite3_column_int(stmt, 11));
		row->first_seen = sqlite3_column_int64(stmt, 12);
		row->last_seen = sqlite3_column_int64(stmt, 13);
		g_ptr_array_add(rows, row);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "listing inbound", error);
		return NULL;
	}

	if (!accounts_check_whole(rows, all, bound, "inbound rows", "an account", error))
		return NULL;

	return g_steal_pointer(&rows);
}

/* --- Balance history ----------------------------------------------------------------- */

GArray *
venture_series_store_balance_history(
	VentureSeriesStore	 *self,
	const gchar		 *account_key,
	const gchar		 *currency,
	gint64			  since,
	gint64			  until,
	GError			**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GArray) points = NULL;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);
	g_return_val_if_fail(NULL != account_key, NULL);

	sql = g_string_new("SELECT b.currency, b.amount, b.at FROM account_balances b"
	                   " WHERE b.account_id = (SELECT id FROM accounts WHERE key = ?)");
	bindings = accounts_bindings_new();
	accounts_bind_add_text(bindings, account_key);

	if (NULL != currency)
	{
		gchar code[VENTURE_MONEY_CURRENCY_LEN];

		if (!venture_series_store_internal_normalise_currency(currency, code, error))
			return NULL;

		g_string_append(sql, " AND b.currency = ?");
		accounts_bind_add_text(bindings, code);
	}

	if (VENTURE_SERIES_NONE != since)
	{
		g_string_append(sql, " AND b.at >= ?");
		accounts_bind_add_int(bindings, since);
	}

	if (VENTURE_SERIES_NONE != until)
	{
		g_string_append(sql, " AND b.at < ?");
		accounts_bind_add_int(bindings, until);
	}

	g_string_append(sql, " ORDER BY b.at, b.currency LIMIT ?");
	accounts_bind_add_int(bindings, VENTURE_SERIES_MAX_ACCOUNT_ROWS);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	points = g_array_new(FALSE, TRUE, sizeof(VentureSeriesAmount));

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesAmount point;

		memset(&point, 0, sizeof(point));
		series_column_currency(stmt, 0, point.currency);
		point.amount = sqlite3_column_int64(stmt, 1);
		point.at = sqlite3_column_int64(stmt, 2);
		g_array_append_val(points, point);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "reading balance history", error);
		return NULL;
	}

	return g_steal_pointer(&points);
}

/* --- The external ledger ---------------------------------------------------------------- */

void
venture_series_txn_filter_init(VentureSeriesTxnFilter *filter)
{
	g_return_if_fail(NULL != filter);

	memset(filter, 0, sizeof(*filter));
	filter->since = VENTURE_SERIES_NONE;
	filter->until = VENTURE_SERIES_NONE;
}

void
venture_series_txn_row_free(VentureSeriesTxnRow *row)
{
	if (NULL == row)
		return;

	g_free(row->key);
	g_free(row->account_key);
	g_free(row->venue_key);
	g_free(row->kind);
	g_free(row->instrument_key);
	g_free(row->instrument_name);
	g_free(row->counterparty);
	g_free(row->source);
	g_free(row);
}

void
venture_series_txn_total_free(VentureSeriesTxnTotal *total)
{
	if (NULL == total)
		return;

	g_free(total->key);
	g_free(total->label);
	g_free(total);
}

/*
 * The WHERE of a ledger read. An account is matched through its id so the
 * (account_id, at) index serves "this character's last month", and only
 * the conditions asked for are written.
 */
static gboolean
accounts_txn_where(
	const VentureSeriesTxnFilter	 *filter,
	GString				 *sql,
	GArray				 *bindings,
	GError				**error
){
	g_string_append(sql, " WHERE 1");

	if (NULL == filter)
		return TRUE;

	if (NULL != filter->account_key)
	{
		g_string_append(sql, " AND t.account_id = (SELECT id FROM accounts WHERE key = ?)");
		accounts_bind_add_text(bindings, filter->account_key);
	}

	if (NULL != filter->login_key)
	{
		g_string_append(sql, " AND t.account_id IN (SELECT id FROM accounts WHERE login = ?)");
		accounts_bind_add_text(bindings, filter->login_key);
	}

	if (NULL != filter->kind)
	{
		if (!accounts_check_choice(filter->kind, accounts_txn_kinds, "ledger row's kind", error))
			return FALSE;

		g_string_append(sql, " AND t.kind = ?");
		accounts_bind_add_text(bindings, filter->kind);
	}

	if (NULL != filter->instrument_key)
	{
		g_string_append(sql, " AND t.instrument_key = ?");
		accounts_bind_add_text(bindings, filter->instrument_key);
	}

	if (NULL != filter->venue_key)
	{
		g_string_append(sql, " AND t.venue_key = ?");
		accounts_bind_add_text(bindings, filter->venue_key);
	}

	if (NULL != filter->source)
	{
		g_string_append(sql, " AND t.source = ?");
		accounts_bind_add_text(bindings, filter->source);
	}

	if (VENTURE_SERIES_NONE != filter->since)
	{
		g_string_append(sql, " AND t.at >= ?");
		accounts_bind_add_int(bindings, filter->since);
	}

	if (VENTURE_SERIES_NONE != filter->until)
	{
		g_string_append(sql, " AND t.at < ?");
		accounts_bind_add_int(bindings, filter->until);
	}

	return TRUE;
}

GPtrArray *
venture_series_store_list_txns(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	GError				**error
){
	VentureSeriesTxnFilter newest;
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (NULL == filter)
	{
		venture_series_txn_filter_init(&newest);
		newest.descending = TRUE;
		filter = &newest;
	}

	sql = g_string_new("SELECT t.key, a.key, t.venue_key, t.kind, t.instrument_key, i.name,"
	                   "  t.quantity, t.unit_price, t.amount, t.currency, t.counterparty,"
	                   "  t.source, t.at, t.first_seen, t.updated_at"
	                   " FROM external_txns t JOIN accounts a ON a.id = t.account_id"
	                   " LEFT JOIN instruments i ON i.key = t.instrument_key");
	bindings = accounts_bindings_new();

	if (!accounts_txn_where(filter, sql, bindings, error))
		return NULL;

	g_string_append(sql, filter->descending ? " ORDER BY t.at DESC, t.key DESC"
	                                        : " ORDER BY t.at, t.key");
	g_string_append(sql, " LIMIT ? OFFSET ?");
	accounts_bind_add_int(bindings, accounts_page(filter->count, VENTURE_SERIES_DEFAULT_PAGE));
	accounts_bind_add_int(bindings, filter->offset);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_txn_row_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesTxnRow *row = g_new0(VentureSeriesTxnRow, 1);

		row->key = series_column_strdup(stmt, 0);
		row->account_key = series_column_strdup(stmt, 1);
		row->venue_key = series_column_strdup(stmt, 2);
		row->kind = series_column_strdup(stmt, 3);
		row->instrument_key = series_column_strdup(stmt, 4);
		row->instrument_name = series_column_strdup(stmt, 5);
		row->quantity = series_column_figure(stmt, 6);
		row->unit_price = series_column_figure(stmt, 7);
		row->amount = series_column_figure(stmt, 8);
		series_column_currency(stmt, 9, row->currency);
		row->counterparty = series_column_strdup(stmt, 10);
		row->source = series_column_strdup(stmt, 11);
		row->at = sqlite3_column_int64(stmt, 12);
		row->first_seen = sqlite3_column_int64(stmt, 13);
		row->updated_at = sqlite3_column_int64(stmt, 14);
		g_ptr_array_add(rows, row);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "listing the ledger", error);
		return NULL;
	}

	return g_steal_pointer(&rows);
}

gboolean
venture_series_store_count_txns(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	gint64				 *out_count,
	GError				**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), FALSE);
	g_return_val_if_fail(NULL != out_count, FALSE);

	*out_count = 0;
	sql = g_string_new("SELECT COUNT(*) FROM external_txns t");
	bindings = accounts_bindings_new();

	if (!accounts_txn_where(filter, sql, bindings, error))
		return FALSE;

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return FALSE;

	rc = sqlite3_step(stmt);

	if (SQLITE_ROW != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "counting the ledger", error);
		return FALSE;
	}

	*out_count = sqlite3_column_int64(stmt, 0);

	return TRUE;
}

GPtrArray *
venture_series_store_txn_totals(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	VentureSeriesTxnGroup		  group,
	GError				**error
){
	const gchar *key_expr;
	const gchar *label_expr;
	const gchar *period_expr;
	const gchar *joins;
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) totals = NULL;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	key_expr = "NULL";
	label_expr = "NULL";
	period_expr = "NULL";
	joins = "";

	/*
	 * Periods are whole UTC days, Monday-based ISO weeks (day 0, the
	 * epoch, was a Thursday) and calendar months: the same boundaries
	 * every report in VENTURE uses.
	 */
	switch (group)
	{
	case VENTURE_SERIES_TXN_GROUP_DAY:
		period_expr = "(t.at / 86400) * 86400";
		break;
	case VENTURE_SERIES_TXN_GROUP_WEEK:
		period_expr = "((t.at / 86400) - (((t.at / 86400) + 3) % 7)) * 86400";
		break;
	case VENTURE_SERIES_TXN_GROUP_MONTH:
		period_expr = "CAST(strftime('%s', t.at, 'unixepoch', 'start of month') AS INTEGER)";
		break;
	case VENTURE_SERIES_TXN_GROUP_ACCOUNT:
		key_expr = "a.key";
		label_expr = "a.name";
		joins = " JOIN accounts a ON a.id = t.account_id";
		break;
	case VENTURE_SERIES_TXN_GROUP_VENUE:
		key_expr = "COALESCE(t.venue_key, '')";
		label_expr = "v.name";
		joins = " LEFT JOIN venues v ON v.key = t.venue_key";
		break;
	case VENTURE_SERIES_TXN_GROUP_INSTRUMENT:
		key_expr = "COALESCE(t.instrument_key, '')";
		label_expr = "i.name";
		joins = " LEFT JOIN instruments i ON i.key = t.instrument_key";
		break;
	case VENTURE_SERIES_TXN_GROUP_SOURCE:
		key_expr = "COALESCE(t.source, '')";
		break;
	case VENTURE_SERIES_TXN_GROUP_LOGIN:
		key_expr = "a.login";
		label_expr = "l.name";
		joins = " JOIN accounts a ON a.id = t.account_id LEFT JOIN logins l ON l.key = a.login";
		break;
	default:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Not a way to group the ledger");
		return NULL;
	}

	sql = g_string_new(NULL);
	g_string_append_printf(sql,
		"SELECT %s, %s, %s, COALESCE(t.currency, ''), COUNT(*),"
		"  SUM(t.kind = 'sale'),"
		"  SUM(CASE WHEN t.kind = 'sale' THEN COALESCE(t.quantity, 0) ELSE 0 END),"
		"  SUM(CASE WHEN t.kind = 'sale' THEN COALESCE(t.amount, 0) ELSE 0 END),"
		"  SUM(t.kind = 'buy'),"
		"  SUM(CASE WHEN t.kind = 'buy' THEN COALESCE(t.quantity, 0) ELSE 0 END),"
		"  SUM(CASE WHEN t.kind = 'buy' THEN COALESCE(t.amount, 0) ELSE 0 END),"
		"  SUM(CASE WHEN t.kind = 'income' THEN COALESCE(t.amount, 0) ELSE 0 END),"
		"  SUM(CASE WHEN t.kind = 'expense' THEN COALESCE(t.amount, 0) ELSE 0 END),"
		"  SUM(CASE WHEN t.kind = 'expired' THEN COALESCE(t.quantity, 0) ELSE 0 END),"
		"  SUM(CASE WHEN t.kind = 'cancelled' THEN COALESCE(t.quantity, 0) ELSE 0 END)"
		" FROM external_txns t%s",
		key_expr, label_expr, period_expr, joins);
	bindings = accounts_bindings_new();

	if (!accounts_txn_where(filter, sql, bindings, error))
		return NULL;

	/* One more than allowed, so a truncated answer is refused rather
	 * than returned as if it were the whole ledger. */
	g_string_append(sql, " GROUP BY 1, 3, 4 ORDER BY 3, 1, 4 LIMIT ?");
	accounts_bind_add_int(bindings, (gint64)VENTURE_SERIES_MAX_ACCOUNT_ROWS + 1);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	totals = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_txn_total_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesTxnTotal *total;
		gint64 in;
		gint64 out_total;

		if (totals->len >= VENTURE_SERIES_MAX_ACCOUNT_ROWS)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "The ledger has more than %d buckets for that question; narrow it",
			            VENTURE_SERIES_MAX_ACCOUNT_ROWS);
			return NULL;
		}

		total = g_new0(VentureSeriesTxnTotal, 1);
		g_ptr_array_add(totals, total);
		total->key = series_column_strdup(stmt, 0);
		total->label = series_column_strdup(stmt, 1);
		total->period_start = series_column_figure(stmt, 2);
		series_column_currency(stmt, 3, total->currency);
		total->txns = sqlite3_column_int64(stmt, 4);
		total->sales = sqlite3_column_int64(stmt, 5);
		total->sold_units = sqlite3_column_int64(stmt, 6);
		total->sales_amount = sqlite3_column_int64(stmt, 7);
		total->buys = sqlite3_column_int64(stmt, 8);
		total->bought_units = sqlite3_column_int64(stmt, 9);
		total->buys_amount = sqlite3_column_int64(stmt, 10);
		total->income = sqlite3_column_int64(stmt, 11);
		total->expense = sqlite3_column_int64(stmt, 12);
		total->expired_units = sqlite3_column_int64(stmt, 13);
		total->cancelled_units = sqlite3_column_int64(stmt, 14);

		/* Money in less money out, in integers; every term is zero or
		 * more, so negating one cannot overflow. */
		if (!venture_series_math_add(total->sales_amount, total->income, &in) ||
		    !venture_series_math_add(total->buys_amount, total->expense, &out_total) ||
		    !venture_series_math_add(in, -out_total, &total->net))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A bucket of the ledger does not fit in a 64-bit integer");
			return NULL;
		}
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "summing the ledger", error);
		return NULL;
	}

	return g_steal_pointer(&totals);
}

/* ==========================================================================
 * What the holdings are worth
 * ========================================================================== */

static const gchar *const accounts_value_bases[] = {
	"conservative", "market", "min", "historical", "region_market", "region_sale_avg", NULL
};
static const gchar *const accounts_value_sorts[] = {
	"value", "quantity", "name", "unit_value", "accounts", "days_of_supply", NULL
};

/* How far back the region's newest sale average may be: the oracle's
 * historical window, so a figure from last season does not value today's
 * bags. */
#define ACCOUNTS_SALE_AVG_DAYS (60)

gboolean
venture_series_value_basis_from_string(
	const gchar		*name,
	VentureSeriesValueBasis	*out
){
	guint i;

	g_return_val_if_fail(NULL != out, FALSE);

	for (i = 0; (NULL != name) && (NULL != accounts_value_bases[i]); i++)
	{
		if (0 == strcmp(accounts_value_bases[i], name))
		{
			*out = (VentureSeriesValueBasis)i;
			return TRUE;
		}
	}

	return FALSE;
}

const gchar *
venture_series_value_basis_to_string(VentureSeriesValueBasis basis)
{
	if ((guint)basis >= G_N_ELEMENTS(accounts_value_bases) - 1)
		return "conservative";

	return accounts_value_bases[basis];
}

gboolean
venture_series_value_sort_from_string(
	const gchar		*name,
	VentureSeriesValueSort	*out
){
	guint i;

	g_return_val_if_fail(NULL != out, FALSE);

	for (i = 0; (NULL != name) && (NULL != accounts_value_sorts[i]); i++)
	{
		if (0 == strcmp(accounts_value_sorts[i], name))
		{
			*out = (VentureSeriesValueSort)i;
			return TRUE;
		}
	}

	return FALSE;
}

const gchar *
venture_series_value_sort_to_string(VentureSeriesValueSort sort)
{
	if ((guint)sort >= G_N_ELEMENTS(accounts_value_sorts) - 1)
		return "value";

	return accounts_value_sorts[sort];
}

void
venture_series_value_filter_init(VentureSeriesValueFilter *filter)
{
	g_return_if_fail(NULL != filter);

	memset(filter, 0, sizeof(*filter));
	filter->basis = VENTURE_SERIES_VALUE_CONSERVATIVE;
	filter->min_value = VENTURE_SERIES_NONE;
	filter->unsold_since = VENTURE_SERIES_NONE;
	filter->sort = VENTURE_SERIES_VALUE_SORT_VALUE;
	filter->descending = TRUE;
}

void
venture_series_valued_line_free(VentureSeriesValuedLine *line)
{
	if (NULL == line)
		return;

	g_free(line->account_key);
	g_free(line->account_name);
	g_free(line->account_venue);
	g_free(line->place);
	g_free(line->instrument_key);
	g_free(line->instrument_name);
	g_free(line->category);
	g_free(line);
}

void
venture_series_valued_instrument_free(VentureSeriesValuedInstrument *instrument)
{
	if (NULL == instrument)
		return;

	g_free(instrument->instrument_key);
	g_free(instrument->instrument_name);
	g_free(instrument->category);
	g_free(instrument);
}

/* The price of one unit under @basis, as an SQL expression over the
 * figures the lines carry. */
static const gchar *
accounts_unit_expr(VentureSeriesValueBasis basis)
{
	switch (basis)
	{
	case VENTURE_SERIES_VALUE_MARKET:
		return "COALESCE(realm_market, region_market)";
	case VENTURE_SERIES_VALUE_MIN:
		return "realm_min";
	case VENTURE_SERIES_VALUE_HISTORICAL:
		return "realm_historical";
	case VENTURE_SERIES_VALUE_REGION_MARKET:
		return "region_market";
	case VENTURE_SERIES_VALUE_REGION_SALE_AVG:
		return "region_sale_avg";
	case VENTURE_SERIES_VALUE_CONSERVATIVE:
	default:
		/* SQLite's two-argument MIN() is NULL when either is: say what
		 * one known figure alone means instead. */
		return "CASE WHEN region_sale_avg IS NULL THEN COALESCE(realm_market, region_market)"
		       " WHEN COALESCE(realm_market, region_market) IS NULL THEN region_sale_avg"
		       " WHEN region_sale_avg < COALESCE(realm_market, region_market) THEN region_sale_avg"
		       " ELSE COALESCE(realm_market, region_market) END";
	}
}

/*
 * The valued lines as common table expressions `lines`, `valued` and
 * `priced`, every value a bound parameter in the order it appears. The
 * figures are joined per line from the indexes the market half keeps:
 * current (venue, instrument), region (group, instrument, currency) and
 * daily's primary key for the newest sale average. A line's value is the
 * integer product, left unpriced when it would not fit.
 *
 * Units sold a day are the store's own estimate unless it is zero, then
 * the source's: a venue the source describes only by statistics has no
 * listings to diff, so the store's zero there says "no evidence", not
 * "nothing sells" -- the reference figures make the same substitution.
 */
static gboolean
accounts_value_cte(
	const VentureSeriesValueFilter	 *filter,
	const gchar *const		 *instrument_keys,
	GString				 *sql,
	GArray				 *bindings,
	GError				**error
){
	g_autofree gchar *currency = NULL;
	guint i;

	if ((NULL == filter) || venture_string_is_empty(filter->currency))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Holdings are valued in one currency: name it");
		return FALSE;
	}

	if ((guint)filter->basis >= G_N_ELEMENTS(accounts_value_bases) - 1)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Not a basis to value holdings on");
		return FALSE;
	}

	currency = g_ascii_strup(filter->currency, -1);

	g_string_append(sql,
		"WITH lines AS (SELECT a.id AS account_id, a.key AS account_key, a.name AS account_name,"
		"  a.venue_key AS account_venue, a.group_key AS account_group, a.login AS account_login,"
		"  h.place AS place,"
		"  h.instrument_key AS instrument_key, i.name AS instrument_name, i.name_fold AS name_fold,"
		"  i.category AS category, h.quantity AS quantity, h.at AS at,"
		"  c.min_price AS realm_min, c.market_value AS realm_market,"
		"  c.source_historical AS realm_historical,"
		"  COALESCE(NULLIF(c.sold_per_day, 0), c.source_sold_per_day, c.sold_per_day) AS realm_spd,"
		"  r.market_avg AS region_market,"
		"  (SELECT d.sale_avg FROM daily d WHERE d.venue_id = rv.id AND d.instrument_id = i.id"
		"     AND d.currency = ? AND d.sale_avg IS NOT NULL AND d.day >= ?"
		"     ORDER BY d.day DESC LIMIT 1) AS region_sale_avg,"
		"  COALESCE(NULLIF(rc.sold_per_day, 0), rc.source_sold_per_day, rc.sold_per_day) AS region_spd"
		" FROM account_holdings h"
		" JOIN accounts a ON a.id = h.account_id"
		" LEFT JOIN instruments i ON i.key = h.instrument_key"
		" LEFT JOIN venues v ON v.key = a.venue_key"
		" LEFT JOIN current c ON c.venue_id = v.id AND c.instrument_id = i.id AND c.currency = ?"
		" LEFT JOIN region r ON r.group_key = COALESCE(v.group_key, ?) AND r.instrument_id = i.id"
		"   AND r.currency = ?"
		" LEFT JOIN venues rv ON rv.key = COALESCE(?, 'region-' || lower(COALESCE(v.group_key, ?)))"
		" LEFT JOIN current rc ON rc.venue_id = rv.id AND rc.instrument_id = i.id AND rc.currency = ?"
		" WHERE 1");
	accounts_bind_add_text(bindings, currency);
	accounts_bind_add_int(bindings, (filter->now / 86400) - ACCOUNTS_SALE_AVG_DAYS);
	accounts_bind_add_text(bindings, currency);
	accounts_bind_add_text(bindings, filter->default_group);
	accounts_bind_add_text(bindings, currency);
	accounts_bind_add_text(bindings, filter->region_venue);
	accounts_bind_add_text(bindings, filter->default_group);
	accounts_bind_add_text(bindings, currency);

	if (NULL != filter->account_key)
	{
		g_string_append(sql, " AND a.key = ?");
		accounts_bind_add_text(bindings, filter->account_key);
	}

	if (NULL != filter->login_key)
	{
		g_string_append(sql, " AND a.login = ?");
		accounts_bind_add_text(bindings, filter->login_key);
	}

	if (NULL != filter->place)
	{
		if (!accounts_check_choice(filter->place, accounts_places, "place", error))
			return FALSE;

		g_string_append(sql, " AND h.place = ?");
		accounts_bind_add_text(bindings, filter->place);
	}

	if (NULL != filter->exclude_place)
	{
		g_string_append(sql, " AND h.place <> ?");
		accounts_bind_add_text(bindings, filter->exclude_place);
	}

	if ((NULL != filter->search) && ('\0' != filter->search[0]))
	{
		g_autofree gchar *fold = NULL;
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *pattern = NULL;

		if (!g_utf8_validate(filter->search, -1, NULL))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "A search must be UTF-8");
			return FALSE;
		}

		fold = g_utf8_casefold(filter->search, -1);
		escaped = accounts_like_escape(fold);
		pattern = g_strdup_printf("%%%s%%", escaped);
		g_string_append(sql, " AND (i.name_fold LIKE ? ESCAPE '\\'"
		                     " OR h.instrument_key LIKE ? ESCAPE '\\')");
		accounts_bind_add_text(bindings, pattern);
		accounts_bind_add_text(bindings, pattern);
	}

	if ((NULL != filter->category_prefix) && ('\0' != filter->category_prefix[0]))
	{
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *pattern = NULL;

		escaped = accounts_like_escape(filter->category_prefix);
		pattern = g_strdup_printf("%s/%%", escaped);
		g_string_append(sql, " AND (i.category = ? OR i.category LIKE ? ESCAPE '\\')");
		accounts_bind_add_text(bindings, filter->category_prefix);
		accounts_bind_add_text(bindings, pattern);
	}

	/* Dead stock: nothing of it sold since then, read on the ledger's
	 * (instrument_key, at) index. */
	if (VENTURE_SERIES_NONE != filter->unsold_since)
	{
		g_string_append(sql, " AND NOT EXISTS (SELECT 1 FROM external_txns t"
		                     " WHERE t.instrument_key = h.instrument_key AND t.kind = 'sale'"
		                     " AND t.at >= ?)");
		accounts_bind_add_int(bindings, filter->unsold_since);
	}

	if (NULL != instrument_keys)
	{
		g_string_append(sql, " AND h.instrument_key IN (''");

		for (i = 0; NULL != instrument_keys[i]; i++)
		{
			g_string_append(sql, ", ?");
			accounts_bind_add_text(bindings, instrument_keys[i]);
		}

		g_string_append(sql, ")");
	}

	g_string_append_printf(sql,
		"), valued AS (SELECT lines.*, (%s) AS unit_value FROM lines),"
		" priced AS (SELECT valued.*, CASE WHEN unit_value IS NULL OR unit_value < 0 THEN NULL"
		"   WHEN unit_value > 0 AND quantity > 9223372036854775807 / unit_value THEN NULL"
		"   ELSE quantity * unit_value END AS line_value FROM valued)",
		accounts_unit_expr(filter->basis));

	return TRUE;
}

/* The per-instrument sums of the valued lines, as a SELECT after the
 * CTE; the minimum is a HAVING, so it bounds instruments, not lines. */
static void
accounts_value_grouped(
	const VentureSeriesValueFilter	*filter,
	GString				*sql,
	GArray				*bindings
){
	g_string_append(sql,
		" SELECT instrument_key, MAX(instrument_name) AS instrument_name, MAX(category) AS category,"
		"  SUM(quantity) AS total_quantity, COUNT(DISTINCT account_id) AS holders,"
		"  COUNT(*) AS n_lines, COUNT(line_value) AS n_priced,"
		"  SUM(CASE WHEN line_value IS NOT NULL THEN quantity ELSE 0 END) AS priced_quantity,"
		"  SUM(line_value) AS total_value,"
		"  COALESCE(MAX(region_spd), MAX(realm_spd)) AS spd,"
		"  (SELECT MAX(t.at) FROM external_txns t WHERE t.instrument_key = priced.instrument_key"
		"     AND t.kind = 'sale') AS last_sale,"
		"  MAX(name_fold) AS sort_name"
		" FROM priced GROUP BY instrument_key");

	if (VENTURE_SERIES_NONE != filter->min_value)
	{
		g_string_append(sql, " HAVING SUM(line_value) >= ?");
		accounts_bind_add_int(bindings, filter->min_value);
	}
}

GPtrArray *
venture_series_store_value_instruments(
	VentureSeriesStore			 *self,
	const VentureSeriesValueFilter		 *filter,
	VentureSeriesValueTotals		 *out_totals,
	GError					**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *order;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (NULL != out_totals)
		memset(out_totals, 0, sizeof(*out_totals));

	/* The totals of the whole answer first, over the same grouping, so
	 * a page and its "of N" can never disagree. */
	if (NULL != out_totals)
	{
		sql = g_string_new(NULL);
		bindings = accounts_bindings_new();

		if (!accounts_value_cte(filter, NULL, sql, bindings, error))
			return NULL;

		g_string_append(sql, " SELECT COUNT(*), COALESCE(SUM(n_lines), 0), COALESCE(SUM(n_priced), 0),"
		                     "  COALESCE(SUM(total_quantity), 0), COALESCE(SUM(total_value), 0)"
		                     " FROM (");
		accounts_value_grouped(filter, sql, bindings);
		g_string_append(sql, ")");

		stmt = accounts_prepare_bound(self, sql->str, bindings, error);
		if (NULL == stmt)
			return NULL;

		rc = sqlite3_step(stmt);

		if (SQLITE_ROW != rc)
		{
			venture_series_store_internal_sqlite_error(self, rc, "valuing the holdings", error);
			return NULL;
		}

		out_totals->instruments = sqlite3_column_int64(stmt, 0);
		out_totals->lines = sqlite3_column_int64(stmt, 1);
		out_totals->priced_lines = sqlite3_column_int64(stmt, 2);
		out_totals->quantity = sqlite3_column_int64(stmt, 3);
		out_totals->value = sqlite3_column_int64(stmt, 4);
		g_clear_pointer(&stmt, series_owned_stmt_free);
		g_string_truncate(sql, 0);
		g_clear_pointer(&bindings, g_array_unref);
	}

	if (NULL == sql)
		sql = g_string_new(NULL);

	bindings = accounts_bindings_new();

	if (!accounts_value_cte(filter, NULL, sql, bindings, error))
		return NULL;

	accounts_value_grouped(filter, sql, bindings);

	switch (filter->sort)
	{
	case VENTURE_SERIES_VALUE_SORT_QUANTITY:
		order = "total_quantity";
		break;
	case VENTURE_SERIES_VALUE_SORT_NAME:
		order = "COALESCE(sort_name, instrument_key)";
		break;
	case VENTURE_SERIES_VALUE_SORT_UNIT_VALUE:
		order = "(CASE WHEN priced_quantity > 0 THEN total_value * 1.0 / priced_quantity END)";
		break;
	case VENTURE_SERIES_VALUE_SORT_ACCOUNTS:
		order = "holders";
		break;
	case VENTURE_SERIES_VALUE_SORT_DAYS_OF_SUPPLY:
		order = "(CASE WHEN spd > 0 THEN total_quantity / spd END)";
		break;
	case VENTURE_SERIES_VALUE_SORT_VALUE:
		order = "total_value";
		break;
	default:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Not a way to sort holdings");
		return NULL;
	}

	/* Unknown last whichever way: "x IS NULL" sorts FALSE first. */
	g_string_append_printf(sql, " ORDER BY %s IS NULL, %s %s, instrument_key LIMIT ? OFFSET ?",
	                       order, order, filter->descending ? "DESC" : "ASC");
	accounts_bind_add_int(bindings, accounts_page(filter->count, VENTURE_SERIES_DEFAULT_PAGE));
	accounts_bind_add_int(bindings, filter->offset);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_valued_instrument_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesValuedInstrument *row = g_new0(VentureSeriesValuedInstrument, 1);

		g_ptr_array_add(rows, row);
		row->instrument_key = series_column_strdup(stmt, 0);
		row->instrument_name = series_column_strdup(stmt, 1);
		row->category = series_column_strdup(stmt, 2);
		row->quantity = sqlite3_column_int64(stmt, 3);
		row->accounts = sqlite3_column_int64(stmt, 4);
		row->lines = sqlite3_column_int64(stmt, 5);
		row->priced_lines = sqlite3_column_int64(stmt, 6);
		row->priced_quantity = sqlite3_column_int64(stmt, 7);
		row->value = series_column_figure(stmt, 8);
		row->sold_per_day = (SQLITE_NULL == sqlite3_column_type(stmt, 9))
			? NAN : sqlite3_column_double(stmt, 9);
		row->last_sale = series_column_figure(stmt, 10);
		row->unit_value = ((VENTURE_SERIES_NONE != row->value) && (row->priced_quantity > 0) &&
		                   (row->value >= 0))
			? venture_series_math_div_round(row->value, row->priced_quantity)
			: VENTURE_SERIES_NONE;
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "valuing the holdings", error);
		return NULL;
	}

	return g_steal_pointer(&rows);
}

GPtrArray *
venture_series_store_value_lines(
	VentureSeriesStore			 *self,
	const VentureSeriesValueFilter		 *filter,
	const gchar *const			 *instrument_keys,
	GError					**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gboolean all;
	guint bound;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	sql = g_string_new(NULL);
	bindings = accounts_bindings_new();

	if (!accounts_value_cte(filter, instrument_keys, sql, bindings, error))
		return NULL;

	g_string_append(sql, " SELECT account_key, account_name, account_venue, place, instrument_key,"
	                     "  instrument_name, category, quantity, unit_value, line_value, at"
	                     " FROM priced ORDER BY account_group, COALESCE(account_name, account_key),"
	                     "  account_key, place, COALESCE(name_fold, instrument_key), instrument_key"
	                     " LIMIT ?");
	bound = (guint)venture_series_accounts_get_max_rows();
	accounts_bind_add_int(bindings, accounts_limit(0, bound, &all));

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_valued_line_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesValuedLine *line = g_new0(VentureSeriesValuedLine, 1);

		g_ptr_array_add(rows, line);
		line->account_key = series_column_strdup(stmt, 0);
		line->account_name = series_column_strdup(stmt, 1);
		line->account_venue = series_column_strdup(stmt, 2);
		line->place = series_column_strdup(stmt, 3);
		line->instrument_key = series_column_strdup(stmt, 4);
		line->instrument_name = series_column_strdup(stmt, 5);
		line->category = series_column_strdup(stmt, 6);
		line->quantity = sqlite3_column_int64(stmt, 7);
		line->unit_value = series_column_figure(stmt, 8);
		line->value = series_column_figure(stmt, 9);
		line->at = sqlite3_column_int64(stmt, 10);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "valuing holding lines", error);
		return NULL;
	}

	if (!accounts_check_whole(rows, all, bound, "holding lines", "an account or an instrument",
	                          error))
		return NULL;

	return g_steal_pointer(&rows);
}

void
venture_series_value_group_total_free(VentureSeriesValueGroupTotal *total)
{
	if (NULL == total)
		return;

	g_free(total->key);
	g_free(total->label);
	g_free(total);
}

GPtrArray *
venture_series_store_value_totals(
	VentureSeriesStore			 *self,
	const VentureSeriesValueFilter		 *filter,
	VentureSeriesValueGroup			  group,
	GError					**error
){
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint bound;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if ((VENTURE_SERIES_VALUE_GROUP_ACCOUNT != group) && (VENTURE_SERIES_VALUE_GROUP_LOGIN != group))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Not a way to group valued holdings");
		return NULL;
	}

	sql = g_string_new(NULL);
	bindings = accounts_bindings_new();

	if (!accounts_value_cte(filter, NULL, sql, bindings, error))
		return NULL;

	/* The same priced lines the instrument totals sum, grouped another
	 * way: SUM over integers, which SQLite refuses to overflow. */
	if (VENTURE_SERIES_VALUE_GROUP_LOGIN == group)
		g_string_append(sql, " SELECT p.account_login, MAX(l.name), COUNT(*), COUNT(p.line_value),"
		                     "  SUM(p.quantity), COALESCE(SUM(p.line_value), 0)"
		                     " FROM priced p LEFT JOIN logins l ON l.key = p.account_login"
		                     " GROUP BY p.account_login ORDER BY p.account_login LIMIT ?");
	else
		g_string_append(sql, " SELECT p.account_key, MAX(p.account_name), COUNT(*),"
		                     "  COUNT(p.line_value), SUM(p.quantity), COALESCE(SUM(p.line_value), 0)"
		                     " FROM priced p GROUP BY p.account_id ORDER BY p.account_key LIMIT ?");

	bound = (guint)venture_series_accounts_get_max_accounts();
	accounts_bind_add_int(bindings, (gint64)bound + 1);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_value_group_total_free);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesValueGroupTotal *total = g_new0(VentureSeriesValueGroupTotal, 1);

		g_ptr_array_add(rows, total);
		total->key = series_column_strdup(stmt, 0);
		total->label = series_column_strdup(stmt, 1);
		total->lines = sqlite3_column_int64(stmt, 2);
		total->priced_lines = sqlite3_column_int64(stmt, 3);
		total->quantity = sqlite3_column_int64(stmt, 4);
		total->value = sqlite3_column_int64(stmt, 5);

		if (NULL == total->key)
			total->key = g_strdup("");
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "valuing the holdings", error);
		return NULL;
	}

	if (rows->len > bound)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The holdings span more than %u accounts; narrow the read", bound);
		return NULL;
	}

	return g_steal_pointer(&rows);
}

/* ==========================================================================
 * Balances by day
 * ========================================================================== */

void
venture_series_balance_day_free(VentureSeriesBalanceDay *day)
{
	if (NULL == day)
		return;

	g_free(day->account_key);
	g_free(day);
}

/*
 * SQLite answers a bare column beside MAX() from the row that holds the
 * maximum, which is exactly "the last point of each day": one grouped
 * statement instead of every point a busy account stored.
 */
static const gchar accounts_sql_balance_openings[] =
	"SELECT a.key, b.amount, MAX(b.at) FROM account_balances b"
	" JOIN accounts a ON a.id = b.account_id"
	" WHERE b.currency = ?1 AND b.at < ?2 GROUP BY b.account_id";

static const gchar accounts_sql_balance_days[] =
	"SELECT a.key, b.at / 86400, b.amount, MAX(b.at) FROM account_balances b"
	" JOIN accounts a ON a.id = b.account_id"
	" WHERE b.currency = ?1 AND b.at >= ?2 AND b.at < ?3"
	" GROUP BY b.account_id, b.at / 86400";

static gint
accounts_compare_balance_days(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureSeriesBalanceDay *one = *(VentureSeriesBalanceDay *const *)a;
	const VentureSeriesBalanceDay *two = *(VentureSeriesBalanceDay *const *)b;
	gint by_key;

	by_key = g_strcmp0(one->account_key, two->account_key);

	if (0 != by_key)
		return by_key;

	return (one->day_start < two->day_start) ? -1 : (one->day_start > two->day_start);
}

GPtrArray *
venture_series_store_balance_days(
	VentureSeriesStore	 *self,
	const gchar		 *currency,
	gint64			  since,
	gint64			  until,
	GError			**error
){
	g_autoptr(GPtrArray) days = NULL;
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	g_autofree gchar *code = NULL;
	gint64 first_day;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (venture_string_is_empty(currency) || (until <= since))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Balances by day need a currency and a window");
		return NULL;
	}

	code = g_ascii_strup(currency, -1);
	first_day = (since / 86400) * 86400;
	days = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_balance_day_free);

	stmt = venture_series_store_internal_stmt(self, accounts_sql_balance_openings, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, code);
	sqlite3_bind_int64(stmt, 2, first_day);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesBalanceDay *day = g_new0(VentureSeriesBalanceDay, 1);

		day->account_key = series_column_strdup(stmt, 0);
		day->amount = sqlite3_column_int64(stmt, 1);
		day->day_start = first_day - 86400;
		g_ptr_array_add(days, day);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "reading opening balances", error);
		return NULL;
	}

	g_clear_pointer(&stmt, series_cached_stmt_release);
	stmt = venture_series_store_internal_stmt(self, accounts_sql_balance_days, error);
	if (NULL == stmt)
		return NULL;

	series_bind_text(stmt, 1, code);
	sqlite3_bind_int64(stmt, 2, first_day);
	sqlite3_bind_int64(stmt, 3, until);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		VentureSeriesBalanceDay *day = g_new0(VentureSeriesBalanceDay, 1);

		day->account_key = series_column_strdup(stmt, 0);
		day->day_start = sqlite3_column_int64(stmt, 1) * 86400;
		day->amount = sqlite3_column_int64(stmt, 2);
		g_ptr_array_add(days, day);
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "reading balances by day", error);
		return NULL;
	}

	g_ptr_array_sort(days, accounts_compare_balance_days);

	return g_steal_pointer(&days);
}

/* ==========================================================================
 * Flips
 * ========================================================================== */

void
venture_series_flip_free(VentureSeriesFlip *flip)
{
	if (NULL == flip)
		return;

	g_free(flip->instrument_key);
	g_free(flip->instrument_name);
	g_free(flip);
}

/*
 * @amount * @part / @whole, rounded half to even, through 128 bits so no
 * product overflows; 0 <= @part <= @whole, @whole >= 1, @amount >= 0.
 * Taking a share and subtracting it from what is left is what makes the
 * shares of one buy's cost add back up to exactly that cost.
 */
static gint64
accounts_share(
	gint64	amount,
	gint64	part,
	gint64	whole
){
	unsigned __int128 product;
	unsigned __int128 quotient;
	unsigned __int128 remainder;

	if (part >= whole)
		return amount;

	product = (unsigned __int128)amount * (unsigned __int128)part;
	quotient = product / (unsigned __int128)whole;
	remainder = product % (unsigned __int128)whole;

	if ((remainder * 2 > (unsigned __int128)whole) ||
	    ((remainder * 2 == (unsigned __int128)whole) && (1 == (quotient & 1))))
		quotient++;

	return (gint64)quotient;
}

typedef struct
{
	gint64	units;
	gint64	amount;
} AccountsLot;

static gint
accounts_compare_flips(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureSeriesFlip *one = *(VentureSeriesFlip *const *)a;
	const VentureSeriesFlip *two = *(VentureSeriesFlip *const *)b;

	if (one->profit != two->profit)
		return (one->profit > two->profit) ? -1 : 1;

	return g_strcmp0(one->instrument_key, two->instrument_key);
}

static const gchar accounts_sql_held[] =
	"SELECT COALESCE(SUM(h.quantity), 0) FROM account_holdings h"
	" WHERE h.instrument_key = ?1 AND h.place <> 'currency'"
	"   AND (?2 IS NULL OR h.account_id = (SELECT id FROM accounts WHERE key = ?2))"
	"   AND (?3 IS NULL OR h.account_id IN (SELECT id FROM accounts WHERE login = ?3))";

/* Adds @value to *@total, refusing an overflow by name. */
static gboolean
accounts_flip_add(
	gint64		 *total,
	gint64		  value,
	GError		**error
){
	if (!venture_series_math_add(*total, value, total))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A flip's total does not fit in a 64-bit integer");
		return FALSE;
	}

	return TRUE;
}

/* Closes one instrument's walk: what is still open, and what it holds. */
static gboolean
accounts_flip_finish(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	VentureSeriesFlip		 *flip,
	GArray				 *lots,
	GPtrArray			 *flips,
	GError				**error
){
	g_autoptr(SeriesCachedStmt) stmt = NULL;
	guint i;
	gint rc;

	/* Sold with nothing bought in the window is not a flip. */
	if (0 == flip->bought_units)
	{
		g_array_set_size(lots, 0);
		venture_series_flip_free(flip);
		return TRUE;
	}

	/* The answer owns it from here, whatever fails next. */
	g_ptr_array_add(flips, flip);

	for (i = 0; i < lots->len; i++)
	{
		AccountsLot *lot = &g_array_index(lots, AccountsLot, i);

		if (!accounts_flip_add(&flip->open_units, lot->units, error) ||
		    !accounts_flip_add(&flip->open_cost, lot->amount, error))
			return FALSE;
	}

	g_array_set_size(lots, 0);
	flip->profit = flip->proceeds - flip->cost;
	stmt = venture_series_store_internal_stmt(self, accounts_sql_held, error);
	if (NULL == stmt)
		return FALSE;

	series_bind_text(stmt, 1, flip->instrument_key);
	series_bind_text(stmt, 2, (NULL != filter) ? filter->account_key : NULL);
	series_bind_text(stmt, 3, (NULL != filter) ? filter->login_key : NULL);
	rc = sqlite3_step(stmt);

	if (SQLITE_ROW != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "reading what is held", error);
		return FALSE;
	}

	flip->held = sqlite3_column_int64(stmt, 0);

	return TRUE;
}

GPtrArray *
venture_series_store_flips(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	GError				**error
){
	VentureSeriesTxnFilter rows_filter;
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) flips = NULL;
	g_autoptr(GArray) lots = NULL;
	VentureSeriesFlip *flip = NULL;
	gint64 read = 0;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (NULL != filter)
		rows_filter = *filter;
	else
		venture_series_txn_filter_init(&rows_filter);

	rows_filter.kind = NULL;

	sql = g_string_new("SELECT t.instrument_key, i.name, COALESCE(t.currency, ''), t.kind,"
	                   "  t.quantity, t.amount, t.at"
	                   " FROM external_txns t LEFT JOIN instruments i ON i.key = t.instrument_key");
	bindings = accounts_bindings_new();

	if (!accounts_txn_where(&rows_filter, sql, bindings, error))
		return NULL;

	/* A buy and a sale at the same second: the buy first, or a flip done
	 * within one snapshot would be "sold before it was bought". */
	g_string_append(sql, " AND t.kind IN ('buy', 'sale') AND t.instrument_key IS NOT NULL"
	                     " AND t.quantity > 0 AND t.amount IS NOT NULL"
	                     " ORDER BY t.instrument_key, t.currency, t.at, t.kind = 'sale', t.key"
	                     " LIMIT ?");
	accounts_bind_add_int(bindings, (gint64)VENTURE_SERIES_MAX_ACCOUNT_ROWS + 1);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	flips = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_flip_free);
	lots = g_array_new(FALSE, FALSE, sizeof(AccountsLot));

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		const gchar *key = (const gchar *)sqlite3_column_text(stmt, 0);
		const gchar *currency = (const gchar *)sqlite3_column_text(stmt, 2);
		const gchar *kind = (const gchar *)sqlite3_column_text(stmt, 3);
		gint64 quantity = sqlite3_column_int64(stmt, 4);
		gint64 amount = sqlite3_column_int64(stmt, 5);
		gint64 at = sqlite3_column_int64(stmt, 6);

		if (++read > VENTURE_SERIES_MAX_ACCOUNT_ROWS)
		{
			venture_series_flip_free(flip);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "More than %d buys and sales in that window; narrow it to match flips",
			            VENTURE_SERIES_MAX_ACCOUNT_ROWS);
			return NULL;
		}

		/* A new instrument (or currency) closes the one before. */
		if ((NULL == flip) || (0 != g_strcmp0(flip->instrument_key, key)) ||
		    (0 != g_ascii_strcasecmp(flip->currency, (NULL != currency) ? currency : "")))
		{
			if ((NULL != flip) && !accounts_flip_finish(self, filter, flip, lots, flips, error))
				return NULL;

			flip = g_new0(VentureSeriesFlip, 1);
			flip->instrument_key = g_strdup(key);
			flip->instrument_name = series_column_strdup(stmt, 1);
			g_strlcpy(flip->currency, (NULL != currency) ? currency : "", sizeof(flip->currency));
			flip->first_buy = VENTURE_SERIES_NONE;
			flip->last_sale = VENTURE_SERIES_NONE;
		}

		if (amount < 0)
			continue;

		if (0 == g_strcmp0(kind, "buy"))
		{
			AccountsLot lot;

			lot.units = quantity;
			lot.amount = amount;
			g_array_append_val(lots, lot);

			if (VENTURE_SERIES_NONE == flip->first_buy)
				flip->first_buy = at;

			if (!accounts_flip_add(&flip->bought_units, quantity, error) ||
			    !accounts_flip_add(&flip->bought_amount, amount, error))
			{
				venture_series_flip_free(flip);
				return NULL;
			}

			continue;
		}

		/* A sale: take units from the oldest buys before it. */
		{
			gint64 units_left = quantity;
			gint64 amount_left = amount;

			while ((units_left > 0) && (lots->len > 0))
			{
				AccountsLot *lot = &g_array_index(lots, AccountsLot, 0);
				gint64 take = MIN(units_left, lot->units);
				gint64 cost = accounts_share(lot->amount, take, lot->units);
				gint64 proceeds = accounts_share(amount_left, take, units_left);

				lot->amount -= cost;
				lot->units -= take;
				amount_left -= proceeds;
				units_left -= take;

				if (!accounts_flip_add(&flip->matched_units, take, error) ||
				    !accounts_flip_add(&flip->cost, cost, error) ||
				    !accounts_flip_add(&flip->proceeds, proceeds, error))
				{
					venture_series_flip_free(flip);
					return NULL;
				}

				flip->last_sale = at;

				if (0 == lot->units)
					g_array_remove_index(lots, 0);
			}

			if (!accounts_flip_add(&flip->unmatched_sold_units, units_left, error))
			{
				venture_series_flip_free(flip);
				return NULL;
			}
		}
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_flip_free(flip);
		venture_series_store_internal_sqlite_error(self, rc, "matching flips", error);
		return NULL;
	}

	g_clear_pointer(&stmt, series_owned_stmt_free);

	if ((NULL != flip) && !accounts_flip_finish(self, filter, flip, lots, flips, error))
		return NULL;

	g_ptr_array_sort(flips, accounts_compare_flips);

	return g_steal_pointer(&flips);
}

/* --- Flip pairs ----------------------------------------------------------------- */

void
venture_series_flip_pair_free(VentureSeriesFlipPair *pair)
{
	if (NULL == pair)
		return;

	g_free(pair->instrument_key);
	g_free(pair->instrument_name);
	g_free(pair->buy_key);
	g_free(pair->buy_account);
	g_free(pair->buy_venue);
	g_free(pair->sale_key);
	g_free(pair->sale_account);
	g_free(pair->sale_venue);
	g_free(pair);
}

/* One buy still holding units, with what identifies it. */
typedef struct
{
	gchar	*key;
	gchar	*account;
	gchar	*venue;
	gint64	 at;
	gint64	 units;
	gint64	 amount;
} AccountsPairLot;

static void
accounts_pair_lot_clear(gpointer data)
{
	AccountsPairLot *lot = data;

	g_free(lot->key);
	g_free(lot->account);
	g_free(lot->venue);
}

/*
 * What is left of a row once its recorded share is taken off: never below
 * zero, and the money that goes with the units still unused is exactly
 * what the recorded shares did not take.
 */
static void
accounts_pair_unused(
	GHashTable	*used,
	const gchar	*key,
	gint64		*units,
	gint64		*amount
){
	const VentureSeriesFlipUse *use;

	use = ((NULL != used) && (NULL != key)) ? g_hash_table_lookup(used, key) : NULL;

	if (NULL == use)
		return;

	*units = MAX((gint64)0, *units - MAX((gint64)0, use->units));
	*amount = MAX((gint64)0, *amount - MAX((gint64)0, use->amount));

	if (0 == *units)
		*amount = 0;
}

GPtrArray *
venture_series_store_flip_pairs(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	GHashTable			 *used,
	GError				**error
){
	VentureSeriesTxnFilter rows_filter;
	g_autoptr(GString) sql = NULL;
	g_autoptr(GArray) bindings = NULL;
	g_autoptr(SeriesOwnedStmt) stmt = NULL;
	g_autoptr(GPtrArray) pairs = NULL;
	g_autoptr(GArray) lots = NULL;
	g_autofree gchar *instrument = NULL;
	g_autofree gchar *instrument_name = NULL;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64 read = 0;
	gint rc;

	g_return_val_if_fail(VENTURE_IS_SERIES_STORE(self), NULL);

	if (NULL != filter)
		rows_filter = *filter;
	else
		venture_series_txn_filter_init(&rows_filter);

	rows_filter.kind = NULL;
	currency[0] = '\0';

	/* The statement of venture_series_store_flips(), with the keys that
	 * name each row: the same rows in the same order is what makes the
	 * pairs add up to its figures. */
	sql = g_string_new("SELECT t.instrument_key, i.name, COALESCE(t.currency, ''), t.kind,"
	                   "  t.quantity, t.amount, t.at, t.key, a.key, t.venue_key"
	                   " FROM external_txns t LEFT JOIN instruments i ON i.key = t.instrument_key"
	                   " LEFT JOIN accounts a ON a.id = t.account_id");
	bindings = accounts_bindings_new();

	if (!accounts_txn_where(&rows_filter, sql, bindings, error))
		return NULL;

	g_string_append(sql, " AND t.kind IN ('buy', 'sale') AND t.instrument_key IS NOT NULL"
	                     " AND t.quantity > 0 AND t.amount IS NOT NULL"
	                     " ORDER BY t.instrument_key, t.currency, t.at, t.kind = 'sale', t.key"
	                     " LIMIT ?");
	accounts_bind_add_int(bindings, (gint64)VENTURE_SERIES_MAX_ACCOUNT_ROWS + 1);

	stmt = accounts_prepare_bound(self, sql->str, bindings, error);
	if (NULL == stmt)
		return NULL;

	pairs = g_ptr_array_new_with_free_func((GDestroyNotify)venture_series_flip_pair_free);
	lots = g_array_new(FALSE, FALSE, sizeof(AccountsPairLot));
	g_array_set_clear_func(lots, accounts_pair_lot_clear);

	while (SQLITE_ROW == (rc = sqlite3_step(stmt)))
	{
		const gchar *key = (const gchar *)sqlite3_column_text(stmt, 0);
		const gchar *row_currency = (const gchar *)sqlite3_column_text(stmt, 2);
		const gchar *kind = (const gchar *)sqlite3_column_text(stmt, 3);
		const gchar *row_key = (const gchar *)sqlite3_column_text(stmt, 7);
		gint64 quantity = sqlite3_column_int64(stmt, 4);
		gint64 amount = sqlite3_column_int64(stmt, 5);
		gint64 at = sqlite3_column_int64(stmt, 6);

		if (NULL == row_currency)
			row_currency = "";

		if (++read > VENTURE_SERIES_MAX_ACCOUNT_ROWS)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "More than %d buys and sales in that window; narrow it to match flips",
			            VENTURE_SERIES_MAX_ACCOUNT_ROWS);
			return NULL;
		}

		/* A new instrument (or currency) closes the one before: its
		 * unsold lots are not this one's. */
		if ((NULL == instrument) || (0 != g_strcmp0(instrument, key)) ||
		    (0 != g_ascii_strcasecmp(currency, row_currency)))
		{
			g_array_set_size(lots, 0);
			g_free(instrument);
			g_free(instrument_name);
			instrument = g_strdup(key);
			instrument_name = series_column_strdup(stmt, 1);
			g_strlcpy(currency, row_currency, sizeof(currency));
		}

		if (amount < 0)
			continue;

		accounts_pair_unused(used, row_key, &quantity, &amount);

		if (0 == g_strcmp0(kind, "buy"))
		{
			AccountsPairLot lot;

			/* A buy recorded flips used up is no lot any more; a lot
			 * of no units would be taken from forever. */
			if (quantity <= 0)
				continue;

			lot.key = g_strdup(row_key);
			lot.account = series_column_strdup(stmt, 8);
			lot.venue = series_column_strdup(stmt, 9);
			lot.at = at;
			lot.units = quantity;
			lot.amount = amount;
			g_array_append_val(lots, lot);
			continue;
		}

		/* A sale: take units from the oldest buys before it, exactly as
		 * venture_series_store_flips() does, one pair per take. */
		{
			gint64 units_left = quantity;
			gint64 amount_left = amount;

			while ((units_left > 0) && (lots->len > 0))
			{
				AccountsPairLot *lot = &g_array_index(lots, AccountsPairLot, 0);
				VentureSeriesFlipPair *pair;
				gint64 take = MIN(units_left, lot->units);
				gint64 cost = accounts_share(lot->amount, take, lot->units);
				gint64 proceeds = accounts_share(amount_left, take, units_left);

				lot->amount -= cost;
				lot->units -= take;
				amount_left -= proceeds;
				units_left -= take;

				pair = g_new0(VentureSeriesFlipPair, 1);
				pair->instrument_key = g_strdup(instrument);
				pair->instrument_name = g_strdup(instrument_name);
				g_strlcpy(pair->currency, currency, sizeof(pair->currency));
				pair->buy_key = g_strdup(lot->key);
				pair->buy_account = g_strdup(lot->account);
				pair->buy_venue = g_strdup(lot->venue);
				pair->buy_at = lot->at;
				pair->sale_key = g_strdup(row_key);
				pair->sale_account = series_column_strdup(stmt, 8);
				pair->sale_venue = series_column_strdup(stmt, 9);
				pair->sale_at = at;
				pair->units = take;
				pair->cost = cost;
				pair->proceeds = proceeds;
				g_ptr_array_add(pairs, pair);

				if (0 == lot->units)
					g_array_remove_index(lots, 0);
			}
		}
	}

	if (SQLITE_DONE != rc)
	{
		venture_series_store_internal_sqlite_error(self, rc, "matching flips", error);
		return NULL;
	}

	return g_steal_pointer(&pairs);
}
