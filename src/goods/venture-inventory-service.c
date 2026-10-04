/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
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

gint64
venture_inventory_service_on_hand(VentureInventoryService *self, gint64 inventory_item_id,
	GDateTime *as_of, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	gint64 total = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), 0);
	rows = find_rows(self, VENTURE_TYPE_INVENTORY_TXN, "inventory-item-id", inventory_item_id, error);
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

static gboolean
item_allows_negative(VentureInventoryService *self, gint64 item_id)
{
	g_autoptr(VentureEntity) item = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, item_id, NULL);
	gboolean allow = FALSE;
	if (item == NULL || g_object_class_find_property(G_OBJECT_GET_CLASS(item), "allow-negative") == NULL)
		return FALSE;
	g_object_get(item, "allow-negative", &allow, NULL);
	return allow;
}

static gboolean
guard_stock(VentureInventoryService *self, gint64 item_id, gint64 delta, GError **error)
{
	gint64 on_hand;
	if (delta >= 0 || item_allows_negative(self, item_id))
		return TRUE;
	on_hand = venture_inventory_service_on_hand(self, item_id, NULL, error);
	if (error != NULL && *error != NULL)
		return FALSE;
	if (on_hand + delta < 0)
		return refuse(error, "negative stock is refused");
	return TRUE;
}

static GPtrArray *
layers_for(VentureInventoryService *self, gint64 item_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "inventory-item-id", VENTURE_FILTER_OP_EQ, item_id, error))
		return NULL;
	venture_query_add_order(query, "received-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}

/* The lot a layer belongs to: its arrival movement, or itself (negated,
 * so it cannot meet a movement id) when it predates lots. */
static gint64
layer_lot(VentureEntity *layer)
{
	gint64 lot = get_id(layer, "lot-txn-id");
	return lot > 0 ? lot : -venture_entity_get_id(layer);
}

/*
 * Takes up to @quantity units from @item_id's cost layers, first in first
 * out, adding what they cost into @costs (per currency; may be NULL) and
 * leaving the units no layer covered in *@out_short.
 *
 * FIFO walks lots, not layers. A lot's layers in one currency sum to the
 * lot's units; a lot bought in two currencies has layers in each, and
 * every unit taken out of it is taken out of every currency -- which is
 * what keeps a made unit's GOLD and TICKET cost leaving together and each
 * leaving exactly. A lot has as many units as its first currency's layers
 * hold; a sibling currency short of that (never written so by this
 * service) gives what it has. A single-currency lot is the old behaviour:
 * its layers are taken in order.
 */
static gboolean
consume_layers(VentureInventoryService *self, gint64 item_id, gint64 quantity, const VentureActor *actor,
	GPtrArray *costs, gint64 *out_short, GError **error)
{
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GHashTable) seen = NULL;
	gint64 need = quantity;
	guint i;
	*out_short = quantity;
	layers = layers_for(self, item_id, error);
	if (layers == NULL)
		return FALSE;
	seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < layers->len && need > 0; i++)
	{
		VentureEntity *first = g_ptr_array_index(layers, i);
		g_autoptr(VentureMoney) lead_unit = NULL;
		gint64 lot = layer_lot(first);
		gint64 lot_remaining = 0;
		gint64 take;
		guint j;
		if (g_hash_table_contains(seen, &lot))
			continue;
		g_hash_table_add(seen, g_memdup2(&lot, sizeof lot));
		g_object_get(first, "unit-cost", &lead_unit, NULL);
		if (lead_unit == NULL)
			return refuse(error, "a cost layer has no unit cost");
		/* The lot's units: its first currency's remaining. */
		for (j = i; j < layers->len; j++)
		{
			VentureEntity *member = g_ptr_array_index(layers, j);
			g_autoptr(VentureMoney) unit = NULL;
			if (layer_lot(member) != lot)
				continue;
			g_object_get(member, "unit-cost", &unit, NULL);
			if (unit != NULL && g_strcmp0(venture_money_get_currency(unit),
				venture_money_get_currency(lead_unit)) == 0)
				lot_remaining += get_int(member, "remaining-qty");
		}
		if (lot_remaining <= 0)
			continue;
		take = lot_remaining < need ? lot_remaining : need;
		/* Every currency of the lot gives up @take units, each from its
		 * own layers in order. @left is per currency, reset when a new
		 * currency is met; members of one lot were written together, so
		 * a currency's layers are met in the order they were made. */
		{
			g_autoptr(GHashTable) left = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			for (j = i; j < layers->len; j++)
			{
				VentureEntity *member = g_ptr_array_index(layers, j);
				g_autoptr(VentureMoney) unit = NULL;
				gint64 *still;
				gint64 remaining, part;
				if (layer_lot(member) != lot)
					continue;
				g_object_get(member, "unit-cost", &unit, NULL);
				if (unit == NULL)
					return refuse(error, "a cost layer has no unit cost");
				still = g_hash_table_lookup(left, venture_money_get_currency(unit));
				if (still == NULL)
				{
					still = g_new(gint64, 1);
					*still = take;
					g_hash_table_insert(left, g_strdup(venture_money_get_currency(unit)), still);
				}
				remaining = get_int(member, "remaining-qty");
				if (*still <= 0 || remaining <= 0)
					continue;
				part = remaining < *still ? remaining : *still;
				if (costs != NULL)
				{
					g_autoptr(VentureMoney) slice = venture_money_multiply_int(unit, part, error);
					if (slice == NULL || !venture_money_totals_add(costs, slice, error))
						return FALSE;
				}
				g_object_set(member, "remaining-qty", remaining - part, NULL);
				if (!save_owned(self, member, actor, error))
					return FALSE;
				*still -= part;
			}
		}
		need -= take;
	}
	*out_short = need;
	return TRUE;
}

static gboolean
consume_fifo(VentureInventoryService *self, gint64 item_id, gint64 quantity, const VentureActor *actor,
	GPtrArray *costs, GError **error)
{
	gint64 short_by = 0;
	if (!consume_layers(self, item_id, quantity, actor, costs, &short_by, error))
		return FALSE;
	if (short_by > 0 && !item_allows_negative(self, item_id))
		return refuse(error, "negative stock is refused");
	return TRUE;
}

static gboolean
restore_fifo(VentureInventoryService *self, gint64 item_id, gint64 receipt_line_id, gint64 quantity,
	const VentureMoney *unit_cost, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) layers = NULL;
	gint64 need = quantity;
	guint i;
	(void)unit_cost;
	(void)date;
	layers = layers_for(self, item_id, error);
	if (layers == NULL)
		return FALSE;
	for (i = 0; i < layers->len && need > 0; i++)
	{
		VentureEntity *layer = g_ptr_array_index(layers, i);
		gint64 remaining, take;
		if (receipt_line_id != 0 && get_id(layer, "goods-receipt-line-id") != receipt_line_id)
			continue;
		remaining = get_int(layer, "remaining-qty");
		if (remaining <= 0)
			continue;
		take = remaining < need ? remaining : need;
		g_object_set(layer, "remaining-qty", remaining - take, NULL);
		if (!save_owned(self, layer, actor, error))
			return FALSE;
		need -= take;
	}
	if (need > 0)
		return refuse(error, "negative stock is refused");
	return TRUE;
}

static VentureEntity *
write_txn(VentureInventoryService *self, gint64 item_id, gint64 quantity, gint kind,
	const VentureMoney *unit_cost, const gchar *notes, GDateTime *date, const gchar *reference,
	gint64 sale_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInventoryTxn) txn = venture_inventory_txn_new();
	g_autoptr(VentureEntity) item = NULL;
	item = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, item_id, error);
	if (item == NULL)
		return NULL;
	g_object_set(txn, "inventory-item-id", item_id, "kind", kind, "quantity", quantity,
		"unit-cost", unit_cost, "occurred-at", date, "reference", reference, "sale-id", sale_id, NULL);
	if (notes != NULL)
		g_object_set(txn, "notes", notes, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(txn), venture_entity_get_organization_id(item));
	if (!save_owned(self, VENTURE_ENTITY(txn), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&txn));
}

static gboolean
venture_inventory_service_receive_impl(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureInventoryCostLayer) layer = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory, grni;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (quantity <= 0 || unit_cost == NULL)
		return refuse(error, "receipts need a positive quantity and unit cost");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	txn = write_txn(self, inventory_item_id, quantity, VENTURE_INVENTORY_TXN_KIND_PURCHASE,
		unit_cost, NULL, when, reference, 0, actor, error);
	if (txn == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(txn);
	layer = venture_inventory_cost_layer_new();
	g_object_set(layer, "inventory-item-id", inventory_item_id, "goods-receipt-line-id", receipt_line_id,
		"received-at", when, "original-qty", quantity, "remaining-qty", quantity, "unit-cost", unit_cost,
		"lot-txn-id", venture_entity_get_id(txn), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(layer), org);
	if (!save_owned(self, VENTURE_ENTITY(layer), actor, error))
		return FALSE;
	total = venture_money_multiply_int(unit_cost, quantity, error);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	grni = account_code(self, org, "grni", "2010", "Goods received not invoiced",
		VENTURE_ACCOUNT_KIND_LIABILITY, error);
	if (total == NULL || inventory == 0 || grni == 0)
		return FALSE;
	/* GRNI is a liability; a credit increases it until the matched bill clears it. */
	return post_pair(self, org, txn, inventory, grni, total, "Inventory receipt", when, actor, error);
}

static gboolean
venture_inventory_service_issue_impl(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *source_type, gint64 source_id,
	const VentureActor *actor, GPtrArray **out_costs, GError **error)
{
	g_autoptr(GPtrArray) costs = venture_money_totals_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory, cogs_id;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (out_costs != NULL)
		*out_costs = NULL;
	if (quantity <= 0)
		return refuse(error, "issues need a positive quantity");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!guard_stock(self, inventory_item_id, -quantity, error))
		return FALSE;
	if (!consume_fifo(self, inventory_item_id, quantity, actor, costs, error))
		return FALSE;
	/* The layers may have cost several currencies; each is kept apart
	 * and each posts by its own treatment below. */
	if (!movement_unit(costs, quantity, &unit, &notes, error))
		return FALSE;
	txn = write_txn(self, inventory_item_id, -quantity, VENTURE_INVENTORY_TXN_KIND_SALE,
		unit, notes, when, source_type, g_strcmp0(source_type, "sale") == 0 ? source_id : 0, actor, error);
	if (txn == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(txn);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	cogs_id = account_code(self, org, "cogs", "5000", "Cost of goods sold", VENTURE_ACCOUNT_KIND_EXPENSE, error);
	if (inventory == 0 || cogs_id == 0)
		return FALSE;
	if (!post_costs(self, org, txn, cogs_id, inventory, costs, "Inventory issue", when, actor, error))
		return FALSE;
	if (out_costs != NULL)
		*out_costs = g_steal_pointer(&costs);
	return TRUE;
}

static gboolean
venture_inventory_service_restore_impl(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory, grni;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (quantity <= 0 || unit_cost == NULL)
		return refuse(error, "returns need a positive quantity and unit cost");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!guard_stock(self, inventory_item_id, -quantity, error))
		return FALSE;
	if (receipt_line_id != 0)
	{
		if (!restore_fifo(self, inventory_item_id, receipt_line_id, quantity, unit_cost, when, actor, error))
			return FALSE;
	}
	else if (!consume_fifo(self, inventory_item_id, quantity, actor, NULL, error))
		return FALSE;
	txn = write_txn(self, inventory_item_id, -quantity, VENTURE_INVENTORY_TXN_KIND_RETURN,
		unit_cost, NULL, when, reference, 0, actor, error);
	if (txn == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(txn);
	total = venture_money_multiply_int(unit_cost, quantity, error);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	grni = account_code(self, org, "grni", "2010", "Goods received not invoiced",
		VENTURE_ACCOUNT_KIND_LIABILITY, error);
	if (total == NULL || inventory == 0 || grni == 0)
		return FALSE;
	return post_pair(self, org, txn, grni, inventory, total, "Inventory return", when, actor, error);
}

/*
 * The currency a lot with no cost at all is carried in: the item's own
 * unit cost's, else its organization's book currency. It still needs a
 * layer -- consume_fifo() refuses to issue units that have none -- and a
 * zero never clashes with anything, because layers are totalled per
 * currency.
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

/* A new cost layer of @quantity units at @unit on @item_id, in @lot. */
static gboolean
add_layer(VentureInventoryService *self, gint64 item_id, gint64 organization_id, gint64 quantity,
	const VentureMoney *unit, gint64 lot, GDateTime *when, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInventoryCostLayer) layer = venture_inventory_cost_layer_new();
	g_object_set(layer, "inventory-item-id", item_id, "received-at", when,
		"original-qty", quantity, "remaining-qty", quantity, "unit-cost", unit,
		"lot-txn-id", lot, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(layer), organization_id);
	return save_owned(self, VENTURE_ENTITY(layer), actor, error);
}

/*
 * Gives @quantity units arriving on @item_id (by the movement @lot) the
 * cost @costs, one set of layers per currency that cost anything.
 *
 * Split exactly, never rounded: 10.00 over 3 units is one layer of 1 at
 * 3.34 and one of 2 at 3.33. A single layer at the rounded average would
 * leave a cent of cost behind (or invent one) every time, and cost of
 * goods sold would drift from what went in. Each currency is split on its
 * own, and all of them share @lot, so FIFO later takes every currency's
 * share of a unit together (consume_layers()).
 *
 * Zero totals make no layers when anything else cost something -- a
 * zero-cost GOLD input beside TICKET ones adds nothing to the unit. When
 * nothing cost anything the units still get one zero layer, in the first
 * zero total's currency, else the item's (zero_cost_currency()).
 */
static gboolean
add_lot_layers(VentureInventoryService *self, VentureEntity *item, gint64 quantity, GPtrArray *costs,
	gint64 lot, GDateTime *when, const VentureActor *actor, GError **error)
{
	gint64 item_id = venture_entity_get_id(item);
	gint64 organization_id = venture_entity_get_organization_id(item);
	gboolean made = FALSE;
	guint i;
	for (i = 0; costs != NULL && i < costs->len; i++)
	{
		const VentureMoney *total = g_ptr_array_index(costs, i);
		gint64 amount = venture_money_get_amount(total);
		gint64 base, over;
		const gchar *currency = venture_money_get_currency(total);
		guint8 exponent = venture_money_get_exponent(total);
		if (amount == 0)
			continue;
		base = amount / quantity;
		over = amount % quantity;
		/* A negative total (never made here) splits the same way with the
		 * remainder's sign; keep the "higher" layer the one that moves the
		 * sum toward the total. */
		if (over < 0)
		{
			base -= 1;
			over += quantity;
		}
		if (over > 0)
		{
			g_autoptr(VentureMoney) high = venture_money_new(base + 1, currency, exponent);
			if (!add_layer(self, item_id, organization_id, over, high, lot, when, actor, error))
				return FALSE;
		}
		if (quantity - over > 0)
		{
			g_autoptr(VentureMoney) low = venture_money_new(base, currency, exponent);
			if (!add_layer(self, item_id, organization_id, quantity - over, low, lot, when, actor, error))
				return FALSE;
		}
		made = TRUE;
	}
	if (!made)
	{
		g_autofree gchar *currency = (costs != NULL && costs->len > 0)
			? g_strdup(venture_money_get_currency(g_ptr_array_index(costs, 0)))
			: zero_cost_currency(self, item);
		g_autoptr(VentureMoney) zero = venture_money_new_zero(currency);
		if (!add_layer(self, item_id, organization_id, quantity, zero, lot, when, actor, error))
			return FALSE;
	}
	return TRUE;
}

/*
 * A receipt paid from @credit_id rather than accrued to goods received not
 * invoiced: stock bought outright at a venue, whose money left the venue's
 * cash the moment it was bought. Layers and FIFO are exactly a receipt's,
 * except that the cost is a total split over the units exactly (one layer
 * a minor unit higher and one at the floor, in one lot), because 100.00
 * paid for 7 units has no exact unit cost and a rounded one would leave a
 * cent behind on every flip.
 */
static gboolean
receive_from_impl(VentureInventoryService *self, gint64 inventory_item_id, gint64 quantity,
	const VentureMoney *total_cost, GDateTime *date, const gchar *reference, gint64 credit_id,
	const VentureActor *actor, VentureEntity **out_txn, GError **error)
{
	g_autoptr(GPtrArray) costs = venture_money_totals_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory;
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
	 * anyway, but only after the layers were written. */
	credit = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, credit_id, error);
	if (credit == NULL)
		return FALSE;
	if (venture_entity_get_organization_id(credit) != org)
		return refuse(error, "the account paid from belongs to another organization");
	if (!venture_money_totals_add(costs, total_cost, error))
		return FALSE;
	if (!movement_unit(costs, quantity, &unit, &notes, error))
		return FALSE;
	txn = write_txn(self, inventory_item_id, quantity, VENTURE_INVENTORY_TXN_KIND_PURCHASE,
		unit, notes, when, reference, 0, actor, error);
	if (txn == NULL)
		return FALSE;
	if (!add_lot_layers(self, item, quantity, costs, venture_entity_get_id(txn), when, actor, error))
		return FALSE;
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	if (inventory == 0)
		return FALSE;
	/* Dr inventory, Cr the account paid from. GRNI is not touched: there
	 * is no bill to come that would clear it. */
	if (!post_pair(self, org, txn, inventory, credit_id, total_cost, "Inventory bought", when, actor, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	return TRUE;
}

/*
 * An issue whose cost goes to @debit_id rather than cost of goods sold:
 * stock leaving for an arbitrage position (sold at a venue, or written off
 * when the trade is abandoned), whose profit or loss is decided when the
 * position closes. FIFO, the lots and the per-currency posting are an
 * issue's, unchanged.
 */
static gboolean
issue_to_impl(VentureInventoryService *self, gint64 inventory_item_id, gint64 quantity,
	GDateTime *date, const gchar *reference, gint64 debit_id, VentureInventoryTxnKind kind,
	const VentureActor *actor, VentureEntity **out_txn, GPtrArray **out_costs, GError **error)
{
	g_autoptr(GPtrArray) costs = venture_money_totals_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureEntity) debit = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory;
	if (quantity <= 0)
		return refuse(error, "issues need a positive quantity");
	if (kind != VENTURE_INVENTORY_TXN_KIND_SALE && kind != VENTURE_INVENTORY_TXN_KIND_WRITE_OFF)
		return refuse(error, "an issue to an account is a sale or a write-off");
	if (debit_id <= 0)
		return refuse(error, "an issue to an account needs the account");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!guard_stock(self, inventory_item_id, -quantity, error))
		return FALSE;
	if (!consume_fifo(self, inventory_item_id, quantity, actor, costs, error))
		return FALSE;
	if (!movement_unit(costs, quantity, &unit, &notes, error))
		return FALSE;
	txn = write_txn(self, inventory_item_id, -quantity, kind, unit, notes, when, reference, 0, actor, error);
	if (txn == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(txn);
	debit = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, debit_id, error);
	if (debit == NULL)
		return FALSE;
	if (venture_entity_get_organization_id(debit) != org)
		return refuse(error, "the account issued to belongs to another organization");
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	if (inventory == 0)
		return FALSE;
	if (!post_costs(self, org, txn, debit_id, inventory, costs,
		kind == VENTURE_INVENTORY_TXN_KIND_WRITE_OFF ? "Inventory written off" : "Inventory issued",
		when, actor, error))
		return FALSE;
	if (out_txn != NULL)
		*out_txn = g_steal_pointer(&txn);
	if (out_costs != NULL)
		*out_costs = g_steal_pointer(&costs);
	return TRUE;
}

static gboolean
venture_inventory_service_transfer_impl(VentureInventoryService *self, gint64 from_item_id,
	gint64 to_item_id, gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) costs = venture_money_totals_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autoptr(VentureEntity) out = NULL;
	g_autoptr(VentureEntity) in = NULL;
	g_autoptr(VentureEntity) destination = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (quantity <= 0 || from_item_id == to_item_id)
		return refuse(error, "transfers move a positive quantity between two locations");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!guard_stock(self, from_item_id, -quantity, error))
		return FALSE;
	if (!consume_fifo(self, from_item_id, quantity, actor, costs, error))
		return FALSE;
	if (!movement_unit(costs, quantity, &unit, &notes, error))
		return FALSE;
	out = write_txn(self, from_item_id, -quantity, VENTURE_INVENTORY_TXN_KIND_TRANSFER,
		unit, notes, when, "transfer", 0, actor, error);
	in = write_txn(self, to_item_id, quantity, VENTURE_INVENTORY_TXN_KIND_TRANSFER,
		unit, notes, when, "transfer", 0, actor, error);
	if (out == NULL || in == NULL)
		return FALSE;
	destination = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, to_item_id, error);
	if (destination == NULL)
		return FALSE;
	/* The units keep what they cost, in every currency and exactly: a
	 * lot split at a rounded average would drift by a minor unit per
	 * move. */
	return add_lot_layers(self, destination, quantity, costs, venture_entity_get_id(in),
		when, actor, error);
}

static gboolean
produce_body(VentureInventoryService *self, const VentureInventoryDraw *draws, guint n_draws,
	gint64 output_item_id, gint64 output_quantity, GDateTime *when, const gchar *reference,
	const VentureActor *actor, VentureEntity **out_txn, GPtrArray **out_costs, GError **error)
{
	g_autoptr(GPtrArray) total = venture_money_totals_new();
	g_autoptr(VentureMoney) unit = NULL;
	g_autofree gchar *notes = NULL;
	g_autoptr(VentureEntity) output = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	gboolean costed;
	gboolean consumed = FALSE;
	guint i;
	/* Cost layers belong to the goods module. Without it there is no
	 * FIFO to consume or to add to, and a craft moves quantities only --
	 * exactly what every other stock movement does on such an install. */
	costed = venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"inventory_cost_layer") != G_TYPE_INVALID;
	output = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, output_item_id, error);
	if (output == NULL)
		return FALSE;

	/* --- What goes in --- */
	for (i = 0; i < n_draws; i++)
	{
		g_autoptr(GPtrArray) cost = venture_money_totals_new();
		g_autoptr(VentureMoney) each = NULL;
		g_autofree gchar *each_notes = NULL;
		g_autoptr(VentureEntity) out = NULL;
		gint64 uncosted = 0;
		guint j;
		if (!guard_stock(self, draws[i].inventory_item_id, -draws[i].quantity, error))
			return FALSE;
		/* What layers there are, and the rest uncosted: the caller has
		 * already checked stock on hand, and units that arrived without a
		 * layer -- an adjustment typed in by hand -- are real stock with
		 * no recorded cost. Refusing them would make stock the count says
		 * is there unusable. */
		if (costed && !consume_layers(self, draws[i].inventory_item_id, draws[i].quantity,
			actor, cost, &uncosted, error))
			return FALSE;
		/* Per currency, never added across: a made unit whose inputs
		 * cost GOLD and TICKET carries both, as sibling layers below. */
		for (j = 0; j < cost->len; j++)
		{
			if (!venture_money_totals_add(total, g_ptr_array_index(cost, j), error))
				return FALSE;
			consumed = TRUE;
		}
		/* The transaction shows the average it left at, for reading;
		 * the layers are what the books count. */
		if (!movement_unit(cost, draws[i].quantity, &each, &each_notes, error))
			return FALSE;
		out = write_txn(self, draws[i].inventory_item_id, -draws[i].quantity,
			VENTURE_INVENTORY_TXN_KIND_PRODUCTION, each, each_notes, when, reference, 0, actor, error);
		if (out == NULL)
			return FALSE;
	}

	/* --- What comes out --- */
	if (!movement_unit(total, output_quantity, &unit, &notes, error))
		return FALSE;
	txn = write_txn(self, output_item_id, output_quantity, VENTURE_INVENTORY_TXN_KIND_PRODUCTION,
		unit, notes, when, reference, 0, actor, error);
	if (txn == NULL)
		return FALSE;
	if (costed && !add_lot_layers(self, output, output_quantity, total, venture_entity_get_id(txn),
		when, actor, error))
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

/*
 * What @layers still hold, per currency, counting only layers received at
 * or before @as_of; @units (optional, parallel to the result by currency)
 * gets how many units carry a cost in each. A lot bought in two currencies
 * appears in both, which is right: its units cost both.
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

GPtrArray *
venture_inventory_service_valuation(VentureInventoryService *self, gint64 organization_id,
	GDateTime *as_of, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(GPtrArray) values = venture_money_totals_new();
	g_autofree gchar *book = NULL;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), NULL);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	layers = venture_database_find(self->database, query, error);
	if (layers == NULL)
		return NULL;
	/* Per currency: layers in GOLD and TICKET -- in one product or in two
	 * -- are two figures, never a refusal of the whole organization. */
	if (!layers_value(layers, as_of, values, NULL, error))
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
	g_autoptr(GPtrArray) layers = NULL;
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
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"inventory_cost_layer") == G_TYPE_INVALID)
	{
		if (out_unit_costs != NULL)
			*out_unit_costs = venture_money_totals_new();
		return g_steal_pointer(&values);
	}
	item = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, inventory_item_id, error);
	if (item == NULL)
		return NULL;
	layers = layers_for(self, inventory_item_id, error);
	if (layers == NULL || !layers_value(layers, as_of, values, units, error))
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
	if (g_strcmp0(name, "inventory_txn") == 0)
		return TRUE;
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
	if (VENTURE_IS_INVENTORY_TXN(record) && !venture_entity_is_persisted(record))
	{
		gint64 item_id = get_id(record, "inventory-item-id");
		gint64 quantity = get_int(record, "quantity");
		if (!guard_stock(self, item_id, quantity, error))
			return FALSE;
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
