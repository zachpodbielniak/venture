/*
 * venture-arbitrage-math.h - The arithmetic of arbitrage
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure functions, no database and no store: odds in every format, the
 * surebet split, back and lay with an exchange's commission, and the
 * figures a flip is judged by (net, ROI, ROI per day, annualised ROI,
 * listing loss, lock days, expected value, confidence). The scan, the
 * calculators and the tests all call these, so a page and a report can
 * never compute the same number two ways.
 *
 * Money stays money. Every amount in or out is a #VentureMoney -- integer
 * minor units -- and is rounded half to even, once, where a ratio meets an
 * amount. Ratios (odds, probabilities, ROI, confidence) are doubles,
 * because they are ratios. An amount a ratio would push past 2^53 minor
 * units is refused rather than rounded wrong.
 */

#ifndef VENTURE_ARBITRAGE_MATH_H
#define VENTURE_ARBITRAGE_MATH_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_ARBITRAGE_MAX_OUTCOMES:
 *
 * The most outcomes one surebet may cover. A bound, not a rule: a horse
 * race is a few dozen.
 */
#define VENTURE_ARBITRAGE_MAX_OUTCOMES (64)

/**
 * VENTURE_ARBITRAGE_ODDS_EPSILON:
 *
 * How close a sum of implied probabilities must be to one to count as one.
 * Odds of 3.0 three ways sum to one in exact arithmetic and to one or a
 * hair under it in binary; a sum that close is "no edge", never a surebet.
 */
#define VENTURE_ARBITRAGE_ODDS_EPSILON (1e-12)

/**
 * VentureOddsFormat:
 * @VENTURE_ODDS_FORMAT_AUTO: decide from the text: a "/" is fractional, a
 *   leading sign is American, anything else decimal
 * @VENTURE_ODDS_FORMAT_DECIMAL: European odds, the payout per unit staked
 *   ("2.50")
 * @VENTURE_ODDS_FORMAT_AMERICAN: moneyline ("+150", "-200")
 * @VENTURE_ODDS_FORMAT_FRACTIONAL: British odds ("3/2")
 *
 * How odds are written.
 */
typedef enum
{
	VENTURE_ODDS_FORMAT_AUTO = 0,
	VENTURE_ODDS_FORMAT_DECIMAL,
	VENTURE_ODDS_FORMAT_AMERICAN,
	VENTURE_ODDS_FORMAT_FRACTIONAL
} VentureOddsFormat;

/* --- Odds ------------------------------------------------------------------- */

/**
 * venture_arbitrage_odds_parse:
 * @text: odds as written
 * @format: how they are written, or %VENTURE_ODDS_FORMAT_AUTO
 * @out_decimal: (out): the decimal odds
 * @error: (out) (optional): return location for a #GError
 *
 * Reads odds in any of the three formats into decimal odds: American +A is
 * 1 + A/100 and -A is 1 + 100/A; fractional a/b is 1 + a/b. Decimal odds
 * of one or less pay nothing and are refused, as are American odds whose
 * magnitude is under 100 (not a moneyline) and a fraction with a zero or
 * negative part. Every refusal is %VENTURE_ERROR_INVALID_ARGUMENT naming
 * the text.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_odds_parse(
	const gchar		 *text,
	VentureOddsFormat	  format,
	gdouble			 *out_decimal,
	GError			**error
);

/**
 * venture_arbitrage_odds_format_from_string:
 * @text: (nullable): "decimal", "american", "fractional", "auto" or %NULL
 * @out: (out): the format
 *
 * Returns: %FALSE for any other word
 */
gboolean
venture_arbitrage_odds_format_from_string(
	const gchar		*text,
	VentureOddsFormat	*out
);

/**
 * venture_arbitrage_implied_probability:
 * @decimal: decimal odds
 *
 * Returns: 1 / @decimal, or NAN for odds of one or less
 */
gdouble
venture_arbitrage_implied_probability(gdouble decimal);

/**
 * venture_arbitrage_overround:
 * @odds: (array length=n): decimal odds, one per outcome
 * @n: how many outcomes, 1 to %VENTURE_ARBITRAGE_MAX_OUTCOMES
 * @out_overround: (out): the sum of the implied probabilities less one;
 *   negative is an edge for the bettor
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success; an empty set or odds of one or less are
 *   refused
 */
gboolean
venture_arbitrage_overround(
	const gdouble	 *odds,
	guint		  n,
	gdouble		 *out_overround,
	GError		**error
);

/**
 * venture_arbitrage_no_vig:
 * @odds: (array length=n): decimal odds, one per outcome
 * @n: how many outcomes
 * @out_probabilities: (array length=n) (out caller-allocates): each
 *   outcome's probability with the margin taken out:
 *   (1/d_i) / sum(1/d_j)
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_no_vig(
	const gdouble	 *odds,
	guint		  n,
	gdouble		 *out_probabilities,
	GError		**error
);

/* --- Rounding --------------------------------------------------------------- */

/**
 * venture_arbitrage_round_minor:
 * @value: an amount in minor units, possibly fractional
 * @out: (out): @value rounded half to even
 * @error: (out) (optional): return location for a #GError
 *
 * The one place a ratio becomes money. A value that is not finite or lies
 * past 2^53 (where a double stops holding every integer) is refused with
 * %VENTURE_ERROR_INVALID_ARGUMENT rather than rounded wrong.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_round_minor(
	gdouble		  value,
	gint64		 *out,
	GError		**error
);

/**
 * venture_arbitrage_money_scale:
 * @amount: an amount
 * @factor: a finite ratio
 * @error: (out) (optional): return location for a #GError
 *
 * @amount times @factor, rounded half to even in @amount's own currency
 * and exponent.
 *
 * Returns: (transfer full) (nullable): the scaled amount, or %NULL on
 *   overflow or a factor that is not finite
 */
VentureMoney *
venture_arbitrage_money_scale(
	const VentureMoney	 *amount,
	gdouble			  factor,
	GError			**error
);

/**
 * venture_arbitrage_parse_percent:
 * @text: a percent, e.g. "5", "2.5" or "-12.25"
 * @allow_negative: whether a negative percent is meaningful here
 * @out_ppm: (out): the percent as parts per million (5% is 50000)
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a percent exactly, so a cut of 4.75% is 47500 parts per million
 * and never 47499.99. At most four places after the point; past one
 * million percent is refused.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_parse_percent(
	const gchar	 *text,
	gboolean	  allow_negative,
	gint64		 *out_ppm,
	GError		**error
);

/* --- Surebets --------------------------------------------------------------- */

/**
 * VentureArbitrageSurebet:
 * @sum: S, the sum of the implied probabilities of the best odds
 * @is_surebet: whether S is under one (by more than
 *   %VENTURE_ARBITRAGE_ODDS_EPSILON): every outcome pays more than the
 *   stakes
 * @overround: S - 1
 * @margin: 1/S - 1, the profit per unit staked before rounding
 * @stakes: (element-type VentureMoney): each outcome's stake,
 *   T*(1/d_i)/S rounded half to even to the currency's minor unit
 * @payouts: (element-type VentureMoney): what each outcome's stake
 *   returns, stake_i * d_i rounded down: a bookmaker never pays a part of
 *   a minor unit
 * @staked: the sum of the rounded stakes
 * @residual: the total asked for less @staked: what rounding left over
 *   (or took), positive or negative
 * @payout: the least of @payouts: what the rounded stakes are sure to
 *   return whichever outcome happens
 * @payout_ideal: T/S, the payout before rounding, rounded half to even;
 *   shown beside @payout, never instead of it
 * @profit: @payout less @staked: what the rounded stakes are sure to
 *   make; may be below @profit_ideal, and negative on a sliver of an edge
 * @profit_ideal: T*(1/S - 1), the profit before rounding, rounded half
 *   to even
 *
 * The split of a total stake across every outcome of an event so that
 * each pays the same. @payout and @profit are what a person gets, so
 * they are the rounded figures; the unrounded ones carry `_ideal` in
 * their name, because a calculator that answered 106.24 beside four
 * payouts of 106.23 promised a cent that no bookmaker pays.
 */
typedef struct
{
	gdouble		 sum;
	gboolean	 is_surebet;
	gdouble		 overround;
	gdouble		 margin;
	GPtrArray	*stakes;
	GPtrArray	*payouts;
	VentureMoney	*staked;
	VentureMoney	*residual;
	VentureMoney	*payout;
	VentureMoney	*payout_ideal;
	VentureMoney	*profit;
	VentureMoney	*profit_ideal;
} VentureArbitrageSurebet;

/**
 * venture_arbitrage_surebet:
 * @odds: (array length=n): the best decimal odds for each outcome
 * @n: how many outcomes, 2 to %VENTURE_ARBITRAGE_MAX_OUTCOMES (one
 *   outcome is not an event anybody could cover)
 * @total: the total stake, T; positive
 * @out: (out caller-allocates): the split; clear it with
 *   venture_arbitrage_surebet_clear()
 * @error: (out) (optional): return location for a #GError
 *
 * Splits @total across the outcomes. It answers for any S: with S of one
 * or more @is_surebet is %FALSE and @profit is the loss, which is what
 * a calculator shows. A missing outcome is the caller's to refuse before
 * calling -- odds for two of three outcomes are not an event.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_surebet(
	const gdouble		 *odds,
	guint			  n,
	const VentureMoney	 *total,
	VentureArbitrageSurebet	 *out,
	GError			**error
);

/**
 * venture_arbitrage_surebet_clear:
 * @surebet: a split filled by venture_arbitrage_surebet()
 *
 * Frees what the split holds, leaving it zeroed.
 */
void
venture_arbitrage_surebet_clear(VentureArbitrageSurebet *surebet);

/* --- Back and lay ----------------------------------------------------------- */

/**
 * venture_arbitrage_effective_back_odds:
 * @odds: decimal odds to back at an exchange
 * @commission: the exchange's commission on net winnings, 0 to under 1
 *
 * Returns: 1 + (@odds - 1)(1 - @commission), or NAN for odds of one or
 *   less or a commission out of range
 */
gdouble
venture_arbitrage_effective_back_odds(
	gdouble	odds,
	gdouble	commission
);

/**
 * venture_arbitrage_effective_lay_odds:
 * @odds: decimal odds to lay at an exchange
 * @commission: the exchange's commission on net winnings, 0 to under 1
 *
 * Returns: 1 + (@odds - 1)/(1 - @commission): what laying really costs
 *   once the exchange keeps its share of a winning lay; NAN out of range
 */
gdouble
venture_arbitrage_effective_lay_odds(
	gdouble	odds,
	gdouble	commission
);

/**
 * VentureArbitrageBackLay:
 * @lay_stake: B*d_back/(L - c), rounded half to even
 * @liability: @lay_stake * (L - 1), rounded up: what the lay costs when
 *   the outcome happens, never a part of a minor unit less
 * @if_back_wins: B(d_back - 1) rounded down (a bookmaker never pays part
 *   of a minor unit), less @liability
 * @if_lay_wins: @lay_stake(1 - c) rounded down, less B
 * @worst: the lesser of the two: what the pair is sure to make. Every
 *   part of it is rounded against the person, so it is a guarantee and
 *   not an estimate
 * @ideal: B*d_back(1 - c)/(L - c) - B before rounding, rounded half to
 *   even; shown beside @worst, never instead of it
 * @rating: d_back(1 - c)/(L - c), the share of the back stake returned
 *   whichever way it goes; above one is a profit
 *
 * A back bet at one venue matched by a lay of the same outcome at an
 * exchange.
 */
typedef struct
{
	VentureMoney	*lay_stake;
	VentureMoney	*liability;
	VentureMoney	*if_back_wins;
	VentureMoney	*if_lay_wins;
	VentureMoney	*worst;
	VentureMoney	*ideal;
	gdouble		 rating;
} VentureArbitrageBackLay;

/**
 * venture_arbitrage_back_lay:
 * @back_odds: decimal odds backed (already net of any commission at the
 *   back venue, see venture_arbitrage_effective_back_odds())
 * @lay_odds: decimal odds laid at the exchange
 * @commission: the exchange's commission on a winning lay, 0 to under 1
 * @back_stake: B, positive
 * @out: (out caller-allocates): the figures; clear with
 *   venture_arbitrage_back_lay_clear()
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_back_lay(
	gdouble				 back_odds,
	gdouble				 lay_odds,
	gdouble				 commission,
	const VentureMoney		 *back_stake,
	VentureArbitrageBackLay		 *out,
	GError				**error
);

/**
 * venture_arbitrage_back_lay_clear:
 * @back_lay: figures filled by venture_arbitrage_back_lay()
 */
void
venture_arbitrage_back_lay_clear(VentureArbitrageBackLay *back_lay);

/* --- Flips ------------------------------------------------------------------ */

/**
 * venture_arbitrage_expected_relists:
 * @sale_rate: the share of listings that sell, (0, 1]
 * @out: (out): 1/@sale_rate - 1, the listings expected to expire before
 *   one sells
 * @error: (out) (optional): return location for a #GError
 *
 * A sale rate of zero is "never sells" -- infinitely many relists -- and
 * is refused with %VENTURE_ERROR_INVALID_ARGUMENT saying so; the scan
 * leaves such a row out and counts it in a note. An unknown rate (NAN) or
 * one above one is refused too: the caller decides what unknown means.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_expected_relists(
	gdouble		  sale_rate,
	gdouble		 *out,
	GError		**error
);

/**
 * venture_arbitrage_annualize:
 * @roi: a return over the hold, e.g. 0.10
 * @lock_days: how long the money is tied up, positive
 * @out: (out): (1 + @roi)^(365 / @lock_days) - 1
 * @error: (out) (optional): return location for a #GError
 *
 * A total loss (@roi of -1) annualises to -1. A hold of zero days or less
 * is refused, and so is an answer too large to be a number -- a 10% return
 * in a minute -- rather than reported as infinity.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_annualize(
	gdouble		  roi,
	gdouble		  lock_days,
	gdouble		 *out,
	GError		**error
);

/**
 * VentureArbitrageFlipInput:
 * @buy_cost: what the units cost to buy, in total
 * @buy_fees: (nullable): what buying costs on top
 * @sell_gross: what the units sell for before fees: units x the sell
 *   reference
 * @sell_fees: (nullable): the sell venue's cut and fixed fees on that sale
 * @deposit: (nullable): what listing the lot once costs
 * @deposit_refundable: whether a listing that sells gets its deposit back
 * @sale_rate: the share of listings that sell, or NAN when unknown
 * @transfer_cost: (nullable): what moving the units costs
 * @units: how many units, at least one
 * @sold_per_day: how many the market sells a day, or NAN
 * @share: the share of those sales this lot can expect, (0, 1]
 * @transit_days: days the units spend moving, at least 0
 * @fill_probability: the chance the trade completes, [0, 1], or NAN to
 *   use @sale_rate
 * @unwind_loss: (nullable): what is lost if it does not
 *
 * Every amount is in one currency; a caller converts first.
 */
typedef struct
{
	const VentureMoney	*buy_cost;
	const VentureMoney	*buy_fees;
	const VentureMoney	*sell_gross;
	const VentureMoney	*sell_fees;
	const VentureMoney	*deposit;
	gboolean		 deposit_refundable;
	gdouble			 sale_rate;
	const VentureMoney	*transfer_cost;
	gint64			 units;
	gdouble			 sold_per_day;
	gdouble			 share;
	gdouble			 transit_days;
	gdouble			 fill_probability;
	const VentureMoney	*unwind_loss;
} VentureArbitrageFlipInput;

/**
 * venture_arbitrage_flip_input_init:
 * @input: (out caller-allocates): the input to reset
 *
 * Zeroes @input and sets every "unknown" to its unknown value: the sale
 * rate, sold per day and fill probability to NAN, the share to one.
 */
void
venture_arbitrage_flip_input_init(VentureArbitrageFlipInput *input);

/**
 * VentureArbitrageFlip:
 * @relists: listings expected to expire before one sells, or NAN when the
 *   sale rate is unknown (then no relist is counted)
 * @listing_loss: deposits expected to be lost: @relists deposits when a
 *   sale refunds its own, @relists + 1 when nothing is refunded
 * @net: sell_gross - sell_fees - buy_cost - buy_fees - listing_loss -
 *   transfer_cost
 * @capital: what is tied up: buy_cost + buy_fees + one deposit +
 *   transfer_cost
 * @roi: @net / @capital
 * @lock_days: units/(sold_per_day x share) + transit_days, or NAN when
 *   sold per day is unknown
 * @roi_per_day: @roi / @lock_days, or NAN
 * @annualized: (1 + @roi)^(365/@lock_days) - 1, or NAN
 * @fill_probability: the probability used for @ev, or NAN
 * @ev: P(fill) x net - (1 - P(fill)) x unwind_loss, or %NULL when there
 *   is no probability
 *
 * What a flip is expected to make.
 */
typedef struct
{
	gdouble		 relists;
	VentureMoney	*listing_loss;
	VentureMoney	*net;
	VentureMoney	*capital;
	gdouble		 roi;
	gdouble		 lock_days;
	gdouble		 roi_per_day;
	gdouble		 annualized;
	gdouble		 fill_probability;
	VentureMoney	*ev;
} VentureArbitrageFlip;

/**
 * venture_arbitrage_flip:
 * @input: what is known
 * @out: (out caller-allocates): the figures; clear with
 *   venture_arbitrage_flip_clear()
 * @error: (out) (optional): return location for a #GError
 *
 * A sale rate of exactly zero is refused as "never sells" (see
 * venture_arbitrage_expected_relists()); an unknown one counts no relist.
 * Amounts in two currencies are refused: nothing here adds across them.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_flip(
	const VentureArbitrageFlipInput	 *input,
	VentureArbitrageFlip		 *out,
	GError				**error
);

/**
 * venture_arbitrage_flip_clear:
 * @flip: figures filled by venture_arbitrage_flip()
 */
void
venture_arbitrage_flip_clear(VentureArbitrageFlip *flip);

/* --- Confidence ------------------------------------------------------------- */

/**
 * VentureArbitrageEvidence:
 * @age_seconds: how old the oldest price used is
 * @interval_seconds: how often its venue updates (the learned interval),
 *   or 0 for the default hour
 * @venues: how many venues quote the instrument
 * @dispersion: the price's standard deviation over its mean (sigma/mu), or
 *   NAN when unknown
 * @depth: the share of the units wanted that is on offer at the price,
 *   [0, 1], or NAN when the book is unknown
 *
 * What a confidence is judged from.
 */
typedef struct
{
	gint64	age_seconds;
	gint64	interval_seconds;
	guint	venues;
	gdouble	dispersion;
	gdouble	depth;
} VentureArbitrageEvidence;

/**
 * venture_arbitrage_confidence:
 * @evidence: what is known
 *
 * A number in [0, 1], the product of four factors:
 *
 * - freshness: 1 while the data is no older than one update interval,
 *   falling linearly to 0 at four intervals: 1 - (age - I)/(3I), clamped;
 * - breadth: min(1, venues / 3) -- one venue quoting is a third as sure
 *   as three;
 * - stability: 1 / (1 + sigma/mu), and 0.8 when the dispersion is unknown
 *   (no evidence of a steady price is not evidence of one);
 * - depth: the share of the units on offer at the price, and 0.5 when the
 *   book is unknown.
 *
 * Returns: the confidence
 */
gdouble
venture_arbitrage_confidence(const VentureArbitrageEvidence *evidence);

/**
 * venture_arbitrage_roi:
 * @net: what was (or would be) made
 * @capital: what was tied up; positive, in @net's currency
 * @out: (out): @net / @capital
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_roi(
	const VentureMoney	 *net,
	const VentureMoney	 *capital,
	gdouble			 *out,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_MATH_H */
