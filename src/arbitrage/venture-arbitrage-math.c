/*
 * venture-arbitrage-math.c - The arithmetic of arbitrage
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See venture-arbitrage-math.h. Two rules run through the file:
 *
 *  - money is minor units and rounding happens once, half to even, where a
 *    ratio meets an amount (venture_arbitrage_round_minor());
 *  - where odds multiply money they do so as integers scaled by a million
 *    (the series store's own scale), so 100 x 1.13 pays 113 and not the
 *    112.99999 a double would floor to.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

/* 2^53: past here a double no longer holds every integer. */
#define ARB_MATH_EXACT_LIMIT (9007199254740992.0)

/* Odds are multiplied as integers at the series store's scale
 * (VENTURE_SERIES_ODDS_SCALE, which only a build with SQLite declares). */
#define ARB_MATH_ODDS_SCALE ((gint64)1000000)

/* A percent read exactly: parts per million of the whole. */
#define ARB_MATH_PPM (1000000)

/* --- Shared ------------------------------------------------------------------ */

gboolean
venture_arbitrage_round_minor(
	gdouble		  value,
	gint64		 *out,
	GError		**error
){
	if (!isfinite(value) || (fabs(value) > ARB_MATH_EXACT_LIMIT))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An amount is too large to compute exactly");
		return FALSE;
	}

	/* The default rounding mode is to nearest, ties to even: the money
	 * rule. nearbyint() honours it without raising inexact. */
	*out = (gint64)nearbyint(value);

	return TRUE;
}

VentureMoney *
venture_arbitrage_money_scale(
	const VentureMoney	 *amount,
	gdouble			  factor,
	GError			**error
){
	gint64 minor;

	g_return_val_if_fail(NULL != amount, NULL);

	if (!isfinite(factor))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An amount cannot be scaled by a ratio that is not a number");
		return NULL;
	}

	if (!venture_arbitrage_round_minor((gdouble)venture_money_get_amount(amount) * factor,
	                                   &minor, error))
		return NULL;

	return venture_money_new(minor, venture_money_get_currency(amount),
	                         venture_money_get_exponent(amount));
}

/* Decimal odds as an integer at the store's scale, refusing what is not
 * odds. */
static gboolean
arb_math_scaled(
	gdouble		  odds,
	gint64		 *out,
	GError		**error
){
	gdouble scaled;

	scaled = odds * (gdouble)ARB_MATH_ODDS_SCALE;

	if (!isfinite(odds) || (odds <= 1.0) || (scaled > ARB_MATH_EXACT_LIMIT))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Odds of %g are not odds: decimal odds pay more than the stake", odds);
		return FALSE;
	}

	*out = (gint64)llround(scaled);

	return TRUE;
}

/* @minor x @scaled / scale, rounded down (@down) or half to even. */
static gboolean
arb_math_times_odds(
	gint64		  minor,
	gint64		  scaled,
	gboolean	  down,
	gint64		 *out,
	GError		**error
){
	gint64 product;
	gint64 quotient;
	gint64 remainder;

	if (!venture_series_math_mul(minor, scaled, &product))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A stake times its odds is too large to compute");
		return FALSE;
	}

	quotient = product / ARB_MATH_ODDS_SCALE;
	remainder = product % ARB_MATH_ODDS_SCALE;

	/* C truncates towards zero; a negative product rounds down to the
	 * next integer below. */
	if (down)
	{
		if (remainder < 0)
			quotient--;

		*out = quotient;
		return TRUE;
	}

	if (product >= 0)
	{
		*out = venture_series_math_div_round(product, ARB_MATH_ODDS_SCALE);
		return TRUE;
	}

	*out = -venture_series_math_div_round(-product, ARB_MATH_ODDS_SCALE);

	return TRUE;
}

static gboolean
arb_math_check_odds(
	const gdouble	 *odds,
	guint		  n,
	guint		  least,
	GError		**error
){
	guint i;

	if ((NULL == odds) || (n < least))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "At least %u outcome%s needed", least, (1 == least) ? " is" : "s are");
		return FALSE;
	}

	if (n > VENTURE_ARBITRAGE_MAX_OUTCOMES)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "At most %d outcomes", VENTURE_ARBITRAGE_MAX_OUTCOMES);
		return FALSE;
	}

	for (i = 0; i < n; i++)
	{
		if (!isfinite(odds[i]) || (odds[i] <= 1.0))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Outcome %u: odds of %g are not odds; decimal odds pay more "
			            "than the stake", i + 1, odds[i]);
			return FALSE;
		}
	}

	return TRUE;
}

/* The sum of a set of amounts in one currency. */
static VentureMoney *
arb_math_sum(
	const VentureMoney *const	 *amounts,
	guint				  n,
	const VentureMoney		 *zero_of,
	GError				**error
){
	g_autoptr(VentureMoney) total = NULL;
	guint i;

	total = venture_money_new(0, venture_money_get_currency(zero_of),
	                          venture_money_get_exponent(zero_of));

	for (i = 0; i < n; i++)
	{
		VentureMoney *next;

		if (NULL == amounts[i])
			continue;

		next = venture_money_add(total, amounts[i], error);

		if (NULL == next)
			return NULL;

		venture_money_free(total);
		total = next;
	}

	return g_steal_pointer(&total);
}

/* --- Percent ----------------------------------------------------------------- */

gboolean
venture_arbitrage_parse_percent(
	const gchar	 *text,
	gboolean	  allow_negative,
	gint64		 *out_ppm,
	GError		**error
){
	const gchar *cursor;
	gboolean negative;
	gint64 whole;
	gint64 fraction;
	guint places;

	if ((NULL == text) || ('\0' == *text))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A percent is a number, e.g. 5 or 2.5");
		return FALSE;
	}

	cursor = text;

	while (g_ascii_isspace(*cursor))
		cursor++;

	negative = ('-' == *cursor);

	if (('-' == *cursor) || ('+' == *cursor))
		cursor++;

	whole = 0;
	fraction = 0;
	places = 0;

	if (!g_ascii_isdigit(*cursor) && ('.' != *cursor))
		goto refuse;

	while (g_ascii_isdigit(*cursor))
	{
		whole = whole * 10 + (*cursor - '0');

		if (whole > ARB_MATH_PPM)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is more than a million percent", text);
			return FALSE;
		}

		cursor++;
	}

	if ('.' == *cursor)
	{
		cursor++;

		while (g_ascii_isdigit(*cursor))
		{
			if (++places > 4)
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "\"%s\": a percent has at most four places after the point",
				            text);
				return FALSE;
			}

			fraction = fraction * 10 + (*cursor - '0');
			cursor++;
		}
	}

	/* "." alone is no number. */
	if ((0 == places) && !g_ascii_isdigit(text[strspn(text, " \t+-")]))
		goto refuse;

	while (g_ascii_isspace(*cursor))
		cursor++;

	/* A trailing sign is how people write it; accept it. */
	if ('%' == *cursor)
		cursor++;

	if ('\0' != *cursor)
		goto refuse;

	while (places < 4)
	{
		fraction *= 10;
		places++;
	}

	*out_ppm = whole * 10000 + fraction;

	if (negative && (0 != *out_ppm))
	{
		if (!allow_negative)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" cannot be negative here", text);
			return FALSE;
		}

		*out_ppm = -*out_ppm;
	}

	return TRUE;

refuse:
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is not a percent: write a number, e.g. 5 or 2.5", text);
	return FALSE;
}

/* --- Odds -------------------------------------------------------------------- */

gboolean
venture_arbitrage_odds_format_from_string(
	const gchar		*text,
	VentureOddsFormat	*out
){
	if ((NULL == text) || ('\0' == *text) || (0 == g_ascii_strcasecmp(text, "auto")))
		*out = VENTURE_ODDS_FORMAT_AUTO;
	else if (0 == g_ascii_strcasecmp(text, "decimal"))
		*out = VENTURE_ODDS_FORMAT_DECIMAL;
	else if (0 == g_ascii_strcasecmp(text, "american"))
		*out = VENTURE_ODDS_FORMAT_AMERICAN;
	else if (0 == g_ascii_strcasecmp(text, "fractional"))
		*out = VENTURE_ODDS_FORMAT_FRACTIONAL;
	else
		return FALSE;

	return TRUE;
}

/* A whole decimal number, all of @text, or FALSE. */
static gboolean
arb_math_number(
	const gchar	*text,
	gdouble		*out
){
	gchar *end;

	if ((NULL == text) || ('\0' == *text))
		return FALSE;

	end = NULL;
	*out = g_ascii_strtod(text, &end);

	return (NULL != end) && ('\0' == *end) && (end != text) && isfinite(*out);
}

gboolean
venture_arbitrage_odds_parse(
	const gchar		 *text,
	VentureOddsFormat	  format,
	gdouble			 *out_decimal,
	GError			**error
){
	g_autofree gchar *copy = NULL;
	gdouble value;
	gdouble decimal;

	if ((NULL == text) || ('\0' == *text))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Odds are missing");
		return FALSE;
	}

	copy = g_strstrip(g_strdup(text));

	if (VENTURE_ODDS_FORMAT_AUTO == format)
	{
		if (NULL != strchr(copy, '/'))
			format = VENTURE_ODDS_FORMAT_FRACTIONAL;
		else if (('+' == copy[0]) || ('-' == copy[0]))
			format = VENTURE_ODDS_FORMAT_AMERICAN;
		else if ((0 == g_ascii_strcasecmp(copy, "evens")) || (0 == g_ascii_strcasecmp(copy, "evs")))
			format = VENTURE_ODDS_FORMAT_FRACTIONAL;
		else
			format = VENTURE_ODDS_FORMAT_DECIMAL;
	}

	decimal = NAN;

	switch (format)
	{
	case VENTURE_ODDS_FORMAT_FRACTIONAL:
	{
		g_auto(GStrv) parts = NULL;
		gdouble numerator;
		gdouble denominator;

		/* Evens is 1/1, as a bookmaker writes it. */
		if ((0 == g_ascii_strcasecmp(copy, "evens")) || (0 == g_ascii_strcasecmp(copy, "evs")))
		{
			decimal = 2.0;
			break;
		}

		parts = g_strsplit(copy, "/", -1);

		if ((2 != g_strv_length(parts)) ||
		    !arb_math_number(g_strstrip(parts[0]), &numerator) ||
		    !arb_math_number(g_strstrip(parts[1]), &denominator) ||
		    (numerator <= 0.0) || (denominator <= 0.0))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not fractional odds: write a/b with both parts above "
			            "zero, e.g. 3/2", text);
			return FALSE;
		}

		decimal = 1.0 + numerator / denominator;
		break;
	}

	case VENTURE_ODDS_FORMAT_AMERICAN:
		if (!arb_math_number(copy, &value) || (fabs(value) < 100.0))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not American odds: a moneyline is +100 or more, or "
			            "-100 or less", text);
			return FALSE;
		}

		decimal = (value > 0.0) ? (1.0 + value / 100.0) : (1.0 + 100.0 / -value);
		break;

	case VENTURE_ODDS_FORMAT_AUTO:
	case VENTURE_ODDS_FORMAT_DECIMAL:
	default:
		if (!arb_math_number(copy, &value))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not decimal odds, e.g. 2.50", text);
			return FALSE;
		}

		decimal = value;
		break;
	}

	if (!isfinite(decimal) || (decimal <= 1.0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" pays nothing: decimal odds are above 1", text);
		return FALSE;
	}

	*out_decimal = decimal;

	return TRUE;
}

gdouble
venture_arbitrage_implied_probability(gdouble decimal)
{
	if (!isfinite(decimal) || (decimal <= 1.0))
		return NAN;

	return 1.0 / decimal;
}

gboolean
venture_arbitrage_overround(
	const gdouble	 *odds,
	guint		  n,
	gdouble		 *out_overround,
	GError		**error
){
	gdouble sum;
	guint i;

	if (!arb_math_check_odds(odds, n, 1, error))
		return FALSE;

	sum = 0.0;

	for (i = 0; i < n; i++)
		sum += 1.0 / odds[i];

	*out_overround = sum - 1.0;

	return TRUE;
}

gboolean
venture_arbitrage_no_vig(
	const gdouble	 *odds,
	guint		  n,
	gdouble		 *out_probabilities,
	GError		**error
){
	gdouble sum;
	guint i;

	if (!arb_math_check_odds(odds, n, 1, error))
		return FALSE;

	sum = 0.0;

	for (i = 0; i < n; i++)
		sum += 1.0 / odds[i];

	for (i = 0; i < n; i++)
		out_probabilities[i] = (1.0 / odds[i]) / sum;

	return TRUE;
}

/* --- Surebets ---------------------------------------------------------------- */

void
venture_arbitrage_surebet_clear(VentureArbitrageSurebet *surebet)
{
	if (NULL == surebet)
		return;

	g_clear_pointer(&surebet->stakes, g_ptr_array_unref);
	g_clear_pointer(&surebet->payouts, g_ptr_array_unref);
	g_clear_pointer(&surebet->staked, venture_money_free);
	g_clear_pointer(&surebet->residual, venture_money_free);
	g_clear_pointer(&surebet->payout, venture_money_free);
	g_clear_pointer(&surebet->payout_ideal, venture_money_free);
	g_clear_pointer(&surebet->profit, venture_money_free);
	g_clear_pointer(&surebet->profit_ideal, venture_money_free);
	memset(surebet, 0, sizeof(*surebet));
}

gboolean
venture_arbitrage_surebet(
	const gdouble		 *odds,
	guint			  n,
	const VentureMoney	 *total,
	VentureArbitrageSurebet	 *out,
	GError			**error
){
	VentureArbitrageSurebet split;
	const gchar *currency;
	guint8 exponent;
	gint64 total_minor;
	gint64 staked;
	gint64 worst;
	gint64 minor;
	gdouble sum;
	guint i;

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));

	if (!arb_math_check_odds(odds, n, 2, error))
		return FALSE;

	if ((NULL == total) || (venture_money_get_amount(total) <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The total stake must be more than nothing");
		return FALSE;
	}

	memset(&split, 0, sizeof(split));
	currency = venture_money_get_currency(total);
	exponent = venture_money_get_exponent(total);
	total_minor = venture_money_get_amount(total);
	sum = 0.0;

	for (i = 0; i < n; i++)
		sum += 1.0 / odds[i];

	split.sum = sum;
	split.overround = sum - 1.0;
	split.margin = 1.0 / sum - 1.0;
	split.is_surebet = (sum < 1.0 - VENTURE_ARBITRAGE_ODDS_EPSILON);
	split.stakes = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	split.payouts = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	staked = 0;
	worst = G_MAXINT64;

	/*
	 * Each stake is T*(1/d_i)/S, rounded half to even: every outcome then
	 * pays T/S before rounding. What each rounded stake pays is worked out
	 * at the store's odds scale and rounded down, because a bookmaker
	 * never pays part of a minor unit; the least of those is what the
	 * split is sure to return.
	 */
	for (i = 0; i < n; i++)
	{
		gint64 stake;
		gint64 scaled;
		gint64 payout;

		if (!venture_arbitrage_round_minor((gdouble)total_minor * (1.0 / odds[i]) / sum,
		                                   &stake, error) ||
		    !arb_math_scaled(odds[i], &scaled, error) ||
		    !arb_math_times_odds(stake, scaled, TRUE, &payout, error) ||
		    !venture_series_math_add(staked, stake, &staked))
		{
			if ((NULL != error) && (NULL == *error))
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				                    "The stakes are too large to add up");
			venture_arbitrage_surebet_clear(&split);
			return FALSE;
		}

		g_ptr_array_add(split.stakes, venture_money_new(stake, currency, exponent));
		g_ptr_array_add(split.payouts, venture_money_new(payout, currency, exponent));

		if (payout < worst)
			worst = payout;
	}

	/* What a person gets is the least rounded payout and what it leaves
	 * over the rounded stakes; the unrounded T/S and T(1/S - 1) are kept
	 * as the _ideal figures beside them. */
	split.staked = venture_money_new(staked, currency, exponent);
	split.residual = venture_money_new(total_minor - staked, currency, exponent);
	split.payout = venture_money_new(worst, currency, exponent);
	split.profit = venture_money_new(worst - staked, currency, exponent);

	if (!venture_arbitrage_round_minor((gdouble)total_minor / sum, &minor, error))
	{
		venture_arbitrage_surebet_clear(&split);
		return FALSE;
	}

	split.payout_ideal = venture_money_new(minor, currency, exponent);

	if (!venture_arbitrage_round_minor((gdouble)total_minor * (1.0 / sum - 1.0), &minor, error))
	{
		venture_arbitrage_surebet_clear(&split);
		return FALSE;
	}

	split.profit_ideal = venture_money_new(minor, currency, exponent);
	*out = split;

	return TRUE;
}

/* --- Back and lay ------------------------------------------------------------ */

gdouble
venture_arbitrage_effective_back_odds(
	gdouble	odds,
	gdouble	commission
){
	if (!isfinite(odds) || (odds <= 1.0) || !isfinite(commission) ||
	    (commission < 0.0) || (commission >= 1.0))
		return NAN;

	return 1.0 + (odds - 1.0) * (1.0 - commission);
}

gdouble
venture_arbitrage_effective_lay_odds(
	gdouble	odds,
	gdouble	commission
){
	if (!isfinite(odds) || (odds <= 1.0) || !isfinite(commission) ||
	    (commission < 0.0) || (commission >= 1.0))
		return NAN;

	return 1.0 + (odds - 1.0) / (1.0 - commission);
}

void
venture_arbitrage_back_lay_clear(VentureArbitrageBackLay *back_lay)
{
	if (NULL == back_lay)
		return;

	g_clear_pointer(&back_lay->lay_stake, venture_money_free);
	g_clear_pointer(&back_lay->liability, venture_money_free);
	g_clear_pointer(&back_lay->if_back_wins, venture_money_free);
	g_clear_pointer(&back_lay->if_lay_wins, venture_money_free);
	g_clear_pointer(&back_lay->worst, venture_money_free);
	g_clear_pointer(&back_lay->ideal, venture_money_free);
	memset(back_lay, 0, sizeof(*back_lay));
}

gboolean
venture_arbitrage_back_lay(
	gdouble				 back_odds,
	gdouble				 lay_odds,
	gdouble				 commission,
	const VentureMoney		 *back_stake,
	VentureArbitrageBackLay		 *out,
	GError				**error
){
	const gchar *currency;
	guint8 exponent;
	gint64 stake;
	gint64 back_scaled;
	gint64 lay_scaled;
	gint64 lay_stake;
	gint64 liability;
	gint64 back_gross;
	gint64 back_wins;
	gint64 lay_kept;
	gint64 ideal;
	gdouble odds[2];

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	odds[0] = back_odds;
	odds[1] = lay_odds;

	if (!arb_math_check_odds(odds, 2, 2, error))
		return FALSE;

	if (!isfinite(commission) || (commission < 0.0) || (commission >= 1.0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A commission of %g is not a share of winnings: it is from 0 to under 1",
		            commission);
		return FALSE;
	}

	if ((NULL == back_stake) || (venture_money_get_amount(back_stake) <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The back stake must be more than nothing");
		return FALSE;
	}

	currency = venture_money_get_currency(back_stake);
	exponent = venture_money_get_exponent(back_stake);
	stake = venture_money_get_amount(back_stake);

	/*
	 * The lay stake that makes both results equal: B*d/(L - c). L > 1 and
	 * c < 1, so the denominator is always above zero. The liability and
	 * the back winnings are integers times scaled odds, so they are exact
	 * before their one rounding -- and that rounding goes against the
	 * person, as the surebet's payouts do: winnings down (no bookmaker
	 * pays part of a minor unit), the liability up (an exchange holds the
	 * whole of it). Rounding both half to even made `worst` a cent better
	 * than what the pair is sure to make about as often as not, and it is
	 * the figure presented as the result.
	 */
	if (!venture_arbitrage_round_minor((gdouble)stake * back_odds / (lay_odds - commission),
	                                   &lay_stake, error) ||
	    !arb_math_scaled(back_odds, &back_scaled, error) ||
	    !arb_math_scaled(lay_odds, &lay_scaled, error) ||
	    !arb_math_times_odds(-lay_stake, lay_scaled - ARB_MATH_ODDS_SCALE, TRUE, &liability, error) ||
	    !arb_math_times_odds(stake, back_scaled - ARB_MATH_ODDS_SCALE, TRUE, &back_gross, error) ||
	    !venture_arbitrage_round_minor((gdouble)stake * back_odds * (1.0 - commission) /
	                                   (lay_odds - commission) - (gdouble)stake, &ideal, error))
		return FALSE;

	/* Up is minus the floor of the negation. */
	liability = -liability;

	/* What the exchange leaves of a winning lay, rounded down. The
	 * commission is a double, so a product meant to be whole (100 x 0.95)
	 * can land a hair under it; a millionth of a minor unit is noise, not
	 * a cent to take away. */
	lay_kept = (gint64)floor((gdouble)lay_stake * (1.0 - commission) + 1e-6);
	back_wins = back_gross - liability;
	out->lay_stake = venture_money_new(lay_stake, currency, exponent);
	out->liability = venture_money_new(liability, currency, exponent);
	out->if_back_wins = venture_money_new(back_wins, currency, exponent);
	out->if_lay_wins = venture_money_new(lay_kept - stake, currency, exponent);
	out->worst = venture_money_new(MIN(back_wins, lay_kept - stake), currency, exponent);
	out->ideal = venture_money_new(ideal, currency, exponent);
	out->rating = back_odds * (1.0 - commission) / (lay_odds - commission);

	return TRUE;
}

/* --- Flips ------------------------------------------------------------------- */

gboolean
venture_arbitrage_expected_relists(
	gdouble		  sale_rate,
	gdouble		 *out,
	GError		**error
){
	if (!isfinite(sale_rate))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The sale rate is unknown");
		return FALSE;
	}

	if (sale_rate <= 0.0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Never sells: a sale rate of 0 means every listing expires, "
		                    "and relisting forever loses every deposit");
		return FALSE;
	}

	if (sale_rate > 1.0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A sale rate of %g is not a share of listings: it is at most 1", sale_rate);
		return FALSE;
	}

	*out = 1.0 / sale_rate - 1.0;

	return TRUE;
}

gboolean
venture_arbitrage_annualize(
	gdouble		  roi,
	gdouble		  lock_days,
	gdouble		 *out,
	GError		**error
){
	gdouble value;

	if (!isfinite(roi) || !isfinite(lock_days) || (lock_days <= 0.0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An annual figure needs a return and a hold of more than no time");
		return FALSE;
	}

	/* Everything lost stays everything lost, however short the hold. */
	if (roi <= -1.0)
	{
		*out = -1.0;
		return TRUE;
	}

	value = pow(1.0 + roi, 365.0 / lock_days) - 1.0;

	if (!isfinite(value))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Too large to annualise: the hold is too short for the return "
		                    "to mean anything over a year");
		return FALSE;
	}

	*out = value;

	return TRUE;
}

void
venture_arbitrage_flip_input_init(VentureArbitrageFlipInput *input)
{
	g_return_if_fail(NULL != input);

	memset(input, 0, sizeof(*input));
	input->sale_rate = NAN;
	input->sold_per_day = NAN;
	input->share = 1.0;
	input->fill_probability = NAN;
	input->deposit_refundable = TRUE;
	input->units = 1;
}

void
venture_arbitrage_flip_clear(VentureArbitrageFlip *flip)
{
	if (NULL == flip)
		return;

	g_clear_pointer(&flip->listing_loss, venture_money_free);
	g_clear_pointer(&flip->net, venture_money_free);
	g_clear_pointer(&flip->capital, venture_money_free);
	g_clear_pointer(&flip->ev, venture_money_free);
	memset(flip, 0, sizeof(*flip));
}

/* An amount, or zero in @like's currency. */
static VentureMoney *
arb_math_or_zero(
	const VentureMoney	*amount,
	const VentureMoney	*like
){
	if (NULL != amount)
		return venture_money_copy(amount);

	return venture_money_new(0, venture_money_get_currency(like), venture_money_get_exponent(like));
}

gboolean
venture_arbitrage_flip(
	const VentureArbitrageFlipInput	 *input,
	VentureArbitrageFlip		 *out,
	GError				**error
){
	g_autoptr(VentureMoney) costs = NULL;
	g_autoptr(VentureMoney) deposit = NULL;
	g_autoptr(VentureMoney) transfer = NULL;
	g_autoptr(VentureMoney) buy_fees = NULL;
	g_autoptr(VentureMoney) sell_fees = NULL;
	g_autoptr(VentureMoney) loss = NULL;
	VentureArbitrageFlip flip;
	const VentureMoney *parts[6];
	const VentureMoney *each[6];
	const gchar *currency;
	gdouble counted;
	guint i;

	g_return_val_if_fail(NULL != input, FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	memset(&flip, 0, sizeof(flip));

	if ((NULL == input->buy_cost) || (NULL == input->sell_gross))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A flip needs what the units cost and what they sell for");
		return FALSE;
	}

	if (input->units < 1)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A flip is of at least one unit");
		return FALSE;
	}

	if (!isfinite(input->share) || (input->share <= 0.0) || (input->share > 1.0) ||
	    !isfinite(input->transit_days) || (input->transit_days < 0.0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The share of sales is above 0 and at most 1, and transit takes "
		                    "no negative time");
		return FALSE;
	}

	currency = venture_money_get_currency(input->buy_cost);
	each[0] = input->buy_fees;
	each[1] = input->sell_gross;
	each[2] = input->sell_fees;
	each[3] = input->deposit;
	each[4] = input->transfer_cost;
	each[5] = input->unwind_loss;

	/* Nothing here adds across currencies: convert first. */
	for (i = 0; i < G_N_ELEMENTS(each); i++)
	{
		if ((NULL != each[i]) && (0 != g_strcmp0(venture_money_get_currency(each[i]), currency)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A flip's amounts must be in one currency: %s and %s",
			            currency, venture_money_get_currency(each[i]));
			return FALSE;
		}
	}

	/* --- Relists and the deposits they lose --- */

	flip.relists = NAN;
	counted = 0.0;

	if (!isnan(input->sale_rate))
	{
		if (!venture_arbitrage_expected_relists(input->sale_rate, &flip.relists, error))
			return FALSE;

		counted = flip.relists;
	}

	deposit = arb_math_or_zero(input->deposit, input->buy_cost);

	/* A listing that sells refunds its own deposit when the venue does;
	 * otherwise the last listing's deposit is lost too. */
	loss = venture_arbitrage_money_scale(deposit, counted + (input->deposit_refundable ? 0.0 : 1.0),
	                                     error);

	if (NULL == loss)
		return FALSE;

	/* --- Net and capital --- */

	buy_fees = arb_math_or_zero(input->buy_fees, input->buy_cost);
	sell_fees = arb_math_or_zero(input->sell_fees, input->buy_cost);
	transfer = arb_math_or_zero(input->transfer_cost, input->buy_cost);
	parts[0] = input->buy_cost;
	parts[1] = buy_fees;
	parts[2] = sell_fees;
	parts[3] = loss;
	parts[4] = transfer;
	costs = arb_math_sum(parts, 5, input->buy_cost, error);

	if (NULL == costs)
		return FALSE;

	flip.net = venture_money_subtract(input->sell_gross, costs, error);

	if (NULL == flip.net)
		return FALSE;

	parts[0] = input->buy_cost;
	parts[1] = buy_fees;
	parts[2] = deposit;
	parts[3] = transfer;
	flip.capital = arb_math_sum(parts, 4, input->buy_cost, error);

	if (NULL == flip.capital)
	{
		venture_arbitrage_flip_clear(&flip);
		return FALSE;
	}

	flip.listing_loss = g_steal_pointer(&loss);
	flip.roi = NAN;

	if (venture_money_get_amount(flip.capital) > 0)
		flip.roi = (gdouble)venture_money_get_amount(flip.net) /
		           (gdouble)venture_money_get_amount(flip.capital);

	/* --- How long the money is tied up --- */

	flip.lock_days = NAN;
	flip.roi_per_day = NAN;
	flip.annualized = NAN;

	if (isfinite(input->sold_per_day) && (input->sold_per_day > 0.0))
		flip.lock_days = (gdouble)input->units / (input->sold_per_day * input->share) +
		                 input->transit_days;

	if (isfinite(flip.lock_days) && (flip.lock_days > 0.0) && isfinite(flip.roi))
	{
		flip.roi_per_day = flip.roi / flip.lock_days;

		/* Too short a hold to annualise is no annual figure, not a
		 * failed flip. */
		if (!venture_arbitrage_annualize(flip.roi, flip.lock_days, &flip.annualized, NULL))
			flip.annualized = NAN;
	}

	/* --- Expected value --- */

	flip.fill_probability = isfinite(input->fill_probability) ? input->fill_probability
	                                                          : input->sale_rate;

	if (isfinite(flip.fill_probability))
	{
		g_autoptr(VentureMoney) unwind = NULL;
		g_autoptr(VentureMoney) win = NULL;
		g_autoptr(VentureMoney) lose = NULL;
		gdouble p;

		p = CLAMP(flip.fill_probability, 0.0, 1.0);
		flip.fill_probability = p;
		unwind = arb_math_or_zero(input->unwind_loss, input->buy_cost);
		win = venture_arbitrage_money_scale(flip.net, p, error);
		lose = (NULL != win) ? venture_arbitrage_money_scale(unwind, 1.0 - p, error) : NULL;
		flip.ev = (NULL != lose) ? venture_money_subtract(win, lose, error) : NULL;

		if (NULL == flip.ev)
		{
			venture_arbitrage_flip_clear(&flip);
			return FALSE;
		}
	}

	*out = flip;

	return TRUE;
}

/* --- Confidence and ROI ------------------------------------------------------ */

gdouble
venture_arbitrage_confidence(const VentureArbitrageEvidence *evidence)
{
	gdouble interval;
	gdouble age;
	gdouble fresh;
	gdouble breadth;
	gdouble stability;
	gdouble depth;

	g_return_val_if_fail(NULL != evidence, 0.0);

	interval = (evidence->interval_seconds > 0) ? (gdouble)evidence->interval_seconds
	                                            : (gdouble)VENTURE_SERIES_INTERVAL_DEFAULT;
	age = MAX(0.0, (gdouble)evidence->age_seconds);
	fresh = CLAMP(1.0 - (age - interval) / (3.0 * interval), 0.0, 1.0);
	breadth = MIN(1.0, (gdouble)evidence->venues / 3.0);
	stability = (isfinite(evidence->dispersion) && (evidence->dispersion >= 0.0))
		? 1.0 / (1.0 + evidence->dispersion) : 0.8;
	depth = isfinite(evidence->depth) ? CLAMP(evidence->depth, 0.0, 1.0) : 0.5;

	return CLAMP(fresh * breadth * stability * depth, 0.0, 1.0);
}

gboolean
venture_arbitrage_roi(
	const VentureMoney	 *net,
	const VentureMoney	 *capital,
	gdouble			 *out,
	GError			**error
){
	if ((NULL == net) || (NULL == capital) ||
	    (0 != g_strcmp0(venture_money_get_currency(net), venture_money_get_currency(capital))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A return is a profit over the capital, in one currency");
		return FALSE;
	}

	if (venture_money_get_amount(capital) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A return needs capital of more than nothing");
		return FALSE;
	}

	*out = (gdouble)venture_money_get_amount(net) / (gdouble)venture_money_get_amount(capital);

	return TRUE;
}
