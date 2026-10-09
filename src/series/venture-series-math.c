/*
 * venture-series-math.c - The arithmetic behind market-data series
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See venture-series-math.h for the contract and docs/market-data.org for
 * the formulas written out. Everything here is a pure function of its
 * arguments, which is what lets the store call it from a worker thread.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

/* --- Wide accumulation --------------------------------------------------- */

/*
 * A sum of price times quantity overflows 64 bits long before either
 * factor does: a 30 000-gold item (300 000 000 copper) times a commodity
 * stack of forty million units is already past it. Both target
 * architectures (x86_64 and aarch64) give GCC a 128-bit integer, which
 * holds the product of any two 63-bit values and the sum of billions of
 * them. Where it does not exist the same code runs on checked 64-bit
 * arithmetic and refuses what would not fit, rather than wrapping.
 */
#ifdef __SIZEOF_INT128__
typedef unsigned __int128 SeriesWide;
#define SERIES_WIDE_MAX (~((SeriesWide)0))
#else
typedef guint64 SeriesWide;
#define SERIES_WIDE_MAX (G_MAXUINT64)
#endif

/*
 * Adds a * b to @acc. Both factors are at least zero. Returns FALSE when
 * the accumulator would overflow.
 */
static gboolean
series_wide_add_product(
	SeriesWide	*acc,
	guint64		 a,
	guint64		 b
){
#ifdef __SIZEOF_INT128__
	SeriesWide product;

	/* Two values below 2^64 multiply to below 2^128: never overflows. */
	product = (SeriesWide)a * (SeriesWide)b;
#else
	guint64 product;

	if (!g_uint64_checked_mul(&product, a, b))
		return FALSE;
#endif

	if (*acc > SERIES_WIDE_MAX - product)
		return FALSE;

	*acc += product;
	return TRUE;
}

/*
 * Divides @numerator by @denominator rounding half to even, and refuses a
 * quotient past the signed 64-bit range. The denominator is at least one.
 */
static gboolean
series_wide_div_round(
	SeriesWide	 numerator,
	guint64		 denominator,
	gint64		*out
){
	SeriesWide quotient;
	SeriesWide remainder;
	SeriesWide twice;

	quotient = numerator / (SeriesWide)denominator;
	remainder = numerator % (SeriesWide)denominator;

	/*
	 * remainder < denominator <= 2^63, so twice the remainder still fits
	 * even in the 64-bit fallback.
	 */
	twice = remainder * 2;

	if ((twice > (SeriesWide)denominator) ||
	    ((twice == (SeriesWide)denominator) && (1 == (quotient & 1))))
		quotient++;

	if (quotient > (SeriesWide)G_MAXINT64)
		return FALSE;

	*out = (gint64)quotient;
	return TRUE;
}

static void
series_set_overflow(
	GError		**error,
	const gchar	 *what
){
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "%s does not fit in a 64-bit integer", what);
}

/* --- Checked scalars ----------------------------------------------------- */

gboolean
venture_series_math_add(
	gint64	 a,
	gint64	 b,
	gint64	*out
){
	gint64 sum;

	g_return_val_if_fail(NULL != out, FALSE);

	if (__builtin_add_overflow(a, b, &sum))
		return FALSE;

	*out = sum;
	return TRUE;
}

gboolean
venture_series_math_mul(
	gint64	 a,
	gint64	 b,
	gint64	*out
){
	gint64 product;

	g_return_val_if_fail(NULL != out, FALSE);

	if (__builtin_mul_overflow(a, b, &product))
		return FALSE;

	*out = product;
	return TRUE;
}

gint64
venture_series_math_div_round(
	gint64	numerator,
	gint64	denominator
){
	gint64 out;

	g_return_val_if_fail(numerator >= 0, 0);
	g_return_val_if_fail(denominator >= 1, 0);

	out = 0;

	/* A quotient of non-negative 64-bit values always fits. */
	(void)series_wide_div_round((SeriesWide)numerator, (guint64)denominator,
	                            &out);
	return out;
}

/* --- Tiers --------------------------------------------------------------- */

static gint
series_tier_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureSeriesTier *left;
	const VentureSeriesTier *right;

	left = (const VentureSeriesTier *)a;
	right = (const VentureSeriesTier *)b;

	if (left->price < right->price)
		return -1;
	if (left->price > right->price)
		return 1;
	return 0;
}

gboolean
venture_series_math_normalise_tiers(
	GArray	 *tiers,
	GError	**error
){
	VentureSeriesTier *data;
	guint read;
	guint write;

	g_return_val_if_fail(NULL != tiers, FALSE);
	g_return_val_if_fail(sizeof(VentureSeriesTier) ==
	                     g_array_get_element_size(tiers), FALSE);

	data = (VentureSeriesTier *)(gpointer)tiers->data;

	for (read = 0; read < tiers->len; read++)
	{
		if (data[read].price < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A price of %" G_GINT64_FORMAT " is negative",
			            data[read].price);
			return FALSE;
		}

		if (data[read].quantity < 1)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A quantity of %" G_GINT64_FORMAT " is below one",
			            data[read].quantity);
			return FALSE;
		}
	}

	if (tiers->len < 2)
		return TRUE;

	g_array_sort(tiers, series_tier_compare);
	data = (VentureSeriesTier *)(gpointer)tiers->data;

	/* Fold each run of one price into its first tier. */
	write = 0;

	for (read = 1; read < tiers->len; read++)
	{
		if (data[read].price == data[write].price)
		{
			if (!venture_series_math_add(data[write].quantity,
			                             data[read].quantity,
			                             &data[write].quantity))
			{
				series_set_overflow(error, "The quantity offered at one price");
				return FALSE;
			}
			continue;
		}

		write++;
		data[write] = data[read];
	}

	g_array_set_size(tiers, write + 1);
	return TRUE;
}

/*
 * The total units of a book, refusing an overflow.
 */
static gboolean
series_units(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	gint64			 *out,
	GError			**error
){
	gint64 units;
	gsize i;

	units = 0;

	for (i = 0; i < n_tiers; i++)
	{
		if (!venture_series_math_add(units, tiers[i].quantity, &units))
		{
			series_set_overflow(error, "The total quantity");
			return FALSE;
		}
	}

	*out = units;
	return TRUE;
}

/*
 * The price of the unit at 1-based @rank (1 .. units) of a sorted book.
 */
static gint64
series_unit_at(
	const VentureSeriesTier	*tiers,
	gsize			 n_tiers,
	gint64			 rank
){
	gint64 seen;
	gsize i;

	seen = 0;

	for (i = 0; i < n_tiers; i++)
	{
		/* rank - seen avoids adding past the range near the top. */
		if (rank - seen <= tiers[i].quantity)
			return tiers[i].price;

		seen += tiers[i].quantity;
	}

	return (n_tiers > 0) ? tiers[n_tiers - 1].price : VENTURE_SERIES_NONE;
}

/*
 * ceil(units * percent / 100) and floor(units * percent / 100), computed
 * without forming units * percent (which can overflow for a large book).
 */
static gint64
series_share_ceil(
	gint64	units,
	guint	percent
){
	gint64 whole;
	gint64 part;

	whole = (units / 100) * (gint64)percent;
	part = (units % 100) * (gint64)percent;

	return whole + part / 100 + ((0 != part % 100) ? 1 : 0);
}

static gint64
series_share_floor(
	gint64	units,
	guint	percent
){
	return (units / 100) * (gint64)percent + ((units % 100) * (gint64)percent) / 100;
}

gint64
venture_series_math_percentile(
	const VentureSeriesTier	*tiers,
	gsize			 n_tiers,
	guint			 percent
){
	gint64 units;
	gint64 rank;

	g_return_val_if_fail((percent >= 1) && (percent <= 100), VENTURE_SERIES_NONE);

	if ((0 == n_tiers) || !series_units(tiers, n_tiers, &units, NULL) ||
	    (units < 1))
		return VENTURE_SERIES_NONE;

	rank = series_share_ceil(units, percent);

	if (rank < 1)
		rank = 1;

	return series_unit_at(tiers, n_tiers, rank);
}

/*
 * The mean of two non-negative values rounded half to even. Their sum
 * always fits the wide type -- two values below 2^63 sum below 2^64 --
 * where it would overflow a signed 64-bit one. The rounding has to see
 * the whole sum: halving each value first and rounding the leftover
 * halves rounds 7.5 to 7.
 */
static gint64
series_midpoint(
	gint64	low,
	gint64	high
){
	gint64 out;

	out = 0;
	(void)series_wide_div_round((SeriesWide)low + (SeriesWide)high, 2, &out);
	return out;
}

/*
 * The median unit of a book whose total is already known.
 */
static gint64
series_median_units(
	const VentureSeriesTier	*tiers,
	gsize			 n_tiers,
	gint64			 units
){
	gint64 low;
	gint64 high;

	if (units < 1)
		return VENTURE_SERIES_NONE;

	if (1 == (units & 1))
		return series_unit_at(tiers, n_tiers, units / 2 + 1);

	low = series_unit_at(tiers, n_tiers, units / 2);
	high = series_unit_at(tiers, n_tiers, units / 2 + 1);

	return series_midpoint(low, high);
}

/*
 * A jump is a price at least 20% above the one before: 5 (p - q) >= q.
 * Written so neither side can overflow: when p - q is past a fifth of the
 * unsigned range it exceeds any q anyway.
 */
static gboolean
series_is_jump(
	gint64	previous,
	gint64	price
){
	guint64 rise;

	if (price <= previous)
		return FALSE;

	rise = (guint64)price - (guint64)previous;

	if (rise > G_MAXUINT64 / 5)
		return TRUE;

	return rise * 5 >= (guint64)previous;
}

/*
 * The market value over a book whose total is known. Shared by the public
 * function and the summary, so the two cannot disagree.
 */
static gboolean
series_market_value(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	gint64			  units,
	gint64			 *out,
	GError			**error
){
	g_autofree VentureSeriesTier *taken = NULL;
	gsize n_taken;
	gint64 min_units;
	gint64 max_units;
	gint64 count;
	gint64 previous;
	long double mean;
	long double variance;
	long double spread;
	SeriesWide sum;
	gint64 kept;
	gsize i;

	*out = VENTURE_SERIES_NONE;

	if ((0 == n_tiers) || (units < 1))
		return TRUE;

	/*
	 * Steps 1 to 3: the cheapest 15% unconditionally, then on to 30%
	 * until a jump. A tier is checked for a jump only once the 15% is in
	 * hand, so a jump inside the first 15% cuts nothing.
	 */
	min_units = series_share_ceil(units, 15);
	if (min_units < 1)
		min_units = 1;

	max_units = series_share_floor(units, 30);
	if (max_units < min_units)
		max_units = min_units;

	taken = g_new0(VentureSeriesTier, n_tiers);
	n_taken = 0;
	count = 0;
	previous = VENTURE_SERIES_NONE;

	for (i = 0; i < n_tiers; i++)
	{
		gint64 take;

		if (count >= min_units)
		{
			if (count >= max_units)
				break;

			if ((VENTURE_SERIES_NONE != previous) &&
			    series_is_jump(previous, tiers[i].price))
				break;
		}

		take = MIN(tiers[i].quantity, max_units - count);
		taken[n_taken].price = tiers[i].price;
		taken[n_taken].quantity = take;
		n_taken++;
		count += take;
		previous = tiers[i].price;
	}

	/*
	 * Step 4: the mean and spread of what was taken. These are
	 * statistics, not money -- they only decide which units are outliers
	 * -- so a long double is the right tool; the figure returned is
	 * recomputed exactly from the survivors below.
	 */
	mean = 0.0L;
	for (i = 0; i < n_taken; i++)
		mean += (long double)taken[i].price * (long double)taken[i].quantity;
	mean /= (long double)count;

	variance = 0.0L;
	for (i = 0; i < n_taken; i++)
	{
		long double delta;

		delta = (long double)taken[i].price - mean;
		variance += delta * delta * (long double)taken[i].quantity;
	}
	variance /= (long double)count;
	spread = 1.5L * sqrtl(variance);

	/* Step 5: the exact mean of the units within 1.5 standard deviations. */
	sum = 0;
	kept = 0;

	for (i = 0; i < n_taken; i++)
	{
		if (fabsl((long double)taken[i].price - mean) > spread)
			continue;

		if (!series_wide_add_product(&sum, (guint64)taken[i].price,
		                             (guint64)taken[i].quantity))
		{
			series_set_overflow(error, "The market value's running total");
			return FALSE;
		}
		kept += taken[i].quantity;
	}

	/*
	 * A population always has a unit within one standard deviation of its
	 * mean, so nothing survives only if rounding conspired; fall back to
	 * everything taken rather than report no value.
	 */
	if (0 == kept)
	{
		sum = 0;
		for (i = 0; i < n_taken; i++)
		{
			if (!series_wide_add_product(&sum, (guint64)taken[i].price,
			                             (guint64)taken[i].quantity))
			{
				series_set_overflow(error, "The market value's running total");
				return FALSE;
			}
		}
		kept = count;
	}

	if (!series_wide_div_round(sum, (guint64)kept, out))
	{
		series_set_overflow(error, "The market value");
		return FALSE;
	}

	return TRUE;
}

gint64
venture_series_math_market_value(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	GError			**error
){
	gint64 units;
	gint64 value;

	if ((0 == n_tiers) || (NULL == tiers))
		return VENTURE_SERIES_NONE;

	if (!series_units(tiers, n_tiers, &units, error))
		return VENTURE_SERIES_NONE;

	if (!series_market_value(tiers, n_tiers, units, &value, error))
		return VENTURE_SERIES_NONE;

	return value;
}

gboolean
venture_series_math_summarise(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	VentureSeriesSummary	 *out,
	GError			**error
){
	SeriesWide sum;
	long double mean;
	long double variance;
	gint64 units;
	gsize i;

	g_return_val_if_fail(NULL != out, FALSE);

	out->units = VENTURE_SERIES_NONE;
	out->tiers = VENTURE_SERIES_NONE;
	out->min = VENTURE_SERIES_NONE;
	out->max = VENTURE_SERIES_NONE;
	out->market_value = VENTURE_SERIES_NONE;
	out->median = VENTURE_SERIES_NONE;
	out->p15 = VENTURE_SERIES_NONE;
	out->mean = VENTURE_SERIES_NONE;
	out->stddev = VENTURE_SERIES_NONE;

	if ((0 == n_tiers) || (NULL == tiers))
		return TRUE;

	if (!series_units(tiers, n_tiers, &units, error))
		return FALSE;

	if (units < 1)
		return TRUE;

	/* The exact quantity-weighted mean. */
	sum = 0;
	for (i = 0; i < n_tiers; i++)
	{
		if (!series_wide_add_product(&sum, (guint64)tiers[i].price,
		                             (guint64)tiers[i].quantity))
		{
			series_set_overflow(error, "The book's total value");
			return FALSE;
		}
	}

	if (!series_wide_div_round(sum, (guint64)units, &out->mean))
	{
		series_set_overflow(error, "The mean price");
		return FALSE;
	}

	if (!series_market_value(tiers, n_tiers, units, &out->market_value, error))
		return FALSE;

	/*
	 * The spread is a statistic: computed in long double around the exact
	 * mean and rounded once to minor units for display.
	 */
	mean = (long double)out->mean;
	variance = 0.0L;
	for (i = 0; i < n_tiers; i++)
	{
		long double delta;

		delta = (long double)tiers[i].price - mean;
		variance += delta * delta * (long double)tiers[i].quantity;
	}
	variance /= (long double)units;

	{
		long double deviation;

		deviation = floorl(sqrtl(variance) + 0.5L);
		out->stddev = (deviation >= (long double)G_MAXINT64) ?
		              G_MAXINT64 : (gint64)deviation;
	}

	out->units = units;
	out->tiers = (gint64)n_tiers;
	out->min = tiers[0].price;
	out->max = tiers[n_tiers - 1].price;
	out->median = series_median_units(tiers, n_tiers, units);
	out->p15 = series_unit_at(tiers, n_tiers,
	                          MAX((gint64)1, series_share_ceil(units, 15)));

	return TRUE;
}

/* --- Plain values -------------------------------------------------------- */

static gint
series_int64_compare(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 left;
	gint64 right;

	left = *(const gint64 *)a;
	right = *(const gint64 *)b;

	return (left < right) ? -1 : ((left > right) ? 1 : 0);
}

/*
 * A sorted copy of @values without the NONE entries.
 */
static gint64 *
series_sorted_present(
	const gint64	*values,
	gsize		 n_values,
	gsize		*out_n
){
	gint64 *copy;
	gsize n;
	gsize i;

	copy = g_new(gint64, MAX(n_values, (gsize)1));
	n = 0;

	for (i = 0; i < n_values; i++)
	{
		if (VENTURE_SERIES_NONE != values[i])
			copy[n++] = values[i];
	}

	qsort(copy, n, sizeof(gint64), series_int64_compare);
	*out_n = n;
	return copy;
}

gint64
venture_series_math_median(
	const gint64	*values,
	gsize		 n_values
){
	g_autofree gint64 *sorted = NULL;
	gint64 low;
	gint64 high;
	gsize n;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	sorted = series_sorted_present(values, n_values, &n);

	if (0 == n)
		return VENTURE_SERIES_NONE;

	if (1 == (n & 1))
		return sorted[n / 2];

	low = sorted[n / 2 - 1];
	high = sorted[n / 2];

	/*
	 * Halve each before adding so two values near the top of the range
	 * cannot overflow. Prices are non-negative; a negative value (a gap,
	 * a ratio someone passed) takes the plain route, which is exact for
	 * anything that is not near the range's edge.
	 */
	if ((low >= 0) && (high >= 0))
		return series_midpoint(low, high);

	return low + (high - low) / 2;
}

gint64
venture_series_math_p33(
	const gint64	*values,
	gsize		 n_values
){
	g_autofree gint64 *sorted = NULL;
	gsize n;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	sorted = series_sorted_present(values, n_values, &n);

	if (0 == n)
		return VENTURE_SERIES_NONE;

	return sorted[n / 3];
}

gint64
venture_series_math_deal_price(
	const gint64	*values,
	gsize		 n_values
){
	gsize present;
	gsize i;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	present = 0;
	for (i = 0; i < n_values; i++)
	{
		if (VENTURE_SERIES_NONE != values[i])
			present++;
	}

	if (present >= VENTURE_SERIES_DEAL_MIN_VENUES)
		return venture_series_math_p33(values, n_values);

	return venture_series_math_median(values, n_values);
}

/* --- Averages over days -------------------------------------------------- */

/*
 * round(1000000 * 0.5^(d / 2.1)) for d = 0 .. 13: a half-life of 2.1
 * days. Fixed rather than computed with pow() so every machine, and the
 * documentation, agree to the last digit.
 */
static const guint32 series_ewma_weights[VENTURE_SERIES_EWMA_DAYS] = {
	1000000, 718873, 516779, 371499, 267060, 191983, 138011,
	99213, 71321, 51271, 36857, 26496, 19047, 13692
};

guint32
venture_series_math_ewma_weight(guint days_ago)
{
	if (days_ago >= VENTURE_SERIES_EWMA_DAYS)
		return 0;

	return series_ewma_weights[days_ago];
}

gint64
venture_series_math_ewma(
	const gint64	 *values,
	gsize		  n_values,
	GError		**error
){
	SeriesWide sum;
	guint64 weights;
	gint64 out;
	gsize i;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	sum = 0;
	weights = 0;

	for (i = 0; (i < n_values) && (i < VENTURE_SERIES_EWMA_DAYS); i++)
	{
		guint32 weight;

		if (VENTURE_SERIES_NONE == values[i])
			continue;

		if (values[i] < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A daily value of %" G_GINT64_FORMAT " is negative",
			            values[i]);
			return VENTURE_SERIES_NONE;
		}

		weight = series_ewma_weights[i];

		if (!series_wide_add_product(&sum, (guint64)values[i], weight))
		{
			series_set_overflow(error, "The weighted sum");
			return VENTURE_SERIES_NONE;
		}
		weights += weight;
	}

	if (0 == weights)
		return VENTURE_SERIES_NONE;

	if (!series_wide_div_round(sum, weights, &out))
	{
		series_set_overflow(error, "The weighted mean");
		return VENTURE_SERIES_NONE;
	}

	return out;
}

gint64
venture_series_math_mean(
	const gint64	 *values,
	gsize		  n_values,
	GError		**error
){
	SeriesWide sum;
	guint64 count;
	gint64 out;
	gsize i;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	sum = 0;
	count = 0;

	for (i = 0; i < n_values; i++)
	{
		if (VENTURE_SERIES_NONE == values[i])
			continue;

		if (values[i] < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A value of %" G_GINT64_FORMAT " is negative",
			            values[i]);
			return VENTURE_SERIES_NONE;
		}

		if (!series_wide_add_product(&sum, (guint64)values[i], 1))
		{
			series_set_overflow(error, "The sum");
			return VENTURE_SERIES_NONE;
		}
		count++;
	}

	if (0 == count)
		return VENTURE_SERIES_NONE;

	if (!series_wide_div_round(sum, count, &out))
	{
		series_set_overflow(error, "The mean");
		return VENTURE_SERIES_NONE;
	}

	return out;
}

/* --- Bulk cost ----------------------------------------------------------- */

gboolean
venture_series_math_bulk_cost(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	gint64			  units,
	VentureSeriesBulkCost	 *out,
	GError			**error
){
	gint64 remaining;
	gsize i;

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	out->requested = units;
	out->worst_price = VENTURE_SERIES_NONE;
	out->average = VENTURE_SERIES_NONE;

	if (units < 1)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Ask for at least one unit, not %" G_GINT64_FORMAT, units);
		return FALSE;
	}

	remaining = units;

	for (i = 0; (i < n_tiers) && (remaining > 0); i++)
	{
		gint64 take;
		gint64 line;

		take = MIN(tiers[i].quantity, remaining);

		if (!venture_series_math_mul(take, tiers[i].price, &line) ||
		    !venture_series_math_add(out->cost, line, &out->cost))
		{
			series_set_overflow(error, "The bulk cost");
			return FALSE;
		}

		out->filled += take;
		out->worst_price = tiers[i].price;
		remaining -= take;
	}

	out->complete = (0 == remaining);

	if (out->filled > 0)
		out->average = venture_series_math_div_round(out->cost, out->filled);

	return TRUE;
}

/* --- Realisable profit --------------------------------------------------- */

gint64
venture_series_math_expected_sales(
	gdouble	sold_per_day,
	gdouble	days
){
	gdouble units;

	if (isnan(sold_per_day) || !isfinite(sold_per_day) || (sold_per_day < 0.0) ||
	    !isfinite(days) || (days <= 0.0))
		return VENTURE_SERIES_NONE;

	units = floor(sold_per_day * days);

	/* 2^63 is the first double past G_MAXINT64; anything from it on is
	 * held there rather than converted out of range. */
	if (units >= 9223372036854775808.0)
		return G_MAXINT64;

	return (gint64)units;
}

gboolean
venture_series_math_realisable(
	const VentureSeriesTier		 *tiers,
	gsize				  n_tiers,
	gint64				  net_price,
	gint64				  max_units,
	VentureSeriesRealisable		 *out,
	GError				**error
){
	gsize i;

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));

	if (max_units < 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The units expected to sell are at least zero, not %" G_GINT64_FORMAT, max_units);
		return FALSE;
	}

	/* Sorted cheapest first, so the first tier that makes nothing ends
	 * the profitable part of the book. */
	for (i = 0; (i < n_tiers) && (tiers[i].price < net_price); i++)
	{
		gint64 take;
		gint64 line;
		gint64 margin;

		if (!venture_series_math_add(out->book_units, tiers[i].quantity, &out->book_units))
		{
			series_set_overflow(error, "The units offered at a profit");
			return FALSE;
		}

		take = MIN(tiers[i].quantity, max_units - out->units);

		if (take <= 0)
		{
			out->capped = TRUE;
			continue;
		}

		if (take < tiers[i].quantity)
			out->capped = TRUE;

		/* net_price > price >= 0, so the margin is positive and fits. */
		margin = net_price - tiers[i].price;

		if (!venture_series_math_mul(take, tiers[i].price, &line) ||
		    !venture_series_math_add(out->cost, line, &out->cost) ||
		    !venture_series_math_mul(take, margin, &line) ||
		    !venture_series_math_add(out->profit, line, &out->profit))
		{
			series_set_overflow(error, "The realisable profit");
			return FALSE;
		}

		out->units += take;
	}

	return TRUE;
}

/* --- Sale estimate ------------------------------------------------------- */

/*
 * Finds or adds the entry for @instrument. The index maps an instrument
 * to its position in @sales.
 */
static VentureSeriesSale *
series_sale_for(
	GArray		*sales,
	GHashTable	*index,
	gint64		 instrument
){
	gpointer found;
	VentureSeriesSale fresh;

	if (g_hash_table_lookup_extended(index, &instrument, NULL, &found))
		return &g_array_index(sales, VentureSeriesSale, GPOINTER_TO_UINT(found));

	memset(&fresh, 0, sizeof(fresh));
	fresh.instrument = instrument;
	g_array_append_val(sales, fresh);
	g_hash_table_insert(index, g_memdup2(&instrument, sizeof(gint64)),
	                    GUINT_TO_POINTER(sales->len - 1));

	return &g_array_index(sales, VentureSeriesSale, sales->len - 1);
}

static gint
series_sale_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureSeriesSale *left;
	const VentureSeriesSale *right;

	left = (const VentureSeriesSale *)a;
	right = (const VentureSeriesSale *)b;

	if (left->instrument < right->instrument)
		return -1;
	if (left->instrument > right->instrument)
		return 1;
	return 0;
}

/*
 * Books @units at @price as sold for @instrument.
 */
static gboolean
series_book_sale(
	GArray		 *sales,
	GHashTable	 *index,
	gint64		  instrument,
	gint64		  price,
	gint64		  units,
	GError		**error
){
	VentureSeriesSale *sale;
	gint64 value;

	sale = series_sale_for(sales, index, instrument);

	if (!venture_series_math_mul(price, units, &value) ||
	    !venture_series_math_add(sale->sold_value, value, &sale->sold_value) ||
	    !venture_series_math_add(sale->sold_units, units, &sale->sold_units))
	{
		series_set_overflow(error, "The estimated sales");
		return FALSE;
	}

	return TRUE;
}

GArray *
venture_series_math_sale_estimate(
	const VentureSeriesListingMark	 *previous,
	gsize				  n_previous,
	const VentureSeriesListingMark	 *current,
	gsize				  n_current,
	gint64				  elapsed,
	GError				**error
){
	g_autoptr(GArray) sales = NULL;
	g_autoptr(GHashTable) index = NULL;
	gsize p;
	gsize c;

	sales = g_array_new(FALSE, TRUE, sizeof(VentureSeriesSale));
	index = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	p = 0;
	c = 0;

	/*
	 * A merge over two id-sorted lists: one pass, no lookup table, which
	 * matters at a hundred thousand listings per venue per hour.
	 */
	while (p < n_previous)
	{
		const VentureSeriesListingMark *old;

		old = &previous[p];

		if ((p > 0) && (previous[p - 1].id >= old->id))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "The earlier listings are not sorted by id");
			return NULL;
		}

		while ((c < n_current) && (current[c].id < old->id))
		{
			if ((c > 0) && (current[c - 1].id >= current[c].id))
			{
				g_set_error_literal(error, VENTURE_ERROR,
				                    VENTURE_ERROR_INVALID_ARGUMENT,
				                    "The later listings are not sorted by id");
				return NULL;
			}
			c++;
		}

		if ((c < n_current) && (current[c].id == old->id))
		{
			/* Still listed: a smaller quantity is a partial sale. */
			if (current[c].quantity < old->quantity)
			{
				if (!series_book_sale(sales, index, old->instrument, old->price,
				                      old->quantity - current[c].quantity, error))
					return NULL;
			}
		}
		else if ((old->expires_in_min < 0) || (old->expires_in_min > elapsed))
		{
			/* Gone before it could have expired: sold. */
			if (!series_book_sale(sales, index, old->instrument, old->price,
			                      old->quantity, error))
				return NULL;
		}
		else
		{
			VentureSeriesSale *sale;

			sale = series_sale_for(sales, index, old->instrument);

			if (!venture_series_math_add(sale->expired_units, old->quantity,
			                             &sale->expired_units))
			{
				series_set_overflow(error, "The expired quantity");
				return NULL;
			}
		}

		p++;
	}

	g_array_sort(sales, series_sale_compare);
	return g_steal_pointer(&sales);
}

/* --- Heat matrix --------------------------------------------------------- */

static gint64
series_floor_div(
	gint64	a,
	gint64	b
){
	gint64 q;

	q = a / b;

	if (((a % b) != 0) && ((a < 0) != (b < 0)))
		q--;

	return q;
}

gboolean
venture_series_math_heat(
	const gint64		 *times,
	const gint64		 *values,
	gsize			  n,
	gint64			  zone_offset,
	VentureSeriesHeat	 *out,
	GError			**error
){
	SeriesWide sums[7][24];
	guint day;
	guint hour;
	gsize i;

	g_return_val_if_fail(NULL != out, FALSE);

	memset(sums, 0, sizeof(sums));
	memset(out->count, 0, sizeof(out->count));

	for (i = 0; i < n; i++)
	{
		gint64 local;
		gint64 days;
		gint64 seconds;

		if (VENTURE_SERIES_NONE == values[i])
			continue;

		if (values[i] < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A value of %" G_GINT64_FORMAT " is negative",
			            values[i]);
			return FALSE;
		}

		if (!venture_series_math_add(times[i], zone_offset, &local))
		{
			series_set_overflow(error, "A local time");
			return FALSE;
		}

		/* 1 January 1970 was a Thursday: index 3 with Monday as 0. */
		days = series_floor_div(local, 86400);
		seconds = local - days * 86400;
		day = (guint)(((days % 7) + 7 + 3) % 7);
		hour = (guint)(seconds / 3600);

		if (!series_wide_add_product(&sums[day][hour], (guint64)values[i], 1))
		{
			series_set_overflow(error, "A heat-map cell");
			return FALSE;
		}
		out->count[day][hour]++;
	}

	for (day = 0; day < 7; day++)
	{
		for (hour = 0; hour < 24; hour++)
		{
			if (0 == out->count[day][hour])
			{
				out->value[day][hour] = VENTURE_SERIES_NONE;
				continue;
			}

			if (!series_wide_div_round(sums[day][hour], out->count[day][hour],
			                           &out->value[day][hour]))
			{
				series_set_overflow(error, "A heat-map cell");
				return FALSE;
			}
		}
	}

	return TRUE;
}

/* --- Interval learning --------------------------------------------------- */

gint64
venture_series_math_learn_interval(
	const gint64	*gaps,
	gsize		 n_gaps
){
	g_autofree gint64 *usable = NULL;
	g_autofree gint64 *maxima = NULL;
	gsize n;
	gsize i;
	gint64 interval;

	if ((0 == n_gaps) || (NULL == gaps))
		return VENTURE_SERIES_INTERVAL_DEFAULT;

	usable = g_new(gint64, n_gaps);
	n = 0;

	for (i = 0; i < n_gaps; i++)
	{
		if (gaps[i] > 0)
			usable[n++] = gaps[i];
	}

	if (0 == n)
		return VENTURE_SERIES_INTERVAL_DEFAULT;

	if (n < 3)
	{
		interval = usable[0];
		for (i = 1; i < n; i++)
			interval = MAX(interval, usable[i]);
	}
	else
	{
		maxima = g_new(gint64, n - 2);

		for (i = 0; i + 2 < n; i++)
			maxima[i] = MAX(usable[i], MAX(usable[i + 1], usable[i + 2]));

		interval = venture_series_math_median(maxima, n - 2);
	}

	return CLAMP(interval, (gint64)1, (gint64)VENTURE_SERIES_INTERVAL_CAP);
}
