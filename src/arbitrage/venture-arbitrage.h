/*
 * venture-arbitrage.h - Arbitrage trades, their legs, and the ledger
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The arbitrage module records an attempt and makes it reach the books.
 * A `arbitrage_trade` is the attempt; its `arbitrage_leg` rows are what
 * was done, each at one venue. An executed leg posts on save through the
 * ledger's source registry (rule `arbitrage_leg`): its money moves between
 * the venue's cash and the organization's arbitrage positions account, its
 * fees to arbitrage fees. A leg that moves stock is executed by the
 * `execute` action, which receives or issues the units through the
 * inventory service against the same two accounts. What is left on the
 * positions account is the trade's profit or loss, and the `close` action
 * moves it to arbitrage gains, one journal per book currency section.
 */

#ifndef VENTURE_ARBITRAGE_H
#define VENTURE_ARBITRAGE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_ARBITRAGE_LEG_RULE:
 *
 * The posting rule an executed leg's save posts through, and the rule name
 * its journals carry.
 */
#define VENTURE_ARBITRAGE_LEG_RULE "arbitrage_leg"

/**
 * VENTURE_ARBITRAGE_CLOSE_RULE:
 *
 * The rule name a trade's closing journals carry; `reopen` reverses
 * exactly these.
 */
#define VENTURE_ARBITRAGE_CLOSE_RULE "arbitrage_close"

/**
 * VENTURE_ARBITRAGE_MAX_LEGS:
 *
 * The most legs one `record` call may create. A bound, not a business
 * rule: a surebet across every bookmaker in a country is a few dozen.
 */
#define VENTURE_ARBITRAGE_MAX_LEGS (200)

/**
 * venture_arbitrage_install:
 * @context: the wiring
 *
 * Installs the save validators for `arbitrage_trade` (status changes the
 * actions own, derived opened and closed times, the expected snapshot) and
 * `arbitrage_leg` (amounts, currencies, references in the trade's
 * organization, stock legs executed only by `execute`, executed legs of a
 * closed trade frozen), registers the `arbitrage_leg` posting rule and
 * source type, and the `execute`, `close`, `reopen`, `abandon` and
 * `record` actions. Called once by the context; a second context over the
 * same database installs nothing twice.
 */
void
venture_arbitrage_install(VentureContext *context);

/**
 * venture_arbitrage_leg_direction:
 * @kind: what the leg did
 * @amount: (nullable): its amount; only a transfer's sign matters
 *
 * Which way a leg's money went: out of the venue into the position (a
 * buy, a stake, a transfer with a positive amount), back into the venue (a
 * sell, a payout, a refund, a transfer with a negative amount), or neither
 * (a fee, a write-off).
 *
 * Returns: 1 for out, -1 for in, 0 for neither
 */
gint
venture_arbitrage_leg_direction(
	VentureArbitrageLegKind	 kind,
	const VentureMoney	*amount
);

/**
 * venture_arbitrage_account:
 * @database: the database
 * @organization_id: the organization
 * @classification: `arbitrage_positions`, `arbitrage_gains`,
 *   `arbitrage_fees`, or one of the external ledger's: `trading_sales`,
 *   `trading_purchases`, `trading_income`, `trading_expenses`,
 *   `trading_capital`
 * @when: (nullable): the date a dated control map is read at
 * @actor: (nullable): audit actor for an account made on first use
 * @error: (out) (optional): return location for a #GError
 *
 * The organization's account for one of the module's roles: the control
 * map's when one is set, else one made on first use under a scoped code
 * (`<org>:1460` positions, `<org>:4960` gains, `<org>:6960` fees,
 * `<org>:4970` trading sales, `<org>:5970` trading purchases,
 * `<org>:4980` trading other income, `<org>:6970` trading expenses,
 * `<org>:3970` trading capital) so it can never take a number a chart
 * already uses. For writers only: a read uses
 * venture_arbitrage_find_account().
 *
 * Returns: the account id, or 0 with @error set
 */
gint64
venture_arbitrage_account(
	VentureDatabase		 *database,
	gint64			  organization_id,
	const gchar		 *classification,
	GDateTime		 *when,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_arbitrage_find_account:
 * @database: the database
 * @organization_id: the organization
 * @classification: any classification venture_arbitrage_account() takes
 * @when: (nullable): the date a dated control map is read at
 * @out_account_id: (out): the account, or 0 when there is none yet
 * @error: (out) (optional): return location for a #GError
 *
 * venture_arbitrage_account() without the making: the control map's
 * account, else the scoped one if it exists. Every read path -- the trade
 * page, the summary, a position -- asks this, so looking at a trade never
 * adds an account to the chart; only writers make one.
 *
 * Returns: %TRUE on success (with 0 when no account exists), %FALSE with
 *   @error set for an unknown classification or a failed query
 */
gboolean
venture_arbitrage_find_account(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *classification,
	GDateTime	 *when,
	gint64		 *out_account_id,
	GError		**error
);

/**
 * venture_arbitrage_venue_cash_account:
 * @database: the database
 * @organization_id: the organization the money belongs to
 * @venue_id: the venue, or 0 for none
 * @when: (nullable): the date a dated control map is read at
 * @actor: (nullable): audit actor for an account made on first use
 * @out_account_id: (out): the account
 * @error: (out) (optional): return location for a #GError
 *
 * Where a leg's money moves through at a venue: the holding at the
 * venue's location (made on first use), else the venue's own account,
 * else the organization's cash (the `cash` control map, else the chart's
 * 1000, else one made under `<org>:1000`).
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_venue_cash_account(
	VentureDatabase		 *database,
	gint64			  organization_id,
	gint64			  venue_id,
	GDateTime		 *when,
	const VentureActor	 *actor,
	gint64			 *out_account_id,
	GError			**error
);

/**
 * venture_arbitrage_execute_leg:
 * @database: the database
 * @leg_id: a planned or failed leg
 * @occurred_at: (nullable): when it happened; the leg's own time, else now
 * @actor: (nullable): audit actor
 * @out_leg: (out) (optional) (transfer full): the leg as saved
 * @error: (out) (optional): return location for a #GError
 *
 * Executes one leg in one transaction, inside the whole-operation posting
 * boundary. A leg with stock first moves the units: a buy is received
 * paid from the venue's cash (venture_inventory_service_receive_from()),
 * a sell is issued at first-in-first-out cost into the positions account
 * (venture_inventory_service_issue_to()), and the leg is stamped with the
 * movement and the cost. The leg is then saved executed, which posts its
 * money and fees through the `arbitrage_leg` rule. A planned trade becomes
 * open. A leg of a closed or abandoned trade is refused.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_execute_leg(
	VentureDatabase		 *database,
	gint64			  leg_id,
	GDateTime		 *occurred_at,
	const VentureActor	 *actor,
	VentureEntity		**out_leg,
	GError			**error
);

/**
 * venture_arbitrage_position:
 * @database: the database
 * @trade_id: the trade
 * @error: (out) (optional): return location for a #GError
 *
 * What the trade has left on the organization's arbitrage positions
 * account, one balance per book section (journal currency), debits
 * positive: the lines its legs' journals, its stock movements' journals
 * and its own closing journals put there, deleted legs included -- a
 * deletion is not a correction, and its journals stand.
 *
 * Returns: (transfer full) (element-type VentureMoney) (nullable): the
 *   balances, zeros included, or %NULL with @error set
 */
GPtrArray *
venture_arbitrage_position(
	VentureDatabase	 *database,
	gint64		  trade_id,
	GError		**error
);

/**
 * venture_arbitrage_close:
 * @database: the database
 * @trade_id: a planned or open trade
 * @closed_at: (nullable): when; now when %NULL, never before its last
 *   executed leg
 * @actor: (nullable): audit actor
 * @out_trade: (out) (optional) (transfer full): the trade as saved
 * @error: (out) (optional): return location for a #GError
 *
 * Closes the trade: what is left on the positions account
 * (venture_arbitrage_position()) is moved to arbitrage gains -- a profit
 * credited, a loss debited -- one journal per book section, each in its
 * own currency and never added to another, keyed
 * `arbitrage_close:<trade>:<n>:<CODE>` so a reopened trade closes again.
 * The trade is stamped with the first journal, its closing time and, when
 * empty, its opening time (its first executed leg).
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_close(
	VentureDatabase		 *database,
	gint64			  trade_id,
	GDateTime		 *closed_at,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
);

/**
 * venture_arbitrage_reopen:
 * @database: the database
 * @trade_id: a closed or abandoned trade
 * @actor: (nullable): audit actor
 * @out_trade: (out) (optional) (transfer full): the trade as saved
 * @error: (out) (optional): return location for a #GError
 *
 * Reverses every closing journal the trade still has posted and sets it
 * open again. Stock written off by an abandon stays written off: the
 * write-off legs are executed legs like any other.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_reopen(
	VentureDatabase		 *database,
	gint64			  trade_id,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
);

/**
 * venture_arbitrage_abandon:
 * @database: the database
 * @trade_id: a planned or open trade
 * @write_off: %TRUE to write off the units the trade bought and still
 *   holds, %FALSE to keep them in stock at their cost
 * @when: (nullable): when; now when %NULL
 * @actor: (nullable): audit actor
 * @out_trade: (out) (optional) (transfer full): the trade as saved
 * @error: (out) (optional): return location for a #GError
 *
 * Gives the trade up. With @write_off, each stock the trade's legs bought
 * into and did not sell out of loses the remaining units (never more than
 * are on hand): a `write_off` leg issues them at first-in-first-out cost
 * into the positions account. The trade is then closed as
 * venture_arbitrage_close() does, with the status abandoned.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_abandon(
	VentureDatabase		 *database,
	gint64			  trade_id,
	gboolean		  write_off,
	GDateTime		 *when,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
);

/**
 * venture_arbitrage_record:
 * @database: the database
 * @organization_id: the organization the trade belongs to
 * @request: `name` (required), `strategy`, `venture_id`, `expected` (a
 *   JSON object, or its text), `notes`, and `legs`: an array of objects
 *   with `kind`, `status`, `venue_id`, `instrument_id`,
 *   `inventory_item_id`, `quantity`, `unit_price`, `amount`, `fees`,
 *   `occurred_at` and `notes` in their wire spelling
 * @actor: (nullable): audit actor
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a trade and its legs in one transaction, inside the
 * whole-operation posting boundary. Legs asked for as executed are
 * executed as venture_arbitrage_execute_leg() does, in the order given,
 * which opens the trade. A leg naming any other field is refused, and so
 * is a request with no legs or more than %VENTURE_ARBITRAGE_MAX_LEGS.
 *
 * Returns: (transfer full) (nullable): the trade, or %NULL with @error set
 */
VentureEntity *
venture_arbitrage_record(
	VentureDatabase		 *database,
	gint64			  organization_id,
	JsonObject		 *request,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_arbitrage_trade_summary:
 * @database: the database
 * @trade_id: the trade
 * @error: (out) (optional): return location for a #GError
 *
 * What the trade page shows, as JSON: `figures`, one object per currency
 * its executed legs moved (`currency`, `capital`, `returned`,
 * `stock_bought`, `stock_cost`, `fees`, `realised`, each a money object);
 * `position`, the positions balances per book section; `legs` (serialised
 * legs, deleted ones included with `deleted: true`); and `journals`, the
 * ids of every journal the trade, its legs and their stock movements
 * posted. The same arithmetic as the `arbitrage_performance` report.
 *
 * Returns: (transfer full) (nullable): the summary, or %NULL with @error set
 */
JsonNode *
venture_arbitrage_trade_summary(
	VentureDatabase	 *database,
	gint64		  trade_id,
	GError		**error
);

/**
 * venture_arbitrage_performance:
 * @context: the wiring
 * @period: (nullable): bounds `closed-at`
 * @options: (nullable): `group_by` (strategy, venue_pair, instrument or
 *   month), `strategy`, `venture_id`, `organization_id`
 * @error: (out) (optional): return location for a #GError
 *
 * How finished trades (closed or abandoned) did, one row per group and
 * currency, never a sum across currencies: realised profit, capital
 * deployed, ROI, fees, hit rate, the average hold time and, where the
 * trade carried an expected snapshot, expected against realised.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @error set
 */
VentureReportResult *
venture_arbitrage_performance(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
);

/**
 * venture_arbitrage_realised_totals:
 * @context: the wiring
 * @organization_id: the organization; zero or less for the default one
 * @venture_id: only trades filed under this venture; zero for every one
 * @period: (nullable): bounds `closed-at`; %NULL for every finished trade
 * @as_of: (nullable): a report's `as_of` visibility cutoff, as
 *   venture_query_set_as_of() takes it: a trade deleted after it still
 *   counts, as a sale deleted after it does in the same report; %NULL
 *   for what is live now
 * @out_gains: (out) (transfer full) (element-type VentureMoney): per
 *   currency, what the finished trades made before their fees -- the
 *   ledger's "Arbitrage gains" -- as venture_money_totals_new() totals
 * @out_fees: (out) (transfer full) (element-type VentureMoney): per
 *   currency, the fees their executed legs paid
 * @out_realised: (out) (transfer full) (element-type VentureMoney): per
 *   currency, @out_gains less @out_fees: each trade's realised result
 * @out_trades: (out) (optional): how many finished trades were counted
 * @error: (out) (optional): return location for a #GError
 *
 * What the operational reports (`pnl`, `ventures`, `monthly`) add for
 * arbitrage: the trades that finished -- closed or abandoned -- in
 * @period, by `closed-at`, with exactly the arithmetic of the trade page
 * and `arbitrage_performance` (venture_arbitrage_trade_summary()). A
 * trade's legs are neither sales nor expenses, so nothing here is counted
 * twice by a report that also totals those. Kept per currency and never
 * converted. With the arbitrage module off the totals are empty and the
 * count zero; past venture_aggregate_get_max_rows() finished trades it
 * refuses rather than total a part.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_arbitrage_realised_totals(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  venture_id,
	VentureDateRange	 *period,
	GDateTime		 *as_of,
	GPtrArray		**out_gains,
	GPtrArray		**out_fees,
	GPtrArray		**out_realised,
	guint			 *out_trades,
	GError			**error
);

/**
 * venture_arbitrage_register_reports:
 * @registry: the report registry
 *
 * Registers `arbitrage_performance`. It belongs to the arbitrage module,
 * so switching that off hides it.
 */
void
venture_arbitrage_register_reports(VentureReportRegistry *registry);

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_H */
