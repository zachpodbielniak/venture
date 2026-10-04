/*
 * venture-arbitrage-records.c - Arbitrage trades and their legs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Trades
 *
 * One attempt: herbs bought on one realm and sold on another, a supplier's
 * SKU listed on a marketplace, both outcomes of a match backed at two
 * bookmakers. The trade holds what the attempt was meant to be -- its
 * strategy and, from the scan that found it, what it was expected to make
 * -- and where it stands. What actually happened is its legs.
 *
 * `opened-at` and `closed-at` are derived from the status by the save
 * validator (stamped when empty, kept when given, cleared on leaving), so
 * the performance report can measure a hold time without anybody typing
 * one. `close-journal-id` is the close action's stamp, and `expected` is
 * the scan's snapshot; both are machinery.
 * ========================================================================== */

static const VentureFieldDecl venture_arbitrage_trade_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this attempt: Peacebloom, Argent Dawn to Silvermoon"),
	VENTURE_FIELD("strategy", "Strategy",
	              "spread, transform, deal, cover, back_lay, or a plugin's; trades of one strategy are compared",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture it was for",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status",
	                   "planned, open, closed or abandoned; closed and abandoned are set by their actions",
	                   venture_arbitrage_trade_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("opened-at", "Opened",
	              "When it started; filled in when it opens",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("closed-at", "Closed",
	              "When it was closed or abandoned; set by those actions",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("expected", "Expected",
	              "What the scan expected, as JSON: profit per currency, ROI, the data's age",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("close-journal-id", "Closing journal",
	                  "Set by Close: the first journal that took the position to gains",
	                  "journal", VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureArbitrageTrade, venture_arbitrage_trade,
	venture_arbitrage_trade_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Arbitrage trade", NULL);)

/* ==========================================================================
 * Legs
 *
 * One movement at one venue: a purchase, a sale, a stake, a payout, a fee,
 * a transfer between venues. Its money moves through the venue's cash --
 * the holding at the venue's location, else the venue's account, else the
 * organization's cash -- and an executed leg posts on save through the
 * ledger's source registry (see venture-arbitrage.c for the table).
 *
 * `venue-id` is the first reference on purpose: a bare amount ("12.50")
 * is read in the currency of the first referenced record that names one,
 * and the venue is the one that does.
 *
 * A leg naming `inventory-item-id` moves stock as well as money, and only
 * the `execute` action may execute it: it receives or issues the units
 * through the inventory service and stamps `inventory-txn-id` and `cost`,
 * which nobody else may write.
 * ========================================================================== */

static const VentureFieldDecl venture_arbitrage_leg_fields[] = {
	VENTURE_FIELD_REF("venue-id", "Venue",
	                  "Where it happened; its holding, account or currency is used",
	                  "venue", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("trade-id", "Trade", "The trade it belongs to",
	                  "arbitrage_trade", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("kind", "Kind",
	                   "buy, sell, fee, transfer, stake, payout or refund; write_off is set by Abandon",
	                   venture_arbitrage_leg_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status",
	                   "planned, executed, failed or cancelled; only executed posts",
	                   venture_arbitrage_leg_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("instrument-id", "Instrument", "Optional: what was traded",
	                  "instrument", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("inventory-item-id", "Stock",
	                  "Optional, buy or sell: the stock the units go into or come out of",
	                  "inventory_item", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("quantity", "Quantity", "How many units; needed with stock",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-price", "Unit price",
	                    "Optional: the price of one unit; with a quantity it fills an empty amount"),
	VENTURE_FIELD_MONEY("amount", "Amount",
	                    "The money that moved, before fees; a transfer arriving here is negative"),
	VENTURE_FIELD_MONEY("fees", "Fees", "Optional: what the venue charged, in the amount's currency"),
	VENTURE_FIELD("occurred-at", "When",
	              "When it happened; filled in when it is executed",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("cost", "Cost of stock",
	              "Set by Execute and Abandon: what the units issued cost, first in first out",
	              VENTURE_FIELD_KIND_MONEY, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("inventory-txn-id", "Stock movement",
	                  "Set by Execute: the movement that received or issued the units",
	                  "inventory_txn", VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

/* "buy 12.5000 GOLD" -- the venue's name would need the database, which a
 * display name does not have, and the trade page shows it beside the leg. */
static gchar *
venture_arbitrage_leg_display_name(VentureEntity *self)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *money = NULL;
	const gchar *kind;
	gint value;

	value = 0;
	g_object_get(self, "kind", &value, "amount", &amount, NULL);
	kind = venture_enum_to_nick(VENTURE_TYPE_ARBITRAGE_LEG_KIND, value);

	if (NULL == amount)
		return g_strdup((NULL != kind) ? kind : "leg");

	money = venture_money_to_display_string(amount, TRUE);

	return g_strdup_printf("%s %s", (NULL != kind) ? kind : "leg", money);
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureArbitrageLeg, venture_arbitrage_leg,
	venture_arbitrage_leg_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = venture_arbitrage_leg_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Arbitrage leg", NULL);)

/* ==========================================================================
 * Presets
 *
 * A saved scan: which strategy, which data source, which venues to buy and
 * sell at, and the rest of the question (minimum profit, ROI, data age,
 * how many) as YAML in `options`, in the same names the arbitrage_scan
 * report and /arbitrage take. Loading one (`preset_id`) fills the
 * question; anything asked beside it wins. The save validator holds the
 * strategy to the registry and the options to the scan's own reader, so a
 * preset that saved is a preset that runs.
 * ========================================================================== */

static const VentureFieldDecl venture_arbitrage_strategy_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this preset: EU herbs, weekend surebets"),
	VENTURE_FIELD("strategy", "Strategy",
	              "spread, transform, deal, cover, back_lay, or a plugin's; spread when empty",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("data-source-id", "Data source",
	                  "Optional: scan only this source's stores; every source when empty",
	                  "data_source", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("buy-venues", "Buy at",
	              "Optional: venue keys to buy at, comma separated; every venue when empty",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sell-venues", "Sell at",
	              "Optional: venue keys to sell at, comma separated; every venue when empty",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("options", "Filters",
	              "The rest of the question, as YAML: min_profit: 10.00 GOLD, min_roi: 15, top: 20",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureArbitrageStrategy, venture_arbitrage_strategy,
	venture_arbitrage_strategy_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Arbitrage preset", NULL);)
