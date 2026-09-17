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
 * venture_deferral_service_cancel_invoice:
 * @self: service
 * @invoice_id: source invoice being voided
 * @date: reversal date
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 *
 * Stops remaining recognition and reverses posted recognition atomically.
 * Returns: %TRUE on success
 */
gboolean venture_deferral_service_cancel_invoice(VentureDeferralService *self,
 gint64 invoice_id, GDateTime *date, const VentureActor *actor, GError **error);
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
/**
 * venture_asset_service_run_tax_period:
 * @self: the service or registry instance
 * @period: reporting period
 * @organization_id: target legal entity ID
 * @dry_run: whether to calculate without applying writes
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: number processed, or -1 on failure
 */
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
/**
 * venture_asset_service_import_opening:
 * @self: service
 * @asset: saved draft carrying the source register's cost, dates, method and life
 * @accumulated: depreciation already taken in the source system up to @cutoff
 * @cutoff: migration cutoff; months up to and including the cutoff month are history
 * @equity_account_id: opening-balance equity account credited with net book value
 * @source_type: source record type stamped on the opening journal
 * @source_id: source record ID stamped on the opening journal
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 *
 * Places a migrated asset in service without recomputing its history. The
 * accumulated amount is posted once, at the cutoff, as an opening journal
 * (asset cost against accumulated depreciation and equity) and recorded as a
 * posted opening depreciation entry. The remaining depreciable amount is
 * scheduled over the months left after the cutoff.
 * Returns: %TRUE if the opening journal, schedule and state committed atomically
 */
gboolean venture_asset_service_import_opening(VentureAssetService *self, VentureEntity *asset,
 const VentureMoney *accumulated, GDateTime *cutoff, gint64 equity_account_id,
 const gchar *source_type, gint64 source_id, const VentureActor *actor, GError **error);
/**
 * venture_asset_service_rollback_opening:
 * @self: service
 * @asset_id: an asset placed through venture_asset_service_import_opening()
 * @date: rollback date
 * @tag_suffix: (nullable): appended to the tag so a corrected import can reuse it
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure details
 *
 * Retires a migrated asset after its opening journal has been reversed by the
 * caller: remaining scheduled entries become skipped and the asset is written
 * off at @date without a second journal. Posted history is retained.
 * Returns: %TRUE on success
 */
gboolean venture_asset_service_rollback_opening(VentureAssetService *self, gint64 asset_id,
 GDateTime *date, const gchar *tag_suffix, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
