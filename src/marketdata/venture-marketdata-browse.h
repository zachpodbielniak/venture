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
 * venues, groups, query, sorts, page, per_page, total, pages, rows}.
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
 *
 * What an instrument page asks.
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
} VentureMarketdataInstrumentQuery;

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
 * quantity), daily (60 days), bulk, tiers, watchlists, record_id,
 * stale_after_seconds}. base and each venue carry age_seconds and stale
 * beside taken_at, as a deal's sides do.
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
 *   "sell", "profit", "roi", "region", "rate" (the sell venue's sale rate)
 *   or "qty". A column sort reads a wide pool of deals and keeps the
 *   first @count; a row with no value for it (nowhere to sell, no item
 *   level) is last either way.
 * @dir: (nullable): "asc" or "desc"; %NULL for the column's natural
 *   order (most first for figures where more is better, A to Z for names)
 * @sell_venue: (nullable): sell only at this venue: a deal with no
 *   plausible price there is left out. With @venue_group it must be one
 *   of the group's, like @venue.
 * @now: the time "now" is, for each price's age; 0 for the wall clock
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
 * Returns: (transfer full) (nullable): the answer; %NULL on error
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
