/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_STRIPE_SERVICE_H
#define VENTURE_STRIPE_SERVICE_H
G_BEGIN_DECLS
#ifndef __GI_SCANNER__
typedef struct _StripeTransport StripeTransport;
#endif
#define VENTURE_TYPE_STRIPE_SERVICE (venture_stripe_service_get_type())
G_DECLARE_FINAL_TYPE(VentureStripeService, venture_stripe_service, VENTURE, STRIPE_SERVICE, GObject)
/**
 * venture_stripe_service_new: (skip)
 * @database: storage
 * @organization_id: the legal entity for this endpoint
 * @transport: (nullable): transport override for offline tests
 * @error: (out) (optional): configuration error, naming the missing variable
 *
 * C-only transport injection: StripeTransport is an opaque provider fixture
 * interface with no introspection metadata. The returned service remains a
 * normal GObject with introspectable operations.
 * Returns: (transfer full) (nullable): a started provider, configured from environment
 */
#ifndef __GI_SCANNER__
VentureStripeService *venture_stripe_service_new(VentureDatabase *database, gint64 organization_id, StripeTransport *transport, GError **error);
#endif
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
/**
 * venture_stripe_service_record_payout:
 * @self: provider
 * @provider_id: processor payout identifier
 * @date: bank effective date
 * @gross: customer receipts in the batch
 * @fees: processor fees, never revenue
 * @net: amount expected on the bank statement
 * @cash_account_id: destination cash account
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): stored payout
 */
VentureProcessorPayout *venture_stripe_service_record_payout(VentureStripeService *self, const gchar *provider_id,
	GDateTime *date, const VentureMoney *gross, const VentureMoney *fees, const VentureMoney *net,
	gint64 cash_account_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_link_payout_item:
 * @self: provider
 * @payout_id: stored payout
 * @payment_id: settled receipt
 * @amount: gross amount for that receipt
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: TRUE when the item is retained
 */
gboolean venture_stripe_service_link_payout_item(VentureStripeService *self, gint64 payout_id, gint64 payment_id,
	const VentureMoney *amount, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_open_dispute:
 * @self: provider
 * @provider_id: processor dispute identifier
 * @payment_id: settled receipt
 * @date: hold date
 * @amount: disputed amount
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): stored dispute
 */
VentureProcessorDispute *venture_stripe_service_open_dispute(VentureStripeService *self, const gchar *provider_id,
	gint64 payment_id, GDateTime *date, const VentureMoney *amount, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_lose_chargeback:
 * @self: provider
 * @dispute_id: open dispute
 * @date: chargeback date
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: TRUE when the receipt is reversed through settlement
 */
gboolean venture_stripe_service_lose_chargeback(VentureStripeService *self, gint64 dispute_id, GDateTime *date,
	const VentureActor *actor, GError **error);
G_END_DECLS
#endif
