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

	return g_test_run();
}
