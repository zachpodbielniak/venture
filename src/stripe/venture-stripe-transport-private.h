/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-stripe-transport-private.h - How the Stripe service calls Stripe
 *
 * Private to src/stripe/. Not installed, not introspected.
 */

#ifndef VENTURE_STRIPE_TRANSPORT_PRIVATE_H
#define VENTURE_STRIPE_TRANSPORT_PRIVATE_H

#include <stripe-glib.h>

G_BEGIN_DECLS

/*
 * venture_stripe_execute:
 * @client: a client from venture_stripe_client_new()
 * @request: the operation
 * @cancellable: (nullable): cancels the operation
 * @error: (out) (optional): its failure
 *
 * stripe_client_execute(), telling the client's transport which operation
 * it is carrying while it does. The transport does the retries (stripe-glib's
 * are off), and announces each on @client's "event" signal as
 * stripe.request.retried with the fields stripe-glib itself gave it -- the
 * operation among them, which nothing the transport is sent says. Every
 * Stripe call VENTURE makes goes through here.
 *
 * Returns: (transfer full) (nullable): the resource, as stripe_client_execute()
 */
StripeResource *
venture_stripe_execute(
	StripeClient		 *client,
	const StripeRequest	 *request,
	GCancellable		 *cancellable,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_STRIPE_TRANSPORT_PRIVATE_H */
