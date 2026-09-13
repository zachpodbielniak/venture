/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_AUTOJOURNAL_SERVICE_H
#define VENTURE_AUTOJOURNAL_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_AUTOJOURNAL_SERVICE (venture_autojournal_service_get_type())
G_DECLARE_FINAL_TYPE(VentureAutojournalService, venture_autojournal_service, VENTURE, AUTOJOURNAL_SERVICE, GObject)
/**
 * venture_database_get_autojournal_service:
 * @database: owning database
 * Returns: (transfer none): service owned through the database's rule registry
 */
VentureAutojournalService *venture_database_get_autojournal_service(VentureDatabase *database);
/**
 * venture_autojournal_service_profile:
 * @self: service
 * @organization_id: exact legal entity
 * @error: (out) (optional): error location
 * Returns: (transfer full) (nullable): persisted profile, seeded on first use
 */
VenturePostingProfile *venture_autojournal_service_profile(VentureAutojournalService *self, gint64 organization_id, GError **error);
/**
 * venture_autojournal_save_hook: (skip)
 * @database: database
 * @entity: source
 * @actor: (nullable): audit actor
 * @handled: (out): whether this service handled the save
 * @error: (out) (optional): error location
 * Returns: TRUE on success
 */
gboolean venture_autojournal_save_hook(VentureDatabase *database, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error);
/* One connection at the pre-save boundary, retaining the caller's actor. */
#define VENTURE_AUTOJOURNAL_SAVE_HOOK(db, entity, actor, error) \
	G_STMT_START { gboolean aj_handled; gboolean aj_ok; \
	aj_ok = venture_autojournal_save_hook(db, entity, actor, &aj_handled, error); \
	if (aj_handled || !aj_ok) return aj_ok; } G_STMT_END
/**
 * venture_autojournal_service_unposted:
 * @self: service
 * @organization_id: exact legal entity
 * @error: (out) (optional): error location
 * Returns: (transfer full) (element-type VentureEntity) (nullable): sources in date order
 */
GPtrArray *venture_autojournal_service_unposted(VentureAutojournalService *self, gint64 organization_id, GError **error);
/**
 * venture_autojournal_service_backfill:
 * @self: service
 * @organization_id: exact legal entity
 * @dry_run: validate the complete batch then roll it back
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error location
 * Returns: (transfer full) (nullable): candidate, posted and skipped counts
 */
JsonNode *venture_autojournal_service_backfill(VentureAutojournalService *self, gint64 organization_id,
	gboolean dry_run, const VentureActor *actor, GError **error);
/**
 * venture_autojournal_register_reports:
 * @registry: report registry
 */
void venture_autojournal_register_reports(VentureReportRegistry *registry);
G_END_DECLS
#endif
