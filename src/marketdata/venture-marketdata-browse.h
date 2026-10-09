/*
 * venture-marketdata-browse.h - What the market pages, reports and widgets show
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One implementation per question, answering in plain JSON: the browse
 * catalogue, an instrument's page, the deals, the venue index, a
 * watchlist, the alerts overview and the health of the data sources. The
 * pages under /market render these, their /api/v1/market twins return
 * them verbatim, and the reports and dashboard widgets read the same
 * answers, so a figure cannot differ between doors.
 *
 * Everything is read on the main thread through read handles on the
 * sources' stores, and only through the store's precomputed, indexed
 * columns: the server is single-threaded, and a sort or a filter computed
 * over a million rows per request would stop every other request while it
 * ran. Every source read is the organization's own; another
 * organization's source is NOT_FOUND, never FORBIDDEN, so its existence is
 * not told.
 *
 * With feeds off, or in a build without SQLite, every answer is still an
 * answer: "available" is false, the notes say why, and the lists are
 * empty. The records (watchlists, alert rules and hits) are always read.
 *
 * Series figures (hourly and daily points, tiers, heat cells) are integer
 * minor units in the currency they carry; headline prices are money
 * objects (amount, currency, exponent, formatted). Ratios are numbers.
 * Times are ISO 8601 in UTC.
 */

#ifndef VENTURE_MARKETDATA_BROWSE_H
#define VENTURE_MARKETDATA_BROWSE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_MARKETDATA_BROWSE_MAX_PAGE:
 *
 * The most rows one browse page shows.
 */
#define VENTURE_MARKETDATA_BROWSE_MAX_PAGE (200)

/**
 * VENTURE_MARKETDATA_DEALS_MAX:
 *
 * The most deals one answer lists.
 */
#define VENTURE_MARKETDATA_DEALS_MAX (500)

/**
 * VENTURE_MARKETDATA_DEALS_HORIZON_DAYS:
 *
 * The days a deal's realisable profit counts expected sales over unless
 * asked otherwise: a week, about how long a flipper holds stock before
 * calling it stuck, and half the fourteen days the sale estimate is taken
 * over.
 */
#define VENTURE_MARKETDATA_DEALS_HORIZON_DAYS (7.0)

/**
 * VENTURE_MARKETDATA_DEALS_MAX_HORIZON_DAYS:
 *
 * The longest horizon a deal's realisable profit may be asked over: past
 * the sale estimate's own fourteen days the rate is an extrapolation of
 * an extrapolation, and a quarter is already generous.
 */
#define VENTURE_MARKETDATA_DEALS_MAX_HORIZON_DAYS (90.0)

/**
 * VENTURE_MARKETDATA_DEALS_CUT_PCT:
 *
 * The marketplace's cut of a sale, in percent, that Deals takes from the
 * sell side unless asked otherwise -- an auction house's 5% -- and the one
 * a deal alert reckons its profit with.
 */
#define VENTURE_MARKETDATA_DEALS_CUT_PCT (5.0)

/**
 * VENTURE_MARKETDATA_BULK_MAX_UNITS:
 *
 * The most units the bulk calculator prices in one question.
 */
#define VENTURE_MARKETDATA_BULK_MAX_UNITS (1000000000)

/**
 * VentureMarketdataBrowseQuery:
 * @organization_id: whose sources
 * @data_source_id: the source to browse, 0 for the organization's first
 *   by name
 * @search: (nullable): instruments whose name or key contains this
 * @category: (nullable): this category path and the ones beneath it
 * @venue: (nullable): only this venue's rows
 * @group_key: (nullable): only venues in this group
 * @venue_group: (nullable): only the venues of this venue group: "characters"
 *   or a venue group record's id (see venture_marketdata_venue_group_*);
 *   ignored when @venue is given
 * @sort: (nullable): a sort name (see venture_marketdata_browse_sorts());
 *   %NULL for min_price. Anything else is refused.
 * @descending: largest first
 * @in_stock_only: leave out rows with nothing offered
 * @page: the page, from 1; 0 means 1
 * @per_page: rows a page, 0 for 50, at most
 *   %VENTURE_MARKETDATA_BROWSE_MAX_PAGE
 * @now: the time "now" is, for each row's last seven days; 0 for the
 *   wall clock
 *
 * What the browse page asks.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	const gchar	*search;
	const gchar	*category;
	const gchar	*venue;
	const gchar	*group_key;
	const gchar	*venue_group;
	const gchar	*sort;
	gboolean	 descending;
	gboolean	 in_stock_only;
	guint		 page;
	guint		 per_page;
	gint64		 now;
} VentureMarketdataBrowseQuery;

/**
 * venture_marketdata_browse_query_init:
 * @query: (out caller-allocates): a query
 *
 * Everything, cheapest first, the first page.
 */
void
venture_marketdata_browse_query_init(VentureMarketdataBrowseQuery *query);

/**
 * venture_marketdata_browse_sorts:
 *
 * The sorts browse offers: precomputed, indexed columns of the store
 * only.
 *
 * Returns: (transfer none) (array zero-terminated=1): their names
 */
const gchar *const *
venture_marketdata_browse_sorts(void);

/**
 * venture_marketdata_browse:
 * @context: a #VentureContext
 * @query: what to list
 * @error: (out) (optional): return location for a #GError
 *
 * One page of the catalogue: {available, notes, sources, data_source_id,
 * venues, groups, query, sorts, page, per_page, total, pages, rows}. Each
 * row carries its last seven days (see venture_marketdata_deals()):
 * trend_7d, median_7d, median_7d_hours and vs_median_7d_pct, and the
 * watchlists its instrument is on (venture_marketdata_watchlist_memberships());
 * watchlist_choices is venture_marketdata_list_choices() for watchlists.
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL on error
 *   (NOT_FOUND for a source that is not the organization's,
 *   INVALID_ARGUMENT for an unknown sort or a page too large)
 */
JsonNode *
venture_marketdata_browse(
	VentureContext				 *context,
	const VentureMarketdataBrowseQuery	 *query,
	GError					**error
);

/**
 * VentureMarketdataInstrumentQuery:
 * @organization_id: whose source
 * @data_source_id: the source, which must be the organization's
 * @key: the instrument's key in the store
 * @venue: (nullable): the venue to chart; %NULL for the cheapest venue
 *   with it in stock, else the first
 * @units: units for the bulk calculator; 0 for none
 * @now: the time "now" is, 0 for the wall clock
 * @venue_group: (nullable): only these venues in the venues table, as on
 *   browse; the charted venue is still chosen from every venue
 * @venues_descending: the venues table dearest first rather than cheapest
 * @range: (nullable): how far back the price history reaches: one of
 *   venture_marketdata_history_ranges(); %NULL for
 *   %VENTURE_MARKETDATA_HISTORY_DEFAULT_RANGE. Anything else is refused.
 * @compare: (nullable): a second line on the price history: "region" for
 *   the median of the charted venue's group, or another venue's key;
 *   %NULL for none
 *
 * What an instrument page asks; its price history alone is the same
 * question (venture_marketdata_history()).
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	const gchar	*key;
	const gchar	*venue;
	gint64		 units;
	gint64		 now;
	const gchar	*venue_group;
	gboolean	 venues_descending;
	const gchar	*range;
	const gchar	*compare;
} VentureMarketdataInstrumentQuery;

/**
 * VENTURE_MARKETDATA_HISTORY_DEFAULT_RANGE:
 *
 * The price history's range when none is asked for: the hourly window as
 * shipped, which is what the instrument page charted before it had a
 * choice.
 */
#define VENTURE_MARKETDATA_HISTORY_DEFAULT_RANGE "14d"

/**
 * venture_marketdata_history_ranges:
 *
 * The ranges a price history offers: 24h, 7d, 14d, 90d and all. A range
 * within series.hourly_days is read hour by hour, a longer one day by
 * day.
 *
 * Returns: (transfer none) (array zero-terminated=1): their names
 */
const gchar *const *
venture_marketdata_history_ranges(void);

/**
 * venture_marketdata_history:
 * @context: a #VentureContext
 * @query: the instrument, venue, range and comparison; the bulk and
 *   venue-table members are ignored
 * @error: (out) (optional): return location for a #GError
 *
 * An instrument's price history at one venue, the chart on its page:
 * {available, notes, data_source_id, source_name, key, name, venue,
 * venue_name, group_key, currency, stale_after_seconds, history}. history
 * is {range, ranges, resolution ("hour" or "day"), since, until,
 * currency, truncated, points: [{at, min_price, market_value, quantity,
 * listings}], compare}: one point per hour or day, a gap (quantity null)
 * where nothing was stored, prices in minor units of currency. compare is
 * null or {kind ("venue" or "region"), venue, venue_name, group_key,
 * venues, points: [{at, min_price, quantity, venues}]} on the same slots.
 * The venue is chosen as the instrument page chooses it.
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 *   (NOT_FOUND for an unknown instrument or a venue that never listed it,
 *   INVALID_ARGUMENT for an unknown range)
 */
JsonNode *
venture_marketdata_history(
	VentureContext				 *context,
	const VentureMarketdataInstrumentQuery	 *query,
	GError					**error
);

/**
 * venture_marketdata_instrument:
 * @context: a #VentureContext
 * @query: the instrument
 * @error: (out) (optional): return location for a #GError
 *
 * An instrument's page: {available, notes, data_source_id, source_name,
 * instrument, venue, venue_name, group_key, base, reference, venues (every
 * venue's current row, cheapest first), hourly (14 days), heat (the last
 * seven days by weekday and hour in the configured zone, lowest price and
 * quantity), daily (60 days), history (the price history over @range,
 * as venture_marketdata_history() answers it), bulk, tiers, watchlists
 * (every list, for the picker), listed_on (the lists it is on: [{id,
 * name, entry_id}]),
 * record_id, stale_after_seconds}. base and each venue carry age_seconds and stale
 * beside taken_at, as a deal's sides do. Where another source of the
 * organization has the item (as on Deals), ref: {data_source_id,
 * source_name, region_venue, region_market, region_historical,
 * region_sale_rate, region_sold_per_day, region_taken_at,
 * region_age_seconds, region_stale, buy_vs_region_pct, buy_realm_market,
 * buy_vs_realm_pct}, "buy" being the charted venue's lowest price; absent
 * when no source has anything.
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 *   (NOT_FOUND for another organization's source, a store that has
 *   stored nothing, or a key it does not know)
 */
JsonNode *
venture_marketdata_instrument(
	VentureContext				 *context,
	const VentureMarketdataInstrumentQuery	 *query,
	GError					**error
);

/**
 * venture_marketdata_picker_choices:
 * @context: a #VentureContext
 * @organization_id: whose groups and sources
 * @data_source_id: the source whose categories to list, 0 for the
 *   organization's first by name
 *
 * What the market pages' pickers offer before any question is asked:
 * {venue_groups: [{value, name}], categories: [{path, instruments}]}.
 * The venue groups are "characters" when the organization has a push
 * source, then its saved groups by name; the categories are the source's
 * store's (see venture_series_store_list_categories()).
 *
 * Returns: (transfer full): the choices; empty lists without SQLite
 */
JsonNode *
venture_marketdata_picker_choices(
	VentureContext	*context,
	gint64		 organization_id,
	gint64		 data_source_id
);

/**
 * VENTURE_MARKETDATA_FIND_MAX_ROWS:
 *
 * The most venue rows one item search reads before it says to narrow the
 * search: every venue an instrument is in stock at is a row.
 */
#define VENTURE_MARKETDATA_FIND_MAX_ROWS (5000)

/**
 * VentureMarketdataFindQuery:
 * @organization_id: whose sources
 * @data_source_id: the source, 0 for the organization's first by name
 * @search: (nullable): the name (or key) to look for; required unless
 *   @category is given
 * @venue_group: (nullable): only these venues, as on browse
 * @category: (nullable): only this category path and the ones beneath it
 *
 * What the item finder asks: every instrument whose name contains
 * @search, each with where it is cheapest and dearest.
 */
typedef struct
{
	gint64		 organization_id;
	gint64		 data_source_id;
	const gchar	*search;
	const gchar	*venue_group;
	const gchar	*category;
} VentureMarketdataFindQuery;

/**
 * venture_marketdata_find:
 * @context: a #VentureContext
 * @query: what to look for
 * @error: (out) (optional): return location for a #GError
 *
 * {available, notes, sources, data_source_id, venue_groups,
 * venue_group_name, query, truncated, items}: one entry per instrument in
 * stock somewhere, by name then key, with its variant label, item level,
 * how many venues offer it, and its cheapest and dearest venue rows
 * (lowest listing at each).
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 *   (INVALID_ARGUMENT for an empty search)
 */
JsonNode *
venture_marketdata_find(
	VentureContext				 *context,
	const VentureMarketdataFindQuery	 *query,
	GError					**error
);

/**
 * VENTURE_MARKETDATA_STALE_MINUTES_DEFAULT:
 *
 * series.stale_minutes when the configuration says nothing.
 */
#define VENTURE_MARKETDATA_STALE_MINUTES_DEFAULT (120)

/**
 * venture_marketdata_stale_seconds:
 * @context: a #VentureContext
 *
 * The age past which a price on the Trading pages is stale
 * (series.stale_minutes). A stale price is still shown -- it may be the
 * only one there is -- but marked, because a realm whose feed stopped
 * looks exactly like a cheap realm.
 *
 * Returns: seconds, at least 60
 */
gint64
venture_marketdata_stale_seconds(VentureContext *context);

/**
 * venture_marketdata_venue_group_choices:
 * @context: a #VentureContext
 * @organization_id: whose groups
 *
 * What a venue group picker offers, as Deals and Browse offer it:
 * [{value, name}] -- "characters" when a push source exists, then the
 * organization's saved groups by name. Empty with feeds off.
 *
 * Returns: (transfer full): a JSON array
 */
JsonNode *
venture_marketdata_venue_group_choices(
	VentureContext	*context,
	gint64		 organization_id
);

/**
 * venture_marketdata_realm_choices:
 * @context: a #VentureContext
 * @organization_id: whose sources
 * @venue_group: (nullable): "characters" or a venue group's id, to offer
 *   only its realms; %NULL or empty for every realm
 * @out_choices: (out) (transfer full): a JSON array of choices, one per
 *   connected realm, in label order, shaped as Deals' `venue_choices`
 *   ({name, value, group_key, members, venues: [{data_source_id,
 *   venue_key, taken_at, age_seconds, stale}]}) plus `region_wide`, true
 *   for a market every realm trades on (a commodity market)
 * @error: (out) (optional): return location for a #GError
 *
 * The realm picker Deals offers, for any page that buys or sells at a
 * chosen realm: every source's venues grouped into connected realms,
 * whatever each source keys them by. Empty with feeds off or without a
 * series store.
 *
 * Returns: %FALSE on error (a venue group that is not the organization's)
 */
gboolean
venture_marketdata_realm_choices(
	VentureContext	 *context,
	gint64		  organization_id,
	const gchar	 *venue_group,
	JsonNode	**out_choices,
	GError		**error
);

/**
 * venture_marketdata_realm_venue_keys:
 * @context: a #VentureContext
 * @organization_id: whose sources
 * @wanted: (nullable): a realm as a person names it: a choice's label,
 *   any member realm's name, a source's whole name for it or any source's
 *   key; %NULL or empty for none
 * @out_keys: (out) (transfer full) (array zero-terminated=1): every
 *   source's venue keys for that connected realm; %NULL when @wanted
 *   names none
 * @out_label: (out) (optional) (transfer full): the realm's label, as the
 *   picker spells it
 * @error: (out) (optional): return location for a #GError
 *
 * A picked realm as the venue keys a scan's `buy_venues` or
 * `sell_venues` take, read exactly as Deals reads its `venue`.
 *
 * Returns: %FALSE with NOT_FOUND when no source has the realm
 */
gboolean
venture_marketdata_realm_venue_keys(
	VentureContext	 *context,
	gint64		  organization_id,
	const gchar	 *wanted,
	gchar		***out_keys,
	gchar		**out_label,
	GError		**error
);

/**
 * venture_marketdata_venue_group_venue_keys:
 * @context: a #VentureContext
 * @organization_id: whose group and sources
 * @spec: (nullable): "characters" or a venue group record's id; empty or
 *   %NULL for no group
 * @out_keys: (out) (transfer full) (array zero-terminated=1): the venue
 *   keys the group stands for in every source of the organization, each
 *   connected realm whole (a group naming one realm of a connected realm
 *   takes every source's keys for it); an empty array when it matches
 *   nothing; %NULL when @spec asks for no group
 * @out_label: (out) (optional) (transfer full): the group's name
 * @error: (out) (optional): return location for a #GError
 *
 * A venue group as the venue keys a filter compares, as Deals reads it,
 * for callers that filter by key whatever the source (the arbitrage scan).
 *
 * Returns: %FALSE on error (INVALID_ARGUMENT for a malformed @spec,
 *   NOT_FOUND for another organization's group or none)
 */
gboolean
venture_marketdata_venue_group_venue_keys(
	VentureContext	 *context,
	gint64		  organization_id,
	const gchar	 *spec,
	gchar		***out_keys,
	gchar		**out_label,
	GError		**error
);

/**
 * VentureMarketdataDealsQuery:
 * @organization_id: whose sources
 * @data_source_id: one source, or 0 for every source of the organization
 * @venue: (nullable): buy only at this venue; with @venue_group, it must
 *   be one of the group's (else it is set aside, with a note)
 * @group_key: (nullable): only venues in this group
 * @venue_group: (nullable): only the venues of this venue group, as on
 *   browse: where to buy (unless @venue narrows it) and where to sell
 *   (unless @sell_venue does)
 * @search: (nullable): only instruments whose name or key contains this
 * @category: (nullable): this category path and beneath
 * @min_value: (nullable): only rows worth at least this (market value, or
 *   the minimum where there is none), compared in its own currency only
 * @max_pct: only rows at most this percent of the region median; NAN for
 *   no bound
 * @count: rows to return, 0 for 50, at most %VENTURE_MARKETDATA_DEALS_MAX
 * @cut_pct: the marketplace's cut of a sale, in percent, taken from the
 *   sell side when a deal's profit is reckoned (an auction house's is 5)
 * @sort: (nullable): "deal" (the default: cheapest against the region
 *   first), or a column: "name", "ilvl", "buy_at", "buy", "sell_at",
 *   "sell", "profit", "roi", "region", "rate" (the sell venue's sale rate),
 *   "sold" (the sell venue's units sold a day), "realisable" (the
 *   realisable profit) or "qty". A column sort reads a wide pool of deals and keeps the
 *   first @count; a row with no value for it (nowhere to sell, no item
 *   level) is last either way.
 * @dir: (nullable): "asc" or "desc"; %NULL for the column's natural
 *   order (most first for figures where more is better, A to Z for names)
 * @sell_venue: (nullable): sell only at this venue: a deal with no
 *   plausible price there is left out. With @venue_group it must be one
 *   of the group's, like @venue.
 * @now: the time "now" is, for each price's age; 0 for the wall clock
 * @min_sold_per_day: sell only where the store estimates at least this
 *   many units sold a day (current.sold_per_day, fourteen days); a venue
 *   with no estimate never qualifies. NAN for no bound
 * @horizon_days: the days a realisable profit counts sales over, above
 *   zero; 7 by default
 * @watchlists: (nullable): only instruments on these watchlists, ids
 *   separated by commas, their union; read before any store is, so a
 *   list narrows the deal index to its own keys
 *
 * What the deals page asks. A deal is an instrument in stock at a venue
 * whose lowest price is at or under its group's deal price: the median of
 * the group's minimums, or the 33rd percentile when 15 or more venues
 * offer it.
 */
typedef struct
{
	gint64			 organization_id;
	gint64			 data_source_id;
	const gchar		*venue;
	const gchar		*group_key;
	const gchar		*venue_group;
	const gchar		*search;
	const gchar		*category;
	const VentureMoney	*min_value;
	gdouble			 max_pct;
	guint			 count;
	gdouble			 cut_pct;
	const gchar		*sort;
	const gchar		*dir;
	const gchar		*sell_venue;
	gint64			 now;
	gdouble			 min_sold_per_day;
	gdouble			 horizon_days;
	const gchar		*watchlists;
} VentureMarketdataDealsQuery;

/**
 * venture_marketdata_deals_query_init:
 * @query: (out caller-allocates): a query
 *
 * Every deal of every source, best first, 50 of them.
 */
void
venture_marketdata_deals_query_init(VentureMarketdataDealsQuery *query);

/**
 * venture_marketdata_deals:
 * @context: a #VentureContext
 * @query: the filters
 * @error: (out) (optional): return location for a #GError
 *
 * The deals, cheapest against their region first: {available, notes,
 * sources, rows (each a browse row plus data_source_id, source_name,
 * discount and discount_pct, and -- where another venue has it in stock --
 * sell: the dearest such venue's row in the venue group, else in the buy
 * venue's group, with profit (its lowest price less the cut, less the buy
 * price) and roi_pct), truncated, cut_pct, totals: {investment, sale,
 * profit} over the rows with a sell side, one unit each, when they share a
 * currency}.
 *
 * venue_choices has one entry per connected realm the sources describe
 * between them -- {name, value, group_key, members, venues:
 * [{data_source_id, venue_key, taken_at, age_seconds, stale}]} -- and
 * @venue and @sell_venue take its name, any member realm's name, or a
 * source's name or key for it. A row says its connected realm in "realm"
 * (the sell object too).
 *
 * Every price says how old it is: a row carries buy_taken_at,
 * buy_age_seconds and buy_stale for its own price and, with a sell side,
 * sell_taken_at, sell_age_seconds and sell_stale (the sell object carries
 * them unprefixed too); the root carries stale_after_seconds
 * (venture_marketdata_stale_seconds()).
 *
 * How fast it sells: with a sell side, a row carries sell_sold_per_day
 * and sell_sale_rate (the sell venue's current.sold_per_day and
 * sale_rate), expected_sales (units the sell venue is expected to sell
 * over horizon_days, rounded down; null with no estimate) and the
 * realisable profit -- realisable_profit, realisable_units,
 * realisable_cost and book_units (venture_series_math_realisable(): the
 * buy venue's tiers under the net sell price, no more units than
 * expected_sales). The root carries horizon_days, min_sold_per_day (null
 * for none) and totals.realisable_profit.
 *
 * Another source's view of the same item: where one of the
 * organization's sources with the same instrument namespace keeps the
 * region venue (venture_series_region_venue_key()) or the buy or sell
 * venue's connected realm -- what tsmctl pushes of TSM's AuctionDB -- a
 * row carries ref: {data_source_id, source_name, region_venue,
 * region_market, region_historical, region_sale_rate,
 * region_sold_per_day, region_taken_at, region_age_seconds, region_stale,
 * buy_vs_region_pct, sell_vs_region_pct, buy_realm_market,
 * buy_vs_realm_pct, sell_realm_market, sell_vs_realm_pct}, each figure
 * null where that source has none. A row it has nothing for carries no
 * ref at all: absent, never zero.
 *
 * Every row also says whether its price is a dip or the new normal, from
 * the buy venue's hourly lowest prices over the last seven days, read for
 * every row in one statement per source: trend_7d (42 four-hour blocks,
 * oldest first, each block's lowest or null), median_7d (the median of
 * the hourly lowest prices; null with fewer than 12 hours of them),
 * median_7d_hours (how many there were) and vs_median_7d_pct (the price
 * against that median, in percent: -20 is a fifth under it).
 *
 * Lists: every row carries `watchlists`, the lists its instrument is on
 * ([{id, name, entry_id}]); the root carries watchlist_choices ([{id,
 * name, entries}], for the picker) and, with @watchlists asked,
 * watchlist_filter ([{id, name}]).
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 *   (NOT_FOUND for a watchlist that is not the organization's)
 */
JsonNode *
venture_marketdata_deals(
	VentureContext				 *context,
	const VentureMarketdataDealsQuery	 *query,
	GError					**error
);

/**
 * venture_marketdata_venue_index:
 * @context: a #VentureContext
 * @organization_id: whose sources
 * @data_source_id: one source, or 0 for every source of the organization
 * @group_key: (nullable): only venues in this group
 * @now: the time "now" is, 0 for the wall clock
 * @error: (out) (optional): return location for a #GError
 *
 * FlippingPal's realm index and Saddlebag's upload timer, per venue:
 * {available, notes, sources, groups, venues: [{data_source_id,
 * source_name, venue_key, venue_name, group_key, instruments, cheaper,
 * equal, dearer, pct_cheaper, pct_equal, pct_dearer, avg_ratio, listings,
 * last_taken_at, last_fetched_at, interval_seconds, next_expected,
 * age_seconds, overdue}]}.
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 */
JsonNode *
venture_marketdata_venue_index(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  data_source_id,
	const gchar	 *group_key,
	gint64		  now,
	GError		**error
);

/**
 * venture_marketdata_watchlist_view:
 * @context: a #VentureContext
 * @organization_id: whose list
 * @watchlist_id: the list
 * @now: the time "now" is, 0 for the wall clock
 * @error: (out) (optional): return location for a #GError
 *
 * A watchlist priced now: {available, notes, watchlist, entries: [{id,
 * instrument_id, instrument_name, key, data_source_id, target_buy,
 * target_sell, best, venues, buy_delta, sell_delta, buy_now, sell_now,
 * trend, trend_currency}]}. Venues are those of the list's group, or
 * every venue when it has none.
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 *   (NOT_FOUND for a list that is not the organization's)
 */
JsonNode *
venture_marketdata_watchlist_view(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  watchlist_id,
	gint64		  now,
	GError		**error
);

/**
 * venture_marketdata_watchlists:
 * @context: a #VentureContext
 * @organization_id: whose lists
 * @error: (out) (optional): return location for a #GError
 *
 * Every list with how many entries it has: {watchlists: [{id, name,
 * group_key, entries}]}.
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 */
JsonNode *
venture_marketdata_watchlists(
	VentureContext	 *context,
	gint64		  organization_id,
	GError		**error
);

/**
 * VENTURE_MARKETDATA_LIST_NAME_MAX:
 *
 * The longest list name typed on a row: a list is named by a person in a
 * box beside a price, and a page of tags must stay a page.
 */
#define VENTURE_MARKETDATA_LIST_NAME_MAX (80)

/**
 * VentureMarketdataListKind:
 * @list_type: the list's record type: a name and nothing it needs
 * @list_label: what a list is called in a refusal: "watchlist"
 * @entry_type: the entry's record type, naming the list and one item
 * @list_field: the entry's reference to its list: "watchlist-id"
 * @item_field: the entry's reference to its item: "instrument-id"
 *
 * A named list of things -- instruments on a watchlist, recipes on a
 * recipe list -- as the "Add to list" control on a row sees it. Both
 * kinds are ordinary records; this is only which fields are which, so
 * one function puts a thing on either and a second press is a no-op on
 * both.
 */
typedef struct
{
	GType		 list_type;
	const gchar	*list_label;
	GType		 entry_type;
	const gchar	*list_field;
	const gchar	*item_field;
} VentureMarketdataListKind;

/**
 * venture_marketdata_list_put:
 * @context: a #VentureContext
 * @kind: which kind of list
 * @organization_id: whose list
 * @list_id: the list, or 0 to name it by @list_name
 * @list_name: (nullable): with no @list_id, the list of this name
 *   (ignoring case), made when there is none
 * @item_id: the item's record, which must exist (the entry's save checks
 *   that it is the organization's)
 * @actor: (nullable): who adds it
 * @out_entry: (out) (optional) (transfer full): the entry, new or found
 * @out_created_list: (out) (optional): whether the list was made here
 * @out_created_entry: (out) (optional): whether the entry was made here
 * @error: (out) (optional): return location for a #GError
 *
 * Finds or makes the list and adds the item unless the list holds it
 * already, in one transaction; every write an ordinary save.
 *
 * Returns: %TRUE on success (NOT_FOUND for another organization's list,
 *   INVALID_ARGUMENT for no list named or a name past
 *   %VENTURE_MARKETDATA_LIST_NAME_MAX characters)
 */
gboolean
venture_marketdata_list_put(
	VentureContext				 *context,
	const VentureMarketdataListKind		 *kind,
	gint64					  organization_id,
	gint64					  list_id,
	const gchar				 *list_name,
	gint64					  item_id,
	const VentureActor			 *actor,
	VentureEntity				**out_entry,
	gboolean				 *out_created_list,
	gboolean				 *out_created_entry,
	GError					**error
);

/**
 * venture_marketdata_list_take:
 * @context: a #VentureContext
 * @kind: which kind of list
 * @organization_id: whose list
 * @entry_id: the entry
 * @actor: (nullable): who removes it
 * @out_list_id: (out) (optional): the list it was on
 * @error: (out) (optional): return location for a #GError
 *
 * Takes an item off its list by deleting the entry. The item stays.
 *
 * Returns: %TRUE on success (NOT_FOUND for another organization's entry
 *   or none)
 */
gboolean
venture_marketdata_list_take(
	VentureContext			 *context,
	const VentureMarketdataListKind	 *kind,
	gint64				  organization_id,
	gint64				  entry_id,
	const VentureActor		 *actor,
	gint64				 *out_list_id,
	GError				**error
);

/**
 * venture_marketdata_list_memberships:
 * @context: a #VentureContext
 * @kind: which kind of list
 * @organization_id: whose lists
 * @item_ids: (element-type gint64): the items asked about
 *
 * Which lists hold each item: item id (a #gint64 key) to a #JsonArray of
 * {id, name, entry_id}, by name. An item on none is absent. Two reads
 * however many items are asked about.
 *
 * Returns: (transfer full): the memberships
 */
GHashTable *
venture_marketdata_list_memberships(
	VentureContext			*context,
	const VentureMarketdataListKind	*kind,
	gint64				 organization_id,
	GArray				*item_ids
);

/**
 * venture_marketdata_list_choices:
 * @context: a #VentureContext
 * @kind: which kind of list
 * @organization_id: whose lists
 *
 * The organization's lists for a picker, by name: [{id, name, entries}].
 *
 * Returns: (transfer full): the lists
 */
JsonArray *
venture_marketdata_list_choices(
	VentureContext			*context,
	const VentureMarketdataListKind	*kind,
	gint64				 organization_id
);

/**
 * venture_marketdata_list_ids:
 * @context: a #VentureContext
 * @kind: which kind of list
 * @organization_id: whose lists
 * @text: list ids separated by commas or spaces ("3,5")
 * @out_items: (out) (transfer full) (element-type gint64): the items on
 *   any of them, each once
 * @out_lists: (out) (transfer full): [{id, name}] of the lists, as asked
 * @error: (out) (optional): return location for a #GError
 *
 * The union of the lists @text names, as item ids: what a page narrows
 * to when asked for a list.
 *
 * Returns: %TRUE on success (INVALID_ARGUMENT for text that is not ids,
 *   NOT_FOUND for another organization's list or none)
 */
gboolean
venture_marketdata_list_ids(
	VentureContext			 *context,
	const VentureMarketdataListKind	 *kind,
	gint64				  organization_id,
	const gchar			 *text,
	GArray				**out_items,
	JsonArray			**out_lists,
	GError				**error
);

/**
 * venture_marketdata_watchlist_kind:
 *
 * Watchlists as a #VentureMarketdataListKind.
 *
 * Returns: (transfer none): the kind
 */
const VentureMarketdataListKind *
venture_marketdata_watchlist_kind(void);

/**
 * venture_marketdata_watchlist_add:
 * @context: a #VentureContext
 * @organization_id: whose list and instrument
 * @watchlist_id: the list, or 0 to name it by @watchlist_name
 * @watchlist_name: (nullable): with no @watchlist_id, the list of this
 *   name (ignoring case) -- made when there is none
 * @data_source_id: the source whose store has the instrument
 * @key: the instrument's key in that store
 * @actor: (nullable): who adds it
 * @out_entry: (out) (optional) (transfer full): the entry, new or found
 * @out_created_list: (out) (optional): whether the list was made here
 * @out_created_entry: (out) (optional): whether the entry was made here
 * @error: (out) (optional): return location for a #GError
 *
 * Puts an instrument on a watchlist in one step: promotes it (an
 * existing record is found, a deleted one restored), finds or makes the
 * list, and adds the entry unless the list already holds it -- so a
 * second press of "Add" is a success that writes nothing. Every write is
 * an ordinary save, under the validators and the audit log, in one
 * transaction.
 *
 * Returns: %TRUE on success (NOT_FOUND for another organization's list or
 *   source, INVALID_ARGUMENT for no list named)
 */
gboolean
venture_marketdata_watchlist_add(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  watchlist_id,
	const gchar		 *watchlist_name,
	gint64			  data_source_id,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureEntity		**out_entry,
	gboolean		 *out_created_list,
	gboolean		 *out_created_entry,
	GError			**error
);

/**
 * venture_marketdata_watchlist_memberships:
 * @context: a #VentureContext
 * @organization_id: whose lists
 * @rows: store rows as the pages answer them, each with data_source_id
 *   (or @data_source_id) and instrument_key
 * @data_source_id: the source of rows that carry none, or 0
 *
 * Says on each row which watchlists hold its instrument: `watchlists`,
 * [{id, name, entry_id}], by name -- an empty list for none. An
 * instrument record with no data source is the key's in every source.
 * Two indexed reads for the whole page, whatever its length.
 */
void
venture_marketdata_watchlist_memberships(
	VentureContext	*context,
	gint64		 organization_id,
	JsonArray	*rows,
	gint64		 data_source_id
);

/**
 * venture_marketdata_watchlist_keys:
 * @context: a #VentureContext
 * @organization_id: whose lists
 * @watchlists: the lists, ids separated by commas ("3,5")
 * @out_keys: (out) (transfer full): data source id (a #gint64 key, 0 for
 *   every source) to a #GPtrArray of instrument keys
 * @out_lists: (out) (transfer full): [{id, name}] of the lists, in order
 * @error: (out) (optional): return location for a #GError
 *
 * The instruments of a union of watchlists, as the keys a store filter
 * compares: what Deals narrows to when asked for a list.
 *
 * Returns: %TRUE on success (INVALID_ARGUMENT for text that is not ids,
 *   NOT_FOUND for another organization's list or none)
 */
gboolean
venture_marketdata_watchlist_keys(
	VentureContext	 *context,
	gint64		  organization_id,
	const gchar	 *watchlists,
	GHashTable	**out_keys,
	JsonArray	**out_lists,
	GError		**error
);

/**
 * venture_marketdata_alerts_overview:
 * @context: a #VentureContext
 * @organization_id: whose alerts
 * @count: recent hits to list, 0 for 50, at most 500
 * @error: (out) (optional): return location for a #GError
 *
 * The recent hits, newest first, and the rules: {hits: [{id, rule_id,
 * rule_name, kind, observed_at, message, data_source_id, venue_key,
 * instrument_key, url, observed, reference, observed_number,
 * reference_number}], rules: [{id, name, kind, enabled, last_hit_at}]}.
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 */
JsonNode *
venture_marketdata_alerts_overview(
	VentureContext	 *context,
	gint64		  organization_id,
	guint		  count,
	GError		**error
);

/**
 * venture_marketdata_source_health:
 * @context: a #VentureContext
 * @organization_id: whose sources
 * @now: the time "now" is, 0 for the wall clock
 * @error: (out) (optional): return location for a #GError
 *
 * Each data source's state: {available, notes, sources: [{id, name,
 * provider, enabled, schedule, last_run, next_check, in_flight, venues,
 * stale_venues, newest_snapshot}]}. A venue is stale when its newest
 * snapshot is older than twice its learned interval.
 *
 * Returns: (transfer full) (nullable): the answer; %NULL on error
 */
JsonNode *
venture_marketdata_source_health(
	VentureContext	 *context,
	gint64		  organization_id,
	gint64		  now,
	GError		**error
);

/**
 * venture_marketdata_instrument_path:
 * @data_source_id: the source
 * @key: the instrument's key
 * @venue: (nullable): a venue to open it at
 *
 * The instrument page's address, the key escaped as one path segment.
 *
 * Returns: (transfer full): the path
 */
gchar *
venture_marketdata_instrument_path(
	gint64		 data_source_id,
	const gchar	*key,
	const gchar	*venue
);

/**
 * venture_marketdata_parse_amount:
 * @text: an amount, e.g. "10.00 GOLD"
 * @error: (out) (optional): return location for a #GError
 *
 * Parses an amount that must name its currency. A bound compared in one
 * currency says nothing about another, so the install's default currency
 * is never assumed: "10" is refused with INVALID_ARGUMENT.
 *
 * Returns: (transfer full) (nullable): the amount, or %NULL on error
 */
VentureMoney *
venture_marketdata_parse_amount(
	const gchar	 *text,
	GError		**error
);

/* --- Source attribution ------------------------------------------------- */

/**
 * venture_marketdata_attribution_add_provider:
 * @context: the context
 * @provider: (nullable): a provider's registry name
 * @attributions: (element-type utf8): the distinct lines so far, owned
 *   strings; the provider's line is added unless it is already there
 *
 * Adds a provider's attribution line (see
 * venture_data_source_provider_get_attribution()), normalised. A provider
 * that declares none, or is not loaded, adds nothing. Main thread.
 */
void
venture_marketdata_attribution_add_provider(
	VentureContext	*context,
	const gchar	*provider,
	GPtrArray	*attributions
);

/**
 * venture_marketdata_attribution_add_source:
 * @context: the context
 * @organization_id: the organization the answer is for
 * @data_source_id: a data_source record whose data is shown
 * @attributions: (element-type utf8): the distinct lines so far
 *
 * Adds the line of the source's provider. A deleted source still counts
 * (its data may still be on the page); another organization's never
 * does.
 */
void
venture_marketdata_attribution_add_source(
	VentureContext	*context,
	gint64		 organization_id,
	gint64		 data_source_id,
	GPtrArray	*attributions
);

/**
 * venture_marketdata_attribution_collect:
 * @context: the context
 * @organization_id: the organization the answer is for
 * @node: (nullable): part of an answer: rows, hits, opportunities
 * @attributions: (element-type utf8): the distinct lines so far
 *
 * Adds the line of every source named by a `data_source_id` member
 * anywhere inside @node. Hand it the part of an answer that is data --
 * not a picker listing every source -- so only the sources actually
 * shown are named.
 */
void
venture_marketdata_attribution_collect(
	VentureContext	*context,
	gint64		 organization_id,
	JsonNode	*node,
	GPtrArray	*attributions
);

/**
 * venture_marketdata_attribution_set:
 * @answer: an answer object
 * @attributions: (nullable) (element-type utf8): the lines
 *
 * Writes the lines as the answer's `attribution` array (empty for none),
 * which every door -- page, API twin, dashboard card -- reads.
 */
void
venture_marketdata_attribution_set(
	JsonObject	*answer,
	GPtrArray	*attributions
);

/**
 * venture_marketdata_attribution_dup:
 * @answer: (nullable): an answer object
 *
 * Returns: (transfer full) (array zero-terminated=1): the answer's
 *   `attribution` lines; empty when it has none
 */
gchar **
venture_marketdata_attribution_dup(JsonObject *answer);

/**
 * venture_marketdata_attribution_append_html:
 * @html: the page being built
 * @attributions: (nullable) (array zero-terminated=1): the lines
 *
 * Appends one visible, escaped `<p class="source-attribution">` per line,
 * to sit right under the data it names. Nothing for none.
 */
void
venture_marketdata_attribution_append_html(
	GString			*html,
	const gchar *const	*attributions
);

/**
 * venture_marketdata_attribution_append_answer_html:
 * @html: the page being built
 * @answer: (nullable): an answer object
 *
 * venture_marketdata_attribution_append_html() of the answer's
 * `attribution` array.
 */
void
venture_marketdata_attribution_append_answer_html(
	GString		*html,
	JsonObject	*answer
);

/**
 * venture_marketdata_register_reports:
 * @registry: the report registry
 *
 * Registers market_deals, venue_index, watchlist, accounts, account_holdings and
 * external_pnl.
 */
void
venture_marketdata_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_MARKETDATA_BROWSE_H */
