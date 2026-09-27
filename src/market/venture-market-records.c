/*
 * venture-market-records.c - Price observations and listings
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Price observations
 *
 * A price somebody saw: the market value an auction house's tooling
 * quoted, a regional average, what a supplier asked on Tuesday. It is not
 * a sale and moves no money; it is evidence of what a thing is worth, from
 * a named source, at a moment. Reports that value stock or yields ask
 * venture_market_latest_price() for the newest one at or before the date
 * they value at, so a history of observations is also a history of
 * valuations. The source is free text on purpose: the set of sources is
 * whatever the operator's market has.
 * ========================================================================== */

static const VentureFieldDecl venture_price_observation_fields[] = {
	VENTURE_FIELD_REF("product-id", "Product", "What was priced", "product",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("source", "Source",
	              "Who or what quoted it: market value, region average, a supplier",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("price", "Price", "One unit's price, in the currency it was quoted in",
	              VENTURE_FIELD_KIND_MONEY, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("volume", "Volume",
	              "Optional: how many were on offer or sold when it was seen",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("observed-at", "Observed", "When the price was seen",
	              VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("location-id", "Location",
	                  "Optional: the market or region it was seen in",
	                  "location", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

/* "market value: 12.50 USD" -- the product's name would need the
 * database, which a display name does not have. */
static gchar *
venture_price_observation_display_name(VentureEntity *self)
{
	g_autofree gchar *source = NULL;
	g_autofree gchar *amount = NULL;
	g_autoptr(VentureMoney) price = NULL;

	g_object_get(self, "source", &source, "price", &price, NULL);

	if (NULL != price)
		amount = venture_money_to_display_string(price, TRUE);

	return g_strdup_printf("%s: %s",
	                       venture_string_is_empty(source) ? "Observation" : source,
	                       (NULL != amount) ? amount : "no price");
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePriceObservation, venture_price_observation,
	venture_price_observation_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = venture_price_observation_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Price observation", NULL);)

/* ==========================================================================
 * Listings
 *
 * An offer to sell that ends: an auction, a marketplace posting, a lot on
 * a consignment shelf. It opens with a quantity at a unit price and ends
 * sold, partly sold, expired or cancelled -- and how often listings end
 * sold is the sale rate, which no sale record can tell you, because the
 * offers that did not sell left no sale behind.
 *
 * The deposit is what listing cost up front and is lost if it does not
 * sell (an auction house's deposit, a stall fee); the fees are what the
 * channel took. All three amounts share one currency: the save validator
 * in venture-market.c refuses a mix rather than guessing a rate.
 * ========================================================================== */

static const VentureFieldDecl venture_listing_fields[] = {
	VENTURE_FIELD_REF("product-id", "Product", "What is on offer", "product",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("inventory-item-id", "Stock",
	                  "Optional: the stock the units come from",
	                  "inventory_item", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("channel", "Channel",
	              "Where it is listed: an auction house, a marketplace, a shop",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("quantity", "Quantity", "Units offered; at least one",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("quantity-sold", "Sold",
	              "Units sold so far; filled to the quantity when marked sold",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("unit-price", "Unit price", "Asking price for one unit",
	              VENTURE_FIELD_KIND_MONEY, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("deposit", "Deposit",
	                    "Optional: paid to list, and lost if it does not sell"),
	VENTURE_FIELD_MONEY("fees", "Fees", "Optional: what the channel took"),
	VENTURE_FIELD("listed-at", "Listed", "When the offer opened",
	              VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("closed-at", "Closed",
	              "When it ended; filled when the outcome is set, if blank",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("outcome", "Outcome", "Open until it ends",
	                   venture_listing_outcome_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("sale-id", "Sale", "Optional: the sale it became",
	                  "sale", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

/* "auction house: 5 x 12.50 USD". */
static gchar *
venture_listing_display_name(VentureEntity *self)
{
	g_autofree gchar *channel = NULL;
	g_autofree gchar *amount = NULL;
	g_autoptr(VentureMoney) price = NULL;
	gint64 quantity;

	g_object_get(self, "channel", &channel, "unit-price", &price,
	             "quantity", &quantity, NULL);

	if (NULL != price)
		amount = venture_money_to_display_string(price, TRUE);

	return g_strdup_printf("%s: %" G_GINT64_FORMAT " x %s",
	                       venture_string_is_empty(channel) ? "Listing" : channel,
	                       quantity, (NULL != amount) ? amount : "no price");
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureListing, venture_listing, venture_listing_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = venture_listing_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Listing", NULL);)
