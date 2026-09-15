/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SUPPLIER_PORTAL_SERVICE_H
#define VENTURE_SUPPLIER_PORTAL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
gboolean venture_supplier_portal_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
VentureEntity *venture_portal_service_invite_supplier(VenturePortalService *self, gint64 organization_id,
	gint64 company_id, const gchar *email, const VentureActor *actor, GError **error);
gboolean venture_portal_service_revoke_supplier(VenturePortalService *self, VentureSupplierPortalAccess *access,
	const VentureActor *actor, GError **error);
VentureEntity *venture_portal_service_lookup_supplier(VenturePortalService *self, const gchar *token, GError **error);
GPtrArray *venture_portal_service_bills(VenturePortalService *self, VentureSupplierPortalAccess *access,
	GError **error);
void venture_supplier_portal_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
