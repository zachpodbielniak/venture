/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */

/*
 * venture-stock-engine.c - the Libreventory store inside Venture's database
 *
 * One store per VentureDatabase, opened on the database's own connection
 * with the table prefix "stock_", so a stock movement and its journal are
 * one transaction: the engine works in a savepoint of Venture's. Each
 * Venture inventory item is one engine item, found by its barcode
 * "venture:inventory_item:<id>" -- a field on the Venture item would be a
 * derived save, and bump its version under whoever holds it.
 *
 * The engine allows any item below zero. Whether an item may go there,
 * and whether an issue may take a unit that carries no cost, are
 * Venture's rules: the service checks the first before it posts, and the
 * "check-move" handler here refuses the second for the items a post names
 * as strict.
 */
#include "goods/venture-stock-engine.h"

#include <string.h>

#define ENGINE_KEY "venture-stock-engine"
#define STOCK_PREFIX "stock_"
#define SECTION_NAME "Venture"
#define BARCODE_FORMAT "venture:inventory_item:%" G_GINT64_FORMAT

typedef struct
{
	LvRegistry	*registry;
	LvConfig	*config;
	LvStore		*store;
	gint64		 section_id;
	GHashTable	*strict;	/* during a post: engine items whose takes need a cost */
	VentureDatabase	*database;	/* not a reference: the engine is the database's data */
	gulong		 finished_id;	/* while opened inside a transaction not yet over */
} Engine;

static void
engine_free(gpointer data)
{
	Engine *engine = data;

	if (engine->finished_id != 0)
		g_signal_handler_disconnect(engine->database, engine->finished_id);
	g_clear_object(&engine->store);
	g_clear_object(&engine->config);
	g_clear_object(&engine->registry);
	g_free(engine);
}

static gboolean
from_engine(GError **error, GError *cause, const gchar *what)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s%s%s", what != NULL ? what : "",
		what != NULL ? ": " : "", cause != NULL ? cause->message : "the stock engine failed");
	return FALSE;
}

/*
 * A take line from stock that carries no cost -- a lot a count brought in,
 * or below zero -- is refused for an item the post named strict. Every
 * line taken from a costed lot carries its cost per currency, a layer at
 * zero included, so an empty cost means no layer.
 */
static gchar *
check_move(LvStore *store, LvMovement *movement, LvMove *move, gpointer user_data)
{
	Engine *engine = user_data;

	(void)store;
	(void)movement;
	if (engine->strict == NULL || move->quantity >= 0 || !g_hash_table_contains(engine->strict, &move->item_id))
		return NULL;
	if (move->lot_id != 0 && move->costs != NULL && json_object_get_size(move->costs) > 0)
		return NULL;
	return g_strdup("VentureInventoryService: negative stock is refused");
}

/* A store opened inside a transaction that rolls back has lost its tables
 * with it; the next use opens it again. */
static void
transaction_finished(VentureDatabase *database, gboolean committed, gpointer user_data)
{
	Engine *engine = user_data;

	g_signal_handler_disconnect(database, engine->finished_id);
	engine->finished_id = 0;
	if (!committed)
		g_object_set_data(G_OBJECT(database), ENGINE_KEY, NULL);
}

/* Venture's words for its movements, on the engine's bases. A write-off
 * is the engine's own base kind. */
static gboolean
add_kinds(LvRegistry *registry, GError **error)
{
	static const struct
	{
		const gchar *id;
		const gchar *label;
		const gchar *base;
	} kinds[] = {
		{ "purchase", "Purchase", LV_BASE_RECEIVE },
		{ "bought", "Bought outright", LV_BASE_RECEIVE },
		{ "opening", "Opening stock", LV_BASE_RECEIVE },
		{ "sale", "Sale", LV_BASE_ISSUE },
		{ "return", "Return to supplier", LV_BASE_ISSUE },
		{ "adjustment", "Count", LV_BASE_ADJUST },
		{ "production", "Production", LV_BASE_PRODUCE },
		{ "item-transfer", "Transfer between items", LV_BASE_PRODUCE },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
	{
		g_autoptr(LvMovementKind) kind = lv_movement_kind_new(kinds[i].id, kinds[i].label, kinds[i].base);

		if (!lv_registry_add_movement_kind(registry, kind, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
find_section(Engine *engine, GError **error)
{
	g_autoptr(GPtrArray) sections = lv_store_list(engine->store, LV_TYPE_SECTION, error);
	g_autoptr(LvSection) section = NULL;
	guint i;

	if (sections == NULL)
		return FALSE;
	for (i = 0; i < sections->len; i++)
	{
		LvSection *each = g_ptr_array_index(sections, i);

		if (g_strcmp0(each->name, SECTION_NAME) == 0)
		{
			engine->section_id = each->id;
			return TRUE;
		}
	}
	section = g_object_new(LV_TYPE_SECTION, "name", SECTION_NAME, "kind", "inventory", "item_kind", "general",
		NULL);
	if (!lv_store_save_section(engine->store, section, error))
		return FALSE;
	engine->section_id = section->id;
	return TRUE;
}

static Engine *
engine_get(VentureDatabase *database, GError **error)
{
	Engine *engine;
	g_autoptr(GError) cause = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	engine = g_object_get_data(G_OBJECT(database), ENGINE_KEY);
	if (engine != NULL)
		return engine;
	engine = g_new0(Engine, 1);
	engine->database = database;
	engine->registry = lv_registry_new();
	if (!add_kinds(engine->registry, &cause) ||
		(engine->config = lv_config_new(engine->registry, NULL, &cause)) == NULL ||
		(engine->store = lv_store_open_with_connection(engine->config, venture_database_get_connection(database),
			STOCK_PREFIX, &cause)) == NULL ||
		!find_section(engine, &cause))
	{
		engine_free(engine);
		from_engine(error, cause, "Cannot open the stock engine");
		return NULL;
	}
	g_signal_connect(engine->store, "check-move", G_CALLBACK(check_move), engine);
	if (venture_database_has_transaction(database))
		engine->finished_id = g_signal_connect(database, "transaction-finished",
			G_CALLBACK(transaction_finished), engine);
	g_object_set_data_full(G_OBJECT(database), ENGINE_KEY, engine, engine_free);
	return engine;
}

LvStore *
venture_stock_engine_get_store(VentureDatabase *database, GError **error)
{
	Engine *engine = engine_get(database, error);

	return engine != NULL ? engine->store : NULL;
}

gint64
venture_stock_engine_find_item(VentureDatabase *database, gint64 inventory_item_id, GError **error)
{
	g_autofree gchar *barcode = g_strdup_printf(BARCODE_FORMAT, inventory_item_id);
	g_autoptr(GError) cause = NULL;
	g_autoptr(LvItem) found = NULL;
	Engine *engine = engine_get(database, error);

	if (engine == NULL)
		return 0;
	found = lv_store_find_item_by_barcode(engine->store, barcode, &cause);
	if (cause != NULL)
	{
		from_engine(error, cause, "Cannot find the stock of an inventory item");
		return 0;
	}
	return found != NULL ? found->id : 0;
}

gint64
venture_stock_engine_ensure_item(VentureDatabase *database, VentureEntity *item, gboolean *created,
	GError **error)
{
	g_autofree gchar *barcode = NULL;
	g_autofree gchar *sku = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GError) cause = NULL;
	g_autoptr(LvItem) made = NULL;
	Engine *engine;
	gint64 id;

	if (created != NULL)
		*created = FALSE;
	id = venture_stock_engine_find_item(database, venture_entity_get_id(item), error);
	if (id != 0 || (error != NULL && *error != NULL))
		return id;
	engine = engine_get(database, error);
	if (engine == NULL)
		return 0;
	g_object_get(item, "sku", &sku, NULL);
	name = venture_string_is_empty(sku) ? g_strdup_printf("Inventory item %" G_GINT64_FORMAT,
		venture_entity_get_id(item)) : g_strdup(sku);
	barcode = g_strdup_printf(BARCODE_FORMAT, venture_entity_get_id(item));
	made = g_object_new(LV_TYPE_ITEM, "name", name, "unit", "each", "section_id", engine->section_id,
		"barcode", barcode, "allow_negative", TRUE, NULL);
	if (!lv_store_save_item(engine->store, made, &cause))
	{
		from_engine(error, cause, "Cannot give an inventory item its stock");
		return 0;
	}
	if (created != NULL)
		*created = TRUE;
	return made->id;
}

gboolean
venture_stock_engine_add_currency(VentureDatabase *database, const gchar *code, GError **error)
{
	g_autoptr(VentureMoney) zero = NULL;
	g_autoptr(LvCurrency) currency = NULL;
	g_autoptr(GError) cause = NULL;
	Engine *engine = engine_get(database, error);

	if (engine == NULL)
		return FALSE;
	if (venture_string_is_empty(code) || lv_registry_get_currency(engine->registry, code) != NULL)
		return TRUE;
	zero = venture_money_new_for_currency(0, code);
	currency = lv_currency_new(code, code, venture_money_get_exponent(zero));
	if (!lv_registry_add_currency(engine->registry, currency, &cause))
		return from_engine(error, cause, "Cannot cost stock in that currency");
	return TRUE;
}

LvMovement *
venture_stock_engine_post(VentureDatabase *database, LvMovementRequest *request, GHashTable *strict,
	LvCosts **costs, GError **error)
{
	g_autoptr(GError) cause = NULL;
	LvMovement *movement;
	Engine *engine = engine_get(database, error);

	if (engine == NULL)
		return NULL;
	engine->strict = strict;
	movement = lv_ledger_post(engine->store, request, costs, &cause);
	engine->strict = NULL;
	if (movement == NULL)
		from_engine(error, cause, NULL);
	return movement;
}

gchar *
venture_stock_engine_time(GDateTime *when)
{
	g_autoptr(GDateTime) now = when == NULL ? g_date_time_new_now_utc() : NULL;
	g_autoptr(GDateTime) utc = g_date_time_to_utc(when != NULL ? when : now);

	return g_date_time_format(utc, "%Y-%m-%dT%H:%M:%S.%fZ");
}

gboolean
venture_stock_engine_migrate(VentureDatabase *database, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) items = NULL;
	VentureInventoryService *inventory;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	/* Sales off: there are no inventory items, and nothing to open the
	 * store for. The first start with it on brings them in. */
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "inventory_item") ||
		!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "inventory_txn"))
		return TRUE;
	if (engine_get(database, error) == NULL)
		return FALSE;
	/* Deleted items too: their stock was counted in the valuation. */
	query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	items = venture_database_find(database, query, error);
	if (items == NULL)
		return FALSE;
	inventory = venture_inventory_service_get(database);
	for (i = 0; i < items->len; i++)
	{
		VentureEntity *item = g_ptr_array_index(items, i);
		gint64 found = venture_stock_engine_find_item(database, venture_entity_get_id(item), error);

		if (found != 0)
			continue;
		if (error != NULL && *error != NULL)
			return FALSE;
		if (!venture_database_begin(database, error))
			return FALSE;
		if (!venture_inventory_service_import_item(inventory, item, error))
		{
			venture_database_rollback(database);
			return FALSE;
		}
		if (!venture_database_commit(database, error))
			return FALSE;
	}
	return TRUE;
}

static gint64
row_int(gpointer row, const gchar *field)
{
	gint64 value = 0;

	g_object_get(row, field, &value, NULL);
	return value;
}

static gboolean
mismatch(GError **error, gint64 item_id, const gchar *what)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		"inventory item #%" G_GINT64_FORMAT ": the rows and the stock engine differ: %s", item_id, what);
	return FALSE;
}

gboolean
venture_stock_engine_check_mirror(VentureDatabase *database, gint64 inventory_item_id, GError **error)
{
	g_autoptr(VentureQuery) txn_query = venture_query_new(VENTURE_TYPE_INVENTORY_TXN);
	g_autoptr(VentureQuery) layer_query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	g_autoptr(GPtrArray) txns = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GHashTable) linked = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GError) cause = NULL;
	LvStore *store = venture_stock_engine_get_store(database, error);
	gint64 engine_item, quantity = 0;
	guint i, open = 0;

	if (store == NULL)
		return FALSE;
	engine_item = venture_stock_engine_find_item(database, inventory_item_id, error);
	if (engine_item == 0)
		return error == NULL || *error == NULL ? mismatch(error, inventory_item_id, "no engine item") : FALSE;
	venture_query_set_limit(txn_query, 0);
	venture_query_set_limit(layer_query, 0);
	if (!venture_query_add_filter_int(txn_query, "inventory-item-id", VENTURE_FILTER_OP_EQ, inventory_item_id,
			error) ||
		!venture_query_add_filter_int(layer_query, "inventory-item-id", VENTURE_FILTER_OP_EQ, inventory_item_id,
			error))
		return FALSE;
	txns = venture_database_find(database, txn_query, error);
	rows = txns != NULL ? venture_database_find(database, layer_query, error) : NULL;
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < txns->len; i++)
		quantity += row_int(g_ptr_array_index(txns, i), "quantity");
	if ((gdouble)quantity != lv_ledger_on_hand(store, engine_item, NULL, &cause) || cause != NULL)
		return mismatch(error, inventory_item_id, "the quantity on hand");
	for (i = 0; i < rows->len; i++)
	{
		gpointer row = g_ptr_array_index(rows, i);
		gint64 layer = row_int(row, "stock-layer-id");

		if (layer != 0)
			g_hash_table_insert(linked, g_memdup2(&layer, sizeof layer), row);
		else if (row_int(row, "remaining-qty") > 0)
			return mismatch(error, inventory_item_id, "an open layer row has no engine layer");
	}
	layers = lv_ledger_list_layers(store, engine_item, &cause);
	if (layers == NULL)
		return mismatch(error, inventory_item_id, cause != NULL ? cause->message : "the layers");
	for (i = 0; i < layers->len; i++)
	{
		LvLayer *layer = g_ptr_array_index(layers, i);
		gpointer row = g_hash_table_lookup(linked, &layer->id);
		g_autoptr(VentureMoney) unit = NULL;
		gint64 units = (gint64)layer->quantity;

		if (layer->remaining <= 0)
			continue;
		open++;
		if (row == NULL)
			return mismatch(error, inventory_item_id, "an engine layer has no row");
		g_object_get(row, "unit-cost", &unit, NULL);
		if (unit == NULL || row_int(row, "remaining-qty") != (gint64)layer->remaining ||
			g_strcmp0(venture_money_get_currency(unit), layer->currency) != 0 ||
			units <= 0 || venture_money_get_amount(unit) * units != layer->cost)
			return mismatch(error, inventory_item_id, "a layer's units or cost");
	}
	for (i = 0; i < rows->len; i++)
		if (row_int(g_ptr_array_index(rows, i), "remaining-qty") > 0)
			open = open > 0 ? open - 1 : G_MAXUINT;
	if (open != 0)
		return mismatch(error, inventory_item_id, "the number of open layers");
	return TRUE;
}
