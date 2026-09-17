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
 * venture_cutover_service_rollback_preflight:
 * @self: the service or registry instance
 * @cutover: cutover record
 * @error: (out) (optional): return location for an error
 *
 * Walks the rollback without writing and lists every record or date that
 * would stop it: credit notes from outside the batch applied to imported
 * documents, imported credits used outside the batch, cash refunds of
 * imported prepayments, disposed assets and unwinding dates in closed or
 * locked periods. An empty array means rollback can proceed.
 *
 * Returns: (transfer full) (element-type utf8) (nullable): the blockers, or %NULL on failure
 */
GPtrArray *venture_cutover_service_rollback_preflight(VentureCutoverService *self,
	VentureAccountingCutover *cutover, GError **error);
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
 * venture_cutover_service_payload_from_csv:
 * @self: the service or registry instance
 * @settings: (nullable): source, cutoff, currency, decimal_separator, thousands_separator and clearing_account_code
 * @tables: (element-type utf8 GPtrArray) (nullable): section name to its parsed CSV rows, header row first,
 *   each row a %NULL-terminated string array
 * @error: (out) (optional): return location for an error
 *
 * Builds a batch payload from one CSV per section: customers, vendors, chart,
 * account_map, open_ar, open_ap (one row per bill line), credits,
 * bank_balances, assets and trial_balance. Cells become strings and every
 * payload rule still applies at preview. Unknown columns, sections and
 * conflicting bill rows are refused together.
 *
 * Returns: (transfer full) (nullable): the payload
 */
JsonObject *venture_cutover_service_payload_from_csv(VentureCutoverService *self, JsonObject *settings,
	GHashTable *tables, GError **error);
/**
 * venture_cutover_csv_template:
 * @section: a CSV section name
 *
 * Returns: (transfer full) (nullable): the section's header row, or %NULL for an unknown section
 */
gchar *venture_cutover_csv_template(const gchar *section);
/**
 * venture_cutover_csv_sections:
 *
 * Returns: (transfer full): the CSV section names, in template order
 */
gchar **venture_cutover_csv_sections(void);
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
