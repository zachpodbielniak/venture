/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_STRIPE_SERVICE_H
#define VENTURE_STRIPE_SERVICE_H
G_BEGIN_DECLS
typedef struct _StripeTransport StripeTransport;
#define VENTURE_TYPE_STRIPE_SERVICE (venture_stripe_service_get_type())
G_DECLARE_FINAL_TYPE(VentureStripeService, venture_stripe_service, VENTURE, STRIPE_SERVICE, GObject)
/**
 * venture_stripe_service_new:
 * @database: storage
 * @organization_id: the legal entity for this endpoint
 * @transport: (nullable): transport override for offline tests
 * @error: (out) (optional): configuration error, naming the missing variable
 * Returns: (transfer full) (nullable): a started provider, configured from environment
 */
VentureStripeService *venture_stripe_service_new(VentureDatabase *database, gint64 organization_id, StripeTransport *transport, GError **error);
/**
 * venture_stripe_service_can_checkout:
 * @self: provider
 * @invoice_id: persisted invoice
 * @error: (out) (optional): eligibility refusal
 * Returns: TRUE when the invoice can use the configured single Stripe price
 */
gboolean venture_stripe_service_can_checkout(VentureStripeService *self, gint64 invoice_id, GError **error);
/**
 * venture_stripe_service_checkout:
 * @self: provider
 * @invoice_id: invoice to pay
 * @actor: (nullable): requesting actor
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): stored Checkout record, including its URL
 */
VentureStripeCheckout *venture_stripe_service_checkout(VentureStripeService *self, gint64 invoice_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_handle_webhook:
 * @self: endpoint provider
 * @raw: original bytes
 * @signature: Stripe-Signature header
 * @error: (out) (optional): signature, mismatch or settlement refusal
 * Returns: TRUE for a processed or durably ignored event, including duplicates
 */
gboolean venture_stripe_service_handle_webhook(VentureStripeService *self, GBytes *raw, const gchar *signature, GError **error);
/**
 * venture_stripe_save_owned: (skip)
 * @database: storage
 * @entity: service-owned evidence
 * @actor: (nullable): actor
 * @error: (out) (optional): save error
 * Returns: TRUE on success; private service write boundary
 */
gboolean venture_stripe_save_owned(VentureDatabase *database, VentureEntity *entity, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
