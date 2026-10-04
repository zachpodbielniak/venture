/*
 * venture-arbitrage-calc.c - The calculators behind /arbitrage/calc
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three questions a person asks before they record anything, answered on
 * the server from a form (so the page works without scripting) and by the
 * JSON twin, both through venture_arbitrage_calculate():
 *
 *  - surebet: odds for every outcome, in any format, and a total stake ->
 *    each stake, what it pays, the overround, the profit, and what
 *    rounding the stakes to the currency's minor unit leaves over;
 *  - back_lay: back odds, lay odds, the exchange's commission and a back
 *    stake -> the lay stake, the liability and both results;
 *  - flip: buy and sell prices, units, a venue's cut, fixed fees and
 *    deposit, the sale rate and the pace of sales -> net, ROI, ROI per
 *    day, annualised, listing loss and expected value.
 *
 * Nothing here reads the database. An amount that names no currency is
 * read in the install's default currency.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <math.h>
#include <string.h>

/* A field as text, NULL when absent or blank. */
static const gchar *
arb_calc_text(
	JsonObject	*input,
	const gchar	*name
){
	const gchar *text;

	text = venture_json_object_get_string(input, name, NULL);

	return venture_string_is_empty(text) ? NULL : text;
}

static VentureMoney *
arb_calc_money(
	JsonObject	 *input,
	const gchar	 *name,
	gboolean	  required,
	GError		**error
){
	VentureMoney *money;
	const gchar *text;

	text = arb_calc_text(input, name);

	if (NULL == text)
	{
		if (required)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s is an amount, e.g. 100.00 USD", name);
		return NULL;
	}

	money = venture_money_from_string(text, venture_money_get_default_currency(), error);

	if (NULL == money)
	{
		g_prefix_error(error, "%s: ", name);
		return NULL;
	}

	if (venture_money_is_negative(money))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s cannot be negative", name);
		venture_money_free(money);
		return NULL;
	}

	return money;
}

/* A percent field as a fraction; absent is @fallback. */
static gboolean
arb_calc_fraction(
	JsonObject	 *input,
	const gchar	 *name,
	gdouble		  fallback,
	gdouble		 *out,
	GError		**error
){
	gint64 ppm;

	*out = fallback;

	if (NULL == arb_calc_text(input, name))
		return TRUE;

	if (!venture_arbitrage_parse_percent(arb_calc_text(input, name), FALSE, &ppm, error))
	{
		g_prefix_error(error, "%s: ", name);
		return FALSE;
	}

	*out = (gdouble)ppm / 1000000.0;

	return TRUE;
}

static gboolean
arb_calc_number(
	JsonObject	 *input,
	const gchar	 *name,
	gdouble		  fallback,
	gdouble		 *out,
	GError		**error
){
	const gchar *text;
	gchar *end = NULL;

	*out = fallback;
	text = arb_calc_text(input, name);

	if (NULL == text)
		return TRUE;

	*out = g_ascii_strtod(text, &end);

	if ((NULL == end) || ('\0' != *end) || !isfinite(*out) || (*out < 0.0))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s is a number of at least 0", name);
		return FALSE;
	}

	return TRUE;
}

static gboolean
arb_calc_format(
	JsonObject		 *input,
	VentureOddsFormat	 *out,
	GError			**error
){
	if (!venture_arbitrage_odds_format_from_string(arb_calc_text(input, "format"), out))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "format is decimal, american, fractional or auto");
		return FALSE;
	}

	return TRUE;
}

static gboolean
arb_calc_odds(
	JsonObject		 *input,
	const gchar		 *name,
	VentureOddsFormat	  format,
	gdouble			 *out,
	GError			**error
){
	if (NULL == arb_calc_text(input, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s: odds are missing", name);
		return FALSE;
	}

	if (!venture_arbitrage_odds_parse(arb_calc_text(input, name), format, out, error))
	{
		g_prefix_error(error, "%s: ", name);
		return FALSE;
	}

	return TRUE;
}

static void
arb_calc_money_array(
	JsonObject	*object,
	const gchar	*name,
	GPtrArray	*amounts
){
	JsonArray *array;
	guint i;

	array = json_array_new();

	for (i = 0; (NULL != amounts) && (i < amounts->len); i++)
		json_array_add_element(array, venture_money_to_json(g_ptr_array_index(amounts, i)));

	json_object_set_array_member(object, name, array);
}

/* --- surebet ------------------------------------------------------------------ */

static JsonObject *
arb_calc_surebet(
	JsonObject	 *input,
	GError		**error
){
	g_autoptr(VentureMoney) stake = NULL;
	g_autoptr(GArray) odds = NULL;
	g_auto(GStrv) parts = NULL;
	g_autofree gdouble *fair = NULL;
	VentureArbitrageSurebet split;
	VentureOddsFormat format;
	JsonObject *answer;
	JsonArray *array;
	const gchar *text;
	guint i;

	if (!arb_calc_format(input, &format, error))
		return NULL;

	text = arb_calc_text(input, "odds");

	if (NULL == text)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "odds: give the best odds for every outcome, e.g. 2.10, 2.05");
		return NULL;
	}

	/* One outcome per comma, space or line: "2.1, 2.05" and "+150 -120"
	 * both read. */
	parts = g_regex_split_simple("[,;\\s]+", text, 0, 0);
	odds = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (i = 0; NULL != parts[i]; i++)
	{
		gdouble value;

		if ('\0' == parts[i][0])
			continue;

		if (!venture_arbitrage_odds_parse(parts[i], format, &value, error))
		{
			g_prefix_error(error, "Outcome %u: ", odds->len + 1);
			return NULL;
		}

		g_array_append_val(odds, value);
	}

	stake = arb_calc_money(input, "stake", TRUE, error);

	if ((NULL == stake) ||
	    !venture_arbitrage_surebet((const gdouble *)odds->data, odds->len, stake, &split, error))
		return NULL;

	fair = g_new0(gdouble, odds->len);
	venture_arbitrage_no_vig((const gdouble *)odds->data, odds->len, fair, NULL);

	answer = json_object_new();
	json_object_set_string_member(answer, "calculator", "surebet");
	array = json_array_new();

	for (i = 0; i < odds->len; i++)
	{
		JsonObject *outcome = json_object_new();

		json_object_set_double_member(outcome, "odds", g_array_index(odds, gdouble, i));
		json_object_set_double_member(outcome, "implied",
		                              venture_arbitrage_implied_probability(g_array_index(odds, gdouble, i)));
		json_object_set_double_member(outcome, "no_vig", fair[i]);
		json_object_set_member(outcome, "stake", venture_money_to_json(g_ptr_array_index(split.stakes, i)));
		json_object_set_member(outcome, "payout", venture_money_to_json(g_ptr_array_index(split.payouts, i)));
		json_array_add_object_element(array, outcome);
	}

	json_object_set_array_member(answer, "outcomes", array);
	json_object_set_double_member(answer, "sum", split.sum);
	json_object_set_double_member(answer, "overround", split.overround);
	json_object_set_double_member(answer, "margin", split.margin);
	json_object_set_boolean_member(answer, "is_surebet", split.is_surebet);
	venture_arbitrage_set_money(answer, "total", stake);
	arb_calc_money_array(answer, "stakes", split.stakes);
	arb_calc_money_array(answer, "payouts", split.payouts);
	venture_arbitrage_set_money(answer, "staked", split.staked);
	venture_arbitrage_set_money(answer, "residual", split.residual);
	venture_arbitrage_set_money(answer, "payout", split.payout);
	venture_arbitrage_set_money(answer, "profit", split.profit);
	venture_arbitrage_set_money(answer, "worst_payout", split.worst_payout);
	venture_arbitrage_set_money(answer, "guaranteed", split.guaranteed);
	venture_arbitrage_surebet_clear(&split);

	return answer;
}

/* --- back_lay ------------------------------------------------------------------ */

static JsonObject *
arb_calc_back_lay(
	JsonObject	 *input,
	GError		**error
){
	g_autoptr(VentureMoney) stake = NULL;
	VentureArbitrageBackLay figures;
	VentureOddsFormat format;
	JsonObject *answer;
	gdouble back;
	gdouble lay;
	gdouble commission;
	gdouble back_commission;
	gdouble effective;

	if (!arb_calc_format(input, &format, error) ||
	    !arb_calc_odds(input, "back_odds", format, &back, error) ||
	    !arb_calc_odds(input, "lay_odds", format, &lay, error) ||
	    !arb_calc_fraction(input, "commission", 0.0, &commission, error) ||
	    !arb_calc_fraction(input, "back_commission", 0.0, &back_commission, error))
		return NULL;

	stake = arb_calc_money(input, "stake", TRUE, error);

	if (NULL == stake)
		return NULL;

	effective = venture_arbitrage_effective_back_odds(back, back_commission);

	if (!isfinite(effective))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "back_commission is a percent under 100");
		return NULL;
	}

	if (!venture_arbitrage_back_lay(effective, lay, commission, stake, &figures, error))
		return NULL;

	answer = json_object_new();
	json_object_set_string_member(answer, "calculator", "back_lay");
	json_object_set_double_member(answer, "back_odds", back);
	json_object_set_double_member(answer, "lay_odds", lay);
	json_object_set_double_member(answer, "effective_back_odds", effective);
	json_object_set_double_member(answer, "effective_lay_odds",
	                              venture_arbitrage_effective_lay_odds(lay, commission));
	json_object_set_double_member(answer, "commission", commission);
	json_object_set_double_member(answer, "rating", figures.rating);
	venture_arbitrage_set_money(answer, "back_stake", stake);
	venture_arbitrage_set_money(answer, "lay_stake", figures.lay_stake);
	venture_arbitrage_set_money(answer, "liability", figures.liability);
	venture_arbitrage_set_money(answer, "if_back_wins", figures.if_back_wins);
	venture_arbitrage_set_money(answer, "if_lay_wins", figures.if_lay_wins);
	venture_arbitrage_set_money(answer, "worst", figures.worst);
	venture_arbitrage_set_money(answer, "ideal", figures.ideal);
	venture_arbitrage_back_lay_clear(&figures);

	return answer;
}

/* --- flip ---------------------------------------------------------------------- */

static JsonObject *
arb_calc_flip(
	JsonObject	 *input,
	GError		**error
){
	g_autoptr(VentureMoney) buy = NULL;
	g_autoptr(VentureMoney) sell = NULL;
	g_autoptr(VentureMoney) fixed = NULL;
	g_autoptr(VentureMoney) transfer = NULL;
	g_autoptr(VentureMoney) buy_cost = NULL;
	g_autoptr(VentureMoney) gross = NULL;
	g_autoptr(VentureFeeModelRegistry) models = NULL;
	g_autoptr(JsonObject) params = NULL;
	VentureArbitrageFlipInput flip_input;
	VentureArbitrageFlip flip;
	VentureFeeQuote quote;
	JsonObject *answer;
	gdouble units_value;
	gdouble rate;
	gdouble per_day;
	gdouble share;
	gdouble hours;
	gint64 units;

	buy = arb_calc_money(input, "buy", TRUE, error);
	sell = (NULL != buy) ? arb_calc_money(input, "sell", TRUE, error) : NULL;

	if (NULL == sell)
		return NULL;

	if (0 != g_strcmp0(venture_money_get_currency(buy), venture_money_get_currency(sell)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "buy is in %s and sell in %s: the calculator does not convert",
		            venture_money_get_currency(buy), venture_money_get_currency(sell));
		return NULL;
	}

	if (!arb_calc_number(input, "units", 1.0, &units_value, error) ||
	    !arb_calc_fraction(input, "sale_rate", NAN, &rate, error) ||
	    !arb_calc_number(input, "sold_per_day", NAN, &per_day, error) ||
	    !arb_calc_fraction(input, "share", 1.0, &share, error) ||
	    !arb_calc_number(input, "transit_hours", 0.0, &hours, error))
		return NULL;

	if ((units_value < 1.0) || (units_value > VENTURE_ARBITRAGE_MAX_UNITS) ||
	    (floor(units_value) != units_value))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "units is a whole number from 1 to %d", VENTURE_ARBITRAGE_MAX_UNITS);
		return NULL;
	}

	units = (gint64)units_value;

	/* Both optional: absent is no fee, unreadable is a refusal. */
	{
		g_autoptr(GError) local_error = NULL;

		fixed = arb_calc_money(input, "fixed", FALSE, &local_error);

		if (NULL == local_error)
			transfer = arb_calc_money(input, "transfer", FALSE, &local_error);

		if (NULL != local_error)
		{
			g_propagate_error(error, g_steal_pointer(&local_error));
			return NULL;
		}
	}

	buy_cost = venture_money_multiply_int(buy, units, error);
	gross = (NULL != buy_cost) ? venture_money_multiply_int(sell, units, error) : NULL;

	if (NULL == gross)
		return NULL;

	/* The sell venue's fees through the percent model, the one a venue
	 * would name: the same arithmetic as the scan. */
	params = json_object_new();

	if (NULL != arb_calc_text(input, "cut"))
		json_object_set_string_member(params, "cut_percent", arb_calc_text(input, "cut"));

	if (NULL != fixed)
	{
		g_autofree gchar *text = venture_money_to_string(fixed);

		json_object_set_string_member(params, "fixed_per_order", text);
	}

	if (NULL != arb_calc_text(input, "deposit"))
		json_object_set_string_member(params, "deposit_percent", arb_calc_text(input, "deposit"));

	if (NULL != arb_calc_text(input, "refundable"))
		json_object_set_string_member(params, "deposit_refundable", arb_calc_text(input, "refundable"));

	models = venture_fee_model_registry_new();

	if (!venture_fee_model_registry_compute(models, "percent", params, VENTURE_FEE_SIDE_SELL, gross,
	                                        units, NULL, &quote, error))
		return NULL;

	if ((NULL != transfer) &&
	    (0 != g_strcmp0(venture_money_get_currency(transfer), venture_money_get_currency(buy))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "transfer is in another currency than the prices");
		venture_fee_quote_clear(&quote);
		return NULL;
	}

	venture_arbitrage_flip_input_init(&flip_input);
	flip_input.buy_cost = buy_cost;
	flip_input.sell_gross = gross;
	flip_input.sell_fees = quote.fee;
	flip_input.deposit = quote.deposit;
	flip_input.deposit_refundable = quote.deposit_refundable || (NULL == quote.deposit);
	flip_input.sale_rate = rate;
	flip_input.transfer_cost = transfer;
	flip_input.units = units;
	flip_input.sold_per_day = per_day;
	flip_input.share = share;
	flip_input.transit_days = hours / 24.0;
	flip_input.unwind_loss = transfer;

	if (!venture_arbitrage_flip(&flip_input, &flip, error))
	{
		venture_fee_quote_clear(&quote);
		return NULL;
	}

	answer = json_object_new();
	json_object_set_string_member(answer, "calculator", "flip");
	json_object_set_int_member(answer, "units", units);
	venture_arbitrage_set_money(answer, "buy_cost", buy_cost);
	venture_arbitrage_set_money(answer, "gross", gross);
	venture_arbitrage_set_money(answer, "sell_fees", quote.fee);
	venture_arbitrage_set_money(answer, "deposit", quote.deposit);
	venture_arbitrage_set_money(answer, "transfer", transfer);
	venture_arbitrage_set_money(answer, "listing_loss", flip.listing_loss);
	venture_arbitrage_set_money(answer, "net", flip.net);
	venture_arbitrage_set_money(answer, "capital", flip.capital);
	venture_arbitrage_set_money(answer, "ev", flip.ev);
	venture_arbitrage_set_ratio(answer, "relists", flip.relists);
	venture_arbitrage_set_ratio(answer, "roi", flip.roi);
	venture_arbitrage_set_ratio(answer, "lock_days", flip.lock_days);
	venture_arbitrage_set_ratio(answer, "roi_per_day", flip.roi_per_day);
	venture_arbitrage_set_ratio(answer, "annualized", flip.annualized);
	venture_arbitrage_set_ratio(answer, "fill_probability", flip.fill_probability);
	venture_arbitrage_flip_clear(&flip);
	venture_fee_quote_clear(&quote);

	return answer;
}

JsonNode *
venture_arbitrage_calculate(
	const gchar	 *calculator,
	JsonObject	 *input,
	GError		**error
){
	g_autoptr(JsonObject) empty = NULL;
	JsonObject *answer;
	JsonNode *node;

	if (NULL == input)
		input = empty = json_object_new();

	if (0 == g_strcmp0(calculator, "surebet"))
		answer = arb_calc_surebet(input, error);
	else if (0 == g_strcmp0(calculator, "back_lay"))
		answer = arb_calc_back_lay(input, error);
	else if (0 == g_strcmp0(calculator, "flip"))
		answer = arb_calc_flip(input, error);
	else
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "calc is surebet, back_lay or flip, not \"%s\"",
		            (NULL != calculator) ? calculator : "");
		return NULL;
	}

	if (NULL == answer)
		return NULL;

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, answer);

	return node;
}
