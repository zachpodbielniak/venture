/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "goods/venture-stock-engine.h"
#include <string.h>

struct _VentureInventoryService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureInventoryService, venture_inventory_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureInventoryService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_INVENTORY_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureInventoryService *self = VENTURE_INVENTORY_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureInventoryService *self = VENTURE_INVENTORY_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_inventory_service_parent_class)->finalize(object);
}

static void
venture_inventory_service_class_init(VentureInventoryServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_inventory_service_init(VentureInventoryService *self)
{
	(void)self;
}

VentureInventoryService *
venture_inventory_service_get(VentureDatabase *database)
{
	VentureInventoryService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-inventory-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_INVENTORY_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-inventory-service", self, g_object_unref);
	}
	return self;
}

static gint64
get_id(VentureEntity *record, const gchar *field)
{
	gint64 id = 0;
	g_object_get(record, field, &id, NULL);
	return id;
}

static gint64
get_int(VentureEntity *record, const gchar *field)
{
	gint64 value = 0;
	g_object_get(record, field, &value, NULL);
	return value;
}

static gboolean
save_owned(VentureInventoryService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = previous;
	return ok;
}

static GPtrArray *
find_rows(VentureInventoryService *self, GType type, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	if (id != 0 && field != NULL &&
		!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}

static gint64
account_code(VentureInventoryService *self, gint64 org, const gchar *role, const gchar *code,
	const gchar *name, VentureAccountKind kind, GError **error)
{
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *scoped = NULL;
	gint64 id;
	id = venture_setup_resolve_account(self->database, org, role, "organization", 0, NULL, &local);
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return 0;
	}
	if (id != 0)
		return id;
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(self->database, query, error);
	if (row != NULL)
		return venture_entity_get_id(row);
	if (error != NULL && *error != NULL)
		return 0;
	scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, code);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped, NULL);
	row = venture_database_find_one(self->database, query, error);
	if (row != NULL)
		return venture_entity_get_id(row);
	if (error != NULL && *error != NULL)
		return 0;
	row = VENTURE_ENTITY(venture_account_new());
	g_object_set(row, "organization-id", org, "code", scoped, "name", name, "kind", kind, "active", TRUE, NULL);
	if (!venture_database_save(self->database, row, NULL, error))
		return 0;
	return venture_entity_get_id(row);
}

/*
 * Posts one debit/credit pair per currency in @amounts, all in one call to
 * the posting service, so the currency rule decides each: a book-currency
 * or valued-with-a-rate cost lands in the book journal, one kept apart
 * posts a pair of its own, and a memo cost -- tracked on its layer --
 * posts nothing. Each pair balances in its own currency, so an issue whose
 * layers cost GOLD and TICKET needs no clearing. Zero amounts are skipped:
 * a zero-cost layer consumed beside a costed one is not a posting.
 */
static gboolean
post_costs(VentureInventoryService *self, gint64 org, VentureEntity *source, gint64 debit_id,
	gint64 credit_id, GPtrArray *amounts, const gchar *memo, GDateTime *when,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) posted = NULL;
	const gchar *currency = NULL;
	guint i;
	for (i = 0; amounts != NULL && i < amounts->len; i++)
	{
		const VentureMoney *amount = g_ptr_array_index(amounts, i);
		VentureJournalLine *debit;
		VentureJournalLine *credit;
		if (venture_money_is_zero(amount))
			continue;
		if (currency == NULL)
			currency = venture_money_get_currency(amount);
		debit = venture_journal_line_new();
		credit = venture_journal_line_new();
		g_object_set(debit, "account-id", debit_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", amount,
			"organization-id", org, NULL);
		g_object_set(credit, "account-id", credit_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", amount,
			"organization-id", org, NULL);
		g_ptr_array_add(lines, debit);
		g_ptr_array_add(lines, credit);
	}
	if (lines->len == 0)
		return TRUE;
	g_object_set(journal, "source-type", venture_entity_get_entity_name(source),
		"source-id", venture_entity_get_id(source), "occurred-at", when,
		"currency", currency, "organization-id", org, "memo", memo, NULL);
	posted = venture_posting_service_post_by_currency(venture_database_get_posting_service(self->database),
		journal, lines, actor, error);
	return posted != NULL;
}

/* One pair in one currency: a receipt or a return, whose cost was given. */
static gboolean
post_pair(VentureInventoryService *self, gint64 org, VentureEntity *source, gint64 debit_id,
	gint64 credit_id, const VentureMoney *amount, const gchar *memo, GDateTime *when,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) amounts = venture_money_totals_new();
	if (!venture_money_totals_add(amounts, amount, error))
		return FALSE;
	return post_costs(self, org, source, debit_id, credit_id, amounts, memo, when, actor, error);
}

/*
 * What a movement's transaction shows as its unit cost, for reading only:
 * the average when everything it moved cost one currency. A movement whose
 * cost is in several currencies has no one unit cost, so the field stays
 * empty and @out_notes says what each currency came to; the cost layers
 * are what the books count either way.
 */
static gboolean
movement_unit(GPtrArray *costs, gint64 quantity, VentureMoney **out_unit, gchar **out_notes,
	GError **error)
{
	*out_unit = NULL;
	*out_notes = NULL;
	if (costs == NULL || costs->len == 0 || quantity <= 0)
		return TRUE;
	if (costs->len == 1)
	{
		*out_unit = venture_money_multiply_rational(g_ptr_array_index(costs, 0), 1, quantity, error);
		return *out_unit != NULL;
	}
	{
		g_autoptr(GString) notes = g_string_new("Cost ");
		guint i;
		for (i = 0; i < costs->len; i++)
		{
			g_autofree gchar *text = venture_money_to_string(g_ptr_array_index(costs, i));
			g_string_append_printf(notes, "%s%s", i > 0 ? " + " : "", text);
		}
		g_string_append_printf(notes, " for %" G_GINT64_FORMAT " units", quantity);
		*out_notes = g_string_free(g_steal_pointer(&notes), FALSE);
	}
	return TRUE;
}

/* --- The stock engine ---------------------------------------------------- */

/*
 * Quantities, lots, cost layers and what each movement cost are the
 * stock engine's (src/goods/venture-stock-engine.c, deps/libreventory).
 * Each operation below builds one engine movement, posts it in a savepoint
 * of the database's transaction, and then writes what it did as rows:
 * inventory_txn for the movement, inventory_cost_layer for the layers it
 * made or used. Every reader of those rows -- the pages, the reports, the
 * cutover, the arbitrage -- reads what the engine says, and the journal is
 * posted from the costs the engine gave back, in the same transaction.
 */

static gboolean
item_allows_negative(VentureEntity *item)
{
	gboolean allow = FALSE;

	if (item == NULL || g_object_class_find_property(G_OBJECT_GET_CLASS(item), "allow-negative") == NULL)
		return FALSE;
	g_object_get(item, "allow-negative", &allow, NULL);
	return allow;
}

/* Whether this install keeps cost layers: they belong to the goods
 * module. Without it there are no layer rows, and a craft moves
 * quantities only, as every other movement then does. */
static gboolean
layers_kept(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"inventory_cost_layer") != G_TYPE_INVALID;
}

static VentureEntity *
get_item(VentureInventoryService *self, gint64 item_id, GError **error)
{
	VentureEntity *item = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, item_id, error);

	/* A missing row is NULL with no error; say which. */
	if (item == NULL && error != NULL && *error == NULL)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"VentureInventoryService: inventory item #%" G_GINT64_FORMAT " does not exist", item_id);
	return item;
}

/* The engine's item for @item, bringing in what its rows hold the first
 * time the engine meets it (an item from before the engine, or one a
 * restore wrote back). */
static gint64
engine_item_for(VentureInventoryService *self, VentureEntity *item, GError **error)
{
	gint64 id = venture_stock_engine_find_item(self->database, venture_entity_get_id(item), error);

	if (id != 0 || (error != NULL && *error != NULL))
		return id;
	if (!venture_inventory_service_import_item(self, item, error))
		return 0;
	return venture_stock_engine_find_item(self->database, venture_entity_get_id(item), error);
}

static LvStore *
store_of(VentureInventoryService *self, GError **error)
{
	return venture_stock_engine_get_store(self->database, error);
}

/* Venture's rule, not the engine's: the stock on hand goes below zero only
 * on an item that allows it. The engine allows every item, so a count can
 * take its own place below zero while the item as a whole stays above. */
static gboolean
guard_stock(VentureInventoryService *self, VentureEntity *item, gint64 engine_item, gint64 delta, GError **error)
{
	g_autoptr(GError) cause = NULL;
	gdouble on_hand;

	if (delta >= 0 || item_allows_negative(item))
		return TRUE;
	on_hand = lv_ledger_on_hand(store_of(self, NULL), engine_item, NULL, &cause);
	if (cause != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&cause));
		return FALSE;
	}
	if (on_hand + (gdouble)delta < 0)
		return refuse(error, "negative stock is refused");
	return TRUE;
}

/* The items whose takes need a cost: an issue never sold a unit that had
 * no cost layer, unless the item allows negative stock. */
static GHashTable *
strict_for(VentureEntity *item, gint64 engine_item)
{
	GHashTable *strict = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	if (!item_allows_negative(item))
		g_hash_table_add(strict, g_memdup2(&engine_item, sizeof engine_item));
	return strict;
}

static gboolean
add_currencies(VentureInventoryService *self, GPtrArray *amounts, GError **error)
{
	guint i;

	for (i = 0; amounts != NULL && i < amounts->len; i++)
		if (!venture_stock_engine_add_currency(self->database,
			venture_money_get_currency(g_ptr_array_index(amounts, i)), error))
			return FALSE;
	return TRUE;
}

static LvCosts *
engine_costs(GPtrArray *amounts)
{
	LvCosts *costs = lv_costs_new();
	guint i;

	for (i = 0; amounts != NULL && i < amounts->len; i++)
	{
		const VentureMoney *amount = g_ptr_array_index(amounts, i);

		lv_costs_add(costs, venture_money_get_currency(amount), venture_money_get_amount(amount));
	}
	return costs;
}

/* The engine's costs as Venture's totals, one per currency, in order. */
static GPtrArray *
venture_costs(LvCosts *costs, GError **error)
{
	g_autoptr(GPtrArray) totals = venture_money_totals_new();
	guint i;

	for (i = 0; costs != NULL && i < lv_costs_get_n(costs); i++)
	{
		g_autoptr(VentureMoney) amount = venture_money_new_for_currency(lv_costs_get_amount(costs, i),
			lv_costs_get_currency(costs, i));

		if (!venture_money_totals_add(totals, amount, error))
			return NULL;
	}
	return g_steal_pointer(&totals);
}

static GPtrArray *
move_costs(LvMove *move, GError **error)
{
	g_autoptr(LvCosts) costs = lv_costs_new_from_json(move->costs);

	return venture_costs(costs, error);
}

static GPtrArray *
moves_of(VentureInventoryService *self, LvMovement *movement, GError **error)
{
	g_autoptr(GError) cause = NULL;
	GPtrArray *moves = lv_ledger_list_moves(store_of(self, NULL), movement->id, 0, &cause);

	if (moves == NULL)
		g_propagate_error(error, g_steal_pointer(&cause));
	return moves;
}

/* The lot the movement's line into @engine_item made. */
static gint64
lot_made(GPtrArray *moves, gint64 engine_item)
{
	guint i;

	for (i = 0; i < moves->len; i++)
	{
		LvMove *move = g_ptr_array_index(moves, i);

		if (move->item_id == engine_item && move->quantity > 0)
			return move->lot_id;
	}
	return 0;
}

static GHashTable *
lots_new(void)
{
	return g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
}

static void
lots_add(GHashTable *lots, gint64 lot, gint64 txn)
{
	if (lot != 0)
		g_hash_table_insert(lots, g_memdup2(&lot, sizeof lot), g_memdup2(&txn, sizeof txn));
}

static GDateTime *
lot_time(VentureInventoryService *self, gint64 lot_id)
{
	g_autoptr(LvLot) lot = lv_store_get(store_of(self, NULL), LV_TYPE_LOT, lot_id, NULL);
	g_autofree gchar *text = NULL;

	if (lot == NULL || lot->purchased == NULL)
		return venture_time_now();
	text = strlen(lot->purchased) == 10 ? g_strconcat(lot->purchased, "T00:00:00Z", NULL) : g_strdup(lot->purchased);
	return g_date_time_new_from_iso8601(text, NULL);
}

static GPtrArray *
layer_rows(VentureInventoryService *self, gint64 item_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);

	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "inventory-item-id", VENTURE_FILTER_OP_EQ, item_id, error))
		return NULL;
	venture_query_add_order(query, "received-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}

/*
 * Writes the engine's layers of @item as rows: a layer the movement just
 * made becomes a row in the lot of the txn @lots names for its engine lot
 * (with @receipt_line_id); a row whose layer the movement used gets the
 * units the engine says are left. One row per engine layer, in the order
 * the engine made them, so the rows read as they always did.
 */
static gboolean
sync_layers(VentureInventoryService *self, VentureEntity *item, gint64 engine_item, GHashTable *lots,
	gint64 receipt_line_id, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) linked = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GError) cause = NULL;
	guint i;

	if (!layers_kept())
		return TRUE;
	layers = lv_ledger_list_layers(store_of(self, NULL), engine_item, &cause);
	if (layers == NULL)
	{
		g_propagate_error(error, g_steal_pointer(&cause));
		return FALSE;
	}
	rows = layer_rows(self, venture_entity_get_id(item), error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		gint64 layer = get_int(g_ptr_array_index(rows, i), "stock-layer-id");

		if (layer != 0)
			g_hash_table_insert(linked, g_memdup2(&layer, sizeof layer), g_ptr_array_index(rows, i));
	}
	for (i = 0; i < layers->len; i++)
	{
		LvLayer *layer = g_ptr_array_index(layers, i);
		VentureEntity *row = g_hash_table_lookup(linked, &layer->id);
		gint64 remaining = (gint64)layer->remaining;
		gint64 *txn;

		if (row != NULL)
		{
			if (get_int(row, "remaining-qty") == remaining)
				continue;
			g_object_set(row, "remaining-qty", remaining, NULL);
			if (!save_owned(self, row, actor, error))
				return FALSE;
			continue;
		}
		txn = lots != NULL ? g_hash_table_lookup(lots, &layer->lot_id) : NULL;
		if (txn == NULL)
			continue;
		{
			g_autoptr(VentureInventoryCostLayer) made = venture_inventory_cost_layer_new();
			g_autoptr(VentureMoney) unit = NULL;
			g_autoptr(GDateTime) when = lot_time(self, layer->lot_id);
			gint64 units = (gint64)layer->quantity;

			/* The engine splits a cost exactly, so every layer's units
			 * share one whole unit cost. */
			unit = venture_money_new_for_currency(units > 0 ? layer->cost / units : 0, layer->currency);
			g_object_set(made, "inventory-item-id", venture_entity_get_id(item),
				"goods-receipt-line-id", receipt_line_id, "received-at", when, "original-qty", units,
				"remaining-qty", remaining, "unit-cost", unit, "lot-txn-id", *txn,
				"stock-layer-id", layer->id, NULL);
			venture_entity_set_organization_id(VENTURE_ENTITY(made), venture_entity_get_organization_id(item));
			if (!save_owned(self, VENTURE_ENTITY(made), actor, error))
				return FALSE;
		}
	}
	return TRUE;
}

static VentureEntity *
write_txn(VentureInventoryService *self, VentureEntity *item, gint64 quantity, gint kind,
	const VentureMoney *unit_cost, const gchar *notes, GDateTime *date, const gchar *reference,
	gint64 sale_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInventoryTxn) txn = venture_inventory_txn_new();

	g_object_set(txn, "inventory-item-id", venture_entity_get_id(item), "kind", kind, "quantity", quantity,
		"unit-cost", unit_cost, "occurred-at", date, "reference", reference, "sale-id", sale_id, NULL);
	if (notes != NULL)
		g_object_set(txn, "notes", notes, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(txn), venture_entity_get_organization_id(item));
	if (!save_owned(self, VENTURE_ENTITY(txn), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&txn));
}

/*
 * The currency a lot with no cost at all is carried in: the item's own
 * unit cost's, else its organization's book currency. It still needs a
 * layer -- an issue refuses units that have none -- and a zero never
 * clashes with anything, because layers are totalled per currency.
 */
static gchar *
zero_cost_currency(VentureInventoryService *self, VentureEntity *item)
{
	g_autoptr(VentureMoney) unit = NULL;
	gchar *currency = NULL;
	g_object_get(item, "unit-cost", &unit, NULL);
	if (unit != NULL)
		return g_strdup(venture_money_get_currency(unit));
	currency = venture_posting_service_book_currency(venture_database_get_posting_service(self->database),
		venture_entity_get_organization_id(item), NULL);
	if (venture_string_is_empty(currency))
	{
		g_free(currency);
		currency = g_strdup(venture_money_get_default_currency());
	}
	return currency;
}

/* --- Reading the stock --------------------------------------------------- */

/* What @item_id's movement rows add up to, before @as_of when given: the
 * history, and an item the engine has not met yet. */
static gint64
rows_on_hand(VentureInventoryService *self, gint64 item_id, GDateTime *as_of, GError **error)
{
	g_autoptr(GPtrArray) rows = find_rows(self, VENTURE_TYPE_INVENTORY_TXN, "inventory-item-id", item_id, error);
	gint64 total = 0;
	guint i;

	if (rows == NULL)
		return 0;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(GDateTime) when = NULL;

		g_object_get(g_ptr_array_index(rows, i), "occurred-at", &when, NULL);
		if (as_of != NULL && when != NULL && g_date_time_compare(when, as_of) >= 0)
			continue;
		total += get_int(g_ptr_array_index(rows, i), "quantity");
	}
	return total;
}

/*
 * What layer rows still hold, per currency, counting only layers received
 * at or before @as_of; @units (optional) gets how many units carry each.
 */
static gboolean
layers_value(GPtrArray *layers, GDateTime *as_of, GPtrArray *values, GHashTable *units, GError **error)
{
	guint i;
	for (i = 0; i < layers->len; i++)
	{
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureMoney) unit = NULL;
		g_autoptr(VentureMoney) slice = NULL;
		gint64 remaining = 0;
		g_object_get(g_ptr_array_index(layers, i), "received-at", &when, "unit-cost", &unit,
			"remaining-qty", &remaining, NULL);
		if (as_of != NULL && when != NULL && g_date_time_compare(when, as_of) > 0)
			continue;
		if (remaining <= 0 || unit == NULL)
			continue;
		slice = venture_money_multiply_int(unit, remaining, error);
		if (slice == NULL || !venture_money_totals_add(values, slice, error))
			return FALSE;
		if (units != NULL)
		{
			gint64 *count = g_hash_table_lookup(units, venture_money_get_currency(unit));
			if (count == NULL)
			{
				count = g_new0(gint64, 1);
				g_hash_table_insert(units, g_strdup(venture_money_get_currency(unit)), count);
			}
			*count += remaining;
		}
	}
	return TRUE;
}

/*
 * What one item's stock is worth, per currency, into @values; @units
 * (optional) gets how many units carry each currency. From the engine,
 * counting the lots that came in at or before @as_of; an item the engine
 * has not met is read from its rows, which hold the same.
 */
static gboolean
item_worth(VentureInventoryService *self, gint64 item_id, GDateTime *as_of, GPtrArray *values,
	GHashTable *units, GError **error)
{
	g_autoptr(GError) cause = NULL;
	g_autoptr(LvCosts) costs = NULL;
	gint64 engine_item = venture_stock_engine_find_item(self->database, item_id, error);
	guint i;

	if (engine_item == 0)
	{
		g_autoptr(GPtrArray) rows = NULL;

		if (error != NULL && *error != NULL)
			return FALSE;
		rows = layer_rows(self, item_id, error);
		return rows != NULL && layers_value(rows, as_of, values, units, error);
	}
	if (as_of != NULL)
	{
		g_autofree gchar *time = venture_stock_engine_time(as_of);

		costs = lv_ledger_valuation_through(store_of(self, NULL), engine_item, NULL, time, &cause);
	}
	else
		costs = lv_ledger_valuation(store_of(self, NULL), engine_item, NULL, &cause);
	if (costs == NULL)
	{
		g_propagate_error(error, g_steal_pointer(&cause));
		return FALSE;
	}
	for (i = 0; i < lv_costs_get_n(costs); i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		gint64 *count;

		/* The engine answers an empty item with one zero and no units:
		 * nothing is on hand, which is no amount at all here. */
		if (lv_costs_get_units(costs, i) <= 0)
			continue;
		amount = venture_money_new_for_currency(lv_costs_get_amount(costs, i), lv_costs_get_currency(costs, i));
		if (!venture_money_totals_add(values, amount, error))
			return FALSE;
		if (units == NULL)
			continue;
		count = g_hash_table_lookup(units, lv_costs_get_currency(costs, i));
		if (count == NULL)
		{
			count = g_new0(gint64, 1);
			g_hash_table_insert(units, g_strdup(lv_costs_get_currency(costs, i)), count);
		}
		*count += (gint64)lv_costs_get_units(costs, i);
	}
	return TRUE;
}

gint64
venture_inventory_service_on_hand(VentureInventoryService *self, gint64 inventory_item_id,
	GDateTime *as_of, GError **error)
{
	g_autoptr(GError) cause = NULL;
	gint64 engine_item;
	gdouble units;

	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), 0);
	/* The history before a time is the movements' own rows: the engine
	 * holds an older item's stock as it stood when it came in, not the
	 * movements before that. */
	if (as_of != NULL)
		return rows_on_hand(self, inventory_item_id, as_of, error);
	engine_item = venture_stock_engine_find_item(self->database, inventory_item_id, error);
	if (engine_item == 0)
		return (error != NULL && *error != NULL) ? 0 : rows_on_hand(self, inventory_item_id, NULL, error);
	units = lv_ledger_on_hand(store_of(self, NULL), engine_item, NULL, &cause);
	if (cause != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&cause));
		return 0;
	}
	return (gint64)units;
}

GPtrArray *
venture_inventory_service_valuation(VentureInventoryService *self, gint64 organization_id,
	GDateTime *as_of, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GPtrArray) values = venture_money_totals_new();
	g_autofree gchar *book = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), NULL);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	/* A deleted item's stock is still stock: its layers always counted. */
	venture_query_set_include_deleted(query, TRUE);
	items = venture_database_find(self->database, query, error);
	if (items == NULL)
		return NULL;
	/* Per currency: layers in GOLD and TICKET -- in one product or in two
	 * -- are two figures, never a refusal of the whole organization. */
	if (layers_kept())
		for (i = 0; i < items->len; i++)
			if (!item_worth(self, venture_entity_get_id(g_ptr_array_index(items, i)), as_of, values, NULL, error))
				return NULL;
	book = venture_posting_service_book_currency(venture_database_get_posting_service(self->database),
		organization_id, NULL);
	if (venture_string_is_empty(book))
	{
		g_free(book);
		book = g_strdup(venture_money_get_default_currency());
	}
	if (values->len == 0)
	{
		g_autoptr(VentureMoney) zero = venture_money_new_zero(book);
		g_ptr_array_add(values, g_steal_pointer(&zero));
	}
	venture_money_totals_sort(values, book);
	return g_steal_pointer(&values);
}

GPtrArray *
venture_inventory_service_item_value(VentureInventoryService *self, gint64 inventory_item_id,
	GDateTime *as_of, GPtrArray **out_unit_costs, GError **error)
{
	g_autoptr(GPtrArray) values = venture_money_totals_new();
	g_autoptr(GHashTable) units = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GPtrArray) unit_costs = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autofree gchar *book = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), NULL);
	if (out_unit_costs != NULL)
		*out_unit_costs = NULL;
	/* Without the goods module there are no layers to read, and no
	 * carrying cost but the one typed on the item. */
	if (!layers_kept())
	{
		if (out_unit_costs != NULL)
			*out_unit_costs = venture_money_totals_new();
		return g_steal_pointer(&values);
	}
	item = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (item == NULL)
		return NULL;
	if (!item_worth(self, inventory_item_id, as_of, values, units, error))
		return NULL;
	book = venture_posting_service_book_currency(venture_database_get_posting_service(self->database),
		venture_entity_get_organization_id(item), NULL);
	venture_money_totals_sort(values, book);
	unit_costs = venture_money_totals_new();
	for (i = 0; i < values->len; i++)
	{
		const VentureMoney *value = g_ptr_array_index(values, i);
		gint64 *count = g_hash_table_lookup(units, venture_money_get_currency(value));
		VentureMoney *average = venture_money_multiply_rational(value, 1,
			count != NULL && *count > 0 ? *count : 1, error);
		if (average == NULL)
			return NULL;
		g_ptr_array_add(unit_costs, average);
	}
	if (out_unit_costs != NULL)
		*out_unit_costs = g_steal_pointer(&unit_costs);
	return g_steal_pointer(&values);
}

/* --- Bringing an item into the engine ------------------------------------ */

/* The lot a layer row belongs to: its arrival movement, or itself (negated,
 * so it cannot meet a movement id) when it predates lots. */
static gint64
layer_lot(VentureEntity *layer)
{
	gint64 lot = get_id(layer, "lot-txn-id");
	return lot > 0 ? lot : -venture_entity_get_id(layer);
}

typedef struct
{
	gint64		 key;		/* layer_lot() */
	GPtrArray	*rows;		/* its open layer rows, in order */
} OpenLot;

static void
open_lot_free(gpointer data)
{
	OpenLot *lot = data;

	g_ptr_array_unref(lot->rows);
	g_free(lot);
}

static gint64
floor_div(gint64 total, gint64 units)
{
	return total >= 0 ? total / units : -((-total + units - 1) / units);
}

/*
 * Whether the engine's exact split of @rows' cost, per currency, over the
 * lot's @units gives @rows back: every row of a currency at the floor of
 * the share or one minor unit above it, and the units above it exactly
 * the remainder -- as every lot the service made was written. Then the lot
 * goes in whole, its sibling currencies sharing it.
 */
static gboolean
lot_splits_exactly(GPtrArray *rows, gint64 units)
{
	g_autoptr(GHashTable) totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GHashTable) counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GHashTable) dear = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GHashTableIter iter;
	gpointer key, value;
	guint i;

	if (units <= 0)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) unit = NULL;
		gint64 remaining = get_int(g_ptr_array_index(rows, i), "remaining-qty");
		gint64 *total, *count;

		g_object_get(g_ptr_array_index(rows, i), "unit-cost", &unit, NULL);
		if (unit == NULL)
			return FALSE;
		total = g_hash_table_lookup(totals, venture_money_get_currency(unit));
		count = g_hash_table_lookup(counts, venture_money_get_currency(unit));
		if (total == NULL)
		{
			total = g_new0(gint64, 1);
			count = g_new0(gint64, 1);
			g_hash_table_insert(totals, g_strdup(venture_money_get_currency(unit)), total);
			g_hash_table_insert(counts, g_strdup(venture_money_get_currency(unit)), count);
			g_hash_table_insert(dear, g_strdup(venture_money_get_currency(unit)), g_new0(gint64, 1));
		}
		*total += venture_money_get_amount(unit) * remaining;
		*count += remaining;
	}
	g_hash_table_iter_init(&iter, counts);
	while (g_hash_table_iter_next(&iter, &key, &value))
		if (*(gint64 *)value != units)
			return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) unit = NULL;
		gint64 floor;

		g_object_get(g_ptr_array_index(rows, i), "unit-cost", &unit, NULL);
		floor = floor_div(*(gint64 *)g_hash_table_lookup(totals, venture_money_get_currency(unit)), units);
		if (venture_money_get_amount(unit) == floor + 1)
			*(gint64 *)g_hash_table_lookup(dear, venture_money_get_currency(unit)) +=
				get_int(g_ptr_array_index(rows, i), "remaining-qty");
		else if (venture_money_get_amount(unit) != floor)
			return FALSE;
	}
	g_hash_table_iter_init(&iter, totals);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		gint64 total = *(gint64 *)value;

		if (*(gint64 *)g_hash_table_lookup(dear, key) != total - floor_div(total, units) * units)
			return FALSE;
	}
	return TRUE;
}

/* Posts one opening movement of @units at what @rows still hold, dated
 * when the first of them came in, and links each row to its engine layer. */
static gboolean
open_rows(VentureInventoryService *self, gint64 engine_item, GPtrArray *rows, gint64 units, const gchar *key,
	GError **error)
{
	g_autoptr(GPtrArray) totals = venture_money_totals_new();
	g_autoptr(LvMovementRequest) request = lv_movement_request_new("opening");
	g_autoptr(LvCosts) cost = NULL;
	g_autoptr(LvMovement) movement = NULL;
	g_autoptr(GPtrArray) moves = NULL;
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GDateTime) received = NULL;
	g_autoptr(GError) cause = NULL;
	g_autofree gchar *date = NULL;
	g_autofree gchar *reference = NULL;
	VentureEntity *first = g_ptr_array_index(rows, 0);
	gint64 lot;
	guint i, j;

	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) unit = NULL;
		g_autoptr(VentureMoney) slice = NULL;

		g_object_get(g_ptr_array_index(rows, i), "unit-cost", &unit, NULL);
		slice = venture_money_multiply_int(unit, get_int(g_ptr_array_index(rows, i), "remaining-qty"), error);
		if (slice == NULL || !venture_money_totals_add(totals, slice, error))
			return FALSE;
	}
	if (!add_currencies(self, totals, error))
		return FALSE;
	g_object_get(first, "received-at", &received, NULL);
	date = venture_stock_engine_time(received);
	reference = get_id(first, "lot-txn-id") > 0
		? g_strdup_printf("inventory_txn:%" G_GINT64_FORMAT, get_id(first, "lot-txn-id"))
		: g_strdup_printf("inventory_cost_layer:%" G_GINT64_FORMAT, venture_entity_get_id(first));
	cost = engine_costs(totals);
	lv_movement_request_set_date(request, date);
	lv_movement_request_set_reference(request, reference);
	lv_movement_request_set_key(request, key);
	lv_movement_request_give(request, engine_item, NULL, (gdouble)units, NULL, cost);
	movement = venture_stock_engine_post(self->database, request, NULL, NULL, error);
	moves = movement != NULL ? moves_of(self, movement, error) : NULL;
	if (moves == NULL)
		return FALSE;
	lot = lot_made(moves, engine_item);
	layers = lv_ledger_list_layers(store_of(self, NULL), engine_item, &cause);
	if (layers == NULL)
	{
		g_propagate_error(error, g_steal_pointer(&cause));
		return FALSE;
	}
	/* One row per layer the split made: same currency, same unit cost. */
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) unit = NULL;

		g_object_get(row, "unit-cost", &unit, NULL);
		for (j = 0; j < layers->len; j++)
		{
			LvLayer *layer = g_ptr_array_index(layers, j);

			if (layer->lot_id != lot || g_strcmp0(layer->currency, venture_money_get_currency(unit)) != 0 ||
				layer->quantity <= 0 || layer->cost != venture_money_get_amount(unit) * (gint64)layer->quantity)
				continue;
			g_object_set(row, "stock-layer-id", layer->id, NULL);
			if (!save_owned(self, row, NULL, error))
				return FALSE;
			break;
		}
		if (j == layers->len)
			return refuse(error, "an open cost layer has no place in the stock engine");
	}
	return TRUE;
}

gboolean
venture_inventory_service_import_item(VentureInventoryService *self, VentureEntity *item, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) lots = g_ptr_array_new_with_free_func(open_lot_free);
	g_autoptr(GHashTable) by_key = g_hash_table_new(g_int64_hash, g_int64_equal);
	gint64 item_id = venture_entity_get_id(item);
	gint64 engine_item, quantity, covered = 0;
	gboolean created = FALSE;
	guint i, j;

	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	engine_item = venture_stock_engine_ensure_item(self->database, item, &created, error);
	if (engine_item == 0)
		return FALSE;
	if (!created)
		return TRUE;
	quantity = rows_on_hand(self, item_id, NULL, error);
	if (error != NULL && *error != NULL)
		return FALSE;
	if (layers_kept())
	{
		rows = layer_rows(self, item_id, error);
		if (rows == NULL)
			return FALSE;
	}
	/* The open lots, in the order first in, first out takes them. */
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		OpenLot *lot;
		gint64 key = layer_lot(row);

		if (get_int(row, "remaining-qty") <= 0)
			continue;
		lot = g_hash_table_lookup(by_key, &key);
		if (lot == NULL)
		{
			lot = g_new0(OpenLot, 1);
			lot->key = key;
			lot->rows = g_ptr_array_new_with_free_func(g_object_unref);
			g_ptr_array_add(lots, lot);
			g_hash_table_insert(by_key, &lot->key, lot);
		}
		g_ptr_array_add(lot->rows, g_object_ref(row));
	}
	for (i = 0; i < lots->len; i++)
	{
		OpenLot *lot = g_ptr_array_index(lots, i);
		g_autoptr(VentureMoney) lead = NULL;
		gint64 units = 0;

		/* A lot has as many units as its first currency's layers hold. */
		g_object_get(g_ptr_array_index(lot->rows, 0), "unit-cost", &lead, NULL);
		for (j = 0; lead != NULL && j < lot->rows->len; j++)
		{
			g_autoptr(VentureMoney) unit = NULL;

			g_object_get(g_ptr_array_index(lot->rows, j), "unit-cost", &unit, NULL);
			if (unit != NULL && g_strcmp0(venture_money_get_currency(unit), venture_money_get_currency(lead)) == 0)
				units += get_int(g_ptr_array_index(lot->rows, j), "remaining-qty");
		}
		if (lead != NULL && lot_splits_exactly(lot->rows, units))
		{
			g_autofree gchar *key = g_strdup_printf("venture-opening:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
				item_id, lot->key);

			if (!open_rows(self, engine_item, lot->rows, units, key, error))
				return FALSE;
			covered += units;
			continue;
		}
		/* Never written so by this service: each row comes in as a lot
		 * of its own, at what it still holds, so the value is the same. */
		for (j = 0; j < lot->rows->len; j++)
		{
			VentureEntity *row = g_ptr_array_index(lot->rows, j);
			g_autoptr(GPtrArray) one = g_ptr_array_new();
			g_autofree gchar *key = g_strdup_printf("venture-opening:%" G_GINT64_FORMAT ":layer:%"
				G_GINT64_FORMAT, item_id, venture_entity_get_id(row));
			gint64 remaining = get_int(row, "remaining-qty");
			g_autoptr(VentureMoney) unit = NULL;

			g_object_get(row, "unit-cost", &unit, NULL);
			if (unit == NULL)
				continue;
			g_ptr_array_add(one, row);
			if (!open_rows(self, engine_item, one, remaining, key, error))
				return FALSE;
			if (lead != NULL && g_strcmp0(venture_money_get_currency(unit), venture_money_get_currency(lead)) == 0)
				covered += remaining;
		}
	}
	/* The units no layer covers: a count, at the count's own place, so it
	 * never takes a costed unit. Below zero when the rows were. */
	if (quantity != covered)
	{
		g_autoptr(LvMovementRequest) request = lv_movement_request_new("adjustment");
		g_autoptr(LvMovement) movement = NULL;
		g_autofree gchar *key = g_strdup_printf("venture-opening:%" G_GINT64_FORMAT ":count", item_id);
		g_autofree gchar *date = venture_stock_engine_time(NULL);

		lv_movement_request_set_date(request, date);
		lv_movement_request_set_reference(request, "opening count");
		lv_movement_request_set_key(request, key);
		if (quantity > covered)
			lv_movement_request_give(request, engine_item, VENTURE_STOCK_COUNT_PLACE,
				(gdouble)(quantity - covered), NULL, NULL);
		else
			lv_movement_request_take(request, engine_item, VENTURE_STOCK_COUNT_PLACE,
				(gdouble)(covered - quantity));
		movement = venture_stock_engine_post(self->database, request, NULL, NULL, error);
		if (movement == NULL)
			return FALSE;
	}
	return TRUE;
}

/* --- Movements ----------------------------------------------------------- */

/* Each operation is one level of the database's transaction: nested in a
 * caller's, it joins it, and a failure abandons the whole -- the engine's
 * savepoint, the rows and the journal together. */
static gboolean
fail(VentureInventoryService *self)
{
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean
venture_inventory_service_receive_impl(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GPtrArray) totals = venture_money_totals_new();
	g_autoptr(LvMovementRequest) request = lv_movement_request_new("purchase");
	g_autoptr(LvCosts) cost = NULL;
	g_autoptr(LvMovement) movement = NULL;
	g_autoptr(GPtrArray) moves = NULL;
	g_autoptr(GHashTable) lots = lots_new();
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *time = NULL;
	gint64 org, inventory, grni, engine_item;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (quantity <= 0 || unit_cost == NULL)
		return refuse(error, "receipts need a positive quantity and unit cost");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	total = venture_money_multiply_int(unit_cost, quantity, error);
	if (total == NULL || !venture_money_totals_add(totals, total, error) ||
		!venture_database_begin(self->database, error))
		return FALSE;
	item = get_item(self, inventory_item_id, error);
	engine_item = item != NULL ? engine_item_for(self, item, error) : 0;
	if (engine_item == 0 || !add_currencies(self, totals, error))
		return fail(self);
	time = venture_stock_engine_time(when);
	cost = engine_costs(totals);
	lv_movement_request_set_date(request, time);
	lv_movement_request_set_reference(request, reference);
	lv_movement_request_give(request, engine_item, NULL, (gdouble)quantity, NULL, cost);
	movement = venture_stock_engine_post(self->database, request, NULL, NULL, error);
	moves = movement != NULL ? moves_of(self, movement, error) : NULL;
	if (moves == NULL)
		return fail(self);
	txn = write_txn(self, item, quantity, VENTURE_INVENTORY_TXN_KIND_PURCHASE, unit_cost, NULL, when, reference, 0,
		actor, error);
	if (txn == NULL)
		return fail(self);
	lots_add(lots, lot_made(moves, engine_item), venture_entity_get_id(txn));
	if (!sync_layers(self, item, engine_item, lots, receipt_line_id, actor, error))
		return fail(self);
	org = venture_entity_get_organization_id(txn);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	grni = account_code(self, org, "grni", "2010", "Goods received not invoiced",
		VENTURE_ACCOUNT_KIND_LIABILITY, error);
	if (inventory == 0 || grni == 0)
		return fail(self);
	/* GRNI is a liability; a credit increases it until the matched bill clears it. */
	if (!post_pair(self, org, txn, inventory, grni, total, "Inventory receipt", when, actor, error))
		return fail(self);
	return venture_database_commit(self->database, error);
}

/*
 * Takes @quantity out of @item by an engine movement of @kind, costed lots
 * first and, unless the item allows negative stock, only costed units.
 * Gives the movement's costs per currency and writes its txn row.
 */
static VentureEntity *
take_out(VentureInventoryService *self, VentureEntity *item, gint64 engine_item, gint64 quantity,
	const gchar *kind, VentureInventoryTxnKind txn_kind, GDateTime *when, const gchar *reference, gint64 sale_id,
	GPtrArray **out_costs, const VentureActor *actor, GError **error)
{
	g_autoptr(LvMovementRequest) request = lv_movement_request_new(kind);
	g_autoptr(GHashTable) strict = strict_for(item, engine_item);
	g_autoptr(LvMovement) movement = NULL;
	g_autoptr(LvCosts) moved = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *time = venture_stock_engine_time(when);
	VentureEntity *txn;

	*out_costs = NULL;
	if (!guard_stock(self, item, engine_item, -quantity, error))
		return NULL;
	lv_movement_request_set_date(request, time);
	lv_movement_request_set_reference(request, reference);
	lv_movement_request_set_lot_order(request, LV_LOT_ORDER_COSTED_FIRST);
	lv_movement_request_take(request, engine_item, NULL, (gdouble)quantity);
	movement = venture_stock_engine_post(self->database, request, strict, &moved, error);
	if (movement == NULL)
		return NULL;
	/* The layers may have cost several currencies; each is kept apart
	 * and each posts by its own treatment. */
	costs = venture_costs(moved, error);
	if (costs == NULL || !sync_layers(self, item, engine_item, NULL, 0, actor, error) ||
		!movement_unit(costs, quantity, &unit, &notes, error))
		return NULL;
	txn = write_txn(self, item, -quantity, txn_kind, unit, notes, when, reference, sale_id, actor, error);
	if (txn != NULL)
		*out_costs = g_steal_pointer(&costs);
	return txn;
}

static gboolean
venture_inventory_service_issue_impl(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *source_type, gint64 source_id,
	const VentureActor *actor, GPtrArray **out_costs, GError **error)
{
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory, cogs_id, engine_item;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (out_costs != NULL)
		*out_costs = NULL;
	if (quantity <= 0)
		return refuse(error, "issues need a positive quantity");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!venture_database_begin(self->database, error))
		return FALSE;
	item = get_item(self, inventory_item_id, error);
	engine_item = item != NULL ? engine_item_for(self, item, error) : 0;
	if (engine_item == 0)
		return fail(self);
	txn = take_out(self, item, engine_item, quantity, "sale", VENTURE_INVENTORY_TXN_KIND_SALE, when, source_type,
		g_strcmp0(source_type, "sale") == 0 ? source_id : 0, &costs, actor, error);
	if (txn == NULL)
		return fail(self);
	org = venture_entity_get_organization_id(txn);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	cogs_id = account_code(self, org, "cogs", "5000", "Cost of goods sold", VENTURE_ACCOUNT_KIND_EXPENSE, error);
	if (inventory == 0 || cogs_id == 0 ||
		!post_costs(self, org, txn, cogs_id, inventory, costs, "Inventory issue", when, actor, error))
		return fail(self);
	if (!venture_database_commit(self->database, error))
		return FALSE;
	if (out_costs != NULL)
		*out_costs = g_steal_pointer(&costs);
	return TRUE;
}

/*
 * The engine lots of @item's receipt line, oldest first: the rows of that
 * line point at their engine layers, and each layer at its lot.
 */
static gboolean
receipt_lots(VentureInventoryService *self, VentureEntity *item, gint64 engine_item, gint64 receipt_line_id,
	GArray *lots, GError **error)
{
	g_autoptr(GPtrArray) rows = layer_rows(self, venture_entity_get_id(item), error);
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GError) cause = NULL;
	guint i, j, k;

	if (rows == NULL)
		return FALSE;
	layers = lv_ledger_list_layers(store_of(self, NULL), engine_item, &cause);
	if (layers == NULL)
	{
		g_propagate_error(error, g_steal_pointer(&cause));
		return FALSE;
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);

		if (get_id(row, "goods-receipt-line-id") != receipt_line_id || get_int(row, "remaining-qty") <= 0)
			continue;
		for (j = 0; j < layers->len; j++)
		{
			LvLayer *layer = g_ptr_array_index(layers, j);
			gboolean seen = FALSE;

			if (layer->id != get_int(row, "stock-layer-id"))
				continue;
			for (k = 0; k < lots->len; k++)
				seen = seen || g_array_index(lots, gint64, k) == layer->lot_id;
			if (!seen)
				g_array_append_val(lots, layer->lot_id);
		}
	}
	return TRUE;
}

static gboolean
venture_inventory_service_restore_impl(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory, grni, engine_item;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (quantity <= 0 || unit_cost == NULL)
		return refuse(error, "returns need a positive quantity and unit cost");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!venture_database_begin(self->database, error))
		return FALSE;
	item = get_item(self, inventory_item_id, error);
	engine_item = item != NULL ? engine_item_for(self, item, error) : 0;
	if (engine_item == 0)
		return fail(self);
	if (receipt_line_id != 0)
	{
		/* The units of that receipt go back, not the oldest units. */
		g_autoptr(LvMovementRequest) request = lv_movement_request_new("return");
		g_autoptr(GArray) lots = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_autoptr(LvMovement) movement = NULL;
		g_autofree gchar *time = venture_stock_engine_time(when);
		gint64 need = quantity;
		guint i;

		if (!guard_stock(self, item, engine_item, -quantity, error) ||
			!receipt_lots(self, item, engine_item, receipt_line_id, lots, error))
			return fail(self);
		for (i = 0; i < lots->len && need > 0; i++)
		{
			g_autoptr(LvLot) lot = lv_store_get(store_of(self, NULL), LV_TYPE_LOT, g_array_index(lots, gint64, i),
				NULL);
			gint64 take = lot != NULL ? MIN((gint64)lot->quantity, need) : 0;

			if (take <= 0)
				continue;
			lv_movement_request_take_lot(request, engine_item, lot->id, (gdouble)take);
			need -= take;
		}
		if (need > 0)
		{
			refuse(error, "negative stock is refused");
			return fail(self);
		}
		lv_movement_request_set_date(request, time);
		lv_movement_request_set_reference(request, reference);
		movement = venture_stock_engine_post(self->database, request, NULL, NULL, error);
		if (movement == NULL || !sync_layers(self, item, engine_item, NULL, 0, actor, error))
			return fail(self);
		txn = write_txn(self, item, -quantity, VENTURE_INVENTORY_TXN_KIND_RETURN, unit_cost, NULL, when, reference,
			0, actor, error);
	}
	else
	{
		g_autoptr(GPtrArray) costs = NULL;
		g_autoptr(LvMovementRequest) request = lv_movement_request_new("return");
		g_autoptr(GHashTable) strict = strict_for(item, engine_item);
		g_autoptr(LvMovement) movement = NULL;
		g_autofree gchar *time = venture_stock_engine_time(when);

		/* First in, first out, and only costed units, as an issue; the
		 * row shows the price the supplier is credited at. */
		if (!guard_stock(self, item, engine_item, -quantity, error))
			return fail(self);
		lv_movement_request_set_date(request, time);
		lv_movement_request_set_reference(request, reference);
		lv_movement_request_set_lot_order(request, LV_LOT_ORDER_COSTED_FIRST);
		lv_movement_request_take(request, engine_item, NULL, (gdouble)quantity);
		movement = venture_stock_engine_post(self->database, request, strict, NULL, error);
		if (movement == NULL || !sync_layers(self, item, engine_item, NULL, 0, actor, error))
			return fail(self);
		txn = write_txn(self, item, -quantity, VENTURE_INVENTORY_TXN_KIND_RETURN, unit_cost, NULL, when, reference,
			0, actor, error);
	}
	if (txn == NULL)
		return fail(self);
	org = venture_entity_get_organization_id(txn);
	total = venture_money_multiply_int(unit_cost, quantity, error);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	grni = account_code(self, org, "grni", "2010", "Goods received not invoiced",
		VENTURE_ACCOUNT_KIND_LIABILITY, error);
	if (total == NULL || inventory == 0 || grni == 0 ||
		!post_pair(self, org, txn, grni, inventory, total, "Inventory return", when, actor, error))
		return fail(self);
	return venture_database_commit(self->database, error);
}

/*
 * A receipt paid from @credit_id rather than accrued to goods received not
 * invoiced: stock bought outright at a venue, whose money left the venue's
 * cash the moment it was bought. The cost is a total the engine splits
 * over the units exactly (one layer a minor unit higher and one at the
 * floor, in one lot), because 100.00 paid for 7 units has no exact unit
 * cost and a rounded one would leave a cent behind on every flip.
 */
static gboolean
receive_from_impl(VentureInventoryService *self, gint64 inventory_item_id, gint64 quantity,
	const VentureMoney *total_cost, GDateTime *date, const gchar *reference, gint64 credit_id,
	const VentureActor *actor, VentureEntity **out_txn, GError **error)
{
	g_autoptr(GPtrArray) costs = venture_money_totals_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *time = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(LvMovementRequest) request = lv_movement_request_new("bought");
	g_autoptr(LvCosts) cost = NULL;
	g_autoptr(LvMovement) movement = NULL;
	g_autoptr(GPtrArray) moves = NULL;
	g_autoptr(GHashTable) lots = lots_new();
	gint64 org, inventory, engine_item;
	if (quantity <= 0 || total_cost == NULL || venture_money_is_negative(total_cost))
		return refuse(error, "receipts need a positive quantity and a cost of zero or more");
	if (credit_id <= 0)
		return refuse(error, "a receipt paid outright needs the account it was paid from");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	item = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (item == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(item);
	/* The account paid from must be the item's organization's: a journal
	 * between two organizations' charts is refused by the posting service
	 * anyway, but only after the stock moved. */
	credit = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, credit_id, error);
	if (credit == NULL)
		return FALSE;
	if (venture_entity_get_organization_id(credit) != org)
		return refuse(error, "the account paid from belongs to another organization");
	if (!venture_money_totals_add(costs, total_cost, error) ||
		!movement_unit(costs, quantity, &unit, &notes, error) || !venture_database_begin(self->database, error))
		return FALSE;
	engine_item = engine_item_for(self, item, error);
	if (engine_item == 0 || !add_currencies(self, costs, error))
		return fail(self);
	time = venture_stock_engine_time(when);
	cost = engine_costs(costs);
	lv_movement_request_set_date(request, time);
	lv_movement_request_set_reference(request, reference);
	lv_movement_request_give(request, engine_item, NULL, (gdouble)quantity, NULL, cost);
	movement = venture_stock_engine_post(self->database, request, NULL, NULL, error);
	moves = movement != NULL ? moves_of(self, movement, error) : NULL;
	if (moves == NULL)
		return fail(self);
	txn = write_txn(self, item, quantity, VENTURE_INVENTORY_TXN_KIND_PURCHASE, unit, notes, when, reference, 0,
		actor, error);
	if (txn == NULL)
		return fail(self);
	lots_add(lots, lot_made(moves, engine_item), venture_entity_get_id(txn));
	if (!sync_layers(self, item, engine_item, lots, 0, actor, error))
		return fail(self);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	if (inventory == 0)
		return fail(self);
	/* Dr inventory, Cr the account paid from. GRNI is not touched: there
	 * is no bill to come that would clear it. */
	if (!post_pair(self, org, txn, inventory, credit_id, total_cost, "Inventory bought", when, actor, error))
		return fail(self);
	if (!venture_database_commit(self->database, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	return TRUE;
}

/*
 * An issue whose cost goes to @debit_id rather than cost of goods sold:
 * stock leaving for an arbitrage position (sold at a venue, or written off
 * when the trade is abandoned), whose profit or loss is decided when the
 * position closes. First in, first out, the lots and the per-currency
 * posting are an issue's, unchanged.
 */
static gboolean
issue_to_impl(VentureInventoryService *self, gint64 inventory_item_id, gint64 quantity,
	GDateTime *date, const gchar *reference, gint64 debit_id, VentureInventoryTxnKind kind,
	const VentureActor *actor, VentureEntity **out_txn, GPtrArray **out_costs, GError **error)
{
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) debit = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory, engine_item;
	if (quantity <= 0)
		return refuse(error, "issues need a positive quantity");
	if (kind != VENTURE_INVENTORY_TXN_KIND_SALE && kind != VENTURE_INVENTORY_TXN_KIND_WRITE_OFF)
		return refuse(error, "an issue to an account is a sale or a write-off");
	if (debit_id <= 0)
		return refuse(error, "an issue to an account needs the account");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!venture_database_begin(self->database, error))
		return FALSE;
	item = get_item(self, inventory_item_id, error);
	engine_item = item != NULL ? engine_item_for(self, item, error) : 0;
	if (engine_item == 0)
		return fail(self);
	txn = take_out(self, item, engine_item, quantity,
		kind == VENTURE_INVENTORY_TXN_KIND_WRITE_OFF ? LV_BASE_WRITE_OFF : "sale", kind, when, reference, 0, &costs,
		actor, error);
	if (txn == NULL)
		return fail(self);
	org = venture_entity_get_organization_id(txn);
	debit = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, debit_id, error);
	if (debit == NULL)
		return fail(self);
	if (venture_entity_get_organization_id(debit) != org)
	{
		refuse(error, "the account issued to belongs to another organization");
		return fail(self);
	}
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	if (inventory == 0 ||
		!post_costs(self, org, txn, debit_id, inventory, costs,
			kind == VENTURE_INVENTORY_TXN_KIND_WRITE_OFF ? "Inventory written off" : "Inventory issued",
			when, actor, error))
		return fail(self);
	if (!venture_database_commit(self->database, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	if (out_costs != NULL)
		*out_costs = g_steal_pointer(&costs);
	return TRUE;
}

/* What the movement's take lines out of @engine_item cost, per currency;
 * @any is set when one of them carried a cost (a layer at zero counts). */
static GPtrArray *
taken_costs(GPtrArray *moves, gint64 engine_item, gboolean *any, GError **error)
{
	g_autoptr(GPtrArray) totals = venture_money_totals_new();
	guint i, j;

	for (i = 0; i < moves->len; i++)
	{
		LvMove *move = g_ptr_array_index(moves, i);
		g_autoptr(GPtrArray) costs = NULL;

		if (move->quantity >= 0 || move->item_id != engine_item)
			continue;
		costs = move_costs(move, error);
		if (costs == NULL)
			return NULL;
		for (j = 0; j < costs->len; j++)
		{
			if (!venture_money_totals_add(totals, g_ptr_array_index(costs, j), error))
				return NULL;
			if (any != NULL)
				*any = TRUE;
		}
	}
	return g_steal_pointer(&totals);
}

/*
 * Moves @quantity from one inventory item to another -- two places of one
 * product -- in one engine movement: the units leave @from first in, first
 * out, and arrive at @to as a new lot carrying what they cost, re-split
 * exactly per currency; with no cost at all, one zero layer in the
 * destination's currency.
 */
static gboolean
venture_inventory_service_transfer_impl(VentureInventoryService *self, gint64 from_item_id,
	gint64 to_item_id, gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) from = NULL;
	g_autoptr(VentureEntity) to = NULL;
	g_autoptr(VentureEntity) out = NULL;
	g_autoptr(VentureEntity) in = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(LvMovementRequest) request = lv_movement_request_new("item-transfer");
	g_autoptr(LvMovement) movement = NULL;
	g_autoptr(GPtrArray) moves = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(GHashTable) strict = NULL;
	g_autoptr(GHashTable) lots = lots_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *time = NULL;
	g_autofree gchar *zero = NULL;
	gint64 from_engine = 0, to_engine = 0;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (quantity <= 0 || from_item_id == to_item_id)
		return refuse(error, "transfers move a positive quantity between two locations");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!venture_database_begin(self->database, error))
		return FALSE;
	from = get_item(self, from_item_id, error);
	to = from != NULL ? get_item(self, to_item_id, error) : NULL;
	if (to != NULL)
		from_engine = engine_item_for(self, from, error);
	if (from_engine != 0)
		to_engine = engine_item_for(self, to, error);
	if (to_engine == 0 || !guard_stock(self, from, from_engine, -quantity, error))
		return fail(self);
	zero = zero_cost_currency(self, to);
	if (!venture_stock_engine_add_currency(self->database, zero, error))
		return fail(self);
	time = venture_stock_engine_time(when);
	strict = strict_for(from, from_engine);
	lv_movement_request_set_date(request, time);
	lv_movement_request_set_reference(request, "transfer");
	lv_movement_request_set_currency(request, zero);
	lv_movement_request_set_lot_order(request, LV_LOT_ORDER_COSTED_FIRST);
	lv_movement_request_take(request, from_engine, NULL, (gdouble)quantity);
	lv_movement_request_give(request, to_engine, NULL, (gdouble)quantity, NULL, NULL);
	movement = venture_stock_engine_post(self->database, request, strict, NULL, error);
	moves = movement != NULL ? moves_of(self, movement, error) : NULL;
	costs = moves != NULL ? taken_costs(moves, from_engine, NULL, error) : NULL;
	if (costs == NULL || !movement_unit(costs, quantity, &unit, &notes, error))
		return fail(self);
	out = write_txn(self, from, -quantity, VENTURE_INVENTORY_TXN_KIND_TRANSFER, unit, notes, when, "transfer", 0,
		actor, error);
	in = out != NULL ? write_txn(self, to, quantity, VENTURE_INVENTORY_TXN_KIND_TRANSFER, unit, notes, when,
		"transfer", 0, actor, error) : NULL;
	if (in == NULL)
		return fail(self);
	lots_add(lots, lot_made(moves, to_engine), venture_entity_get_id(in));
	if (!sync_layers(self, from, from_engine, NULL, 0, actor, error) ||
		!sync_layers(self, to, to_engine, lots, 0, actor, error))
		return fail(self);
	return venture_database_commit(self->database, error);
}

/*
 * The share of @totals (what @whole units cost) that @part units take,
 * after @before units took theirs: each currency by the units, in whole
 * minor units, so the shares of one item add up to its cost exactly. A
 * craft that names one item twice takes it once in the engine.
 */
static GPtrArray *
share_of(GPtrArray *totals, gint64 whole, gint64 part, gint64 before, GError **error)
{
	g_autoptr(GPtrArray) shares = venture_money_totals_new();
	guint i;

	for (i = 0; i < totals->len; i++)
	{
		const VentureMoney *total = g_ptr_array_index(totals, i);
		gint64 amount = venture_money_get_amount(total);
		gint64 upto = whole > 0 ? (amount * (before + part)) / whole : 0;
		gint64 prior = whole > 0 ? (amount * before) / whole : 0;
		g_autoptr(VentureMoney) slice = venture_money_new_for_currency(before + part == whole ? amount - prior
			: upto - prior, venture_money_get_currency(total));

		if (!venture_money_totals_add(shares, slice, error))
			return NULL;
	}
	return g_steal_pointer(&shares);
}

static gint
input_index(GPtrArray *inputs, gint64 item_id)
{
	guint i;

	for (i = 0; i < inputs->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(inputs, i)) == item_id)
			return (gint)i;
	return -1;
}

static gboolean
produce_body(VentureInventoryService *self, const VentureInventoryDraw *draws, guint n_draws,
	gint64 output_item_id, gint64 output_quantity, GDateTime *when, const gchar *reference,
	const VentureActor *actor, VentureEntity **out_txn, GPtrArray **out_costs, GError **error)
{
	g_autoptr(GPtrArray) total = venture_money_totals_new();
	g_autoptr(GPtrArray) inputs = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GArray) engines = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) wanted = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) given = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(LvMovementRequest) request = lv_movement_request_new("production");
	g_autoptr(LvMovement) movement = NULL;
	g_autoptr(GPtrArray) moves = NULL;
	g_autoptr(GHashTable) lots = lots_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *time = NULL;
	g_autofree gchar *zero = NULL;
	g_autoptr(VentureEntity) output = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	gboolean costed = layers_kept();
	gboolean consumed = FALSE;
	gint64 output_engine;
	guint i, j;
	output = get_item(self, output_item_id, error);
	output_engine = output != NULL ? engine_item_for(self, output, error) : 0;
	if (output_engine == 0)
		return FALSE;

	/* --- What goes in: each item once, its draws summed --- */
	for (i = 0; i < n_draws; i++)
	{
		gint at = input_index(inputs, draws[i].inventory_item_id);

		if (at < 0)
		{
			VentureEntity *input = get_item(self, draws[i].inventory_item_id, error);
			gint64 engine_input = 0;
			gint64 none = 0;

			if (input == NULL)
				return FALSE;
			g_ptr_array_add(inputs, input);
			engine_input = engine_item_for(self, input, error);
			if (engine_input == 0)
				return FALSE;
			g_array_append_val(engines, engine_input);
			g_array_append_val(wanted, none);
			g_array_append_val(given, none);
			at = (gint)inputs->len - 1;
		}
		g_array_index(wanted, gint64, at) += draws[i].quantity;
	}
	/* Units that arrived without a layer -- an adjustment typed in by
	 * hand -- are real stock with no recorded cost, used after the costed
	 * ones at no cost: refusing them would make stock the count says is
	 * there unusable. */
	for (j = 0; j < inputs->len; j++)
		if (!guard_stock(self, g_ptr_array_index(inputs, j), g_array_index(engines, gint64, j),
			-g_array_index(wanted, gint64, j), error))
			return FALSE;
	zero = zero_cost_currency(self, output);
	if (!venture_stock_engine_add_currency(self->database, zero, error))
		return FALSE;
	time = venture_stock_engine_time(when);
	lv_movement_request_set_date(request, time);
	lv_movement_request_set_reference(request, reference);
	lv_movement_request_set_currency(request, zero);
	lv_movement_request_set_lot_order(request, LV_LOT_ORDER_COSTED_FIRST);
	for (j = 0; j < inputs->len; j++)
		lv_movement_request_take(request, g_array_index(engines, gint64, j), NULL,
			(gdouble)g_array_index(wanted, gint64, j));
	lv_movement_request_give(request, output_engine, NULL, (gdouble)output_quantity, NULL, NULL);
	movement = venture_stock_engine_post(self->database, request, NULL, NULL, error);
	moves = movement != NULL ? moves_of(self, movement, error) : NULL;
	if (moves == NULL)
		return FALSE;

	/* Each draw leaves as its own row at its share of what its item cost;
	 * per currency, never added across: a made unit whose inputs cost GOLD
	 * and TICKET carries both. */
	for (i = 0; i < n_draws; i++)
	{
		gint at = input_index(inputs, draws[i].inventory_item_id);
		g_autoptr(GPtrArray) item_costs = NULL;
		g_autoptr(GPtrArray) cost = NULL;
		g_autoptr(VentureMoney) each = NULL;
		g_autofree gchar *each_notes = NULL;
		g_autoptr(VentureEntity) out = NULL;

		item_costs = taken_costs(moves, g_array_index(engines, gint64, at), NULL, error);
		if (item_costs == NULL)
			return FALSE;
		cost = share_of(item_costs, g_array_index(wanted, gint64, at), draws[i].quantity,
			g_array_index(given, gint64, at), error);
		if (cost == NULL)
			return FALSE;
		g_array_index(given, gint64, at) += draws[i].quantity;
		/* Without the goods module no layer is read, and a craft moves
		 * quantities only. */
		if (!costed)
			g_ptr_array_set_size(cost, 0);
		for (j = 0; j < cost->len; j++)
		{
			if (!venture_money_totals_add(total, g_ptr_array_index(cost, j), error))
				return FALSE;
			consumed = TRUE;
		}
		/* The row shows the average it left at, for reading; the layers
		 * are what the books count. */
		if (!movement_unit(cost, draws[i].quantity, &each, &each_notes, error))
			return FALSE;
		out = write_txn(self, g_ptr_array_index(inputs, at), -draws[i].quantity, VENTURE_INVENTORY_TXN_KIND_PRODUCTION,
			each, each_notes, when, reference, 0, actor, error);
		if (out == NULL)
			return FALSE;
	}

	/* --- What comes out --- */
	if (!movement_unit(total, output_quantity, &unit, &notes, error))
		return FALSE;
	txn = write_txn(self, output, output_quantity, VENTURE_INVENTORY_TXN_KIND_PRODUCTION, unit, notes, when,
		reference, 0, actor, error);
	if (txn == NULL)
		return FALSE;
	lots_add(lots, lot_made(moves, output_engine), venture_entity_get_id(txn));
	for (j = 0; j < inputs->len; j++)
		if (!sync_layers(self, g_ptr_array_index(inputs, j), g_array_index(engines, gint64, j), NULL, 0, actor,
			error))
			return FALSE;
	if (!sync_layers(self, output, output_engine, lots, 0, actor, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	if (out_costs != NULL && consumed)
		*out_costs = g_steal_pointer(&total);
	return TRUE;
}

gboolean
venture_inventory_service_produce(VentureInventoryService *self,
	const VentureInventoryDraw *draws, guint n_draws, gint64 output_item_id,
	gint64 output_quantity, GDateTime *date, const gchar *reference,
	const VentureActor *actor, VentureEntity **out_txn, GPtrArray **out_costs,
	GError **error)
{
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	g_return_val_if_fail(n_draws == 0 || draws != NULL, FALSE);
	if (out_txn != NULL)
		*out_txn = NULL;
	if (out_costs != NULL)
		*out_costs = NULL;
	if (self->database == NULL)
		return refuse(error, "the database is unavailable");
	if (output_quantity <= 0)
		return refuse(error, "production makes a positive quantity");
	for (i = 0; i < n_draws; i++)
	{
		if (draws[i].quantity <= 0)
			return refuse(error, "production consumes positive quantities");
		if (draws[i].inventory_item_id == output_item_id)
			return refuse(error, "production cannot consume the stock it makes");
	}
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	/* Nested inside a caller's transaction this joins it, and a failure
	 * below abandons the whole of it -- which is the point. */
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!produce_body(self, draws, n_draws, output_item_id, output_quantity, when, reference,
		actor, &txn, &costs, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	if (!venture_database_commit(self->database, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	if (out_costs != NULL)
		*out_costs = g_steal_pointer(&costs);
	return TRUE;
}

/* --- A count typed in by hand -------------------------------------------- */

/*
 * An inventory_txn saved by anyone but this service is a count: the
 * quantity changes and no cost does. It posts an engine count at the
 * count's own place first -- up brings units with no cost, down takes the
 * count's units and then goes below zero there, never a costed unit -- and
 * the row is written beside it, in one transaction. An edit posts the
 * difference; a move to another item takes it back from the first.
 */
static gboolean
save_count(VentureInventoryService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) old_item = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(LvMovementRequest) request = lv_movement_request_new("adjustment");
	g_autoptr(LvMovement) movement = NULL;
	g_autofree gchar *time = NULL;
	g_autofree gchar *reference = NULL;
	gint64 item_id = get_id(record, "inventory-item-id");
	gint64 quantity = get_int(record, "quantity");
	gint64 old_item_id = 0, old_quantity = 0, delta;
	gint64 engine_item, old_engine = 0;

	if (venture_entity_is_persisted(record))
	{
		stored = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_TXN, venture_entity_get_id(record),
			error);
		if (stored == NULL)
			return error == NULL || *error != NULL ? FALSE : refuse(error, "the movement does not exist");
		old_item_id = get_id(stored, "inventory-item-id");
		old_quantity = get_int(stored, "quantity");
	}
	if (old_item_id == item_id && old_quantity == quantity)
		return save_owned(self, record, actor, error);
	delta = old_item_id == item_id ? quantity - old_quantity : quantity;
	g_object_get(record, "occurred-at", &when, "reference", &reference, NULL);
	if (!venture_database_begin(self->database, error))
		return FALSE;
	item = get_item(self, item_id, error);
	engine_item = item != NULL ? engine_item_for(self, item, error) : 0;
	if (engine_item == 0)
		return fail(self);
	if (old_item_id != 0 && old_item_id != item_id)
	{
		old_item = get_item(self, old_item_id, error);
		old_engine = old_item != NULL ? engine_item_for(self, old_item, error) : 0;
		if (old_engine == 0 || !guard_stock(self, old_item, old_engine, -old_quantity, error))
			return fail(self);
	}
	if (!guard_stock(self, item, engine_item, delta, error))
		return fail(self);
	time = venture_stock_engine_time(when);
	lv_movement_request_set_date(request, time);
	lv_movement_request_set_reference(request, reference);
	if (old_engine != 0 && old_quantity > 0)
		lv_movement_request_take(request, old_engine, VENTURE_STOCK_COUNT_PLACE, (gdouble)old_quantity);
	else if (old_engine != 0 && old_quantity < 0)
		lv_movement_request_give(request, old_engine, VENTURE_STOCK_COUNT_PLACE, (gdouble)-old_quantity, NULL, NULL);
	if (delta > 0)
		lv_movement_request_give(request, engine_item, VENTURE_STOCK_COUNT_PLACE, (gdouble)delta, NULL, NULL);
	else if (delta < 0)
		lv_movement_request_take(request, engine_item, VENTURE_STOCK_COUNT_PLACE, (gdouble)-delta);
	if ((delta != 0 || old_quantity != 0) &&
		(movement = venture_stock_engine_post(self->database, request, NULL, NULL, error)) == NULL)
		return fail(self);
	if (!save_owned(self, record, actor, error))
		return fail(self);
	return venture_database_commit(self->database, error);
}

gboolean
venture_inventory_product_is_stocked(VentureDatabase *database, gint64 product_id)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) found = NULL;
	if (database == NULL || product_id <= 0)
		return FALSE;
	query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_limit(query, 1);
	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ, product_id, NULL))
		return FALSE;
	found = venture_database_find_one(database, query, NULL);
	return found != NULL;
}

static GPtrArray *
items_for_product(VentureInventoryService *self, gint64 product_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ, product_id, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static gboolean
venture_inventory_service_issue_sale_impl(VentureInventoryService *self, VentureEntity *sale,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 product_id, quantity, i;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (!VENTURE_IS_SALE(sale))
		return TRUE;
	g_object_get(sale, "product-id", &product_id, "quantity", &quantity, "occurred-at", &when, NULL);
	if (product_id <= 0 || quantity <= 0)
		return TRUE;
	{
		g_autoptr(GPtrArray) existing = find_rows(self, VENTURE_TYPE_INVENTORY_TXN, "sale-id",
			venture_entity_get_id(sale), error);
		if (existing == NULL)
			return FALSE;
		if (existing->len > 0)
			return TRUE;
	}
	items = items_for_product(self, product_id, error);
	if (items == NULL)
		return FALSE;
	if (items->len == 0)
		return TRUE;
	for (i = 0; i < (gint64)items->len; i++)
	{
		gint64 item_id = venture_entity_get_id(g_ptr_array_index(items, i));
		gint64 available = venture_inventory_service_on_hand(self, item_id, NULL, error);
		gint64 take;
		if (error != NULL && *error != NULL)
			return FALSE;
		take = available < quantity ? available : quantity;
		if (take <= 0)
			continue;
		if (!venture_inventory_service_issue(self, item_id, take, when, "sale",
			venture_entity_get_id(sale), actor, NULL, error))
			return FALSE;
		quantity -= take;
		if (quantity == 0)
			return TRUE;
	}
	return quantity == 0 || refuse(error, "negative stock is refused");
}

static gboolean
venture_inventory_service_issue_invoice_impl(VentureInventoryService *self, VentureEntity *invoice,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(GDateTime) when = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (invoice == NULL)
		return TRUE;
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SALES_ORDER);
		g_autoptr(VentureEntity) so = NULL;
		venture_query_set_organization(query, venture_entity_get_organization_id(invoice));
		if (venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(invoice), NULL))
			so = venture_database_find_one(self->database, query, NULL);
		if (so != NULL)
			return TRUE;
	}
	g_object_get(invoice, "issued-at", &when, NULL);
	lines = find_rows(self, VENTURE_TYPE_INVOICE_LINE, "invoice-id", venture_entity_get_id(invoice), error);
	if (lines == NULL)
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		g_autoptr(GPtrArray) items = NULL;
		gdouble quantity = 0;
		gint64 product_id = get_id(g_ptr_array_index(lines, i), "product-id");
		gint64 need;
		guint j;
		g_object_get(g_ptr_array_index(lines, i), "quantity", &quantity, NULL);
		need = (gint64)quantity;
		if (product_id <= 0 || need <= 0)
			continue;
		items = items_for_product(self, product_id, error);
		if (items == NULL)
			return FALSE;
		if (items->len == 0)
			continue;
		for (j = 0; j < items->len && need > 0; j++)
		{
			gint64 item_id = venture_entity_get_id(g_ptr_array_index(items, j));
			gint64 available = venture_inventory_service_on_hand(self, item_id, NULL, error);
			gint64 take = available < need ? available : need;
			if (take <= 0)
				continue;
			if (!venture_inventory_service_issue(self, item_id, take, when, "invoice",
				venture_entity_get_id(invoice), actor, NULL, error))
				return FALSE;
			need -= take;
		}
		if (need > 0)
			return refuse(error, "negative stock is refused");
	}
	return TRUE;
}

static gboolean
line_parent_is_draft(VentureDatabase *database, VentureEntity *line,
	GType parent_type, const gchar *parent_field, GError **error)
{
	g_autoptr(VentureEntity) parent = venture_database_get(database, parent_type,
		get_id(line, parent_field), error);
	g_autofree gchar *status = NULL;
	if (parent == NULL)
	{
		/* A missing row is NULL with no error set; unnamed, it reached the
		 * caller as "Unknown error" for a line saved with order id 0. */
		if (error != NULL && *error == NULL)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"An order line needs the %s it belongs to; %s %" G_GINT64_FORMAT " does not exist",
				parent_type == VENTURE_TYPE_SALES_ORDER ? "sales order" : "purchase order",
				parent_field, get_id(line, parent_field));
		return FALSE;
	}
	g_object_get(parent, "status", &status, NULL);
	return g_strcmp0(status, "draft") == 0 || refuse(error, "only draft order lines can be edited");
}

static gboolean
owned_name(const gchar *name)
{
	return g_strcmp0(name, "purchase_order") == 0 || g_strcmp0(name, "purchase_order_line") == 0 ||
		g_strcmp0(name, "goods_receipt") == 0 || g_strcmp0(name, "goods_receipt_line") == 0 ||
		g_strcmp0(name, "inventory_cost_layer") == 0 || g_strcmp0(name, "sales_order") == 0 ||
		g_strcmp0(name, "sales_order_line") == 0 || g_strcmp0(name, "fulfillment") == 0;
}

gboolean
venture_goods_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name;
	VentureInventoryService *inventory;
	if (record == NULL || database == NULL)
		return TRUE;
	name = venture_entity_get_entity_name(record);
	if (!owned_name(name) && g_strcmp0(name, "inventory_txn") != 0)
		return TRUE;
	inventory = venture_inventory_service_get(database);
	if (inventory->writing == record)
		return TRUE;
	if (g_object_get_data(G_OBJECT(database), "venture-purchasing-writing") == record ||
		g_object_get_data(G_OBJECT(database), "venture-sales-order-writing") == record)
		return TRUE;
	/* A movement row is the stock engine's history too: deleting it would
	 * leave the engine holding stock the rows no longer show. A count is
	 * corrected by another count. */
	if (g_strcmp0(name, "inventory_txn") == 0)
		return !removal || refuse(error, "stock movements are kept; correct the stock with a count");
	if (g_strcmp0(name, "inventory_cost_layer") == 0 || g_strcmp0(name, "fulfillment") == 0 ||
		g_strcmp0(name, "goods_receipt") == 0 || g_strcmp0(name, "goods_receipt_line") == 0)
		return refuse(error, "inventory evidence is owned by VentureInventoryService");
	if (removal)
		return refuse(error, "goods history cannot be removed");
	if (g_strcmp0(name, "sales_order_line") == 0 || g_strcmp0(name, "purchase_order_line") == 0)
	{
		gboolean sales = g_strcmp0(name, "sales_order_line") == 0;
		GType parent_type = sales ? VENTURE_TYPE_SALES_ORDER : VENTURE_TYPE_PURCHASE_ORDER;
		const gchar *parent_field = sales ? "sales-order-id" : "purchase-order-id";
		/* Check both parents so moving a line cannot erase approved or fulfilled evidence. */
		if (!line_parent_is_draft(database, record, parent_type, parent_field, error))
			return FALSE;
		if (venture_entity_is_persisted(record))
		{
			g_autoptr(VentureEntity) stored = venture_database_get(database,
				G_OBJECT_TYPE(record), venture_entity_get_id(record), error);
			if (stored == NULL || !line_parent_is_draft(database, stored, parent_type, parent_field, error))
				return FALSE;
		}
	}
	if (g_strcmp0(name, "purchase_order") == 0 || g_strcmp0(name, "sales_order") == 0)
	{
		g_autofree gchar *status = NULL;
		g_object_get(record, "status", &status, NULL);
		if (venture_entity_is_persisted(record))
		{
			g_autoptr(VentureEntity) stored = venture_database_get(database,
				G_OBJECT_TYPE(record), venture_entity_get_id(record), NULL);
			g_autofree gchar *stored_status = NULL;
			if (stored != NULL)
				g_object_get(stored, "status", &stored_status, NULL);
			if (stored_status != NULL && g_strcmp0(stored_status, "draft") != 0)
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					g_strcmp0(name, "purchase_order") == 0 ?
					"VenturePurchasingService: purchase orders are owned by VenturePurchasingService" :
					"VentureSalesOrderService: sales orders are owned by VentureSalesOrderService");
				return FALSE;
			}
		}
		if (status != NULL && g_strcmp0(status, "draft") != 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				g_strcmp0(name, "purchase_order") == 0 ?
				"VenturePurchasingService: purchase orders are owned by VenturePurchasingService" :
				"VentureSalesOrderService: sales orders are owned by VentureSalesOrderService");
			return FALSE;
		}
	}
	if (g_strcmp0(name, "sales_order_line") == 0)
	{
		g_autoptr(VentureEntity) stored = venture_entity_is_persisted(record)
			? venture_database_get(database, G_OBJECT_TYPE(record), venture_entity_get_id(record), error) : NULL;
		gint64 allocated = 0, fulfilled = 0, invoiced = 0, stored_a = 0, stored_f = 0, stored_i = 0;
		if (venture_entity_is_persisted(record) && stored == NULL)
			return FALSE;
		g_object_get(record, "allocated-qty", &allocated, "fulfilled-qty", &fulfilled, "invoiced-qty", &invoiced, NULL);
		if (stored != NULL)
			g_object_get(stored, "allocated-qty", &stored_a, "fulfilled-qty", &stored_f, "invoiced-qty", &stored_i, NULL);
		if (allocated != stored_a || fulfilled != stored_f || invoiced != stored_i)
			return refuse(error, "fulfillment counters are owned by VentureSalesOrderService");
	}
	return TRUE;
}

gboolean
venture_goods_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureInventoryService *self;
	*handled = FALSE;
	if (record == NULL)
		return TRUE;
	self = venture_inventory_service_get(database);
	if (self->writing == record)
		return TRUE;
	if (VENTURE_IS_INVENTORY_TXN(record))
	{
		*handled = TRUE;
		return save_count(self, record, actor, error);
	}
	if (VENTURE_IS_SALE(record) &&
		venture_inventory_product_is_stocked(database, get_id(record, "product-id")))
	{
		if (venture_entity_is_persisted(record))
			return TRUE;
		*handled = TRUE;
		if (!save_owned(self, record, actor, error) ||
			!venture_inventory_service_issue_sale(self, record, actor, error))
			return FALSE;
		return TRUE;
	}
	return TRUE;
}


static gint64
report_org(VentureContext *context, JsonObject *options)
{
	gint64 id = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	return id != 0 ? id : venture_context_get_default_organization_id(context);
}

static gint64
received_qty(VentureDatabase *db, gint64 po_line_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_GOODS_RECEIPT_LINE);
	g_autoptr(GPtrArray) rows = NULL;
	gint64 sum = 0;
	guint i;
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "purchase-order-line-id", VENTURE_FILTER_OP_EQ, po_line_id, NULL))
		return 0;
	rows = venture_database_find(db, query, NULL);
	if (rows == NULL)
		return 0;
	for (i = 0; i < rows->len; i++)
	{
		gint64 quantity = 0, returned = 0;
		g_object_get(g_ptr_array_index(rows, i), "quantity", &quantity, "returned-qty", &returned, NULL);
		sum += quantity - returned;
	}
	return sum;
}

/*
 * What approved orders still commit, per order in its own currency. The
 * totals are per currency too -- the book currency under "committed", any
 * other under "committed_<CODE>" -- because an organization that buys in
 * gold and in tickets has two commitments, not one number in neither. It
 * used to start from zero dollars and refuse the first order in anything
 * else.
 */
static VentureReportResult *
committed_spend_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PURCHASE_ORDER);
	g_autoptr(GPtrArray) orders = NULL;
	g_autoptr(GPtrArray) totals = venture_money_totals_new();
	g_autoptr(VentureReportResult) result = venture_report_result_new("Committed spend", period);
	g_autofree gchar *book = NULL;
	VentureDatabase *db = venture_context_get_database(context);
	gint64 org = report_org(context, options);
	guint i;
	book = venture_posting_service_book_currency(venture_database_get_posting_service(db), org, error);
	if (book == NULL)
		return NULL;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	orders = venture_database_find(db, query, error);
	if (orders == NULL)
		return NULL;
	venture_report_result_add_column(result, "number", "Number", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "committed", "Committed", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < orders->len; i++)
	{
		g_autoptr(VentureQuery) lines_q = venture_query_new(VENTURE_TYPE_PURCHASE_ORDER_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(VentureMoney) committed = NULL;
		g_autofree gchar *number = NULL;
		g_autofree gchar *status = NULL;
		g_autofree gchar *currency = NULL;
		guint j;
		g_object_get(g_ptr_array_index(orders, i), "number", &number, "status", &status,
			"currency", &currency, NULL);
		if (g_strcmp0(status, "draft") == 0 || g_strcmp0(status, "cancelled") == 0)
			continue;
		committed = venture_money_new_zero(currency != NULL && *currency != '\0' ? currency : book);
		venture_query_set_limit(lines_q, 0);
		if (!venture_query_add_filter_int(lines_q, "purchase-order-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(g_ptr_array_index(orders, i)), error))
			return NULL;
		lines = venture_database_find(db, lines_q, error);
		if (lines == NULL)
			return NULL;
		for (j = 0; j < lines->len; j++)
		{
			g_autoptr(VentureMoney) unit = NULL;
			g_autoptr(VentureMoney) slice = NULL;
			gint64 ordered = 0, open;
			g_object_get(g_ptr_array_index(lines, j), "quantity", &ordered, "unit-price", &unit, NULL);
			open = ordered - received_qty(db, venture_entity_get_id(g_ptr_array_index(lines, j)));
			if (open <= 0 || unit == NULL)
				continue;
			slice = venture_money_multiply_int(unit, open, error);
			if (slice == NULL)
				return NULL;
			{
				VentureMoney *next = venture_money_add(committed, slice, error);
				if (next == NULL)
					return NULL;
				venture_money_free(committed);
				committed = next;
			}
		}
		if (venture_money_is_zero(committed) && g_strcmp0(status, "received") == 0)
			continue;
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "number", number);
		venture_report_result_set_text(result, "status", status);
		venture_report_result_set_money(result, "committed", committed);
		if (!venture_money_totals_add(totals, committed, error))
			return NULL;
	}
	venture_money_totals_sort(totals, book);
	{
		const VentureMoney *in_book = venture_money_totals_lookup(totals, book);
		g_autoptr(VentureMoney) zero = venture_money_new_zero(book);
		venture_report_result_add_metric(result, venture_metric_new_money("committed", "Committed spend",
			in_book != NULL ? in_book : zero));
	}
	for (i = 0; i < totals->len; i++)
	{
		const VentureMoney *total = g_ptr_array_index(totals, i);
		g_autofree gchar *key = NULL;
		g_autofree gchar *label = NULL;
		if (g_strcmp0(total->currency, book) == 0)
			continue;
		key = g_strdup_printf("committed_%s", total->currency);
		label = g_strdup_printf("Committed spend (%s)", total->currency);
		venture_report_result_add_metric(result, venture_metric_new_money(key, label, total));
	}
	return g_steal_pointer(&result);
}

static VentureReportResult *
reorder_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Reorder worklist", period);
	VentureInventoryService *service = venture_inventory_service_get(venture_context_get_database(context));
	guint i;
	venture_query_set_organization(query, report_org(context, options));
	venture_query_set_limit(query, 0);
	items = venture_database_find(venture_context_get_database(context), query, error);
	if (items == NULL)
		return NULL;
	venture_report_result_add_column(result, "sku", "SKU", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "on_hand", "On hand", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "reorder_point", "Reorder at", VENTURE_REPORT_COLUMN_NUMBER);
	for (i = 0; i < items->len; i++)
	{
		g_autofree gchar *sku = NULL;
		gint64 reorder = 0;
		gint64 on_hand;
		g_object_get(g_ptr_array_index(items, i), "sku", &sku, "reorder-point", &reorder, NULL);
		on_hand = venture_inventory_service_on_hand(service, venture_entity_get_id(g_ptr_array_index(items, i)),
			NULL, error);
		if (error != NULL && *error != NULL)
			return NULL;
		if (on_hand > reorder)
			continue;
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "sku", sku);
		venture_report_result_set_number(result, "on_hand", (gdouble)on_hand);
		venture_report_result_set_number(result, "reorder_point", (gdouble)reorder);
	}
	return g_steal_pointer(&result);
}

/* How a currency's inventory reaches the books on @when, in words: the
 * reader reconciling this report to the inventory account needs to know
 * which figures are in it at all. */
static const gchar *
route_label(VentureBookRoute route)
{
	switch (route)
	{
	case VENTURE_BOOK_ROUTE_BOOK:
		return "book currency";
	case VENTURE_BOOK_ROUTE_CONVERTED:
		return "valued into the book currency";
	case VENTURE_BOOK_ROUTE_SEPARATE:
		return "separate book";
	case VENTURE_BOOK_ROUTE_MEMO:
		return "memo, not posted";
	default:
		return NULL;
	}
}

/*
 * One row per currency the remaining layers cost. The first (the book
 * currency) is also the "valuation" metric, kept under that key for every
 * dashboard that reads it; each other currency is "valuation_<CODE>".
 */
static VentureReportResult *
valuation_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Inventory valuation", period);
	VentureDatabase *db = venture_context_get_database(context);
	GDateTime *as_of = period != NULL ? venture_date_range_get_end(period) : NULL;
	gint64 org = report_org(context, options);
	guint i;
	values = venture_inventory_service_valuation(venture_inventory_service_get(db), org, as_of, error);
	if (values == NULL)
		return NULL;
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "valuation", "FIFO valuation", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "books", "In the books", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < values->len; i++)
	{
		const VentureMoney *value = g_ptr_array_index(values, i);
		const gchar *currency = venture_money_get_currency(value);
		VentureBookRoute route = VENTURE_BOOK_ROUTE_BOOK;
		g_autofree gchar *key = i == 0 ? g_strdup("valuation") : g_strdup_printf("valuation_%s", currency);
		g_autofree gchar *label = i == 0 ? g_strdup("FIFO valuation")
			: g_strdup_printf("FIFO valuation (%s)", currency);
		if (!venture_posting_service_route_currency(venture_database_get_posting_service(db), org,
			currency, as_of, &route, NULL, error))
			return NULL;
		venture_report_result_add_metric(result, venture_metric_new_money(key, label, value));
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "currency", currency);
		venture_report_result_set_money(result, "valuation", value);
		venture_report_result_set_text(result, "books", route_label(route));
	}
	return g_steal_pointer(&result);
}

void
venture_goods_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "committed_spend",
		"Committed spend", "Open purchase orders valued at remaining unordered quantity.",
		committed_spend_report)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "reorder_worklist",
		"Reorder worklist", "Inventory items at or below their reorder point.",
		reorder_report)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "inventory_valuation",
		"Inventory valuation", "FIFO cost layers remaining, one row per currency, each saying how it reaches the books.",
		valuation_report)));
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_inventory_service_receive(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	gboolean result;
	g_autofree gchar *unit_cost_text = NULL;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (subject == NULL)
		return FALSE;
	unit_cost_text = unit_cost != NULL ? venture_money_to_string(unit_cost) : NULL;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "inventory_item_id", g_variant_new_int64((gint64)inventory_item_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64((gint64)quantity));
	g_variant_builder_add(&arguments, "{sv}", "unit_cost", g_variant_new_maybe(G_VARIANT_TYPE_STRING, unit_cost_text != NULL ? g_variant_new_string(unit_cost_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "receipt_line_id", g_variant_new_int64((gint64)receipt_line_id));
	g_variant_builder_add(&arguments, "{sv}", "reference", g_variant_new_maybe(G_VARIANT_TYPE_STRING, reference != NULL ? g_variant_new_string(reference) : NULL));
	operation = venture_accounting_operation_begin(db, "inventory-receive", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_inventory_service_receive_impl(self, inventory_item_id, quantity, unit_cost, date, receipt_line_id, reference, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_inventory_service_issue(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *source_type, gint64 source_id,
	const VentureActor *actor, GPtrArray **costs, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	gboolean result;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (subject == NULL)
		return FALSE;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "inventory_item_id", g_variant_new_int64((gint64)inventory_item_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64((gint64)quantity));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "source_type", g_variant_new_maybe(G_VARIANT_TYPE_STRING, source_type != NULL ? g_variant_new_string(source_type) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "source_id", g_variant_new_int64((gint64)source_id));
	operation = venture_accounting_operation_begin(db, "inventory-issue", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_inventory_service_issue_impl(self, inventory_item_id, quantity, date, source_type, source_id, actor, costs, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal;
 * the writes are one transaction of their own (joining the caller's). */
gboolean
venture_inventory_service_receive_from(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *total_cost, GDateTime *date, const gchar *reference,
	gint64 credit_account_id, const VentureActor *actor, VentureEntity **out_txn, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autofree gchar *cost_text = NULL;
	g_autofree gchar *date_text = NULL;
	VentureDatabase *db;
	GVariantBuilder arguments;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (out_txn != NULL)
		*out_txn = NULL;
	db = self->database;
	if (db == NULL)
		return refuse(error, "the database is unavailable");
	subject = venture_database_get(db, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (subject == NULL)
		return FALSE;
	cost_text = total_cost != NULL ? venture_money_to_string(total_cost) : NULL;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "inventory_item_id", g_variant_new_int64(inventory_item_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64(quantity));
	g_variant_builder_add(&arguments, "{sv}", "total_cost", g_variant_new_maybe(G_VARIANT_TYPE_STRING, cost_text != NULL ? g_variant_new_string(cost_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "reference", g_variant_new_maybe(G_VARIANT_TYPE_STRING, reference != NULL ? g_variant_new_string(reference) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "credit_account_id", g_variant_new_int64(credit_account_id));
	operation = venture_accounting_operation_begin(db, "inventory-receive-from", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	if (!venture_database_begin(db, error))
		return FALSE;
	if (!receive_from_impl(self, inventory_item_id, quantity, total_cost, date, reference,
		credit_account_id, actor, &txn, error))
	{
		venture_database_rollback(db);
		return FALSE;
	}
	if (!venture_database_commit(db, error) || !venture_accounting_operation_finish(operation, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	return TRUE;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal;
 * the writes are one transaction of their own (joining the caller's). */
gboolean
venture_inventory_service_issue_to(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *reference, gint64 debit_account_id,
	VentureInventoryTxnKind kind, const VentureActor *actor, VentureEntity **out_txn,
	GPtrArray **out_costs, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autofree gchar *date_text = NULL;
	VentureDatabase *db;
	GVariantBuilder arguments;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (out_txn != NULL)
		*out_txn = NULL;
	if (out_costs != NULL)
		*out_costs = NULL;
	db = self->database;
	if (db == NULL)
		return refuse(error, "the database is unavailable");
	subject = venture_database_get(db, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (subject == NULL)
		return FALSE;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "inventory_item_id", g_variant_new_int64(inventory_item_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64(quantity));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "reference", g_variant_new_maybe(G_VARIANT_TYPE_STRING, reference != NULL ? g_variant_new_string(reference) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "debit_account_id", g_variant_new_int64(debit_account_id));
	g_variant_builder_add(&arguments, "{sv}", "kind", g_variant_new_int32((gint32)kind));
	operation = venture_accounting_operation_begin(db, "inventory-issue-to", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	if (!venture_database_begin(db, error))
		return FALSE;
	if (!issue_to_impl(self, inventory_item_id, quantity, date, reference, debit_account_id, kind,
		actor, &txn, &costs, error))
	{
		venture_database_rollback(db);
		return FALSE;
	}
	if (!venture_database_commit(db, error) || !venture_accounting_operation_finish(operation, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	if (out_costs != NULL)
		*out_costs = g_steal_pointer(&costs);
	return TRUE;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_inventory_service_restore(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	gboolean result;
	g_autofree gchar *unit_cost_text = NULL;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (subject == NULL)
		return FALSE;
	unit_cost_text = unit_cost != NULL ? venture_money_to_string(unit_cost) : NULL;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "inventory_item_id", g_variant_new_int64((gint64)inventory_item_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64((gint64)quantity));
	g_variant_builder_add(&arguments, "{sv}", "unit_cost", g_variant_new_maybe(G_VARIANT_TYPE_STRING, unit_cost_text != NULL ? g_variant_new_string(unit_cost_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	g_variant_builder_add(&arguments, "{sv}", "receipt_line_id", g_variant_new_int64((gint64)receipt_line_id));
	g_variant_builder_add(&arguments, "{sv}", "reference", g_variant_new_maybe(G_VARIANT_TYPE_STRING, reference != NULL ? g_variant_new_string(reference) : NULL));
	operation = venture_accounting_operation_begin(db, "inventory-restore", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_inventory_service_restore_impl(self, inventory_item_id, quantity, unit_cost, date, receipt_line_id, reference, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_inventory_service_transfer(VentureInventoryService *self, gint64 from_item_id,
	gint64 to_item_id, gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	gboolean result;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_INVENTORY_ITEM, from_item_id, error);
	if (subject == NULL)
		return FALSE;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "from_item_id", g_variant_new_int64((gint64)from_item_id));
	g_variant_builder_add(&arguments, "{sv}", "to_item_id", g_variant_new_int64((gint64)to_item_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64((gint64)quantity));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	operation = venture_accounting_operation_begin(db, "inventory-transfer", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_inventory_service_transfer_impl(self, from_item_id, to_item_id, quantity, date, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_inventory_service_issue_sale(VentureInventoryService *self, VentureEntity *sale,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(sale), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(db, "inventory-issue-sale", VENTURE_ENTITY(sale), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(sale)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_inventory_service_issue_sale_impl(self, sale, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_inventory_service_issue_invoice(VentureInventoryService *self, VentureEntity *invoice,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	gboolean result;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	g_return_val_if_fail(VENTURE_IS_ENTITY(invoice), FALSE);
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	operation = venture_accounting_operation_begin(db, "inventory-issue-invoice", VENTURE_ENTITY(invoice), NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(VENTURE_ENTITY(invoice)), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_inventory_service_issue_invoice_impl(self, invoice, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}
