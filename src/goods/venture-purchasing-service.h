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
VenturePurchasingService *venture_purchasing_service_get(VentureDatabase *database);
gboolean venture_purchasing_service_approve(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
gboolean venture_purchasing_service_send(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
gboolean venture_purchasing_service_receive_line(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error);
gboolean venture_purchasing_service_match(VenturePurchasingService *self, gint64 purchase_order_id,
	gint64 vendor_bill_id, gboolean exception, const VentureActor *actor, GError **error);
gboolean venture_purchasing_service_cancel(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error);
gboolean venture_purchasing_service_return_line(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error);
gboolean venture_purchasing_check_bill_approval(VentureDatabase *database, VentureEntity *bill, GError **error);
G_END_DECLS
#endif
