/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_GOODS_RECORDS_H
#define VENTURE_GOODS_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_PURCHASE_ORDER (venture_purchase_order_get_type())
VENTURE_DECLARE_ENTITY(VenturePurchaseOrder, venture_purchase_order, PURCHASE_ORDER)
#define VENTURE_TYPE_PURCHASE_ORDER_LINE (venture_purchase_order_line_get_type())
VENTURE_DECLARE_ENTITY(VenturePurchaseOrderLine, venture_purchase_order_line, PURCHASE_ORDER_LINE)
#define VENTURE_TYPE_GOODS_RECEIPT (venture_goods_receipt_get_type())
VENTURE_DECLARE_ENTITY(VentureGoodsReceipt, venture_goods_receipt, GOODS_RECEIPT)
#define VENTURE_TYPE_GOODS_RECEIPT_LINE (venture_goods_receipt_line_get_type())
VENTURE_DECLARE_ENTITY(VentureGoodsReceiptLine, venture_goods_receipt_line, GOODS_RECEIPT_LINE)
#define VENTURE_TYPE_INVENTORY_COST_LAYER (venture_inventory_cost_layer_get_type())
VENTURE_DECLARE_ENTITY(VentureInventoryCostLayer, venture_inventory_cost_layer, INVENTORY_COST_LAYER)
#define VENTURE_TYPE_SALES_ORDER (venture_sales_order_get_type())
VENTURE_DECLARE_ENTITY(VentureSalesOrder, venture_sales_order, SALES_ORDER)
#define VENTURE_TYPE_SALES_ORDER_LINE (venture_sales_order_line_get_type())
VENTURE_DECLARE_ENTITY(VentureSalesOrderLine, venture_sales_order_line, SALES_ORDER_LINE)
#define VENTURE_TYPE_FULFILLMENT (venture_fulfillment_get_type())
VENTURE_DECLARE_ENTITY(VentureFulfillment, venture_fulfillment, FULFILLMENT)

G_END_DECLS
#endif
