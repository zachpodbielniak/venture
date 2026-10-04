/*
 * venture-series-store.h - One data source's market data, in its own file
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A market data source returns far more than the main database is for: a
 * World of Warcraft region is some 186 realms times ten thousand items,
 * every hour, out of a hundred thousand listings per realm. The series
 * store keeps all of it -- every price the source ever reported, reduced
 * to compact figures as it arrives -- in one SQLite file per data source,
 * <directory>/store.db, outside VentureDatabase.
 *
 * It is the one place VENTURE hand-writes SQL for data. That is a
 * deliberate exception to "everything derives from the field table": the
 * rows here are derived, append-heavy and high-volume, closer to the
 * audit log than to a record, and none of them is ever edited by a
 * person, shown in a generic form or audited. docs/market-data.org has the
 * schema, every formula and the reasons.
 *
 * Threading. A handle is one SQLite connection and belongs to the thread
 * that uses it. There is one writer handle, venture_series_store_open(),
 * owned by whatever drives ingestion (a worker thread, in the feeds
 * module); any number of reader handles,
 * venture_series_store_open_reader(), can be used at the same time from
 * other threads, and WAL mode lets them read while the writer writes.
 * Nothing in this file touches VentureDatabase, the entity registry, the
 * configuration or anything else that is not thread-safe, and it logs
 * with g_debug() and g_message() only, never g_warning(): limits arrive
 * as arguments, read from the configuration on the main thread by the
 * caller.
 *
 * Times are Unix seconds, UTC. Every price is integer minor units with a
 * currency code beside it; odds are decimal odds times
 * %VENTURE_SERIES_ODDS_SCALE. A figure that does not exist is
 * %VENTURE_SERIES_NONE, never zero.
 *
 * Only built with SQLite (VENTURE_HAVE_SQLITE): a SQLITE=0 build has no
 * store and no market data pages.
 */

#ifndef VENTURE_SERIES_STORE_H
#define VENTURE_SERIES_STORE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

#include "series/venture-series-math.h"
#include "boxed/venture-money.h"

G_BEGIN_DECLS

/**
 * VENTURE_SERIES_STORE_FILENAME:
 *
 * The file a store keeps inside the directory it is given.
 */
#define VENTURE_SERIES_STORE_FILENAME "store.db"

/**
 * VENTURE_SERIES_ODDS_SCALE:
 *
 * Decimal odds are stored as an integer times this: 2.15 is 2150000.
 * Odds are a ratio, not money, but an integer keeps two sources quoting
 * the same odds equal.
 */
#define VENTURE_SERIES_ODDS_SCALE (1000000)

/**
 * VENTURE_SERIES_MAX_KEY_LENGTH:
 *
 * The longest venue or instrument key, in bytes.
 */
#define VENTURE_SERIES_MAX_KEY_LENGTH (512)

/**
 * VENTURE_SERIES_MAX_PAGE:
 *
 * The most rows one listing call returns.
 */
#define VENTURE_SERIES_MAX_PAGE (1000)

/**
 * VENTURE_SERIES_DEFAULT_PAGE:
 *
 * The rows a listing call returns when it asks for none in particular.
 */
#define VENTURE_SERIES_DEFAULT_PAGE (50)

#define VENTURE_TYPE_SERIES_STORE (venture_series_store_get_type())

G_DECLARE_FINAL_TYPE(VentureSeriesStore, venture_series_store,
                     VENTURE, SERIES_STORE, GObject)

/**
 * VentureSeriesSide:
 * @VENTURE_SERIES_SIDE_SELL: an offer to sell (an auction, an ask)
 * @VENTURE_SERIES_SIDE_BUY: an offer to buy (a bid, a buy order)
 *
 * Which side of the book a listing is on.
 */
typedef enum
{
	VENTURE_SERIES_SIDE_SELL = 0,
	VENTURE_SERIES_SIDE_BUY
} VentureSeriesSide;

/**
 * VentureSeriesQuoteSide:
 * @VENTURE_SERIES_QUOTE_BACK: odds to back an outcome
 * @VENTURE_SERIES_QUOTE_LAY: odds to lay an outcome on an exchange
 * @VENTURE_SERIES_QUOTE_BID: a price a venue would pay
 * @VENTURE_SERIES_QUOTE_ASK: a price a venue would sell at
 *
 * What a quote offers.
 */
typedef enum
{
	VENTURE_SERIES_QUOTE_BACK = 0,
	VENTURE_SERIES_QUOTE_LAY,
	VENTURE_SERIES_QUOTE_BID,
	VENTURE_SERIES_QUOTE_ASK
} VentureSeriesQuoteSide;

/**
 * VentureSeriesSort:
 * @VENTURE_SERIES_SORT_MIN_PRICE: the venue's lowest price
 * @VENTURE_SERIES_SORT_MARKET_VALUE: the market value
 * @VENTURE_SERIES_SORT_QUANTITY: units offered
 * @VENTURE_SERIES_SORT_LISTINGS: listings
 * @VENTURE_SERIES_SORT_PCT_VS_REGION: the minimum as a percent of the
 *   region median
 * @VENTURE_SERIES_SORT_DEAL_PRICE: the region's deal price
 * @VENTURE_SERIES_SORT_REGION_MEDIAN: the region median
 * @VENTURE_SERIES_SORT_NAME: the instrument's name
 * @VENTURE_SERIES_SORT_UPDATED: when the row was last updated
 * @VENTURE_SERIES_SORT_VENUE: the venue's key
 * @VENTURE_SERIES_SORT_SALE_RATE: the share of units that sold rather than
 *   expired over fourteen days
 * @VENTURE_SERIES_SORT_SOLD_PER_DAY: units sold per day with history over
 *   fourteen days
 *
 * The columns a listing can be sorted by: an allowlist, so a sort named
 * in a query string never reaches SQL as text.
 */
typedef enum
{
	VENTURE_SERIES_SORT_MIN_PRICE = 0,
	VENTURE_SERIES_SORT_MARKET_VALUE,
	VENTURE_SERIES_SORT_QUANTITY,
	VENTURE_SERIES_SORT_LISTINGS,
	VENTURE_SERIES_SORT_PCT_VS_REGION,
	VENTURE_SERIES_SORT_DEAL_PRICE,
	VENTURE_SERIES_SORT_REGION_MEDIAN,
	VENTURE_SERIES_SORT_NAME,
	VENTURE_SERIES_SORT_UPDATED,
	VENTURE_SERIES_SORT_VENUE,
	VENTURE_SERIES_SORT_SALE_RATE,
	VENTURE_SERIES_SORT_SOLD_PER_DAY
} VentureSeriesSort;

/* --- What goes in ---------------------------------------------------------- */

/**
 * VentureSeriesVenue:
 * @key: the source's key for the venue; required
 * @namespace_: the namespace the key belongs to, or %NULL
 * @name: a readable name, or %NULL to keep the one stored
 * @kind: a free-form kind (realm, marketplace, bookmaker), or %NULL
 * @group_key: the group the venue is priced against (a region), or %NULL
 * @currency: the venue's currency, or %NULL
 * @attrs_json: a JSON object of anything else, or %NULL
 *
 * A venue to create or update. A %NULL member keeps what is stored.
 */
typedef struct
{
	const gchar	*key;
	const gchar	*namespace_;
	const gchar	*name;
	const gchar	*kind;
	const gchar	*group_key;
	const gchar	*currency;
	const gchar	*attrs_json;
} VentureSeriesVenue;

/**
 * VentureSeriesInstrument:
 * @key: the source's key for the instrument; required
 * @namespace_: the namespace the key belongs to, or %NULL
 * @name: a readable name, or %NULL to keep the one stored
 * @kind: a free-form kind (item, outcome, sku), or %NULL
 * @category: a "/"-separated category path, or %NULL
 * @parent_key: the key of a parent instrument (an event for an outcome),
 *   or %NULL
 * @attrs_json: a JSON object of anything else, or %NULL
 *
 * An instrument to create or update. A %NULL member keeps what is stored.
 */
typedef struct
{
	const gchar	*key;
	const gchar	*namespace_;
	const gchar	*name;
	const gchar	*kind;
	const gchar	*category;
	const gchar	*parent_key;
	const gchar	*attrs_json;
} VentureSeriesInstrument;

/**
 * VentureSeriesListing:
 * @instrument_key: what is listed; required
 * @listing_id: the source's id for the listing, or 0 when it has none (see
 *   venture_series_listing_id_from_string() for sources whose ids are text)
 * @unit_price: the price of one unit in minor units, at least zero
 * @quantity: units listed, at least one
 * @side: which side of the book
 * @expires_in_min: the least time, in seconds, the listing has left, or -1
 *   when the source does not say (a World of Warcraft "long" auction has
 *   at least 7200)
 *
 * One listing in a snapshot.
 */
typedef struct
{
	const gchar		*instrument_key;
	guint64			 listing_id;
	gint64			 unit_price;
	gint64			 quantity;
	VentureSeriesSide	 side;
	gint64			 expires_in_min;
} VentureSeriesListing;

/**
 * VentureSeriesStats:
 * @instrument_key: what the figures are for; required
 * @min_price: the lowest price, or %VENTURE_SERIES_NONE
 * @market_value: a market value, or %VENTURE_SERIES_NONE
 * @mean: a mean price, or %VENTURE_SERIES_NONE
 * @median: a median price, or %VENTURE_SERIES_NONE
 * @sale_avg: an average sale price, or %VENTURE_SERIES_NONE
 * @quantity: units offered, or %VENTURE_SERIES_NONE
 * @listings: listings, or %VENTURE_SERIES_NONE
 * @sold: units sold since the last snapshot, or %VENTURE_SERIES_NONE
 * @source_figures: whether the next three members are set at all; a
 *   caller that cleared the struct with memset() leaves it FALSE, and
 *   then a zero in them is not read as a sale rate of nothing
 * @historical: the source's own long-run price (TSM's historical), or
 *   %VENTURE_SERIES_NONE
 * @sale_rate: the source's own sale rate, 0 to 1, or NAN
 * @sold_per_day: the source's own units sold a day, or NAN
 *
 * Figures a source computed itself, for an instrument it does not list
 * one by one. At least one figure is required. Start from
 * venture_series_stats_init(): "none" is %VENTURE_SERIES_NONE or NAN,
 * never zero.
 *
 * The last three are kept beside the store's own estimates, never over
 * them: venture_series_store_reference() answers with the store's figure
 * when it has one and the source's only when it has none, and says which.
 */
typedef struct
{
	const gchar	*instrument_key;
	gint64		 min_price;
	gint64		 market_value;
	gint64		 mean;
	gint64		 median;
	gint64		 sale_avg;
	gint64		 quantity;
	gint64		 listings;
	gint64		 sold;
	gboolean	 source_figures;
	gint64		 historical;
	gdouble		 sale_rate;
	gdouble		 sold_per_day;
} VentureSeriesStats;

/**
 * venture_series_stats_init:
 * @stats: (out caller-allocates): figures to clear
 *
 * Sets every figure to "none": %VENTURE_SERIES_NONE, and NAN for the two
 * ratios, with no instrument; @source_figures is TRUE, so the source's
 * own figures are read once a caller sets them.
 */
void
venture_series_stats_init(VentureSeriesStats *stats);

/**
 * VentureSeriesQuote:
 * @venue_key: the venue quoting; required
 * @instrument_key: what is quoted; required
 * @side: what the quote offers
 * @value: decimal odds times %VENTURE_SERIES_ODDS_SCALE when @currency is
 *   %NULL, else a price in minor units of @currency
 * @currency: the price's currency, or %NULL for odds
 * @liquidity: the most that can be staked or bought, in minor units of
 *   the venue's currency, or %VENTURE_SERIES_NONE
 * @taken_at: when the quote was current
 *
 * One price or set of odds.
 */
typedef struct
{
	const gchar		*venue_key;
	const gchar		*instrument_key;
	VentureSeriesQuoteSide	 side;
	gint64			 value;
	const gchar		*currency;
	gint64			 liquidity;
	gint64			 taken_at;
} VentureSeriesQuote;

/**
 * VentureSeriesEntry:
 * @key: the source's id for the entry (an RSS guid); required
 * @title: required
 * @url: a link, or %NULL
 * @summary: a summary, or %NULL
 * @published_at: when it was published, or %VENTURE_SERIES_NONE
 * @venue_key: a venue it concerns, or %NULL
 * @instrument_key: an instrument it concerns, or %NULL
 * @attrs_json: a JSON object of anything else, or %NULL
 *
 * A non-price item: a feed article, a notice.
 */
typedef struct
{
	const gchar	*key;
	const gchar	*title;
	const gchar	*url;
	const gchar	*summary;
	gint64		 published_at;
	const gchar	*venue_key;
	const gchar	*instrument_key;
	const gchar	*attrs_json;
} VentureSeriesEntry;

/**
 * VentureSeriesCommitResult:
 * @duplicate: the snapshot had already been applied, so nothing was written
 * @late: the snapshot is older than the venue's newest, so only its
 *   history (hourly and daily) was written
 * @listings: listings accepted
 * @listings_refused: listings dropped, because their instrument was
 *   refused or a figure overflowed
 * @instruments: instruments the snapshot touched
 * @instruments_new: instruments created
 * @instruments_refused: new instruments refused because the store is past
 *   its size cap
 * @rows_written: rows inserted or updated
 * @sold_estimate: units estimated sold since the venue's last snapshot
 *
 * What applying a snapshot or a batch did.
 */
typedef struct
{
	gboolean	duplicate;
	gboolean	late;
	gint64		listings;
	gint64		listings_refused;
	gint64		instruments;
	gint64		instruments_new;
	gint64		instruments_refused;
	gint64		rows_written;
	gint64		sold_estimate;
} VentureSeriesCommitResult;

/**
 * VentureSeriesPurgeResult:
 * @hourly: hourly rows deleted
 * @daily: daily rows deleted
 * @quotes: quote history rows deleted
 * @snapshots: snapshot rows deleted
 * @entries: entries deleted
 * @balances: account balance history rows deleted (never an account's
 *   newest in a currency)
 * @txns: external ledger rows deleted
 *
 * What a purge deleted.
 */
typedef struct
{
	gint64	hourly;
	gint64	daily;
	gint64	quotes;
	gint64	snapshots;
	gint64	entries;
	gint64	balances;
	gint64	txns;
} VentureSeriesPurgeResult;

/**
 * VentureSeriesRegionResult:
 * @groups: groups recomputed
 * @rows: region rows written
 * @current_updated: current rows whose region figures changed
 *
 * What a region recompute did.
 */
typedef struct
{
	gint64	groups;
	gint64	rows;
	gint64	current_updated;
} VentureSeriesRegionResult;

/* --- What comes out ------------------------------------------------------- */

/**
 * VentureSeriesRow:
 * @venue_key: the venue
 * @venue_name: (nullable): its name
 * @group_key: its group, "" when it has none
 * @instrument_key: the instrument
 * @instrument_name: (nullable): its name
 * @category: (nullable): its category path
 * @kind: (nullable): its kind
 * @currency: the currency of every price in the row
 * @taken_at: the snapshot the row reflects
 * @seen_at: the last snapshot that listed the instrument here
 * @quantity: units offered (0 when it is out of stock), or
 *   %VENTURE_SERIES_NONE when the source did not say
 * @listings: listings
 * @min_price: the lowest price
 * @market_value: the market value
 * @median: the median unit price
 * @p15: the 15th-percentile unit price
 * @mean: the mean unit price
 * @stddev: the standard deviation of unit prices
 * @bid_price: the best buy order
 * @bid_quantity: units wanted at any price
 * @region_median: the median of the minimums across the venue's group
 * @deal_price: the group's deal price
 * @pct_vs_region: @min_price as a percent of @region_median (80 is 20%
 *   cheaper), or NAN
 * @stock_changed_at: the snapshot at which @quantity last crossed zero, in
 *   either direction, or %VENTURE_SERIES_NONE when it never has (a row's
 *   first sighting is not a change). Equal to @taken_at exactly when the
 *   newest snapshot is the one that took it out of or put it back in stock.
 * @sale_rate: units sold / (sold + expired) over the last fourteen days, as
 *   of the last region recompute; NAN when nothing sold or expired, or the
 *   recompute has not run since the row's history began
 * @sold_per_day: units sold per day with history over the same days, or
 *   NAN
 * @source_historical: the source's own historical price from its last
 *   statistics, or %VENTURE_SERIES_NONE
 * @source_sale_rate: the source's own sale rate, or NAN
 * @source_sold_per_day: the source's own units sold a day, or NAN
 *
 * One instrument at one venue, now. Every price is
 * %VENTURE_SERIES_NONE when unknown. The store's estimates and the
 * source's figures are separate members on purpose: a page that shows
 * one must be able to say which it is.
 */
typedef struct
{
	gchar	*venue_key;
	gchar	*venue_name;
	gchar	*group_key;
	gchar	*instrument_key;
	gchar	*instrument_name;
	gchar	*category;
	gchar	*kind;
	gchar	 currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64	 taken_at;
	gint64	 seen_at;
	gint64	 quantity;
	gint64	 listings;
	gint64	 min_price;
	gint64	 market_value;
	gint64	 median;
	gint64	 p15;
	gint64	 mean;
	gint64	 stddev;
	gint64	 bid_price;
	gint64	 bid_quantity;
	gint64	 region_median;
	gint64	 deal_price;
	gdouble	 pct_vs_region;
	gint64	 stock_changed_at;
	gdouble	 sale_rate;
	gdouble	 sold_per_day;
	gint64	 source_historical;
	gdouble	 source_sale_rate;
	gdouble	 source_sold_per_day;
} VentureSeriesRow;

/**
 * venture_series_row_free:
 * @row: (transfer full) (nullable): a row
 */
void
venture_series_row_free(VentureSeriesRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesRow, venture_series_row_free)

/**
 * VentureSeriesFilter:
 * @venue_keys: (array zero-terminated=1) (nullable): only these venues
 * @group_key: (nullable): only venues in this group
 * @instrument_key: (nullable): only this instrument
 * @search: (nullable): only instruments whose name or key contains this,
 *   ignoring case
 * @category_prefix: (nullable): only this category and the ones beneath
 *   it
 * @min_value: only rows whose market value (or, without one, minimum) is
 *   at least this, in @min_value_currency; %VENTURE_SERIES_NONE for no
 *   bound
 * @min_value_currency: (nullable): the currency of @min_value, required
 *   with it: a bound in one currency says nothing about another
 * @in_stock_only: leave out rows with nothing offered
 * @deals_only: only rows in stock whose minimum is at or under their
 *   group's deal price
 * @max_pct_vs_region: only rows whose minimum is at most this percent of
 *   the region median (80 is 20% cheaper); NAN for no bound
 * @sort: the column to sort by
 * @descending: sort largest first
 * @offset: rows to skip
 * @count: rows to return, 0 for %VENTURE_SERIES_DEFAULT_PAGE, at most
 *   %VENTURE_SERIES_MAX_PAGE
 *
 * Narrows and orders venture_series_store_list_current(). Zero-filled
 * means everything, sorted by minimum price -- except @min_value, which
 * must be %VENTURE_SERIES_NONE to mean no bound, and @max_pct_vs_region,
 * which must be NAN; use venture_series_filter_init().
 */
typedef struct
{
	const gchar *const	*venue_keys;
	const gchar		*group_key;
	const gchar		*instrument_key;
	const gchar		*search;
	const gchar		*category_prefix;
	gint64			 min_value;
	const gchar		*min_value_currency;
	gboolean		 in_stock_only;
	gboolean		 deals_only;
	gdouble			 max_pct_vs_region;
	VentureSeriesSort	 sort;
	gboolean		 descending;
	guint			 offset;
	guint			 count;
} VentureSeriesFilter;

/**
 * venture_series_filter_init:
 * @filter: (out caller-allocates): a filter
 *
 * Sets @filter to "everything, cheapest first, the default page".
 */
void
venture_series_filter_init(VentureSeriesFilter *filter);

/**
 * VentureSeriesPoint:
 * @at: the hour the point is for (the snapshot's time, truncated to the
 *   hour)
 * @currency: the currency of the prices
 * @min_price: the lowest price
 * @quantity: units offered
 * @market_value: the market value
 * @listings: listings
 *
 * One hour of one instrument at one venue: the last snapshot taken in
 * that hour.
 */
typedef struct
{
	gint64	at;
	gchar	currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64	min_price;
	gint64	quantity;
	gint64	market_value;
	gint64	listings;
} VentureSeriesPoint;

/**
 * VentureSeriesDay:
 * @day_start: midnight UTC of the day
 * @currency: the currency of the prices
 * @snapshots: snapshots folded into the day
 * @min_price: the lowest price seen all day
 * @max_quantity: the most units offered in any one snapshot
 * @price_at_max: the minimum price in that snapshot
 * @market_value: the market value in that snapshot
 * @mean: the mean price in that snapshot
 * @listings: listings in that snapshot
 * @sold_estimate: units estimated sold during the day
 * @expired_estimate: units that vanished when they may have expired
 * @sale_avg: what the units sold for on average
 *
 * One day of one instrument at one venue. The day keeps the snapshot
 * with the most units on offer, as Undermine Exchange does: the fullest
 * market of the day is the one that says most about its prices.
 */
typedef struct
{
	gint64	day_start;
	gchar	currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64	snapshots;
	gint64	min_price;
	gint64	max_quantity;
	gint64	price_at_max;
	gint64	market_value;
	gint64	mean;
	gint64	listings;
	gint64	sold_estimate;
	gint64	expired_estimate;
	gint64	sale_avg;
} VentureSeriesDay;

/**
 * VentureSeriesRegion:
 * @found: whether the group lists the instrument at all
 * @group_key: the group
 * @currency: the currency of the prices
 * @computed_at: when the figures were computed
 * @venues_offering: venues with the instrument in stock
 * @total_quantity: units offered across them
 * @median_min: the median of their minimums
 * @p33: the minimum a third of the way up
 * @deal_price: the deal price (see venture_series_math_deal_price()), or
 *   %VENTURE_SERIES_NONE when the median is below the recompute's minimum
 *   value
 * @market_avg: the mean of their market values
 *
 * One instrument across a group of venues.
 */
typedef struct
{
	gboolean	found;
	gchar		group_key[VENTURE_SERIES_MAX_KEY_LENGTH + 1];
	gchar		currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64		computed_at;
	gint64		venues_offering;
	gint64		total_quantity;
	gint64		median_min;
	gint64		p33;
	gint64		deal_price;
	gint64		market_avg;
} VentureSeriesRegion;

/**
 * VentureSeriesVenueStats:
 * @venue_key: the venue
 * @venue_name: (nullable): its name
 * @group_key: its group
 * @instruments: instruments in stock with a region median to compare
 * @cheaper: of those, how many are below the region median
 * @equal: how many are at it
 * @dearer: how many are above it
 * @avg_ratio: the mean of minimum / region median, or NAN
 * @listings: listings across every instrument
 * @last_taken_at: the venue's newest snapshot, or %VENTURE_SERIES_NONE
 * @interval_seconds: its learned update interval
 *
 * How cheap one venue is against its group: FlippingPal's realm index.
 */
typedef struct
{
	gchar	*venue_key;
	gchar	*venue_name;
	gchar	*group_key;
	gint64	 instruments;
	gint64	 cheaper;
	gint64	 equal;
	gint64	 dearer;
	gdouble	 avg_ratio;
	gint64	 listings;
	gint64	 last_taken_at;
	gint64	 interval_seconds;
} VentureSeriesVenueStats;

/**
 * venture_series_venue_stats_free:
 * @stats: (transfer full) (nullable): venue stats
 */
void
venture_series_venue_stats_free(VentureSeriesVenueStats *stats);

/**
 * VentureSeriesVenueRow:
 * @key: the venue's key
 * @namespace_: (nullable): its namespace
 * @name: (nullable): its name
 * @kind: (nullable): its kind
 * @group_key: its group, "" when none
 * @currency: (nullable): its currency
 * @attrs_json: (nullable): its attributes
 * @first_seen: when the store first saw it
 * @last_seen: when the store last saw it
 *
 * A venue as stored.
 */
typedef struct
{
	gchar	*key;
	gchar	*namespace_;
	gchar	*name;
	gchar	*kind;
	gchar	*group_key;
	gchar	*currency;
	gchar	*attrs_json;
	gint64	 first_seen;
	gint64	 last_seen;
} VentureSeriesVenueRow;

/**
 * venture_series_venue_row_free:
 * @row: (transfer full) (nullable): a venue row
 */
void
venture_series_venue_row_free(VentureSeriesVenueRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesVenueRow, venture_series_venue_row_free)

/**
 * VentureSeriesInstrumentRow:
 * @key: the instrument's key
 * @namespace_: (nullable): its namespace
 * @name: (nullable): its name
 * @kind: (nullable): its kind
 * @category: (nullable): its category path
 * @parent_key: (nullable): its parent's key
 * @attrs_json: (nullable): its attributes
 * @first_seen: when the store first saw it
 * @last_seen: when the store last saw it
 *
 * An instrument as stored.
 */
typedef struct
{
	gchar	*key;
	gchar	*namespace_;
	gchar	*name;
	gchar	*kind;
	gchar	*category;
	gchar	*parent_key;
	gchar	*attrs_json;
	gint64	 first_seen;
	gint64	 last_seen;
} VentureSeriesInstrumentRow;

/**
 * venture_series_instrument_row_free:
 * @row: (transfer full) (nullable): an instrument row
 */
void
venture_series_instrument_row_free(VentureSeriesInstrumentRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesInstrumentRow,
                              venture_series_instrument_row_free)

/**
 * VentureSeriesQuoteRow:
 * @venue_key: the venue quoting
 * @venue_name: (nullable): its name
 * @instrument_key: the instrument
 * @parent_key: (nullable): the instrument's parent
 * @side: what the quote offers
 * @value: odds times %VENTURE_SERIES_ODDS_SCALE, or a price in minor units
 * @currency: the price's currency, "" for odds
 * @liquidity: the most that can be taken, or %VENTURE_SERIES_NONE
 * @taken_at: when the quote was current
 *
 * The latest quote of one side of one instrument at one venue.
 */
typedef struct
{
	gchar			*venue_key;
	gchar			*venue_name;
	gchar			*instrument_key;
	gchar			*parent_key;
	VentureSeriesQuoteSide	 side;
	gint64			 value;
	gchar			 currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64			 liquidity;
	gint64			 taken_at;
} VentureSeriesQuoteRow;

/**
 * venture_series_quote_row_free:
 * @row: (transfer full) (nullable): a quote row
 */
void
venture_series_quote_row_free(VentureSeriesQuoteRow *row);

/**
 * VentureSeriesEntryRow:
 * @key: the entry's key
 * @title: its title
 * @url: (nullable): its link
 * @summary: (nullable): its summary
 * @published_at: when it was published, or %VENTURE_SERIES_NONE
 * @fetched_at: when the store first received it
 * @venue_key: (nullable): the venue it concerns
 * @instrument_key: (nullable): the instrument it concerns
 *
 * A stored entry.
 */
typedef struct
{
	gchar	*key;
	gchar	*title;
	gchar	*url;
	gchar	*summary;
	gint64	 published_at;
	gint64	 fetched_at;
	gchar	*venue_key;
	gchar	*instrument_key;
} VentureSeriesEntryRow;

/**
 * venture_series_entry_row_free:
 * @row: (transfer full) (nullable): an entry row
 */
void
venture_series_entry_row_free(VentureSeriesEntryRow *row);

/**
 * VentureSeriesReference:
 * @currency: the currency of every price, "" when nothing was found
 * @market_14d: the 14-day weighted market value (see
 *   venture_series_math_ewma())
 * @historical_60d: the mean daily market value over 60 days
 * @sale_avg: what units sold for over 14 days
 * @sale_rate: units sold / (sold + expired) over 14 days, or NAN
 * @sold_per_day: units sold per day with data over 14 days, or NAN
 * @days_14: days with data in the 14-day window
 * @days_60: days with data in the 60-day window
 * @historical_from_source: @historical_60d is the source's own historical
 *   price, because the store had no daily history to compute one
 * @sale_rate_from_source: likewise for @sale_rate
 * @sold_per_day_from_source: likewise for @sold_per_day
 *
 * The slow-moving prices the oracle, the scanner and the pages compare a
 * snapshot against. With a group, each day's market value is the mean
 * across the group's venues and sales are summed across them.
 *
 * The store's own estimate always wins. A source's figure (a `stat`
 * message's historical, sale_rate or sold_per_day) fills a figure only
 * when the store could not compute it, only from a current row taken on
 * or before the moment asked about, and only in the currency the answer
 * is in; with a group it is the mean over the group's venues that sent
 * one.
 */
typedef struct
{
	gchar		currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64		market_14d;
	gint64		historical_60d;
	gint64		sale_avg;
	gdouble		sale_rate;
	gdouble		sold_per_day;
	gint64		days_14;
	gint64		days_60;
	gboolean	historical_from_source;
	gboolean	sale_rate_from_source;
	gboolean	sold_per_day_from_source;
} VentureSeriesReference;

/**
 * VentureSeriesVenueState:
 * @found: whether the store has the venue
 * @last_taken_at: its newest snapshot, or %VENTURE_SERIES_NONE
 * @last_fetched_at: when that snapshot was fetched, or %VENTURE_SERIES_NONE
 * @interval_seconds: its learned update interval
 * @gaps: gaps remembered, at most %VENTURE_SERIES_GAP_HISTORY
 * @next_expected: @last_taken_at + @interval_seconds, or
 *   %VENTURE_SERIES_NONE
 *
 * What the store has learned about when a venue updates.
 */
typedef struct
{
	gboolean	found;
	gint64		last_taken_at;
	gint64		last_fetched_at;
	gint64		interval_seconds;
	gint64		gaps;
	gint64		next_expected;
} VentureSeriesVenueState;

/* --- Opening ---------------------------------------------------------------- */

/**
 * venture_series_store_open:
 * @directory: the store's directory; created (mode 0700) if missing
 * @error: (out) (optional): return location for a #GError
 *
 * Opens the writer handle on @directory/store.db, creating the file and
 * bringing its schema up to this build's version. There must be only one
 * writer per store; a second waits on the first's transactions and gains
 * nothing.
 *
 * A file whose schema is newer than this build knows is refused with
 * %VENTURE_ERROR_MIGRATION rather than written under rules it was not
 * written to; a file that is not an SQLite database is refused with
 * %VENTURE_ERROR_DATABASE naming it.
 *
 * Returns: (transfer full) (nullable): the handle, or %NULL on error
 */
VentureSeriesStore *
venture_series_store_open(
	const gchar	 *directory,
	GError		**error
);

/**
 * venture_series_store_open_reader:
 * @directory: the store's directory
 * @error: (out) (optional): return location for a #GError
 *
 * Opens a read-only handle, for a thread other than the writer's. The
 * store must already exist at this build's schema version: a missing file
 * is %VENTURE_ERROR_NOT_FOUND, an older schema %VENTURE_ERROR_MIGRATION
 * (the writer upgrades it), a newer one %VENTURE_ERROR_MIGRATION.
 *
 * Returns: (transfer full) (nullable): the handle, or %NULL on error
 */
VentureSeriesStore *
venture_series_store_open_reader(
	const gchar	 *directory,
	GError		**error
);

/**
 * venture_series_store_is_reader:
 * @self: a #VentureSeriesStore
 *
 * Returns: whether @self is a read-only handle
 */
gboolean
venture_series_store_is_reader(VentureSeriesStore *self);

/**
 * venture_series_store_get_path:
 * @self: a #VentureSeriesStore
 *
 * Returns: (transfer none): the store file's path
 */
const gchar *
venture_series_store_get_path(VentureSeriesStore *self);

/**
 * venture_series_store_schema_version:
 *
 * Returns: the schema version this build writes
 */
guint
venture_series_store_schema_version(void);

/**
 * venture_series_store_schema_step:
 * @version: a schema version, 1 to venture_series_store_schema_version()
 *
 * The SQL that brings a store from @version - 1 to @version. Steps are
 * append-only: an applied step is never edited. Exposed for the
 * documentation and for the upgrade tests.
 *
 * Returns: (transfer none) (nullable): the SQL, or %NULL for a version
 *   that does not exist
 */
const gchar *
venture_series_store_schema_step(guint version);

/**
 * venture_series_store_set_max_bytes:
 * @self: the writer handle
 * @max_bytes: the size past which new instruments are refused, or 0 for no
 *   cap
 *
 * Caps the store. Past the cap, snapshots of known instruments carry on --
 * history the operator already pays for keeps arriving -- but an
 * instrument the store has never seen is refused and counted in
 * #VentureSeriesCommitResult.instruments_refused.
 */
void
venture_series_store_set_max_bytes(
	VentureSeriesStore	*self,
	guint64			 max_bytes
);

/**
 * venture_series_store_get_size:
 * @self: a #VentureSeriesStore
 * @out_bytes: (out): the bytes in use (pages holding data, not free ones)
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_get_size(
	VentureSeriesStore	 *self,
	guint64			 *out_bytes,
	GError			**error
);

/* --- Backups ------------------------------------------------------------------ */

/**
 * venture_series_store_backup:
 * @directory: the store's directory (the one venture_series_store_open() takes)
 * @destination: the file to write; it must not be inside @directory
 * @pages_per_step: pages copied between looks at @cancellable; 0 for the
 *   default, 4096 (about 16 MiB). Tests pass 1 to make a small store's copy
 *   span many of a concurrent writer's commits.
 * @cancellable: (nullable): checked between steps of the copy
 * @out_sha256: (out) (optional) (transfer full): the copy's SHA-256, hex
 * @out_size: (out) (optional): the copy's size in bytes
 * @error: (out) (optional): return location for a #GError
 *
 * Copies a store with SQLite's online backup API, on a connection of its
 * own, while its writer may be committing on another: the copy is taken
 * inside one read transaction, so it is the store as of one moment however
 * many steps the copy takes and however many commits land meanwhile (WAL
 * lets the writer go on). The copy is written beside @destination as
 * "@destination.partial" and renamed into place only when complete, and it
 * is left in rollback-journal mode so it is one self-contained file.
 *
 * It reads gigabytes for a large store: call it on the thread that owns
 * the store's writer (the series worker), never the main thread. It never
 * calls g_warning().
 *
 * Returns: %TRUE when @destination holds a complete copy
 */
gboolean
venture_series_store_backup(
	const gchar	 *directory,
	const gchar	 *destination,
	guint		  pages_per_step,
	GCancellable	 *cancellable,
	gchar		**out_sha256,
	guint64		 *out_size,
	GError		**error
);

/**
 * VentureSeriesStoreCheck:
 * @schema_version: the file's `user_version`
 * @intact: whether `PRAGMA integrity_check` answered "ok" and the version is
 *   one this build can open
 * @problems: (nullable): what was wrong, one per line; %NULL when intact
 * @venues: venue rows
 * @instruments: instrument rows
 * @current_rows: rows of the current table (venue x instrument)
 *
 * What venture_series_store_check_file() found.
 */
typedef struct
{
	gint64		 schema_version;
	gboolean	 intact;
	gchar		*problems;
	gint64		 venues;
	gint64		 instruments;
	gint64		 current_rows;
} VentureSeriesStoreCheck;

/**
 * venture_series_store_check_file:
 * @path: a store file, typically a backup copy
 * @out: (out caller-allocates): what was found; clear it with
 *   venture_series_store_check_clear()
 * @error: (out) (optional): return location for a #GError
 *
 * Opens @path immutable -- no lock, no journal, not one byte written, so a
 * retained backup's digest still matches afterwards -- and checks it is a
 * series store SQLite reads back whole. A file that is not an SQLite
 * database at all is an error; a damaged one is @out with @intact FALSE.
 *
 * Returns: %TRUE when the check ran
 */
gboolean
venture_series_store_check_file(
	const gchar		 *path,
	VentureSeriesStoreCheck	 *out,
	GError			**error
);

/**
 * venture_series_store_check_clear:
 * @check: a check filled by venture_series_store_check_file()
 *
 * Frees what @check holds; the struct itself is the caller's.
 */
void
venture_series_store_check_clear(VentureSeriesStoreCheck *check);

/* --- Transactions ------------------------------------------------------------- */

/**
 * venture_series_store_begin:
 * @self: the writer handle
 * @error: (out) (optional): return location for a #GError
 *
 * Opens a transaction that batches the upserts after it: ten thousand
 * instrument upserts are ten thousand transactions otherwise. Calls nest;
 * only the outermost commit writes. A snapshot's commit inside one joins
 * it.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_begin(
	VentureSeriesStore	 *self,
	GError			**error
);

/**
 * venture_series_store_commit:
 * @self: the writer handle
 * @error: (out) (optional): return location for a #GError
 *
 * Closes one level of venture_series_store_begin().
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_commit(
	VentureSeriesStore	 *self,
	GError			**error
);

/**
 * venture_series_store_rollback:
 * @self: the writer handle
 *
 * Abandons the whole transaction, whatever its depth.
 */
void
venture_series_store_rollback(VentureSeriesStore *self);

/* --- Venues, instruments, meta ------------------------------------------------ */

/**
 * venture_series_store_upsert_venue:
 * @self: the writer handle
 * @venue: the venue
 * @seen_at: when the source reported it
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the venue or updates the members @venue sets.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_upsert_venue(
	VentureSeriesStore		 *self,
	const VentureSeriesVenue	 *venue,
	gint64				  seen_at,
	GError				**error
);

/**
 * venture_series_store_upsert_instrument:
 * @self: the writer handle
 * @instrument: the instrument
 * @seen_at: when the source reported it
 * @out_created: (out) (optional): whether it was new
 * @out_refused: (out) (optional): whether it was new and refused by the cap
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the instrument or updates the members @instrument sets. A new
 * instrument past the size cap is refused without an error.
 *
 * Returns: %TRUE on success, including a refusal by the cap
 */
gboolean
venture_series_store_upsert_instrument(
	VentureSeriesStore		 *self,
	const VentureSeriesInstrument	 *instrument,
	gint64				  seen_at,
	gboolean			 *out_created,
	gboolean			 *out_refused,
	GError				**error
);

/**
 * venture_series_store_set_meta:
 * @self: the writer handle
 * @key: a key
 * @value: (nullable): the value, or %NULL to remove it
 * @error: (out) (optional): return location for a #GError
 *
 * Stores a small piece of source-level state: a cursor, a quota.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_set_meta(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	const gchar		 *value,
	GError			**error
);

/**
 * venture_series_store_get_meta:
 * @self: a #VentureSeriesStore
 * @key: a key
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the value, or %NULL (without an
 *   error) when there is none
 */
gchar *
venture_series_store_get_meta(
	VentureSeriesStore	 *self,
	const gchar		 *key,
	GError			**error
);

/**
 * venture_series_store_set_venue_cursor:
 * @self: the writer handle
 * @venue_key: the venue
 * @cursor: (nullable): where its next fetch resumes, or %NULL
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success; %VENTURE_ERROR_NOT_FOUND for an unknown venue
 */
gboolean
venture_series_store_set_venue_cursor(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *cursor,
	GError			**error
);

/**
 * venture_series_store_get_venue_cursor:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the cursor, or %NULL without an
 *   error when there is none
 */
gchar *
venture_series_store_get_venue_cursor(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	GError			**error
);

/* --- Snapshots ------------------------------------------------------------------ */

/**
 * VentureSeriesSnapshot:
 *
 * A snapshot being assembled in memory. Plain data until committed: adding
 * a listing allocates nothing per row beyond the arrays it grows.
 */
typedef struct _VentureSeriesSnapshot VentureSeriesSnapshot;

/**
 * venture_series_listing_id_from_string:
 * @id: a listing id the source gives as text
 *
 * A stable 64-bit id for a textual one (FNV-1a), never zero, so sources
 * whose ids are strings still get the sale estimate. A collision costs
 * one listing's estimate, never a crash.
 *
 * Returns: the id
 */
guint64
venture_series_listing_id_from_string(const gchar *id);

/**
 * venture_series_store_begin_snapshot:
 * @self: the writer handle
 * @venue_key: the venue the snapshot is of
 * @currency: (nullable): the currency of its prices; %NULL for the venue's
 * @taken_at: when the source took it (its Last-Modified)
 * @fetched_at: when it was fetched
 * @complete: whether it lists everything at the venue, so that an
 *   instrument missing from it is out of stock there
 * @error: (out) (optional): return location for a #GError
 *
 * Starts assembling a snapshot. Nothing is written until
 * venture_series_store_commit_snapshot().
 *
 * Returns: (transfer full) (nullable): the snapshot, or %NULL on error
 */
VentureSeriesSnapshot *
venture_series_store_begin_snapshot(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *currency,
	gint64			  taken_at,
	gint64			  fetched_at,
	gboolean		  complete,
	GError			**error
);

/**
 * venture_series_snapshot_add_listing:
 * @snapshot: a snapshot
 * @listing: the listing
 * @error: (out) (optional): return location for a #GError
 *
 * Adds one listing. A malformed one, or one whose quantity would take its
 * instrument's total past the 64-bit range, is refused with
 * %VENTURE_ERROR_INVALID_ARGUMENT and leaves the snapshot as it was, so a
 * caller can count it and carry on. An instrument given stats in the same
 * snapshot is refused the same way.
 *
 * Returns: %TRUE when the listing was added
 */
gboolean
venture_series_snapshot_add_listing(
	VentureSeriesSnapshot		 *snapshot,
	const VentureSeriesListing	 *listing,
	GError				**error
);

/**
 * venture_series_snapshot_add_stats:
 * @snapshot: a snapshot
 * @stats: the figures
 * @error: (out) (optional): return location for a #GError
 *
 * Adds precomputed figures for an instrument. A second set for the same
 * instrument replaces the first; an instrument given listings in the same
 * snapshot is refused with %VENTURE_ERROR_INVALID_ARGUMENT.
 *
 * Returns: %TRUE when the figures were added
 */
gboolean
venture_series_snapshot_add_stats(
	VentureSeriesSnapshot		 *snapshot,
	const VentureSeriesStats	 *stats,
	GError				**error
);

/**
 * venture_series_snapshot_free:
 * @snapshot: (transfer full) (nullable): a snapshot never committed
 *
 * Abandons a snapshot.
 */
void
venture_series_snapshot_free(VentureSeriesSnapshot *snapshot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureSeriesSnapshot, venture_series_snapshot_free)

/**
 * venture_series_store_commit_snapshot:
 * @self: the writer handle the snapshot was begun on
 * @snapshot: (transfer full): the snapshot; consumed whatever happens
 * @out: (out caller-allocates) (optional): what was written
 * @error: (out) (optional): return location for a #GError
 *
 * Applies a snapshot in one transaction: computes every instrument's
 * figures, upserts `current`, folds the hour into `hourly` and the day
 * into `daily`, estimates sales against the venue's previous listings and
 * learns its update interval.
 *
 * Applying the same venue and @taken_at twice is a no-op reported as
 * @out's duplicate. A snapshot older than the venue's newest is late: its
 * hourly and daily history is written, and nothing a newer snapshot
 * already decided (current, the listing set, the interval) is touched.
 *
 * Returns: %TRUE on success; on error nothing was written
 */
gboolean
venture_series_store_commit_snapshot(
	VentureSeriesStore		 *self,
	VentureSeriesSnapshot		 *snapshot,
	VentureSeriesCommitResult	 *out,
	GError				**error
);

/**
 * venture_series_store_add_quotes:
 * @self: the writer handle
 * @quotes: (array length=n_quotes): the quotes
 * @n_quotes: how many
 * @out: (out caller-allocates) (optional): what was written
 * @error: (out) (optional): return location for a #GError
 *
 * Applies quotes in one transaction: each becomes the latest for its
 * venue, instrument and side unless a newer one is stored, and is kept in
 * the quote history. The same quote twice is written once. Unknown
 * venues and instruments are created (instruments subject to the cap).
 *
 * Returns: %TRUE on success; on error nothing was written
 */
gboolean
venture_series_store_add_quotes(
	VentureSeriesStore		 *self,
	const VentureSeriesQuote	 *quotes,
	gsize				  n_quotes,
	VentureSeriesCommitResult	 *out,
	GError				**error
);

/**
 * venture_series_store_add_entries:
 * @self: the writer handle
 * @entries: (array length=n_entries): the entries
 * @n_entries: how many
 * @fetched_at: when they were fetched
 * @out_new: (out) (optional): how many were new
 * @error: (out) (optional): return location for a #GError
 *
 * Stores entries, matched on their key: a known key is updated, never
 * duplicated.
 *
 * Returns: %TRUE on success; on error nothing was written
 */
gboolean
venture_series_store_add_entries(
	VentureSeriesStore		 *self,
	const VentureSeriesEntry	 *entries,
	gsize				  n_entries,
	gint64				  fetched_at,
	gint64				 *out_new,
	GError				**error
);

/* --- Derived figures and retention --------------------------------------------- */

/**
 * venture_series_store_recompute_region:
 * @self: the writer handle
 * @group_key: (nullable): one group, or %NULL for every group
 * @now: the time to stamp the figures with
 * @deal_min_value: the median below which an instrument gets no deal
 *   price, or %VENTURE_SERIES_NONE
 * @deal_min_currency: (nullable): the currency of @deal_min_value; rows in
 *   other currencies get no bound
 * @out: (out caller-allocates) (optional): what was written
 * @error: (out) (optional): return location for a #GError
 *
 * Recomputes every group's figures per instrument and currency (see
 * #VentureSeriesRegion) and writes the median, the deal price and each
 * venue's percent of the median onto `current`, where browsing sorts by
 * them without computing anything. One transaction.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_recompute_region(
	VentureSeriesStore		 *self,
	const gchar			 *group_key,
	gint64				  now,
	gint64				  deal_min_value,
	const gchar			 *deal_min_currency,
	VentureSeriesRegionResult	 *out,
	GError				**error
);

/**
 * venture_series_store_purge:
 * @self: the writer handle
 * @now: the time to count back from
 * @hourly_days: days of hourly history (and quote history and snapshot
 *   log) to keep, today included; 0 keeps it forever
 * @daily_days: days of daily history (and entries) to keep; 0 keeps it
 *   forever
 * @out: (out caller-allocates) (optional): what was deleted
 * @error: (out) (optional): return location for a #GError
 *
 * Deletes history past its retention, for good, and hands the space back
 * to the file system.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_purge(
	VentureSeriesStore		 *self,
	gint64				  now,
	guint				  hourly_days,
	guint				  daily_days,
	VentureSeriesPurgeResult	 *out,
	GError				**error
);

/* --- Reading ------------------------------------------------------------------ */

/**
 * venture_series_sort_from_string:
 * @name: a sort's name: min_price, market_value, quantity, listings,
 *   pct_vs_region, deal_price, region_median, name, updated, venue,
 *   sale_rate or sold_per_day
 * @out: (out): the sort
 *
 * Returns: %FALSE for any other name
 */
gboolean
venture_series_sort_from_string(
	const gchar		*name,
	VentureSeriesSort	*out
);

/**
 * venture_series_sort_to_string:
 * @sort: a sort
 *
 * Returns: (transfer none): its name
 */
const gchar *
venture_series_sort_to_string(VentureSeriesSort sort);

/**
 * venture_series_store_get_current:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @instrument_key: the instrument
 * @out: (out) (transfer full) (nullable): the row, or %NULL when there is
 *   none
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success, including when there is no row
 */
gboolean
venture_series_store_get_current(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	VentureSeriesRow	**out,
	GError			**error
);

/**
 * venture_series_store_list_current:
 * @self: a #VentureSeriesStore
 * @filter: (nullable): what to list; %NULL for the defaults
 * @error: (out) (optional): return location for a #GError
 *
 * Lists current rows. Sorting is on precomputed, indexed columns, with
 * unknown values last whichever way the sort runs.
 *
 * Returns: (transfer full) (element-type VentureSeriesRow) (nullable): the
 *   rows, or %NULL on error
 */
GPtrArray *
venture_series_store_list_current(
	VentureSeriesStore		 *self,
	const VentureSeriesFilter	 *filter,
	GError				**error
);

/**
 * venture_series_store_count_current:
 * @self: a #VentureSeriesStore
 * @filter: (nullable): what to count; paging is ignored
 * @out_count: (out): how many rows match
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_count_current(
	VentureSeriesStore		 *self,
	const VentureSeriesFilter	 *filter,
	gint64				 *out_count,
	GError				**error
);

/**
 * venture_series_store_other_venues:
 * @self: a #VentureSeriesStore
 * @instrument_key: the instrument
 * @group_key: (nullable): only venues in this group
 * @error: (out) (optional): return location for a #GError
 *
 * Every venue's current row for one instrument, cheapest first, in stock
 * or not: Undermine Exchange's "other realms" table.
 *
 * Returns: (transfer full) (element-type VentureSeriesRow) (nullable): the
 *   rows, or %NULL on error
 */
GPtrArray *
venture_series_store_other_venues(
	VentureSeriesStore	 *self,
	const gchar		 *instrument_key,
	const gchar		 *group_key,
	GError			**error
);

/**
 * venture_series_store_hourly:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @instrument_key: the instrument
 * @since: the earliest time wanted
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type VentureSeriesPoint) (nullable):
 *   the hourly points from @since on, oldest first; %NULL on error
 */
GArray *
venture_series_store_hourly(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  since,
	GError			**error
);

/**
 * venture_series_store_daily:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @instrument_key: the instrument
 * @since: the earliest time wanted (its day is included)
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type VentureSeriesDay) (nullable): the
 *   days from @since on, oldest first; %NULL on error
 */
GArray *
venture_series_store_daily(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  since,
	GError			**error
);

/**
 * venture_series_store_heat:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @instrument_key: the instrument
 * @since: the earliest time wanted
 * @zone_offset: seconds east of UTC of the zone to read the hours in
 * @out: (out caller-allocates): the matrix of mean minimum prices
 * @out_currency: (out caller-allocates) (array fixed-size=16): the
 *   currency of the matrix, "" when it is empty
 * @error: (out) (optional): return location for a #GError
 *
 * The weekday by hour heat map of the hourly minimum price. Only points
 * in the newest point's currency count.
 *
 * Returns: %TRUE on success
 */
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
);

/**
 * venture_series_store_bulk_cost:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @instrument_key: the instrument
 * @units: units to buy, at least one
 * @out: (out caller-allocates): the cost
 * @out_currency: (out caller-allocates) (array fixed-size=16): its
 *   currency, "" when nothing is offered
 * @error: (out) (optional): return location for a #GError
 *
 * Prices buying @units from the venue's current book, cheapest first,
 * over its %VENTURE_SERIES_MAX_TIERS cheapest prices.
 *
 * Returns: %TRUE on success, including a book that runs out
 */
gboolean
venture_series_store_bulk_cost(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	gint64			  units,
	VentureSeriesBulkCost	 *out,
	gchar			  out_currency[VENTURE_MONEY_CURRENCY_LEN],
	GError			**error
);

/**
 * venture_series_store_get_tiers:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @instrument_key: the instrument
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type VentureSeriesTier) (nullable): the
 *   venue's cheapest tiers, cheapest first (empty when nothing is
 *   offered); %NULL on error
 */
GArray *
venture_series_store_get_tiers(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *instrument_key,
	GError			**error
);

/**
 * venture_series_store_get_region:
 * @self: a #VentureSeriesStore
 * @group_key: the group ("" for venues with none)
 * @instrument_key: the instrument
 * @currency: (nullable): the currency, or %NULL for whichever the group
 *   has the most venues offering in
 * @out: (out caller-allocates): the region row; @found says whether there
 *   is one
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success, including when there is no row
 */
gboolean
venture_series_store_get_region(
	VentureSeriesStore	 *self,
	const gchar		 *group_key,
	const gchar		 *instrument_key,
	const gchar		 *currency,
	VentureSeriesRegion	 *out,
	GError			**error
);

/**
 * venture_series_store_venue_index:
 * @self: a #VentureSeriesStore
 * @group_key: (nullable): only venues in this group
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type VentureSeriesVenueStats)
 *   (nullable): one entry per venue, by key; %NULL on error
 */
GPtrArray *
venture_series_store_venue_index(
	VentureSeriesStore	 *self,
	const gchar		 *group_key,
	GError			**error
);

/**
 * venture_series_store_reference:
 * @self: a #VentureSeriesStore
 * @venue_key: (nullable): the venue, or %NULL to use @group_key
 * @group_key: (nullable): the group, used when @venue_key is %NULL
 * @instrument_key: the instrument
 * @now: the time "today" is the day of
 * @out: (out caller-allocates): the figures; every one
 *   %VENTURE_SERIES_NONE or NAN when there is no history
 * @error: (out) (optional): return location for a #GError
 *
 * Computes the reference prices from the daily history. Only days in the
 * newest day's currency count.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_store_reference(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	const gchar		 *group_key,
	const gchar		 *instrument_key,
	gint64			  now,
	VentureSeriesReference	 *out,
	GError			**error
);

/**
 * venture_series_store_get_venue_state:
 * @self: a #VentureSeriesStore
 * @venue_key: the venue
 * @out: (out caller-allocates): the state
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success, including an unknown venue (@found FALSE)
 */
gboolean
venture_series_store_get_venue_state(
	VentureSeriesStore	 *self,
	const gchar		 *venue_key,
	VentureSeriesVenueState	 *out,
	GError			**error
);

/**
 * venture_series_store_list_venues:
 * @self: a #VentureSeriesStore
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type VentureSeriesVenueRow) (nullable):
 *   every venue, by key; %NULL on error
 */
GPtrArray *
venture_series_store_list_venues(
	VentureSeriesStore	 *self,
	GError			**error
);

/**
 * venture_series_store_get_instrument:
 * @self: a #VentureSeriesStore
 * @instrument_key: the instrument
 * @out: (out) (transfer full) (nullable): the row, or %NULL when unknown
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success, including when there is no row
 */
gboolean
venture_series_store_get_instrument(
	VentureSeriesStore		 *self,
	const gchar			 *instrument_key,
	VentureSeriesInstrumentRow	**out,
	GError				**error
);

/**
 * venture_series_store_list_quotes:
 * @self: a #VentureSeriesStore
 * @instrument_key: (nullable): only this instrument
 * @parent_key: (nullable): only children of this instrument (an event's
 *   outcomes)
 * @error: (out) (optional): return location for a #GError
 *
 * The latest quotes, by instrument, venue and side.
 *
 * Returns: (transfer full) (element-type VentureSeriesQuoteRow) (nullable):
 *   the quotes; %NULL on error
 */
GPtrArray *
venture_series_store_list_quotes(
	VentureSeriesStore	 *self,
	const gchar		 *instrument_key,
	const gchar		 *parent_key,
	GError			**error
);

/**
 * venture_series_store_list_quoted_parents:
 * @self: a #VentureSeriesStore
 * @since: only quotes taken at or after this Unix time
 * @count: keys to return, 0 for %VENTURE_SERIES_DEFAULT_PAGE, at most
 *   %VENTURE_SERIES_MAX_PAGE
 * @error: (out) (optional): return location for a #GError
 *
 * The parents (events) of the instruments with quotes, the most recently
 * quoted first: what a scan for surebets walks, one event at a time with
 * venture_series_store_list_quotes().
 *
 * Returns: (transfer full) (element-type utf8) (nullable): the keys
 */
GPtrArray *
venture_series_store_list_quoted_parents(
	VentureSeriesStore	 *self,
	gint64			  since,
	guint			  count,
	GError			**error
);

/**
 * venture_series_store_list_quoted_instruments:
 * @self: a #VentureSeriesStore
 * @side: only instruments quoted on this side (e.g. lay)
 * @since: only quotes taken at or after this Unix time
 * @count: keys to return, 0 for %VENTURE_SERIES_DEFAULT_PAGE, at most
 *   %VENTURE_SERIES_MAX_PAGE
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type utf8) (nullable): the keys, the
 *   most recently quoted first
 */
GPtrArray *
venture_series_store_list_quoted_instruments(
	VentureSeriesStore	 *self,
	VentureSeriesQuoteSide	  side,
	gint64			  since,
	guint			  count,
	GError			**error
);

/**
 * venture_series_store_list_entries:
 * @self: a #VentureSeriesStore
 * @since: the earliest publication (or fetch) time wanted
 * @count: entries to return, 0 for %VENTURE_SERIES_DEFAULT_PAGE, at most
 *   %VENTURE_SERIES_MAX_PAGE
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type VentureSeriesEntryRow) (nullable):
 *   the newest entries first; %NULL on error
 */
GPtrArray *
venture_series_store_list_entries(
	VentureSeriesStore	 *self,
	gint64			  since,
	guint			  count,
	GError			**error
);

/**
 * venture_series_store_list_new_entries:
 * @self: a #VentureSeriesStore
 * @fetched_since: the earliest first arrival wanted
 * @count: entries to return, 0 for %VENTURE_SERIES_DEFAULT_PAGE, at most
 *   %VENTURE_SERIES_MAX_PAGE
 * @error: (out) (optional): return location for a #GError
 *
 * The entries that first reached the store at or after @fetched_since,
 * whatever their publication time: what a run brought, as opposed to what
 * was published lately. An entry fetched again keeps its first arrival, so
 * it is not new twice.
 *
 * Returns: (transfer full) (element-type VentureSeriesEntryRow) (nullable):
 *   the earliest arrivals first; %NULL on error
 */
GPtrArray *
venture_series_store_list_new_entries(
	VentureSeriesStore	 *self,
	gint64			  fetched_since,
	guint			  count,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_SERIES_STORE_H */
