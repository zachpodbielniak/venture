/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static const VentureFieldDecl price_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable provider account and environment; zero means legacy unbound", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("stripe-price-id", "Stripe price", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureStripePriceLink, venture_stripe_price_link, price_fields,
	venture_entity_class_set_unique_partition(VENTURE_ENTITY_CLASS(klass), "connection-id");)

static const VentureFieldDecl customer_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable provider account and environment; zero means legacy unbound", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
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
	VENTURE_ENTITY_CLASS(klass)->before_save = customer_before_save;
	venture_entity_class_set_unique_partition(VENTURE_ENTITY_CLASS(klass), "connection-id");)


static const VentureFieldDecl checkout_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable provider account and environment; zero means legacy unbound", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("session-id", "Session", "Empty until the durable attempt receives its provider identity", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("status", "Status", "initiated, open, processing, complete, failed, expired or exception; service owned", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("url", "Checkout URL", "Empty while creation is uncertain", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active attempt", "Blocks concurrent collection across accounts", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("channel", "Collection channel", "hosted or automatic; legacy rows are hosted", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("authorization-id", "Reusable permission", NULL, "stripe_authorization", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("source-event-id", "Billing source", "Immutable subscription event generating the invoice", "subscription_event", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("provider-invoice-id", "Collection invoice", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("collection-phase", "Collection phase", "reserved, draft, itemized, finalized, paying, requested, retry_wait, requires_action, exhausted, reconciled, deleting, voiding or void", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("pay-attempt", "Payment request number", "Durable bounded provider retry identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("retry-allowed", "Automatic retries", "Manual cancellation stops retries until new customer authorization", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cancellation-evidence", "Provider cancellation evidence", "Confirmed draft deletion or voiding; original payment evidence remains", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("next-attempt-at", "Next collection attempt", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("success-url", "Reserved success URL", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cancel-url", "Reserved cancellation URL", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("ach-enabled", "Reserved ACH choice", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("expires-at", "Provider deadline", "Fixed before provider creation; unknown for legacy attempts", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("expected", "Expected payment", NULL),
	VENTURE_FIELD_REF("payment-id", "Payment", NULL, "payment", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureStripeCheckout, venture_stripe_checkout, checkout_fields,
	venture_entity_class_set_unique_partition(VENTURE_ENTITY_CLASS(klass), "connection-id");
	venture_entity_class_set_field_unique_scope(VENTURE_ENTITY_CLASS(klass), "invoice-id", NULL, "active");)

static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable provider account and environment; zero means legacy unbound", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("event-id", "Stripe event", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("type", "Event type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("received-at", "Received", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("processed-at", "Processed", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("payload", "Verified payload", NULL, VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("provider-evidence", "Verified provider evidence", "Minimal authenticated invoice-payment projection", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("result", "Result", "processed, ignored or mismatch", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureStripeEvent, venture_stripe_event, event_fields,
	venture_entity_class_set_unique_partition(VENTURE_ENTITY_CLASS(klass), "connection-id");)

static const VentureFieldDecl payout_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable provider account and environment; zero means legacy unbound", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("provider-id", "Provider payout", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("date", "Effective date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("gross", "Gross receipts", NULL),
	VENTURE_FIELD_MONEY("fees", "Processor fees", NULL),
	VENTURE_FIELD_MONEY("amount", "Net bank deposit", NULL),
	VENTURE_FIELD_REF("bank-account-id", "Destination cash account", NULL, "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("status", "Status", "pending, paid or failed", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureProcessorPayout, venture_processor_payout, payout_fields,
	venture_entity_class_set_unique_partition(VENTURE_ENTITY_CLASS(klass), "connection-id");)

static const VentureFieldDecl payout_item_fields[] = {
	VENTURE_FIELD_REF("payout-id", "Payout", NULL, "processor_payout", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("payment-id", "Receipt", NULL, "payment", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("amount", "Gross amount in the batch", NULL)
};
VENTURE_DEFINE_ENTITY(VentureProcessorPayoutItem, venture_processor_payout_item, payout_item_fields)

static const VentureFieldDecl dispute_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable provider account and environment; zero means legacy unbound", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("provider-id", "Provider dispute", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("payment-id", "Receipt", NULL, "payment", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("opened-at", "Opened", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("closed-at", "Closed", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Disputed amount", NULL),
	VENTURE_FIELD("status", "Status", "open, won or lost", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("refund-id", "Chargeback refund", NULL, "refund", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureProcessorDispute, venture_processor_dispute, dispute_fields,
	venture_entity_class_set_unique_partition(VENTURE_ENTITY_CLASS(klass), "connection-id");)

static const VentureFieldDecl exception_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable Stripe account binding", "integration_connection", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("event-id", "Verified event", NULL, "stripe_event", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("checkout-id", "Checkout attempt", NULL, "stripe_checkout", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("kind", "Kind", "mismatch, closed_period, out_of_order or failed_refund", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("provider-id", "Provider event", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("reason", "Reason", NULL),
	VENTURE_FIELD("resolved", "Resolved", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "Occurred", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureProcessorException, venture_processor_exception, exception_fields)

static const VentureFieldDecl payment_link_fields[] = {
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable account and environment", "integration_connection", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("invoice-version", "Invoice version", "Capability binds the exact invoice revision", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("token-hash", "Capability digest", "SHA256; bearer token is never persisted", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("expires-at", "Expiry", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("enabled", "Enabled", "Revocation keeps payment evidence and history", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("checkout-id", "Checkout", NULL, "stripe_checkout", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "Copy payment link", "Shown only by the creating action; never persisted", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TRANSIENT)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureStripePaymentLink, venture_stripe_payment_link, payment_link_fields,
	venture_entity_class_set_field_unique_scope(VENTURE_ENTITY_CLASS(klass), "invoice-id", NULL, "enabled");)

static const VentureFieldDecl authorization_fields[] = {
	VENTURE_FIELD("next-check-at", "Next collection check", "Persistent bounded scheduling clock", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("collection-note", "Collection outcome", "Service-owned operational status; provider errors are not exposed"),
	VENTURE_FIELD_REF("connection-id", "Integration connection", "Immutable account and environment", "integration_connection", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("subscription-id", "Subscription", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("plan-price-id", "Authorized price", NULL, "plan_price", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("price-version", "Price version at consent", "Provenance; catalog activation changes do not alter immutable commercial terms", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("seats", "Authorized seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("limit", "Per-invoice limit", "Exact currency and maximum charge agreed on hosted Setup"),
	VENTURE_FIELD_TEXT("consent-text", "Authorization wording", NULL),
	VENTURE_FIELD("enabled", "Enabled", "Pending or active permission; revocation preserves evidence", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("status", "Status", "pending, active or revoked; service owned", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("customer-id", "Stripe customer", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("session-id", "Setup session", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("setup-intent-id", "Verified setup", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("payment-method-id", "Verified payment method", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("mandate-id", "Verified bank mandate", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("evidence", "Verified authorization evidence", "Retained minimal provider projection", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("authorized-at", "Authorized", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("expires-at", "Setup deadline", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("success-url", "Reserved success URL", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("cancel-url", "Reserved cancellation URL", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("ach-enabled", "Authorized bank option", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "Copy authorization link", "One-time action output; never persisted", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TRANSIENT)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureStripeAuthorization, venture_stripe_authorization, authorization_fields,
	venture_entity_class_set_unique_partition(VENTURE_ENTITY_CLASS(klass), "connection-id");
	venture_entity_class_set_field_unique_scope(VENTURE_ENTITY_CLASS(klass), "subscription-id", NULL, "enabled");)
