/*
 * venture-series-math.c - The arithmetic behind market-data series
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See venture-series-math.h for the contract and docs/market-data.org for
 * the formulas written out. Everything here is a pure function of its
 * arguments, which is what lets the store call it from a worker thread.
 *
 * The arithmetic is the Tallow kernel src/kernels/vk_series.tlw, compiled
 * natively (docs/kernels.org). This file converts at the edge: a tier
 * array becomes two arrays (prices, quantities), VENTURE_SERIES_NONE
 * becomes an absent value and back, and each kernel outcome becomes the
 * message it always was. Exact means need no 128-bit accumulator any more:
 * the kernel carries a sum as a quotient and remainder of its known
 * divisor, which is the same integer the unsigned __int128 gave (and has
 * no 64-bit fallback whose remainder * 2 could overflow).
 *
 * One statistic stays here: the market value's outlier test is computed in
 * long double (series_keep()), which Tallow does not have; a double would
 * move the boundary between a kept unit and an outlier.
 */

#include "venture.h"

#include <vk-kernels.h>

#include <math.h>
#include <string.h>

static void
series_set_overflow(
	GError		**error,
	const gchar	 *what
){
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "%s does not fit in a 64-bit integer", what);
}

/* --- Converting at the edge ---------------------------------------------- */

/* The prices and the quantities of @n tiers, as the two arrays the kernel
 * takes. */
static void
series_split(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	GArray			**out_prices,
	GArray			**out_quantities
){
	GArray *prices;
	GArray *quantities;
	gsize i;

	prices = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_tiers);
	quantities = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_tiers);

	for (i = 0; i < n_tiers; i++)
	{
		g_array_append_val(prices, tiers[i].price);
		g_array_append_val(quantities, tiers[i].quantity);
	}

	*out_prices = prices;
	*out_quantities = quantities;
}

/* Values with VENTURE_SERIES_NONE as the kernel's absent values. */
static GArray *
series_optional(
	const gint64	*values,
	gsize		 n_values
){
	GArray *out;
	gsize i;

	out = g_array_sized_new(FALSE, FALSE, sizeof(TlwOptI64), (guint)n_values);

	for (i = 0; i < n_values; i++)
	{
		TlwOptI64 value;

		value.has = (VENTURE_SERIES_NONE != values[i]);
		value.val = value.has ? values[i] : 0;
		g_array_append_val(out, value);
	}

	return out;
}

/* A kernel's optional as the C API's value or VENTURE_SERIES_NONE. */
static gint64
series_or_none(TlwOptI64 value)
{
	return value.has ? value.val : VENTURE_SERIES_NONE;
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
	g_return_val_if_fail(numerator >= 0, 0);
	g_return_val_if_fail(denominator >= 1, 0);

	/* A quotient of non-negative 64-bit values always fits. */
	return vk_series_div_round(numerator, denominator);
}

/* --- Tiers --------------------------------------------------------------- */

gboolean
venture_series_math_normalise_tiers(
	GArray	 *tiers,
	GError	**error
){
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autoptr(VkSeriesBook) book = NULL;
	VentureSeriesTier *data;
	guint i;

	g_return_val_if_fail(NULL != tiers, FALSE);
	g_return_val_if_fail(sizeof(VentureSeriesTier) ==
	                     g_array_get_element_size(tiers), FALSE);

	/* vk_series_normalise: every tier judged in order, then sorted by
	 * price with each run of one price folded into one tier. */
	series_split((const VentureSeriesTier *)(gconstpointer)tiers->data, tiers->len,
	             &prices, &quantities);
	book = vk_series_normalise(prices, quantities);

	switch (book->tag)
	{
	case VK_SERIES_BOOK_TIERS:
		break;

	case VK_SERIES_BOOK_NEGATIVE_PRICE:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A price of %" G_GINT64_FORMAT " is negative",
		            book->u.negative_price.price);
		return FALSE;

	case VK_SERIES_BOOK_NO_QUANTITY:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A quantity of %" G_GINT64_FORMAT " is below one",
		            book->u.no_quantity.quantity);
		return FALSE;

	case VK_SERIES_BOOK_OVERFLOW:
	default:
		series_set_overflow(error, "The quantity offered at one price");
		return FALSE;
	}

	g_array_set_size(tiers, book->u.tiers.prices->len);
	data = (VentureSeriesTier *)(gpointer)tiers->data;

	for (i = 0; i < tiers->len; i++)
	{
		data[i].price = g_array_index(book->u.tiers.prices, gint64, i);
		data[i].quantity = g_array_index(book->u.tiers.quantities, gint64, i);
	}

	return TRUE;
}

gint64
venture_series_math_percentile(
	const VentureSeriesTier	*tiers,
	gsize			 n_tiers,
	guint			 percent
){
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;

	g_return_val_if_fail((percent >= 1) && (percent <= 100), VENTURE_SERIES_NONE);

	if ((0 == n_tiers) || (NULL == tiers))
		return VENTURE_SERIES_NONE;

	series_split(tiers, n_tiers, &prices, &quantities);

	return series_or_none(vk_series_percentile(prices, quantities, (gint64)percent));
}

/*
 * Step 4 of the market value: which of the taken tiers are within 1.5
 * standard deviations of the taken units' mean. These are statistics, not
 * money -- they only decide which units are outliers -- and are computed in
 * long double, which the kernel has no type for; the figure returned is
 * the kernel's exact mean of the survivors.
 */
static GArray *
series_keep(
	GArray	*prices,
	GArray	*taken
){
	GArray *keep;
	long double mean;
	long double variance;
	long double spread;
	gint64 count;
	guint i;

	count = 0;
	for (i = 0; i < taken->len; i++)
		count += g_array_index(taken, gint64, i);

	mean = 0.0L;
	for (i = 0; i < taken->len; i++)
		mean += (long double)g_array_index(prices, gint64, i) *
		        (long double)g_array_index(taken, gint64, i);
	mean /= (long double)count;

	variance = 0.0L;
	for (i = 0; i < taken->len; i++)
	{
		long double delta;

		delta = (long double)g_array_index(prices, gint64, i) - mean;
		variance += delta * delta * (long double)g_array_index(taken, gint64, i);
	}
	variance /= (long double)count;
	spread = 1.5L * sqrtl(variance);

	keep = g_array_sized_new(FALSE, FALSE, sizeof(gboolean), taken->len);

	for (i = 0; i < taken->len; i++)
	{
		gboolean kept;

		kept = !(fabsl((long double)g_array_index(prices, gint64, i) - mean) > spread);
		g_array_append_val(keep, kept);
	}

	return keep;
}

/*
 * The market value of a book whose total is known. Shared by the public
 * function and the summary, so the two cannot disagree: the kernel takes
 * the cheapest 15% to 30% (vk_series_take), this file finds the outliers,
 * the kernel averages the rest exactly (vk_series_kept_mean).
 */
static gboolean
series_market_value(
	GArray		 *prices,
	GArray		 *quantities,
	gint64		  units,
	gint64		 *out,
	GError		**error
){
	g_autoptr(GArray) taken = NULL;
	g_autoptr(GArray) keep = NULL;
	TlwOptI64 value;

	*out = VENTURE_SERIES_NONE;

	if ((0 == prices->len) || (units < 1))
		return TRUE;

	taken = vk_series_take(prices, quantities, units);
	keep = series_keep(prices, taken);
	value = vk_series_kept_mean(prices, taken, keep);

	if (!value.has)
	{
		series_set_overflow(error, "The market value");
		return FALSE;
	}

	*out = value.val;

	return TRUE;
}

gint64
venture_series_math_market_value(
	const VentureSeriesTier	 *tiers,
	gsize			  n_tiers,
	GError			**error
){
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	TlwOptI64 units;
	gint64 value;

	if ((0 == n_tiers) || (NULL == tiers))
		return VENTURE_SERIES_NONE;

	series_split(tiers, n_tiers, &prices, &quantities);
	units = vk_series_units(quantities);

	if (!units.has)
	{
		series_set_overflow(error, "The total quantity");
		return VENTURE_SERIES_NONE;
	}

	if (!series_market_value(prices, quantities, units.val, &value, error))
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
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autoptr(VkSeriesSummary) summary = NULL;
	long double mean;
	long double variance;
	long double deviation;
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

	/* Units, the exact quantity-weighted mean, the ends, the median and
	 * the 15th percentile (vk_series_summarise). */
	series_split(tiers, n_tiers, &prices, &quantities);
	summary = vk_series_summarise(prices, quantities);

	switch (summary->tag)
	{
	case VK_SERIES_SUMMARY_STATS:
		break;

	case VK_SERIES_SUMMARY_UNITLESS:
		return TRUE;

	case VK_SERIES_SUMMARY_TOO_MANY:
		series_set_overflow(error, "The total quantity");
		return FALSE;

	case VK_SERIES_SUMMARY_TOO_VALUABLE:
	default:
		series_set_overflow(error, "The book's total value");
		return FALSE;
	}

	out->mean = summary->u.stats.mean;

	if (!series_market_value(prices, quantities, summary->u.stats.units,
	                         &out->market_value, error))
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
	variance /= (long double)summary->u.stats.units;
	deviation = floorl(sqrtl(variance) + 0.5L);
	out->stddev = (deviation >= (long double)G_MAXINT64) ? G_MAXINT64 : (gint64)deviation;

	out->units = summary->u.stats.units;
	out->tiers = (gint64)n_tiers;
	out->min = summary->u.stats.min_;
	out->max = summary->u.stats.max_;
	out->median = series_or_none(summary->u.stats.median);
	out->p15 = series_or_none(summary->u.stats.p15);

	return TRUE;
}

/* --- Plain values -------------------------------------------------------- */

gint64
venture_series_math_median(
	const gint64	*values,
	gsize		 n_values
){
	g_autoptr(GArray) present = NULL;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	/* Two middles meet half to even; a negative one takes the plain
	 * route (vk_series_median). */
	present = series_optional(values, n_values);

	return series_or_none(vk_series_median(present));
}

gint64
venture_series_math_p33(
	const gint64	*values,
	gsize		 n_values
){
	g_autoptr(GArray) present = NULL;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	present = series_optional(values, n_values);

	return series_or_none(vk_series_p33(present));
}

gint64
venture_series_math_deal_price(
	const gint64	*values,
	gsize		 n_values
){
	g_autoptr(GArray) present = NULL;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	present = series_optional(values, n_values);

	return series_or_none(vk_series_deal_price(present));
}

/* --- Averages over days -------------------------------------------------- */

guint32
venture_series_math_ewma_weight(guint days_ago)
{
	/* round(1000000 x 0.5^(d / 2.1)), a fixed table (vk_series_ewma_weight) */
	return (guint32)vk_series_ewma_weight((gint64)days_ago);
}

/* An average's value, VENTURE_SERIES_NONE with nothing to average, or the
 * error: @what_negative names a negative value, @what_sum the sum. */
static gint64
series_average(
	VkSeriesAverage	 *average,
	const gchar	 *what_negative,
	const gchar	 *what_sum,
	GError		**error
){
	switch (average->tag)
	{
	case VK_SERIES_AVERAGE_VALUE:
		return average->u.value.value;

	case VK_SERIES_AVERAGE_NEGATIVE:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s of %" G_GINT64_FORMAT " is negative", what_negative,
		            average->u.negative.value);
		return VENTURE_SERIES_NONE;

	case VK_SERIES_AVERAGE_TOO_LARGE:
		series_set_overflow(error, what_sum);
		return VENTURE_SERIES_NONE;

	case VK_SERIES_AVERAGE_NOTHING:
	default:
		return VENTURE_SERIES_NONE;
	}
}

gint64
venture_series_math_ewma(
	const gint64	 *values,
	gsize		  n_values,
	GError		**error
){
	g_autoptr(GArray) present = NULL;
	g_autoptr(VkSeriesAverage) average = NULL;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	present = series_optional(values, n_values);
	average = vk_series_ewma(present);

	return series_average(average, "A daily value", "The weighted sum", error);
}

gint64
venture_series_math_mean(
	const gint64	 *values,
	gsize		  n_values,
	GError		**error
){
	g_autoptr(GArray) present = NULL;
	g_autoptr(VkSeriesAverage) average = NULL;

	if ((0 == n_values) || (NULL == values))
		return VENTURE_SERIES_NONE;

	present = series_optional(values, n_values);
	average = vk_series_mean(present);

	return series_average(average, "A value", "The sum", error);
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
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autoptr(VkSeriesFill) fill = NULL;

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

	/* Cheapest first until the units are filled (vk_series_bulk_cost). */
	series_split(tiers, (NULL != tiers) ? n_tiers : 0, &prices, &quantities);
	fill = vk_series_bulk_cost(prices, quantities, units);

	if (VK_SERIES_FILL_FILLED != fill->tag)
	{
		series_set_overflow(error, "The bulk cost");
		return FALSE;
	}

	out->filled = fill->u.filled.filled;
	out->cost = fill->u.filled.cost;
	out->worst_price = series_or_none(fill->u.filled.worst);
	out->average = series_or_none(fill->u.filled.average);
	out->complete = fill->u.filled.complete;

	return TRUE;
}

/* --- Realisable profit --------------------------------------------------- */

gint64
venture_series_math_expected_sales(
	gdouble	sold_per_day,
	gdouble	days
){
	/* floor(pace x days), held at G_MAXINT64 (vk_series_expected_sales) */
	return series_or_none(vk_series_expected_sales(sold_per_day, days));
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
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autoptr(VkSeriesRealisable) realised = NULL;

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));

	if (max_units < 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "The units expected to sell are at least zero, not %" G_GINT64_FORMAT, max_units);
		return FALSE;
	}

	/* Sorted cheapest first, so the first tier that makes nothing ends
	 * the profitable part of the book (vk_series_realisable). */
	series_split(tiers, (NULL != tiers) ? n_tiers : 0, &prices, &quantities);
	realised = vk_series_realisable(prices, quantities, net_price, max_units);

	switch (realised->tag)
	{
	case VK_SERIES_REALISABLE_PROFIT:
		break;

	case VK_SERIES_REALISABLE_BOOK_OVERFLOW:
		series_set_overflow(error, "The units offered at a profit");
		return FALSE;

	case VK_SERIES_REALISABLE_PROFIT_OVERFLOW:
	default:
		series_set_overflow(error, "The realisable profit");
		return FALSE;
	}

	out->units = realised->u.profit.units;
	out->cost = realised->u.profit.cost;
	out->profit = realised->u.profit.profit;
	out->book_units = realised->u.profit.book_units;
	out->capped = realised->u.profit.capped;

	return TRUE;
}

/* --- Sale estimate ------------------------------------------------------- */

GArray *
venture_series_math_sale_estimate(
	const VentureSeriesListingMark	 *previous,
	gsize				  n_previous,
	const VentureSeriesListingMark	 *current,
	gsize				  n_current,
	gint64				  elapsed,
	GError				**error
){
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GArray) instruments = NULL;
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autoptr(GArray) expires = NULL;
	g_autoptr(GArray) current_ids = NULL;
	g_autoptr(GArray) current_quantities = NULL;
	g_autoptr(VkSeriesSales) estimate = NULL;
	GArray *sales;
	gsize i;

	/* The marks as the kernel's parallel arrays. */
	ids = g_array_sized_new(FALSE, FALSE, sizeof(guint64), (guint)n_previous);
	instruments = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_previous);
	prices = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_previous);
	quantities = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_previous);
	expires = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_previous);
	current_ids = g_array_sized_new(FALSE, FALSE, sizeof(guint64), (guint)n_current);
	current_quantities = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_current);

	for (i = 0; i < n_previous; i++)
	{
		g_array_append_val(ids, previous[i].id);
		g_array_append_val(instruments, previous[i].instrument);
		g_array_append_val(prices, previous[i].price);
		g_array_append_val(quantities, previous[i].quantity);
		g_array_append_val(expires, previous[i].expires_in_min);
	}

	for (i = 0; i < n_current; i++)
	{
		g_array_append_val(current_ids, current[i].id);
		g_array_append_val(current_quantities, current[i].quantity);
	}

	/*
	 * A merge over two id-sorted lists (vk_series_sale_estimate): one
	 * pass, no lookup table, which matters at a hundred thousand
	 * listings per venue per hour.
	 */
	estimate = vk_series_sale_estimate(ids, instruments, prices, quantities, expires,
	                                   current_ids, current_quantities, elapsed);

	switch (estimate->tag)
	{
	case VK_SERIES_SALES_ESTIMATED:
		break;

	case VK_SERIES_SALES_EARLIER_UNSORTED:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The earlier listings are not sorted by id");
		return NULL;

	case VK_SERIES_SALES_LATER_UNSORTED:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The later listings are not sorted by id");
		return NULL;

	case VK_SERIES_SALES_SALES_OVERFLOW:
		series_set_overflow(error, "The estimated sales");
		return NULL;

	case VK_SERIES_SALES_EXPIRED_OVERFLOW:
	default:
		series_set_overflow(error, "The expired quantity");
		return NULL;
	}

	sales = g_array_sized_new(FALSE, TRUE, sizeof(VentureSeriesSale),
	                          estimate->u.estimated.instruments->len);

	for (i = 0; i < estimate->u.estimated.instruments->len; i++)
	{
		VentureSeriesSale sale;

		memset(&sale, 0, sizeof(sale));
		sale.instrument = g_array_index(estimate->u.estimated.instruments, gint64, i);
		sale.sold_units = g_array_index(estimate->u.estimated.sold_units, gint64, i);
		sale.sold_value = g_array_index(estimate->u.estimated.sold_values, gint64, i);
		sale.expired_units = g_array_index(estimate->u.estimated.expired_units, gint64, i);
		g_array_append_val(sales, sale);
	}

	return sales;
}

/* --- Heat matrix --------------------------------------------------------- */

gboolean
venture_series_math_heat(
	const gint64		 *times,
	const gint64		 *values,
	gsize			  n,
	gint64			  zone_offset,
	VentureSeriesHeat	 *out,
	GError			**error
){
	g_autoptr(GArray) time_array = NULL;
	g_autoptr(GArray) present = NULL;
	g_autoptr(VkSeriesHeat) heat = NULL;
	guint day;
	guint hour;

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out->count, 0, sizeof(out->count));

	/* Each value placed in its weekday-by-hour cell (1 January 1970 was a
	 * Thursday) and each cell averaged exactly (vk_series_heat). */
	time_array = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n);
	if (n > 0)
		g_array_append_vals(time_array, times, (guint)n);
	present = series_optional(values, n);
	heat = vk_series_heat(time_array, present, zone_offset);

	switch (heat->tag)
	{
	case VK_SERIES_HEAT_CELLS:
		break;

	case VK_SERIES_HEAT_NEGATIVE_VALUE:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A value of %" G_GINT64_FORMAT " is negative",
		            heat->u.negative_value.value);
		return FALSE;

	case VK_SERIES_HEAT_LOCAL_OVERFLOW:
	default:
		series_set_overflow(error, "A local time");
		return FALSE;
	}

	for (day = 0; day < 7; day++)
	{
		for (hour = 0; hour < 24; hour++)
		{
			guint cell;

			cell = day * 24 + hour;
			out->count[day][hour] = (guint32)g_array_index(heat->u.cells.counts, gint64, cell);
			out->value[day][hour] = (0 == out->count[day][hour])
			                        ? VENTURE_SERIES_NONE
			                        : g_array_index(heat->u.cells.means, gint64, cell);
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
	g_autoptr(GArray) array = NULL;

	if ((0 == n_gaps) || (NULL == gaps))
		return VENTURE_SERIES_INTERVAL_DEFAULT;

	/* The median of each three gaps' largest, held to [1, the cap]
	 * (vk_series_learn_interval). */
	array = g_array_sized_new(FALSE, FALSE, sizeof(gint64), (guint)n_gaps);
	g_array_append_vals(array, gaps, (guint)n_gaps);

	return vk_series_learn_interval(array);
}
