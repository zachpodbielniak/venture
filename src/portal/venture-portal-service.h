/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PORTAL_SERVICE_H
#define VENTURE_PORTAL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PORTAL_SERVICE (venture_portal_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePortalService, venture_portal_service, VENTURE, PORTAL_SERVICE, GObject)
/**
 * venture_portal_service_send_invitation:
 * @self: portal service
 * @base_url: configured public HTTPS application URL
 * @supplier: whether to invite a supplier rather than a customer
 * @organization_id: owning organization
 * @company_id: company to invite
 * @email: invitation recipient
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure
 *
 * Atomically creates access and queues its private link through the outbox.
 * Generic serialization of the returned access never includes its token.
 * Returns: (transfer full) (nullable): the newly invited access record
 */
VentureEntity *venture_portal_service_send_invitation(VenturePortalService *self,
	const gchar *base_url, gboolean supplier, gint64 organization_id, gint64 company_id,
	const gchar *email, const VentureActor *actor, GError **error);
/**
 * venture_portal_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VenturePortalService *venture_portal_service_get(VentureDatabase *database);
/**
 * venture_portal_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_portal_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_portal_service_invite:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @company_id: company id
 * @email: invitation email address
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_portal_service_invite(VenturePortalService *self, gint64 organization_id,
	gint64 company_id, const gchar *email, const VentureActor *actor, GError **error);
/**
 * venture_portal_service_revoke:
 * @self: the service or registry instance
 * @access: portal invitation record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_portal_service_revoke(VenturePortalService *self, VentureCustomerPortalAccess *access,
	const VentureActor *actor, GError **error);
/**
 * venture_portal_service_lookup:
 * @self: the service or registry instance
 * @token: access token
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_portal_service_lookup(VenturePortalService *self, const gchar *token, GError **error);
/**
 * venture_portal_service_pay:
 * @self: the service or registry instance
 * @access: portal invitation record
 * @invoice_id: invoice id
 * @amount: amount with an explicit currency
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Refuses direct portal receipts. Use venture_portal_service_checkout() and a verified processor callback.
 *
 * Returns: FALSE; unverified receipts are refused
 */
gboolean venture_portal_service_pay(VenturePortalService *self, VentureCustomerPortalAccess *access,
	gint64 invoice_id, const VentureMoney *amount, const VentureActor *actor, GError **error);
/**
 * venture_portal_service_invoices:
 * @self: the service or registry instance
 * @access: portal invitation record
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): owned result
 */
GPtrArray *venture_portal_service_invoices(VenturePortalService *self, VentureCustomerPortalAccess *access,
	GError **error);
/**
 * venture_portal_actions_register:
 * @database: database owning the records
 */
void venture_portal_actions_register(VentureDatabase *database);
/**
 * venture_portal_service_checkout:
 * @self: portal service
 * @stripe: (nullable): configured Checkout service
 * @token: invitation token, checked again for revocation
 * @invoice_id: invoice belonging to the invited customer
 * @actor: (nullable): audit actor
 * @error: (out) (optional): return location for an error
 *
 * Opens Checkout for the server-calculated balance. Only a verified processor
 * callback can record the receipt.
 *
 * Returns: (transfer full) (nullable): the Checkout session
 */
VentureStripeCheckout *venture_portal_service_checkout(VenturePortalService *self,
	VentureStripeService *stripe, const gchar *token, gint64 invoice_id,
	const VentureActor *actor, GError **error);
/**
 * venture_portal_service_subscriptions:
 * @self: portal service
 * @access: the customer's portal access
 * @error: (out) (optional): return location for an error
 *
 * The invited customer's own subscriptions, oldest first; empty when the
 * billing module is off.
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): the subscriptions
 */
GPtrArray *venture_portal_service_subscriptions(VenturePortalService *self, VentureCustomerPortalAccess *access,
	GError **error);
/**
 * venture_portal_service_offered_prices:
 * @self: portal service
 * @access: the customer's portal access
 * @subscription_id: one of the customer's subscriptions
 * @error: (out) (optional): %VENTURE_ERROR_NOT_FOUND when it is not theirs
 *
 * The prices the customer may switch that subscription to: every active
 * price, in the same currency, of an active plan sold by the venture that
 * sells their current plan (the shared plans, when it is shared), except
 * the one they have.
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): the prices
 */
GPtrArray *venture_portal_service_offered_prices(VenturePortalService *self, VentureCustomerPortalAccess *access,
	gint64 subscription_id, GError **error);
/**
 * venture_portal_service_manage_subscription:
 * @self: portal service
 * @token: invitation token, checked again for revocation
 * @subscription_id: the customer's own subscription
 * @action: "change" or "cancel"
 * @plan_price_id: for "change", one of venture_portal_service_offered_prices()
 * @now: for "change", switch today rather than at renewal
 * @actor: (nullable): audit actor
 * @error: (out) (optional): %VENTURE_ERROR_NOT_FOUND for a subscription
 *   that is not this customer's, or the billing service's refusal
 *
 * Switches the subscription's price (at renewal unless @now) or cancels it
 * at renewal, through venture_billing_service_execute(), so every service
 * rule holds. A customer cannot pause, cancel immediately or change seats.
 *
 * Returns: TRUE when the instruction committed
 */
gboolean venture_portal_service_manage_subscription(VenturePortalService *self, const gchar *token,
	gint64 subscription_id, const gchar *action, gint64 plan_price_id, gboolean now,
	const VentureActor *actor, GError **error);
G_END_DECLS
#endif
