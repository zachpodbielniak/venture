/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static const VentureFieldDecl price_fields[] = {
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("stripe-price-id", "Stripe price", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)
};
VENTURE_DEFINE_ENTITY(VentureStripePriceLink, venture_stripe_price_link, price_fields)

static const VentureFieldDecl customer_fields[] = {
	VENTURE_FIELD_REF("company-id", "Company", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("stripe-customer-id", "Stripe customer", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)
};
static gboolean
customer_before_save(VentureEntity *entity, GError **error)
{
	gint64 company, contact;
	g_object_get(entity, "company-id", &company, "contact-id", &contact, NULL);
	if ((company > 0) == (contact > 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A Stripe customer link requires exactly one company or contact");
		return FALSE;
	}
	return TRUE;
}
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureStripeCustomerLink, venture_stripe_customer_link, customer_fields,
	VENTURE_ENTITY_CLASS(klass)->before_save = customer_before_save;)


static const VentureFieldDecl checkout_fields[] = {
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("session-id", "Session", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("status", "Status", "open, complete or expired; owned by VentureStripeService", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("url", "Checkout URL", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("expected", "Expected payment", NULL),
	VENTURE_FIELD_REF("payment-id", "Payment", NULL, "payment", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureStripeCheckout, venture_stripe_checkout, checkout_fields)

static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD("event-id", "Stripe event", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("type", "Event type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("received-at", "Received", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("processed-at", "Processed", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("payload", "Verified payload", NULL, VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("result", "Result", "processed, ignored or mismatch", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureStripeEvent, venture_stripe_event, event_fields)

static const VentureFieldDecl payout_fields[] = {
	VENTURE_FIELD("provider-id", "Provider payout", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("date", "Effective date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("gross", "Gross receipts", NULL),
	VENTURE_FIELD_MONEY("fees", "Processor fees", NULL),
	VENTURE_FIELD_MONEY("amount", "Net bank deposit", NULL),
	VENTURE_FIELD_REF("bank-account-id", "Destination cash account", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("status", "Status", "pending, paid or failed", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureProcessorPayout, venture_processor_payout, payout_fields)

static const VentureFieldDecl payout_item_fields[] = {
	VENTURE_FIELD_REF("payout-id", "Payout", NULL, "processor_payout", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("payment-id", "Receipt", NULL, "payment", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("amount", "Gross amount in the batch", NULL)
};
VENTURE_DEFINE_ENTITY(VentureProcessorPayoutItem, venture_processor_payout_item, payout_item_fields)

static const VentureFieldDecl dispute_fields[] = {
	VENTURE_FIELD("provider-id", "Provider dispute", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("payment-id", "Receipt", NULL, "payment", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("opened-at", "Opened", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("closed-at", "Closed", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Disputed amount", NULL),
	VENTURE_FIELD("status", "Status", "open, won or lost", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("refund-id", "Chargeback refund", NULL, "refund", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureProcessorDispute, venture_processor_dispute, dispute_fields)

static const VentureFieldDecl exception_fields[] = {
	VENTURE_FIELD("kind", "Kind", "mismatch, closed_period, out_of_order or failed_refund", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("provider-id", "Provider event", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("reason", "Reason", NULL),
	VENTURE_FIELD("resolved", "Resolved", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "Occurred", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureProcessorException, venture_processor_exception, exception_fields)
