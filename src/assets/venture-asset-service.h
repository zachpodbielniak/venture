/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ASSET_SERVICE_H
#define VENTURE_ASSET_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ASSET_SERVICE (venture_asset_service_get_type())
G_DECLARE_FINAL_TYPE(VentureAssetService, venture_asset_service, VENTURE, ASSET_SERVICE, GObject)

/**
 * venture_asset_service_get:
 * @database: owning repository
 * Returns: (transfer none): the repository's asset service
 */
VentureAssetService *venture_asset_service_get(VentureDatabase *database);
/**
 * venture_asset_service_place_in_service:
 * @self: service
 * @asset: saved draft, with in-service date and account choices
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 * Returns: %TRUE if the full schedule and state committed atomically
 */
gboolean venture_asset_service_place_in_service(VentureAssetService *self,
 VentureEntity *asset, const VentureActor *actor, GError **error);
/**
 * venture_assets_save:
 * @database: repository
 * @entity: proposed record
 * @actor: (nullable): audit actor
 * @handled: (out): whether the service performed the operation
 * @error: (out) (optional): failure details
 * Returns: %TRUE on success
 */
gboolean venture_assets_save(VentureDatabase *database, VentureEntity *entity,
 const VentureActor *actor, gboolean *handled, GError **error);
G_END_DECLS
#endif
