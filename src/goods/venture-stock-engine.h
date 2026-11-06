/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_STOCK_ENGINE_H
#define VENTURE_STOCK_ENGINE_H

/*
 * Venture's stock runs on Libreventory's ledger (deps/libreventory,
 * AGPL-3.0-only), embedded in Venture's own database and transaction.
 * This header is not part of <venture.h> (the Makefile leaves it out of
 * the public headers): the engine's types stay out of the public API and
 * the GIR. The inventory service and the tests include it.
 * docs/inventory.org ("The stock engine") has the rules.
 */
#include <venture.h>
#include <libreventory-core.h>

G_BEGIN_DECLS

/*
 * The engine place a count typed in by hand keeps its units at. Every
 * other movement uses the item's own place (none). A count that finds
 * fewer takes from this place only, below zero if it must, so it never
 * takes a costed unit and never changes what the stock is worth: the
 * quantity and the cost are separate questions in Venture, as they were
 * before the engine.
 */
#define VENTURE_STOCK_COUNT_PLACE "count"

/**
 * venture_stock_engine_get_store:
 * @database: the database
 * @error: (out) (optional): return location for an error
 *
 * The engine's store in @database, tables prefixed "stock_", opened on
 * the database's own connection. venture_stock_engine_migrate() opens it
 * at startup; a database that skipped that opens it on first use.
 *
 * Returns: (transfer none) (nullable): the store, owned by @database
 */
LvStore *venture_stock_engine_get_store(VentureDatabase *database, GError **error);

/**
 * venture_stock_engine_find_item:
 * @database: the database
 * @inventory_item_id: a Venture inventory item
 * @error: (out) (optional): return location for an error
 *
 * Returns: the engine's item for @inventory_item_id, or 0 when it has none
 *   yet
 */
gint64 venture_stock_engine_find_item(VentureDatabase *database, gint64 inventory_item_id, GError **error);

/**
 * venture_stock_engine_ensure_item:
 * @database: the database
 * @item: a saved Venture inventory item
 * @created: (out) (optional): %TRUE when the engine item is new, and so
 *   holds none of the item's stock yet
 * @error: (out) (optional): return location for an error
 *
 * Returns: the engine's item for @item, made when it has none; 0 on error
 */
gint64 venture_stock_engine_ensure_item(VentureDatabase *database, VentureEntity *item, gboolean *created,
	GError **error);

/**
 * venture_stock_engine_add_currency:
 * @database: the database
 * @code: a currency code Venture knows
 * @error: (out) (optional): return location for an error
 *
 * Tells the engine about @code, with its exponent, before a cost in it is
 * posted. Knowing it already is no error.
 *
 * Returns: %TRUE on success
 */
gboolean venture_stock_engine_add_currency(VentureDatabase *database, const gchar *code, GError **error);

/**
 * venture_stock_engine_post:
 * @database: the database, inside a transaction the caller owns
 * @request: the movement
 * @strict: (nullable) (element-type gint64 gint64): engine items whose
 *   take lines must carry a cost: stock with no cost layer, or below zero,
 *   is refused for them, as an issue always refused it
 * @costs: (out) (optional) (transfer full): what the movement moved
 * @error: (out) (optional): return location for an error
 *
 * Posts @request in a savepoint of the caller's transaction.
 *
 * Returns: (transfer full) (nullable): the movement
 */
LvMovement *venture_stock_engine_post(VentureDatabase *database, LvMovementRequest *request, GHashTable *strict,
	LvCosts **costs, GError **error);

/**
 * venture_stock_engine_time:
 * @when: (nullable): a time; %NULL is now
 *
 * Returns: (transfer full): @when as the engine dates a movement, in UTC
 */
gchar *venture_stock_engine_time(GDateTime *when);

/**
 * venture_stock_engine_migrate:
 * @database: a migrated database
 * @error: (out) (optional): return location for an error
 *
 * Opens the store and brings every inventory item that has no engine item
 * into the engine (venture_inventory_service_import_item()). A second run
 * finds every item and does nothing. Startup calls it after the scripts
 * and the seeds. With the inventory types switched off it does nothing.
 *
 * Returns: %TRUE on success
 */
gboolean venture_stock_engine_migrate(VentureDatabase *database, GError **error);

/**
 * venture_stock_engine_check_mirror:
 * @database: the database
 * @inventory_item_id: a Venture inventory item
 * @error: (out) (optional): set to say what differs
 *
 * Compares the item's inventory_txn and inventory_cost_layer rows with the
 * engine: the quantity on hand, and each open layer's units and cost.
 *
 * Returns: %TRUE when they agree
 */
gboolean venture_stock_engine_check_mirror(VentureDatabase *database, gint64 inventory_item_id, GError **error);

/**
 * venture_inventory_service_import_item:
 * @self: the inventory service
 * @item: an inventory item with no engine item
 * @error: (out) (optional): return location for an error
 *
 * Gives @item an engine item holding what its rows hold: one "opening"
 * movement per open lot, dated when the lot came in, with what its layers
 * still hold per currency, and a count for the units no layer covers.
 * Each open layer row is linked to its engine layer. Joins the caller's
 * transaction.
 *
 * Returns: %TRUE on success
 */
gboolean venture_inventory_service_import_item(VentureInventoryService *self, VentureEntity *item, GError **error);

G_END_DECLS

#endif
