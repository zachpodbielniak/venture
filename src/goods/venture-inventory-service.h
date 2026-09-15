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
 * @as_of: (nullable): cutoff time; NULL uses the service default
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureMoney *venture_inventory_service_valuation(VentureInventoryService *self, gint64 organization_id,
	GDateTime *as_of, GError **error);
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
 * @cogs: (out) (transfer full) (optional): cost of the issued inventory
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_inventory_service_issue(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *source_type, gint64 source_id,
	const VentureActor *actor, VentureMoney **cogs, GError **error);
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
