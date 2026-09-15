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
G_END_DECLS
#endif
