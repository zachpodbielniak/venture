/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PORTAL_SERVICE_H
#define VENTURE_PORTAL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PORTAL_SERVICE (venture_portal_service_get_type())
G_DECLARE_FINAL_TYPE(VenturePortalService, venture_portal_service, VENTURE, PORTAL_SERVICE, GObject)
VenturePortalService *venture_portal_service_get(VentureDatabase *database);
gboolean venture_portal_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
VentureEntity *venture_portal_service_invite(VenturePortalService *self, gint64 organization_id,
	gint64 company_id, const gchar *email, const VentureActor *actor, GError **error);
gboolean venture_portal_service_revoke(VenturePortalService *self, VentureCustomerPortalAccess *access,
	const VentureActor *actor, GError **error);
VentureEntity *venture_portal_service_lookup(VenturePortalService *self, const gchar *token, GError **error);
gboolean venture_portal_service_pay(VenturePortalService *self, VentureCustomerPortalAccess *access,
	gint64 invoice_id, const VentureMoney *amount, const VentureActor *actor, GError **error);
GPtrArray *venture_portal_service_invoices(VenturePortalService *self, VentureCustomerPortalAccess *access,
	GError **error);
void venture_portal_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
