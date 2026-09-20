/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROJECT_SERVICE_H
#define VENTURE_PROJECT_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PROJECT_SERVICE (venture_project_service_get_type())
G_DECLARE_FINAL_TYPE(VentureProjectService, venture_project_service, VENTURE, PROJECT_SERVICE, GObject)
/**
 * venture_project_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureProjectService *venture_project_service_get(VentureDatabase *database);
/**
 * venture_project_service_save:
 * @self: the service or registry instance
 * @record: candidate record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_project_service_save(VentureProjectService *self, VentureEntity *record, const VentureActor *actor, GError **error);
/**
 * venture_project_service_approve_time:
 * @self: the service or registry instance
 * @time: time entry
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Freezes billable and actual labour cost using the selected same-project
 * rate under the database lock. Both rates and occurrence time are required.
 * Existing approved evidence is immutable, including historical unknown cost.
 * A failed save leaves @time unchanged.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_project_service_approve_time(VentureProjectService *self, VentureEntity *time, const VentureActor *actor, GError **error);
/**
 * venture_project_service_bill:
 * @self: the service or registry instance
 * @project_id: project id
 * @date: effective date
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_project_service_bill(VentureProjectService *self, gint64 project_id, GDateTime *date, const VentureActor *actor, GError **error);
/**
 * venture_projects_save_hook:
 * @database: database owning the records
 * @record: candidate record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @handled: (out): whether the service performed the write
 * @error: (out) (optional): return location for an error
 *
 * Routes generic writes through the subsystem operation when required.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_projects_save_hook(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_projects_register_reports:
 * @registry: registry receiving the registrations
 */
void venture_projects_register_reports(VentureReportRegistry *registry);
/**
 * venture_projects_check_write:
 * @database: database containing the evidence
 * @record: proposed record
 * @removal: whether this is a delete, restore or purge operation
 * @error: (out) (optional): return location for an error
 *
 * Retains billing allocations and approved or billed source evidence across
 * every lifecycle path, checking the stored row as well as the proposal.
 *
 * Returns: TRUE if the lifecycle operation is permitted
 */
gboolean venture_projects_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_projects_actions_register:
 * @database: database receiving the actions
 *
 * Registers record-level approval and billing actions for generic web, REST,
 * CLI and staged assistant surfaces. Called once by registry initialization.
 */
void venture_projects_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
