/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BILLING_RECORDS_H
#define VENTURE_BILLING_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_billing_interval_get_type:
 * Returns: the billing vocabulary GType
 */
GType venture_billing_interval_get_type(void) G_GNUC_CONST;
/**
 * venture_billing_status_get_type:
 * Returns: the billing vocabulary GType
 */
GType venture_billing_status_get_type(void) G_GNUC_CONST;
/**
 * venture_billing_event_kind_get_type:
 * Returns: the billing vocabulary GType
 */
GType venture_billing_event_kind_get_type(void) G_GNUC_CONST;
/**
 * venture_billing_dunning_action_get_type:
 * Returns: the billing vocabulary GType
 */
GType venture_billing_dunning_action_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_PLAN (venture_plan_get_type())
VENTURE_DECLARE_ENTITY(VenturePlan, venture_plan, PLAN)
/**
 * venture_plan_new:
 * Returns: (transfer full): a billing plan record
 */
#define VENTURE_TYPE_PLAN_PRICE (venture_plan_price_get_type())
VENTURE_DECLARE_ENTITY(VenturePlanPrice, venture_plan_price, PLAN_PRICE)
/**
 * venture_plan_price_new:
 * Returns: (transfer full): a billing plan_price record
 */
#define VENTURE_TYPE_CUSTOMER_SUBSCRIPTION (venture_customer_subscription_get_type())
VENTURE_DECLARE_ENTITY(VentureCustomerSubscription, venture_customer_subscription, CUSTOMER_SUBSCRIPTION)
/**
 * venture_customer_subscription_new:
 * Returns: (transfer full): a billing customer_subscription record
 */
#define VENTURE_TYPE_SUBSCRIPTION_EVENT (venture_subscription_event_get_type())
VENTURE_DECLARE_ENTITY(VentureSubscriptionEvent, venture_subscription_event, SUBSCRIPTION_EVENT)
/**
 * venture_subscription_event_new:
 * Returns: (transfer full): a billing subscription_event record
 */
#define VENTURE_TYPE_DUNNING_STEP (venture_dunning_step_get_type())
VENTURE_DECLARE_ENTITY(VentureDunningStep, venture_dunning_step, DUNNING_STEP)
/**
 * venture_dunning_step_new:
 * Returns: (transfer full): a billing dunning_step record
 */
#define VENTURE_TYPE_BILLING_NOTICE (venture_billing_notice_get_type())
VENTURE_DECLARE_ENTITY(VentureBillingNotice, venture_billing_notice, BILLING_NOTICE)
/**
 * venture_billing_notice_new:
 * Returns: (transfer full): a billing billing_notice record
 */
#define VENTURE_TYPE_CUSTOMER_PAYMENT_METHOD (venture_customer_payment_method_get_type())
VENTURE_DECLARE_ENTITY(VentureCustomerPaymentMethod, venture_customer_payment_method, CUSTOMER_PAYMENT_METHOD)
#define VENTURE_TYPE_BILLING_REQUEST (venture_billing_request_get_type())
VENTURE_DECLARE_ENTITY(VentureBillingRequest, venture_billing_request, BILLING_REQUEST)
/**
 * venture_billing_request_new:
 * Returns: (transfer full): a stageable billing instruction
 */
G_END_DECLS
#endif
