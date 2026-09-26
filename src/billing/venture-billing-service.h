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
/**
 * venture_billing_service_next_invoice:
 * @self: the billing service
 * @subscription: a saved subscription
 * @error: (out) (optional): error location
 *
 * What the subscription's next invoice will charge, worked out as the
 * renewal will: any price it switches to at renewal, the discount while it
 * still covers invoices and the part-period difference carried to it.
 * Nothing is written.
 *
 * Returns: (transfer full) (nullable): the amount, or %NULL with no error
 *   set when nothing more will be invoiced (paused, ended or ending at
 *   renewal)
 */
VentureMoney *venture_billing_service_next_invoice(VentureBillingService *self,
	VentureCustomerSubscription *subscription, GError **error);
/**
 * venture_billing_service_trial_reminder:
 * @self: the billing service
 * @subscription: a saved subscription
 *
 * The trial-ending reminder queued for @subscription by a renewal sweep,
 * if one was. At most one ever exists: it is keyed by the subscription.
 *
 * Returns: (transfer full) (nullable): the reminder's mail_message, or
 *   %NULL when none was queued or the mail module is off
 */
VentureEntity *venture_billing_service_trial_reminder(VentureBillingService *self,
	VentureCustomerSubscription *subscription);
G_END_DECLS
#endif
