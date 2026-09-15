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
G_END_DECLS
#endif
