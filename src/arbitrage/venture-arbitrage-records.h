/*
 * venture-arbitrage-records.h - Arbitrage trades and their legs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The arbitrage module's record types. A trade is one attempt to buy low
 * and sell high, or to cover every outcome of an event; its legs are what
 * was actually done, one movement of money (and maybe stock) at one venue
 * each; a preset (`arbitrage_strategy`) is a saved scan. Both are field tables and nothing else: the rules that span
 * rows, the posting rule and the actions live in venture-arbitrage.c.
 */

#ifndef VENTURE_ARBITRAGE_RECORDS_H
#define VENTURE_ARBITRAGE_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_ARBITRAGE_TRADE (venture_arbitrage_trade_get_type())
VENTURE_DECLARE_ENTITY(VentureArbitrageTrade, venture_arbitrage_trade, ARBITRAGE_TRADE)

#define VENTURE_TYPE_ARBITRAGE_LEG (venture_arbitrage_leg_get_type())
VENTURE_DECLARE_ENTITY(VentureArbitrageLeg, venture_arbitrage_leg, ARBITRAGE_LEG)

#define VENTURE_TYPE_ARBITRAGE_STRATEGY (venture_arbitrage_strategy_get_type())
VENTURE_DECLARE_ENTITY(VentureArbitrageStrategy, venture_arbitrage_strategy, ARBITRAGE_STRATEGY)

G_END_DECLS

#endif /* VENTURE_ARBITRAGE_RECORDS_H */
