/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CUTOVER_SERVICE_H
#define VENTURE_CUTOVER_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CUTOVER_SERVICE (venture_cutover_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCutoverService, venture_cutover_service, VENTURE, CUTOVER_SERVICE, GObject)
/**
 * venture_cutover_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureCutoverService *venture_cutover_service_get(VentureDatabase *database);
/**
 * venture_cutover_service_preview:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @payload: import payload
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_cutover_service_preview(VentureCutoverService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error);
/**
 * venture_cutover_service_import:
 * @self: the service or registry instance
 * @cutover: cutover record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_cutover_service_import(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
/**
 * venture_cutover_service_reconcile:
 * @self: the service or registry instance
 * @cutover: cutover record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_cutover_service_reconcile(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
/**
 * venture_cutover_service_activate:
 * @self: the service or registry instance
 * @cutover: cutover record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_cutover_service_activate(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
/**
 * venture_cutover_service_rollback:
 * @self: the service or registry instance
 * @cutover: cutover record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_cutover_service_rollback(VentureCutoverService *self, VentureAccountingCutover *cutover,
	const VentureActor *actor, GError **error);
/**
 * venture_cutover_check_write: (skip)
 * @database: repository
 * @record: candidate write
 * @removal: TRUE when deleting
 * @error: (out) (optional): location
 * Returns: TRUE if the write is service-owned or not a cutover record
 */
gboolean venture_cutover_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_cutover_actions_register:
 * @database: database owning the records
 */
void venture_cutover_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
