/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PURCHASING_SERVICE_H
#define VENTURE_PURCHASING_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PURCHASING_SERVICE (venture_purchasing_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePurchasingService, venture_purchasing_service, VENTURE, PURCHASING_SERVICE, GObject)
/**
 * venture_purchasing_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VenturePurchasingService *venture_purchasing_service_get(VentureDatabase *database);
/**
 * venture_purchasing_service_approve:
 * @self: the service or registry instance
 * @purchase_order_id: purchase order id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_purchasing_service_approve(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_purchasing_service_send:
 * @self: the service or registry instance
 * @purchase_order_id: purchase order id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_purchasing_service_send(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_purchasing_service_receive_line:
 * @self: the service or registry instance
 * @purchase_order_line_id: purchase order line id
 * @quantity: quantity in the inventory item's units
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_purchasing_service_receive_line(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_purchasing_service_match:
 * @self: the service or registry instance
 * @purchase_order_id: purchase order id
 * @vendor_bill_id: vendor bill id
 * @exception: whether to record an explicit matching exception
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_purchasing_service_match(VenturePurchasingService *self, gint64 purchase_order_id,
	gint64 vendor_bill_id, gboolean exception, const VentureActor *actor, GError **error);
/**
 * venture_purchasing_service_cancel:
 * @self: the service or registry instance
 * @purchase_order_id: purchase order id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_purchasing_service_cancel(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_purchasing_service_return_line:
 * @self: the service or registry instance
 * @purchase_order_line_id: purchase order line id
 * @quantity: quantity in the inventory item's units
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_purchasing_service_return_line(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_purchasing_check_bill_approval:
 * @database: database owning the records
 * @bill: source vendor bill
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_purchasing_check_bill_approval(VentureDatabase *database, VentureEntity *bill, GError **error);
G_END_DECLS
#endif
