/*
 * venture-arbitrage-strategies.c - The built-in strategies
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Five ways of finding an opportunity, each written against the same
 * public scan API a plugin gets (venture-arbitrage-engine.h), so a
 * plugin's strategy can do anything these do:
 *
 *  - spread: an instrument bought at one venue and sold at another --
 *    cross-realm flipping, drop shipping (`buy_sources` lists the N
 *    cheapest sources per item), retail and crypto spreads;
 *  - deal: a venue price at or under the group's deal price (its median,
 *    or the 33rd percentile with 15 venues or more), sold back at the
 *    region's figure;
 *  - transform: a recipe's inputs bought where each is cheapest, walking
 *    each venue's book for the units needed (a reusable component once),
 *    against the output's best venue;
 *  - cover: a surebet across bookmakers for one event, the best back odds
 *    for each outcome;
 *  - back_lay: a back at one venue matched by a lay at an exchange.
 *
 * Every candidate is chosen by an indexed read and bounded by
 * VENTURE_ARBITRAGE_SCAN_CANDIDATES per source; see the engine header.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <math.h>
#include <string.h>

#ifdef VENTURE_HAVE_SQLITE

/* Store rows a strategy reads to choose its candidates, per source. */
#define ARB_SCAN_ROWS (2000)

/* Recipes the transform strategy prices in one scan. */
#define ARB_SCAN_RECIPES (200)

/* Instruments one product may be quoted as. */
#define ARB_SCAN_INSTRUMENTS (100)

/* ==========================================================================
 * What every strategy reads the same way
 * ========================================================================== */

typedef struct
{
	VentureArbitrageScan	*scan;
	JsonObject		*options;
	gint64			 now;
	gint64			 max_age;	/* seconds, 0 for no bound */
	gint64			 units;
	const gchar		*group;
	const gchar		*category;
	const gchar		*kind;
	const gchar		*instrument;
	const gchar		*basis;
	gchar			**buy_venues;
	GHashTable		*intervals;	/* "id\037venue" -> interval */
	GHashTable		*references;	/* "id\037group\037key" -> VentureSeriesReference */
	guint			 stale;
	guint			 started;	/* events left out: under way or over */
	guint			 undated;	/* events kept that name no start */
} ArbWalk;

static void
arb_walk_init(
	ArbWalk			*walk,
	VentureArbitrageScan	*scan,
	const gchar		*default_basis
){
	const gchar *text;

	memset(walk, 0, sizeof(*walk));
	walk->scan = scan;
	walk->options = venture_arbitrage_scan_get_options(scan);
	walk->now = venture_arbitrage_scan_get_now(scan);
	walk->max_age = venture_json_object_get_int(walk->options, "max_age_hours", 0) * 3600;
	walk->units = venture_arbitrage_scan_get_units(scan);
	walk->group = venture_json_object_get_string(walk->options, "group_key", NULL);
	walk->category = venture_json_object_get_string(walk->options, "category_path", NULL);
	walk->kind = venture_json_object_get_string(walk->options, "kind", NULL);
	walk->instrument = venture_json_object_get_string(walk->options, "instrument", NULL);
	walk->basis = venture_json_object_get_string(walk->options, "sell_basis", default_basis);
	text = venture_json_object_get_string(walk->options, "buy_venues", NULL);
	walk->buy_venues = venture_string_is_empty(text) ? NULL : g_strsplit(text, ",", -1);
	walk->intervals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	walk->references = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static void
arb_walk_clear(ArbWalk *walk)
{
	if (walk->stale > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%u price%s older than max_age_hours %s left out.", walk->stale,
		                       (1 == walk->stale) ? "" : "s", (1 == walk->stale) ? "was" : "were");
		venture_arbitrage_scan_add_note(walk->scan, note);
	}

	if (walk->started > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%u event%s that had already started (commence_time at or before the "
		                       "scan's time) %s left out: odds on a match under way or over are "
		                       "not an opportunity.", walk->started, (1 == walk->started) ? "" : "s",
		                       (1 == walk->started) ? "was" : "were");
		venture_arbitrage_scan_add_note(walk->scan, note);
	}

	if (walk->undated > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%u event%s name%s no commence_time and %s kept: whether %s already "
		                       "started cannot be told from the odds, so bound the prices with "
		                       "max_age_hours.", walk->undated, (1 == walk->undated) ? "" : "s",
		                       (1 == walk->undated) ? "s" : "", (1 == walk->undated) ? "was" : "were",
		                       (1 == walk->undated) ? "it has" : "they have");
		venture_arbitrage_scan_add_note(walk->scan, note);
	}

	g_strfreev(walk->buy_venues);
	g_clear_pointer(&walk->intervals, g_hash_table_unref);
	g_clear_pointer(&walk->references, g_hash_table_unref);
}

/* Whether a price seen at @taken_at is young enough; counts it if not. */
static gboolean
arb_fresh(
	ArbWalk	*walk,
	gint64	 taken_at
){
	if ((walk->max_age <= 0) || (walk->now - taken_at <= walk->max_age))
		return TRUE;

	walk->stale++;

	return FALSE;
}

/* A minor-unit figure of the store as money, or NULL when absent. */
static VentureMoney *
arb_money(
	gint64		 minor,
	const gchar	*currency
){
	if ((VENTURE_SERIES_NONE == minor) || venture_string_is_empty(currency) ||
	    !venture_currency_is_valid(currency))
		return NULL;

	return venture_money_new_for_currency(minor, currency);
}

/* How often a venue updates, read once per scan. */
static gint64
arb_interval(
	ArbWalk			*walk,
	VentureSeriesStore	*store,
	gint64			 source_id,
	const gchar		*venue_key
){
	g_autofree gchar *key = NULL;
	VentureSeriesVenueState state;
	gint64 *cached;

	key = g_strdup_printf("%" G_GINT64_FORMAT "\037%s", source_id, venue_key);
	cached = g_hash_table_lookup(walk->intervals, key);

	if (NULL != cached)
		return *cached;

	memset(&state, 0, sizeof(state));

	if (!venture_series_store_get_venue_state(store, venue_key, &state, NULL) || !state.found)
		state.interval_seconds = 0;

	cached = g_new(gint64, 1);
	*cached = state.interval_seconds;
	g_hash_table_insert(walk->intervals, g_steal_pointer(&key), cached);

	return *cached;
}

/* The slow-moving figures of an instrument in a group (or at a venue when
 * it has none), read once per scan. */
static const VentureSeriesReference *
arb_reference(
	ArbWalk			*walk,
	VentureSeriesStore	*store,
	gint64			 source_id,
	const VentureSeriesRow	*row
){
	g_autofree gchar *key = NULL;
	VentureSeriesReference *reference;
	gboolean by_group;

	by_group = !venture_string_is_empty(row->group_key);
	key = g_strdup_printf("%" G_GINT64_FORMAT "\037%s\037%s", source_id,
	                      by_group ? row->group_key : row->venue_key, row->instrument_key);
	reference = g_hash_table_lookup(walk->references, key);

	if (NULL != reference)
		return reference;

	reference = g_new0(VentureSeriesReference, 1);

	if (!venture_series_store_reference(store, by_group ? NULL : row->venue_key,
	                                    by_group ? row->group_key : NULL, row->instrument_key,
	                                    walk->now, reference, NULL))
	{
		memset(reference, 0, sizeof(*reference));
		reference->sale_avg = VENTURE_SERIES_NONE;
		reference->sale_rate = NAN;
		reference->sold_per_day = NAN;
	}

	g_hash_table_insert(walk->references, g_steal_pointer(&key), reference);

	return reference;
}

/* What one unit sells for at a row's venue, on the asked basis. */
static VentureMoney *
arb_sell_price(
	ArbWalk			*walk,
	VentureSeriesStore	*store,
	gint64			 source_id,
	const VentureSeriesRow	*row
){
	if (0 == g_strcmp0(walk->basis, "market"))
		return arb_money(row->market_value, row->currency);

	if (0 == g_strcmp0(walk->basis, "region_median"))
		return arb_money(row->region_median, row->currency);

	if (0 == g_strcmp0(walk->basis, "bid"))
		return arb_money(row->bid_price, row->currency);

	/* FlippingPal's "profit at sale avg": what units actually sold for
	 * across the region, not what is listed now. */
	if (0 == g_strcmp0(walk->basis, "sale_avg"))
	{
		const VentureSeriesReference *reference;

		reference = arb_reference(walk, store, source_id, row);

		return arb_money(reference->sale_avg, reference->currency);
	}

	return arb_money(row->min_price, row->currency);
}

/* The spread of one row's price, sigma over mu, or NAN. */
static gdouble
arb_dispersion(const VentureSeriesRow *row)
{
	if ((VENTURE_SERIES_NONE == row->stddev) || (VENTURE_SERIES_NONE == row->mean) || (row->mean <= 0))
		return NAN;

	return (gdouble)row->stddev / (gdouble)row->mean;
}

static gboolean
arb_kind_matches(
	ArbWalk			*walk,
	const VentureSeriesRow	*row
){
	return venture_string_is_empty(walk->kind) ||
	       ((NULL != row->kind) && (0 == g_ascii_strcasecmp(row->kind, walk->kind)));
}

/*
 * Buying @units of @row's instrument at its venue, walking the book: the
 * total, the average unit price, the units the book could fill and the
 * depth. A venue whose store keeps only statistics (no tiers) is priced
 * at its lowest price with depth unknown.
 */
typedef struct
{
	VentureMoney	*amount;
	VentureMoney	*unit_price;
	gint64		 units;
	gdouble		 depth;
} ArbBuy;

static void
arb_buy_clear(ArbBuy *buy)
{
	g_clear_pointer(&buy->amount, venture_money_free);
	g_clear_pointer(&buy->unit_price, venture_money_free);
}

static gboolean
arb_buy(
	VentureSeriesStore	 *store,
	const VentureSeriesRow	 *row,
	gint64			  units,
	ArbBuy			 *out,
	GError			**error
){
	VentureSeriesBulkCost bulk;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];

	memset(out, 0, sizeof(*out));
	memset(&bulk, 0, sizeof(bulk));
	memset(currency, 0, sizeof(currency));

	if (!venture_series_store_bulk_cost(store, row->venue_key, row->instrument_key, units, &bulk,
	                                    currency, error))
		return FALSE;

	if ((bulk.filled > 0) && venture_currency_is_valid(currency))
	{
		out->amount = venture_money_new_for_currency(bulk.cost, currency);
		out->unit_price = venture_money_new_for_currency(bulk.average, currency);
		out->units = bulk.filled;
		out->depth = (gdouble)bulk.filled / (gdouble)units;
		return TRUE;
	}

	out->unit_price = arb_money(row->min_price, row->currency);

	if (NULL == out->unit_price)
		return TRUE;

	out->units = units;
	out->depth = NAN;

	return TRUE;
}

/* ==========================================================================
 * spread
 * ========================================================================== */

typedef struct
{
	gint64			 source_id;
	VentureSeriesStore	*store;
	VentureSeriesRow	*row;	/* borrowed from the arrays kept below */
} ArbQuote;

/* Each source's rows for one instrument: every venue, cheapest first. */
static GPtrArray *
arb_instrument_quotes(
	ArbWalk		 *walk,
	const gchar	 *instrument_key,
	GPtrArray	 *keep,
	GError		**error
){
	GPtrArray *sources;
	GPtrArray *quotes;
	guint i;
	guint j;

	sources = venture_arbitrage_scan_get_sources(walk->scan);
	quotes = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < sources->len; i++)
	{
		gint64 source_id = venture_entity_get_id(g_ptr_array_index(sources, i));
		VentureSeriesStore *store;
		GPtrArray *rows;

		store = venture_arbitrage_scan_open_store(walk->scan, source_id);

		if (NULL == store)
			continue;

		rows = venture_series_store_other_venues(store, instrument_key, walk->group, error);

		if (NULL == rows)
		{
			g_ptr_array_unref(quotes);
			return NULL;
		}

		g_ptr_array_add(keep, rows);

		for (j = 0; j < rows->len; j++)
		{
			ArbQuote *quote = g_new0(ArbQuote, 1);

			quote->source_id = source_id;
			quote->store = store;
			quote->row = g_ptr_array_index(rows, j);
			g_ptr_array_add(quotes, quote);
		}
	}

	return quotes;
}

static gboolean
arb_same_venue(
	const ArbQuote	*a,
	const ArbQuote	*b
){
	return (a->source_id == b->source_id) && (0 == g_strcmp0(a->row->venue_key, b->row->venue_key));
}

/* Prices one buy against one sell and hands it to the scan. */
static gboolean
arb_spread_pair(
	ArbWalk		 *walk,
	const ArbQuote	 *buy_quote,
	const ArbQuote	 *sell_quote,
	guint		  venues,
	GError		**error
){
	g_autoptr(VentureMoney) sell_price = NULL;
	g_autoptr(VentureMoney) reference = NULL;
	g_autoptr(JsonObject) extra = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *title = NULL;
	VentureArbitrageSide buy;
	VentureArbitrageSide sell;
	VentureArbitrageMarket market;
	const VentureSeriesRow *row;
	ArbBuy bought;
	gboolean ok;

	sell_price = arb_sell_price(walk, sell_quote->store, sell_quote->source_id, sell_quote->row);

	if (NULL == sell_price)
		return TRUE;

	if (!arb_buy(buy_quote->store, buy_quote->row, walk->units, &bought, error))
		return FALSE;

	if (NULL == bought.unit_price)
	{
		arb_buy_clear(&bought);
		return TRUE;
	}

	row = buy_quote->row;
	extra = json_object_new();

	if (bought.units < walk->units)
		json_object_set_int_member(extra, "requested_units", walk->units);

	/* Buy price against the region's sale average, for max_buy_pct
	 * (FlippingPal's "sale avg vs buy"). */
	if (json_object_has_member(walk->options, "max_buy_pct") ||
	    (0 == g_strcmp0(walk->basis, "sale_avg")))
	{
		const VentureSeriesReference *averages;

		averages = arb_reference(walk, buy_quote->store, buy_quote->source_id, row);

		if ((VENTURE_SERIES_NONE != averages->sale_avg) && (averages->sale_avg > 0) &&
		    (0 == g_strcmp0(averages->currency, venture_money_get_currency(bought.unit_price))))
			json_object_set_double_member(extra, "buy_vs_sale_avg",
				(gdouble)venture_money_get_amount(bought.unit_price) / (gdouble)averages->sale_avg);
	}

	memset(&buy, 0, sizeof(buy));
	buy.data_source_id = buy_quote->source_id;
	buy.venue_key = row->venue_key;
	buy.instrument_key = row->instrument_key;
	buy.instrument_name = row->instrument_name;
	buy.units = bought.units;
	buy.unit_price = bought.unit_price;
	buy.amount = bought.amount;
	buy.taken_at = row->taken_at;
	buy.interval_seconds = arb_interval(walk, buy_quote->store, buy_quote->source_id, row->venue_key);
	buy.depth = bought.depth;

	row = sell_quote->row;
	reference = arb_money(row->market_value, row->currency);
	memset(&sell, 0, sizeof(sell));
	sell.data_source_id = sell_quote->source_id;
	sell.venue_key = row->venue_key;
	sell.instrument_key = row->instrument_key;
	sell.instrument_name = row->instrument_name;
	sell.units = bought.units;
	sell.unit_price = sell_price;
	sell.reference = reference;
	sell.taken_at = row->taken_at;
	sell.interval_seconds = arb_interval(walk, sell_quote->store, sell_quote->source_id, row->venue_key);
	sell.depth = NAN;

	market.sale_rate = row->sale_rate;
	market.sold_per_day = row->sold_per_day;
	market.dispersion = arb_dispersion(row);
	market.venues = venues;

	key = g_strdup_printf("%s:%" G_GINT64_FORMAT ":%s>%" G_GINT64_FORMAT ":%s:%s",
	                      venture_json_object_get_string(walk->options, "strategy", "spread"),
	                      buy.data_source_id, buy.venue_key, sell.data_source_id, sell.venue_key,
	                      buy.instrument_key);
	title = g_strdup_printf("%s: %s \xe2\x86\x92 %s",
	                        (NULL != buy.instrument_name) ? buy.instrument_name : buy.instrument_key,
	                        buy_quote->row->venue_name ? buy_quote->row->venue_name : buy.venue_key,
	                        row->venue_name ? row->venue_name : sell.venue_key);

	ok = venture_arbitrage_scan_add_flip(walk->scan, key, title, &buy, &sell, &market, extra, error);
	arb_buy_clear(&bought);

	return ok;
}

/* The candidates: instruments cheap against their region at a buy venue,
 * by the indexed pct_vs_region, in order of first sighting. */
static GPtrArray *
arb_candidates(
	ArbWalk		 *walk,
	gboolean	  deals_only,
	GPtrArray	 *keep,
	GError		**error
){
	g_autoptr(GHashTable) seen = NULL;
	GPtrArray *sources;
	GPtrArray *candidates;
	gboolean truncated;
	guint i;

	sources = venture_arbitrage_scan_get_sources(walk->scan);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	candidates = g_ptr_array_new_with_free_func(g_free);
	truncated = FALSE;

	for (i = 0; i < sources->len; i++)
	{
		gint64 source_id = venture_entity_get_id(g_ptr_array_index(sources, i));
		VentureSeriesStore *store;
		VentureSeriesFilter filter;
		guint read;
		guint per_source;

		store = venture_arbitrage_scan_open_store(walk->scan, source_id);

		if (NULL == store)
			continue;

		venture_series_filter_init(&filter);
		filter.venue_keys = (const gchar *const *)walk->buy_venues;
		filter.group_key = walk->group;
		filter.category_prefix = walk->category;
		filter.instrument_key = walk->instrument;
		filter.in_stock_only = TRUE;
		filter.deals_only = deals_only;
		filter.sort = VENTURE_SERIES_SORT_PCT_VS_REGION;
		filter.descending = FALSE;
		filter.count = VENTURE_SERIES_MAX_PAGE;
		read = 0;
		per_source = 0;

		while (read < ARB_SCAN_ROWS)
		{
			GPtrArray *rows;
			guint j;

			filter.offset = read;
			rows = venture_series_store_list_current(store, &filter, error);

			if (NULL == rows)
			{
				g_ptr_array_unref(candidates);
				return NULL;
			}

			g_ptr_array_add(keep, rows);
			read += rows->len;

			for (j = 0; j < rows->len; j++)
			{
				VentureSeriesRow *row = g_ptr_array_index(rows, j);
				ArbQuote *quote;

				if (!arb_kind_matches(walk, row) || !arb_fresh(walk, row->taken_at))
					continue;

				/* A deal is one venue's row; a spread one instrument. */
				if (!deals_only && g_hash_table_contains(seen, row->instrument_key))
					continue;

				if (per_source >= VENTURE_ARBITRAGE_SCAN_CANDIDATES)
				{
					truncated = TRUE;
					break;
				}

				g_hash_table_add(seen, row->instrument_key);
				quote = g_new0(ArbQuote, 1);
				quote->source_id = source_id;
				quote->store = store;
				quote->row = row;
				g_ptr_array_add(candidates, quote);
				per_source++;
			}

			if (truncated || (rows->len < filter.count))
				break;
		}

		if (read >= ARB_SCAN_ROWS)
			truncated = TRUE;
	}

	if (truncated)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("Only the %d instruments per source that are cheapest against their "
		                       "region were considered; narrow the question with a category, a venue "
		                       "set or an instrument to see the rest.",
		                       VENTURE_ARBITRAGE_SCAN_CANDIDATES);
		venture_arbitrage_scan_add_note(walk->scan, note);
	}

	return candidates;
}

static gint
arb_compare_cheapest(
	gconstpointer	a,
	gconstpointer	b
){
	const ArbQuote *x = *(ArbQuote *const *)a;
	const ArbQuote *y = *(ArbQuote *const *)b;
	gint by_currency;

	by_currency = g_strcmp0(x->row->currency, y->row->currency);

	if (0 != by_currency)
		return by_currency;

	if (x->row->min_price != y->row->min_price)
		return (x->row->min_price < y->row->min_price) ? -1 : 1;

	return g_strcmp0(x->row->venue_key, y->row->venue_key);
}

static gboolean
arb_spread_scan(
	VentureArbitrageScan	 *scan,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(GPtrArray) keep = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	ArbWalk walk;
	gint64 per_item;
	gboolean ok;
	guint i;

	(void)user_data;

	arb_walk_init(&walk, scan, "min");
	keep = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	per_item = venture_json_object_get_int(walk.options, "buy_sources", 1);
	candidates = arb_candidates(&walk, FALSE, keep, error);
	ok = (NULL != candidates);

	for (i = 0; ok && (i < candidates->len); i++)
	{
		const ArbQuote *candidate = g_ptr_array_index(candidates, i);
		g_autoptr(GPtrArray) quotes = NULL;
		g_autoptr(GPtrArray) buys = NULL;
		g_autoptr(GPtrArray) sells = NULL;
		guint venues;
		guint j;
		guint k;

		quotes = arb_instrument_quotes(&walk, candidate->row->instrument_key, keep, error);

		if (NULL == quotes)
		{
			ok = FALSE;
			break;
		}

		buys = g_ptr_array_new();
		sells = g_ptr_array_new();
		venues = 0;

		for (j = 0; j < quotes->len; j++)
		{
			ArbQuote *quote = g_ptr_array_index(quotes, j);
			VentureSeriesRow *row = quote->row;

			if ((row->quantity > 0) && (VENTURE_SERIES_NONE != row->min_price))
				venues++;

			if (!arb_fresh(&walk, row->taken_at))
				continue;

			if ((row->quantity > 0) && (VENTURE_SERIES_NONE != row->min_price) &&
			    venture_arbitrage_scan_venue_allowed(scan, TRUE, row->venue_key))
				g_ptr_array_add(buys, quote);

			if (venture_arbitrage_scan_venue_allowed(scan, FALSE, row->venue_key))
				g_ptr_array_add(sells, quote);
		}

		/* The N cheapest buy sources in each currency: N = 1 is a
		 * flip, more is the drop-shipper's list of suppliers. */
		g_ptr_array_sort(buys, arb_compare_cheapest);

		for (j = 0; ok && (j < buys->len); j++)
		{
			const ArbQuote *buy = g_ptr_array_index(buys, j);
			g_autoptr(GHashTable) best = NULL;
			GHashTableIter iter;
			gpointer value;
			guint rank;

			/* Its rank among sources in its own currency. */
			rank = 0;

			for (k = 0; k < j; k++)
				if (0 == g_strcmp0(((ArbQuote *)g_ptr_array_index(buys, k))->row->currency,
				                   buy->row->currency))
					rank++;

			if (rank >= (guint)per_item)
				continue;

			/* The best other venue to sell at, per currency: a sell
			 * in another currency is compared after conversion. */
			best = g_hash_table_new(g_str_hash, g_str_equal);

			for (k = 0; k < sells->len; k++)
			{
				const ArbQuote *sell = g_ptr_array_index(sells, k);
				g_autoptr(VentureMoney) price = NULL;
				const ArbQuote *held;

				if (arb_same_venue(buy, sell))
					continue;

				price = arb_sell_price(&walk, sell->store, sell->source_id, sell->row);

				if (NULL == price)
					continue;

				held = g_hash_table_lookup(best, venture_money_get_currency(price));

				if (NULL != held)
				{
					g_autoptr(VentureMoney) held_price = NULL;

					held_price = arb_sell_price(&walk, held->store, held->source_id, held->row);

					if ((NULL != held_price) && (venture_money_compare(held_price, price) >= 0))
						continue;
				}

				g_hash_table_insert(best, (gpointer)g_intern_string(venture_money_get_currency(price)),
				                    (gpointer)sell);
			}

			g_hash_table_iter_init(&iter, best);

			while (ok && g_hash_table_iter_next(&iter, NULL, &value))
				ok = arb_spread_pair(&walk, buy, value, venues, error);
		}
	}

	arb_walk_clear(&walk);

	return ok;
}

/* ==========================================================================
 * deal
 * ========================================================================== */

static gboolean
arb_deal_scan(
	VentureArbitrageScan	 *scan,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(GPtrArray) keep = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	ArbWalk walk;
	gboolean ok;
	guint i;

	(void)user_data;

	/* A deal is judged against the group's figure, so that is what it
	 * is expected to fetch unless the question says otherwise. */
	arb_walk_init(&walk, scan, "region_median");
	keep = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	candidates = arb_candidates(&walk, TRUE, keep, error);
	ok = (NULL != candidates);

	for (i = 0; ok && (i < candidates->len); i++)
	{
		const ArbQuote *quote = g_ptr_array_index(candidates, i);

		/* Sold where it was bought: the sell set must allow it too. */
		if (!venture_arbitrage_scan_venue_allowed(scan, FALSE, quote->row->venue_key))
			continue;

		ok = arb_spread_pair(&walk, quote, quote, 1, error);
	}

	arb_walk_clear(&walk);

	return ok;
}

/* ==========================================================================
 * transform
 * ========================================================================== */

/* The instruments a product is quoted as: (source, key) pairs. */
static GPtrArray *
arb_product_instruments(
	VentureArbitrageScan	 *scan,
	gint64			  product_id,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
	venture_query_set_organization(query, venture_arbitrage_scan_get_organization_id(scan));
	venture_query_set_limit(query, ARB_SCAN_INSTRUMENTS);

	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ, product_id, error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;

	return venture_database_find(venture_context_get_database(venture_arbitrage_scan_get_context(scan)),
	                             query, error);
}

/* Whether the instrument record's source is one of the scan's. */
static VentureSeriesStore *
arb_instrument_store(
	VentureArbitrageScan	*scan,
	VentureEntity		*instrument,
	gint64			*out_source,
	gchar			**out_key
){
	GPtrArray *sources;
	gint64 source_id;
	guint i;

	*out_key = NULL;
	g_object_get(instrument, "data-source-id", &source_id, "key", out_key, NULL);
	*out_source = source_id;

	if ((source_id <= 0) || venture_string_is_empty(*out_key))
		return NULL;

	sources = venture_arbitrage_scan_get_sources(scan);

	for (i = 0; i < sources->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(sources, i)) == source_id)
			return venture_arbitrage_scan_open_store(scan, source_id);

	return NULL;
}

static gchar *
arb_product_name(
	VentureDatabase	*database,
	gint64		 product_id
){
	g_autoptr(VentureEntity) product = NULL;

	product = venture_database_get(database, VENTURE_TYPE_PRODUCT, product_id, NULL);

	if (NULL != product)
		return venture_entity_get_display_name(product);

	return g_strdup_printf("Product #%" G_GINT64_FORMAT, product_id);
}

/* One input's cheapest venue for the units needed. */
typedef struct
{
	gint64		 product_id;
	gchar		*name;
	gint64		 quantity;
	gboolean	 reusable;
	gint64		 source_id;
	gchar		*instrument_key;
	gchar		*venue_key;
	gchar		*venue_name;
	VentureMoney	*unit_price;	/* the venue's own currency */
	VentureMoney	*amount;	/* the venue's own currency */
	VentureMoney	*converted;	/* in the output's currency */
	VentureMoney	*fees;		/* the venue's own currency */
	gint64		 taken_at;
	gdouble		 depth;
	gchar		*problem;
} ArbInput;

static void
arb_input_free(gpointer data)
{
	ArbInput *input;

	input = data;
	g_free(input->name);
	g_free(input->instrument_key);
	g_free(input->venue_key);
	g_free(input->venue_name);
	g_clear_pointer(&input->unit_price, venture_money_free);
	g_clear_pointer(&input->amount, venture_money_free);
	g_clear_pointer(&input->converted, venture_money_free);
	g_clear_pointer(&input->fees, venture_money_free);
	g_free(input->problem);
	g_free(input);
}

/*
 * Prices one input: across every instrument the product is quoted as, at
 * every allowed buy venue, the cost of the units needed by walking that
 * venue's book; the cheapest after conversion into @currency wins. A
 * venue that cannot fill the units, or whose currency has no rate, is
 * passed over; none left leaves the input unquoted.
 */
static gboolean
arb_price_input(
	ArbWalk		 *walk,
	ArbInput	 *input,
	const gchar	 *currency,
	GPtrArray	 *keep,
	GError		**error
){
	g_autoptr(GPtrArray) instruments = NULL;
	guint i;
	guint j;

	instruments = arb_product_instruments(walk->scan, input->product_id, error);

	if (NULL == instruments)
		return FALSE;

	for (i = 0; i < instruments->len; i++)
	{
		g_autofree gchar *key = NULL;
		VentureSeriesStore *store;
		GPtrArray *rows;
		gint64 source_id;

		store = arb_instrument_store(walk->scan, g_ptr_array_index(instruments, i), &source_id, &key);

		if (NULL == store)
			continue;

		rows = venture_series_store_other_venues(store, key, walk->group, error);

		if (NULL == rows)
			return FALSE;

		g_ptr_array_add(keep, rows);

		for (j = 0; j < rows->len; j++)
		{
			VentureSeriesRow *row = g_ptr_array_index(rows, j);
			g_autoptr(VentureMoney) converted = NULL;
			ArbBuy bought;

			if ((row->quantity <= 0) || (VENTURE_SERIES_NONE == row->min_price) ||
			    !venture_arbitrage_scan_venue_allowed(walk->scan, TRUE, row->venue_key) ||
			    !arb_fresh(walk, row->taken_at))
				continue;

			if (!arb_buy(store, row, input->quantity, &bought, error))
				return FALSE;

			/* A book too thin for the units is not this input's
			 * source: buying half is not making the thing. */
			if ((NULL == bought.unit_price) || (bought.units < input->quantity))
			{
				arb_buy_clear(&bought);
				continue;
			}

			if (NULL == bought.amount)
				bought.amount = venture_money_multiply_int(bought.unit_price, input->quantity, NULL);

			converted = (NULL != bought.amount)
				? venture_arbitrage_scan_convert(walk->scan, bought.amount, currency, NULL) : NULL;

			if (NULL == converted)
			{
				arb_buy_clear(&bought);
				continue;
			}

			if ((NULL != input->converted) && (venture_money_compare(converted, input->converted) >= 0))
			{
				arb_buy_clear(&bought);
				continue;
			}

			g_clear_pointer(&input->converted, venture_money_free);
			g_clear_pointer(&input->unit_price, venture_money_free);
			g_clear_pointer(&input->amount, venture_money_free);
			g_free(input->venue_key);
			g_free(input->venue_name);
			g_free(input->instrument_key);
			input->converted = g_steal_pointer(&converted);
			input->unit_price = g_steal_pointer(&bought.unit_price);
			input->amount = g_steal_pointer(&bought.amount);
			input->venue_key = g_strdup(row->venue_key);
			input->venue_name = g_strdup((NULL != row->venue_name) ? row->venue_name : row->venue_key);
			input->instrument_key = g_strdup(key);
			input->source_id = source_id;
			input->taken_at = row->taken_at;
			input->depth = bought.depth;
			arb_buy_clear(&bought);
		}
	}

	if (NULL == input->converted)
		input->problem = g_strdup_printf("%s: no allowed venue quotes %" G_GINT64_FORMAT " in a "
		                                 "currency with a rate to %s", input->name, input->quantity,
		                                 currency);

	return TRUE;
}

/* The output's best venue: the highest price on the basis, compared in
 * the book currency (rows with no rate to it are passed over). */
typedef struct
{
	gint64			 source_id;
	VentureSeriesStore	*store;
	VentureSeriesRow	*row;
	VentureMoney		*price;
	gchar			*key;
	guint			 venues;
} ArbOutput;

static gboolean
arb_price_output(
	ArbWalk		 *walk,
	gint64		  product_id,
	ArbOutput	 *out,
	GPtrArray	 *keep,
	GError		**error
){
	g_autoptr(GPtrArray) instruments = NULL;
	g_autoptr(VentureMoney) best = NULL;
	g_autofree gchar *book = NULL;
	guint i;
	guint j;

	memset(out, 0, sizeof(*out));
	instruments = arb_product_instruments(walk->scan, product_id, error);

	if (NULL == instruments)
		return FALSE;

	book = venture_database_get_book_currency(
		venture_context_get_database(venture_arbitrage_scan_get_context(walk->scan)),
		venture_arbitrage_scan_get_organization_id(walk->scan));

	for (i = 0; i < instruments->len; i++)
	{
		g_autofree gchar *key = NULL;
		VentureSeriesStore *store;
		GPtrArray *rows;
		gint64 source_id;

		store = arb_instrument_store(walk->scan, g_ptr_array_index(instruments, i), &source_id, &key);

		if (NULL == store)
			continue;

		rows = venture_series_store_other_venues(store, key, walk->group, error);

		if (NULL == rows)
			return FALSE;

		g_ptr_array_add(keep, rows);

		for (j = 0; j < rows->len; j++)
		{
			VentureSeriesRow *row = g_ptr_array_index(rows, j);
			g_autoptr(VentureMoney) price = NULL;
			g_autoptr(VentureMoney) in_book = NULL;

			if ((row->quantity > 0) && (VENTURE_SERIES_NONE != row->min_price))
				out->venues++;

			if (!venture_arbitrage_scan_venue_allowed(walk->scan, FALSE, row->venue_key) ||
			    !arb_fresh(walk, row->taken_at))
				continue;

			price = arb_sell_price(walk, store, source_id, row);
			in_book = (NULL != price) ? venture_arbitrage_scan_convert(walk->scan, price, book, NULL) : NULL;

			if ((NULL == in_book) || ((NULL != best) && (venture_money_compare(in_book, best) <= 0)))
				continue;

			g_clear_pointer(&best, venture_money_free);
			g_clear_pointer(&out->price, venture_money_free);
			g_free(out->key);
			best = g_steal_pointer(&in_book);
			out->price = g_steal_pointer(&price);
			out->source_id = source_id;
			out->store = store;
			out->row = row;
			out->key = g_strdup(key);
		}
	}

	return TRUE;
}

static void
arb_add_money(
	VentureMoney		**total,
	const VentureMoney	 *part
){
	VentureMoney *next;

	if (NULL == part)
		return;

	if (NULL == *total)
	{
		*total = venture_money_copy(part);
		return;
	}

	next = venture_money_add(*total, part, NULL);

	if (NULL != next)
	{
		venture_money_free(*total);
		*total = next;
	}
}

static gboolean
arb_transform_recipe(
	ArbWalk		 *walk,
	VentureEntity	 *recipe,
	GPtrArray	 *keep,
	GError		**error
){
	VentureDatabase *database;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(GPtrArray) inputs = NULL;
	g_autoptr(GPtrArray) missing = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) buy_fees = NULL;
	g_autoptr(VentureMoney) gross_native = NULL;
	g_autoptr(VentureMoney) sell_fees_native = NULL;
	g_autoptr(VentureMoney) deposit = NULL;
	g_autoptr(VentureMoney) transfer = NULL;
	g_autoptr(GHashTable) moved = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *recipe_name = NULL;
	g_autofree gchar *output_name = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *title = NULL;
	VentureFeeQuote quote;
	VentureArbitrageEvidence evidence;
	ArbOutput output;
	JsonObject *opportunity;
	JsonObject *narrow;
	JsonArray *array;
	JsonArray *legs;
	const gchar *currency;
	gboolean refundable;
	gint64 output_id;
	gint64 output_quantity;
	gint64 batches;
	gint64 oldest;
	gint64 hours;
	gdouble depth;
	guint i;

	database = venture_context_get_database(venture_arbitrage_scan_get_context(walk->scan));
	batches = walk->units;
	g_object_get(recipe, "output-product-id", &output_id, "output-quantity", &output_quantity, NULL);
	output_quantity = MAX((gint64)1, output_quantity);
	recipe_name = venture_entity_get_display_name(recipe);
	output_name = arb_product_name(database, output_id);
	missing = g_ptr_array_new_with_free_func(g_free);
	inputs = g_ptr_array_new_with_free_func(arb_input_free);

	/* --- The output first: its currency is the one everything else is
	 * compared and added in --- */

	if (!arb_price_output(walk, output_id, &output, keep, error))
		return FALSE;

	if (NULL == output.price)
	{
		currency = NULL;
		g_ptr_array_add(missing, g_strdup_printf("%s: no allowed venue quotes it", output_name));
	}
	else
		currency = venture_money_get_currency(output.price);

	/* --- The inputs, each at its cheapest venue --- */

	query = venture_query_new(VENTURE_TYPE_RECIPE_COMPONENT);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(recipe), error) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		goto fail;

	components = venture_database_find(database, query, error);

	if (NULL == components)
		goto fail;

	for (i = 0; i < components->len; i++)
	{
		VentureEntity *component = g_ptr_array_index(components, i);
		ArbInput *input;
		gint64 quantity;
		gboolean reusable;

		input = g_new0(ArbInput, 1);
		g_object_get(component, "product-id", &input->product_id, "quantity", &quantity,
		             "reusable", &reusable, NULL);

		if (quantity < 1)
		{
			arb_input_free(input);
			continue;
		}

		/* A tool is needed once however many batches -- never
		 * multiplied (the goal_materials rule). */
		input->reusable = reusable;
		input->quantity = reusable ? quantity : quantity * batches;
		input->name = arb_product_name(database, input->product_id);
		input->depth = NAN;
		g_ptr_array_add(inputs, input);

		if (NULL == currency)
			continue;

		if (!arb_price_input(walk, input, currency, keep, error))
			goto fail;

		if (NULL != input->problem)
			g_ptr_array_add(missing, g_strdup(input->problem));
	}

	/* --- Fees, at each venue in its own currency, then converted --- */

	moved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	refundable = TRUE;
	oldest = (NULL != output.row) ? output.row->taken_at : walk->now;
	depth = 1.0;
	hours = 0;

	for (i = 0; (NULL != currency) && (0 == missing->len) && (i < inputs->len); i++)
	{
		ArbInput *input = g_ptr_array_index(inputs, i);
		g_autoptr(VentureMoney) fees = NULL;

		arb_add_money(&cost, input->converted);
		oldest = MIN(oldest, input->taken_at);

		if (isfinite(input->depth))
			depth = MIN(depth, input->depth);

		if (!venture_arbitrage_scan_fees(walk->scan, input->source_id, input->venue_key,
		                                 VENTURE_FEE_SIDE_BUY, input->amount, input->quantity, NULL,
		                                 input->instrument_key, &quote, &local_error))
		{
			g_ptr_array_add(missing, g_strdup_printf("fees: %s", local_error->message));
			g_clear_error(&local_error);
			continue;
		}

		input->fees = g_steal_pointer(&quote.fee);
		venture_fee_quote_clear(&quote);
		fees = venture_arbitrage_scan_convert(walk->scan, input->fees, currency, NULL);

		if (NULL == fees)
		{
			g_ptr_array_add(missing, g_strdup_printf("fees at %s: no exchange rate to %s",
			                                         input->venue_name, currency));
			continue;
		}

		arb_add_money(&buy_fees, fees);

		/* Each venue the inputs come from, other than where the
		 * output sells, is a lot to move. */
		if ((input->source_id != output.source_id) ||
		    (0 != g_strcmp0(input->venue_key, output.row->venue_key)))
		{
			g_autofree gchar *where = g_strdup_printf("%" G_GINT64_FORMAT "\037%s",
			                                          input->source_id, input->venue_key);

			if (!g_hash_table_contains(moved, where))
				g_hash_table_add(moved, g_steal_pointer(&where));
		}
	}

	if ((NULL != currency) && (0 == missing->len))
	{
		gint64 sold_units = output_quantity * batches;
		g_autoptr(VentureMoney) reference = NULL;

		gross_native = venture_money_multiply_int(output.price, sold_units, error);

		if (NULL == gross_native)
			goto fail;

		reference = arb_money(output.row->market_value, output.row->currency);

		if (NULL != reference)
		{
			VentureMoney *all = venture_money_multiply_int(reference, sold_units, NULL);

			venture_money_free(reference);
			reference = all;
		}

		if (!venture_arbitrage_scan_fees(walk->scan, output.source_id, output.row->venue_key,
		                                 VENTURE_FEE_SIDE_SELL, gross_native, sold_units, reference,
		                                 output.row->instrument_key, &quote, &local_error))
		{
			g_ptr_array_add(missing, g_strdup_printf("fees: %s", local_error->message));
			g_clear_error(&local_error);
		}
		else
		{
			sell_fees_native = g_steal_pointer(&quote.fee);
			deposit = g_steal_pointer(&quote.deposit);
			refundable = quote.deposit_refundable || (NULL == deposit);
			venture_fee_quote_clear(&quote);
		}

		/* What moving the lots costs: each input venue's transfer cost,
		 * and the output venue's once if anything moved to it. */
		if (g_hash_table_size(moved) > 0)
		{
			GHashTableIter iter;
			gpointer where;

			g_hash_table_iter_init(&iter, moved);

			while (g_hash_table_iter_next(&iter, &where, NULL))
			{
				g_auto(GStrv) parts = g_strsplit(where, "\037", 2);
				const VentureArbitrageVenue *venue;

				venue = venture_arbitrage_scan_venue(walk->scan, g_ascii_strtoll(parts[0], NULL, 10),
				                                     parts[1]);
				hours += MAX((gint64)0, venue->transfer_hours);

				if (NULL != venue->transfer_cost)
				{
					g_autoptr(VentureMoney) part = NULL;

					part = venture_arbitrage_scan_convert(walk->scan, venue->transfer_cost, currency, NULL);

					if (NULL == part)
						g_ptr_array_add(missing, g_strdup_printf("transfer from %s: no exchange "
						                                         "rate to %s", venue->name, currency));
					else
						arb_add_money(&transfer, part);
				}
			}

			{
				const VentureArbitrageVenue *venue;

				venue = venture_arbitrage_scan_venue(walk->scan, output.source_id, output.row->venue_key);
				hours += MAX((gint64)0, venue->transfer_hours);
				arb_add_money(&transfer, venue->transfer_cost);
			}
		}
	}

	/* --- The opportunity --- */

	key = g_strdup_printf("transform:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT "%s%s",
	                      venture_entity_get_id(recipe), (NULL != output.row) ? output.source_id : 0,
	                      (NULL != output.row) ? ":" : "",
	                      (NULL != output.row) ? output.row->venue_key : "");
	title = g_strdup_printf("%s: %s", recipe_name, output_name);
	opportunity = json_object_new();
	json_object_set_string_member(opportunity, "strategy", "transform");
	json_object_set_string_member(opportunity, "key", key);
	json_object_set_string_member(opportunity, "title", title);
	json_object_set_int_member(opportunity, "recipe_id", venture_entity_get_id(recipe));
	json_object_set_int_member(opportunity, "units", batches);
	json_object_set_int_member(opportunity, "output_units", output_quantity * batches);

	if (NULL != output.key)
	{
		json_object_set_string_member(opportunity, "instrument_key", output.key);
		json_object_set_int_member(opportunity, "data_source_id", output.source_id);
	}

	json_object_set_string_member(opportunity, "instrument_name", output_name);

	if (NULL != currency)
		json_object_set_string_member(opportunity, "currency", currency);
	else
		json_object_set_null_member(opportunity, "currency");

	narrow = json_object_new();
	json_object_set_int_member(narrow, "recipe_id", venture_entity_get_id(recipe));
	json_object_set_object_member(opportunity, "narrow", narrow);

	/* Each input's line: where it is cheapest and what that costs, or
	 * why it has no price. */
	array = json_array_new();
	legs = json_array_new();

	for (i = 0; i < inputs->len; i++)
	{
		ArbInput *input = g_ptr_array_index(inputs, i);
		JsonObject *line = json_object_new();

		json_object_set_int_member(line, "product_id", input->product_id);
		json_object_set_string_member(line, "name", input->name);
		json_object_set_int_member(line, "quantity", input->quantity);
		json_object_set_boolean_member(line, "reusable", input->reusable);

		if (NULL != input->venue_key)
		{
			json_object_set_int_member(line, "data_source_id", input->source_id);
			json_object_set_string_member(line, "instrument_key", input->instrument_key);
			json_object_set_string_member(line, "venue_key", input->venue_key);
			json_object_set_string_member(line, "venue_name", input->venue_name);
		}

		venture_arbitrage_set_money(line, "unit_price", input->unit_price);
		venture_arbitrage_set_money(line, "cost", input->amount);
		venture_arbitrage_set_money(line, "fees", input->fees);
		venture_arbitrage_set_money(line, "converted", input->converted);

		if (NULL != input->problem)
			json_object_set_string_member(line, "problem", input->problem);

		json_array_add_object_element(array, line);
	}

	json_object_set_array_member(opportunity, "inputs", array);

	if (NULL != output.row)
	{
		JsonObject *sell = json_object_new();

		json_object_set_int_member(sell, "data_source_id", output.source_id);
		json_object_set_string_member(sell, "venue_key", output.row->venue_key);
		json_object_set_string_member(sell, "venue_name",
		                              (NULL != output.row->venue_name) ? output.row->venue_name
		                                                                : output.row->venue_key);
		venture_arbitrage_set_money(sell, "unit_price", output.price);
		venture_arbitrage_set_money(sell, "amount", gross_native);
		venture_arbitrage_set_money(sell, "fees", sell_fees_native);
		venture_arbitrage_set_money(sell, "deposit", deposit);
		json_object_set_object_member(opportunity, "sell", sell);
		venture_arbitrage_set_ratio(opportunity, "sale_rate", output.row->sale_rate);
		venture_arbitrage_set_ratio(opportunity, "sold_per_day", output.row->sold_per_day);
	}

	json_object_set_int_member(opportunity, "age_seconds", MAX((gint64)0, walk->now - oldest));

	/* The output's price fixed the currency, so its sale, fees and
	 * deposit are in it already; the inputs and the moves were
	 * converted above. */
	if (0 == missing->len)
	{
		VentureArbitrageFlipInput input;
		VentureArbitrageFlip flip;
		g_autoptr(VentureMoney) unwind = NULL;
		gint64 ppm;

		/* Legs for the plan: every input bought where it is cheapest,
		 * the output sold. */
		for (i = 0; i < inputs->len; i++)
		{
			ArbInput *line = g_ptr_array_index(inputs, i);
			JsonObject *leg = json_object_new();
			g_autofree gchar *unit = venture_money_to_string(line->unit_price);
			g_autofree gchar *amount = venture_money_to_string(line->amount);

			json_object_set_string_member(leg, "kind", "buy");
			json_object_set_int_member(leg, "data_source_id", line->source_id);
			json_object_set_string_member(leg, "venue_key", line->venue_key);
			json_object_set_string_member(leg, "instrument_key", line->instrument_key);
			json_object_set_int_member(leg, "quantity", line->quantity);
			json_object_set_string_member(leg, "unit_price", unit);
			json_object_set_string_member(leg, "amount", amount);

			if ((NULL != line->fees) && !venture_money_is_zero(line->fees))
			{
				g_autofree gchar *fees = venture_money_to_string(line->fees);

				json_object_set_string_member(leg, "fees", fees);
			}

			json_array_add_object_element(legs, leg);
		}

		{
			JsonObject *leg = json_object_new();
			g_autofree gchar *unit = venture_money_to_string(output.price);
			g_autofree gchar *amount = venture_money_to_string(gross_native);

			json_object_set_string_member(leg, "kind", "sell");
			json_object_set_int_member(leg, "data_source_id", output.source_id);
			json_object_set_string_member(leg, "venue_key", output.row->venue_key);
			json_object_set_string_member(leg, "instrument_key", output.key);
			json_object_set_int_member(leg, "quantity", output_quantity * batches);
			json_object_set_string_member(leg, "unit_price", unit);
			json_object_set_string_member(leg, "amount", amount);

			if ((NULL != sell_fees_native) && !venture_money_is_zero(sell_fees_native))
			{
				g_autofree gchar *fees = venture_money_to_string(sell_fees_native);

				json_object_set_string_member(leg, "fees", fees);
			}

			json_array_add_object_element(legs, leg);
		}

		arb_add_money(&unwind, deposit);
		arb_add_money(&unwind, transfer);

		venture_arbitrage_flip_input_init(&input);
		input.buy_cost = cost;
		input.buy_fees = buy_fees;
		input.sell_gross = gross_native;
		input.sell_fees = sell_fees_native;
		input.deposit = deposit;
		input.deposit_refundable = refundable;
		input.sale_rate = output.row->sale_rate;
		input.transfer_cost = transfer;
		input.units = output_quantity * batches;
		input.sold_per_day = output.row->sold_per_day;
		input.transit_days = (gdouble)hours / 24.0;
		input.unwind_loss = unwind;

		if (venture_arbitrage_member_percent(walk->options, "share", 1000000, FALSE, &ppm, NULL) &&
		    (ppm > 0))
			input.share = MIN(1.0, (gdouble)ppm / 1000000.0);

		if ((NULL != cost) && venture_arbitrage_flip(&input, &flip, &local_error))
		{
			venture_arbitrage_set_money(opportunity, "net", flip.net);
			venture_arbitrage_set_money(opportunity, "capital", flip.capital);
			venture_arbitrage_set_money(opportunity, "listing_loss", flip.listing_loss);
			venture_arbitrage_set_money(opportunity, "ev", flip.ev);
			venture_arbitrage_set_ratio(opportunity, "roi", flip.roi);
			venture_arbitrage_set_ratio(opportunity, "relists", flip.relists);
			venture_arbitrage_set_ratio(opportunity, "lock_days", flip.lock_days);
			venture_arbitrage_set_ratio(opportunity, "roi_per_day", flip.roi_per_day);
			venture_arbitrage_set_ratio(opportunity, "annualized", flip.annualized);
			venture_arbitrage_flip_clear(&flip);
		}
		else if (NULL != local_error)
		{
			/* A market that never sells is no craft to make. */
			g_ptr_array_add(missing, g_strdup(local_error->message));
			g_clear_error(&local_error);
		}

		venture_arbitrage_set_money(opportunity, "cost", cost);
		venture_arbitrage_set_money(opportunity, "buy_fees", buy_fees);
		venture_arbitrage_set_money(opportunity, "gross", gross_native);
		venture_arbitrage_set_money(opportunity, "transfer_cost", transfer);
	}

	json_object_set_array_member(opportunity, "legs", legs);

	if (missing->len > 0)
	{
		array = json_array_new();

		for (i = 0; i < missing->len; i++)
			json_array_add_string_element(array, g_ptr_array_index(missing, i));

		json_object_set_array_member(opportunity, "missing", array);
		json_object_set_null_member(opportunity, "net");
		json_object_set_null_member(opportunity, "capital");
		json_object_set_null_member(opportunity, "roi");
	}

	evidence.age_seconds = MAX((gint64)0, walk->now - oldest);
	evidence.interval_seconds = (NULL != output.row)
		? arb_interval(walk, output.store, output.source_id, output.row->venue_key) : 0;
	evidence.venues = output.venues;
	evidence.dispersion = (NULL != output.row) ? arb_dispersion(output.row) : NAN;
	evidence.depth = depth;
	venture_arbitrage_set_ratio(opportunity, "confidence", venture_arbitrage_confidence(&evidence));
	json_object_set_array_member(opportunity, "warnings", json_array_new());
	venture_arbitrage_scan_add(walk->scan, opportunity);

	g_clear_pointer(&output.price, venture_money_free);
	g_free(output.key);

	return TRUE;

fail:
	g_clear_pointer(&output.price, venture_money_free);
	g_free(output.key);

	return FALSE;
}

static gboolean
arb_transform_scan(
	VentureArbitrageScan	 *scan,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(GPtrArray) keep = NULL;
	g_autoptr(GPtrArray) recipes = NULL;
	g_autoptr(VentureQuery) query = NULL;
	VentureContext *context;
	ArbWalk walk;
	gint64 recipe_id;
	gboolean ok;
	guint i;

	(void)user_data;

	context = venture_arbitrage_scan_get_context(scan);

	if (!venture_context_module_enabled(context, "production"))
	{
		venture_arbitrage_scan_add_note(scan, "The transform strategy prices recipes, and the "
		                                      "production module is off.");
		return TRUE;
	}

	arb_walk_init(&walk, scan, "min");
	keep = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	recipe_id = venture_json_object_get_int(walk.options, "recipe_id", 0);
	query = venture_query_new(VENTURE_TYPE_RECIPE);
	venture_query_set_organization(query, venture_arbitrage_scan_get_organization_id(scan));
	venture_query_set_limit(query, ARB_SCAN_RECIPES + 1);
	ok = venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, error);

	if (ok && (recipe_id > 0))
		ok = venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, recipe_id, error);

	recipes = ok ? venture_database_find(venture_context_get_database(context), query, error) : NULL;
	ok = (NULL != recipes);

	if (ok && (recipe_id > 0) && (0 == recipes->len))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No recipe #%" G_GINT64_FORMAT " in this organization", recipe_id);
		ok = FALSE;
	}

	if (ok && (recipes->len > ARB_SCAN_RECIPES))
	{
		g_ptr_array_set_size(recipes, ARB_SCAN_RECIPES);
		venture_arbitrage_scan_add_note(scan, "Only the first 200 recipes by name were priced; "
		                                      "name one with recipe_id.");
	}

	for (i = 0; ok && (i < recipes->len); i++)
	{
		VentureEntity *recipe = g_ptr_array_index(recipes, i);
		gboolean active;

		g_object_get(recipe, "active", &active, NULL);

		/* An inactive recipe is not being made; one asked for by id
		 * still answers. */
		if (!active && (0 == recipe_id))
			continue;

		ok = arb_transform_recipe(&walk, recipe, keep, error);
	}

	arb_walk_clear(&walk);

	return ok;
}

/* ==========================================================================
 * Odds: cover and back_lay
 * ========================================================================== */

/* The stake: the question's, else 100 in the bookmakers' currency, else
 * in the book currency. */
static VentureMoney *
arb_stake(
	ArbWalk		*walk,
	const gchar	*currency
){
	g_autofree gchar *book = NULL;
	const gchar *text;
	gint exponent;
	gint64 hundred;

	text = venture_json_object_get_string(walk->options, "total_stake", NULL);

	if (NULL != text)
		return venture_money_from_string(text, NULL, NULL);

	if (venture_string_is_empty(currency))
	{
		book = venture_database_get_book_currency(
			venture_context_get_database(venture_arbitrage_scan_get_context(walk->scan)),
			venture_arbitrage_scan_get_organization_id(walk->scan));
		currency = book;
	}

	exponent = venture_currency_get_exponent(currency);
	hundred = 100;

	while (exponent-- > 0)
		hundred *= 10;

	return venture_money_new_for_currency(hundred, currency);
}

/* A venue's commission on winnings, from its fee model; 0 for none. */
static gboolean
arb_commission(
	ArbWalk			 *walk,
	gint64			  source_id,
	const gchar		 *venue_key,
	const VentureMoney	 *stake,
	gdouble			 *out,
	GError			**error
){
	VentureFeeQuote quote;

	if (!venture_arbitrage_scan_fees(walk->scan, source_id, venue_key, VENTURE_FEE_SIDE_SELL, stake, 1,
	                                 NULL, NULL, &quote, error))
		return FALSE;

	*out = quote.commission;
	venture_fee_quote_clear(&quote);

	return TRUE;
}

/* Whether a bookmaker may take a stake in @currency: one with no record
 * or no currency may; one in another currency may not. */
static gboolean
arb_takes_currency(
	ArbWalk		*walk,
	gint64		 source_id,
	const gchar	*venue_key,
	const gchar	*currency
){
	const VentureArbitrageVenue *venue;

	venue = venture_arbitrage_scan_venue(walk->scan, source_id, venue_key);

	return venture_string_is_empty(venue->currency) || (NULL == currency) ||
	       (0 == g_strcmp0(venue->currency, currency));
}

/* The stake's currency when the question names none: the bookmakers'. */
static const gchar *
arb_quotes_currency(
	ArbWalk		*walk,
	gint64		 source_id,
	GPtrArray	*quotes
){
	guint i;

	if (json_object_has_member(walk->options, "total_stake"))
		return NULL;

	for (i = 0; i < quotes->len; i++)
	{
		VentureSeriesQuoteRow *quote = g_ptr_array_index(quotes, i);
		const VentureArbitrageVenue *venue;

		venue = venture_arbitrage_scan_venue(walk->scan, source_id, quote->venue_key);

		if (!venture_string_is_empty(venue->currency))
			return venue->currency;
	}

	return NULL;
}

static gchar *
arb_instrument_name(
	VentureSeriesStore	*store,
	const gchar		*key
){
	g_autoptr(VentureSeriesInstrumentRow) row = NULL;

	if (venture_series_store_get_instrument(store, key, &row, NULL) && (NULL != row) &&
	    !venture_string_is_empty(row->name))
		return g_strdup(row->name);

	return g_strdup(key);
}

/*
 * When @key's event starts, from the commence_time attribute an odds
 * provider stores on the event (the-odds-api's plugin does, and so does
 * any JSONL that follows it); an outcome asks its parent, a few levels
 * up at most -- parents are a provider's data, and a loop in them must
 * end a scan, not hang it. %FALSE when none names one, or it is not a
 * time.
 */
static gboolean
arb_commence_time(
	VentureSeriesStore	*store,
	const gchar		*key,
	guint			 depth,
	gint64			*out
){
	g_autoptr(VentureSeriesInstrumentRow) row = NULL;
	g_autoptr(JsonNode) attrs = NULL;
	g_autoptr(GDateTime) when = NULL;
	const gchar *text;

	if (!venture_series_store_get_instrument(store, key, &row, NULL) || (NULL == row))
		return FALSE;

	if (!venture_string_is_empty(row->attrs_json))
		attrs = venture_json_parse(row->attrs_json, NULL);

	text = ((NULL != attrs) && JSON_NODE_HOLDS_OBJECT(attrs))
		? venture_json_object_get_string(json_node_get_object(attrs), "commence_time", NULL) : NULL;

	if (venture_string_is_empty(text))
		return (depth < 4) && !venture_string_is_empty(row->parent_key) &&
		       arb_commence_time(store, row->parent_key, depth + 1, out);

	when = venture_time_from_string(text, NULL);

	if (NULL == when)
		return FALSE;

	*out = g_date_time_to_unix(when);

	return TRUE;
}

/*
 * Whether @key's event is open for betting as of the scan's time: one
 * whose commence_time is at or before it is under way or over, and its
 * quotes -- however fresh they were when taken -- are a match nobody can
 * back any more. A finished match's last odds are exactly what a surebet
 * scan would otherwise offer, because books stop moving them. An event
 * that names no start is kept and counted, and the scan says so.
 */
static gboolean
arb_event_open(
	ArbWalk			*walk,
	VentureSeriesStore	*store,
	const gchar		*key
){
	gint64 starts;

	if (!arb_commence_time(store, key, 0, &starts))
	{
		walk->undated++;
		return TRUE;
	}

	if (starts <= walk->now)
	{
		walk->started++;
		return FALSE;
	}

	return TRUE;
}

static gboolean
arb_cover_event(
	ArbWalk			 *walk,
	gint64			  source_id,
	VentureSeriesStore	 *store,
	const gchar		 *event,
	GError			**error
){
	g_autoptr(GPtrArray) quotes = NULL;
	g_autoptr(GPtrArray) outcomes = NULL;
	g_autoptr(GPtrArray) best = NULL;
	g_autoptr(GPtrArray) missing = NULL;
	g_autoptr(GHashTable) venues = NULL;
	g_autoptr(VentureMoney) stake = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *event_name = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gdouble *odds = NULL;
	VentureArbitrageSurebet split;
	VentureArbitrageEvidence evidence;
	const gchar *currency;
	JsonObject *opportunity;
	JsonObject *narrow;
	JsonArray *array;
	JsonArray *legs;
	gint64 oldest;
	gint64 interval;
	guint i;
	guint j;

	if (!arb_event_open(walk, store, event))
		return TRUE;

	quotes = venture_series_store_list_quotes(store, NULL, event, error);

	if (NULL == quotes)
		return FALSE;

	/* The event's outcomes are its children with any quote at all: an
	 * outcome only some venue quotes is still an outcome to cover. */
	outcomes = g_ptr_array_new();

	for (i = 0; i < quotes->len; i++)
	{
		VentureSeriesQuoteRow *quote = g_ptr_array_index(quotes, i);

		if (!g_ptr_array_find_with_equal_func(outcomes, quote->instrument_key, g_str_equal, NULL))
			g_ptr_array_add(outcomes, quote->instrument_key);
	}

	if (outcomes->len < 2)
		return TRUE;

	if (outcomes->len > VENTURE_ARBITRAGE_MAX_OUTCOMES)
	{
		venture_arbitrage_scan_add_note(walk->scan, "An event with more outcomes than a surebet "
		                                            "covers was passed over.");
		return TRUE;
	}

	currency = arb_quotes_currency(walk, source_id, quotes);
	stake = arb_stake(walk, currency);

	if (NULL == stake)
		return TRUE;

	currency = venture_money_get_currency(stake);
	best = g_ptr_array_new();
	missing = g_ptr_array_new_with_free_func(g_free);
	venues = g_hash_table_new(g_str_hash, g_str_equal);
	odds = g_new0(gdouble, outcomes->len);
	oldest = walk->now;
	interval = 0;

	/* The best back odds for each outcome, net of the venue's
	 * commission, at a venue that takes the stake's currency. */
	for (i = 0; i < outcomes->len; i++)
	{
		const gchar *outcome = g_ptr_array_index(outcomes, i);
		VentureSeriesQuoteRow *chosen = NULL;
		gdouble chosen_odds = 0.0;

		for (j = 0; j < quotes->len; j++)
		{
			VentureSeriesQuoteRow *quote = g_ptr_array_index(quotes, j);
			gdouble commission;
			gdouble effective;

			g_hash_table_add(venues, quote->venue_key);

			if ((0 != g_strcmp0(quote->instrument_key, outcome)) ||
			    (VENTURE_SERIES_QUOTE_BACK != quote->side) || ('\0' != quote->currency[0]) ||
			    !venture_arbitrage_scan_venue_allowed(walk->scan, TRUE, quote->venue_key) ||
			    !arb_takes_currency(walk, source_id, quote->venue_key, currency) ||
			    !arb_fresh(walk, quote->taken_at))
				continue;

			if (!arb_commission(walk, source_id, quote->venue_key, stake, &commission, &local_error))
			{
				g_ptr_array_add(missing, g_strdup_printf("fees: %s", local_error->message));
				g_clear_error(&local_error);
				continue;
			}

			effective = venture_arbitrage_effective_back_odds(
				(gdouble)quote->value / (gdouble)VENTURE_SERIES_ODDS_SCALE, commission);

			if (isfinite(effective) && (effective > chosen_odds))
			{
				chosen = quote;
				chosen_odds = effective;
			}
		}

		if (NULL == chosen)
		{
			g_autofree gchar *name = arb_instrument_name(store, outcome);

			g_ptr_array_add(missing, g_strdup_printf("%s: no allowed, fresh back odds", name));
			g_ptr_array_add(best, NULL);
			continue;
		}

		g_ptr_array_add(best, chosen);
		odds[i] = chosen_odds;
		oldest = MIN(oldest, chosen->taken_at);
		interval = MAX(interval, arb_interval(walk, store, source_id, chosen->venue_key));
	}

	event_name = arb_instrument_name(store, event);
	key = g_strdup_printf("cover:%" G_GINT64_FORMAT ":%s", source_id, event);
	title = g_strdup_printf("%s: every outcome covered", event_name);
	opportunity = json_object_new();
	json_object_set_string_member(opportunity, "strategy", "cover");
	json_object_set_string_member(opportunity, "key", key);
	json_object_set_string_member(opportunity, "title", title);
	json_object_set_string_member(opportunity, "instrument_key", event);
	json_object_set_string_member(opportunity, "instrument_name", event_name);
	json_object_set_int_member(opportunity, "data_source_id", source_id);
	json_object_set_string_member(opportunity, "currency", currency);
	json_object_set_int_member(opportunity, "age_seconds", MAX((gint64)0, walk->now - oldest));
	json_object_set_int_member(opportunity, "venues", g_hash_table_size(venues));
	narrow = json_object_new();
	json_object_set_string_member(narrow, "instrument", event);
	json_object_set_object_member(opportunity, "narrow", narrow);
	legs = json_array_new();
	array = json_array_new();
	memset(&split, 0, sizeof(split));

	if ((0 == missing->len) && !venture_arbitrage_surebet(odds, outcomes->len, stake, &split, error))
	{
		json_object_unref(opportunity);
		json_array_unref(legs);
		json_array_unref(array);
		return FALSE;
	}

	for (i = 0; i < outcomes->len; i++)
	{
		VentureSeriesQuoteRow *chosen = g_ptr_array_index(best, i);
		const gchar *outcome = g_ptr_array_index(outcomes, i);
		g_autofree gchar *name = arb_instrument_name(store, outcome);
		JsonObject *line = json_object_new();

		json_object_set_string_member(line, "instrument_key", outcome);
		json_object_set_string_member(line, "name", name);

		if (NULL != chosen)
		{
			json_object_set_string_member(line, "venue_key", chosen->venue_key);
			json_object_set_string_member(line, "venue_name",
			                              (NULL != chosen->venue_name) ? chosen->venue_name
			                                                            : chosen->venue_key);
			json_object_set_double_member(line, "odds",
			                              (gdouble)chosen->value / (gdouble)VENTURE_SERIES_ODDS_SCALE);
			json_object_set_double_member(line, "effective_odds", odds[i]);
		}

		if (NULL != split.stakes)
		{
			VentureMoney *amount = g_ptr_array_index(split.stakes, i);
			JsonObject *leg = json_object_new();
			g_autofree gchar *text = venture_money_to_string(amount);

			venture_arbitrage_set_money(line, "stake", amount);
			venture_arbitrage_set_money(line, "payout", g_ptr_array_index(split.payouts, i));
			json_object_set_string_member(leg, "kind", "stake");
			json_object_set_int_member(leg, "data_source_id", source_id);
			json_object_set_string_member(leg, "venue_key", chosen->venue_key);
			json_object_set_string_member(leg, "instrument_key", outcome);
			json_object_set_string_member(leg, "amount", text);
			json_array_add_object_element(legs, leg);
		}

		json_array_add_object_element(array, line);
	}

	json_object_set_array_member(opportunity, "outcomes", array);
	json_object_set_array_member(opportunity, "legs", legs);

	if (missing->len > 0)
	{
		array = json_array_new();

		for (i = 0; i < missing->len; i++)
			json_array_add_string_element(array, g_ptr_array_index(missing, i));

		json_object_set_array_member(opportunity, "missing", array);
		json_object_set_null_member(opportunity, "net");
		json_object_set_null_member(opportunity, "capital");
		json_object_set_null_member(opportunity, "roi");
	}
	else
	{
		gdouble roi;

		/* What the rounded stakes are sure to make is the profit; the
		 * ideal T(1/S - 1) is shown beside it. */
		venture_arbitrage_set_money(opportunity, "net", split.profit);
		venture_arbitrage_set_money(opportunity, "capital", split.staked);
		venture_arbitrage_set_money(opportunity, "ideal_profit", split.profit_ideal);
		venture_arbitrage_set_money(opportunity, "residual", split.residual);
		venture_arbitrage_set_money(opportunity, "payout", split.payout);
		venture_arbitrage_set_money(opportunity, "payout_ideal", split.payout_ideal);
		json_object_set_double_member(opportunity, "sum", split.sum);
		json_object_set_double_member(opportunity, "overround", split.overround);
		json_object_set_boolean_member(opportunity, "is_surebet", split.is_surebet);

		if (venture_arbitrage_roi(split.profit, split.staked, &roi, NULL))
			venture_arbitrage_set_ratio(opportunity, "roi", roi);

		/* Both sides are quoted now: it fills, or it does not exist. */
		venture_arbitrage_set_money(opportunity, "ev", split.profit);
	}

	venture_arbitrage_surebet_clear(&split);

	evidence.age_seconds = MAX((gint64)0, walk->now - oldest);
	evidence.interval_seconds = interval;
	evidence.venues = g_hash_table_size(venues);
	evidence.dispersion = NAN;
	evidence.depth = NAN;
	venture_arbitrage_set_ratio(opportunity, "confidence", venture_arbitrage_confidence(&evidence));
	json_object_set_array_member(opportunity, "warnings", json_array_new());
	venture_arbitrage_scan_add(walk->scan, opportunity);

	return TRUE;
}

/* Instruments (or events) to walk in one source: the one asked for, or
 * the most recently quoted. */
static GPtrArray *
arb_quoted(
	ArbWalk			 *walk,
	VentureSeriesStore	 *store,
	gboolean		  parents,
	GError			**error
){
	GPtrArray *keys;
	gint64 since;

	if (!venture_string_is_empty(walk->instrument))
	{
		keys = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(keys, g_strdup(walk->instrument));
		return keys;
	}

	since = (walk->max_age > 0) ? (walk->now - walk->max_age) : 0;
	keys = parents
		? venture_series_store_list_quoted_parents(store, since, VENTURE_ARBITRAGE_SCAN_CANDIDATES, error)
		: venture_series_store_list_quoted_instruments(store, VENTURE_SERIES_QUOTE_LAY, since,
		                                               VENTURE_ARBITRAGE_SCAN_CANDIDATES, error);

	if ((NULL != keys) && (keys->len >= VENTURE_ARBITRAGE_SCAN_CANDIDATES))
		venture_arbitrage_scan_add_note(walk->scan, "Only the most recently quoted 500 events per "
		                                            "source were considered; name one with "
		                                            "instrument, or bound max_age_hours.");

	return keys;
}

static gboolean
arb_cover_scan(
	VentureArbitrageScan	 *scan,
	gpointer		  user_data,
	GError			**error
){
	GPtrArray *sources;
	ArbWalk walk;
	gboolean ok;
	guint i;
	guint j;

	(void)user_data;

	arb_walk_init(&walk, scan, "min");
	sources = venture_arbitrage_scan_get_sources(scan);
	ok = TRUE;

	for (i = 0; ok && (i < sources->len); i++)
	{
		gint64 source_id = venture_entity_get_id(g_ptr_array_index(sources, i));
		g_autoptr(GPtrArray) events = NULL;
		VentureSeriesStore *store;

		store = venture_arbitrage_scan_open_store(scan, source_id);

		if (NULL == store)
			continue;

		events = arb_quoted(&walk, store, TRUE, error);
		ok = (NULL != events);

		for (j = 0; ok && (j < events->len); j++)
			ok = arb_cover_event(&walk, source_id, store, g_ptr_array_index(events, j), error);
	}

	arb_walk_clear(&walk);

	return ok;
}

static gboolean
arb_back_lay_instrument(
	ArbWalk			 *walk,
	gint64			  source_id,
	VentureSeriesStore	 *store,
	const gchar		 *instrument,
	GError			**error
){
	g_autoptr(GPtrArray) quotes = NULL;
	g_autoptr(VentureMoney) stake = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *title = NULL;
	VentureSeriesQuoteRow *back;
	VentureSeriesQuoteRow *lay;
	VentureArbitrageBackLay figures;
	VentureArbitrageEvidence evidence;
	JsonObject *opportunity;
	JsonObject *narrow;
	JsonArray *legs;
	const gchar *currency;
	gdouble back_odds;
	gdouble lay_odds;
	gdouble commission;
	gdouble roi;
	guint i;

	if (!arb_event_open(walk, store, instrument))
		return TRUE;

	quotes = venture_series_store_list_quotes(store, instrument, NULL, error);

	if (NULL == quotes)
		return FALSE;

	currency = arb_quotes_currency(walk, source_id, quotes);
	stake = arb_stake(walk, currency);

	if (NULL == stake)
		return TRUE;

	currency = venture_money_get_currency(stake);
	back = NULL;
	lay = NULL;
	back_odds = 0.0;
	lay_odds = G_MAXDOUBLE;
	commission = 0.0;

	/* The best back (net of its venue's commission) and the cheapest lay
	 * (with the exchange's commission kept for the stake arithmetic). */
	for (i = 0; i < quotes->len; i++)
	{
		VentureSeriesQuoteRow *quote = g_ptr_array_index(quotes, i);
		gdouble value = (gdouble)quote->value / (gdouble)VENTURE_SERIES_ODDS_SCALE;
		gdouble rate;

		if (('\0' != quote->currency[0]) || !arb_fresh(walk, quote->taken_at) ||
		    !arb_takes_currency(walk, source_id, quote->venue_key, currency))
			continue;

		if ((VENTURE_SERIES_QUOTE_BACK == quote->side) &&
		    venture_arbitrage_scan_venue_allowed(walk->scan, TRUE, quote->venue_key))
		{
			gdouble effective;

			if (!arb_commission(walk, source_id, quote->venue_key, stake, &rate, NULL))
				continue;

			effective = venture_arbitrage_effective_back_odds(value, rate);

			if (isfinite(effective) && (effective > back_odds))
			{
				back = quote;
				back_odds = effective;
			}
		}
		else if ((VENTURE_SERIES_QUOTE_LAY == quote->side) &&
		         venture_arbitrage_scan_venue_allowed(walk->scan, FALSE, quote->venue_key))
		{
			if (!arb_commission(walk, source_id, quote->venue_key, stake, &rate, NULL))
				continue;

			/* Cheaper once its commission is counted. */
			if (venture_arbitrage_effective_lay_odds(value, rate) <
			    venture_arbitrage_effective_lay_odds(lay_odds, commission))
			{
				lay = quote;
				lay_odds = value;
				commission = rate;
			}
		}
	}

	if ((NULL == back) || (NULL == lay) || (0 == g_strcmp0(back->venue_key, lay->venue_key)))
		return TRUE;

	if (!venture_arbitrage_back_lay(back_odds, lay_odds, commission, stake, &figures, &local_error))
		return TRUE;

	name = arb_instrument_name(store, instrument);
	key = g_strdup_printf("back_lay:%" G_GINT64_FORMAT ":%s:%s>%s", source_id, instrument,
	                      back->venue_key, lay->venue_key);
	title = g_strdup_printf("%s: back at %s, lay at %s", name,
	                        (NULL != back->venue_name) ? back->venue_name : back->venue_key,
	                        (NULL != lay->venue_name) ? lay->venue_name : lay->venue_key);
	opportunity = json_object_new();
	json_object_set_string_member(opportunity, "strategy", "back_lay");
	json_object_set_string_member(opportunity, "key", key);
	json_object_set_string_member(opportunity, "title", title);
	json_object_set_string_member(opportunity, "instrument_key", instrument);
	json_object_set_string_member(opportunity, "instrument_name", name);
	json_object_set_int_member(opportunity, "data_source_id", source_id);
	json_object_set_string_member(opportunity, "currency", currency);
	json_object_set_double_member(opportunity, "back_odds", back_odds);
	json_object_set_double_member(opportunity, "lay_odds", lay_odds);
	json_object_set_double_member(opportunity, "commission", commission);
	json_object_set_double_member(opportunity, "rating", figures.rating);
	venture_arbitrage_set_money(opportunity, "back_stake", stake);
	venture_arbitrage_set_money(opportunity, "lay_stake", figures.lay_stake);
	venture_arbitrage_set_money(opportunity, "liability", figures.liability);
	venture_arbitrage_set_money(opportunity, "if_back_wins", figures.if_back_wins);
	venture_arbitrage_set_money(opportunity, "if_lay_wins", figures.if_lay_wins);
	venture_arbitrage_set_money(opportunity, "ideal_profit", figures.ideal);
	venture_arbitrage_set_money(opportunity, "net", figures.worst);
	venture_arbitrage_set_money(opportunity, "ev", figures.worst);
	json_object_set_int_member(opportunity, "age_seconds",
	                           MAX((gint64)0, walk->now - MIN(back->taken_at, lay->taken_at)));

	{
		g_autoptr(VentureMoney) capital = venture_money_add(stake, figures.liability, NULL);

		venture_arbitrage_set_money(opportunity, "capital", capital);

		if ((NULL != capital) && venture_arbitrage_roi(figures.worst, capital, &roi, NULL))
			venture_arbitrage_set_ratio(opportunity, "roi", roi);
	}

	narrow = json_object_new();
	json_object_set_string_member(narrow, "instrument", instrument);
	json_object_set_object_member(opportunity, "narrow", narrow);

	/* The back stake at the bookmaker, the liability put up at the
	 * exchange: both leave the venue's cash when placed. */
	legs = json_array_new();

	{
		JsonObject *leg = json_object_new();
		g_autofree gchar *text = venture_money_to_string(stake);

		json_object_set_string_member(leg, "kind", "stake");
		json_object_set_int_member(leg, "data_source_id", source_id);
		json_object_set_string_member(leg, "venue_key", back->venue_key);
		json_object_set_string_member(leg, "instrument_key", instrument);
		json_object_set_string_member(leg, "amount", text);
		json_object_set_string_member(leg, "notes", "Back");
		json_array_add_object_element(legs, leg);
	}

	{
		JsonObject *leg = json_object_new();
		g_autofree gchar *text = venture_money_to_string(figures.liability);
		g_autofree gchar *notes = g_strdup_printf("Lay: liability for a lay stake of %s",
		                                          venture_money_to_string(figures.lay_stake));

		json_object_set_string_member(leg, "kind", "stake");
		json_object_set_int_member(leg, "data_source_id", source_id);
		json_object_set_string_member(leg, "venue_key", lay->venue_key);
		json_object_set_string_member(leg, "instrument_key", instrument);
		json_object_set_string_member(leg, "amount", text);
		json_object_set_string_member(leg, "notes", notes);
		json_array_add_object_element(legs, leg);
	}

	json_object_set_array_member(opportunity, "legs", legs);

	evidence.age_seconds = MAX((gint64)0, walk->now - MIN(back->taken_at, lay->taken_at));
	evidence.interval_seconds = MAX(arb_interval(walk, store, source_id, back->venue_key),
	                                arb_interval(walk, store, source_id, lay->venue_key));
	evidence.venues = 2;
	evidence.dispersion = NAN;
	evidence.depth = NAN;
	venture_arbitrage_set_ratio(opportunity, "confidence", venture_arbitrage_confidence(&evidence));
	json_object_set_array_member(opportunity, "warnings", json_array_new());
	venture_arbitrage_back_lay_clear(&figures);
	venture_arbitrage_scan_add(walk->scan, opportunity);

	return TRUE;
}

static gboolean
arb_back_lay_scan(
	VentureArbitrageScan	 *scan,
	gpointer		  user_data,
	GError			**error
){
	GPtrArray *sources;
	ArbWalk walk;
	gboolean ok;
	guint i;
	guint j;

	(void)user_data;

	arb_walk_init(&walk, scan, "min");
	sources = venture_arbitrage_scan_get_sources(scan);
	ok = TRUE;

	for (i = 0; ok && (i < sources->len); i++)
	{
		gint64 source_id = venture_entity_get_id(g_ptr_array_index(sources, i));
		g_autoptr(GPtrArray) instruments = NULL;
		VentureSeriesStore *store;

		store = venture_arbitrage_scan_open_store(scan, source_id);

		if (NULL == store)
			continue;

		instruments = arb_quoted(&walk, store, FALSE, error);
		ok = (NULL != instruments);

		for (j = 0; ok && (j < instruments->len); j++)
			ok = arb_back_lay_instrument(&walk, source_id, store, g_ptr_array_index(instruments, j),
			                             error);
	}

	arb_walk_clear(&walk);

	return ok;
}

#else /* !VENTURE_HAVE_SQLITE */

/* Without a series store there is nothing to scan; the driver says so
 * before any strategy runs. */
static gboolean
arb_nothing(
	VentureArbitrageScan	 *scan,
	gpointer		  user_data,
	GError			**error
){
	(void)scan;
	(void)user_data;
	(void)error;

	return TRUE;
}

#define arb_spread_scan arb_nothing
#define arb_deal_scan arb_nothing
#define arb_transform_scan arb_nothing
#define arb_cover_scan arb_nothing
#define arb_back_lay_scan arb_nothing

#endif /* VENTURE_HAVE_SQLITE */

void
venture_arbitrage_register_builtin_strategies(VentureArbitrageStrategyRegistry *registry)
{
	venture_arbitrage_strategy_registry_add(registry, "spread", "Spread",
		"Buy where an instrument is cheap and sell where it is dear: cross-venue flips, drop "
		"shipping (buy_sources lists the cheapest suppliers), retail and crypto spreads",
		NULL, arb_spread_scan, NULL, NULL, NULL, NULL);
	venture_arbitrage_strategy_registry_add(registry, "transform", "Crafting",
		"A recipe's inputs bought where each is cheapest, walking each book for the units "
		"needed, against the output's best venue",
		NULL, arb_transform_scan, NULL, NULL, NULL, NULL);
	venture_arbitrage_strategy_registry_add(registry, "deal", "Deals",
		"A venue's price at or under its group's deal price, sold back at the region's figure",
		NULL, arb_deal_scan, NULL, NULL, NULL, NULL);
	venture_arbitrage_strategy_registry_add(registry, "cover", "Surebets",
		"Every outcome of an event backed at the venue with the best odds, so whichever "
		"happens pays more than the stakes",
		NULL, arb_cover_scan, NULL, NULL, NULL, NULL);
	venture_arbitrage_strategy_registry_add(registry, "back_lay", "Back and lay",
		"A back at one venue matched by a lay of the same outcome at an exchange",
		NULL, arb_back_lay_scan, NULL, NULL, NULL, NULL);
}

/* ==========================================================================
 * Trends, for the pages' sparklines
 * ========================================================================== */

void
venture_arbitrage_add_trends(
	VentureContext	*context,
	JsonArray	*rows
){
#ifdef VENTURE_HAVE_SQLITE
	g_autoptr(GHashTable) stores = NULL;
	VentureFeedsService *service;
	gint64 now;
	guint i;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	service = venture_context_get_feeds_service(context);

	if ((NULL == service) || (NULL == rows))
		return;

	stores = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_object_unref);
	now = g_get_real_time() / G_USEC_PER_SEC;

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonObject *buy;
		g_autoptr(GArray) days = NULL;
		VentureSeriesStore *store;
		const gchar *venue;
		const gchar *instrument;
		gint64 source_id;
		JsonArray *trend;
		guint j;

		buy = json_object_has_member(row, "buy") ? json_object_get_object_member(row, "buy") : NULL;
		instrument = venture_json_object_get_string(row, "instrument_key", NULL);

		if ((NULL == buy) || (NULL == instrument))
			continue;

		source_id = venture_json_object_get_int(buy, "data_source_id", 0);
		venue = venture_json_object_get_string(buy, "venue_key", NULL);
		store = g_hash_table_lookup(stores, &source_id);

		if (NULL == store)
		{
			store = venture_feeds_service_open_reader(service, source_id, NULL);

			if (NULL == store)
				continue;

			g_hash_table_insert(stores, g_memdup2(&source_id, sizeof(source_id)), store);
		}

		days = venture_series_store_daily(store, venue, instrument, now - 14 * 86400, NULL);

		if (NULL == days)
			continue;

		trend = json_array_new();

		/* The buy venue's lowest price each day, in the row's
		 * currency only: a day in another reads as a gap. */
		for (j = 0; j < days->len; j++)
		{
			VentureSeriesDay *day = &g_array_index(days, VentureSeriesDay, j);

			if ((VENTURE_SERIES_NONE == day->min_price) ||
			    (0 != g_strcmp0(day->currency, venture_json_object_get_string(buy, "currency",
			                                                                   day->currency))))
				json_array_add_null_element(trend);
			else
				json_array_add_int_element(trend, day->min_price);
		}

		json_object_set_array_member(row, "trend", trend);

		if (days->len > 0)
			json_object_set_string_member(row, "trend_currency",
			                              g_array_index(days, VentureSeriesDay, days->len - 1).currency);
	}
#else
	(void)context;
	(void)rows;
#endif
}
