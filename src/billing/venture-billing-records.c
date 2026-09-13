/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
GType
venture_billing_interval_get_type(void)
{
	static gsize type = 0;
	static const GEnumValue values[] = {
		{ 0, "VENTURE_BILLING_INTERVAL_MONTH", "month" },
		{ 1, "VENTURE_BILLING_INTERVAL_YEAR", "year" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type))
	{
		GType registered = g_enum_register_static("VentureBillingInterval", values);
		g_once_init_leave(&type, registered);
	}
	return (GType)type;
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
	VENTURE_FIELD_REF("venture-id", "Venture id", NULL, "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("code", "Code", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VenturePlan, venture_plan, plan_fields)

static const VentureFieldDecl plan_price_fields[] = {
	VENTURE_FIELD_REF("plan-id", "Plan id", NULL, "plan", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("currency", "Currency", NULL),
	VENTURE_FIELD_ENUM("interval", "Interval", NULL, venture_billing_interval_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("amount", "Amount", NULL),
	VENTURE_FIELD("per-seat", "Per seat", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("trial-days", "Trial days", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VenturePlanPrice, venture_plan_price, plan_price_fields)

static const VentureFieldDecl customer_subscription_fields[] = {
	VENTURE_FIELD_REF("company-id", "Company id", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact id", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("plan-price-id", "Plan price id", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_billing_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("seats", "Seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("current-period-start", "Current period start", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("current-period-end", "Current period end", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("trial-end", "Trial end", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cancel-at-period-end", "Cancel at period end", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cancelled-at", "Cancelled at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External id", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("pending-plan-price-id", "Pending plan price id", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("past-due-at", "Past due at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("pending-adjustment", "Pending adjustment", "Proration settled on the next renewal"),
	VENTURE_FIELD("billing-anchor", "Billing anchor", "Original calendar day for renewal boundaries", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-event-at", "Last event at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
};
VENTURE_DEFINE_ENTITY(VentureCustomerSubscription, venture_customer_subscription, customer_subscription_fields)

static const VentureFieldDecl subscription_event_fields[] = {
	VENTURE_FIELD_REF("subscription-id", "Subscription id", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_billing_event_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("at", "At", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("from-plan-price-id", "From plan price id", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("to-plan-price-id", "To plan price id", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("from-seats", "From seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("to-seats", "To seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("proration-amount", "Proration amount", NULL),
	VENTURE_FIELD_REF("invoice-id", "Invoice id", NULL, "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("from-status", "From status", NULL, venture_billing_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("to-status", "To status", NULL, venture_billing_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("from-mrr", "From mrr", NULL),
	VENTURE_FIELD_MONEY("to-mrr", "To mrr", NULL),
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
	VENTURE_FIELD_REF("subscription-id", "Subscription id", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("dunning-step-id", "Dunning step id", NULL, "dunning_step", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("channel", "Channel", NULL),
	VENTURE_FIELD("at", "At", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("past-due-at", "Past due at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("delivery-key", "Delivery key", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
};
VENTURE_DEFINE_ENTITY(VentureBillingNotice, venture_billing_notice, billing_notice_fields)


/* A durable instruction, so approval stages intent without granting CRUD the
 * authority to write subscription state. The service fills the result fields. */
static const VentureFieldDecl billing_request_fields[] = {
	VENTURE_FIELD_NAME("action", "Action", "start, renew, change, change-seats, pause, resume, cancel, mark-payment-failed, recover, renew-sweep, dunning-sweep"),
	VENTURE_FIELD_REF("subscription-id", "Subscription", NULL, "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Customer", NULL, "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("plan-price-id", "Price", NULL, "plan_price", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("seats", "Seats", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("at", "Effective date", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("at-period-end", "At period end", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("dry-run", "Dry run", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External ID", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("expected-version", "Expected version", "Required by staged actions on an existing subscription", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("invoice-id", "Invoice", "Service result", "invoice", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("proration-amount", "Proration", "Service result"),
	VENTURE_FIELD("processed", "Processed", "Service result", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBillingRequest, venture_billing_request, billing_request_fields)
