/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
GType
venture_billing_interval_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_BILLING_INTERVAL_MONTH", "month" },
		{ 1, "VENTURE_BILLING_INTERVAL_YEAR", "year" },
		/* Appended, never inserted: stored prices read back by value. */
		{ 2, "VENTURE_BILLING_INTERVAL_QUARTER", "quarter" },
		{ 3, "VENTURE_BILLING_INTERVAL_HALF_YEAR", "half_year" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureBillingInterval", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}
/*
 * Every period is a whole number of calendar months, which is what keeps
 * renewals on the customer's billing day and lets MRR divide a price by
 * its months exactly.
 */
gint
venture_billing_interval_months(gint interval)
{
	switch (interval)
	{
	case 1:
		return 12;
	case 2:
		return 3;
	case 3:
		return 6;
	default:
		return 1;
	}
}

const gchar *
venture_billing_interval_phrase(gint interval)
{
	switch (interval)
	{
	case 1:
		return "a year";
	case 2:
		return "a quarter";
	case 3:
		return "every 6 months";
	default:
		return "a month";
	}
}

GType
venture_billing_status_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_BILLING_STATUS_TRIALING", "trialing" },
		{ 1, "VENTURE_BILLING_STATUS_ACTIVE", "active" },
		{ 2, "VENTURE_BILLING_STATUS_PAST_DUE", "past_due" },
		{ 3, "VENTURE_BILLING_STATUS_PAUSED", "paused" },
		{ 4, "VENTURE_BILLING_STATUS_CANCELLED", "cancelled" },
		{ 5, "VENTURE_BILLING_STATUS_EXPIRED", "expired" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureBillingStatus", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}
GType
venture_billing_event_kind_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_BILLING_EVENT_KIND_CREATED", "created" },
		{ 1, "VENTURE_BILLING_EVENT_KIND_RENEWED", "renewed" },
		{ 2, "VENTURE_BILLING_EVENT_KIND_UPGRADED", "upgraded" },
		{ 3, "VENTURE_BILLING_EVENT_KIND_DOWNGRADED", "downgraded" },
		{ 4, "VENTURE_BILLING_EVENT_KIND_SEATS_CHANGED", "seats_changed" },
		{ 5, "VENTURE_BILLING_EVENT_KIND_PAUSED", "paused" },
		{ 6, "VENTURE_BILLING_EVENT_KIND_RESUMED", "resumed" },
		{ 7, "VENTURE_BILLING_EVENT_KIND_CANCELLED", "cancelled" },
		{ 8, "VENTURE_BILLING_EVENT_KIND_PAYMENT_FAILED", "payment_failed" },
		{ 9, "VENTURE_BILLING_EVENT_KIND_RECOVERED", "recovered" },
		{ 10, "VENTURE_BILLING_EVENT_KIND_COLLECTED", "collected" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureBillingEventKind", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}
GType
venture_billing_dunning_action_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_BILLING_DUNNING_ACTION_NOTICE", "notice" },
		{ 1, "VENTURE_BILLING_DUNNING_ACTION_RETRY", "retry" },
		{ 2, "VENTURE_BILLING_DUNNING_ACTION_PAUSE", "pause" },
		{ 3, "VENTURE_BILLING_DUNNING_ACTION_CANCEL", "cancel" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureBillingDunningAction", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
}
static const VentureFieldDecl plan_fields[] = {
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("code", "Code", "Short unique name; made from the name when a plan is created on its page", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePlan, venture_plan, plan_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Plan", "Plans");
	venture_entity_class_set_create_path(VENTURE_ENTITY_CLASS(klass), "/plans/new");)

/* A price reads as what it charges: "$30.00 a month per seat", and a
 * metered one says what it counts: "... + API calls at $0.01". */
static gchar *
plan_price_display_name(VentureEntity *self)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) unit_amount = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *unit = NULL;
	gint interval = 0;
	gboolean per_seat = FALSE;
	g_object_get(self, "amount", &amount, "interval", &interval, "per-seat", &per_seat,
		"usage-unit", &unit, "unit-amount", &unit_amount, NULL);
	if (amount == NULL)
		return g_strdup_printf("Price #%" G_GINT64_FORMAT, venture_entity_get_id(self));
	text = venture_money_to_display_string(amount, TRUE);
	if (venture_plan_price_is_metered(VENTURE_PLAN_PRICE(self)) && unit_amount != NULL)
	{
		g_autofree gchar *rate = venture_money_to_display_string(unit_amount, TRUE);
		return g_strdup_printf("%s %s%s + %s at %s", text, venture_billing_interval_phrase(interval),
			per_seat ? " per seat" : "", unit, rate);
	}
	return g_strdup_printf("%s %s%s", text, venture_billing_interval_phrase(interval), per_seat ? " per seat" : "");
}
static const VentureFieldDecl plan_price_fields[] = {
	VENTURE_FIELD_REF("plan-id", "Plan", NULL, "plan", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("currency", "Currency", NULL),
	VENTURE_FIELD_ENUM("interval", "Billed every", NULL, venture_billing_interval_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("per-seat", "Per seat", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("trial-days", "Free trial days", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("product-id", "Product", "Catalog mapping for hosted collection", "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("tax-code-id", "Tax", "Rate charged on each invoice; empty uses the customer's address, as any invoice line does",
		"tax_code", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("usage-unit", "Usage unit", "Metered prices: what is counted, such as API calls; empty for a flat price",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-amount", "Price per unit", "Metered prices: charged for each unit used beyond those included"),
	VENTURE_FIELD("included-units", "Included units", "Metered prices: units each period includes at no extra charge",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePlanPrice, venture_plan_price, plan_price_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = plan_price_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Price", NULL);)

/*
 * A discount a plan offers: a percent or an amount off each invoice, for
 * every period or for the first N. Chosen when a customer is put on the
 * plan; the billing service applies it to the invoices it covers and
 * counts them on the subscription.
 */
gchar *
venture_plan_discount_describe(VenturePlanDiscount *self)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *off = NULL;
	gint64 percent = 0, periods = 0;

	g_return_val_if_fail(VENTURE_IS_PLAN_DISCOUNT(self), NULL);
	g_object_get(self, "percent-off", &percent, "amount-off", &amount, "periods", &periods, NULL);
	if (percent > 0)
		off = g_strdup_printf("%" G_GINT64_FORMAT "%% off", percent);
	else if (amount != NULL)
	{
		g_autofree gchar *text = venture_money_to_display_string(amount, TRUE);
		off = g_strdup_printf("%s off", text);
	}
	else
		off = g_strdup("Nothing off");
	if (periods == 1)
		return g_strdup_printf("%s the first period", off);
	if (periods > 1)
		return g_strdup_printf("%s the first %" G_GINT64_FORMAT " periods", off, periods);
	return g_strdup_printf("%s every period", off);
}

static gchar *
plan_discount_display_name(VentureEntity *self)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *what = venture_plan_discount_describe(VENTURE_PLAN_DISCOUNT(self));

	g_object_get(self, "name", &name, NULL);
	return g_strdup_printf("%s \xe2\x80\x94 %s", name != NULL && *name != '\0' ? name : "Discount", what);
}

static const VentureFieldDecl plan_discount_fields[] = {
	VENTURE_FIELD_REF("plan-id", "Plan", NULL, "plan", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("name", "Name", "What you call the offer, such as Launch offer"),
	VENTURE_FIELD("percent-off", "Percent off", "Whole percent taken off each invoice it covers; or use an amount",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount-off", "Amount off", "Taken off each invoice it covers; or use a percent"),
	VENTURE_FIELD("periods", "For how many periods", "0 for every period",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("code", "Code", "Optional code a customer quotes to claim it",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("ends-at", "Offered until", "No new subscription may take it after this day",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePlanDiscount, venture_plan_discount, plan_discount_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = plan_discount_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Discount", NULL);)

static const VentureFieldDecl customer_subscription_fields[] = {
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("plan-price-id", "Plan and price", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_billing_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("seats", "Seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("current-period-start", "This period started", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("current-period-end", "Renews on", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("trial-end", "Trial ends", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cancel-at-period-end", "Ends at renewal", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cancelled-at", "Cancelled on", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External id", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("pending-plan-price-id", "Switches to at renewal", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("past-due-at", "Overdue since", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("pending-adjustment", "Carried to next invoice", "The difference for part of a period after a mid-period change; added to (or credited on) the next renewal invoice"),
	VENTURE_FIELD("billing-anchor", "Billing day", "Original calendar day for renewal boundaries", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("last-event-at", "Last changed", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("discount-id", "Discount", "Taken off the invoices it covers", "plan_discount", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("discount-periods-used", "Discounted invoices", "How many invoices the discount has covered so far",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("quote-id", "From quote", "The accepted quote this subscription was started from", "quote", VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureCustomerSubscription, venture_customer_subscription, customer_subscription_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Subscription", NULL);
	venture_entity_class_set_create_path(VENTURE_ENTITY_CLASS(klass), "/billing/subscriptions/new");)

static const VentureFieldDecl subscription_event_fields[] = {
	VENTURE_FIELD_REF("subscription-id", "Subscription", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_billing_event_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("at", "At", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("from-plan-price-id", "From price", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("to-plan-price-id", "To price", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("from-seats", "From seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("to-seats", "To seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("proration-amount", "Part-period difference", NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("final-invoice-id", "Final usage invoice", "Usage charged when the subscription ends", "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("from-status", "From status", NULL, venture_billing_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("to-status", "To status", NULL, venture_billing_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("from-mrr", "Monthly revenue before", NULL),
	VENTURE_FIELD_MONEY("to-mrr", "Monthly revenue after", NULL),
	VENTURE_FIELD("period-start", "Period start", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period-end", "Period end", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureSubscriptionEvent, venture_subscription_event, subscription_event_fields)

static const VentureFieldDecl dunning_step_fields[] = {
	VENTURE_FIELD("day-offset", "Day offset", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("action", "Action", NULL, venture_billing_dunning_action_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureDunningStep, venture_dunning_step, dunning_step_fields)

static const VentureFieldDecl billing_notice_fields[] = {
	VENTURE_FIELD_REF("subscription-id", "Subscription", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("dunning-step-id", "Dunning step id", NULL, "dunning_step", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("channel", "Channel", NULL),
	VENTURE_FIELD("at", "At", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("past-due-at", "Overdue since", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delivery-key", "Delivery key", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
};
VENTURE_DEFINE_ENTITY(VentureBillingNotice, venture_billing_notice, billing_notice_fields)


/* A durable instruction, so approval stages intent without granting CRUD the
 * authority to write subscription state. The service fills the result fields. */
static const VentureFieldDecl billing_request_fields[] = {
	VENTURE_FIELD_NAME("action", "Action", "start, activate, prepay, prepay-installments, renew, change, change-seats, pause, resume, cancel, mark-payment-failed, recover, collect, renew-sweep, dunning-sweep"),
	VENTURE_FIELD_REF("subscription-id", "Subscription", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("plan-price-id", "Price", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("seats", "Seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("at", "Effective date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("at-period-end", "At period end", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("dry-run", "Dry run", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External ID", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("discount-id", "Discount", "Start: a discount the plan offers", "plan_discount", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("discount-code", "Discount code", "Start: a code the customer quoted, instead of a discount; matched without regard to case",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("skip-trial", "Skip the free trial", "Start: bill the first period now even when the price has a trial",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("defer-days", "Deferred start", "Start: override the catalogue trial with this many days; zero uses catalogue terms",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("quote-id", "Quote", "Start: the accepted quote the subscription comes from", "quote", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("expected-version", "Expected version", "Required by staged actions on an existing subscription", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("invoice-id", "Invoice", "Service result", "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("proration-amount", "Proration", "Service result"),
	VENTURE_FIELD("processed", "Processed", "Service result", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBillingRequest, venture_billing_request, billing_request_fields)

static const VentureFieldDecl payment_method_fields[] = {
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("method", "Method", "card, ach, direct_debit or manual"),
	VENTURE_FIELD("authorized", "Authorized", "Customer authorization to collect", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External identity", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureCustomerPaymentMethod, venture_customer_payment_method, payment_method_fields)

/*
 * A price is metered when it names what it counts. The unit amount alone
 * does not make one: a rate with nothing to count is refused at the save,
 * so the two are only ever set together.
 */
gboolean
venture_plan_price_is_metered(VenturePlanPrice *self)
{
	g_autofree gchar *unit = NULL;

	g_return_val_if_fail(VENTURE_IS_PLAN_PRICE(self), FALSE);
	g_object_get(self, "usage-unit", &unit, NULL);
	return unit != NULL && *unit != '\0';
}

/* Thousands grouped, as a count reads on an invoice: "1,240". */
gchar *
venture_billing_format_count(gint64 count)
{
	g_autofree gchar *digits = g_strdup_printf("%" G_GINT64_FORMAT, count < 0 ? -count : count);
	GString *text = g_string_new(count < 0 ? "-" : NULL);
	gsize length = strlen(digits), i;

	for (i = 0; i < length; i++)
	{
		if (i > 0 && (length - i) % 3 == 0)
			g_string_append_c(text, ',');
		g_string_append_c(text, digits[i]);
	}
	return g_string_free(text, FALSE);
}

/* "1,240 API calls on 2026-03-05": what was used, and when. */
static gchar *
usage_record_display_name(VentureEntity *self)
{
	g_autoptr(GDateTime) at = NULL;
	g_autofree gchar *count = NULL;
	g_autofree gchar *day = NULL;
	gint64 quantity = 0;

	g_object_get(self, "quantity", &quantity, "occurred-at", &at, NULL);
	count = venture_billing_format_count(quantity);
	if (at == NULL)
		return g_strdup_printf("%s used", count);
	day = g_date_time_format(at, "%F");
	return g_strdup_printf("%s used on %s", count, day);
}

/*
 * One report of metered use. The billing service counts a period's
 * records at renewal, in [start, end), and bills what is beyond the
 * price's included units on the renewal invoice. The idempotency key lets
 * a sender retry a report without it being counted twice.
 */
static const VentureFieldDecl usage_record_fields[] = {
	VENTURE_FIELD_REF("subscription-id", "Subscription", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("quantity", "Quantity", "Whole units used, such as API calls", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("occurred-at", "Used at", "When the use happened; now when left empty", VENTURE_FIELD_KIND_DATETIME,
		VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("idempotency-key", "Idempotency key", "A sender's own id for this report; a repeat is refused, not counted",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureUsageRecord, venture_usage_record, usage_record_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = usage_record_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Usage", "Usage");)

/*
 * One row per Lightsite billing instruction, keyed by the caller's
 * idempotency key in the billing organization. The unique key is what turns
 * a retry into a replay of the answer it was given, instead of a second
 * subscription or a second first invoice; the request hash is what makes a
 * reused key for another request a conflict. Written only by
 * venture_lightsite_billing_subscribe(); the billing save hook refuses
 * every other writer and every removal.
 */
static const VentureFieldDecl lightsite_billing_receipt_fields[] = {
	VENTURE_FIELD("idempotency-key", "Idempotency key", "The caller's key for one instruction",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("request-hash", "Request hash", "SHA-256 of the canonical request",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	/* Another legal entity, not a record of this organization: a number,
	 * never a reference the same-organization check would refuse. */
	VENTURE_FIELD("business-id", "Business", "The customer business's own organization",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("plan-code", "Plan code", "The plan asked for", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("outcome", "Outcome", "start, change or unchanged", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("subscription-id", "Subscription", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("result", "Answer", "The billing view answered, replayed verbatim for the same request"),
	VENTURE_FIELD("actor-user-id", "Administrator", "Workspace administrator whose token gave the instruction",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureLightsiteBillingReceipt, venture_lightsite_billing_receipt, lightsite_billing_receipt_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Lightsite billing receipt", "Lightsite billing receipts");)
