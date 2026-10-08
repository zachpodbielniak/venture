/*
 * venture-feed-batch.h - What one fetch of a market data source produced
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A provider hands the feeds worker a batch: venues, instruments,
 * snapshots of listings or figures, quotes, entries, record upserts, a
 * cursor and whether nothing changed. It is plain data -- strings copied
 * in, integers in minor units -- so it can be built on whatever thread a
 * provider runs on and handed to the worker, which alone writes it into
 * the source's series store. Nothing in a batch touches the database, the
 * configuration or the store.
 *
 * Prices are already minor units in the batch's or venue's currency: a
 * provider converts with venture_feed_decimal_to_minor() (exact, never
 * through a double). Odds are decimal odds times
 * VENTURE_SERIES_ODDS_SCALE, with no currency.
 */

#ifndef VENTURE_FEED_BATCH_H
#define VENTURE_FEED_BATCH_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_FEED_BATCH_MAX_RECORDS:
 *
 * The most record upserts one batch may carry. The run applies at most
 * feeds.max_records_per_run of them across all its units; this bounds what
 * a provider can make the worker hold in memory before that.
 */
#define VENTURE_FEED_BATCH_MAX_RECORDS (10000)

/**
 * VENTURE_FEED_BATCH_MAX_ACCOUNTS:
 *
 * The most operator accounts one batch may describe or name.
 */
#define VENTURE_FEED_BATCH_MAX_ACCOUNTS (10000)

/**
 * VENTURE_FEED_BATCH_MAX_ACCOUNT_ROWS:
 *
 * The most balance, holding, position and inbound rows one batch may
 * carry, together. A first sync of a large operator is a few hundred
 * thousand; past this a batch is somebody's mistake, held in memory.
 */
#define VENTURE_FEED_BATCH_MAX_ACCOUNT_ROWS (1000000)

/**
 * VENTURE_FEED_BATCH_MAX_TXNS:
 *
 * The most external ledger rows one batch may carry. A first sync of a
 * TSM ledger is tens of thousands of rows; the bound is for memory, not
 * for a real ledger.
 */
#define VENTURE_FEED_BATCH_MAX_TXNS (1000000)

typedef struct _VentureFeedBatch VentureFeedBatch;

#define VENTURE_TYPE_FEED_BATCH (venture_feed_batch_get_type())

GType venture_feed_batch_get_type(void) G_GNUC_CONST;

/**
 * venture_feed_batch_new:
 *
 * Returns: (transfer full): an empty batch
 */
VentureFeedBatch *
venture_feed_batch_new(void);

/**
 * venture_feed_batch_ref:
 * @self: a batch
 *
 * The count is atomic, so a batch may be handed between threads.
 *
 * Returns: (transfer full): @self
 */
VentureFeedBatch *
venture_feed_batch_ref(VentureFeedBatch *self);

/**
 * venture_feed_batch_unref:
 * @self: (transfer full): a batch
 */
void
venture_feed_batch_unref(VentureFeedBatch *self);

/**
 * venture_feed_batch_add_venue:
 * @self: a batch
 * @key: the venue's key at the source
 * @name: (nullable): what it is called
 * @kind: (nullable): marketplace, auction_house, bookmaker...
 * @group_key: (nullable): the region it belongs to, for region figures
 * @currency: (nullable): what its prices are in
 * @attrs_json: (nullable): a JSON object of anything else
 * @error: (out) (optional): return location for a #GError
 *
 * A venue: somewhere that quotes prices. The source's venue namespace is
 * applied by the worker, never by the provider.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_venue(
	VentureFeedBatch	 *self,
	const gchar		 *key,
	const gchar		 *name,
	const gchar		 *kind,
	const gchar		 *group_key,
	const gchar		 *currency,
	const gchar		 *attrs_json,
	GError			**error
);

/**
 * venture_feed_batch_add_instrument:
 * @self: a batch
 * @key: the instrument's key at the source
 * @name: (nullable): what it is called
 * @kind: (nullable): item, sku, outcome...
 * @category: (nullable): a "/"-separated path
 * @parent_key: (nullable): the instrument it belongs to (an event's outcome)
 * @attrs_json: (nullable): a JSON object of anything else
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_instrument(
	VentureFeedBatch	 *self,
	const gchar		 *key,
	const gchar		 *name,
	const gchar		 *kind,
	const gchar		 *category,
	const gchar		 *parent_key,
	const gchar		 *attrs_json,
	GError			**error
);

/**
 * venture_feed_batch_begin_snapshot:
 * @self: a batch
 * @venue_key: the venue it is of
 * @currency: (nullable): its prices' currency; %NULL for the venue's, then
 *   the source's
 * @taken_at: when the source took it (Unix seconds); its Last-Modified
 * @complete: whether it lists everything at the venue
 * @error: (out) (optional): return location for a #GError
 *
 * Starts a snapshot of one venue. Listings and figures for that venue
 * added afterwards belong to it, until another snapshot of the same venue
 * is begun.
 *
 * Returns: %TRUE when it was begun
 */
gboolean
venture_feed_batch_begin_snapshot(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *currency,
	gint64			  taken_at,
	gboolean		  complete,
	GError			**error
);

/**
 * venture_feed_batch_add_listing:
 * @self: a batch
 * @venue_key: the venue; a snapshot of it must have been begun
 * @instrument_key: what is listed
 * @listing_id: the source's id for the listing, or 0;
 *   venture_series_listing_id_from_string() for a textual one
 * @unit_price: one unit's price, minor units
 * @quantity: units, at least 1
 * @buy: %TRUE for a buy order, %FALSE for an offer to sell
 * @expires_in_min: the least time the listing has left, in *seconds* --
 *   "min" is minimum, not minutes -- or -1 when unknown; a listing that
 *   vanishes sooner than this could not have expired, so it counts as sold
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was added; a malformed listing is refused with
 *   %VENTURE_ERROR_INVALID_ARGUMENT and the caller may count it and go on
 */
gboolean
venture_feed_batch_add_listing(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	guint64			  listing_id,
	gint64			  unit_price,
	gint64			  quantity,
	gboolean		  buy,
	gint64			  expires_in_min,
	GError			**error
);

/**
 * venture_feed_batch_add_stats:
 * @self: a batch
 * @venue_key: the venue; a snapshot of it must have been begun
 * @instrument_key: what the figures are for
 * @min_price: the lowest price, or %VENTURE_SERIES_NONE
 * @market_value: the source's market value, or %VENTURE_SERIES_NONE
 * @mean: the mean, or %VENTURE_SERIES_NONE
 * @median: the median, or %VENTURE_SERIES_NONE
 * @sale_avg: the average sale price, or %VENTURE_SERIES_NONE
 * @quantity: units available, or %VENTURE_SERIES_NONE
 * @listings: listings, or %VENTURE_SERIES_NONE
 * @sold: units sold, or %VENTURE_SERIES_NONE
 * @error: (out) (optional): return location for a #GError
 *
 * Precomputed figures, for a source with no individual listings. At least
 * one must be given.
 *
 * Returns: %TRUE when they were added
 */
gboolean
venture_feed_batch_add_stats(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  min_price,
	gint64			  market_value,
	gint64			  mean,
	gint64			  median,
	gint64			  sale_avg,
	gint64			  quantity,
	gint64			  listings,
	gint64			  sold,
	GError			**error
);

/**
 * venture_feed_batch_add_stats_full:
 * @self: a batch
 * @venue_key: the venue; a snapshot of it must have been begun
 * @stats: the figures, started from venture_series_stats_init(); its
 *   instrument_key names the instrument
 * @error: (out) (optional): return location for a #GError
 *
 * venture_feed_batch_add_stats() with the source's own sales figures as
 * well: a historical price, a sale rate (0 to 1) and units sold a day,
 * which the store keeps beside its own estimates.
 *
 * Returns: %TRUE when they were added
 */
gboolean
venture_feed_batch_add_stats_full(
	VentureFeedBatch		 *self,
	const gchar			 *venue_key,
	const VentureSeriesStats	 *stats,
	GError				**error
);

/**
 * venture_feed_batch_add_account:
 * @self: a batch
 * @account: one of the operator's accounts; its strings are copied
 * @error: (out) (optional): return location for a #GError
 *
 * An account the operator plays or trades with: a character, a shared
 * bank, a guild bank, a seller account. Rows may name an account the
 * batch does not describe; the store creates it bare.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_account(
	VentureFeedBatch		 *self,
	const VentureSeriesAccount	 *account,
	GError				**error
);

/**
 * venture_feed_batch_add_login:
 * @self: a batch
 * @login: a login the operator reaches accounts through; its strings are
 *   copied
 * @error: (out) (optional): return location for a #GError
 *
 * A credential the operator signs in with: a game account or licence, a
 * platform account, a seller login. An account may name a login the
 * batch does not describe; the store creates it bare. At most
 * %VENTURE_FEED_BATCH_MAX_ACCOUNTS a batch.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_login(
	VentureFeedBatch		 *self,
	const VentureSeriesLogin	 *login,
	GError				**error
);

/**
 * venture_feed_batch_add_account_snapshot:
 * @self: a batch
 * @account_key: whose state the batch restates
 * @at: as of when it is complete (Unix seconds)
 * @covers: which kinds, #VentureSeriesCovers; at least one
 * @error: (out) (optional): return location for a #GError
 *
 * Says that the batch's rows of @covers for @account_key are all of
 * them: the store removes every one it held that the batch does not
 * restate. It must come before the account's rows of those kinds, and
 * an account is snapshotted at most once a batch; either refusal is
 * %VENTURE_ERROR_INVALID_ARGUMENT, and the rows then upsert alone.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_account_snapshot(
	VentureFeedBatch	 *self,
	const gchar		 *account_key,
	gint64			  at,
	guint			  covers,
	GError			**error
);

/**
 * venture_feed_batch_add_balance:
 * @self: a batch
 * @balance: an account's money in one currency, minor units
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_balance(
	VentureFeedBatch		 *self,
	const VentureSeriesBalance	 *balance,
	GError				**error
);

/**
 * venture_feed_batch_add_holding:
 * @self: a batch
 * @holding: a quantity of an instrument an account holds at a place
 * @error: (out) (optional): return location for a #GError
 *
 * The same account, place and instrument twice in one batch is summed:
 * items in two bag slots are one holding.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_holding(
	VentureFeedBatch		 *self,
	const VentureSeriesHolding	 *holding,
	GError				**error
);

/**
 * venture_feed_batch_add_position:
 * @self: a batch
 * @position: one of the operator's open listings; prices in the data
 *   source's currency
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_position(
	VentureFeedBatch		 *self,
	const VentureSeriesPosition	 *position,
	GError				**error
);

/**
 * venture_feed_batch_add_inbound:
 * @self: a batch
 * @inbound: something waiting for an account to collect; money in the
 *   data source's currency
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_inbound(
	VentureFeedBatch		 *self,
	const VentureSeriesInbound	 *inbound,
	GError				**error
);

/**
 * venture_feed_batch_add_txn:
 * @self: a batch
 * @txn: a row of the source's ledger; money in the data source's currency
 * @error: (out) (optional): return location for a #GError
 *
 * Upserted on its key: the same key sent again with a larger quantity
 * updates the row, it never adds a second.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_txn(
	VentureFeedBatch	 *self,
	const VentureSeriesTxn	 *txn,
	GError			**error
);

/**
 * venture_feed_batch_get_accounts:
 * @self: a batch
 * @out: (out caller-allocates): a view of its account rows
 *
 * Fills @out with the batch's own arrays (its currency left %NULL). The
 * view borrows from @self and is valid while @self is.
 *
 * Returns: whether the batch carries anything about accounts
 */
gboolean
venture_feed_batch_get_accounts(
	VentureFeedBatch		*self,
	VentureSeriesAccountBatch	*out
);

/**
 * venture_feed_batch_add_quote:
 * @self: a batch
 * @venue_key: who quotes it
 * @instrument_key: what is quoted
 * @side: back, lay, bid or ask
 * @value: a price in minor units, or decimal odds times
 *   %VENTURE_SERIES_ODDS_SCALE when @currency is %NULL
 * @currency: (nullable): the price's currency; %NULL for odds
 * @liquidity: what is available at it, minor units, or %VENTURE_SERIES_NONE
 * @taken_at: when it was quoted (Unix seconds)
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_quote(
	VentureFeedBatch	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	VentureSeriesQuoteSide	  side,
	gint64			  value,
	const gchar		 *currency,
	gint64			  liquidity,
	gint64			  taken_at,
	GError			**error
);

/**
 * venture_feed_batch_add_entry:
 * @self: a batch
 * @key: the entry's key at the source
 * @title: its title
 * @url: (nullable): an http or https address
 * @summary: (nullable): its text
 * @published_at: when it was published (Unix seconds), or %VENTURE_SERIES_NONE
 * @venue_key: (nullable): the venue it is about
 * @instrument_key: (nullable): the instrument it is about
 * @error: (out) (optional): return location for a #GError
 *
 * A non-price item: an article, a notice.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_entry(
	VentureFeedBatch	 *self,
	const gchar		 *key,
	const gchar		 *title,
	const gchar		 *url,
	const gchar		 *summary,
	gint64			  published_at,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	GError			**error
);

/**
 * venture_feed_batch_add_record:
 * @self: a batch
 * @record_type: a record type name, lower case: price_observation
 * @fields: the record's fields in the wire spelling (underscores)
 * @match: (nullable) (array zero-terminated=1): the fields that find an
 *   existing record to update; %NULL for the type's declared keys
 * @error: (out) (optional): return location for a #GError
 *
 * An upsert into the main database. The worker never applies it: the main
 * thread does, after the run, and only for a type the source accepts
 * (its settings' `record_types`) and the feeds module allows.
 *
 * Returns: %TRUE when it was added
 */
gboolean
venture_feed_batch_add_record(
	VentureFeedBatch	 *self,
	const gchar		 *record_type,
	JsonObject		 *fields,
	const gchar *const	 *match,
	GError			**error
);

/**
 * venture_feed_batch_set_cursor:
 * @self: a batch
 * @cursor: (nullable): where the unit's next fetch resumes
 *
 * Kept per unit in the source's store and handed back in the next
 * request's cursor.
 */
void
venture_feed_batch_set_cursor(
	VentureFeedBatch	*self,
	const gchar		*cursor
);

/**
 * venture_feed_batch_get_cursor:
 * @self: a batch
 *
 * Returns: (transfer none) (nullable): the cursor
 */
const gchar *
venture_feed_batch_get_cursor(VentureFeedBatch *self);

/**
 * venture_feed_batch_set_not_modified:
 * @self: a batch
 * @not_modified: whether the unit answered that nothing changed
 */
void
venture_feed_batch_set_not_modified(
	VentureFeedBatch	*self,
	gboolean		 not_modified
);

/**
 * venture_feed_batch_get_not_modified:
 * @self: a batch
 *
 * Returns: whether nothing changed
 */
gboolean
venture_feed_batch_get_not_modified(VentureFeedBatch *self);

/**
 * venture_feed_batch_set_units_changed:
 * @self: a batch
 * @changed: whether this fetch changed what the provider's units() lists
 *
 * Says that the source's unit list is now different -- a provider that
 * discovers its units, as the Blizzard plugin reads the realm index,
 * learned a new set. The worker then closes the source's open run at the
 * end of this pass instead of at the end of its run window, so the
 * after-run hooks run (and can queue a refresh that freezes the new units)
 * within seconds rather than up to a window later.
 */
void
venture_feed_batch_set_units_changed(
	VentureFeedBatch	*self,
	gboolean		 changed
);

/**
 * venture_feed_batch_get_units_changed:
 * @self: a batch
 *
 * Returns: whether the fetch changed the source's unit list
 */
gboolean
venture_feed_batch_get_units_changed(VentureFeedBatch *self);

/**
 * venture_feed_batch_set_error:
 * @self: a batch
 * @message: (nullable): what went wrong after some of the batch was made
 * @retry_after: seconds the producer asked to be left alone, or 0
 *
 * A partial batch: what was added is stored, and the unit's run is
 * recorded as partial with this message.
 */
void
venture_feed_batch_set_error(
	VentureFeedBatch	*self,
	const gchar		*message,
	gint64			 retry_after
);

/**
 * venture_feed_batch_get_error:
 * @self: a batch
 * @out_retry_after: (out) (optional): the retry delay asked for
 *
 * Returns: (transfer none) (nullable): the partial batch's message
 */
const gchar *
venture_feed_batch_get_error(
	VentureFeedBatch	*self,
	gint64			*out_retry_after
);

/**
 * venture_feed_batch_add_note:
 * @self: a batch
 * @note: something worth putting on the run: a skipped line, a fallback
 */
void
venture_feed_batch_add_note(
	VentureFeedBatch	*self,
	const gchar		*note
);

/**
 * venture_feed_batch_add_refused:
 * @self: a batch
 * @count: items the provider refused while building the batch
 */
void
venture_feed_batch_add_refused(
	VentureFeedBatch	*self,
	gint64			 count
);

/**
 * venture_feed_batch_set_remote_quota:
 * @self: a batch
 * @used: requests the far end says this account has spent, at least 0
 * @remaining: requests it says are left, at least 0
 *
 * The far end's own account of its quota -- the-odds-api's
 * x-requests-used and x-requests-remaining headers. The run records it in
 * place of the source's hourly budget, because it is the count the far end
 * will enforce. A negative figure is ignored.
 */
void
venture_feed_batch_set_remote_quota(
	VentureFeedBatch	*self,
	gint64			 used,
	gint64			 remaining
);

/**
 * venture_feed_batch_get_remote_quota:
 * @self: a batch
 * @out_used: (out) (optional): requests spent
 * @out_remaining: (out) (optional): requests left
 *
 * Returns: whether the batch carries the far end's quota
 */
gboolean
venture_feed_batch_get_remote_quota(
	VentureFeedBatch	*self,
	gint64			*out_used,
	gint64			*out_remaining
);

/**
 * venture_feed_batch_count_items:
 * @self: a batch
 *
 * Returns: everything it carries: venues, instruments, listings, figures,
 *   quotes, entries and records
 */
gint64
venture_feed_batch_count_items(VentureFeedBatch *self);

/**
 * venture_feed_batch_describe:
 * @self: a batch
 *
 * One line saying what the batch holds, for a test fetch's report.
 *
 * Returns: (transfer full): the summary
 */
gchar *
venture_feed_batch_describe(VentureFeedBatch *self);

/**
 * venture_feed_batch_add_jsonl:
 * @self: a batch
 * @messages: (element-type VentureJsonlMessage): protocol-1 messages, in order
 * @default_currency: (nullable): the source's currency, for a price whose
 *   message and venue name none
 * @fetched_at: when the messages were read (Unix seconds); the time of
 *   anything that carries none
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the JSON-lines vocabulary (docs/plugins.org) into the batch: the
 * one translation shared by the file_jsonl, push and exec providers.
 * Prices are decimal strings converted exactly at the currency's exponent;
 * one with more places than the currency has is refused, counted and
 * skipped. A listing with no snapshot before it gets one, incomplete, at
 * its own time. An `error` message makes the batch partial; `log` lines
 * become notes at most twenty a batch; a `result` is ignored. The
 * account-operations messages' money is in @default_currency, the data
 * source's, and a `balance` in any other currency is refused.
 *
 * It may be called more than once on one batch with consecutive slices of
 * one stream: what a later message needs from an earlier one (a venue's
 * currency, an account's snapshot) is kept on the batch, so a large
 * stream need not be held as messages all at once.
 *
 * Returns: %TRUE unless a message could not be read at all
 */
gboolean
venture_feed_batch_add_jsonl(
	VentureFeedBatch	 *self,
	GPtrArray		 *messages,
	const gchar		 *default_currency,
	gint64			  fetched_at,
	GError			**error
);

/**
 * venture_feed_decimal_to_minor:
 * @text: a decimal such as "12.50"; an optional leading minus
 * @currency: the currency it is in
 * @out_minor: (out): the amount in minor units
 * @error: (out) (optional): return location for a #GError
 *
 * Exact conversion of a decimal string to minor units at the currency's
 * exponent. More places than the currency has are refused rather than
 * rounded: a feed that quotes a tenth of a cent is quoting something this
 * currency cannot hold, and rounding it would invent a price.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_feed_decimal_to_minor(
	const gchar	 *text,
	const gchar	 *currency,
	gint64		 *out_minor,
	GError		**error
);

/**
 * venture_feed_decimal_to_odds:
 * @text: decimal, American (+150, -200) or fractional (5/2) odds
 * @format: (nullable): "decimal", "american" or "fractional"; %NULL is decimal
 * @out_scaled: (out): decimal odds times %VENTURE_SERIES_ODDS_SCALE
 * @error: (out) (optional): return location for a #GError
 *
 * Converts odds to the store's scaled decimal form exactly; a fractional
 * or American price is rounded half to even at the sixth decimal place.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_feed_decimal_to_odds(
	const gchar	 *text,
	const gchar	 *format,
	gint64		 *out_scaled,
	GError		**error
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureFeedBatch, venture_feed_batch_unref)

G_END_DECLS

#endif /* VENTURE_FEED_BATCH_H */
