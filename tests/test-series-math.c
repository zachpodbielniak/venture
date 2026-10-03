/*
 * test-series-math.c - The arithmetic behind market-data series
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every figure a market page, the price oracle or the arbitrage scanner
 * shows comes out of these functions. Each test pins one rule written in
 * docs/market-data.org, with the numbers worked by hand, so a change that
 * quietly moves a market value or a deal price fails here rather than in
 * somebody's trade.
 */

#include <venture.h>

#include <math.h>
#include <string.h>

static GArray *
tiers_of(
	const gint64	*pairs,
	gsize		 n_pairs
){
	GArray *tiers;
	gsize i;

	tiers = g_array_new(FALSE, TRUE, sizeof(VentureSeriesTier));

	for (i = 0; i < n_pairs; i++)
	{
		VentureSeriesTier tier;

		tier.price = pairs[i * 2];
		tier.quantity = pairs[i * 2 + 1];
		g_array_append_val(tiers, tier);
	}

	return tiers;
}

#define TIERS(arr) ((const VentureSeriesTier *)(gpointer)(arr)->data), (arr)->len

/* --- Summaries ---------------------------------------------------------------- */

/*
 * No listings is a fact, not an error, and must never read as a price of
 * zero: a page would show the item as free.
 */
static void
test_empty(void)
{
	VentureSeriesSummary summary;
	VentureSeriesBulkCost cost;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_series_math_summarise(NULL, 0, &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpint(summary.units, ==, VENTURE_SERIES_NONE);
	g_assert_cmpint(summary.min, ==, VENTURE_SERIES_NONE);
	g_assert_cmpint(summary.market_value, ==, VENTURE_SERIES_NONE);
	g_assert_cmpint(summary.median, ==, VENTURE_SERIES_NONE);
	g_assert_cmpint(summary.mean, ==, VENTURE_SERIES_NONE);
	g_assert_cmpint(summary.stddev, ==, VENTURE_SERIES_NONE);

	g_assert_cmpint(venture_series_math_market_value(NULL, 0, NULL), ==,
	                VENTURE_SERIES_NONE);
	g_assert_cmpint(venture_series_math_percentile(NULL, 0, 15), ==,
	                VENTURE_SERIES_NONE);
	g_assert_cmpint(venture_series_math_median(NULL, 0), ==, VENTURE_SERIES_NONE);
	g_assert_cmpint(venture_series_math_deal_price(NULL, 0), ==,
	                VENTURE_SERIES_NONE);
	g_assert_cmpint(venture_series_math_ewma(NULL, 0, NULL), ==,
	                VENTURE_SERIES_NONE);

	g_assert_true(venture_series_math_bulk_cost(NULL, 0, 5, &cost, &error));
	g_assert_cmpint(cost.filled, ==, 0);
	g_assert_false(cost.complete);
	g_assert_cmpint(cost.average, ==, VENTURE_SERIES_NONE);
}

static void
test_single(void)
{
	static const gint64 pairs[] = { 12345, 1 };
	g_autoptr(GArray) tiers = tiers_of(pairs, 1);
	VentureSeriesSummary summary;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_series_math_summarise(TIERS(tiers), &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpint(summary.units, ==, 1);
	g_assert_cmpint(summary.tiers, ==, 1);
	g_assert_cmpint(summary.min, ==, 12345);
	g_assert_cmpint(summary.max, ==, 12345);
	g_assert_cmpint(summary.market_value, ==, 12345);
	g_assert_cmpint(summary.median, ==, 12345);
	g_assert_cmpint(summary.p15, ==, 12345);
	g_assert_cmpint(summary.mean, ==, 12345);
	g_assert_cmpint(summary.stddev, ==, 0);
}

/* A spread of zero must not divide by it when trimming outliers. */
static void
test_all_equal(void)
{
	static const gint64 pairs[] = { 250, 17, 250, 23 };
	g_autoptr(GArray) tiers = tiers_of(pairs, 2);
	VentureSeriesSummary summary;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_series_math_normalise_tiers(tiers, &error));
	g_assert_cmpuint(tiers->len, ==, 1);
	g_assert_cmpint(g_array_index(tiers, VentureSeriesTier, 0).quantity, ==, 40);

	g_assert_true(venture_series_math_summarise(TIERS(tiers), &summary, &error));
	g_assert_cmpint(summary.market_value, ==, 250);
	g_assert_cmpint(summary.stddev, ==, 0);
	g_assert_cmpint(summary.median, ==, 250);
}

/*
 * The first 15% of units is taken whatever it holds. Here a doubling sits
 * at unit 6 of 100; cutting there would make the market value the price
 * of five underpriced units (100). The rule takes on past it to 30 units
 * and the 1.5-sigma trim then drops the five cheap ones:
 * 10 x 200 + 15 x 205 over 25 units = 203.
 */
static void
test_jump_inside_first_fifteen(void)
{
	static const gint64 pairs[] = { 100, 5, 200, 10, 205, 85 };
	g_autoptr(GArray) tiers = tiers_of(pairs, 3);
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(venture_series_math_market_value(TIERS(tiers), &error), ==,
	                203);
	g_assert_no_error(error);
}

/*
 * Past the first 15%, a 20% jump ends the sample: 20 units at 100, then
 * 130 (30% up) -- the market value is 100, not dragged up by the next
 * tier.
 */
static void
test_jump_after_fifteen_cuts(void)
{
	static const gint64 pairs[] = { 100, 20, 130, 80 };
	g_autoptr(GArray) tiers = tiers_of(pairs, 2);

	g_assert_cmpint(venture_series_math_market_value(TIERS(tiers), NULL), ==,
	                100);
}

/*
 * A rise just under 20% does not cut, and the sample stops at 30% of the
 * units anyway: 10 each at 100..109, 100 units, so 30 units at 100, 101,
 * 102 -- mean 101.
 */
static void
test_thirty_percent_cap(void)
{
	g_autoptr(GArray) tiers = NULL;
	gint64 pairs[20];
	guint i;

	for (i = 0; i < 10; i++)
	{
		pairs[i * 2] = 100 + i;
		pairs[i * 2 + 1] = 10;
	}

	tiers = tiers_of(pairs, 10);
	g_assert_cmpint(venture_series_math_market_value(TIERS(tiers), NULL), ==,
	                101);
}

/*
 * One unit listed at 1 copper is an outlier, not the market: the sample
 * is 1 @ 1 and 5 @ 100 (mean 83.5, sigma 36.9), the 1 is 82.5 from the
 * mean, beyond 1.5 sigma, and is dropped.
 */
static void
test_outlier_trimmed(void)
{
	static const gint64 pairs[] = { 1, 1, 100, 19 };
	g_autoptr(GArray) tiers = tiers_of(pairs, 2);

	g_assert_cmpint(venture_series_math_market_value(TIERS(tiers), NULL), ==,
	                100);
}

static void
test_percentiles(void)
{
	g_autoptr(GArray) tiers = NULL;
	gint64 pairs[20];
	guint i;

	for (i = 0; i < 10; i++)
	{
		pairs[i * 2] = (gint64)(i + 1) * 10;
		pairs[i * 2 + 1] = 1;
	}

	tiers = tiers_of(pairs, 10);

	/* Nearest rank: ceil(0.15 x 10) = 2, the second unit. */
	g_assert_cmpint(venture_series_math_percentile(TIERS(tiers), 15), ==, 20);
	g_assert_cmpint(venture_series_math_percentile(TIERS(tiers), 100), ==, 100);
	g_assert_cmpint(venture_series_math_percentile(TIERS(tiers), 1), ==, 10);
}

/*
 * Even counts average the middle two and round half to even: 1, 2, 3, 4
 * is 2.5 and rounds to 2; 1, 2, 4, 5 is exactly 3. NONE is skipped, not
 * read as zero.
 */
static void
test_median(void)
{
	static const gint64 odd[] = { 5, 1, 3 };
	static const gint64 even[] = { 4, 1, 3, 2 };
	static const gint64 even2[] = { 1, 2, 4, 5 };
	static const gint64 gappy[] = { VENTURE_SERIES_NONE, 7, VENTURE_SERIES_NONE };
	static const gint64 huge[] = { G_MAXINT64, G_MAXINT64 - 2 };

	g_assert_cmpint(venture_series_math_median(odd, 3), ==, 3);
	g_assert_cmpint(venture_series_math_median(even, 4), ==, 2);
	g_assert_cmpint(venture_series_math_median(even2, 4), ==, 3);
	g_assert_cmpint(venture_series_math_median(gappy, 3), ==, 7);

	/* Two values near the top of the range do not overflow their sum. */
	g_assert_cmpint(venture_series_math_median(huge, 2), ==, G_MAXINT64 - 1);
}

static void
test_div_round(void)
{
	g_assert_cmpint(venture_series_math_div_round(5, 2), ==, 2);
	g_assert_cmpint(venture_series_math_div_round(7, 2), ==, 4);
	g_assert_cmpint(venture_series_math_div_round(3, 2), ==, 2);
	g_assert_cmpint(venture_series_math_div_round(1, 2), ==, 0);
	g_assert_cmpint(venture_series_math_div_round(10, 3), ==, 3);
	g_assert_cmpint(venture_series_math_div_round(11, 3), ==, 4);
}

/* --- Overflow ------------------------------------------------------------------- */

/*
 * A commodity can carry millions of units at prices in the hundreds of
 * millions of copper. The sum of price times quantity is past 64 bits and
 * must still give the right mean; a quantity past 64 bits must be refused,
 * never wrapped into a small number.
 */
static void
test_overflow_safe(void)
{
	static const gint64 big[] = {
		G_GINT64_CONSTANT(1000000000000000), 1000000,
		G_GINT64_CONSTANT(1000000000000002), 1000000
	};
	static const gint64 merge[] = { 1, G_MAXINT64, 1, 1 };
	static const gint64 units[] = { 1, G_MAXINT64, 2, 1 };
	g_autoptr(GArray) tiers = tiers_of(big, 2);
	g_autoptr(GArray) merged = tiers_of(merge, 2);
	g_autoptr(GArray) many = tiers_of(units, 2);
	VentureSeriesSummary summary;
	VentureSeriesBulkCost cost;
	g_autoptr(GError) error = NULL;
	gint64 out;

	g_assert_true(venture_series_math_summarise(TIERS(tiers), &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpint(summary.mean, ==, G_GINT64_CONSTANT(1000000000000001));
	g_assert_cmpint(summary.units, ==, 2000000);

	g_assert_false(venture_series_math_normalise_tiers(merged, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(venture_series_math_summarise(TIERS(many), &summary, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Buying the whole of that book costs more than 64 bits can say. */
	g_assert_false(venture_series_math_bulk_cost(TIERS(tiers), 1000000000, &cost,
	                                             &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	{
		static const gint64 dear[] = { G_MAXINT64 / 2, 3 };
		g_autoptr(GArray) expensive = tiers_of(dear, 1);

		g_assert_false(venture_series_math_bulk_cost(TIERS(expensive), 3, &cost,
		                                             &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
	}

	g_assert_false(venture_series_math_add(G_MAXINT64, 1, &out));
	g_assert_false(venture_series_math_mul(G_MAXINT64 / 2, 3, &out));
	g_assert_true(venture_series_math_add(G_MAXINT64 - 1, 1, &out));
	g_assert_cmpint(out, ==, G_MAXINT64);
}

static void
test_normalise_refuses(void)
{
	static const gint64 negative[] = { -1, 1 };
	static const gint64 empty[] = { 5, 0 };
	static const gint64 shuffled[] = { 30, 1, 10, 2, 20, 3, 10, 4 };
	g_autoptr(GArray) bad_price = tiers_of(negative, 1);
	g_autoptr(GArray) bad_quantity = tiers_of(empty, 1);
	g_autoptr(GArray) sorted = tiers_of(shuffled, 4);
	g_autoptr(GError) error = NULL;

	g_assert_false(venture_series_math_normalise_tiers(bad_price, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(venture_series_math_normalise_tiers(bad_quantity, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_true(venture_series_math_normalise_tiers(sorted, &error));
	g_assert_cmpuint(sorted->len, ==, 3);
	g_assert_cmpint(g_array_index(sorted, VentureSeriesTier, 0).price, ==, 10);
	g_assert_cmpint(g_array_index(sorted, VentureSeriesTier, 0).quantity, ==, 6);
	g_assert_cmpint(g_array_index(sorted, VentureSeriesTier, 2).price, ==, 30);
}

/* --- Averages over days ------------------------------------------------------------ */

/*
 * The weights are a fixed table, so the documentation and every machine
 * agree to the digit: 0.5^(d / 2.1) times a million.
 */
static void
test_ewma_weights(void)
{
	static const gint64 constant[] = { 500, 500, 500, 500 };
	static const gint64 gapped[] = { 100, VENTURE_SERIES_NONE, 200 };
	static const gint64 none[] = { VENTURE_SERIES_NONE, VENTURE_SERIES_NONE };
	guint d;

	g_assert_cmpuint(venture_series_math_ewma_weight(0), ==, 1000000);
	g_assert_cmpuint(venture_series_math_ewma_weight(1), ==, 718873);
	g_assert_cmpuint(venture_series_math_ewma_weight(2), ==, 516779);
	g_assert_cmpuint(venture_series_math_ewma_weight(13), ==, 13692);
	g_assert_cmpuint(venture_series_math_ewma_weight(14), ==, 0);

	/* Each weight is the formula, rounded. */
	for (d = 0; d < VENTURE_SERIES_EWMA_DAYS; d++)
		g_assert_cmpuint(venture_series_math_ewma_weight(d), ==,
		                 (guint32)floor(1000000.0 * pow(0.5, d / 2.1) + 0.5));

	g_assert_cmpint(venture_series_math_ewma(constant, 4, NULL), ==, 500);

	/*
	 * A missing day is left out of both sums, not read as zero:
	 * (100 x 1000000 + 200 x 516779) / 1516779 = 134.07 -> 134.
	 */
	g_assert_cmpint(venture_series_math_ewma(gapped, 3, NULL), ==, 134);
	g_assert_cmpint(venture_series_math_ewma(none, 2, NULL), ==,
	                VENTURE_SERIES_NONE);
}

static void
test_mean(void)
{
	static const gint64 values[] = { 1, 2, VENTURE_SERIES_NONE, 4 };

	/* 7 / 3 = 2.33 -> 2. */
	g_assert_cmpint(venture_series_math_mean(values, 4, NULL), ==, 2);
}

/*
 * Fewer than 15 venues: the deal price is the median. From 15, it is the
 * venue a third of the way up -- index floor(n / 3) of the sorted list.
 */
static void
test_deal_price(void)
{
	gint64 fourteen[14];
	gint64 fifteen[15];
	guint i;

	for (i = 0; i < 14; i++)
		fourteen[i] = 14 - i;
	for (i = 0; i < 15; i++)
		fifteen[i] = 15 - i;

	/* 1..14: median 7.5, half to even is 8. */
	g_assert_cmpint(venture_series_math_deal_price(fourteen, 14), ==, 8);
	g_assert_cmpint(venture_series_math_p33(fourteen, 14), ==, 5);

	/* 1..15: floor(15 / 3) = 5, the sixth cheapest: 6, not the median 8. */
	g_assert_cmpint(venture_series_math_deal_price(fifteen, 15), ==, 6);
	g_assert_cmpint(venture_series_math_median(fifteen, 15), ==, 8);

	/* NONE venues do not count towards the fifteen. */
	fifteen[0] = VENTURE_SERIES_NONE;
	g_assert_cmpint(venture_series_math_deal_price(fifteen, 15), ==,
	                venture_series_math_median(fifteen, 15));
}

/* --- Bulk cost --------------------------------------------------------------------- */

static void
test_bulk_cost(void)
{
	static const gint64 pairs[] = { 10, 5, 20, 5 };
	g_autoptr(GArray) tiers = tiers_of(pairs, 2);
	VentureSeriesBulkCost cost;
	g_autoptr(GError) error = NULL;

	/* 5 at 10 then 2 at 20: 90, average 12.86 -> 13. */
	g_assert_true(venture_series_math_bulk_cost(TIERS(tiers), 7, &cost, &error));
	g_assert_cmpint(cost.cost, ==, 90);
	g_assert_cmpint(cost.filled, ==, 7);
	g_assert_cmpint(cost.worst_price, ==, 20);
	g_assert_cmpint(cost.average, ==, 13);
	g_assert_true(cost.complete);

	/* Running out says so, with what it could fill. */
	g_assert_true(venture_series_math_bulk_cost(TIERS(tiers), 20, &cost, &error));
	g_assert_cmpint(cost.filled, ==, 10);
	g_assert_cmpint(cost.cost, ==, 150);
	g_assert_false(cost.complete);

	g_assert_false(venture_series_math_bulk_cost(TIERS(tiers), 0, &cost, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- Sale estimate ------------------------------------------------------------------ */

/*
 * Over one hour (3600 s): a listing with two hours left that vanished was
 * bought; one that could have expired is counted as expired; a stack that
 * shrank sold the difference.
 */
static void
test_sale_estimate(void)
{
	VentureSeriesListingMark previous[4];
	VentureSeriesListingMark current[2];
	g_autoptr(GArray) sales = NULL;
	g_autoptr(GError) error = NULL;
	const VentureSeriesSale *a;
	const VentureSeriesSale *b;

	memset(previous, 0, sizeof(previous));
	memset(current, 0, sizeof(current));

	previous[0] = (VentureSeriesListingMark){ 1, 1, 10, 1, 7200 };
	previous[1] = (VentureSeriesListingMark){ 2, 1, 12, 1, 0 };
	previous[2] = (VentureSeriesListingMark){ 3, 2, 5, 10, -1 };
	previous[3] = (VentureSeriesListingMark){ 4, 2, 5, 5, 7200 };

	current[0] = (VentureSeriesListingMark){ 3, 2, 5, 4, -1 };
	current[1] = (VentureSeriesListingMark){ 5, 2, 6, 1, 7200 };

	sales = venture_series_math_sale_estimate(previous, 4, current, 2, 3600,
	                                          &error);
	g_assert_no_error(error);
	g_assert_cmpuint(sales->len, ==, 2);

	a = &g_array_index(sales, VentureSeriesSale, 0);
	b = &g_array_index(sales, VentureSeriesSale, 1);

	g_assert_cmpint(a->instrument, ==, 1);
	g_assert_cmpint(a->sold_units, ==, 1);
	g_assert_cmpint(a->sold_value, ==, 10);
	g_assert_cmpint(a->expired_units, ==, 1);

	g_assert_cmpint(b->instrument, ==, 2);
	g_assert_cmpint(b->sold_units, ==, 11);
	g_assert_cmpint(b->sold_value, ==, 55);
	g_assert_cmpint(b->expired_units, ==, 0);

	/* An unsorted list would silently misread every listing after it. */
	g_clear_pointer(&sales, g_array_unref);
	previous[1].id = 0;
	sales = venture_series_math_sale_estimate(previous, 4, current, 2, 3600,
	                                          &error);
	g_assert_null(sales);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- Heat matrix and intervals ------------------------------------------------------ */

/*
 * 2026-09-28 is a Monday. Two values at 01:00 UTC average into one cell;
 * read two hours west of UTC the same instants fall on Sunday at 23:00.
 */
static void
test_heat(void)
{
	static const gint64 monday = G_GINT64_CONSTANT(1790553600);
	gint64 times[3];
	gint64 values[3];
	VentureSeriesHeat heat;
	g_autoptr(GError) error = NULL;

	times[0] = monday + 3600;
	times[1] = monday + 7 * 86400 + 3600;
	times[2] = monday + 3600;
	values[0] = 10;
	values[1] = 11;
	values[2] = VENTURE_SERIES_NONE;

	g_assert_true(venture_series_math_heat(times, values, 3, 0, &heat, &error));
	g_assert_cmpuint(heat.count[0][1], ==, 2);
	g_assert_cmpint(heat.value[0][1], ==, 10);
	g_assert_cmpint(heat.value[0][0], ==, VENTURE_SERIES_NONE);

	g_assert_true(venture_series_math_heat(times, values, 3, -7200, &heat,
	                                       &error));
	g_assert_cmpuint(heat.count[6][23], ==, 2);
	g_assert_cmpuint(heat.count[0][1], ==, 0);
}

/*
 * The interval: the median of every three-gap window's maximum, capped at
 * two hours. One early double update does not halve it, and one outage
 * in a long history does not move it.
 */
static void
test_learn_interval(void)
{
	static const gint64 double_update[] = { 3600, 3600, 60, 3600, 3600 };
	static const gint64 junk[] = { 0, -5 };
	static const gint64 slow[] = { 10000, 10000, 10000, 10000 };
	gint64 outage[11];
	guint i;

	g_assert_cmpint(venture_series_math_learn_interval(NULL, 0), ==,
	                VENTURE_SERIES_INTERVAL_DEFAULT);
	g_assert_cmpint(venture_series_math_learn_interval(junk, 2), ==,
	                VENTURE_SERIES_INTERVAL_DEFAULT);
	g_assert_cmpint(venture_series_math_learn_interval(double_update, 5), ==,
	                3600);
	g_assert_cmpint(venture_series_math_learn_interval(slow, 4), ==,
	                VENTURE_SERIES_INTERVAL_CAP);

	for (i = 0; i < 11; i++)
		outage[i] = 1800;
	outage[5] = 86400;

	g_assert_cmpint(venture_series_math_learn_interval(outage, 11), ==, 1800);
}

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/series-math/empty", test_empty);
	g_test_add_func("/series-math/single", test_single);
	g_test_add_func("/series-math/all-equal", test_all_equal);
	g_test_add_func("/series-math/jump-inside-first-fifteen",
	                test_jump_inside_first_fifteen);
	g_test_add_func("/series-math/jump-after-fifteen-cuts",
	                test_jump_after_fifteen_cuts);
	g_test_add_func("/series-math/thirty-percent-cap", test_thirty_percent_cap);
	g_test_add_func("/series-math/outlier-trimmed", test_outlier_trimmed);
	g_test_add_func("/series-math/percentiles", test_percentiles);
	g_test_add_func("/series-math/median", test_median);
	g_test_add_func("/series-math/div-round", test_div_round);
	g_test_add_func("/series-math/overflow-safe", test_overflow_safe);
	g_test_add_func("/series-math/normalise-refuses", test_normalise_refuses);
	g_test_add_func("/series-math/ewma-weights", test_ewma_weights);
	g_test_add_func("/series-math/mean", test_mean);
	g_test_add_func("/series-math/deal-price", test_deal_price);
	g_test_add_func("/series-math/bulk-cost", test_bulk_cost);
	g_test_add_func("/series-math/sale-estimate", test_sale_estimate);
	g_test_add_func("/series-math/heat", test_heat);
	g_test_add_func("/series-math/learn-interval", test_learn_interval);

	return g_test_run();
}
