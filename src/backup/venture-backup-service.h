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
 * Versioned pack of chart, journals, invoices, frozen tax, receipts and allocations.
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
 * Remaps identities. Settlement journals are rebuilt from invoices and receipts.
 */
gboolean venture_backup_service_restore(VentureBackupService *self, gint64 organization_id,
	const gchar *payload, const VentureActor *actor, GError **error);
void venture_backup_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
