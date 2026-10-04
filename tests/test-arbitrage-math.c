/*
 * test-arbitrage-math.c - The arithmetic of arbitrage, and the fee models
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every figure the scan, the calculators and a recorded trade's expected
 * snapshot show comes out of venture-arbitrage-math.c and the fee models.
 * The numbers here are worked by hand (and checked in exact arithmetic)
 * so a change that quietly moves a stake, a payout or a cut fails here
 * rather than in somebody's bet. Tables first, then the edge cases that
 * used to be bugs elsewhere: rounding at exponent 0 and 4, odds that pay
 * nothing, a market that never sells, an answer too large to annualise.
 */

#include <venture.h>

#include <math.h>
#include <string.h>

static VentureMoney *
money(
	gint64		 minor,
	const gchar	*currency,
	guint8		 exponent
){
	return venture_money_new(minor, currency, exponent);
}

static gint64
minor_of(const VentureMoney *amount)
{
	g_assert_nonnull(amount);

	return venture_money_get_amount(amount);
}

/* ==========================================================================
 * Odds
 * ========================================================================== */

typedef struct
{
	const gchar		*text;
	VentureOddsFormat	 format;
	gdouble			 decimal;	/* NAN: refused */
} OddsCase;

/*
 * Every format, and what each refuses. What breaks if this regresses: a
 * moneyline read as decimal odds (+150 as 150.0), or odds of 1.0 -- which
 * pay back the stake and nothing more -- accepted into a surebet whose
 * every split then loses.
 */
static void
test_odds_formats(void)
{
	static const OddsCase cases[] = {
		{ "2.5", VENTURE_ODDS_FORMAT_AUTO, 2.5 },
		{ " 2.10 ", VENTURE_ODDS_FORMAT_AUTO, 2.1 },
		{ "+150", VENTURE_ODDS_FORMAT_AUTO, 2.5 },
		{ "-200", VENTURE_ODDS_FORMAT_AUTO, 1.5 },
		{ "+100", VENTURE_ODDS_FORMAT_AUTO, 2.0 },
		{ "-100", VENTURE_ODDS_FORMAT_AUTO, 2.0 },
		{ "100", VENTURE_ODDS_FORMAT_AMERICAN, 2.0 },
		{ "3/2", VENTURE_ODDS_FORMAT_AUTO, 2.5 },
		{ "11/10", VENTURE_ODDS_FORMAT_FRACTIONAL, 2.1 },
		{ "1/4", VENTURE_ODDS_FORMAT_AUTO, 1.25 },
		{ "evens", VENTURE_ODDS_FORMAT_AUTO, 2.0 },
		{ "2.5", VENTURE_ODDS_FORMAT_DECIMAL, 2.5 },
		/* Refused: pays nothing, not a moneyline, not a fraction. */
		{ "1.0", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "1", VENTURE_ODDS_FORMAT_DECIMAL, NAN },
		{ "0.5", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "+50", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "-99", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "0/1", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "1/0", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "1/2/3", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "2", VENTURE_ODDS_FORMAT_FRACTIONAL, NAN },
		{ "+150", VENTURE_ODDS_FORMAT_DECIMAL, 150.0 },
		{ "abc", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "", VENTURE_ODDS_FORMAT_AUTO, NAN },
		{ "nan", VENTURE_ODDS_FORMAT_DECIMAL, NAN },
		{ "inf", VENTURE_ODDS_FORMAT_DECIMAL, NAN },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(GError) error = NULL;
		gdouble value = 0.0;
		gboolean ok;

		ok = venture_arbitrage_odds_parse(cases[i].text, cases[i].format, &value, &error);

		if (isnan(cases[i].decimal))
		{
			if (ok)
				g_error("\"%s\" should be refused, read %g", cases[i].text, value);

			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
			continue;
		}

		if (!ok)
			g_error("\"%s\" refused: %s", cases[i].text, error->message);

		g_assert_cmpfloat_with_epsilon(value, cases[i].decimal, 1e-12);
	}
}

/*
 * The margin of a book: two outcomes at 1.90 carry an overround of about
 * 5.26%, and taking it out gives each a fair chance of one half. Every
 * set refuses odds of one or less and an empty set.
 */
static void
test_overround_and_no_vig(void)
{
	static const gdouble book[] = { 1.9, 1.9 };
	static const gdouble skewed[] = { 1.5, 3.0 };
	static const gdouble bad[] = { 2.0, 1.0 };
	g_autoptr(GError) error = NULL;
	gdouble overround;
	gdouble fair[2];

	g_assert_true(venture_arbitrage_overround(book, 2, &overround, NULL));
	g_assert_cmpfloat_with_epsilon(overround, 2.0 / 1.9 - 1.0, 1e-12);
	g_assert_true(venture_arbitrage_no_vig(book, 2, fair, NULL));
	g_assert_cmpfloat_with_epsilon(fair[0], 0.5, 1e-12);

	/* 1/1.5 + 1/3 is exactly one: no margin, and the fair chances are
	 * the implied ones. */
	g_assert_true(venture_arbitrage_overround(skewed, 2, &overround, NULL));
	g_assert_cmpfloat_with_epsilon(overround, 0.0, 1e-12);
	g_assert_true(venture_arbitrage_no_vig(skewed, 2, fair, NULL));
	g_assert_cmpfloat_with_epsilon(fair[0] + fair[1], 1.0, 1e-12);
	g_assert_cmpfloat_with_epsilon(fair[1], 1.0 / 3.0, 1e-12);

	g_assert_false(venture_arbitrage_overround(bad, 2, &overround, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_overround(book, 0, &overround, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);

	g_assert_cmpfloat_with_epsilon(venture_arbitrage_implied_probability(4.0), 0.25, 1e-12);
	g_assert_true(isnan(venture_arbitrage_implied_probability(1.0)));
}

/* ==========================================================================
 * Rounding and percents
 * ========================================================================== */

/*
 * Half to even, once, and refused past what a double holds exactly. What
 * breaks if this regresses: stakes that drift a cent per outcome in one
 * direction, or a huge amount silently rounded to a neighbour.
 */
static void
test_round_minor(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) big = NULL;
	g_autoptr(VentureMoney) scaled = NULL;
	gint64 out;

	g_assert_true(venture_arbitrage_round_minor(2.5, &out, NULL));
	g_assert_cmpint(out, ==, 2);
	g_assert_true(venture_arbitrage_round_minor(3.5, &out, NULL));
	g_assert_cmpint(out, ==, 4);
	g_assert_true(venture_arbitrage_round_minor(-2.5, &out, NULL));
	g_assert_cmpint(out, ==, -2);
	g_assert_true(venture_arbitrage_round_minor(10465.2, &out, NULL));
	g_assert_cmpint(out, ==, 10465);

	g_assert_false(venture_arbitrage_round_minor(1e300, &out, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_round_minor(NAN, &out, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Overflow is refused, not wrapped. */
	big = money(G_MAXINT64 / 2, "USD", 2);
	scaled = venture_arbitrage_money_scale(big, 4.0, &error);
	g_assert_null(scaled);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

typedef struct
{
	const gchar	*text;
	gboolean	 negative;
	gint64		 ppm;	/* -1: refused */
} PercentCase;

/*
 * A percent is read exactly: 4.75% is 47500 parts per million, never
 * 47499.99. What breaks if this regresses: a 4.75% cut charged as 4.74%.
 */
static void
test_percent(void)
{
	static const PercentCase cases[] = {
		{ "5", FALSE, 50000 },
		{ "2.5", FALSE, 25000 },
		{ "4.75", FALSE, 47500 },
		{ "0.0001", FALSE, 1 },
		{ "100", FALSE, 1000000 },
		{ "5%", FALSE, 50000 },
		{ " 12 ", FALSE, 120000 },
		{ "-5", TRUE, -50000 },
		{ "-5", FALSE, -1 },
		{ "0.00001", FALSE, -1 },
		{ "abc", FALSE, -1 },
		{ "", FALSE, -1 },
		{ "1000001", FALSE, -1 },
		{ "5x", FALSE, -1 },
		{ ".", FALSE, -1 },
		{ ".5", FALSE, 5000 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(GError) error = NULL;
		gint64 ppm = 0;
		gboolean ok;

		ok = venture_arbitrage_parse_percent(cases[i].text, cases[i].negative, &ppm, &error);

		if (cases[i].ppm == -1)
		{
			if (ok)
				g_error("\"%s\" should be refused", cases[i].text);

			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
			continue;
		}

		if (!ok)
			g_error("\"%s\" refused: %s", cases[i].text, error->message);

		g_assert_cmpint(ppm, ==, cases[i].ppm);
	}
}

/* ==========================================================================
 * Surebets
 * ========================================================================== */

typedef struct
{
	const gchar	*name;
	gdouble		 odds[3];
	guint		 n;
	gint64		 total;
	const gchar	*currency;
	guint8		 exponent;
	gboolean	 surebet;
	gint64		 stakes[3];
	gint64		 payouts[3];
	gint64		 residual;
	gint64		 guaranteed;
	gint64		 profit;
} SurebetCase;

/*
 * The split, worked in exact arithmetic for each case. Two outcomes at
 * 2.50 and 1.80 sum to 0.9556: a surebet worth T(1/S - 1). The same
 * split in yen (exponent 0) loses two yen to rounding against the ideal,
 * and in a four-place currency one minor unit -- each reported, never
 * hidden. Three outcomes at 3.30 cannot split 100 evenly: a residual of
 * one. S of exactly one (2.0, 2.0; three at 3.0) is no surebet, and S
 * above one is a sure loss. What breaks if this regresses: a calculator
 * that promises a profit the rounded stakes cannot deliver.
 */
static void
test_surebet_table(void)
{
	static const SurebetCase cases[] = {
		{ "usd", { 2.5, 1.8, 0 }, 2, 10000, "USD", 2, TRUE,
		  { 4186, 5814, 0 }, { 10465, 10465, 0 }, 0, 465, 465 },
		{ "yen", { 2.5, 1.8, 0 }, 2, 1000, "JPY", 0, TRUE,
		  { 419, 581, 0 }, { 1047, 1045, 0 }, 0, 45, 47 },
		{ "gold", { 2.5, 1.8, 0 }, 2, 1000000, "GOLD", 4, TRUE,
		  { 418605, 581395, 0 }, { 1046512, 1046511, 0 }, 0, 46511, 46512 },
		{ "residual", { 3.3, 3.3, 3.3 }, 3, 100, "JPY", 0, TRUE,
		  { 33, 33, 33 }, { 108, 108, 108 }, 1, 9, 10 },
		{ "even", { 2.0, 2.0, 0 }, 2, 10000, "USD", 2, FALSE,
		  { 5000, 5000, 0 }, { 10000, 10000, 0 }, 0, 0, 0 },
		{ "three-at-three", { 3.0, 3.0, 3.0 }, 3, 900, "JPY", 0, FALSE,
		  { 300, 300, 300 }, { 900, 900, 900 }, 0, 0, 0 },
		{ "overround", { 1.9, 1.9, 0 }, 2, 10000, "USD", 2, FALSE,
		  { 5000, 5000, 0 }, { 9500, 9500, 0 }, 0, -500, -500 },
	};
	guint i;
	guint j;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureMoney) total = NULL;
		g_autoptr(GError) error = NULL;
		VentureArbitrageSurebet split;

		total = money(cases[i].total, cases[i].currency, cases[i].exponent);

		if (!venture_arbitrage_surebet(cases[i].odds, cases[i].n, total, &split, &error))
			g_error("%s: %s", cases[i].name, error->message);

		if (split.is_surebet != cases[i].surebet)
			g_error("%s: a surebet should be %d (S = %.17g)", cases[i].name, cases[i].surebet,
			        split.sum);

		for (j = 0; j < cases[i].n; j++)
		{
			g_assert_cmpint(minor_of(g_ptr_array_index(split.stakes, j)), ==, cases[i].stakes[j]);
			g_assert_cmpint(minor_of(g_ptr_array_index(split.payouts, j)), ==, cases[i].payouts[j]);
			g_assert_cmpstr(venture_money_get_currency(g_ptr_array_index(split.stakes, j)), ==,
			                cases[i].currency);
		}

		/* What was asked for is what was staked plus what is left. */
		g_assert_cmpint(minor_of(split.staked) + minor_of(split.residual), ==, cases[i].total);
		g_assert_cmpint(minor_of(split.residual), ==, cases[i].residual);
		g_assert_cmpint(minor_of(split.guaranteed), ==, cases[i].guaranteed);
		g_assert_cmpint(minor_of(split.guaranteed), ==, minor_of(split.worst_payout) - minor_of(split.staked));
		g_assert_cmpint(minor_of(split.profit), ==, cases[i].profit);
		venture_arbitrage_surebet_clear(&split);
	}
}

/*
 * One outcome is not an event anybody can cover, odds of one pay
 * nothing, and a stake of nothing splits into nothing: each is refused
 * by name. A missing outcome is refused before the split is ever made.
 */
static void
test_surebet_refusals(void)
{
	static const gdouble one[] = { 2.0 };
	static const gdouble flat[] = { 2.0, 1.0 };
	static const gdouble fine[] = { 2.1, 2.1 };
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) nothing = NULL;
	g_autoptr(GError) error = NULL;
	VentureArbitrageSurebet split;

	total = money(10000, "USD", 2);
	nothing = money(0, "USD", 2);

	g_assert_false(venture_arbitrage_surebet(one, 1, total, &split, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_surebet(flat, 2, total, &split, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_surebet(fine, 2, nothing, &split, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_surebet(fine, 0, total, &split, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* ==========================================================================
 * Back and lay
 * ========================================================================== */

/*
 * Lay stake B*d/(L - c), liability lay*(L - 1), and both results. 3.0
 * backed against 2.9 laid at 5% commission is exactly break-even (rating
 * one); 3.2 against 3.0 at 2% makes 5.23 either way, a cent apart only
 * by rounding. The effective odds are the formulas in docs/arbitrage.org.
 * What breaks if this regresses: a lay stake that leaves one result a
 * loss.
 */
static void
test_back_lay(void)
{
	g_autoptr(VentureMoney) stake = NULL;
	g_autoptr(GError) error = NULL;
	VentureArbitrageBackLay figures;

	stake = money(10000, "USD", 2);

	g_assert_cmpfloat_with_epsilon(venture_arbitrage_effective_back_odds(3.0, 0.05), 2.9, 1e-12);
	g_assert_cmpfloat_with_epsilon(venture_arbitrage_effective_lay_odds(2.9, 0.05), 3.0, 1e-12);
	g_assert_true(isnan(venture_arbitrage_effective_back_odds(3.0, 1.0)));
	g_assert_true(isnan(venture_arbitrage_effective_lay_odds(1.0, 0.05)));

	g_assert_true(venture_arbitrage_back_lay(3.0, 2.9, 0.05, stake, &figures, &error));
	g_assert_no_error(error);
	g_assert_cmpint(minor_of(figures.lay_stake), ==, 10526);
	g_assert_cmpint(minor_of(figures.liability), ==, 19999);
	g_assert_cmpint(minor_of(figures.if_back_wins), ==, 1);
	g_assert_cmpint(minor_of(figures.if_lay_wins), ==, 0);
	g_assert_cmpint(minor_of(figures.worst), ==, 0);
	g_assert_cmpfloat_with_epsilon(figures.rating, 1.0, 1e-9);
	venture_arbitrage_back_lay_clear(&figures);

	g_assert_true(venture_arbitrage_back_lay(3.2, 3.0, 0.02, stake, &figures, &error));
	g_assert_cmpint(minor_of(figures.lay_stake), ==, 10738);
	g_assert_cmpint(minor_of(figures.liability), ==, 21476);
	g_assert_cmpint(minor_of(figures.if_back_wins), ==, 524);
	g_assert_cmpint(minor_of(figures.if_lay_wins), ==, 523);
	g_assert_cmpint(minor_of(figures.worst), ==, 523);
	g_assert_cmpint(minor_of(figures.ideal), ==, 523);
	g_assert_cmpfloat(figures.rating, >, 1.05);
	venture_arbitrage_back_lay_clear(&figures);

	/* A commission of all the winnings, and odds that pay nothing. */
	g_assert_false(venture_arbitrage_back_lay(3.0, 2.9, 1.0, stake, &figures, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_back_lay(1.0, 2.9, 0.05, stake, &figures, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* ==========================================================================
 * Flips
 * ========================================================================== */

/*
 * A sale rate of one in four means three listings expire before one
 * sells; none is no relist; zero is "never sells", refused by name --
 * relisting forever would lose every deposit -- and so is anything that
 * is not a share.
 */
static void
test_relists(void)
{
	g_autoptr(GError) error = NULL;
	gdouble relists;

	g_assert_true(venture_arbitrage_expected_relists(0.25, &relists, NULL));
	g_assert_cmpfloat_with_epsilon(relists, 3.0, 1e-12);
	g_assert_true(venture_arbitrage_expected_relists(1.0, &relists, NULL));
	g_assert_cmpfloat_with_epsilon(relists, 0.0, 1e-12);

	g_assert_false(venture_arbitrage_expected_relists(0.0, &relists, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "Never sells"));
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_expected_relists(NAN, &relists, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_expected_relists(1.5, &relists, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * (1 + ROI)^(365/days) - 1. A year's hold is its own ROI; a total loss
 * stays a total loss; no hold at all, and a 10% return in a fraction of a
 * second (an overflow, not a number), are refused.
 */
static void
test_annualize(void)
{
	g_autoptr(GError) error = NULL;
	gdouble out;

	g_assert_true(venture_arbitrage_annualize(0.1, 365.0, &out, NULL));
	g_assert_cmpfloat_with_epsilon(out, 0.1, 1e-12);
	g_assert_true(venture_arbitrage_annualize(0.1, 36.5, &out, NULL));
	g_assert_cmpfloat_with_epsilon(out, pow(1.1, 10.0) - 1.0, 1e-9);
	g_assert_true(venture_arbitrage_annualize(-1.0, 3.0, &out, NULL));
	g_assert_cmpfloat(out, ==, -1.0);
	g_assert_true(venture_arbitrage_annualize(-2.0, 3.0, &out, NULL));
	g_assert_cmpfloat(out, ==, -1.0);
	g_assert_true(venture_arbitrage_annualize(0.0, 1.0, &out, NULL));
	g_assert_cmpfloat(out, ==, 0.0);

	g_assert_false(venture_arbitrage_annualize(0.1, 0.0, &out, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_annualize(0.1, 1e-6, &out, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * One flip worked by hand: 10 units bought for 100.00, sold for 150.00
 * less a 5% cut, a 3.00 refundable deposit at a sale rate of 25% (three
 * relists: 9.00 lost), 1.00 to move. Net 32.50 on capital 104.00 (cost,
 * one deposit, the move): ROI 31.25%. Two sold a day and half the market
 * is ten days: 3.125% a day. EV at 25%: 8.12 (half to even) less 3.00 of
 * a 4.00 unwind. Not refundable, the last deposit goes too. What breaks
 * if this regresses: every number on the scan page and the calculator.
 */
static void
test_flip(void)
{
	g_autoptr(VentureMoney) cost = money(10000, "USD", 2);
	g_autoptr(VentureMoney) gross = money(15000, "USD", 2);
	g_autoptr(VentureMoney) fees = money(750, "USD", 2);
	g_autoptr(VentureMoney) deposit = money(300, "USD", 2);
	g_autoptr(VentureMoney) transfer = money(100, "USD", 2);
	g_autoptr(VentureMoney) unwind = money(400, "USD", 2);
	g_autoptr(VentureMoney) euros = money(100, "EUR", 2);
	g_autoptr(GError) error = NULL;
	VentureArbitrageFlipInput input;
	VentureArbitrageFlip flip;

	venture_arbitrage_flip_input_init(&input);
	input.buy_cost = cost;
	input.sell_gross = gross;
	input.sell_fees = fees;
	input.deposit = deposit;
	input.deposit_refundable = TRUE;
	input.sale_rate = 0.25;
	input.transfer_cost = transfer;
	input.units = 10;
	input.sold_per_day = 2.0;
	input.share = 0.5;
	input.unwind_loss = unwind;

	g_assert_true(venture_arbitrage_flip(&input, &flip, &error));
	g_assert_no_error(error);
	g_assert_cmpfloat_with_epsilon(flip.relists, 3.0, 1e-12);
	g_assert_cmpint(minor_of(flip.listing_loss), ==, 900);
	g_assert_cmpint(minor_of(flip.net), ==, 3250);
	g_assert_cmpint(minor_of(flip.capital), ==, 10400);
	g_assert_cmpfloat_with_epsilon(flip.roi, 0.3125, 1e-12);
	g_assert_cmpfloat_with_epsilon(flip.lock_days, 10.0, 1e-12);
	g_assert_cmpfloat_with_epsilon(flip.roi_per_day, 0.03125, 1e-12);
	g_assert_cmpfloat_with_epsilon(flip.annualized, pow(1.3125, 36.5) - 1.0, 1e-6);
	g_assert_cmpint(minor_of(flip.ev), ==, 812 - 300);
	venture_arbitrage_flip_clear(&flip);

	/* Not refunded: four deposits lost, not three. */
	input.deposit_refundable = FALSE;
	g_assert_true(venture_arbitrage_flip(&input, &flip, NULL));
	g_assert_cmpint(minor_of(flip.listing_loss), ==, 1200);
	g_assert_cmpint(minor_of(flip.net), ==, 2950);
	venture_arbitrage_flip_clear(&flip);

	/* Unknown sale rate: no relist counted, no EV, no lock without a
	 * pace of sales. */
	input.deposit_refundable = TRUE;
	input.sale_rate = NAN;
	input.sold_per_day = NAN;
	g_assert_true(venture_arbitrage_flip(&input, &flip, NULL));
	g_assert_true(isnan(flip.relists));
	g_assert_cmpint(minor_of(flip.listing_loss), ==, 0);
	g_assert_null(flip.ev);
	g_assert_true(isnan(flip.lock_days));
	g_assert_true(isnan(flip.annualized));
	venture_arbitrage_flip_clear(&flip);

	/* Never sells. */
	input.sale_rate = 0.0;
	g_assert_false(venture_arbitrage_flip(&input, &flip, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* A spread of nothing, and one below: net zero and negative, never
	 * refused -- the scan's filter is what leaves them out. */
	input.sale_rate = 1.0;
	input.sell_fees = NULL;
	input.deposit = NULL;
	input.transfer_cost = NULL;
	input.sell_gross = cost;
	g_assert_true(venture_arbitrage_flip(&input, &flip, NULL));
	g_assert_cmpint(minor_of(flip.net), ==, 0);
	g_assert_cmpfloat(flip.roi, ==, 0.0);
	venture_arbitrage_flip_clear(&flip);
	input.sell_gross = fees;
	g_assert_true(venture_arbitrage_flip(&input, &flip, NULL));
	g_assert_cmpint(minor_of(flip.net), ==, 750 - 10000);
	venture_arbitrage_flip_clear(&flip);

	/* Nothing here adds across currencies. */
	input.sell_gross = gross;
	input.transfer_cost = euros;
	g_assert_false(venture_arbitrage_flip(&input, &flip, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * The four factors and their documented defaults. What breaks if this
 * regresses: the min_confidence filter judging against a different
 * number than docs/arbitrage.org promises.
 */
static void
test_confidence(void)
{
	VentureArbitrageEvidence evidence;
	g_autoptr(VentureMoney) net = money(500, "USD", 2);
	g_autoptr(VentureMoney) capital = money(2000, "USD", 2);
	g_autoptr(VentureMoney) zero = money(0, "USD", 2);
	g_autoptr(VentureMoney) gold = money(2000, "GOLD", 4);
	g_autoptr(GError) error = NULL;
	gdouble roi;

	evidence.age_seconds = 0;
	evidence.interval_seconds = 3600;
	evidence.venues = 3;
	evidence.dispersion = 0.0;
	evidence.depth = 1.0;
	g_assert_cmpfloat_with_epsilon(venture_arbitrage_confidence(&evidence), 1.0, 1e-12);

	/* Two intervals old: a third of the way down. */
	evidence.age_seconds = 7200;
	g_assert_cmpfloat_with_epsilon(venture_arbitrage_confidence(&evidence), 2.0 / 3.0, 1e-12);
	evidence.age_seconds = 4 * 3600;
	g_assert_cmpfloat_with_epsilon(venture_arbitrage_confidence(&evidence), 0.0, 1e-12);

	/* One venue quoting, a spread of half the mean, an unknown book. */
	evidence.age_seconds = 0;
	evidence.venues = 1;
	evidence.dispersion = 0.5;
	evidence.depth = NAN;
	g_assert_cmpfloat_with_epsilon(venture_arbitrage_confidence(&evidence),
	                               (1.0 / 3.0) * (1.0 / 1.5) * 0.5, 1e-12);

	/* Unknown spread: 0.8, not certainty. */
	evidence.venues = 3;
	evidence.dispersion = NAN;
	evidence.depth = 1.0;
	g_assert_cmpfloat_with_epsilon(venture_arbitrage_confidence(&evidence), 0.8, 1e-12);

	g_assert_true(venture_arbitrage_roi(net, capital, &roi, NULL));
	g_assert_cmpfloat_with_epsilon(roi, 0.25, 1e-12);
	g_assert_false(venture_arbitrage_roi(net, zero, &roi, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_arbitrage_roi(net, gold, &roi, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* ==========================================================================
 * Fee models
 * ========================================================================== */

static JsonObject *
params_of(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	JsonObject *params;

	params = venture_fee_model_parse_params(yaml, &error);
	g_assert_no_error(error);
	g_assert_nonnull(params);

	return params;
}

/*
 * The percent model: a 5% cut of 100.00 is 5.00, plus 0.10 a unit for
 * three, plus 0.50 an order; a 7.00 floor beats 5.80; a 15% deposit on
 * the price, refunded on a sale unless the venue says not; buying is free
 * without a buy: mapping and priced by it with one. A fee in another
 * currency than the sale is refused, and so is a deposit on a reference
 * nobody gave. What breaks if this regresses: every venue's net.
 */
static void
test_percent_model(void)
{
	g_autoptr(VentureFeeModelRegistry) models = venture_fee_model_registry_new();
	g_autoptr(VentureMoney) amount = money(10000, "USD", 2);
	g_autoptr(GError) error = NULL;
	VentureFeeQuote quote;

	{
		g_autoptr(JsonObject) params = params_of("cut_percent: 5\nfixed_per_unit: 0.10\n"
		                                         "fixed_per_order: 0.50\ndeposit_percent: 15\n");

		if (!venture_fee_model_registry_compute(models, "percent", params, VENTURE_FEE_SIDE_SELL,
		                                        amount, 3, NULL, &quote, &error))
			g_error("percent refused: %s", error->message);
		g_assert_cmpint(minor_of(quote.fee), ==, 500 + 30 + 50);
		g_assert_cmpint(minor_of(quote.deposit), ==, 1500);
		g_assert_true(quote.deposit_refundable);
		venture_fee_quote_clear(&quote);

		/* Buying is free here. */
		g_assert_true(venture_fee_model_registry_compute(models, "percent", params, VENTURE_FEE_SIDE_BUY,
		                                                 amount, 3, NULL, &quote, NULL));
		g_assert_cmpint(minor_of(quote.fee), ==, 0);
		g_assert_null(quote.deposit);
		venture_fee_quote_clear(&quote);
	}

	{
		g_autoptr(JsonObject) params = params_of("cut_percent: 5\nmin_fee: 7.00\n"
		                                         "deposit_refundable: false\ndeposit_percent: 1\n"
		                                         "buy:\n  fixed_per_order: 2.00\n");

		g_assert_true(venture_fee_model_registry_compute(models, "percent", params, VENTURE_FEE_SIDE_SELL,
		                                                 amount, 1, NULL, &quote, NULL));
		g_assert_cmpint(minor_of(quote.fee), ==, 700);
		g_assert_false(quote.deposit_refundable);
		venture_fee_quote_clear(&quote);

		g_assert_true(venture_fee_model_registry_compute(models, "percent", params, VENTURE_FEE_SIDE_BUY,
		                                                 amount, 1, NULL, &quote, NULL));
		g_assert_cmpint(minor_of(quote.fee), ==, 200);
		venture_fee_quote_clear(&quote);
	}

	{
		g_autoptr(JsonObject) params = params_of("fixed_per_order: 0.50 EUR\n");

		g_assert_false(venture_fee_model_registry_compute(models, "percent", params, VENTURE_FEE_SIDE_SELL,
		                                                  amount, 1, NULL, &quote, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

	{
		g_autoptr(JsonObject) params = params_of("deposit_percent: 5\ndeposit_basis: reference\n");

		g_assert_false(venture_fee_model_registry_compute(models, "percent", params, VENTURE_FEE_SIDE_SELL,
		                                                  amount, 1, NULL, &quote, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}
}

/*
 * The registry: built-ins first, a plugin's model added by a plain
 * function, a name taken or malformed refused; parameters judged by the
 * model (an unknown key -- a misspelt cut is a cut nobody charged -- a
 * cut over 100, a commission of everything); the commission model keeps
 * its rate and charges it on net winnings only; an unloaded model is
 * NOT_FOUND, never read as no fee.
 */
static gboolean
flat_two(
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	VentureFeeQuote		 *out,
	gpointer		  user_data,
	GError			**error
){
	(void)params;
	(void)side;
	(void)units;
	(void)reference;
	(void)error;

	g_assert_cmpstr(user_data, ==, "plugin");
	out->fee = venture_money_new(200, venture_money_get_currency(amount),
	                             venture_money_get_exponent(amount));

	return TRUE;
}

static void
test_fee_registry(void)
{
	g_autoptr(VentureFeeModelRegistry) models = venture_fee_model_registry_new();
	g_autoptr(VentureMoney) winnings = money(5000, "USD", 2);
	g_autoptr(VentureMoney) loss = money(-5000, "USD", 2);
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) names = NULL;
	VentureFeeQuote quote;

	names = venture_fee_model_registry_dup_names(models);
	g_assert_cmpuint(g_strv_length(names), ==, 3);
	g_assert_cmpstr(names[0], ==, "percent");
	g_assert_cmpstr(names[1], ==, "commission");
	g_assert_cmpstr(names[2], ==, "none");

	g_assert_true(venture_fee_model_registry_add(models, "flat_two", "Two dollars", flat_two, NULL,
	                                             (gpointer)"plugin", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_fee_model_registry_add(models, "percent", "again", flat_two, NULL, NULL, NULL,
	                                              &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_add(models, "Bad-Name", "no", flat_two, NULL, NULL, NULL,
	                                              &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_true(venture_fee_model_registry_compute(models, "flat_two", NULL, VENTURE_FEE_SIDE_SELL,
	                                                 winnings, 1, NULL, &quote, NULL));
	g_assert_cmpint(minor_of(quote.fee), ==, 200);
	venture_fee_quote_clear(&quote);

	/* Validation, as a venue's save does it. */
	g_assert_true(venture_fee_model_registry_validate(models, "percent", "cut_percent: 5", NULL));
	g_assert_true(venture_fee_model_registry_validate(models, "", "", NULL));
	g_assert_true(venture_fee_model_registry_validate(models, "none", NULL, NULL));
	g_assert_false(venture_fee_model_registry_validate(models, "percent", "cut_percnt: 5", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "cut_percnt"));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "percent", "cut_percent: 101", &error));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "commission", "rate_percent: 100", &error));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "none", "cut_percent: 5", &error));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "", "cut_percent: 5", &error));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "wow_auction", "", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "registered: percent, commission, none, flat_two"));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "percent", "- not\n- a mapping\n", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* The commission: kept as a share, charged on winnings only. */
	{
		g_autoptr(JsonObject) params = params_of("rate_percent: 5");

		g_assert_true(venture_fee_model_registry_compute(models, "commission", params, VENTURE_FEE_SIDE_SELL,
		                                                 winnings, 1, NULL, &quote, NULL));
		g_assert_cmpfloat_with_epsilon(quote.commission, 0.05, 1e-12);
		g_assert_cmpint(minor_of(quote.fee), ==, 250);
		venture_fee_quote_clear(&quote);
		g_assert_true(venture_fee_model_registry_compute(models, "commission", params, VENTURE_FEE_SIDE_SELL,
		                                                 loss, 1, NULL, &quote, NULL));
		g_assert_cmpint(minor_of(quote.fee), ==, 0);
		venture_fee_quote_clear(&quote);
	}

	/* Empty is none; an unloaded model is never no fee. */
	g_assert_true(venture_fee_model_registry_compute(models, NULL, NULL, VENTURE_FEE_SIDE_SELL, winnings, 1,
	                                                 NULL, &quote, NULL));
	g_assert_cmpint(minor_of(quote.fee), ==, 0);
	venture_fee_quote_clear(&quote);
	g_assert_false(venture_fee_model_registry_compute(models, "wow_auction", NULL, VENTURE_FEE_SIDE_SELL,
	                                                  winnings, 1, NULL, &quote, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* ==========================================================================
 * The calculators
 * ========================================================================== */

static JsonObject *
input_of(const gchar *const *pairs)
{
	JsonObject *input;
	guint i;

	input = json_object_new();

	for (i = 0; NULL != pairs[i]; i += 2)
		json_object_set_string_member(input, pairs[i], pairs[i + 1]);

	return input;
}

/*
 * The calculators read a form's strings and answer with the same math:
 * odds in mixed formats on one line, a stake in yen, a flip through the
 * percent model. A refusal names the field. What breaks if this
 * regresses: /arbitrage/calc showing one number and the scan another.
 */
static void
test_calculators(void)
{
	static const gchar *const surebet[] = { "odds", "+150, 1.8", "stake", "1000 JPY", NULL };
	static const gchar *const back_lay[] = { "back_odds", "3.2", "lay_odds", "3.0", "commission", "2",
	                                         "stake", "100.00 USD", NULL };
	static const gchar *const flip[] = { "buy", "10.00 USD", "sell", "15.00 USD", "units", "10",
	                                     "cut", "5", "deposit", "2", "sale_rate", "25", "sold_per_day", "2",
	                                     "share", "50", "transfer", "1.00 USD", NULL };
	static const gchar *const bad[] = { "odds", "2.1, 1.0", "stake", "100 USD", NULL };
	g_autoptr(JsonObject) surebet_input = input_of(surebet);
	g_autoptr(JsonObject) back_lay_input = input_of(back_lay);
	g_autoptr(JsonObject) flip_input = input_of(flip);
	g_autoptr(JsonObject) bad_input = input_of(bad);
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *root;

	answer = venture_arbitrage_calculate("surebet", surebet_input, &error);
	g_assert_no_error(error);
	root = json_node_get_object(answer);
	g_assert_true(json_object_get_boolean_member(root, "is_surebet"));
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(root, "guaranteed"), "amount"),
	                ==, 45);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(root, "profit"), "amount"),
	                ==, 47);
	g_clear_pointer(&answer, json_node_unref);

	answer = venture_arbitrage_calculate("back_lay", back_lay_input, &error);
	g_assert_no_error(error);
	root = json_node_get_object(answer);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(root, "worst"), "amount"),
	                ==, 523);
	g_clear_pointer(&answer, json_node_unref);

	/* 150.00 less a 5% cut (7.50), a 2% deposit (3.00) at three relists
	 * (9.00), 100.00 and 1.00 to move: 32.50 on 104.00. */
	answer = venture_arbitrage_calculate("flip", flip_input, &error);
	g_assert_no_error(error);
	root = json_node_get_object(answer);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(root, "net"), "amount"),
	                ==, 3250);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(root, "roi"), 0.3125, 1e-12);
	g_clear_pointer(&answer, json_node_unref);

	answer = venture_arbitrage_calculate("surebet", bad_input, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "Outcome 2"));
	g_clear_error(&error);

	answer = venture_arbitrage_calculate("roulette", bad_input, &error);
	g_assert_null(answer);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/arbitrage-math/odds-formats", test_odds_formats);
	g_test_add_func("/arbitrage-math/overround-and-no-vig", test_overround_and_no_vig);
	g_test_add_func("/arbitrage-math/round-minor", test_round_minor);
	g_test_add_func("/arbitrage-math/percent", test_percent);
	g_test_add_func("/arbitrage-math/surebet-table", test_surebet_table);
	g_test_add_func("/arbitrage-math/surebet-refusals", test_surebet_refusals);
	g_test_add_func("/arbitrage-math/back-lay", test_back_lay);
	g_test_add_func("/arbitrage-math/relists", test_relists);
	g_test_add_func("/arbitrage-math/annualize", test_annualize);
	g_test_add_func("/arbitrage-math/flip", test_flip);
	g_test_add_func("/arbitrage-math/confidence", test_confidence);
	g_test_add_func("/arbitrage-math/percent-model", test_percent_model);
	g_test_add_func("/arbitrage-math/fee-registry", test_fee_registry);
	g_test_add_func("/arbitrage-math/calculators", test_calculators);

	return g_test_run();
}
