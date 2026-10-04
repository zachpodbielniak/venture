/*
 * venture-marketdata-records.c - Venues, instruments and watchlists
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Venues
 *
 * A place things are priced and traded: one realm's auction house, a
 * bookmaker, an exchange, a supplier's price list, your own shop. The
 * series store keeps a venue as a key and a group (a realm and its
 * region); a venue *record* is that venue promoted into the books, with
 * what the store cannot know: where money moves through when you trade
 * there (a location's holding, else an account), what it charges and what
 * it costs to move goods to it.
 *
 * `namespace` and `key` name the store's row, and `external-ref` is the
 * two joined -- "eu-realm:3678" -- derived at the save and unique in the
 * organization, deleted rows included, so promoting the same venue twice
 * finds the first record instead of making a second. A venue typed in by
 * hand with no key has no reference and no uniqueness to keep.
 *
 * The fee model is a name stored as text for now: the registry that
 * gives the names meaning arrives with the arbitrage engine, and its
 * parameters are YAML the model reads.
 * ========================================================================== */

static const VentureFieldDecl venture_venue_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Which venue: Argent Dawn auction house, Pinnacle, a supplier"),
	VENTURE_FIELD_ENUM("kind", "Kind",
	                   "marketplace, auction_house, bookmaker, exchange, supplier, store or other",
	                   venture_venue_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("namespace", "Namespace",
	              "Optional: tells this key from another source's same key -- eu-realm, bookmaker",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("key", "Key",
	              "Optional: the venue's key in its data source's store",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("group-key", "Group",
	              "Optional: the group it is compared within -- a region, a country",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("external-ref", "Reference",
	              "Namespace and key, joined; derived when saved and unique in the organization",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("data-source-id", "Data source",
	                  "Optional: the source whose store has this venue's prices",
	                  "data_source", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("location-id", "Location",
	                  "Optional: the location whose holding money moves through when you trade here",
	                  "location", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("account-id", "Account",
	                  "Optional: the account money moves through when the venue has no location",
	                  "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("currency", "Currency",
	              "Optional: what the venue prices in",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("fee-model", "Fee model",
	              "Optional: the name of what the venue charges -- a cut, a deposit, a commission",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("fee-params", "Fee parameters",
	              "Optional: the fee model's parameters, as YAML",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("transfer-cost", "Transfer cost",
	                    "Optional: what moving one lot to or from this venue costs"),
	VENTURE_FIELD("transfer-hours", "Transfer hours",
	              "Optional: how long moving goods or money here takes; 0 when nobody said",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureVenue, venture_venue, venture_venue_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Venue", NULL);)

/* ==========================================================================
 * Instruments
 *
 * A thing that is priced: an item, an outcome of an event, a share, a
 * supplier's SKU. Like a venue it may name its row in a data source's
 * store (`data-source-id` and `key`, joined with the namespace into the
 * unique `external-ref`), and it may name the `product` it is -- which is
 * what joins outside prices to recipes, stock and goals: the price oracle
 * answers for a product through the instruments that name it.
 *
 * `parent-id` nests instruments: an event over its outcomes. The tree is
 * held to the category rules -- no loop, one organization, bounded depth
 * -- by venture_category_check_tree_node(), the one definition of a loop.
 * ========================================================================== */

static const VentureFieldDecl venture_instrument_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What is priced: Copper Ore, Arsenal to win, SKU 4411"),
	VENTURE_FIELD_ENUM("kind", "Kind", "item, outcome, event, asset, sku or other",
	                   venture_instrument_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("namespace", "Namespace",
	              "Optional: tells this key from another source's same key -- wow-item, sku",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("key", "Key",
	              "Optional: the instrument's key in its data source's store",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("external-ref", "Reference",
	              "Namespace and key, joined; derived when saved and unique in the organization",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("data-source-id", "Data source",
	                  "Optional: the source whose store has this instrument's prices",
	                  "data_source", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("product-id", "Product",
	                  "Optional: the product it is, so recipes, stock and goals can be priced from it",
	                  "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("category-id", "Category", "Optional: where it is filed",
	                  "category", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("parent-id", "Part of",
	                  "Optional: what it belongs to -- an outcome's event",
	                  "instrument", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("attrs", "Attributes",
	              "Optional: what the source said about it, as YAML or JSON",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInstrument, venture_instrument, venture_instrument_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Instrument", NULL);)

/* ==========================================================================
 * Watchlists
 *
 * A named list of instruments somebody is keeping an eye on, optionally
 * in one venue group. Shared in the organization rather than owned by a
 * person: alerts read them, and a record with a personal owner is one the
 * webhooks and automation are kept quiet about.
 * ========================================================================== */

static const VentureFieldDecl venture_watchlist_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What the list is for: herbs to flip, Saturday's matches"),
	VENTURE_FIELD("group-key", "Group",
	              "Optional: the venue group its prices are read in -- a region",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture it is for",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureWatchlist, venture_watchlist, venture_watchlist_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Watchlist", NULL);)

/* ==========================================================================
 * Watchlist entries
 *
 * One instrument on a watchlist, with the prices that would make it worth
 * acting on: buy at or under one, sell at or over the other. Either may be
 * left empty; both are in whatever currency the venue prices in, and the
 * validator holds them to one currency and to nothing negative.
 * ========================================================================== */

static const VentureFieldDecl venture_watchlist_entry_fields[] = {
	VENTURE_FIELD_REF("watchlist-id", "Watchlist", "The list it is on",
	                  "watchlist", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("instrument-id", "Instrument", "What is watched",
	                  "instrument", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("target-buy", "Buy at",
	                    "Optional: worth buying at or below this"),
	VENTURE_FIELD_MONEY("target-sell", "Sell at",
	                    "Optional: worth selling at or above this"),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureWatchlistEntry, venture_watchlist_entry,
	venture_watchlist_entry_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Watchlist entry", NULL);)
