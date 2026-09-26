/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BILLING_SERVICE_H
#define VENTURE_BILLING_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BILLING_SERVICE (venture_billing_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBillingService, venture_billing_service, VENTURE, BILLING_SERVICE, GObject)
/**
 * venture_billing_service_get:
 * @database: the owning database
 * Returns: (transfer none): the database's billing service
 */
VentureBillingService *venture_billing_service_get(VentureDatabase *database);
/**
 * venture_billing_service_execute:
 * @self: the service
 * @request: an unsaved instruction; results replace it only on success
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): error location
 * Returns: TRUE when the entire instruction commits
 */
gboolean venture_billing_service_execute(VentureBillingService *self, VentureBillingRequest *request, const VentureActor *actor, GError **error);
/**
 * venture_billing_service_cancel_credit:
 * @self: the service
 * @subscription: the subscription that would be cancelled
 * @at: the day it would be cancelled
 * @days_left: (out) (optional): the days of the period left unused
 * @error: (out) (optional): error location
 *
 * What cancelling @subscription immediately on @at would credit the
 * customer, tax included: the same figure a `cancel` instruction issues
 * as a credit note, so a page can say it before anybody presses the button.
 * Zero for a trial, a period that was never invoiced, and a date outside
 * the period being served.
 *
 * Returns: (transfer full) (nullable): the credit, or %NULL on error
 */
VentureMoney *venture_billing_service_cancel_credit(VentureBillingService *self, VentureCustomerSubscription *subscription, GDateTime *at, gint64 *days_left, GError **error);
/**
 * venture_billing_service_move_customers:
 * @self: the service
 * @from: the price its customers are on, usually retired
 * @to: another price of the same plan
 * @at_period_end: %TRUE to switch at each renewal, %FALSE to switch now and prorate
 * @at: the effective date
 * @actor: (nullable): the audit actor
 * @moved: (out) (optional): how many subscriptions moved
 * @error: (out) (optional): error location
 *
 * Moves every trialing, active and past-due subscription on @from to @to,
 * each by an ordinary `change` instruction through
 * venture_billing_service_execute(), all in one transaction. One refusal
 * rolls every move back, and the error names the customer it stopped at.
 *
 * Returns: TRUE when every subscription moved
 */
gboolean venture_billing_service_move_customers(VentureBillingService *self, VenturePlanPrice *from, VenturePlanPrice *to, gboolean at_period_end, GDateTime *at, const VentureActor *actor, guint *moved, GError **error);
/**
 * venture_billing_save_hook: (skip)
 * @database: the database with its lock held
 * @record: the proposed save
 * @actor: (nullable): the audit actor
 * @handled: (out): whether the service completed the save
 * @error: (out) (optional): error location
 * Returns: TRUE if allowed
 */
gboolean venture_billing_save_hook(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_billing_check_removal: (skip)
 * @database: the database
 * @record: the record being removed or restored
 * @error: (out) (optional): error location
 * Returns: TRUE if no billing history is changed
 */
gboolean venture_billing_check_removal(VentureDatabase *database, VentureEntity *record, GError **error);
/**
 * venture_billing_prepare_request:
 * @self: the billing service
 * @request: the instruction being staged
 * @error: (out) (optional): error location
 *
 * Pins the subscription version before its instruction enters the shared
 * confirmation queue. Approval must still pass every financial guard.
 * Returns: TRUE when the instruction can be staged
 */
gboolean venture_billing_prepare_request(VentureBillingService *self, VentureBillingRequest *request, GError **error);
G_END_DECLS
#endif
