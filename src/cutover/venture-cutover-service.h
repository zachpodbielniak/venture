/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CUTOVER_SERVICE_H
#define VENTURE_CUTOVER_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CUTOVER_SERVICE (venture_cutover_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCutoverService, venture_cutover_service, VENTURE, CUTOVER_SERVICE, GObject)
VentureCutoverService *venture_cutover_service_get(VentureDatabase *database);
VentureEntity *venture_cutover_service_preview(VentureCutoverService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error);
gboolean venture_cutover_service_import(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
gboolean venture_cutover_service_reconcile(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
gboolean venture_cutover_service_activate(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
gboolean venture_cutover_service_rollback(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
void venture_cutover_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
