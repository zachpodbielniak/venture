/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CRM_IMPORT_SERVICE_H
#define VENTURE_CRM_IMPORT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CRM_IMPORT_SERVICE (venture_crm_import_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCrmImportService, venture_crm_import_service, VENTURE, CRM_IMPORT_SERVICE, GObject)
/**
 * venture_crm_import_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureCrmImportService *venture_crm_import_service_get(VentureDatabase *database);
/**
 * venture_crm_import_service_preview:
 * @self: the service
 * @organization_id: target legal entity ID
 * @manifest: mapped manifest naming the source, the CSV text per object,
 *   the stage map and optional column, owner and currency settings
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Validates the manifest, the column maps and every deal stage, and
 * records a batch in the preview state with one row per source row.
 * Nothing but the batch and its rows is written; a refusal writes nothing.
 * A migration belongs to one legal entity: @organization_id of zero
 * (the "all entities" selection) is refused.
 *
 * Returns: (transfer full) (nullable): the crm_import batch
 */
VentureEntity *venture_crm_import_service_preview(VentureCrmImportService *self, gint64 organization_id,
	JsonObject *manifest, const VentureActor *actor, GError **error);
/**
 * venture_crm_import_service_import:
 * @self: the service
 * @batch: a previewed or imported batch
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Creates or matches companies, contacts, deals, history and next actions
 * in one transaction. Rows already carrying a record are skipped, so a
 * rerun is idempotent.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_crm_import_service_import(VentureCrmImportService *self, VentureCrmImport *batch,
	const VentureActor *actor, GError **error);
/**
 * venture_crm_import_service_activate:
 * @self: the service
 * @batch: an imported batch
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Accepts the import. An active batch can no longer be rolled back.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_crm_import_service_activate(VentureCrmImportService *self, VentureCrmImport *batch,
	const VentureActor *actor, GError **error);
/**
 * venture_crm_import_service_rollback:
 * @self: the service
 * @batch: a previewed or imported batch
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Deletes exactly the records this batch created, leaves matched records
 * untouched and marks every row rolled back. Refused once activated;
 * repeating a rollback is a no-op.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_crm_import_service_rollback(VentureCrmImportService *self, VentureCrmImport *batch,
	const VentureActor *actor, GError **error);
/**
 * venture_crm_import_default_columns:
 * @source: hubspot, zoho_crm or salesforce
 * @object: companies, contacts, deals, notes or tasks
 *
 * The built-in column map for a vendor's default export headers, keyed by
 * canonical field name. A manifest's =columns= member overrides entries.
 *
 * Returns: (transfer full) (nullable): the map, or %NULL for an unknown pair
 */
JsonObject *venture_crm_import_default_columns(const gchar *source, const gchar *object);
/**
 * venture_crm_import_check_write: (skip)
 * @database: repository
 * @record: candidate write
 * @removal: TRUE when deleting
 * @error: (out) (optional): location
 * Returns: TRUE if the write is service-owned or not a crm_import record
 */
gboolean venture_crm_import_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_crm_import_actions_register:
 * @database: database owning the records
 *
 * Registers the import, activate and rollback actions on crm_import.
 */
void venture_crm_import_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
