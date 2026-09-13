/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_QUOTE_RECORDS_H
#define VENTURE_QUOTE_RECORDS_H
G_BEGIN_DECLS
#define VENTURE_TYPE_PRICE_LIST (venture_price_list_get_type())
VENTURE_DECLARE_ENTITY(VenturePriceList, venture_price_list, PRICE_LIST)
/**
 * venture_price_list_new:
 * Returns: (transfer full): a new price_list record
 */
#define VENTURE_TYPE_PRICE_LIST_ITEM (venture_price_list_item_get_type())
VENTURE_DECLARE_ENTITY(VenturePriceListItem, venture_price_list_item, PRICE_LIST_ITEM)
/**
 * venture_price_list_item_new:
 * Returns: (transfer full): a new price_list_item record
 */
#define VENTURE_TYPE_QUOTE (venture_quote_get_type())
VENTURE_DECLARE_ENTITY(VentureQuote, venture_quote, QUOTE)
/**
 * venture_quote_new:
 * Returns: (transfer full): a new quote record
 */
#define VENTURE_TYPE_QUOTE_LINE (venture_quote_line_get_type())
VENTURE_DECLARE_ENTITY(VentureQuoteLine, venture_quote_line, QUOTE_LINE)
/**
 * venture_quote_line_new:
 * Returns: (transfer full): a new quote_line record
 */
#define VENTURE_TYPE_QUOTE_EVENT (venture_quote_event_get_type())
VENTURE_DECLARE_ENTITY(VentureQuoteEvent, venture_quote_event, QUOTE_EVENT)
/**
 * venture_quote_event_new:
 * Returns: (transfer full): a new quote_event record
 */
#define VENTURE_TYPE_QUOTE_DELIVERY (venture_quote_delivery_get_type())
VENTURE_DECLARE_ENTITY(VentureQuoteDelivery, venture_quote_delivery, QUOTE_DELIVERY)
/**
 * venture_quote_delivery_new:
 * Returns: (transfer full): a new quote_delivery record
 */
#define VENTURE_TYPE_QUOTE_ACTION (venture_quote_action_get_type())
VENTURE_DECLARE_ENTITY(VentureQuoteAction, venture_quote_action, QUOTE_ACTION)
/**
 * venture_quote_action_new:
 * Returns: (transfer full): a new quote_action record
 */
/**
 * VentureQuoteStatus:
 * @VENTURE_QUOTE_DRAFT: editable proposal
 * @VENTURE_QUOTE_SENT: frozen and awaiting an answer
 * @VENTURE_QUOTE_ACCEPTED: handed off to billing
 * @VENTURE_QUOTE_DECLINED: declined by the customer
 * @VENTURE_QUOTE_EXPIRED: validity elapsed
 * @VENTURE_QUOTE_SUPERSEDED: replaced by another revision
 */
typedef enum { VENTURE_QUOTE_DRAFT, VENTURE_QUOTE_SENT,
 VENTURE_QUOTE_ACCEPTED, VENTURE_QUOTE_DECLINED, VENTURE_QUOTE_EXPIRED,
 VENTURE_QUOTE_SUPERSEDED } VentureQuoteStatus;
/**
 * venture_quote_status_get_type:
 * Returns: the commercial lifecycle enum
 */
GType venture_quote_status_get_type(void) G_GNUC_CONST;
/**
 * venture_quote_apply_percentages:
 * @subtotal: exact extended line amount
 * @discount_percent: integer percent 0..100
 * @tax_percent: integer percent 0..100
 * @error: (out) (optional): invalid percentages or arithmetic failure
 * Returns: (transfer full) (nullable): discounted and taxed amount
 */
VentureMoney *venture_quote_apply_percentages(const VentureMoney *subtotal,
	gint64 discount_percent, gint64 tax_percent, GError **error);
G_END_DECLS
#endif
