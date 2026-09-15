/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl purchase_order_fields[] = {
	VENTURE_FIELD("number", "Number", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("vendor-id", "Vendor", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("status", "Status", "draft, approved, sent, partial, received, cancelled; VenturePurchasingService owns transitions"),
	VENTURE_FIELD_NAME("currency", "Currency", "Uppercase ISO 4217 code"),
	VENTURE_FIELD("ordered-at", "Ordered", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("expected-at", "Expected", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("match-tolerance-percent", "Match tolerance %", "Price difference allowed in three-way match",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("match-status", "Match status", "unmatched, matched, mismatch, exception",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("match-exception", "Match exception", "Allows bill approval despite a three-way mismatch",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("vendor-bill-id", "Vendor bill", NULL, "vendor_bill", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("memo", "Memo", NULL)
};
VENTURE_DEFINE_ENTITY(VenturePurchaseOrder, venture_purchase_order, purchase_order_fields)

static const VentureFieldDecl purchase_order_line_fields[] = {
	VENTURE_FIELD_REF("purchase-order-id", "Purchase order", NULL, "purchase_order", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("inventory-item-id", "Inventory item", NULL, "inventory_item", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("description", "Description", NULL),
	VENTURE_FIELD("quantity", "Quantity", "Whole units ordered", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-price", "Unit price", NULL),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VenturePurchaseOrderLine, venture_purchase_order_line, purchase_order_line_fields)

static const VentureFieldDecl goods_receipt_fields[] = {
	VENTURE_FIELD("number", "Number", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("purchase-order-id", "Purchase order", NULL, "purchase_order", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("received-at", "Received", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("status", "Status", "posted, cancelled, returned"),
	VENTURE_FIELD_TEXT("memo", "Memo", NULL)
};
VENTURE_DEFINE_ENTITY(VentureGoodsReceipt, venture_goods_receipt, goods_receipt_fields)

static const VentureFieldDecl goods_receipt_line_fields[] = {
	VENTURE_FIELD_REF("goods-receipt-id", "Goods receipt", NULL, "goods_receipt", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("purchase-order-line-id", "Purchase order line", NULL, "purchase_order_line", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("inventory-item-id", "Inventory item", NULL, "inventory_item", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("quantity", "Quantity", "Whole units received", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-cost", "Unit cost", NULL),
	VENTURE_FIELD("returned-qty", "Returned", "Whole units returned to the vendor",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureGoodsReceiptLine, venture_goods_receipt_line, goods_receipt_line_fields)

static const VentureFieldDecl inventory_cost_layer_fields[] = {
	VENTURE_FIELD_REF("inventory-item-id", "Inventory item", NULL, "inventory_item", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("goods-receipt-line-id", "Receipt line", NULL, "goods_receipt_line", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("received-at", "Received", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("original-qty", "Original quantity", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("remaining-qty", "Remaining quantity", "Unconsumed FIFO quantity",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-cost", "Unit cost", NULL)
};
VENTURE_DEFINE_ENTITY(VentureInventoryCostLayer, venture_inventory_cost_layer, inventory_cost_layer_fields)

static const VentureFieldDecl sales_order_fields[] = {
	VENTURE_FIELD("number", "Number", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("status", "Status", "draft, allocated, partial, fulfilled, invoiced, cancelled"),
	VENTURE_FIELD_NAME("currency", "Currency", "Uppercase ISO 4217 code"),
	VENTURE_FIELD("ordered-at", "Ordered", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("memo", "Memo", NULL)
};
VENTURE_DEFINE_ENTITY(VentureSalesOrder, venture_sales_order, sales_order_fields)

static const VentureFieldDecl sales_order_line_fields[] = {
	VENTURE_FIELD_REF("sales-order-id", "Sales order", NULL, "sales_order", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("inventory-item-id", "Inventory item", NULL, "inventory_item", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("description", "Description", NULL),
	VENTURE_FIELD("quantity", "Quantity", "Whole units ordered", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-price", "Unit price", NULL),
	VENTURE_FIELD("allocated-qty", "Allocated", "Derived by VentureSalesOrderService",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("fulfilled-qty", "Fulfilled", "Derived by VentureSalesOrderService",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("invoiced-qty", "Invoiced", "Derived by VentureSalesOrderService",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("is-service", "Service line", "Service lines may be invoiced without fulfillment",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureSalesOrderLine, venture_sales_order_line, sales_order_line_fields)

static const VentureFieldDecl fulfillment_fields[] = {
	VENTURE_FIELD_REF("sales-order-id", "Sales order", NULL, "sales_order", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("sales-order-line-id", "Sales order line", NULL, "sales_order_line", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("quantity", "Quantity", "Whole units shipped", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("shipped-at", "Shipped", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("status", "Status", "shipped or cancelled"),
	VENTURE_FIELD_TEXT("memo", "Memo", NULL)
};
VENTURE_DEFINE_ENTITY(VentureFulfillment, venture_fulfillment, fulfillment_fields)
