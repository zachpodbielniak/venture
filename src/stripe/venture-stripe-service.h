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
 * @error: (out) (optional): configuration error
 *
 * C-only transport injection: StripeTransport is an opaque provider fixture
 * interface with no introspection metadata. The returned service remains a
 * normal GObject with introspectable operations.
 * Returns: (transfer full) (nullable): an organization-bound provider; environment credentials only with explicit transport injection
 */
#ifndef __GI_SCANNER__
VentureStripeService *venture_stripe_service_new(VentureDatabase *database, gint64 organization_id, StripeTransport *transport, GError **error);
#endif
/**
 * venture_stripe_service_for_organization: (skip)
 * @database: storage
 * @organization_id: verified explicit organization
 * @transport: (nullable): offline transport injection
 * @error: (out) (optional): configuration failure
 * Returns: (transfer full) (nullable): provider for the active encrypted binding
 */
#ifndef __GI_SCANNER__
VentureStripeService *venture_stripe_service_for_organization(VentureDatabase *database,
	gint64 organization_id, StripeTransport *transport, GError **error);
/**
 * venture_stripe_service_for_connection: (skip)
 * @database: storage
 * @organization_id: expected binding owner
 * @connection_id: exact historical binding
 * @allow_disabled: allow verified callbacks against a disconnected binding
 * @transport: (nullable): offline transport injection
 * @error: (out) (optional): configuration failure
 * Returns: (transfer full) (nullable): provider retaining the historical account identity
 */
VentureStripeService *venture_stripe_service_for_connection(VentureDatabase *database,
	gint64 organization_id, gint64 connection_id, gboolean allow_disabled,
	StripeTransport *transport, GError **error);
/**
 * venture_stripe_settings_configure: (skip)
 * @database: storage
 * @organization_id: explicit organization administered by the caller
 * @settings: write-only credential and return URL object
 * @expected_version: zero to connect or current version to rotate
 * @expected_connection_id: zero to connect or the exact connection being rotated
 * @transport: (nullable): offline transport injection
 * @actor: (nullable): audit actor
 * @error: (out) (optional): redacted refusal
 *
 * Verifies the provider account before sealing settings. Account/environment
 * replacement requires disconnecting the old binding first.
 * Returns: (transfer full) (nullable): public connection metadata
 */
VentureIntegrationConnection *venture_stripe_settings_configure(VentureDatabase *database,
	gint64 organization_id, JsonNode *settings, gint64 expected_version,
	gint64 expected_connection_id, StripeTransport *transport, const VentureActor *actor, GError **error);
/**
 * venture_stripe_settings_test: (skip)
 * @database: storage
 * @organization_id: explicit organization administered by the caller
 * @transport: (nullable): offline transport injection
 * @error: (out) (optional): redacted refusal
 * Returns: whether current credentials still identify the bound account
 */
gboolean venture_stripe_settings_test(VentureDatabase *database, gint64 organization_id,
	StripeTransport *transport, GError **error);
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
 * venture_stripe_service_retry_event:
 * @self: historically bound provider
 * @event_id: immutable previously verified event record
 * @accept_balance_change: explicitly accept allocating the original amount with any excess as customer credit
 * @actor: (nullable): finance operator attribution
 * @error: (out) (optional): authorization or accounting refusal
 *
 * Replays only retained authenticated evidence. Never changes its amount,
 * currency, account, settlement date or payment identity. Accounting period
 * and separation-of-duties rules apply to the operator's execution.
 * Returns: TRUE if settled or already handled
 */
gboolean venture_stripe_service_retry_event(VentureStripeService *self, gint64 event_id,
	gboolean accept_balance_change, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_create_payment_link:
 * @self: active organization provider
 * @invoice_id: issued invoice
 * @base_url: trusted installation HTTPS origin
 * @expires: (nullable): expiry, default seven days and maximum thirty days
 * @actor: (nullable): finance operator attribution
 * @error: (out) (optional): refusal
 *
 * Replaces prior links after revoking their provider sessions. The bearer URL
 * is returned once in the transient url property; only its digest is stored.
 * Returns: (transfer full) (nullable): bound payment capability
 */
VentureStripePaymentLink *venture_stripe_service_create_payment_link(VentureStripeService *self,
	gint64 invoice_id, const gchar *base_url, GDateTime *expires, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_resolve_payment_link:
 * @database: repository
 * @token: complete opaque path capability
 * @now: (nullable): lookup clock, defaults to current UTC time
 * @error: (out) (optional): unavailable capability
 *
 * Checks digest in constant time, expiry, account, organization, invoice
 * revision and outstanding status. Does not contact Stripe or settle cash.
 * Returns: (transfer full) (nullable): verified capability metadata
 */
VentureStripePaymentLink *venture_stripe_service_resolve_payment_link(VentureDatabase *database,
	const gchar *token, GDateTime *now, GError **error);
/**
 * venture_stripe_service_pay_link:
 * @self: active provider for the verified link organization
 * @token: opaque payment capability
 * @now: (nullable): current lookup clock
 * @error: (out) (optional): refusal
 *
 * Creates or reuses one durable Checkout; the provider deadline cannot outlive
 * this link. Processing bank payments remain pending without a new collection.
 * Returns: (transfer full) (nullable): provider Checkout metadata
 */
VentureStripeCheckout *venture_stripe_service_pay_link(VentureStripeService *self,
	const gchar *token, GDateTime *now, GError **error);
/**
 * venture_stripe_service_revoke_payment_link:
 * @self: provider retaining the link's exact historical binding
 * @link_id: owned link to revoke
 * @actor: (nullable): finance operator attribution
 * @error: (out) (optional): provider expiry or authorization refusal
 *
 * Revokes locally before expiring an open remote session. A transport failure
 * retains the blocked attempt and can be retried. Already processing bank
 * payments cannot be cancelled here; their authenticated callbacks still run.
 * Returns: TRUE when revocation and any necessary expiry completed
 */
gboolean venture_stripe_service_revoke_payment_link(VentureStripeService *self,
	gint64 link_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_actions_register:
 * @database: repository owning generic actions
 *
 * Registers finance-checked payment link creation, revocation and event replay.
 */
void venture_stripe_actions_register(VentureDatabase *database);
/**
 * venture_stripe_actions_set_context:
 * @database: repository
 * @context: installation configuration owner, weakly retained
 *
 * Supplies the trusted installation origin and explicit offline test service.
 */
void venture_stripe_actions_set_context(VentureDatabase *database, VentureContext *context);
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
/**
 * venture_stripe_service_authorize_subscription:
 * @self: organization provider
 * @subscription_id: live Venture customer subscription
 * @limit: maximum amount authorized for each recurring invoice
 * @actor: (nullable): finance audit actor
 * @error: (out) (optional): refusal
 *
 * Reserves exact subscription terms before creating hosted Setup. The returned
 * URL is transient; only verified provider evidence can activate permission.
 * Returns: (transfer full) (nullable): pending authorization with hosted URL
 */
VentureStripeAuthorization *venture_stripe_service_authorize_subscription(VentureStripeService *self,
	gint64 subscription_id, const VentureMoney *limit, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_verify_authorization:
 * @self: bound provider
 * @authorization_id: pending hosted permission
 * @actor: (nullable): finance audit actor
 * @error: (out) (optional): verification refusal
 *
 * Reads the retained session through the authenticated provider API. Only an
 * exact completed Setup and matching reusable method can activate permission.
 * Returns: TRUE when verified permission is active
 */
gboolean venture_stripe_service_verify_authorization(VentureStripeService *self,
	gint64 authorization_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_revoke_authorization:
 * @self: bound provider
 * @authorization_id: permission to disable
 * @actor: (nullable): finance audit actor
 * @error: (out) (optional): refusal
 *
 * Stops future collection. Existing processing payments retain their evidence
 * and settle through their historical account; this is not bank cancellation.
 * Returns: TRUE when permission is disabled
 */
gboolean venture_stripe_service_revoke_authorization(VentureStripeService *self,
	gint64 authorization_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_collect_invoice:
 * @self: organization provider
 * @invoice_id: ordinary invoice generated by Venture billing
 * @actor: (nullable): finance audit actor
 * @error: (out) (optional): refusal or uncertain provider outcome
 *
 * Reserves one invoice-wide attempt, creates a controlled provider invoice and
 * requests payment with verified reusable permission. HTTP success posts no cash.
 * Returns: (transfer full) (nullable): durable automatic collection attempt
 */
VentureStripeCheckout *venture_stripe_service_collect_invoice(VentureStripeService *self,
	gint64 invoice_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_cancel_collection:
 * @self: historical organization provider
 * @checkout_id: automatic collection attempt
 * @actor: (nullable): finance audit actor
 * @error: (out) (optional): refusal or uncertain cancellation
 *
 * Confirms unpaid draft deletion or invoice voiding through the provider.
 * Processing or settled funds cannot be cancelled through this operation.
 * Manual cancellation stops automatic retries for this authorization/invoice.
 * Returns: TRUE when the retained attempt can no longer collect
 */
gboolean venture_stripe_service_cancel_collection(VentureStripeService *self,
	gint64 checkout_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_retry_collection:
 * @self: the organization collection service
 * @checkout_id: the failed automatic attempt
 * @now: explicit scheduling clock; never a cash settlement date
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error location
 *
 * Confirms the old provider invoice is cancelled before reserving a new one.
 * Retries wait one day and stop after three attempts per authorization.
 * Returns: (transfer full): the new or already pending collection, or NULL
 */
VentureStripeCheckout *venture_stripe_service_retry_collection(VentureStripeService *self,
	gint64 checkout_id, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_collect_due:
 * @self: the organization collection service
 * @now: explicit scheduling clock, never the settlement date
 * @limit: maximum authorizations to examine, from 1 to 100
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error location
 *
 * Advances at most one renewal and one collection per verified permission.
 * A durable next-check clock distributes work across subsequent calls. Individual
 * operational failures remain on the permission and processor exception records.
 * Returns: number examined, or -1 for a sweep-level failure
 */
gint venture_stripe_service_collect_due(VentureStripeService *self, GDateTime *now,
	guint limit, const VentureActor *actor, GError **error);
/**
 * venture_stripe_service_reconcile_collection:
 * @self: the original account service
 * @checkout_id: retained automatic reservation
 * @provider_invoice_id: candidate invoice from the provider's dashboard
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error location
 *
 * Reads the invoice and validates its immutable opaque correlation before
 * linking an uncertain creation response. It never records a cash receipt.
 * Returns: (transfer full): retained reconciled reservation, or NULL
 */
VentureStripeCheckout *venture_stripe_service_reconcile_collection(VentureStripeService *self,
	gint64 checkout_id, const gchar *provider_invoice_id, const VentureActor *actor, GError **error);
/**
 * venture_stripe_collection_start:
 * @context: owning application context
 *
 * Installs an idle main-context collection timer. Only verified enabled
 * organization authorizations opt in; module and connection switches stay live.
 */
void venture_stripe_collection_start(VentureContext *context);
/**
 * venture_stripe_collection_stop:
 * @context: owning application context
 *
 * Removes the collection timer before server shutdown.
 */
void venture_stripe_collection_stop(VentureContext *context);
G_END_DECLS
#endif
