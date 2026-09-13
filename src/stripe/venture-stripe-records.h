/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_STRIPE_RECORDS_H
#define VENTURE_STRIPE_RECORDS_H
G_BEGIN_DECLS
VENTURE_DECLARE_ENTITY(VentureStripePriceLink, venture_stripe_price_link, STRIPE_PRICE_LINK)
VENTURE_DECLARE_ENTITY(VentureStripeCustomerLink, venture_stripe_customer_link, STRIPE_CUSTOMER_LINK)
VENTURE_DECLARE_ENTITY(VentureStripeCheckout, venture_stripe_checkout, STRIPE_CHECKOUT)
VENTURE_DECLARE_ENTITY(VentureStripeEvent, venture_stripe_event, STRIPE_EVENT)
G_END_DECLS
#endif
