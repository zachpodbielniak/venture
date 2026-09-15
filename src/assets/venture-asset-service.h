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
#define VENTURE_TYPE_DEFERRAL_SERVICE (venture_deferral_service_get_type())
G_DECLARE_FINAL_TYPE(VentureDeferralService, venture_deferral_service, VENTURE, DEFERRAL_SERVICE, GObject)
/**
 * venture_deferral_service_new:
 * @database: repository
 * Returns: (transfer full): a deferral service
 */
VentureDeferralService *venture_deferral_service_new(VentureDatabase *database);
/**
 * venture_deferral_service_schedule:
 * @self: service
 * @deferral: new source record
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 * Returns: %TRUE if source and schedule committed
 */
gboolean venture_deferral_service_schedule(VentureDeferralService *self, VentureEntity *deferral,
 const VentureActor *actor, GError **error);
/**
 * venture_asset_service_run_period:
 * @self: service
 * @period: YYYY-MM
 * @organization_id: exact legal entity
 * @dry_run: validate without writing
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 * Returns: number of scheduled rows, or -1 on failure; one transaction per run
 */
gint venture_asset_service_run_period(VentureAssetService *self, const gchar *period,
 gint64 organization_id, gboolean dry_run, const VentureActor *actor, GError **error);
gint venture_asset_service_run_tax_period(VentureAssetService *self, const gchar *period,
 gint64 organization_id, gboolean dry_run, const VentureActor *actor, GError **error);
/**
 * venture_assets_check_removal:
 * @database: repository holding the authoritative state
 * @entity: record being deleted, restored or purged
 * @error: (out) (optional): refusal details
 * Returns: %TRUE for removable draft assets or unrelated records
 */
gboolean venture_assets_check_removal(VentureDatabase *database, VentureEntity *entity, GError **error);
/**
 * venture_asset_service_dispose:
 * @self: service
 * @asset: current asset with disposal date and proceeds
 * @write_off: recognize the whole book value as loss with zero proceeds
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 * Returns: %TRUE if disposal journal, schedule and state committed
 */
gboolean venture_asset_service_dispose(VentureAssetService *self, VentureEntity *asset,
 gboolean write_off, const VentureActor *actor, GError **error);
/**
 * venture_asset_service_create_from_expense:
 * @self: service
 * @expense: saved capital expense
 * @tag: organization-unique asset tag
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 * Returns: (transfer full) (nullable): a saved draft asset
 */
VentureEntity *venture_asset_service_create_from_expense(VentureAssetService *self,
 VentureEntity *expense, const gchar *tag, const VentureActor *actor, GError **error);
/**
 * venture_assets_register_reports:
 * @registry: report registry
 * Registers the financial asset register and remaining-deferral report.
 */
void venture_assets_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
