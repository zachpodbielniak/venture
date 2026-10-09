/*
 * venture-arbitrage-crafting.h - What every recipe makes, and the flip
 *                                planner's shopping list
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two answers built on the arbitrage engine rather than beside it:
 *
 *  - crafting: the `transform` strategy asked about every recipe at once,
 *    with nothing left out for being a loss, at the realms a person picks
 *    (reagents bought at one realm or the cheapest, the craft sold at one
 *    realm or the best; a region-wide commodity market is open from
 *    every realm). Every figure is the strategy's: no second formula.
 *  - the flip planner: the organization's planned trades (what "Record
 *    attempt" and "Add to plan" make), regrouped as a shopping list --
 *    what to buy at each venue and what to post at each -- with the
 *    money it needs, what it should bring back after the venue's cut and
 *    lost deposits, and the profit; asked again on today's prices on
 *    request.
 *
 * docs/arbitrage.org ("Crafting" and "The flip planner") has every rule.
 */

#ifndef VENTURE_ARBITRAGE_CRAFTING_H
#define VENTURE_ARBITRAGE_CRAFTING_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_ARBITRAGE_PLANNER_MAX_TRADES:
 *
 * The most planned trades the planner reads. Past it the planner refuses
 * rather than total part of the plan: a shopping list that leaves trades
 * out is a list that buys short.
 */
#define VENTURE_ARBITRAGE_PLANNER_MAX_TRADES (500)

/**
 * VENTURE_ARBITRAGE_PLANNER_MAX_REPRICE:
 *
 * The most planned trades one re-pricing asks again: each is a scan, and
 * the server is single-threaded.
 */
#define VENTURE_ARBITRAGE_PLANNER_MAX_REPRICE (100)

/**
 * venture_arbitrage_crafting_option_names:
 *
 * The names the crafting question takes: data_source_id,
 * recipe_category_id, recipe_id, buy_realm, sell_realm, venue_group,
 * units, sell_basis, max_age_hours, share.
 *
 * Returns: (transfer none) (array zero-terminated=1): the names
 */
const gchar *const *
venture_arbitrage_crafting_option_names(void);

/**
 * venture_arbitrage_crafting:
 * @context: a #VentureContext
 * @organization_id: whose recipes and sources; 0 for the default
 * @asked: (nullable): the question, by venture_arbitrage_crafting_option_names()
 * @error: (out) (optional): return location for a #GError
 *
 * Every recipe the question names (every active one, or a recipe
 * category and beneath it), priced by the `transform` strategy with
 * losses kept: reagents at `buy_realm` (else the cheapest realm), the
 * output at `sell_realm` (else the best), commodities from the region's
 * market whichever realm is picked. Each row is the strategy's row with
 * the recipe's category path added (`recipe_category`). The answer also
 * carries the pickers' choices: `realm_choices` (the connected realms,
 * the region-wide markets left out), `categories` (the organization's
 * recipe categories, by path) and `venue_groups`; `options` is the scan's
 * question, which "Add to plan" sends back with a row's key.
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL with @error
 *   set (INVALID_ARGUMENT for an unknown option or a bad value, NOT_FOUND
 *   for a realm no source has)
 */
JsonNode *
venture_arbitrage_crafting(
	VentureContext	 *context,
	gint64		  organization_id,
	JsonObject	 *asked,
	GError		**error
);

/**
 * venture_arbitrage_planner_build:
 * @trades: the planned trades as JSON: each {id, name, strategy,
 *   expected (object, as recorded), legs: [{kind, status, venue_id,
 *   venue_name, venue_key, data_source_id, instrument_id,
 *   instrument_name, instrument_key, quantity, unit_price, amount,
 *   fees}]}; money as "12.50 GOLD" strings
 * @error: (out) (optional): return location for a #GError
 *
 * The shopping list, from plain data so it is the same whoever asks:
 * `buy` -- one group per venue, each instrument once with the units
 * added up and the most a unit may cost (the dearest planned price);
 * `sell` -- one group per venue, each instrument and price once with the
 * units added up; and `totals`, one per currency (never added across
 * two): `outlay` (every buy and fee leg, with buy fees), `gross` (what
 * the sells fetch), `cut` (the sell legs' fees), `deposit` (what posting
 * them ties up, refunded when they sell), `listing_loss` (the deposits
 * relisting is expected to lose), `revenue` (gross less cut less listing
 * loss) and `profit` (revenue less outlay). Only planned legs count.
 *
 * Returns: (transfer full) (nullable): {buy, sell, totals}, or %NULL with
 *   @error set when an amount cannot be read
 */
JsonObject *
venture_arbitrage_planner_build(
	JsonArray	 *trades,
	GError		**error
);

/**
 * venture_arbitrage_planner:
 * @context: a #VentureContext
 * @organization_id: whose plan; 0 for the default
 * @reprice: whether to ask each trade's question again on today's prices
 * @error: (out) (optional): return location for a #GError
 *
 * The organization's plan: every trade still `planned`, newest first,
 * with its legs, then venture_arbitrage_planner_build() over them. With
 * @reprice, each trade (at most %VENTURE_ARBITRAGE_PLANNER_MAX_REPRICE)
 * gets `reprice`: {state, net, planned_net, buy_stale, sell_stale,
 * message} -- `ok`, `stale` (still there, a price is old), `unprofitable`
 * (still there, now a loss), `unquoted` (a price or fee is missing), `gone`
 * (the question no longer finds it) or `unknown` (recorded before the
 * question was kept, or by hand).
 *
 * Returns: (transfer full) (nullable): {available, trades, buy, sell,
 *   totals, notes, repriced}, or %NULL with @error set (CONFLICT past
 *   %VENTURE_ARBITRAGE_PLANNER_MAX_TRADES planned trades)
 */
JsonNode *
venture_arbitrage_planner(
	VentureContext	 *context,
	gint64		  organization_id,
	gboolean	  reprice,
	GError		**error
);

/**
 * venture_arbitrage_planner_csv:
 * @answer: what venture_arbitrage_planner() answered
 *
 * The shopping list as CSV: `section` (buy or sell), venue, instrument,
 * key, quantity, unit price, amount, fees -- buy lines first, venue by
 * venue -- and a `total` line per currency. A field a spreadsheet would
 * run as a formula is defused as the scan's CSV export defuses it.
 *
 * Returns: (transfer full): the bytes
 */
GBytes *
venture_arbitrage_planner_csv(JsonObject *answer);

/**
 * venture_arbitrage_planner_export_rows:
 * @answer: what venture_arbitrage_planner() answered
 *
 * The buy lines as rows an export format reads ({instrument_key,
 * instrument_name, data_source_id}), each instrument once: what a game
 * addon's shopping import (the Blizzard plugin's `tsm`) is handed.
 *
 * Returns: (transfer full): the rows
 */
JsonArray *
venture_arbitrage_planner_export_rows(JsonObject *answer);

/**
 * venture_arbitrage_planner_remove:
 * @context: a #VentureContext
 * @organization_id: whose plan; 0 for the default
 * @trade_id: the planned trade
 * @actor: (nullable): who removes it
 * @error: (out) (optional): return location for a #GError
 *
 * Takes a trade off the plan: deletes its legs and the trade, in one
 * transaction. Only a trade still planned whose every leg is planned,
 * cancelled or failed: one that moved money is the books' and is closed
 * or abandoned instead. Deleting removes nothing from the books
 * (docs/arbitrage.org, "Deletion"); a planned trade posted nothing.
 *
 * Returns: %TRUE on success (NOT_FOUND for another organization's trade
 *   or none, CONFLICT for one that is not only planned)
 */
gboolean
venture_arbitrage_planner_remove(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  trade_id,
	const VentureActor	 *actor,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_CRAFTING_H */
