/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SALES_ORDER_SERVICE_H
#define VENTURE_SALES_ORDER_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_SALES_ORDER_SERVICE (venture_sales_order_service_get_type())
G_DECLARE_FINAL_TYPE(VentureSalesOrderService, venture_sales_order_service, VENTURE, SALES_ORDER_SERVICE, GObject)
/**
 * venture_sales_order_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureSalesOrderService *venture_sales_order_service_get(VentureDatabase *database);
/**
 * venture_sales_order_service_allocate:
 * @self: the service or registry instance
 * @sales_order_id: sales order id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_sales_order_service_allocate(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_sales_order_service_ship_line:
 * @self: the service or registry instance
 * @sales_order_line_id: sales order line id
 * @quantity: quantity in the inventory item's units
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_sales_order_service_ship_line(VentureSalesOrderService *self, gint64 sales_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_sales_order_service_invoice:
 * @self: the service or registry instance
 * @sales_order_id: sales order id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_sales_order_service_invoice(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_sales_order_service_invoice_fulfilled:
 * @self: the service or registry instance
 * @sales_order_id: sales order id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_sales_order_service_invoice_fulfilled(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_sales_order_service_cancel:
 * @self: the service or registry instance
 * @sales_order_id: sales order id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_sales_order_service_cancel(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_sales_order_service_set_draft_deleted:
 * @self: the sales order service
 * @record: a persisted draft order or one of its unfulfilled lines
 * @deleted: TRUE to retire, FALSE to restore a draft
 * @actor: (nullable): audit actor
 * @error: (out) (optional): validation or database error
 *
 * Retires or restores one draft record through the owning service. Retiring
 * an order requires no live lines. A line must have no allocated, fulfilled
 * or invoiced units, and restoring it requires a live draft parent.
 * Completed goods history and generic removal remain protected.
 *
 * Returns: TRUE on success
 */
gboolean venture_sales_order_service_set_draft_deleted(VentureSalesOrderService *self,
	VentureEntity *record, gboolean deleted, const VentureActor *actor, GError **error);

/**
 * venture_sales_order_service_import_history:
 * @self: owning service
 * @order: persisted draft with original invoice link
 * @status: original lifecycle state
 * @shipped_at: (nullable): original shipment date, required for fulfilled quantities
 * @lines: (element-type VentureSalesOrderLine): original line quantities
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Records historical fulfillment without consuming current inventory.
 * Source lines cannot name a current inventory item.
 * Returns: TRUE on success
 */
gboolean venture_sales_order_service_import_history(VentureSalesOrderService *self, VentureSalesOrder *order,
 const gchar *status, GDateTime *shipped_at, GPtrArray *lines, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
