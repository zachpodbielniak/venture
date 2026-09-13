/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_quote_status_get_type(void)
{
	static gsize type_id;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_QUOTE_DRAFT", "draft" },
		{ 1, "VENTURE_QUOTE_SENT", "sent" },
		{ 2, "VENTURE_QUOTE_ACCEPTED", "accepted" },
		{ 3, "VENTURE_QUOTE_DECLINED", "declined" },
		{ 4, "VENTURE_QUOTE_EXPIRED", "expired" },
		{ 5, "VENTURE_QUOTE_SUPERSEDED", "superseded" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type_id))
	{
		GType result = g_enum_register_static("VentureQuoteStatus", values);
		g_once_init_leave(&type_id, result);
	}
	return type_id;
}

static const VentureFieldDecl price_list_fields[] = {
	VENTURE_FIELD("name", "Name", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("currency", "Currency", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("is-default", "Is default", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VenturePriceList, venture_price_list, price_list_fields)

static const VentureFieldDecl price_list_item_fields[] = {
	VENTURE_FIELD_REF("price-list-id", "Price-list", NULL, "price_list", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("unit-price", "Unit-price", NULL),
	VENTURE_FIELD("min-quantity", "Min quantity", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VenturePriceListItem, venture_price_list_item, price_list_item_fields)

static const VentureFieldDecl quote_fields[] = {
	VENTURE_FIELD("number", "Number", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("revision", "Revision", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("parent-id", "Parent", NULL, "quote", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Company", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deal-id", "Deal", NULL, "deal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", "Use VentureQuoteService", venture_quote_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("issued-at", "Issued at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("valid-until", "Valid until", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("currency", "Currency", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("terms", "Terms", NULL, VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("notes", "Notes", NULL, VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("acceptance-token", "Acceptance token", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("subtotal", "Subtotal", NULL),
	VENTURE_FIELD_MONEY("discount", "Discount", NULL),
	VENTURE_FIELD_MONEY("tax", "Tax", NULL),
	VENTURE_FIELD_MONEY("total", "Total", NULL)
};
VENTURE_DEFINE_ENTITY(VentureQuote, venture_quote, quote_fields)

static const VentureFieldDecl quote_line_fields[] = {
	VENTURE_FIELD_REF("quote-id", "Quote", NULL, "quote", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("description", "Description", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("quantity", "Quantity", "Whole units", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-price", "Unit-price", NULL),
	VENTURE_FIELD("discount-percent", "Discount percent", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tax-percent", "Tax percent", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureQuoteLine, venture_quote_line, quote_line_fields)

static const VentureFieldDecl quote_event_fields[] = {
	VENTURE_FIELD_REF("quote-id", "Quote", NULL, "quote", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("kind", "Kind", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("accepted-by", "Accepted by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("accepted-at", "Accepted at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("method", "Method", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("ip", "Ip", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reason", "Reason", NULL, VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "Occurred at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("subtotal", "Subtotal", NULL),
	VENTURE_FIELD_MONEY("discount", "Discount", NULL),
	VENTURE_FIELD_MONEY("tax", "Tax", NULL),
	VENTURE_FIELD_MONEY("total", "Total", NULL)
};
VENTURE_DEFINE_ENTITY(VentureQuoteEvent, venture_quote_event, quote_event_fields)

static const VentureFieldDecl quote_delivery_fields[] = {
	VENTURE_FIELD_REF("quote-id", "Quote", NULL, "quote", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("channel", "Channel", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("recipient", "Recipient", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("acceptance-url", "Acceptance url", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE)
};
VENTURE_DEFINE_ENTITY(VentureQuoteDelivery, venture_quote_delivery, quote_delivery_fields)

static const VentureFieldDecl quote_action_fields[] = {
	VENTURE_FIELD_REF("quote-id", "Quote", NULL, "quote", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("action", "Action", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("expected-version", "Expected version", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("accepted-by", "Accepted by", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reason", "Reason", NULL, VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("result-quote-id", "Result-quote", NULL, "quote", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureQuoteAction, venture_quote_action, quote_action_fields)


VentureMoney *
venture_quote_apply_percentages(const VentureMoney *subtotal, gint64 discount_percent,
	gint64 tax_percent, GError **error)
{
	g_autoptr(VentureMoney) discount = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	if (discount_percent < 0 || discount_percent > 100 || tax_percent < 0 || tax_percent > 100)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Percentages must be 0..100");
		return NULL;
	}
	discount = venture_money_multiply_rational(subtotal, discount_percent, 100, error);
	if (discount == NULL) return NULL;
	net = venture_money_subtract(subtotal, discount, error);
	if (net == NULL) return NULL;
	tax = venture_money_multiply_rational(net, tax_percent, 100, error);
	return tax == NULL ? NULL : venture_money_add(net, tax, error);
}
