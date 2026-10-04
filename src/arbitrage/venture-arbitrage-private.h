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

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_PRIVATE_H */
