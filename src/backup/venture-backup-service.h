/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BACKUP_SERVICE_H
#define VENTURE_BACKUP_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BACKUP_SERVICE (venture_backup_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBackupService, venture_backup_service, VENTURE, BACKUP_SERVICE, GObject)
/**
 * venture_backup_service_get:
 * @database: the owning database
 *
 * Returns: (transfer none): the per-database service
 */
VentureBackupService *venture_backup_service_get(VentureDatabase *database);
/**
 * venture_backup_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Checks service ownership and lifecycle restrictions before a generic write.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_backup_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_backup_service_export:
 * @self: the service
 * @organization_id: legal entity
 * @format: (nullable): json or csv
 * @actor: (nullable)
 * @error: (out) (optional)
 *
 * Exports a version 4 consistent accounting snapshot, including historical
 * document state and references. Sensitive fields are excluded. CSV is a
 * type/count summary, not a restorable archive.
 *
 * Returns: (transfer full) (nullable): the backup record
 */
VentureEntity *venture_backup_service_export(VentureBackupService *self, gint64 organization_id,
	const gchar *format, const VentureActor *actor, GError **error);
/**
 * venture_backup_service_restore:
 * @self: the service
 * @organization_id: empty destination org
 * @payload: JSON pack
 * @actor: (nullable)
 * @error: (out) (optional)
 *
 * Restores a version 4 snapshot atomically into an empty organization. IDs and
 * UUIDs are remapped; historical financial operations are not replayed. External
 * references must resolve by UUID. Version 3 supports only manual ledger packs.
 *
 * Returns: TRUE on success, FALSE with no committed restore changes on failure
 */
gboolean venture_backup_service_restore(VentureBackupService *self, gint64 organization_id,
	const gchar *payload, const VentureActor *actor, GError **error);
/**
 * venture_backup_actions_register:
 * @database: database owning the records
 */
void venture_backup_actions_register(VentureDatabase *database);
/**
 * venture_backup_service_snapshot:
 * @self: the service
 * @organization_id: legal entity
 * @error: (out) (optional)
 *
 * The same version 4 snapshot venture_backup_service_export() stores, as
 * JSON text and without an accounting_backup record: for writing to a file.
 *
 * Returns: (transfer full) (nullable): the archive
 */
gchar *venture_backup_service_snapshot(VentureBackupService *self, gint64 organization_id, GError **error);
G_END_DECLS
#endif
