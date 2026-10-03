/*
 * venture-series-math.h - The arithmetic behind market-data series
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure functions over plain arrays: no database, no configuration, no
 * GObject. The series store calls them while it ingests a snapshot on a
 * worker thread, and the tests call them directly, so every formula a page
 * or the price oracle shows is defined once, here, and pinned by
 * tests/test-series-math.c. docs/market-data.org writes each one out.
 *
 * Prices are integer minor units and never a double. Quantities can be
 * enormous (a commodity with eleven million units on one venue is
 * ordinary), so every sum is checked: a figure that would not fit is
 * refused with a named error rather than wrapped into a plausible wrong
 * number. Doubles appear only where the answer is a ratio or a
 * statistical threshold (a standard deviation compared against a spread),
 * and a price derived through one is rounded back to minor units exactly
 * once.
 */

#ifndef VENTURE_SERIES_MATH_H
#define VENTURE_SERIES_MATH_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_SERIES_NONE:
 *
 * The value of a figure that does not exist: no listing, so no minimum;
 * no sale, so no average sale price. Every price is at least zero, so the
 * most negative 64-bit integer can never be mistaken for one -- which is
 * why it is used instead of zero, and zero stays a real price.
 */
#define VENTURE_SERIES_NONE (G_MININT64)

/**
 * VENTURE_SERIES_MAX_TIERS:
 *
 * How many of the cheapest distinct prices a snapshot keeps per instrument
 * and venue for bulk pricing. Beyond it a buyer is walking a book deeper
 * than anyone prices from, and the bulk calculator says it ran out rather
 * than guessing.
 */
#define VENTURE_SERIES_MAX_TIERS (64)

/**
 * VENTURE_SERIES_EWMA_DAYS:
 *
 * The window of the 14-day market value: today and the thirteen days
 * before it.
 */
#define VENTURE_SERIES_EWMA_DAYS (14)

/**
 * VENTURE_SERIES_EWMA_SCALE:
 *
 * The integer weights of the 14-day average are 0.5^(d / 2.1) scaled by
 * this and rounded, so the weighted mean is exact integer arithmetic.
 */
#define VENTURE_SERIES_EWMA_SCALE (1000000)

/**
 * VENTURE_SERIES_HISTORICAL_DAYS:
 *
 * The window of the historical value: an unweighted mean of the daily
 * market values over this many days.
 */
#define VENTURE_SERIES_HISTORICAL_DAYS (60)

/**
 * VENTURE_SERIES_GAP_HISTORY:
 *
 * How many of a venue's most recent update gaps are kept to learn its
 * interval.
 */
#define VENTURE_SERIES_GAP_HISTORY (36)

/**
 * VENTURE_SERIES_INTERVAL_DEFAULT:
 *
 * The interval assumed, in seconds, for a venue with no history.
 */
#define VENTURE_SERIES_INTERVAL_DEFAULT (3600)

/**
 * VENTURE_SERIES_INTERVAL_CAP:
 *
 * The longest learned interval, in seconds. A venue that went quiet for a
 * day has not changed its cadence; it has had an outage.
 */
#define VENTURE_SERIES_INTERVAL_CAP (7200)

/**
 * VENTURE_SERIES_DEAL_MIN_VENUES:
 *
 * From this many venues listing an instrument, its deal price is the
 * venue a third of the way up the sorted minimums rather than the median.
 */
#define VENTURE_SERIES_DEAL_MIN_VENUES (15)

/**
 * VentureSeriesTier:
 * @price: a unit price in minor units, at least zero
 * @quantity: the units offered at exactly that price, at least one
 *
 * One rung of an order book: every unit offered at one price.
 */
typedef struct
{
	gint64	price;
	gint64	quantity;
} VentureSeriesTier;

/**
 * VentureSeriesSummary:
 * @units: the total quantity
 * @tiers: the number of distinct prices
 * @min: the lowest price
 * @max: the highest price
 * @market_value: the TSM-style market value, see
 *   venture_series_math_market_value()
 * @median: the median unit price
 * @p15: the price of the unit at the 15th percentile, by nearest rank
 * @mean: the quantity-weighted mean price
 * @stddev: the quantity-weighted population standard deviation, rounded to
 *   minor units
 *
 * What one instrument's listings at one venue add up to. Every member is
 * %VENTURE_SERIES_NONE when there were no units.
 */
typedef struct
{
	gint64	units;
	gint64	tiers;
	gint64	min;
	gint64	max;
	gint64	market_value;
	gint64	median;
	gint64	p15;
	gint64	mean;
	gint64	stddev;
} VentureSeriesSummary;

/**
 * VentureSeriesBulkCost:
 * @requested: the units asked for
 * @filled: the units the tiers could supply, at most @requested
 * @cost: what @filled units cost, in minor units
 * @worst_price: the price of the last unit bought, or %VENTURE_SERIES_NONE
 * @average: @cost / @filled rounded half to even, or %VENTURE_SERIES_NONE
 * @complete: whether @filled equals @requested
 *
 * The cost of buying a number of units by walking a book from its
 * cheapest tier.
 */
typedef struct
{
	gint64		requested;
	gint64		filled;
	gint64		cost;
	gint64		worst_price;
	gint64		average;
	gboolean	complete;
} VentureSeriesBulkCost;

/**
 * VentureSeriesListingMark:
 * @id: the listing's identifier, never zero
 * @instrument: which instrument the listing is for (an opaque number the
 *   caller chose; the store uses its row id)
 * @price: the unit price in minor units
 * @quantity: the units listed
 * @expires_in_min: the least time, in seconds, the listing had left when
 *   it was seen; -1 when the source does not say
 *
 * One listing as the sale estimate remembers it between snapshots.
 */
typedef struct
{
	guint64	id;
	gint64	instrument;
	gint64	price;
	gint64	quantity;
	gint64	expires_in_min;
} VentureSeriesListingMark;

/**
 * VentureSeriesSale:
 * @instrument: the instrument, as in #VentureSeriesListingMark
 * @sold_units: units estimated sold
 * @sold_value: what those units sold for, in minor units
 * @expired_units: units that vanished when they may simply have expired
 *
 * The sale estimate for one instrument across two snapshots.
 */
typedef struct
{
	gint64	instrument;
	gint64	sold_units;
	gint64	sold_value;
	gint64	expired_units;
} VentureSeriesSale;

/**
 * VentureSeriesHeat:
 * @value: the mean of the values that fell in each weekday (0 is Monday)
 *   and hour, rounded half to even; %VENTURE_SERIES_NONE where none fell
 * @count: how many values fell in each cell
 *
 * A weekday by hour matrix, read in one zone.
 */
typedef struct
{
	gint64	value[7][24];
	guint32	count[7][24];
} VentureSeriesHeat;

/**
 * venture_series_math_add:
 * @a: an addend
 * @b: an addend
 * @out: (out): the sum
 *
 * Adds two 64-bit integers, refusing an overflow.
 *
 * Returns: %FALSE (and leaves @out alone) if the sum would not fit
 */
gboolean
venture_series_math_add(
	gint64	 a,
	gint64	 b,
	gint64	*out
);

/**
 * venture_series_math_mul:
 * @a: a factor
 * @b: a factor
 * @out: (out): the product
 *
 * Multiplies two 64-bit integers, refusing an overflow.
 *
 * Returns: %FALSE (and leaves @out alone) if the product would not fit
 */
gboolean
venture_series_math_mul(
	gint64	 a,
	gint64	 b,
	gint64	*out
);

/**
 * venture_series_math_normalise_tiers:
 * @tiers: (element-type VentureSeriesTier): tiers in any order, possibly
 *   repeating prices
 * @error: (out) (optional): return location for a #GError
 *
 * Sorts @tiers by price and merges tiers at the same price, in place. A
 * negative price, a quantity below one, or a merged quantity past the
 * 64-bit range is refused with %VENTURE_ERROR_INVALID_ARGUMENT naming the
 * problem, and @tiers is then left in an unspecified order.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_math_normalise_tiers(
	GArray	 *tiers,
	GError	**error
);

/**
 * venture_series_math_summarise:
 * @tiers: (array length=n_tiers): tiers sorted by price, distinct prices,
 *   as venture_series_math_normalise_tiers() leaves them
 * @n_tiers: the number of tiers
 * @out: (out caller-allocates): the summary
 * @error: (out) (optional): return location for a #GError
 *
 * Computes every figure of #VentureSeriesSummary in one pass over the
 * book. No tiers is not an error: every member is %VENTURE_SERIES_NONE.
 * A total past the 64-bit range is refused with
 * %VENTURE_ERROR_INVALID_ARGUMENT.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_math_summarise(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	VentureSeriesSummary	 *out,
	GError			**error
);

/**
 * venture_series_math_market_value:
 * @tiers: (array length=n_tiers): tiers sorted by price, distinct prices
 * @n_tiers: the number of tiers
 * @error: (out) (optional): return location for a #GError
 *
 * The market value of a book, the way TradeSkillMaster's AuctionDB
 * computes one snapshot's:
 *
 * 1. Expand the book into units, cheapest first, and let Q be their
 *    number.
 * 2. Take the first max(1, ceil(0.15 Q)) units unconditionally.
 * 3. Keep taking, up to floor(0.30 Q) units, until a unit's price is at
 *    least 20% above the unit before it; that unit and everything after
 *    it are left out. A jump inside the first 15% cuts nothing.
 * 4. Over the units taken, compute the mean m and the population standard
 *    deviation s, and drop every unit whose price p has |p - m| > 1.5 s.
 * 5. The market value is the mean of the units left, rounded half to even.
 *
 * Returns: the market value, or %VENTURE_SERIES_NONE for an empty book or
 *   an overflow (which also sets @error)
 */
gint64
venture_series_math_market_value(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	GError			**error
);

/**
 * venture_series_math_percentile:
 * @tiers: (array length=n_tiers): tiers sorted by price, distinct prices
 * @n_tiers: the number of tiers
 * @percent: a percentile, 1 to 100
 *
 * The price of the unit at the nearest rank ceil(@percent / 100 * Q) of
 * the Q units of the book, counted from the cheapest.
 *
 * Returns: that price, or %VENTURE_SERIES_NONE for an empty book
 */
gint64
venture_series_math_percentile(
	const VentureSeriesTier	*tiers,
	gsize			 n_tiers,
	guint			 percent
);

/**
 * venture_series_math_median:
 * @values: (array length=n_values): values in any order; not modified
 * @n_values: the number of values
 *
 * The median of plain values, %VENTURE_SERIES_NONE entries skipped. With
 * an even count it is the mean of the middle two, rounded half to even.
 *
 * Returns: the median, or %VENTURE_SERIES_NONE when there is no value
 */
gint64
venture_series_math_median(
	const gint64	*values,
	gsize		 n_values
);

/**
 * venture_series_math_p33:
 * @values: (array length=n_values): values in any order; not modified
 * @n_values: the number of values
 *
 * The value a third of the way up: with the values sorted ascending and
 * counted from zero, the one at index floor(n / 3). %VENTURE_SERIES_NONE
 * entries are skipped.
 *
 * Returns: that value, or %VENTURE_SERIES_NONE when there is no value
 */
gint64
venture_series_math_p33(
	const gint64	*values,
	gsize		 n_values
);

/**
 * venture_series_math_deal_price:
 * @values: (array length=n_values): every venue's minimum price for one
 *   instrument; not modified
 * @n_values: the number of values
 *
 * The price a buyer should call a deal, as Undermine Exchange sets it:
 * the median of the venues' minimums, or venture_series_math_p33() of them
 * when at least %VENTURE_SERIES_DEAL_MIN_VENUES venues list the
 * instrument -- with that many, a third of the market being cheaper is a
 * fact about the market rather than about one venue.
 *
 * Returns: the deal price, or %VENTURE_SERIES_NONE when no venue lists it
 */
gint64
venture_series_math_deal_price(
	const gint64	*values,
	gsize		 n_values
);

/**
 * venture_series_math_ewma_weight:
 * @days_ago: 0 for today
 *
 * The integer weight of a day in the 14-day market value:
 * round(%VENTURE_SERIES_EWMA_SCALE * 0.5^(@days_ago / 2.1)). The table is
 * fixed (1000000, 718873, 516779, 371499, 267060, 191983, 138011, 99213,
 * 71321, 51271, 36857, 26496, 19047, 13692) so the figure is the same on
 * every machine.
 *
 * Returns: the weight, or 0 at or past %VENTURE_SERIES_EWMA_DAYS
 */
guint32
venture_series_math_ewma_weight(guint days_ago);

/**
 * venture_series_math_ewma:
 * @values: (array length=n_values): daily values, index 0 being today and
 *   index d being d days ago; %VENTURE_SERIES_NONE for a day with none
 * @n_values: the number of values
 * @error: (out) (optional): return location for a #GError
 *
 * The 14-day weighted market value: sum(w_d v_d) / sum(w_d) over the days
 * that have a value, with w_d from venture_series_math_ewma_weight(),
 * rounded half to even. A day missing is left out of both sums rather
 * than read as zero.
 *
 * Returns: the weighted mean, or %VENTURE_SERIES_NONE when no day in the
 *   window has a value or the sums overflow (which also sets @error)
 */
gint64
venture_series_math_ewma(
	const gint64	 *values,
	gsize		  n_values,
	GError		**error
);

/**
 * venture_series_math_mean:
 * @values: (array length=n_values): values; %VENTURE_SERIES_NONE skipped
 * @n_values: the number of values
 * @error: (out) (optional): return location for a #GError
 *
 * The unweighted mean, rounded half to even.
 *
 * Returns: the mean, or %VENTURE_SERIES_NONE when there is no value or the
 *   sum overflows (which also sets @error)
 */
gint64
venture_series_math_mean(
	const gint64	 *values,
	gsize		  n_values,
	GError		**error
);

/**
 * venture_series_math_bulk_cost:
 * @tiers: (array length=n_tiers): tiers sorted by price
 * @n_tiers: the number of tiers
 * @units: the units to buy, at least one
 * @out: (out caller-allocates): the cost
 * @error: (out) (optional): return location for a #GError
 *
 * Walks the tiers from the cheapest, buying until @units are bought or
 * the tiers run out. Running out is not an error: @out says how many
 * were filled. A cost past the 64-bit range is refused with
 * %VENTURE_ERROR_INVALID_ARGUMENT.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_series_math_bulk_cost(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	gint64			  units,
	VentureSeriesBulkCost	 *out,
	GError			**error
);

/**
 * venture_series_math_sale_estimate:
 * @previous: (array length=n_previous): the earlier snapshot's listings,
 *   sorted by id with no id repeated
 * @n_previous: the number of earlier listings
 * @current: (array length=n_current): the later snapshot's listings,
 *   sorted by id with no id repeated
 * @n_current: the number of later listings
 * @elapsed: seconds between the two snapshots
 * @error: (out) (optional): return location for a #GError
 *
 * Estimates what sold between two snapshots of one venue by diffing the
 * listing ids:
 *
 * - A listing that vanished, and could not have expired -- its
 *   @expires_in_min exceeds @elapsed, or the source gives no expiry at all
 *   (-1) -- counts as sold, all of its quantity at its price.
 * - A listing that vanished and might have expired counts as expired.
 * - A listing still there with less quantity sold the difference.
 *
 * A cancelled listing looks exactly like a sale; that is the estimate's
 * known bias, and why it is called an estimate.
 *
 * Returns: (transfer full) (element-type VentureSeriesSale) (nullable): one
 *   entry per instrument that sold or expired anything, sorted by
 *   instrument; %NULL with @error set when a value overflows
 */
GArray *
venture_series_math_sale_estimate(
	const VentureSeriesListingMark	 *previous,
	gsize				  n_previous,
	const VentureSeriesListingMark	 *current,
	gsize				  n_current,
	gint64				  elapsed,
	GError				**error
);

/**
 * venture_series_math_heat:
 * @times: (array length=n): Unix times in seconds
 * @values: (array length=n): one value per time; %VENTURE_SERIES_NONE
 *   entries skipped
 * @n: the number of samples
 * @zone_offset: seconds east of UTC of the zone the matrix is read in
 * @out: (out caller-allocates): the matrix
 * @error: (out) (optional): return location for a #GError
 *
 * Buckets each sample by the weekday (Monday first) and hour it falls on
 * in the zone, and averages each cell, rounded half to even.
 *
 * Returns: %TRUE on success; %FALSE on an overflowing sum
 */
gboolean
venture_series_math_heat(
	const gint64		 *times,
	const gint64		 *values,
	gsize			  n,
	gint64			  zone_offset,
	VentureSeriesHeat	 *out,
	GError			**error
);

/**
 * venture_series_math_learn_interval:
 * @gaps: (array length=n_gaps): the seconds between consecutive updates,
 *   oldest first
 * @n_gaps: the number of gaps
 *
 * Learns how often a venue updates, the way Undermine Exchange schedules
 * its realms: the maximum of every window of three consecutive gaps (so
 * one early double update does not halve the estimate), then the median
 * of those maxima (so one outage does not double it), capped at
 * %VENTURE_SERIES_INTERVAL_CAP. With fewer than three gaps it is their
 * maximum; with none, %VENTURE_SERIES_INTERVAL_DEFAULT. Gaps of zero or
 * less are ignored.
 *
 * Returns: the interval in seconds, at least one
 */
gint64
venture_series_math_learn_interval(
	const gint64	*gaps,
	gsize		 n_gaps
);

/**
 * venture_series_math_div_round:
 * @numerator: at least zero
 * @denominator: at least one
 *
 * Divides, rounding half to even, the one rounding every derived price
 * here uses.
 *
 * Returns: the rounded quotient
 */
gint64
venture_series_math_div_round(
	gint64	numerator,
	gint64	denominator
);

G_END_DECLS

#endif /* VENTURE_SERIES_MATH_H */
