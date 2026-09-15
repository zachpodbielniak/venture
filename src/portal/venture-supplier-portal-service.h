/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SUPPLIER_PORTAL_SERVICE_H
#define VENTURE_SUPPLIER_PORTAL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_supplier_portal_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_supplier_portal_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_portal_service_invite_supplier:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @company_id: company id
 * @email: invitation email address
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_portal_service_invite_supplier(VenturePortalService *self, gint64 organization_id,
	gint64 company_id, const gchar *email, const VentureActor *actor, GError **error);
/**
 * venture_portal_service_revoke_supplier:
 * @self: the service or registry instance
 * @access: portal invitation record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_portal_service_revoke_supplier(VenturePortalService *self, VentureSupplierPortalAccess *access,
	const VentureActor *actor, GError **error);
/**
 * venture_portal_service_lookup_supplier:
 * @self: the service or registry instance
 * @token: access token
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_portal_service_lookup_supplier(VenturePortalService *self, const gchar *token, GError **error);
/**
 * venture_portal_service_bills:
 * @self: the service or registry instance
 * @access: portal invitation record
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (element-type VentureEntity) (nullable): owned result
 */
GPtrArray *venture_portal_service_bills(VenturePortalService *self, VentureSupplierPortalAccess *access,
	GError **error);
/**
 * venture_supplier_portal_actions_register:
 * @database: database owning the records
 */
void venture_supplier_portal_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
