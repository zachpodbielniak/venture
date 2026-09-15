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
VentureInventoryService *venture_inventory_service_get(VentureDatabase *database);
gint64 venture_inventory_service_on_hand(VentureInventoryService *self, gint64 inventory_item_id,
	GDateTime *as_of, GError **error);
VentureMoney *venture_inventory_service_valuation(VentureInventoryService *self, gint64 organization_id,
	GDateTime *as_of, GError **error);
gboolean venture_inventory_service_receive(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error);
gboolean venture_inventory_service_issue(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, GDateTime *date, const gchar *source_type, gint64 source_id,
	const VentureActor *actor, VentureMoney **cogs, GError **error);
gboolean venture_inventory_service_restore(VentureInventoryService *self, gint64 inventory_item_id,
	gint64 quantity, const VentureMoney *unit_cost, GDateTime *date, gint64 receipt_line_id,
	const gchar *reference, const VentureActor *actor, GError **error);
gboolean venture_inventory_service_transfer(VentureInventoryService *self, gint64 from_item_id,
	gint64 to_item_id, gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error);
gboolean venture_inventory_product_is_stocked(VentureDatabase *database, gint64 product_id);
gboolean venture_inventory_service_issue_sale(VentureInventoryService *self, VentureEntity *sale,
	const VentureActor *actor, GError **error);
gboolean venture_inventory_service_issue_invoice(VentureInventoryService *self, VentureEntity *invoice,
	const VentureActor *actor, GError **error);
gboolean venture_goods_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
gboolean venture_goods_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
void venture_goods_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
