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
