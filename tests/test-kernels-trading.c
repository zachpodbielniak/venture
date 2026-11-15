/*
 * test-kernels-trading.c - The trading kernels through their generated C API
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * vk_arbitrage.tlw, vk_fees.tlw and vk_series.tlw test themselves (`make
 * kernels-check`). This suite tests them from the side VENTURE uses: the
 * vk_* functions `tallow render` writes, called the way the shims call
 * them, the shims' behaviour where the move had to preserve something
 * subtle, and one regression case for each bug the move fixed (each of
 * those fails on the C the kernel replaced).
 */

#include <venture.h>
#include <vk-kernels.h>

#include <math.h>

/* --- vk_arbitrage ---------------------------------------------------------- */

/*
 * round_minor is nearbyint() in the default rounding mode, written without
 * libm's rounding mode: every tie, both signs, the edge of 2^53, and a
 * sweep of ordinary values must agree bit for bit with what the C used.
 */
static void
test_kernels_arbitrage_round_minor(void)
{
	static const gdouble ties[] = {
		0.5, 1.5, 2.5, 3.5, -0.5, -1.5, -2.5, -3.5, 4503599627370495.5,
		-4503599627370495.5, 0.49999999999999994, -0.49999999999999994,
		1e-300, -1e-300, 9007199254740991.0, 9007199254740992.0, -9007199254740992.0
	};
	GRand *rand;
	TlwOptI64 minor;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(ties); i++)
	{
		minor = vk_arbitrage_round_minor(ties[i]);
		g_assert_true(minor.has);
		g_assert_cmpint(minor.val, ==, (gint64)nearbyint(ties[i]));
	}

	rand = g_rand_new_with_seed(20261115);

	for (i = 0; i < 100000; i++)
	{
		gdouble value;

		value = g_rand_double_range(rand, -1e12, 1e12) / (gdouble)(1 + g_rand_int_range(rand, 0, 1000));
		minor = vk_arbitrage_round_minor(value);
		g_assert_true(minor.has);
		g_assert_cmpint(minor.val, ==, (gint64)nearbyint(value));
	}

	g_rand_free(rand);

	g_assert_false(vk_arbitrage_round_minor(9007199254740994.0).has);
	g_assert_false(vk_arbitrage_round_minor(NAN).has);
	g_assert_false(vk_arbitrage_round_minor(INFINITY).has);
}

/* Odds at the store's scale round against the person, and a stake times
 * odds rounds toward negative infinity. */
static void
test_kernels_arbitrage_scaled(void)
{
	TlwOptI64 scaled;
	TlwOptI64 paid;

	scaled = vk_arbitrage_scaled(1.13, FALSE);
	g_assert_true(scaled.has);
	g_assert_cmpint(scaled.val, ==, 1130000);

	scaled = vk_arbitrage_scaled(217.0 / 117.0 + 1.0, TRUE);
	g_assert_true(scaled.has);
	g_assert_cmpint(scaled.val, ==, 2854701);

	g_assert_false(vk_arbitrage_scaled(1.0, FALSE).has);
	g_assert_false(vk_arbitrage_scaled(1e10, FALSE).has);

	paid = vk_arbitrage_times_odds(-1, 1500000);
	g_assert_true(paid.has);
	g_assert_cmpint(paid.val, ==, -2);

	g_assert_false(vk_arbitrage_times_odds(G_MAXINT64 / 2, 1500000).has);
}

/*
 * venture_arbitrage_roi divided raw minor units, so a net in cents over
 * capital in thousandths came out ten times too small: 1.50 over 10.000
 * is 15%, not 1.5%.
 */
static void
test_kernels_arbitrage_roi_exponent(void)
{
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) capital = NULL;
	g_autoptr(GError) error = NULL;
	gdouble roi;

	net = venture_money_new(150, "USD", 2);
	capital = venture_money_new(10000, "USD", 3);
	roi = 0.0;
	g_assert_true(venture_arbitrage_roi(net, capital, &roi, &error));
	g_assert_no_error(error);
	g_assert_cmpfloat_with_epsilon(roi, 0.15, 1e-12);
}

/*
 * A flip whose sale price has more decimals than its cost: the net lands
 * at the sale's exponent and the capital at the cost's, and the ROI came
 * from their raw minor units -- 2.000 over 10.00 read as 2000 / 1000.
 */
static void
test_kernels_arbitrage_flip_exponent(void)
{
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) gross = NULL;
	VentureArbitrageFlipInput input;
	VentureArbitrageFlip flip;
	g_autoptr(GError) error = NULL;

	cost = venture_money_new(1000, "USD", 2);
	gross = venture_money_new(12000, "USD", 3);
	venture_arbitrage_flip_input_init(&input);
	input.buy_cost = cost;
	input.sell_gross = gross;

	g_assert_true(venture_arbitrage_flip(&input, &flip, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(flip.net), ==, 2000);
	g_assert_cmpuint(venture_money_get_exponent(flip.net), ==, 3);
	g_assert_cmpfloat_with_epsilon(flip.roi, 0.2, 1e-12);
	venture_arbitrage_flip_clear(&flip);
}

/*
 * The arbitrage percent now goes through vk_percent; what the hand-written
 * reader accepted at its edges it still accepts, and what it refused it
 * still refuses: a sign that is nothing, a fraction of a percent past a
 * million, and anything at all after a '%'.
 */
static void
test_kernels_arbitrage_percent_edges(void)
{
	g_autoptr(GError) error = NULL;
	gint64 ppm;

	ppm = 1;
	g_assert_true(venture_arbitrage_parse_percent("-0", FALSE, &ppm, &error));
	g_assert_no_error(error);
	g_assert_cmpint(ppm, ==, 0);

	g_assert_true(venture_arbitrage_parse_percent("1000000.9999", FALSE, &ppm, &error));
	g_assert_no_error(error);
	g_assert_cmpint(ppm, ==, G_GINT64_CONSTANT(10000009999));

	g_assert_true(venture_arbitrage_parse_percent("-1000000.5", TRUE, &ppm, &error));
	g_assert_cmpint(ppm, ==, G_GINT64_CONSTANT(-10000005000));

	g_assert_true(venture_arbitrage_parse_percent("5 %", FALSE, &ppm, &error));
	g_assert_cmpint(ppm, ==, 50000);

	g_assert_false(venture_arbitrage_parse_percent("1000001", FALSE, &ppm, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(venture_arbitrage_parse_percent("5% ", FALSE, &ppm, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(venture_arbitrage_parse_percent("   ", FALSE, &ppm, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_false(venture_arbitrage_parse_percent("-2", FALSE, &ppm, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* The split straight from the kernel: stakes and the residual sum to the
 * total, and the least payout is what every outcome is sure to return. */
static void
test_kernels_arbitrage_surebet(void)
{
	g_autoptr(GArray) odds = NULL;
	g_autoptr(VkArbitrageSurebet) split = NULL;
	static const gdouble values[] = { 3.3, 3.3, 3.3 };
	gint64 sum;
	guint i;

	odds = g_array_new(FALSE, FALSE, sizeof(gdouble));
	g_array_append_vals(odds, values, G_N_ELEMENTS(values));
	split = vk_arbitrage_surebet(odds, 100);
	g_assert_cmpint(split->tag, ==, VK_ARBITRAGE_SUREBET_SPLIT);

	sum = 0;
	for (i = 0; i < split->u.split.stakes->len; i++)
	{
		sum += g_array_index(split->u.split.stakes, gint64, i);
		g_assert_cmpint(g_array_index(split->u.split.payouts, gint64, i), >=,
		                split->u.split.worst);
	}

	g_assert_cmpint(sum, ==, split->u.split.staked);
	g_assert_cmpint(sum, ==, 99);
	g_assert_cmpint(split->u.split.worst, ==, 108);
}

/* --- vk_fees --------------------------------------------------------------- */

static TlwOptI64
some_minor(gint64 value)
{
	TlwOptI64 out;

	out.has = TRUE;
	out.val = value;

	return out;
}

static TlwOptI64
no_minor(void)
{
	TlwOptI64 out;

	out.has = FALSE;
	out.val = 0;

	return out;
}

/*
 * The percent model combines its parts the way VentureMoney.add does: a
 * fixed fee in whole units joins a cut in cents at the cents, and a floor
 * written in thousandths replaces a smaller fee at its own exponent.
 */
static void
test_kernels_fees_percent(void)
{
	g_autoptr(VkFeesFee) fee = NULL;
	g_autoptr(VkFeesFee) floor_fee = NULL;
	g_autoptr(VkFeesFee) overflow = NULL;

	/* 5% of 100.00 is 5.00, plus 3 x 2 units at exponent 0, plus 0.25 */
	fee = vk_fees_percent(10000, 2, 50000, some_minor(3), 0, 2, some_minor(25), 2, no_minor(), 0);
	g_assert_cmpint(fee->tag, ==, VK_FEES_FEE_CHARGED);
	g_assert_cmpint(fee->u.charged.fee, ==, 1125);
	g_assert_cmpint(fee->u.charged.exponent, ==, 2);

	/* 1% of 1.00 is 0.01, under a floor of 0.050 */
	floor_fee = vk_fees_percent(100, 2, 10000, no_minor(), 0, 1, no_minor(), 0, some_minor(50), 3);
	g_assert_cmpint(floor_fee->tag, ==, VK_FEES_FEE_CHARGED);
	g_assert_cmpint(floor_fee->u.charged.fee, ==, 50);
	g_assert_cmpint(floor_fee->u.charged.exponent, ==, 3);

	overflow = vk_fees_percent(100, 2, 10000, some_minor(2), 0, G_MAXINT64, no_minor(), 0,
	                           no_minor(), 0);
	g_assert_cmpint(overflow->tag, ==, VK_FEES_FEE_UNITS);
}

/* Only a positive amount on the selling side pays commission. */
static void
test_kernels_fees_commission(void)
{
	TlwOptI64 fee;

	fee = vk_fees_commission(1001, 50000, TRUE);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, 50);

	fee = vk_fees_commission(1000, 50000, FALSE);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, 0);

	fee = vk_fees_commission(-1000, 50000, TRUE);
	g_assert_true(fee.has);
	g_assert_cmpint(fee.val, ==, 0);

	g_assert_cmpfloat(vk_fees_commission_share(50000), ==, 0.05);
}

/* --- vk_series ------------------------------------------------------------- */

static GArray *
int64_array(
	const gint64	*values,
	guint		 n
){
	GArray *array;

	array = g_array_sized_new(FALSE, FALSE, sizeof(gint64), n);
	g_array_append_vals(array, values, n);

	return array;
}

/*
 * The exact weighted mean without a 128-bit accumulator: sums far past 64
 * bits, ties half to even, and a divisor near 2^63 -- the size at which the
 * C's 64-bit fallback doubled a remainder past the range.
 */
static void
test_kernels_series_exact_mean(void)
{
	static const gint64 top[] = { G_MAXINT64, G_MAXINT64 };
	static const gint64 top_weights[] = { G_GINT64_CONSTANT(4611686018427387904),
	                                      G_GINT64_CONSTANT(4611686018427387903) };
	static const gint64 bits[] = { 0, 1 };
	static const gint64 tie[] = { 2, 3 };
	static const gint64 ones[] = { 1, 1 };
	g_autoptr(GArray) values = NULL;
	g_autoptr(GArray) weights = NULL;
	g_autoptr(GArray) tie_values = NULL;
	g_autoptr(GArray) tie_weights = NULL;
	g_autoptr(GArray) bit_values = NULL;
	TlwOptI64 mean;

	values = int64_array(top, 2);
	weights = int64_array(top_weights, 2);
	mean = vk_series_weighted_mean(values, weights);
	g_assert_true(mean.has);
	g_assert_cmpint(mean.val, ==, G_MAXINT64);

	/* 1 x (2^62 - 1) over 2^63 - 1 is a hair under a half; 1 x 2^62 a
	 * hair over */
	bit_values = int64_array(bits, 2);
	mean = vk_series_weighted_mean(bit_values, weights);
	g_assert_true(mean.has);
	g_assert_cmpint(mean.val, ==, 0);

	g_array_index(weights, gint64, 0) = G_GINT64_CONSTANT(4611686018427387903);
	g_array_index(weights, gint64, 1) = G_GINT64_CONSTANT(4611686018427387904);
	mean = vk_series_weighted_mean(bit_values, weights);
	g_assert_true(mean.has);
	g_assert_cmpint(mean.val, ==, 1);

	tie_values = int64_array(tie, 2);
	tie_weights = int64_array(ones, 2);
	mean = vk_series_weighted_mean(tie_values, tie_weights);
	g_assert_true(mean.has);
	g_assert_cmpint(mean.val, ==, 2);

	g_assert_cmpint(vk_series_midpoint(G_MAXINT64, G_MAXINT64 - 1), ==, G_MAXINT64 - 1);
	g_assert_cmpint(venture_series_math_div_round(G_MAXINT64 - 1, G_MAXINT64), ==, 1);
	g_assert_cmpint(venture_series_math_div_round(G_GINT64_CONSTANT(4611686018427387903),
	                                              G_MAXINT64), ==, 0);
}

/*
 * A book worth more than 64 bits: forty million units at three hundred
 * million copper, twice. Its mean, median and market value are exact.
 */
static void
test_kernels_series_wide_book(void)
{
	VentureSeriesTier tiers[2];
	VentureSeriesSummary summary;
	g_autoptr(GError) error = NULL;

	tiers[0].price = 300000000;
	tiers[0].quantity = 40000000;
	tiers[1].price = 300000001;
	tiers[1].quantity = 40000000;

	g_assert_true(venture_series_math_summarise(tiers, 2, &summary, &error));
	g_assert_no_error(error);
	g_assert_cmpint(summary.units, ==, 80000000);
	g_assert_cmpint(summary.mean, ==, 300000000);
	g_assert_cmpint(summary.median, ==, 300000000);
	g_assert_cmpint(summary.market_value, ==, 300000000);
	g_assert_cmpint(summary.stddev, ==, 1);
}

static gint
compare_int64(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 left;
	gint64 right;

	left = *(const gint64 *)a;
	right = *(const gint64 *)b;

	return (left < right) ? -1 : ((left > right) ? 1 : 0);
}

/* The kernel's heap sort is qsort(): integers have one sorted order. */
static void
test_kernels_series_sorted(void)
{
	GRand *rand;
	guint round;

	rand = g_rand_new_with_seed(20261115);

	for (round = 0; round < 500; round++)
	{
		g_autoptr(GArray) values = NULL;
		g_autoptr(GArray) sorted = NULL;
		gint64 expected[97];
		guint n;
		guint i;

		n = (guint)g_rand_int_range(rand, 0, 97);
		for (i = 0; i < n; i++)
			expected[i] = (gint64)g_rand_int_range(rand, -50, 50) * G_GINT64_CONSTANT(100000000000);

		values = int64_array(expected, n);
		sorted = vk_series_sorted(values);
		qsort(expected, n, sizeof(gint64), compare_int64);

		g_assert_cmpuint(sorted->len, ==, n);
		for (i = 0; i < n; i++)
			g_assert_cmpint(g_array_index(sorted, gint64, i), ==, expected[i]);
	}

	g_rand_free(rand);
}

/* A heat cell is the floor-division weekday and hour the C computed,
 * before and after 1970 and at the range's ends. */
static void
test_kernels_series_heat_cell(void)
{
	static const gint64 edges[] = { 0, -1, 86399, 86400, -86400, -86401, G_MAXINT64 };
	GRand *rand;
	guint i;

	rand = g_rand_new_with_seed(20261115);

	for (i = 0; i < 10000 + G_N_ELEMENTS(edges); i++)
	{
		gint64 local;
		gint64 days;
		gint64 seconds;
		gint64 day;

		local = (i < G_N_ELEMENTS(edges)) ? edges[i]
		        : (gint64)g_rand_double_range(rand, -1e12, 1e12);
		days = local / 86400;
		if (((local % 86400) != 0) && (local < 0))
			days--;
		seconds = local - days * 86400;
		day = ((days % 7) + 7 + 3) % 7;

		g_assert_cmpint(vk_series_heat_cell(local), ==, day * 24 + seconds / 3600);
	}

	g_rand_free(rand);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/kernels-trading/arbitrage/round-minor", test_kernels_arbitrage_round_minor);
	g_test_add_func("/kernels-trading/arbitrage/scaled", test_kernels_arbitrage_scaled);
	g_test_add_func("/kernels-trading/arbitrage/roi-exponent", test_kernels_arbitrage_roi_exponent);
	g_test_add_func("/kernels-trading/arbitrage/flip-exponent", test_kernels_arbitrage_flip_exponent);
	g_test_add_func("/kernels-trading/arbitrage/percent-edges", test_kernels_arbitrage_percent_edges);
	g_test_add_func("/kernels-trading/arbitrage/surebet", test_kernels_arbitrage_surebet);
	g_test_add_func("/kernels-trading/fees/percent", test_kernels_fees_percent);
	g_test_add_func("/kernels-trading/fees/commission", test_kernels_fees_commission);
	g_test_add_func("/kernels-trading/series/exact-mean", test_kernels_series_exact_mean);
	g_test_add_func("/kernels-trading/series/wide-book", test_kernels_series_wide_book);
	g_test_add_func("/kernels-trading/series/sorted", test_kernels_series_sorted);
	g_test_add_func("/kernels-trading/series/heat-cell", test_kernels_series_heat_cell);

	return g_test_run();
}
