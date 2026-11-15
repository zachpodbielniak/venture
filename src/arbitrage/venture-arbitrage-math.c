/*
 * venture-arbitrage-math.c - The arithmetic of arbitrage
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * See venture-arbitrage-math.h. Two rules run through the arithmetic:
 *
 *  - money is minor units and rounding happens once, half to even, where a
 *    ratio meets an amount (venture_arbitrage_round_minor());
 *  - where odds multiply money they do so as integers scaled by a million
 *    (the series store's own scale), so 100 x 1.13 pays 113 and not the
 *    112.99999 a double would floor to.
 *
 * The arithmetic itself is the Tallow kernel src/kernels/vk_arbitrage.tlw
 * (and vk_percent.tlw for a percent read from text), compiled natively into
 * this library. What stays here is what only C does: reading odds from
 * text, checking what a caller passed, building VentureMoney values and
 * turning each kernel outcome into its message. docs/kernels.org describes
 * the split.
 */

#include "venture.h"

#include <vk-kernels.h>

#include <math.h>
#include <string.h>

/* The largest whole percent a percent may have: a million. The kernel reads
 * one more so that 1000000.9999 is accepted, as it always was. */
#define ARB_MATH_PERCENT_LIMIT (1000000)

/* Parts per million in one whole percent past the limit: anything at or
 * above (limit + 1) percent is more than a million percent. */
#define ARB_MATH_PPM_PAST_LIMIT (G_GINT64_CONSTANT(10000010000))

/* --- Shared ------------------------------------------------------------------ */

gboolean
venture_arbitrage_round_minor(
	gdouble		  value,
	gint64		 *out,
	GError		**error
){
	TlwOptI64 minor;

	/* To nearest, ties to even (the money rule), and only within 2^53,
	 * where a double still holds every integer (vk_arbitrage_round_minor). */
	minor = vk_arbitrage_round_minor(value);

	if (!minor.has)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An amount is too large to compute exactly");
		return FALSE;
	}

	*out = minor.val;

	return TRUE;
}

VentureMoney *
venture_arbitrage_money_scale(
	const VentureMoney	 *amount,
	gdouble			  factor,
	GError			**error
){
	TlwOptI64 scaled;

	g_return_val_if_fail(NULL != amount, NULL);

	if (!isfinite(factor))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An amount cannot be scaled by a ratio that is not a number");
		return NULL;
	}

	scaled = vk_arbitrage_scale(venture_money_get_amount(amount), factor);

	if (!scaled.has)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An amount is too large to compute exactly");
		return NULL;
	}

	return venture_money_new(scaled.val, venture_money_get_currency(amount),
	                         venture_money_get_exponent(amount));
}

/* @odds as the GArray of gdouble the kernels take. */
static GArray *
arb_math_odds_array(
	const gdouble	*odds,
	guint		 n
){
	GArray *array;

	array = g_array_sized_new(FALSE, FALSE, sizeof(gdouble), n);
	g_array_append_vals(array, odds, n);

	return array;
}

/* The outcomes a caller passed, as an array, or NULL with @error set when
 * there are too few or too many or one is not odds. */
static GArray *
arb_math_check_odds(
	const gdouble	 *odds,
	guint		  n,
	guint		  least,
	GError		**error
){
	g_autoptr(GArray) array = NULL;
	TlwOptI64 bad;

	if ((NULL == odds) || (n < least))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "At least %u outcome%s needed", least, (1 == least) ? " is" : "s are");
		return NULL;
	}

	if (n > VENTURE_ARBITRAGE_MAX_OUTCOMES)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "At most %d outcomes", VENTURE_ARBITRAGE_MAX_OUTCOMES);
		return NULL;
	}

	array = arb_math_odds_array(odds, n);
	bad = vk_arbitrage_first_not_odds(array, 0);

	if (bad.has)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Outcome %u: odds of %g are not odds; decimal odds pay more "
		            "than the stake", (guint)bad.val + 1, odds[bad.val]);
		return NULL;
	}

	return g_steal_pointer(&array);
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
	const gchar	*text,
	gboolean	 allow_negative,
	gint64		*out_ppm,
	GError		**error
){
	g_autoptr(VkPercentPercent) read = NULL;
	const gchar *sign;

	if ((NULL == text) || ('\0' == *text))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A percent is a number, e.g. 5 or 2.5");
		return FALSE;
	}

	/*
	 * vk_percent reads it: either sign, at most four places, a trailing
	 * '%' (a trailing sign is how people write it). Nothing may follow the
	 * sign, not even a space, as before.
	 */
	sign = strchr(text, '%');

	if ((NULL != sign) && ('\0' != sign[1]))
		goto refuse;

	read = vk_percent_parse(text, ARB_MATH_PERCENT_LIMIT + 1, TRUE, TRUE);

	switch (read->tag)
	{
	case VK_PERCENT_PERCENT_RATE:
		break;

	case VK_PERCENT_PERCENT_TOO_PRECISE:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\": a percent has at most four places after the point", text);
		return FALSE;

	case VK_PERCENT_PERCENT_TOO_LARGE:
		goto too_large;

	case VK_PERCENT_PERCENT_EMPTY:
	case VK_PERCENT_PERCENT_MALFORMED:
	case VK_PERCENT_PERCENT_NEGATIVE:
	default:
		goto refuse;
	}

	/* A million percent and any fraction of one more is the most. */
	if ((read->u.rate.ppm >= ARB_MATH_PPM_PAST_LIMIT) ||
	    (read->u.rate.ppm <= -ARB_MATH_PPM_PAST_LIMIT))
		goto too_large;

	/* Minus nothing is nothing, so "-0" is fine where a sign is not. */
	if ((read->u.rate.ppm < 0) && !allow_negative)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" cannot be negative here", text);
		return FALSE;
	}

	*out_ppm = read->u.rate.ppm;

	return TRUE;

too_large:
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is more than a million percent", text);
	return FALSE;

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
	TlwOptF64 converted;
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
		converted.has = FALSE;

		/* 1 + a/b, both parts above zero (vk_arbitrage_fractional). */
		if ((2 == g_strv_length(parts)) &&
		    arb_math_number(g_strstrip(parts[0]), &numerator) &&
		    arb_math_number(g_strstrip(parts[1]), &denominator))
			converted = vk_arbitrage_fractional(numerator, denominator);

		if (!converted.has)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not fractional odds: write a/b with both parts above "
			            "zero, e.g. 3/2", text);
			return FALSE;
		}

		decimal = converted.val;
		break;
	}

	case VENTURE_ODDS_FORMAT_AMERICAN:
		/* +A is 1 + A/100, -A is 1 + 100/A (vk_arbitrage_american). */
		converted.has = FALSE;

		if (arb_math_number(copy, &value))
			converted = vk_arbitrage_american(value);

		if (!converted.has)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not American odds: a moneyline is +100 or more, or "
			            "-100 or less", text);
			return FALSE;
		}

		decimal = converted.val;
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

	if (!vk_arbitrage_is_odds(decimal))
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
	TlwOptF64 probability;

	probability = vk_arbitrage_implied_probability(decimal);

	return probability.has ? probability.val : NAN;
}

gboolean
venture_arbitrage_overround(
	const gdouble	 *odds,
	guint		  n,
	gdouble		 *out_overround,
	GError		**error
){
	g_autoptr(GArray) array = NULL;

	array = arb_math_check_odds(odds, n, 1, error);

	if (NULL == array)
		return FALSE;

	*out_overround = vk_arbitrage_overround(array);

	return TRUE;
}

gboolean
venture_arbitrage_no_vig(
	const gdouble	 *odds,
	guint		  n,
	gdouble		 *out_probabilities,
	GError		**error
){
	g_autoptr(GArray) array = NULL;
	g_autoptr(GArray) probabilities = NULL;

	array = arb_math_check_odds(odds, n, 1, error);

	if (NULL == array)
		return FALSE;

	probabilities = vk_arbitrage_no_vig(array);
	memcpy(out_probabilities, probabilities->data, sizeof(gdouble) * n);

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
	g_autoptr(GArray) array = NULL;
	g_autoptr(VkArbitrageSurebet) split = NULL;
	const gchar *currency;
	guint8 exponent;
	gint64 total_minor;
	gint64 staked;
	gint64 worst;
	gdouble sum;
	guint i;

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	array = arb_math_check_odds(odds, n, 2, error);

	if (NULL == array)
		return FALSE;

	if ((NULL == total) || (venture_money_get_amount(total) <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The total stake must be more than nothing");
		return FALSE;
	}

	currency = venture_money_get_currency(total);
	exponent = venture_money_get_exponent(total);
	total_minor = venture_money_get_amount(total);

	/*
	 * vk_arbitrage_surebet: each stake is T(1/d_i)/S, half to even; what
	 * each rounded stake pays is worked out at the odds scale and rounded
	 * down, and the least of those is what the split is sure to return.
	 */
	split = vk_arbitrage_surebet(array, total_minor);

	switch (split->tag)
	{
	case VK_ARBITRAGE_SUREBET_SPLIT:
		break;

	case VK_ARBITRAGE_SUREBET_NOT_ODDS:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Odds of %g are not odds: decimal odds pay more than the stake",
		            split->u.not_odds.odds);
		return FALSE;

	case VK_ARBITRAGE_SUREBET_TOO_LARGE:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A stake times its odds is too large to compute");
		return FALSE;

	case VK_ARBITRAGE_SUREBET_UNSUMMABLE:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The stakes are too large to add up");
		return FALSE;

	case VK_ARBITRAGE_SUREBET_INEXACT:
	default:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An amount is too large to compute exactly");
		return FALSE;
	}

	sum = vk_arbitrage_inverse_sum(array);
	out->sum = sum;
	out->overround = sum - 1.0;
	out->margin = vk_arbitrage_margin(sum);
	out->is_surebet = vk_arbitrage_is_surebet(sum);
	out->stakes = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	out->payouts = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);

	for (i = 0; i < n; i++)
	{
		g_ptr_array_add(out->stakes,
		                venture_money_new(g_array_index(split->u.split.stakes, gint64, i),
		                                  currency, exponent));
		g_ptr_array_add(out->payouts,
		                venture_money_new(g_array_index(split->u.split.payouts, gint64, i),
		                                  currency, exponent));
	}

	/* What a person gets is the least rounded payout and what it leaves
	 * over the rounded stakes; the residual is what rounding left of the
	 * total, so the stakes and it sum back to the total. The unrounded
	 * T/S and T(1/S - 1) are kept as the _ideal figures beside them. */
	staked = split->u.split.staked;
	worst = split->u.split.worst;
	out->staked = venture_money_new(staked, currency, exponent);
	out->residual = venture_money_new(total_minor - staked, currency, exponent);
	out->payout = venture_money_new(worst, currency, exponent);
	out->profit = venture_money_new(worst - staked, currency, exponent);
	out->payout_ideal = venture_money_new(split->u.split.payout_ideal, currency, exponent);
	out->profit_ideal = venture_money_new(split->u.split.profit_ideal, currency, exponent);

	return TRUE;
}

/* --- Back and lay ------------------------------------------------------------ */

gdouble
venture_arbitrage_effective_back_odds(
	gdouble	odds,
	gdouble	commission
){
	TlwOptF64 effective;

	effective = vk_arbitrage_effective_back_odds(odds, commission);

	return effective.has ? effective.val : NAN;
}

gdouble
venture_arbitrage_effective_lay_odds(
	gdouble	odds,
	gdouble	commission
){
	TlwOptF64 effective;

	effective = vk_arbitrage_effective_lay_odds(odds, commission);

	return effective.has ? effective.val : NAN;
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
	g_autoptr(GArray) array = NULL;
	g_autoptr(VkArbitrageBackLay) pair = NULL;
	const gchar *currency;
	guint8 exponent;
	gdouble odds[2];

	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));
	odds[0] = back_odds;
	odds[1] = lay_odds;
	array = arb_math_check_odds(odds, 2, 2, error);

	if (NULL == array)
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

	/*
	 * vk_arbitrage_back_lay: the lay stake B d/(L - c), half to even; the
	 * liability and the back winnings are integers times scaled odds,
	 * exact before their one rounding, and that rounding goes against the
	 * person -- winnings down, the liability up -- so `worst` is a
	 * guarantee and not an estimate.
	 */
	pair = vk_arbitrage_back_lay(back_odds, lay_odds, commission,
	                             venture_money_get_amount(back_stake));

	switch (pair->tag)
	{
	case VK_ARBITRAGE_BACK_LAY_PAIR:
		break;

	case VK_ARBITRAGE_BACK_LAY_NOT_ODDS:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Odds of %g are not odds: decimal odds pay more than the stake",
		            pair->u.not_odds.odds);
		return FALSE;

	case VK_ARBITRAGE_BACK_LAY_TOO_LARGE:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A stake times its odds is too large to compute");
		return FALSE;

	case VK_ARBITRAGE_BACK_LAY_INEXACT:
	default:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An amount is too large to compute exactly");
		return FALSE;
	}

	out->lay_stake = venture_money_new(pair->u.pair.lay_stake, currency, exponent);
	out->liability = venture_money_new(pair->u.pair.liability, currency, exponent);
	out->if_back_wins = venture_money_new(pair->u.pair.if_back_wins, currency, exponent);
	out->if_lay_wins = venture_money_new(pair->u.pair.if_lay_wins, currency, exponent);
	out->worst = venture_money_new(pair->u.pair.worst, currency, exponent);
	out->ideal = venture_money_new(pair->u.pair.ideal, currency, exponent);
	out->rating = pair->u.pair.rating;

	return TRUE;
}

/* --- Flips ------------------------------------------------------------------- */

gboolean
venture_arbitrage_expected_relists(
	gdouble		  sale_rate,
	gdouble		 *out,
	GError		**error
){
	g_autoptr(VkArbitrageRelists) relists = NULL;

	/* 1/r - 1 for a rate in (0, 1] (vk_arbitrage_expected_relists). */
	relists = vk_arbitrage_expected_relists(sale_rate);

	switch (relists->tag)
	{
	case VK_ARBITRAGE_RELISTS_EXPECTED:
		*out = relists->u.expected.relists;
		return TRUE;

	case VK_ARBITRAGE_RELISTS_NEVER_SELLS:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Never sells: a sale rate of 0 means every listing expires, "
		                    "and relisting forever loses every deposit");
		return FALSE;

	case VK_ARBITRAGE_RELISTS_NOT_A_SHARE:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A sale rate of %g is not a share of listings: it is at most 1", sale_rate);
		return FALSE;

	case VK_ARBITRAGE_RELISTS_UNKNOWN:
	default:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The sale rate is unknown");
		return FALSE;
	}
}

gboolean
venture_arbitrage_annualize(
	gdouble		  roi,
	gdouble		  lock_days,
	gdouble		 *out,
	GError		**error
){
	g_autoptr(VkArbitrageAnnual) annual = NULL;

	/* (1 + roi)^(365/days) - 1, and everything lost stays everything lost
	 * however short the hold (vk_arbitrage_annualize). */
	annual = vk_arbitrage_annualize(roi, lock_days);

	switch (annual->tag)
	{
	case VK_ARBITRAGE_ANNUAL_ANNUALIZED:
		*out = annual->u.annualized.rate;
		return TRUE;

	case VK_ARBITRAGE_ANNUAL_TOO_SHORT:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Too large to annualise: the hold is too short for the return "
		                    "to mean anything over a year");
		return FALSE;

	case VK_ARBITRAGE_ANNUAL_NO_HOLD:
	default:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "An annual figure needs a return and a hold of more than no time");
		return FALSE;
	}
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

/* @net / @capital at the finer of their exponents, or NAN when one does
 * not fit there (vk_arbitrage_roi). */
static gdouble
arb_math_ratio(
	const VentureMoney	*net,
	const VentureMoney	*capital
){
	TlwOptF64 ratio;

	ratio = vk_arbitrage_roi(venture_money_get_amount(net), venture_money_get_exponent(net),
	                         venture_money_get_amount(capital),
	                         venture_money_get_exponent(capital));

	return ratio.has ? ratio.val : NAN;
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
	TlwOptF64 days;
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

	/* Nothing here adds across currencies: convert first. Codes compare
	 * case-insensitively, as everywhere else in money. */
	for (i = 0; i < G_N_ELEMENTS(each); i++)
	{
		if ((NULL != each[i]) &&
		    (0 != g_ascii_strcasecmp(venture_money_get_currency(each[i]), currency)))
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
	loss = venture_arbitrage_money_scale(deposit,
	                                     vk_arbitrage_listing_factor(counted,
	                                                                 input->deposit_refundable),
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

	/* The net and the capital can sit at different exponents (a sale
	 * price in mills, costs in cents): the ratio is taken at the finer. */
	if (venture_money_get_amount(flip.capital) > 0)
		flip.roi = arb_math_ratio(flip.net, flip.capital);

	/* --- How long the money is tied up --- */

	flip.lock_days = NAN;
	flip.roi_per_day = NAN;
	flip.annualized = NAN;
	days = vk_arbitrage_lock_days(input->units, input->sold_per_day, input->share,
	                              input->transit_days);

	if (days.has)
		flip.lock_days = days.val;

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

		p = vk_arbitrage_unit_interval(flip.fill_probability);
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
	g_return_val_if_fail(NULL != evidence, 0.0);

	/* freshness x breadth x stability x depth (vk_arbitrage_confidence) */
	return vk_arbitrage_confidence(evidence->interval_seconds,
	                               VENTURE_SERIES_INTERVAL_DEFAULT,
	                               evidence->age_seconds, (gint64)evidence->venues,
	                               evidence->dispersion, evidence->depth);
}

gboolean
venture_arbitrage_roi(
	const VentureMoney	 *net,
	const VentureMoney	 *capital,
	gdouble			 *out,
	GError			**error
){
	gdouble ratio;

	if ((NULL == net) || (NULL == capital) ||
	    (0 != g_ascii_strcasecmp(venture_money_get_currency(net),
	                             venture_money_get_currency(capital))))
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

	/* At the finer of the two exponents: dividing the raw minor units of
	 * a net in cents by capital in mills was off by a factor of ten. */
	ratio = arb_math_ratio(net, capital);

	if (isnan(ratio))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A return's amounts are too large to bring to one exponent");
		return FALSE;
	}

	*out = ratio;

	return TRUE;
}
