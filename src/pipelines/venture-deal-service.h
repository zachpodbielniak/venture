/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DEAL_SERVICE_H
#define VENTURE_DEAL_SERVICE_H
G_BEGIN_DECLS
#define VENTURE_TYPE_DEAL_SERVICE (venture_deal_service_get_type())
G_DECLARE_FINAL_TYPE(VentureDealService, venture_deal_service, VENTURE, DEAL_SERVICE, GObject)
/**
 * venture_database_get_deal_service:
 * @database: owning database
 * Returns: (transfer none): the canonical transition service
 */
VentureDealService *venture_database_get_deal_service(VentureDatabase *database);
/**
 * venture_deal_service_move_stage:
 * @self: the service
 * @deal: persisted opportunity, at its current version
 * @stage_id: destination stage
 * @note: (nullable): transition explanation
 * @actor: (nullable): responsible user
 * @error: (out) (optional): refusal
 *
 * Atomically saves the transition and history. Inputs remain unchanged.
 * Returns: (transfer full) (nullable): the updated deal
 */
VentureDeal *venture_deal_service_move_stage(VentureDealService *self, VentureDeal *deal,
	gint64 stage_id, const gchar *note, const VentureActor *actor, GError **error);
/**
 * venture_deal_service_ensure_default:
 * @self: the service
 * @organization_id: owning organization
 * @error: (out) (optional): refusal
 * Returns: the default pipeline id, or zero on failure
 */
gint64 venture_deal_service_ensure_default(VentureDealService *self, gint64 organization_id, GError **error);
G_END_DECLS
#endif
