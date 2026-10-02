/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_INVENTORY_SERVICE_H
#define VENTURE_INVENTORY_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_INVENTORY_SERVICE (venture_inventory_service_get_type())
G_DECLARE_FINAL_TYPE(VentureInventoryService, venture_inventory_service, VENTURE, INVENTORY_SERVICE, GObject)
/**
 * venture_inventory_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureInventoryService *venture_inventory_service_get(VentureDatabase *database);
/**
 * venture_inventory_service_on_hand:
 * @self: the service or registry instance
 * @inventory_item_id: inventory item id
 * @as_of: (nullable): cutoff time; NULL uses the service default
 * @error: (out) (optional): return location for an error
 *
 * Returns: quantity on hand; check @error for a query failure
 */
gint64 venture_inventory_service_on_hand(VentureInventoryService *self, gint64 inventory_item_id,
	GDateTime *as_of, GError **error);
/**
 * venture_inventory_service_valuation:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @as_of: (nullable): count only layers received at or before this time
 * @error: (out) (optional): return location for an error
 *
 * What the FIFO cost layers still on hand cost, one amount per currency --
 * never added across currencies, so layers bought in GOLD and in TICKET
 * give two figures rather than refusing the organization. The book
 * currency (venture_posting_service_book_currency()) comes first, then the
 * rest by code. Nothing on hand is one zero in the book currency. A memo
 * currency's layers are counted here although they post nothing.
 *
 * Returns: (transfer full) (element-type VentureMoney) (nullable): the
 *   values, or %NULL with @error set
 */
GPtrArray *venture_inventory_service_valuation(VentureInventoryService *self, gint64 organization_id,
	GDateTime *as_of, GError **error);
/**
 * venture_inventory_service_item_value:
 * @self: the service
 * @inventory_item_id: inventory item id
 * @as_of: (nullable): count only layers received at or before this time
 * @out_unit_costs: (out) (optional) (transfer full) (element-type VentureMoney):
 *   the average cost of the units carrying a cost in each currency, in the
 *   same order as the result
 * @error: (out) (optional): return location for an error
 *
 * What one item's remaining cost layers cost, per currency, book currency
 * first. Empty when the item has no costed layer left (or the goods module
 * is off): the caller falls back to the item's typed unit cost.
 *
 * Returns: (transfer full) (element-type VentureMoney) (nullable): the
 *   values, or %NULL with @error set
 */
GPtrArray *venture_inventory_service_item_value(VentureInventoryService *self, gint64 inventory_item_id,
	GDateTime *as_of, GPtrArray **out_unit_costs, GError **error);
/**
 * venture_inventory_service_receive:
 * @self: the service or registry instance
 * @inventory_item_id: inventory item id
 * @quantity: quantity in the inventory item's units
 * @unit_cost: cost per unit in integer minor units
 * @date: effective date
 * @receipt_line_id: receipt line id
 * @reference: external or operator reference
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_receive(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error);
/**
 * venture_inventory_service_issue:
 * @self: the service or registry instance
 * @inventory_item_id: inventory item id
 * @quantity: quantity in the inventory item's units
 * @date: effective date
 * @source_type: registered source entity name
 * @source_id: source record ID
 * @actor: (nullable): audit actor; NULL for internal service work
 * @costs: (out) (transfer full) (optional) (element-type VentureMoney): cost
 *   of the issued inventory, one amount per currency its layers were in
 *   (zero-cost layers give a zero); empty when no layer was consumed
 * @error: (out) (optional): return location for an error
 *
 * Takes @quantity units out FIFO and posts cost of goods sold against
 * inventory. The layers consumed may cost several currencies; each is
 * posted as its own balanced pair through
 * venture_posting_service_post_by_currency() in one call, so a valued
 * currency with a rate lands in the book journal, a separate one in its
 * own and a memo one nowhere. Nothing is refused for mixing currencies.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_issue(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *source_type, gint64 source_id,
	const VentureActor *actor, GPtrArray **costs, GError **error);
/**
 * venture_inventory_service_restore:
 * @self: the service or registry instance
 * @inventory_item_id: inventory item id
 * @quantity: quantity in the inventory item's units
 * @unit_cost: cost per unit in integer minor units
 * @date: effective date
 * @receipt_line_id: receipt line id
 * @reference: external or operator reference
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_restore(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error);
/**
 * venture_inventory_service_transfer:
 * @self: the service or registry instance
 * @from_item_id: from item id
 * @to_item_id: to item id
 * @quantity: quantity in the inventory item's units
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_transfer(VentureInventoryService *self, gint64 from_item_id,
	gint64 to_item_id, gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error);
/**
 * VentureInventoryDraw:
 * @inventory_item_id: the stock the units come out of
 * @quantity: whole units taken; positive
 *
 * One input to venture_inventory_service_produce(): so many units out of
 * one inventory item.
 */
typedef struct
{
	gint64 inventory_item_id;
	gint64 quantity;
} VentureInventoryDraw;
/**
 * venture_inventory_service_produce:
 * @self: the service
 * @draws: (array length=n_draws): the stock consumed
 * @n_draws: how many draws
 * @output_item_id: the inventory item the made units go into
 * @output_quantity: whole units made; positive
 * @date: (nullable): effective date; NULL is now
 * @reference: (nullable): what made them, e.g. "recipe:12"
 * @actor: (nullable): audit actor; NULL for internal service work
 * @out_txn: (out) (optional) (transfer full): the output's transaction
 * @out_costs: (out) (optional) (nullable) (transfer full) (element-type VentureMoney):
 *   the FIFO cost of everything consumed, one amount per currency, which is
 *   the cost the made units carry; NULL when no cost layer was consumed
 * @error: (out) (optional): return location for an error
 *
 * Turns stock into other stock, in one transaction: each draw leaves as a
 * negative PRODUCTION transaction at its FIFO cost, the output arrives as a
 * positive PRODUCTION transaction, and the consumed cost becomes the
 * output's cost layers -- split exactly, so the layers sum to the minor
 * unit to what was consumed and the cost of goods sold later is right.
 * Posts no journal: stock moved from inventory to inventory.
 *
 * Inputs that cost two currencies give the output sibling layers, one set
 * per currency, all naming the output transaction as their lot; FIFO takes
 * a unit out of every sibling at once, so selling a made unit later posts
 * its GOLD and its TICKET cost together. Zero-cost inputs add no layer
 * beside costed ones; with no cost at all the output still gets one zero
 * layer, in the item's unit-cost currency or the book currency.
 *
 * A draw that would take an item below zero is refused unless the item
 * allows negative stock. Units with no cost layer (typed in by hand) are
 * consumed at no cost. With the goods module off there are no cost layers,
 * and only quantities move. Any failure rolls every write back.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_produce(VentureInventoryService *self,
	const VentureInventoryDraw *draws, guint n_draws, gint64 output_item_id,
	gint64 output_quantity, GDateTime *date, const gchar *reference,
	const VentureActor *actor, VentureEntity **out_txn, GPtrArray **out_costs,
	GError **error);
/**
 * venture_inventory_product_is_stocked:
 * @database: database owning the records
 * @product_id: product id
 *
 * Returns: whether the product is backed by inventory
 */
gboolean venture_inventory_product_is_stocked(VentureDatabase *database, gint64 product_id);
/**
 * venture_inventory_service_issue_sale:
 * @self: the service or registry instance
 * @sale: source sale
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_issue_sale(VentureInventoryService *self, VentureEntity *sale,
	const VentureActor *actor, GError **error);
/**
 * venture_inventory_service_issue_invoice:
 * @self: the service or registry instance
 * @invoice: source invoice
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_issue_invoice(VentureInventoryService *self, VentureEntity *invoice,
	const VentureActor *actor, GError **error);
/**
 * venture_goods_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_goods_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_goods_save_hook:
 * @database: database owning the records
 * @record: candidate record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @handled: (out): whether the service performed the write
 * @error: (out) (optional): return location for an error
 *
 * Routes generic writes through the subsystem operation when required.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_goods_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_goods_register_reports:
 * @registry: registry receiving the registrations
 */
void venture_goods_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
