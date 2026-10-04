/*
 * venture-series-accounts.h - The operator's accounts, in a series store
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A market is two things: what everybody offers, which the rest of the
 * series store keeps, and what the operator owns in it -- the characters,
 * seller accounts, shared and guild banks the operator plays or trades
 * with ("accounts"), their money over time, what they hold and where,
 * the listings they have up ("positions"), what waits for them to collect
 * ("inbound", mail), and the source's ledger of what they bought, sold,
 * earned and spent ("external transactions"). This header is that second
 * half. It is game-agnostic on purpose: a place is "bag" or "warbank" or
 * "other", an account kind is "character" or "shared", and nothing here
 * knows what a realm is.
 *
 * The same rules as the rest of the store hold: hand-written SQL in the
 * source's own file, outside VentureDatabase (see docs/market-data.org);
 * one writer handle on the feeds worker, readers anywhere; times are Unix
 * seconds UTC; money is integer minor units beside its currency code,
 * never a double; "none" is %VENTURE_SERIES_NONE, never zero.
 *
 * How state is replaced. An account's holdings, positions and inbound are
 * what the source last said they were. A batch that carries an account
 * snapshot covering a kind restates that kind for that account in full:
 * every row of it the batch does not restate is gone. A kind the snapshot
 * does not cover, and every account the batch does not snapshot, is left
 * exactly as it was; rows outside a snapshot are upserts. Balances are
 * history rather than state: a covered balance that is not restated
 * drops to zero at the snapshot's time. A snapshot older than the one a
 * kind was last replaced by is stale and changes nothing of that kind.
 *
 * Only built with SQLite (VENTURE_HAVE_SQLITE).
 */

#ifndef VENTURE_SERIES_ACCOUNTS_H
#define VENTURE_SERIES_ACCOUNTS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

#include "series/venture-series-store.h"

G_BEGIN_DECLS

/**
 * VENTURE_SERIES_MAX_ACCOUNTS:
 *
 * The most accounts one read returns. An operator with more characters
 * than this is not one operator.
 */
#define VENTURE_SERIES_MAX_ACCOUNTS (10000)

/**
 * VENTURE_SERIES_MAX_ACCOUNT_ROWS:
 *
 * The most rows one read of holdings, positions, inbound or the ledger
 * returns; a page asks for less and pages.
 */
#define VENTURE_SERIES_MAX_ACCOUNT_ROWS (100000)

/**
 * VentureSeriesCovers:
 * @VENTURE_SERIES_COVERS_NONE: nothing
 * @VENTURE_SERIES_COVERS_HOLDINGS: what the account holds, by place
 * @VENTURE_SERIES_COVERS_POSITIONS: its open listings
 * @VENTURE_SERIES_COVERS_INBOUND: what waits for it to collect
 * @VENTURE_SERIES_COVERS_BALANCES: its money, every currency
 *
 * Which of an account's kinds a snapshot restates in full.
 */
typedef enum
{
	VENTURE_SERIES_COVERS_NONE = 0,
	VENTURE_SERIES_COVERS_HOLDINGS = 1 << 0,
	VENTURE_SERIES_COVERS_POSITIONS = 1 << 1,
	VENTURE_SERIES_COVERS_INBOUND = 1 << 2,
	VENTURE_SERIES_COVERS_BALANCES = 1 << 3
} VentureSeriesCovers;

/**
 * VentureSeriesTxnGroup:
 * @VENTURE_SERIES_TXN_GROUP_DAY: by UTC day
 * @VENTURE_SERIES_TXN_GROUP_WEEK: by ISO week, Monday 00:00 UTC
 * @VENTURE_SERIES_TXN_GROUP_MONTH: by calendar month, UTC
 * @VENTURE_SERIES_TXN_GROUP_ACCOUNT: by account
 * @VENTURE_SERIES_TXN_GROUP_VENUE: by venue
 * @VENTURE_SERIES_TXN_GROUP_INSTRUMENT: by instrument
 * @VENTURE_SERIES_TXN_GROUP_SOURCE: by the source's own label for where a
 *   row came from (TSM's "Auction", "Vendor", "Trade")
 *
 * How venture_series_store_txn_totals() buckets the ledger.
 */
typedef enum
{
	VENTURE_SERIES_TXN_GROUP_DAY = 0,
	VENTURE_SERIES_TXN_GROUP_WEEK,
	VENTURE_SERIES_TXN_GROUP_MONTH,
	VENTURE_SERIES_TXN_GROUP_ACCOUNT,
	VENTURE_SERIES_TXN_GROUP_VENUE,
	VENTURE_SERIES_TXN_GROUP_INSTRUMENT,
	VENTURE_SERIES_TXN_GROUP_SOURCE
} VentureSeriesTxnGroup;

/**
 * venture_series_txn_group_from_string:
 * @name: (nullable): "day", "week", "month", "account", "venue",
 *   "instrument" or "source"
 * @out: (out): the grouping
 *
 * Returns: %TRUE when @name is one
 */
gboolean
venture_series_txn_group_from_string(
	const gchar		*name,
	VentureSeriesTxnGroup	*out
);

/* --- What goes in ---------------------------------------------------------- */

/**
 * VentureSeriesAccount:
 * @key: the account's key at the source; required
 * @name: (nullable): what it is called
 * @kind: (nullable): character, shared, guild or other; %NULL keeps the
 *   stored kind (other for a new account)
 * @group_key: (nullable): the group it belongs to, a realm or region
 * @venue_key: (nullable): the venue it trades on
 * @attrs_json: (nullable): a JSON object of scalars
 * @last_seen: when the source last saw it in use (a login), or
 *   %VENTURE_SERIES_NONE
 *
 * One of the operator's accounts. %NULL members keep what is stored.
 */
typedef struct
{
	const gchar	*key;
	const gchar	*name;
	const gchar	*kind;
	const gchar	*group_key;
	const gchar	*venue_key;
	const gchar	*attrs_json;
	gint64		 last_seen;
} VentureSeriesAccount;

/**
 * VentureSeriesAccountSnapshot:
 * @account_key: whose state follows
 * @at: as of when it is complete
 * @covers: which kinds it restates, #VentureSeriesCovers
 *
 * Says the batch's rows of @covers for @account_key are all of them.
 */
typedef struct
{
	const gchar	*account_key;
	gint64		 at;
	guint		 covers;
} VentureSeriesAccountSnapshot;

/**
 * VentureSeriesBalance:
 * @account_key: whose money
 * @currency: in what
 * @amount: how much, minor units, at least 0
 * @at: as of when
 *
 * One point of an account's balance history.
 */
typedef struct
{
	const gchar	*account_key;
	const gchar	*currency;
	gint64		 amount;
	gint64		 at;
} VentureSeriesBalance;

/**
 * VentureSeriesHolding:
 * @account_key: who holds it
 * @place: where: bag, bank, reagent_bank, warbank, guild, mail, auction,
 *   void, equipped, currency or other
 * @instrument_key: what
 * @quantity: how many; 0 says the account no longer holds it there
 * @at: as of when
 *
 * One line of an account's inventory. A batch carries each (account,
 * place, instrument) at most once: the feed batch sums duplicates.
 */
typedef struct
{
	const gchar	*account_key;
	const gchar	*place;
	const gchar	*instrument_key;
	gint64		 quantity;
	gint64		 at;
} VentureSeriesHolding;

/**
 * VentureSeriesPosition:
 * @key: the source's id for the listing, stable for its life and unique
 *   in the store
 * @account_key: whose listing
 * @venue_key: where it is listed
 * @instrument_key: what
 * @quantity: units, at least 1
 * @unit_price: the buyout for one unit, minor units of the batch's
 *   currency
 * @bid: one unit's bid, or %VENTURE_SERIES_NONE
 * @expires_at: when it runs out, or %VENTURE_SERIES_NONE
 * @posted_at: when it went up, or %VENTURE_SERIES_NONE
 * @at: when the source saw it (its snapshot's time)
 *
 * One of the operator's own open listings.
 */
typedef struct
{
	const gchar	*key;
	const gchar	*account_key;
	const gchar	*venue_key;
	const gchar	*instrument_key;
	gint64		 quantity;
	gint64		 unit_price;
	gint64		 bid;
	gint64		 expires_at;
	gint64		 posted_at;
	gint64		 at;
} VentureSeriesPosition;

/**
 * VentureSeriesInbound:
 * @key: the source's id for it, unique in the store
 * @account_key: who it waits for
 * @sender: (nullable): who sent it
 * @subject: (nullable): its subject
 * @money: money attached, minor units, or %VENTURE_SERIES_NONE
 * @cod: cash on delivery owed to collect it, or %VENTURE_SERIES_NONE
 * @instrument_key: (nullable): an item attached
 * @quantity: how many of it, or %VENTURE_SERIES_NONE without one
 * @expires_at: when it is returned or lost, or %VENTURE_SERIES_NONE
 * @returned: whether it is something sent back
 * @at: when the source saw it
 *
 * Something waiting to be collected: a mail.
 */
typedef struct
{
	const gchar	*key;
	const gchar	*account_key;
	const gchar	*sender;
	const gchar	*subject;
	gint64		 money;
	gint64		 cod;
	const gchar	*instrument_key;
	gint64		 quantity;
	gint64		 expires_at;
	gboolean	 returned;
	gint64		 at;
} VentureSeriesInbound;

/**
 * VentureSeriesTxn:
 * @key: the source's stable id for the row; required
 * @account_key: whose
 * @venue_key: (nullable): where
 * @kind: sale, buy, income, expense, expired or cancelled
 * @instrument_key: (nullable): what changed hands
 * @quantity: how many, or %VENTURE_SERIES_NONE
 * @unit_price: one unit's price, or %VENTURE_SERIES_NONE
 * @amount: the total, never signed: the kind says which way it went; or
 *   %VENTURE_SERIES_NONE for an expiry or a cancellation
 * @counterparty: (nullable): the other side
 * @source: (nullable): the source's label for where it came from
 * @at: when
 *
 * One row of the source's ledger of the operator's trading.
 */
typedef struct
{
	const gchar	*key;
	const gchar	*account_key;
	const gchar	*venue_key;
	const gchar	*kind;
	const gchar	*instrument_key;
	gint64		 quantity;
	gint64		 unit_price;
	gint64		 amount;
	const gchar	*counterparty;
	const gchar	*source;
	gint64		 at;
} VentureSeriesTxn;

/**
 * VentureSeriesAccountBatch:
 * @currency: (nullable): what the positions', inbound rows' and ledger's
 *   money is in -- the data source's currency; %NULL when the batch
 *   carries none
 * @accounts: (array length=n_accounts): accounts to upsert
 * @n_accounts: how many
 * @snapshots: (array length=n_snapshots): at most one per account
 * @n_snapshots: how many
 * @balances: (array length=n_balances): balance points
 * @n_balances: how many
 * @holdings: (array length=n_holdings): holdings
 * @n_holdings: how many
 * @positions: (array length=n_positions): positions
 * @n_positions: how many
 * @inbound: (array length=n_inbound): inbound rows
 * @n_inbound: how many
 * @txns: (array length=n_txns): ledger rows
 * @n_txns: how many
 *
 * Everything one fetch or push said about the operator's accounts, as
 * plain arrays. Every row's account need not be among @accounts: one the
 * store has never seen is created bare.
 */
typedef struct
{
	const gchar				*currency;
	const VentureSeriesAccount		*accounts;
	guint					 n_accounts;
	const VentureSeriesAccountSnapshot	*snapshots;
	guint					 n_snapshots;
	const VentureSeriesBalance		*balances;
	guint					 n_balances;
	const VentureSeriesHolding		*holdings;
	guint					 n_holdings;
	const VentureSeriesPosition		*positions;
	guint					 n_positions;
	const VentureSeriesInbound		*inbound;
	guint					 n_inbound;
	const VentureSeriesTxn			*txns;
	guint					 n_txns;
} VentureSeriesAccountBatch;

/**
 * VentureSeriesAccountResult:
 * @accounts: accounts written
 * @accounts_new: accounts the store had not seen
 * @balances: balance points added or changed
 * @balances_unchanged: balance points that repeated their neighbour and
 *   were not stored
 * @holdings: holding rows written
 * @positions: position rows written
 * @inbound: inbound rows written
 * @removed: rows a snapshot removed because it did not restate them
 * @stale: account snapshots older than the state they would replace,
 *   whose rows were skipped
 * @txns_new: ledger rows added
 * @txns_updated: ledger rows changed in place (a merged quantity grew)
 * @txns_unchanged: ledger rows sent again exactly as stored
 * @instruments_new: instruments the rows named that the store created
 * @instruments_refused: new instruments refused past the size cap (their
 *   rows are kept; only the instrument row is missing)
 * @rows_written: rows inserted, updated or deleted
 *
 * What applying a batch did.
 */
typedef struct
{
	gint64	accounts;
	gint64	accounts_new;
	gint64	balances;
	gint64	balances_unchanged;
	gint64	holdings;
	gint64	positions;
	gint64	inbound;
	gint64	removed;
	gint64	stale;
	gint64	txns_new;
	gint64	txns_updated;
	gint64	txns_unchanged;
	gint64	instruments_new;
	gint64	instruments_refused;
	gint64	rows_written;
} VentureSeriesAccountResult;

/**
 * venture_series_store_apply_accounts:
 * @self: a writer handle
 * @batch: what to apply
 * @fetched_at: when it arrived (Unix seconds); an account's sync time and
 *   a new ledger row's first sighting
 * @out: (out) (optional): what was done
 * @error: (out) (optional): return location for a #GError
 *
 * Applies one batch in one transaction (a savepoint inside a batch
 * already open): accounts first, then each snapshot's replacement, then
 * the rows outside any snapshot, then the ledger. A malformed row fails
 * the whole batch and writes nothing; a stale snapshot is skipped and
 * counted. Venues and instruments the rows name are created bare when
 * new, instruments subject to the size cap.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_apply_accounts(
	VentureSeriesStore			 *self,
	const VentureSeriesAccountBatch		 *batch,
	gint64					  fetched_at,
	VentureSeriesAccountResult		 *out,
	GError					**error
);

/* --- What comes out --------------------------------------------------------- */

/**
 * VentureSeriesAmount:
 * @currency: the currency
 * @amount: minor units
 * @at: as of when, or 0 for a total
 *
 * Money in one currency.
 */
typedef struct
{
	gchar	currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64	amount;
	gint64	at;
} VentureSeriesAmount;

/**
 * VentureSeriesAccountRow:
 * @key: the account's key
 * @name: (nullable): its name
 * @kind: character, shared, guild or other
 * @group_key: its group, "" when none
 * @venue_key: (nullable): the venue it trades on
 * @attrs_json: (nullable): its attributes
 * @last_seen: when the source last saw it in use, or %VENTURE_SERIES_NONE
 * @first_seen: when the store first heard of it
 * @synced_at: when a batch last named it
 * @holdings_at: the snapshot that last replaced its holdings, or
 *   %VENTURE_SERIES_NONE
 * @positions_at: likewise for its positions
 * @inbound_at: likewise for its inbound
 * @balances_at: likewise for its balances
 * @holdings: holding rows
 * @positions: open positions
 * @positions_expired: positions whose expiry is at or before the time
 *   asked about -- gone from the market, still to be collected
 * @soonest_position_expiry: the earliest expiry among its positions, past
 *   or future, or %VENTURE_SERIES_NONE
 * @inbound: rows waiting to be collected
 * @inbound_expired: of those, the ones whose expiry is at or before the
 *   time asked about
 * @soonest_inbound_expiry: the earliest inbound expiry, or
 *   %VENTURE_SERIES_NONE
 * @balances: (element-type VentureSeriesAmount): its newest balance in
 *   each currency, oldest currency code first
 * @inbound_money: (element-type VentureSeriesAmount): money waiting in its
 *   inbound, per currency (@at 0)
 * @inbound_cod: (element-type VentureSeriesAmount): cash on delivery it
 *   would owe to collect, per currency
 *
 * An account with the figures a "needs attention" list is made of.
 */
typedef struct
{
	gchar	*key;
	gchar	*name;
	gchar	*kind;
	gchar	*group_key;
	gchar	*venue_key;
	gchar	*attrs_json;
	gint64	 last_seen;
	gint64	 first_seen;
	gint64	 synced_at;
	gint64	 holdings_at;
	gint64	 positions_at;
	gint64	 inbound_at;
	gint64	 balances_at;
	gint64	 holdings;
	gint64	 positions;
	gint64	 positions_expired;
	gint64	 soonest_position_expiry;
	gint64	 inbound;
	gint64	 inbound_expired;
	gint64	 soonest_inbound_expiry;
	GArray	*balances;
	GArray	*inbound_money;
	GArray	*inbound_cod;
} VentureSeriesAccountRow;

/**
 * venture_series_account_row_free:
 * @row: (transfer full) (nullable): an account row
 */
void
venture_series_account_row_free(VentureSeriesAccountRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesAccountRow, venture_series_account_row_free)

/**
 * venture_series_store_list_accounts:
 * @self: a store
 * @kind: (nullable): only this kind
 * @group_key: (nullable): only this group ("" for none)
 * @now: the moment "expired" is judged against
 * @error: (out) (optional): return location for a #GError
 *
 * Every account, by group, name and key, at most
 * %VENTURE_SERIES_MAX_ACCOUNTS.
 *
 * Returns: (transfer full) (element-type VentureSeriesAccountRow)
 *   (nullable): the accounts, or %NULL on error
 */
GPtrArray *
venture_series_store_list_accounts(
	VentureSeriesStore	 *self,
	const gchar		 *kind,
	const gchar		 *group_key,
	gint64			  now,
	GError			**error
);

/**
 * venture_series_store_get_account:
 * @self: a store
 * @key: the account's key
 * @now: the moment "expired" is judged against
 * @out: (out) (transfer full) (nullable): the account, or %NULL when the
 *   store has none by that key
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE unless the read failed
 */
gboolean
venture_series_store_get_account(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	gint64			  now,
	VentureSeriesAccountRow	**out,
	GError			**error
);

/**
 * VentureSeriesHoldingFilter:
 * @account_key: (nullable): one account's
 * @instrument_key: (nullable): one instrument's
 * @place: (nullable): one place's
 * @exclude_place: (nullable): every place but this one -- "currency", to
 *   count goods without a game's tokens
 * @search: (nullable): instruments whose name or key contains this,
 *   ignoring case
 * @offset: rows to skip
 * @count: rows to return; 0 for %VENTURE_SERIES_MAX_ACCOUNT_ROWS
 *
 * Which holdings a read wants. Start from venture_series_holding_filter_init().
 */
typedef struct
{
	const gchar	*account_key;
	const gchar	*instrument_key;
	const gchar	*place;
	const gchar	*exclude_place;
	const gchar	*search;
	guint		 offset;
	guint		 count;
} VentureSeriesHoldingFilter;

/**
 * venture_series_holding_filter_init:
 * @filter: (out caller-allocates): a filter to clear
 */
void
venture_series_holding_filter_init(VentureSeriesHoldingFilter *filter);

/**
 * VentureSeriesHoldingRow:
 * @account_key: who holds it
 * @account_name: (nullable): their name
 * @place: where
 * @instrument_key: what
 * @instrument_name: (nullable): its name, when the store knows it
 * @quantity: how many
 * @at: as of when
 *
 * One line of an account's inventory.
 */
typedef struct
{
	gchar	*account_key;
	gchar	*account_name;
	gchar	*place;
	gchar	*instrument_key;
	gchar	*instrument_name;
	gint64	 quantity;
	gint64	 at;
} VentureSeriesHoldingRow;

/**
 * venture_series_holding_row_free:
 * @row: (transfer full) (nullable): a holding row
 */
void
venture_series_holding_row_free(VentureSeriesHoldingRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesHoldingRow, venture_series_holding_row_free)

/**
 * venture_series_store_list_holdings:
 * @self: a store
 * @filter: (nullable): which; %NULL for all
 * @error: (out) (optional): return location for a #GError
 *
 * Holdings by account, place and instrument name: an account's detail
 * page, grouped by place.
 *
 * Returns: (transfer full) (element-type VentureSeriesHoldingRow)
 *   (nullable): the rows, or %NULL on error
 */
GPtrArray *
venture_series_store_list_holdings(
	VentureSeriesStore			 *self,
	const VentureSeriesHoldingFilter	 *filter,
	GError					**error
);

/**
 * VentureSeriesHoldingTotal:
 * @instrument_key: what
 * @instrument_name: (nullable): its name
 * @category: (nullable): its category path
 * @quantity: held across every account and place
 * @accounts: accounts holding any
 * @places: (element-type VentureSeriesHoldingRow): where, account by
 *   account and place by place
 *
 * One instrument across every account: the inventory page's row.
 */
typedef struct
{
	gchar		*instrument_key;
	gchar		*instrument_name;
	gchar		*category;
	gint64		 quantity;
	gint64		 accounts;
	GPtrArray	*places;
} VentureSeriesHoldingTotal;

/**
 * venture_series_holding_total_free:
 * @total: (transfer full) (nullable): a total
 */
void
venture_series_holding_total_free(VentureSeriesHoldingTotal *total);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesHoldingTotal, venture_series_holding_total_free)

/**
 * venture_series_store_holdings_by_instrument:
 * @self: a store
 * @filter: (nullable): which holdings count; @offset and @count page the
 *   instruments, not the rows
 * @error: (out) (optional): return location for a #GError
 *
 * What the operator holds of each instrument, summed over accounts and
 * places, by instrument name, each with its breakdown. Quantities are
 * integers; venture_series_store_value_instruments() is the reader that
 * values them.
 *
 * Returns: (transfer full) (element-type VentureSeriesHoldingTotal)
 *   (nullable): the totals, or %NULL on error
 */
GPtrArray *
venture_series_store_holdings_by_instrument(
	VentureSeriesStore			 *self,
	const VentureSeriesHoldingFilter	 *filter,
	GError					**error
);

/**
 * VentureSeriesPositionFilter:
 * @account_key: (nullable): one account's
 * @venue_key: (nullable): one venue's
 * @instrument_key: (nullable): one instrument's
 * @expires_before: only those expiring before this moment (the ones with
 *   no expiry are left out), or %VENTURE_SERIES_NONE for all
 * @offset: rows to skip
 * @count: rows to return; 0 for %VENTURE_SERIES_MAX_ACCOUNT_ROWS
 *
 * Which positions a read wants. Start from
 * venture_series_position_filter_init(): "no bound" is not zero.
 */
typedef struct
{
	const gchar	*account_key;
	const gchar	*venue_key;
	const gchar	*instrument_key;
	gint64		 expires_before;
	guint		 offset;
	guint		 count;
} VentureSeriesPositionFilter;

/**
 * venture_series_position_filter_init:
 * @filter: (out caller-allocates): a filter to clear
 */
void
venture_series_position_filter_init(VentureSeriesPositionFilter *filter);

/**
 * VentureSeriesPositionRow:
 * @key: the source's id for the listing
 * @account_key: whose
 * @venue_key: where
 * @instrument_key: what
 * @instrument_name: (nullable): its name
 * @quantity: units
 * @unit_price: one unit's buyout
 * @bid: one unit's bid, or %VENTURE_SERIES_NONE
 * @currency: the prices' currency
 * @expires_at: when it runs out, or %VENTURE_SERIES_NONE
 * @posted_at: when it went up, as the source said, or %VENTURE_SERIES_NONE
 * @first_seen: when the store first saw it -- its age, when the source
 *   does not say when it was posted
 * @last_seen: the snapshot that last restated it
 *
 * One open listing.
 */
typedef struct
{
	gchar	*key;
	gchar	*account_key;
	gchar	*venue_key;
	gchar	*instrument_key;
	gchar	*instrument_name;
	gint64	 quantity;
	gint64	 unit_price;
	gint64	 bid;
	gchar	 currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64	 expires_at;
	gint64	 posted_at;
	gint64	 first_seen;
	gint64	 last_seen;
} VentureSeriesPositionRow;

/**
 * venture_series_position_row_free:
 * @row: (transfer full) (nullable): a position row
 */
void
venture_series_position_row_free(VentureSeriesPositionRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesPositionRow, venture_series_position_row_free)

/**
 * venture_series_store_list_positions:
 * @self: a store
 * @filter: (nullable): which; %NULL for all
 * @error: (out) (optional): return location for a #GError
 *
 * Open positions, soonest expiry first (none last), then by key.
 *
 * Returns: (transfer full) (element-type VentureSeriesPositionRow)
 *   (nullable): the rows, or %NULL on error
 */
GPtrArray *
venture_series_store_list_positions(
	VentureSeriesStore			 *self,
	const VentureSeriesPositionFilter	 *filter,
	GError					**error
);

/**
 * VentureSeriesInboundFilter:
 * @account_key: (nullable): one account's
 * @expires_before: only those expiring before this moment, or
 *   %VENTURE_SERIES_NONE for all
 * @offset: rows to skip
 * @count: rows to return; 0 for %VENTURE_SERIES_MAX_ACCOUNT_ROWS
 *
 * Which inbound rows a read wants. Start from
 * venture_series_inbound_filter_init().
 */
typedef struct
{
	const gchar	*account_key;
	gint64		 expires_before;
	guint		 offset;
	guint		 count;
} VentureSeriesInboundFilter;

/**
 * venture_series_inbound_filter_init:
 * @filter: (out caller-allocates): a filter to clear
 */
void
venture_series_inbound_filter_init(VentureSeriesInboundFilter *filter);

/**
 * VentureSeriesInboundRow:
 * @key: the source's id for it
 * @account_key: who it waits for
 * @sender: (nullable): who sent it
 * @subject: (nullable): its subject
 * @money: money attached, or %VENTURE_SERIES_NONE
 * @cod: cash on delivery owed, or %VENTURE_SERIES_NONE
 * @currency: the money's currency, "" when it carries none
 * @instrument_key: (nullable): an item attached
 * @instrument_name: (nullable): its name
 * @quantity: how many, or %VENTURE_SERIES_NONE
 * @expires_at: when it is lost, or %VENTURE_SERIES_NONE
 * @returned: whether it was sent back
 * @first_seen: when the store first saw it
 * @last_seen: the snapshot that last restated it
 *
 * Something waiting to be collected.
 */
typedef struct
{
	gchar		*key;
	gchar		*account_key;
	gchar		*sender;
	gchar		*subject;
	gint64		 money;
	gint64		 cod;
	gchar		 currency[VENTURE_MONEY_CURRENCY_LEN];
	gchar		*instrument_key;
	gchar		*instrument_name;
	gint64		 quantity;
	gint64		 expires_at;
	gboolean	 returned;
	gint64		 first_seen;
	gint64		 last_seen;
} VentureSeriesInboundRow;

/**
 * venture_series_inbound_row_free:
 * @row: (transfer full) (nullable): an inbound row
 */
void
venture_series_inbound_row_free(VentureSeriesInboundRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesInboundRow, venture_series_inbound_row_free)

/**
 * venture_series_store_list_inbound:
 * @self: a store
 * @filter: (nullable): which; %NULL for all
 * @error: (out) (optional): return location for a #GError
 *
 * Inbound rows, soonest expiry first (none last), then by key.
 *
 * Returns: (transfer full) (element-type VentureSeriesInboundRow)
 *   (nullable): the rows, or %NULL on error
 */
GPtrArray *
venture_series_store_list_inbound(
	VentureSeriesStore			 *self,
	const VentureSeriesInboundFilter	 *filter,
	GError					**error
);

/**
 * venture_series_store_balance_history:
 * @self: a store
 * @account_key: whose
 * @currency: (nullable): one currency's; %NULL for every currency
 * @since: from this moment, inclusive, or %VENTURE_SERIES_NONE
 * @until: to this moment, exclusive, or %VENTURE_SERIES_NONE
 * @error: (out) (optional): return location for a #GError
 *
 * An account's balance history, oldest first: one point per change, since
 * a repeat of the previous value is never stored. The newest point before
 * @since is the balance at @since, and a chart should start from it.
 *
 * Returns: (transfer full) (element-type VentureSeriesAmount) (nullable):
 *   the points, or %NULL on error; empty for an unknown account
 */
GArray *
venture_series_store_balance_history(
	VentureSeriesStore	 *self,
	const gchar		 *account_key,
	const gchar		 *currency,
	gint64			  since,
	gint64			  until,
	GError			**error
);

/**
 * VentureSeriesTxnFilter:
 * @account_key: (nullable): one account's
 * @kind: (nullable): one kind's: sale, buy, income, expense, expired,
 *   cancelled
 * @instrument_key: (nullable): one instrument's
 * @venue_key: (nullable): one venue's
 * @source: (nullable): one source label's
 * @since: from this moment, inclusive, or %VENTURE_SERIES_NONE
 * @until: to this moment, exclusive, or %VENTURE_SERIES_NONE
 * @offset: rows to skip (listing only)
 * @count: rows to return (listing only); 0 for
 *   %VENTURE_SERIES_DEFAULT_PAGE, at most %VENTURE_SERIES_MAX_ACCOUNT_ROWS
 * @descending: newest first (listing only)
 *
 * Which ledger rows a read wants. Start from
 * venture_series_txn_filter_init(): "no bound" is not zero.
 */
typedef struct
{
	const gchar	*account_key;
	const gchar	*kind;
	const gchar	*instrument_key;
	const gchar	*venue_key;
	const gchar	*source;
	gint64		 since;
	gint64		 until;
	guint		 offset;
	guint		 count;
	gboolean	 descending;
} VentureSeriesTxnFilter;

/**
 * venture_series_txn_filter_init:
 * @filter: (out caller-allocates): a filter to clear
 */
void
venture_series_txn_filter_init(VentureSeriesTxnFilter *filter);

/**
 * VentureSeriesTxnRow:
 * @key: the source's id for the row
 * @account_key: whose
 * @venue_key: (nullable): where
 * @kind: what happened
 * @instrument_key: (nullable): what
 * @instrument_name: (nullable): its name
 * @quantity: how many, or %VENTURE_SERIES_NONE
 * @unit_price: one unit's price, or %VENTURE_SERIES_NONE
 * @amount: the total, or %VENTURE_SERIES_NONE
 * @currency: the money's currency, "" when it carries none
 * @counterparty: (nullable): the other side
 * @source: (nullable): the source's label
 * @at: when it happened
 * @first_seen: when the store first received it
 * @updated_at: when the store last changed it
 *
 * One ledger row.
 */
typedef struct
{
	gchar	*key;
	gchar	*account_key;
	gchar	*venue_key;
	gchar	*kind;
	gchar	*instrument_key;
	gchar	*instrument_name;
	gint64	 quantity;
	gint64	 unit_price;
	gint64	 amount;
	gchar	 currency[VENTURE_MONEY_CURRENCY_LEN];
	gchar	*counterparty;
	gchar	*source;
	gint64	 at;
	gint64	 first_seen;
	gint64	 updated_at;
} VentureSeriesTxnRow;

/**
 * venture_series_txn_row_free:
 * @row: (transfer full) (nullable): a ledger row
 */
void
venture_series_txn_row_free(VentureSeriesTxnRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesTxnRow, venture_series_txn_row_free)

/**
 * venture_series_store_list_txns:
 * @self: a store
 * @filter: (nullable): which; %NULL for the newest page
 * @error: (out) (optional): return location for a #GError
 *
 * Ledger rows by time (then key), a page at a time.
 *
 * Returns: (transfer full) (element-type VentureSeriesTxnRow) (nullable):
 *   the rows, or %NULL on error
 */
GPtrArray *
venture_series_store_list_txns(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	GError				**error
);

/**
 * venture_series_store_count_txns:
 * @self: a store
 * @filter: (nullable): which; paging is ignored
 * @out_count: (out): how many rows match
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_count_txns(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	gint64				 *out_count,
	GError				**error
);

/**
 * VentureSeriesTxnTotal:
 * @key: (nullable): the group: an account, venue or instrument key or a
 *   source label ("" for rows that name none); %NULL for a period
 * @label: (nullable): the account's, venue's or instrument's name, when
 *   the store knows it
 * @period_start: the period's first second (UTC) for a day, week or month,
 *   else %VENTURE_SERIES_NONE
 * @currency: the money's currency, "" for rows that carry none
 * @txns: rows
 * @sales: sale rows
 * @sold_units: units sold
 * @sales_amount: what they sold for, after the source's fees
 * @buys: buy rows
 * @bought_units: units bought
 * @buys_amount: what they cost
 * @income: other money in
 * @expense: other money out
 * @expired_units: units whose listing expired
 * @cancelled_units: units whose listing was cancelled
 * @net: @sales_amount + @income - @buys_amount - @expense
 *
 * One bucket of the ledger, in one currency. Money is summed as integers
 * by SQLite, which refuses an overflow rather than wrapping or rounding.
 */
typedef struct
{
	gchar	*key;
	gchar	*label;
	gint64	 period_start;
	gchar	 currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64	 txns;
	gint64	 sales;
	gint64	 sold_units;
	gint64	 sales_amount;
	gint64	 buys;
	gint64	 bought_units;
	gint64	 buys_amount;
	gint64	 income;
	gint64	 expense;
	gint64	 expired_units;
	gint64	 cancelled_units;
	gint64	 net;
} VentureSeriesTxnTotal;

/**
 * venture_series_txn_total_free:
 * @total: (transfer full) (nullable): a total
 */
void
venture_series_txn_total_free(VentureSeriesTxnTotal *total);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesTxnTotal, venture_series_txn_total_free)

/**
 * venture_series_store_txn_totals:
 * @self: a store
 * @filter: (nullable): which rows count; paging is ignored
 * @group: how to bucket them
 * @error: (out) (optional): return location for a #GError
 *
 * The ledger summed for a profit and loss: a bucket per group and
 * currency, in order of the group (periods oldest first). Periods are
 * UTC, like every period boundary in VENTURE. At most
 * %VENTURE_SERIES_MAX_ACCOUNT_ROWS buckets; past that the read is refused
 * rather than truncated, and the caller narrows.
 *
 * Returns: (transfer full) (element-type VentureSeriesTxnTotal)
 *   (nullable): the buckets, or %NULL on error
 */
GPtrArray *
venture_series_store_txn_totals(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	VentureSeriesTxnGroup		  group,
	GError				**error
);

/* --- What the holdings are worth -------------------------------------------- */

/**
 * VentureSeriesValueBasis:
 * @VENTURE_SERIES_VALUE_CONSERVATIVE: the lower of the region's sale
 *   average and the market value where the account trades (the region's
 *   average market value for an account with no venue, or an instrument
 *   its venue does not list) -- what an item has been selling for, never
 *   more than it is listed at. Either alone when the other is unknown.
 * @VENTURE_SERIES_VALUE_MARKET: the market value at the account's venue,
 *   else the region's average market value
 * @VENTURE_SERIES_VALUE_MIN: the lowest price on offer at the account's
 *   venue
 * @VENTURE_SERIES_VALUE_HISTORICAL: the source's own historical price at
 *   the account's venue
 * @VENTURE_SERIES_VALUE_REGION_MARKET: the store's region figure: the mean
 *   market value across the venue's group
 * @VENTURE_SERIES_VALUE_REGION_SALE_AVG: what units sold for across the
 *   region, from the region venue's newest day with a sale average in the
 *   last sixty
 *
 * Which price one unit of a holding is valued at. Every figure is read in
 * the currency asked for only: a price in another is no price.
 */
typedef enum
{
	VENTURE_SERIES_VALUE_CONSERVATIVE = 0,
	VENTURE_SERIES_VALUE_MARKET,
	VENTURE_SERIES_VALUE_MIN,
	VENTURE_SERIES_VALUE_HISTORICAL,
	VENTURE_SERIES_VALUE_REGION_MARKET,
	VENTURE_SERIES_VALUE_REGION_SALE_AVG
} VentureSeriesValueBasis;

/**
 * venture_series_value_basis_from_string:
 * @name: (nullable): "conservative", "market", "min", "historical",
 *   "region_market" or "region_sale_avg"
 * @out: (out): the basis
 *
 * Returns: %TRUE when @name is one
 */
gboolean
venture_series_value_basis_from_string(
	const gchar		*name,
	VentureSeriesValueBasis	*out
);

/**
 * venture_series_value_basis_to_string:
 * @basis: a basis
 *
 * Returns: (transfer none): its name
 */
const gchar *
venture_series_value_basis_to_string(VentureSeriesValueBasis basis);

/**
 * VentureSeriesValueSort:
 * @VENTURE_SERIES_VALUE_SORT_VALUE: by total value
 * @VENTURE_SERIES_VALUE_SORT_QUANTITY: by units held
 * @VENTURE_SERIES_VALUE_SORT_NAME: by the instrument's name
 * @VENTURE_SERIES_VALUE_SORT_UNIT_VALUE: by what one unit is worth
 * @VENTURE_SERIES_VALUE_SORT_ACCOUNTS: by how many accounts hold some
 * @VENTURE_SERIES_VALUE_SORT_DAYS_OF_SUPPLY: by units held over units
 *   sold a day
 *
 * How venture_series_store_value_instruments() orders its answer. Every
 * order is computed by SQLite in the one statement that sums, so a page
 * of the most valuable is a LIMIT, not a sort in C. Unknown values sort
 * last whichever way the sort runs.
 */
typedef enum
{
	VENTURE_SERIES_VALUE_SORT_VALUE = 0,
	VENTURE_SERIES_VALUE_SORT_QUANTITY,
	VENTURE_SERIES_VALUE_SORT_NAME,
	VENTURE_SERIES_VALUE_SORT_UNIT_VALUE,
	VENTURE_SERIES_VALUE_SORT_ACCOUNTS,
	VENTURE_SERIES_VALUE_SORT_DAYS_OF_SUPPLY
} VentureSeriesValueSort;

/**
 * venture_series_value_sort_from_string:
 * @name: (nullable): "value", "quantity", "name", "unit_value",
 *   "accounts" or "days_of_supply"
 * @out: (out): the sort
 *
 * Returns: %TRUE when @name is one
 */
gboolean
venture_series_value_sort_from_string(
	const gchar		*name,
	VentureSeriesValueSort	*out
);

/**
 * venture_series_value_sort_to_string:
 * @sort: a sort
 *
 * Returns: (transfer none): its name
 */
const gchar *
venture_series_value_sort_to_string(VentureSeriesValueSort sort);

/**
 * VentureSeriesValueFilter:
 * @currency: what to value in, required: the source's currency
 * @basis: which price a unit is worth
 * @region_venue: (nullable): the venue whose figures are the region's
 *   sale average; %NULL for "region-" and the account's venue's group in
 *   lower case, the name the region-wide statistics travel under
 * @default_group: (nullable): the group whose region figures value an
 *   account that trades on no venue (a shared bank)
 * @now: the moment the sale average's sixty days count back from
 * @account_key: (nullable): one account's holdings
 * @place: (nullable): one place's
 * @exclude_place: (nullable): every place but this one
 * @search: (nullable): instruments whose name or key contains this
 * @category_prefix: (nullable): this category path and the ones beneath
 * @min_value: only instruments whose total is at least this, or
 *   %VENTURE_SERIES_NONE
 * @unsold_since: only instruments with no sale in the ledger at or after
 *   this moment ("dead stock"), or %VENTURE_SERIES_NONE
 * @sort: the order
 * @descending: largest first
 * @offset: instruments to skip
 * @count: instruments to return, 0 for %VENTURE_SERIES_DEFAULT_PAGE, at
 *   most %VENTURE_SERIES_MAX_ACCOUNT_ROWS
 *
 * What venture_series_store_value_instruments() and _value_lines() read.
 * Start from venture_series_value_filter_init(): two "no bound" values are
 * not zero.
 */
typedef struct
{
	const gchar		*currency;
	VentureSeriesValueBasis	 basis;
	const gchar		*region_venue;
	const gchar		*default_group;
	gint64			 now;
	const gchar		*account_key;
	const gchar		*place;
	const gchar		*exclude_place;
	const gchar		*search;
	const gchar		*category_prefix;
	gint64			 min_value;
	gint64			 unsold_since;
	VentureSeriesValueSort	 sort;
	gboolean		 descending;
	guint			 offset;
	guint			 count;
} VentureSeriesValueFilter;

/**
 * venture_series_value_filter_init:
 * @filter: (out caller-allocates): a filter to clear
 *
 * Every holding, conservatively valued, most valuable first.
 */
void
venture_series_value_filter_init(VentureSeriesValueFilter *filter);

/**
 * VentureSeriesValuedLine:
 * @account_key: who holds it
 * @account_name: (nullable): their name
 * @account_venue: (nullable): the venue they trade on
 * @place: where
 * @instrument_key: what
 * @instrument_name: (nullable): its name
 * @category: (nullable): its category path
 * @quantity: how many
 * @unit_value: one unit's value, or %VENTURE_SERIES_NONE when unpriced
 * @value: @quantity times @unit_value, or %VENTURE_SERIES_NONE
 * @at: as of when
 *
 * One holding, valued.
 */
typedef struct
{
	gchar	*account_key;
	gchar	*account_name;
	gchar	*account_venue;
	gchar	*place;
	gchar	*instrument_key;
	gchar	*instrument_name;
	gchar	*category;
	gint64	 quantity;
	gint64	 unit_value;
	gint64	 value;
	gint64	 at;
} VentureSeriesValuedLine;

/**
 * venture_series_valued_line_free:
 * @line: (transfer full) (nullable): a valued line
 */
void
venture_series_valued_line_free(VentureSeriesValuedLine *line);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesValuedLine, venture_series_valued_line_free)

/**
 * VentureSeriesValuedInstrument:
 * @instrument_key: what
 * @instrument_name: (nullable): its name
 * @category: (nullable): its category path
 * @quantity: units held across every account and place
 * @accounts: accounts holding some
 * @lines: holding lines
 * @priced_lines: of those, the ones with a price
 * @priced_quantity: units on priced lines
 * @value: the priced lines' total, or %VENTURE_SERIES_NONE when none is
 *   priced
 * @unit_value: @value over @priced_quantity, rounded half to even, or
 *   %VENTURE_SERIES_NONE
 * @sold_per_day: units sold a day across the region (else the most of
 *   any venue the holders trade on), or NAN when unknown
 * @last_sale: the ledger's newest sale of it, or %VENTURE_SERIES_NONE
 *
 * One instrument across the holdings asked about, valued.
 */
typedef struct
{
	gchar	*instrument_key;
	gchar	*instrument_name;
	gchar	*category;
	gint64	 quantity;
	gint64	 accounts;
	gint64	 lines;
	gint64	 priced_lines;
	gint64	 priced_quantity;
	gint64	 value;
	gint64	 unit_value;
	gdouble	 sold_per_day;
	gint64	 last_sale;
} VentureSeriesValuedInstrument;

/**
 * venture_series_valued_instrument_free:
 * @instrument: (transfer full) (nullable): a valued instrument
 */
void
venture_series_valued_instrument_free(VentureSeriesValuedInstrument *instrument);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesValuedInstrument, venture_series_valued_instrument_free)

/**
 * VentureSeriesValueTotals:
 * @instruments: instruments matching the filter (every page)
 * @lines: holding lines
 * @priced_lines: lines with a price
 * @quantity: units
 * @value: the priced lines' total, 0 when none is priced
 *
 * The whole answer's totals, whichever page was asked for.
 */
typedef struct
{
	gint64	instruments;
	gint64	lines;
	gint64	priced_lines;
	gint64	quantity;
	gint64	value;
} VentureSeriesValueTotals;

/**
 * venture_series_store_value_instruments:
 * @self: a store
 * @filter: which holdings, valued how, which page
 * @out_totals: (out caller-allocates) (optional): the totals of every
 *   instrument the filter matches
 * @error: (out) (optional): return location for a #GError
 *
 * Values the operator's holdings and sums them per instrument, in one
 * statement: the per-unit figures are joined from the current and region
 * tables, the products and the sums are integers in SQLite (a sum that
 * overflows is refused), and the sort and the page are applied there too.
 * A line whose product would not fit in 64 bits is left unpriced.
 *
 * Returns: (transfer full) (element-type VentureSeriesValuedInstrument)
 *   (nullable): the page, or %NULL on error (INVALID_ARGUMENT without a
 *   currency)
 */
GPtrArray *
venture_series_store_value_instruments(
	VentureSeriesStore			 *self,
	const VentureSeriesValueFilter		 *filter,
	VentureSeriesValueTotals		 *out_totals,
	GError					**error
);

/**
 * venture_series_store_value_lines:
 * @self: a store
 * @filter: which holdings, valued how; the sort, the minimum and the page
 *   are not read
 * @instrument_keys: (array zero-terminated=1) (nullable): only these
 *   instruments (a page's breakdown); %NULL for every one
 * @error: (out) (optional): return location for a #GError
 *
 * The holdings behind venture_series_store_value_instruments(), line by
 * line and valued the same way, by account, place and instrument name, at
 * most %VENTURE_SERIES_MAX_ACCOUNT_ROWS.
 *
 * Returns: (transfer full) (element-type VentureSeriesValuedLine)
 *   (nullable): the lines, or %NULL on error
 */
GPtrArray *
venture_series_store_value_lines(
	VentureSeriesStore			 *self,
	const VentureSeriesValueFilter		 *filter,
	const gchar *const			 *instrument_keys,
	GError					**error
);

/* --- Balances by day ------------------------------------------------------------ */

/**
 * VentureSeriesBalanceDay:
 * @account_key: whose
 * @day_start: midnight UTC of the day
 * @amount: the balance at the end of that day
 *
 * One account's closing balance on one day.
 */
typedef struct
{
	gchar	*account_key;
	gint64	 day_start;
	gint64	 amount;
} VentureSeriesBalanceDay;

/**
 * venture_series_balance_day_free:
 * @day: (transfer full) (nullable): a balance day
 */
void
venture_series_balance_day_free(VentureSeriesBalanceDay *day);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesBalanceDay, venture_series_balance_day_free)

/**
 * venture_series_store_balance_days:
 * @self: a store
 * @currency: which currency's balances
 * @since: the first day wanted, any moment in it
 * @until: the end, exclusive
 * @error: (out) (optional): return location for a #GError
 *
 * Every account's closing balance for each day in [@since, @until) on
 * which it changed, plus, dated the day before @since's, the balance each
 * account opened that window with: everything a sparkline per account
 * needs, read in two grouped statements however many points a busy
 * account stored. A day with no row kept the previous day's balance.
 *
 * Returns: (transfer full) (element-type VentureSeriesBalanceDay)
 *   (nullable): the days, by account key then day; %NULL on error
 */
GPtrArray *
venture_series_store_balance_days(
	VentureSeriesStore	 *self,
	const gchar		 *currency,
	gint64			  since,
	gint64			  until,
	GError			**error
);

/* --- Flips ------------------------------------------------------------------------ */

/**
 * VentureSeriesFlip:
 * @instrument_key: what was bought and sold
 * @instrument_name: (nullable): its name
 * @currency: the money's currency
 * @bought_units: units bought in the window
 * @bought_amount: what they cost
 * @matched_units: of the units sold, those matched to an earlier buy
 * @cost: what the matched units cost, first bought first sold
 * @proceeds: what the matched units sold for, after the source's fees
 * @profit: @proceeds - @cost
 * @unmatched_sold_units: units sold with no earlier buy in the window to
 *   match (made, farmed, or bought before it)
 * @open_units: units bought and not yet sold in the window
 * @open_cost: what those cost
 * @held: units the accounts hold of it now, every place but currency
 * @first_buy: the window's first buy
 * @last_sale: the window's last matched sale, or %VENTURE_SERIES_NONE
 *
 * One instrument's buys matched to its later sales: what a flip made.
 */
typedef struct
{
	gchar	*instrument_key;
	gchar	*instrument_name;
	gchar	 currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64	 bought_units;
	gint64	 bought_amount;
	gint64	 matched_units;
	gint64	 cost;
	gint64	 proceeds;
	gint64	 profit;
	gint64	 unmatched_sold_units;
	gint64	 open_units;
	gint64	 open_cost;
	gint64	 held;
	gint64	 first_buy;
	gint64	 last_sale;
} VentureSeriesFlip;

/**
 * venture_series_flip_free:
 * @flip: (transfer full) (nullable): a flip
 */
void
venture_series_flip_free(VentureSeriesFlip *flip);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesFlip, venture_series_flip_free)

/**
 * venture_series_store_flips:
 * @self: a store
 * @filter: (nullable): which ledger rows count -- an account, a venue, an
 *   instrument, the window; the kind and paging are not read
 * @error: (out) (optional): return location for a #GError
 *
 * Matches every instrument's buys to its later sales, first in first out,
 * across the accounts the filter keeps: a sale takes units from the
 * oldest buy before it, its proceeds and each buy's cost split between
 * the units in integers that add back up exactly. Instruments with no buy
 * in the window are left out. Rows are read in one ordered statement; a
 * window of more than %VENTURE_SERIES_MAX_ACCOUNT_ROWS buys and sales is
 * refused (INVALID_ARGUMENT) rather than matched in part.
 *
 * Returns: (transfer full) (element-type VentureSeriesFlip) (nullable): one
 *   per instrument and currency, by profit, largest first; %NULL on error
 */
GPtrArray *
venture_series_store_flips(
	VentureSeriesStore		 *self,
	const VentureSeriesTxnFilter	 *filter,
	GError				**error
);

G_END_DECLS

#endif /* VENTURE_SERIES_ACCOUNTS_H */
