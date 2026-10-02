/*
 * venture-sessions-records.c - Sessions and what they yielded
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Sessions
 *
 * A time-boxed stretch of effort whose outcome is measured: a farming run
 * in a game, a day behind a market stall, a study block, a production
 * shift, an afternoon of foraging. Nothing here knows which. What makes
 * them comparable is `activity` -- the kind of run -- and the report that
 * divides what each kind yielded by the hours it took.
 *
 * `minutes` is derived by the save validator from the two times, so the
 * list and the report can read a duration without doing date arithmetic;
 * `posted-at` is written by the `post` action only (see
 * venture-sessions.c), which is why it is technical.
 * ========================================================================== */

static const VentureFieldDecl venture_session_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this run"),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture it was for",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("activity", "Activity",
	              "What kind of run it was; sessions of one activity are compared",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("category-id", "Category", "Optional: where it is filed",
	                  "category", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("location-id", "Location",
	                  "Optional: where it happened, and where its yields land",
	                  "location", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("started-at", "Started", "When it began",
	              VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("ended-at", "Ended",
	              "When it finished; empty while it is still going",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("minutes", "Minutes",
	              "How long it took; worked out from the two times",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("cost", "Cost",
	                    "Optional: what it took -- consumables, fees, fuel"),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("posted-at", "Posted",
	              "When its yields were last put into stock; set by Post",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_TECHNICAL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureSession, venture_session, venture_session_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Session", NULL);)

/* ==========================================================================
 * Yields
 *
 * One thing a session produced, in one of two forms and never both:
 *
 *  - goods: `product-id` and `quantity` -- ore mined, pastries baked,
 *    pages written up as a product -- which `post` puts into stock;
 *  - money: `amount` -- coins looted, cash taken at the stall, a bounty --
 *    which is counted by the report and never touches inventory.
 *
 * `unit-value` is what one unit was reckoned to be worth when it was
 * recorded. It is a valuation, not a cost: nothing was paid for a herb
 * picked off the ground, and a cost layer at the herb's market price would
 * book a profit when it was picked and none when it sold. `post` therefore
 * gives the received units a zero cost layer, and the report reads
 * `unit-value` only to say what the run was worth.
 * ========================================================================== */

static const VentureFieldDecl venture_session_yield_fields[] = {
	VENTURE_FIELD_REF("session-id", "Session", "The session that produced it",
	                  "session", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product",
	                  "Goods: what it produced (or give an amount instead)",
	                  "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("quantity", "Quantity", "Goods: how many units; at least one",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount",
	                    "Money: what it produced directly (or give a product instead)"),
	VENTURE_FIELD_MONEY("unit-value", "Unit value",
	                    "Goods, optional: what one unit was worth when recorded"),
	VENTURE_FIELD_REF("inventory-item-id", "Stock",
	                  "Goods, optional: the stock it lands in when posted",
	                  "inventory_item", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("inventory-txn-id", "Stock movement",
	                  "Set by Post: the movement that put it into stock",
	                  "inventory_txn", VENTURE_COLUMN_FLAG_TECHNICAL),
	/* Money's two stamps, one per kind of currency: a posted currency
	 * lands as a journal, a memo one as a holding movement. Either one
	 * set is what makes a money yield posted. */
	VENTURE_FIELD_REF("journal-id", "Journal",
	                  "Set by Post: the journal that put the money into the holding",
	                  "journal", VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("holding-txn-id", "Holding movement",
	                  "Set by Post: the memo movement that put the money into the holding",
	                  "holding_txn", VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

/* "x12" or "34.00 GOLD" -- the product's name would need the database,
 * which a display name does not have, and the session page's related
 * list shows the product beside it anyway. */
static gchar *
venture_session_yield_display_name(VentureEntity *self)
{
	g_autoptr(VentureMoney) amount = NULL;
	gint64 quantity;

	g_object_get(self, "quantity", &quantity, "amount", &amount, NULL);

	if (NULL != amount)
		return venture_money_to_display_string(amount, TRUE);

	return g_strdup_printf("x%" G_GINT64_FORMAT, quantity);
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureSessionYield, venture_session_yield,
	venture_session_yield_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = venture_session_yield_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Session yield", NULL);)
