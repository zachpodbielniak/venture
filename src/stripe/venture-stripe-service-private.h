/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-stripe-service-private.h - The Stripe service's party hold, for
 * the rest of src/stripe/
 *
 * Private to src/stripe/. Not installed, not introspected.
 */

#ifndef VENTURE_STRIPE_SERVICE_PRIVATE_H
#define VENTURE_STRIPE_SERVICE_PRIVATE_H

#include <venture.h>
#include "core/venture-fiber-private.h"

G_BEGIN_DECLS

/*
 * venture_stripe_service_hold_record:
 * @self: the service the caller is about to use
 * @record: the record a Stripe record action was asked for: an invoice,
 *   a stripe_checkout or stripe_payment_link (held by its invoice's
 *   party), a customer_subscription or stripe_authorization (held by its
 *   company)
 *
 * Takes the hold a Checkout, a Pay now click and Stripe customer creation
 * take for the same paying party (stripe_party_hold() in
 * venture-stripe-customers.inc), so a record action that reaches Stripe
 * waits for any other fiber paying that party, and they for it. The hold
 * is re-entrant: the service functions the action calls take it again
 * and only count.
 *
 * Never call it with a transaction open; it then answers %NULL.
 *
 * Returns: (transfer full) (nullable): the hold, for
 *   venture_fiber_release(); %NULL when there is nothing to hold (another
 *   record type, no party, an unreadable invoice), with a transaction open,
 *   or on a plain stack that found a fiber holding it -- the caller then
 *   goes on unguarded, as it always has
 */
VentureFiberHold *
venture_stripe_service_hold_record(
	VentureStripeService	*self,
	VentureEntity		*record
);

G_END_DECLS

#endif /* VENTURE_STRIPE_SERVICE_PRIVATE_H */
