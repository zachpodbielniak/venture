/*
 * venture-arbitrage-private.h - What the arbitrage sources share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The one implementation of a trade's per-currency figures, shared by the
 * trade page (venture_arbitrage_trade_summary()) and the
 * arbitrage_performance report, so a card and a report row can never
 * disagree. Not installed: include "arbitrage/venture-arbitrage-private.h"
 * only from src/arbitrage.
 */

#ifndef VENTURE_ARBITRAGE_PRIVATE_H
#define VENTURE_ARBITRAGE_PRIVATE_H

#include "venture.h"

G_BEGIN_DECLS

/*
 * One currency's figures for one trade, from its executed legs:
 *
 *  capital       money out: buys (stock ones included), stakes, transfers
 *                leaving a venue
 *  returned      money in: sells, payouts, refunds, transfers arriving
 *  stock_bought  the part of capital that became stock
 *  stock_cost    what the units that left (sold or written off) cost
 *  fees          every fee, and fee legs' amounts
 *  realised      returned - (capital - stock_bought) - stock_cost - fees
 *
 * Every member is a money in @currency, zero when nothing moved.
 */
typedef struct
{
	gchar		*currency;
	VentureMoney	*capital;
	VentureMoney	*returned;
	VentureMoney	*stock_bought;
	VentureMoney	*stock_cost;
	VentureMoney	*fees;
	VentureMoney	*realised;
} VentureArbitrageFigures;

void
venture_arbitrage_figures_free(VentureArbitrageFigures *figures);

GPtrArray *
venture_arbitrage_trade_legs(
	VentureDatabase	 *database,
	gint64		  trade_id,
	gboolean	  include_deleted,
	GError		**error
);

GPtrArray *
venture_arbitrage_compute_figures(
	GPtrArray	 *legs,
	GError		**error
);

const VentureArbitrageFigures *
venture_arbitrage_figures_lookup(
	GPtrArray	*figures,
	const gchar	*currency
);

GPtrArray *
venture_arbitrage_expected_profit(
	const gchar	*expected
);

/*
 * The record, execute and close actions' own work, inside a transaction
 * and a posting boundary the caller already holds -- for recording flips
 * from an external ledger (venture-arbitrage-books.c), which funds a purse
 * between one leg and the next and records many trades in one operation.
 * A non-empty @external_ref stamps the trade with it and @data_source_id,
 * which nothing else may write. Legs asked for as executed are executed in
 * order by the record; the books record them planned and execute each
 * itself.
 */
gboolean
venture_arbitrage_record_in_transaction(
	VentureDatabase		 *database,
	gint64			  organization_id,
	JsonObject		 *request,
	gint64			  data_source_id,
	const gchar		 *external_ref,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
);

gboolean
venture_arbitrage_execute_in_transaction(
	VentureDatabase		 *database,
	gint64			  leg_id,
	GDateTime		 *occurred_at,
	const VentureActor	 *actor,
	VentureEntity		**out_leg,
	GError			**error
);

gboolean
venture_arbitrage_close_in_transaction(
	VentureDatabase		 *database,
	gint64			  trade_id,
	GDateTime		 *closed_at,
	const VentureActor	 *actor,
	VentureEntity		**out_trade,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_PRIVATE_H */
