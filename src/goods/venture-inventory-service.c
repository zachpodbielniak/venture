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

static gboolean
post_pair(VentureInventoryService *self, gint64 org, VentureEntity *source, gint64 debit_id,
	gint64 credit_id, const VentureMoney *amount, const gchar *memo, GDateTime *when,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
	g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureJournal) posted = NULL;
	if (amount == NULL || venture_money_is_zero(amount))
		return TRUE;
	g_object_set(journal, "source-type", venture_entity_get_entity_name(source),
		"source-id", venture_entity_get_id(source), "occurred-at", when,
		"currency", venture_money_get_currency(amount), "organization-id", org, "memo", memo, NULL);
	g_object_set(debit, "account-id", debit_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", amount,
		"organization-id", org, NULL);
	g_object_set(credit, "account-id", credit_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", amount,
		"organization-id", org, NULL);
	g_ptr_array_add(lines, g_object_ref(debit));
	g_ptr_array_add(lines, g_object_ref(credit));
	posted = venture_posting_service_post(venture_database_get_posting_service(self->database),
		journal, lines, NULL, actor, error);
	return posted != NULL;
}

static gboolean
add_money(VentureMoney **sum, const VentureMoney *value, GError **error)
{
	VentureMoney *next;
	if (value == NULL)
		return TRUE;
	if (*sum == NULL)
	{
		*sum = venture_money_copy(value);
		return TRUE;
	}
	next = venture_money_add(*sum, value, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*sum);
	*sum = next;
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

static gboolean
consume_fifo(VentureInventoryService *self, gint64 item_id, gint64 quantity, const VentureActor *actor,
	VentureMoney **cogs, GError **error)
{
	g_autoptr(GPtrArray) layers = NULL;
	gint64 need = quantity;
	guint i;
	if (cogs != NULL)
		*cogs = NULL;
	layers = layers_for(self, item_id, error);
	if (layers == NULL)
		return FALSE;
	for (i = 0; i < layers->len && need > 0; i++)
	{
		VentureEntity *layer = g_ptr_array_index(layers, i);
		g_autoptr(VentureMoney) unit = NULL;
		g_autoptr(VentureMoney) slice = NULL;
		gint64 remaining = get_int(layer, "remaining-qty");
		gint64 take;
		if (remaining <= 0)
			continue;
		take = remaining < need ? remaining : need;
		g_object_get(layer, "unit-cost", &unit, NULL);
		if (unit == NULL)
			return refuse(error, "a cost layer has no unit cost");
		slice = venture_money_multiply_int(unit, take, error);
		if (slice == NULL)
			return FALSE;
		if (cogs != NULL && !add_money(cogs, slice, error))
			return FALSE;
		g_object_set(layer, "remaining-qty", remaining - take, NULL);
		if (!save_owned(self, layer, actor, error))
			return FALSE;
		need -= take;
	}
	if (need > 0 && !item_allows_negative(self, item_id))
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
	const VentureMoney *unit_cost, GDateTime *date, const gchar *reference, gint64 sale_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInventoryTxn) txn = venture_inventory_txn_new();
	g_autoptr(VentureEntity) item = NULL;
	item = venture_database_get(self->database, VENTURE_TYPE_INVENTORY_ITEM, item_id, error);
	if (item == NULL)
		return NULL;
	g_object_set(txn, "inventory-item-id", item_id, "kind", kind, "quantity", quantity,
		"unit-cost", unit_cost, "occurred-at", date, "reference", reference, "sale-id", sale_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(txn), venture_entity_get_organization_id(item));
	if (!save_owned(self, VENTURE_ENTITY(txn), actor, error))
		return NULL;
	return VENTURE_ENTITY(g_steal_pointer(&txn));
}

gboolean
venture_inventory_service_receive(VentureInventoryService *self, gint64 inventory_item_id,
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
		unit_cost, when, reference, 0, actor, error);
	if (txn == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(txn);
	layer = venture_inventory_cost_layer_new();
	g_object_set(layer, "inventory-item-id", inventory_item_id, "goods-receipt-line-id", receipt_line_id,
		"received-at", when, "original-qty", quantity, "remaining-qty", quantity, "unit-cost", unit_cost, NULL);
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

gboolean
venture_inventory_service_issue(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *source_type, gint64 source_id,
	const VentureActor *actor, VentureMoney **cogs, GError **error)
{
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 org, inventory, cogs_id;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (cogs != NULL)
		*cogs = NULL;
	if (quantity <= 0)
		return refuse(error, "issues need a positive quantity");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!guard_stock(self, inventory_item_id, -quantity, error))
		return FALSE;
	if (!consume_fifo(self, inventory_item_id, quantity, actor, &cost, error))
		return FALSE;
	txn = write_txn(self, inventory_item_id, -quantity, VENTURE_INVENTORY_TXN_KIND_SALE,
		cost, when, source_type, g_strcmp0(source_type, "sale") == 0 ? source_id : 0, actor, error);
	if (txn == NULL)
		return FALSE;
	org = venture_entity_get_organization_id(txn);
	inventory = account_code(self, org, "inventory", "1200", "Inventory", VENTURE_ACCOUNT_KIND_ASSET, error);
	cogs_id = account_code(self, org, "cogs", "5000", "Cost of goods sold", VENTURE_ACCOUNT_KIND_EXPENSE, error);
	if (inventory == 0 || cogs_id == 0)
		return FALSE;
	if (!post_pair(self, org, txn, cogs_id, inventory, cost, "Inventory issue", when, actor, error))
		return FALSE;
	if (cogs != NULL)
		*cogs = g_steal_pointer(&cost);
	return TRUE;
}

gboolean
venture_inventory_service_restore(VentureInventoryService *self, gint64 inventory_item_id,
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
		unit_cost, when, reference, 0, actor, error);
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

gboolean
venture_inventory_service_transfer(VentureInventoryService *self, gint64 from_item_id,
	gint64 to_item_id, gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureEntity) out = NULL;
	g_autoptr(VentureEntity) in = NULL;
	g_autoptr(VentureInventoryCostLayer) layer = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), FALSE);
	if (quantity <= 0 || from_item_id == to_item_id)
		return refuse(error, "transfers move a positive quantity between two locations");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	if (!guard_stock(self, from_item_id, -quantity, error))
		return FALSE;
	if (!consume_fifo(self, from_item_id, quantity, actor, &cost, error))
		return FALSE;
	{
		g_autoptr(VentureMoney) unit = venture_money_multiply_rational(cost, 1, quantity, error);
		if (unit == NULL)
			return FALSE;
		out = write_txn(self, from_item_id, -quantity, VENTURE_INVENTORY_TXN_KIND_TRANSFER,
			unit, when, "transfer", 0, actor, error);
		in = write_txn(self, to_item_id, quantity, VENTURE_INVENTORY_TXN_KIND_TRANSFER,
			unit, when, "transfer", 0, actor, error);
		if (out == NULL || in == NULL)
			return FALSE;
		layer = venture_inventory_cost_layer_new();
		g_object_set(layer, "inventory-item-id", to_item_id, "received-at", when,
			"original-qty", quantity, "remaining-qty", quantity, "unit-cost", unit, NULL);
	}
	venture_entity_set_organization_id(VENTURE_ENTITY(layer), venture_entity_get_organization_id(in));
	return save_owned(self, VENTURE_ENTITY(layer), actor, error);
}

VentureMoney *
venture_inventory_service_valuation(VentureInventoryService *self, gint64 organization_id,
	GDateTime *as_of, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	g_autoptr(GPtrArray) layers = NULL;
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INVENTORY_SERVICE(self), NULL);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	layers = venture_database_find(self->database, query, error);
	if (layers == NULL)
		return NULL;
	for (i = 0; i < layers->len; i++)
	{
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureMoney) unit = NULL;
		g_autoptr(VentureMoney) slice = NULL;
		gint64 remaining;
		g_object_get(g_ptr_array_index(layers, i), "received-at", &when, "unit-cost", &unit,
			"remaining-qty", &remaining, NULL);
		if (as_of != NULL && when != NULL && g_date_time_compare(when, as_of) > 0)
			continue;
		if (remaining <= 0 || unit == NULL)
			continue;
		slice = venture_money_multiply_int(unit, remaining, error);
		if (slice == NULL || !add_money(&total, slice, error))
			return NULL;
	}
	return total != NULL ? g_steal_pointer(&total) : venture_money_new_zero("USD");
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

gboolean
venture_inventory_service_issue_sale(VentureInventoryService *self, VentureEntity *sale,
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

gboolean
venture_inventory_service_issue_invoice(VentureInventoryService *self, VentureEntity *invoice,
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
		return FALSE;
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

static VentureReportResult *
committed_spend_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PURCHASE_ORDER);
	g_autoptr(GPtrArray) orders = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Committed spend", period);
	g_autoptr(VentureMoney) total = venture_money_new_zero("USD");
	VentureDatabase *db = venture_context_get_database(context);
	guint i;
	venture_query_set_organization(query, report_org(context, options));
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
		g_autoptr(VentureMoney) committed = venture_money_new_zero("USD");
		g_autofree gchar *number = NULL;
		g_autofree gchar *status = NULL;
		guint j;
		g_object_get(g_ptr_array_index(orders, i), "number", &number, "status", &status, NULL);
		if (g_strcmp0(status, "draft") == 0 || g_strcmp0(status, "cancelled") == 0)
			continue;
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
		{
			VentureMoney *next = venture_money_add(total, committed, error);
			if (next == NULL)
				return NULL;
			venture_money_free(total);
			total = next;
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_money("committed", "Committed spend", total));
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

static VentureReportResult *
valuation_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Inventory valuation", period);
	value = venture_inventory_service_valuation(venture_inventory_service_get(venture_context_get_database(context)),
		report_org(context, options), period != NULL ? venture_date_range_get_end(period) : NULL, error);
	if (value == NULL)
		return NULL;
	venture_report_result_add_metric(result, venture_metric_new_money("valuation", "FIFO valuation", value));
	venture_report_result_begin_row(result);
	venture_report_result_set_money(result, "valuation", value);
	return g_steal_pointer(&result);
}

void
venture_goods_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("committed_spend",
		"Committed spend", "Open purchase orders valued at remaining unordered quantity.",
		committed_spend_report)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("reorder_worklist",
		"Reorder worklist", "Inventory items at or below their reorder point.",
		reorder_report)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("inventory_valuation",
		"Inventory valuation", "FIFO cost layers remaining, reconciling to the inventory control account.",
		valuation_report)));
}
