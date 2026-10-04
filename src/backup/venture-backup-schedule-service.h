/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BACKUP_SCHEDULE_SERVICE_H
#define VENTURE_BACKUP_SCHEDULE_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_BACKUP_SCHEDULE_SERVICE (venture_backup_schedule_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBackupScheduleService, venture_backup_schedule_service, VENTURE, BACKUP_SCHEDULE_SERVICE, GObject)
/**
 * venture_backup_schedule_service_get:
 * @database: the owning database
 *
 * The one place scheduled backups, their retention, verification and the
 * restore drill happen. Installs the backup_schedule save validator.
 *
 * Returns: (transfer none): the per-database service
 */
VentureBackupScheduleService *venture_backup_schedule_service_get(VentureDatabase *database);
/**
 * venture_backup_schedule_check_write:
 * @database: database owning the records
 * @record: candidate record
 * @removal: whether this is a removal operation
 * @error: (out) (optional): return location for an error
 *
 * Refuses generic writes and removals of backup_run: a run is evidence and
 * only the service records or prunes one.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_backup_schedule_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
/**
 * venture_backup_schedule_service_run:
 * @self: the service
 * @schedule: a saved backup_schedule
 * @as_of: (nullable): the run time; now when omitted
 * @actor: (nullable): attribution
 * @error: (out) (optional): return location for an error
 *
 * Writes one backup for @schedule: a version-4 accounting snapshot for an
 * organization schedule, a copy of the whole database for an installation
 * schedule. Records a backup_run with size, SHA-256 and status, verifies it
 * when the schedule says so, then prunes successful runs beyond retention,
 * oldest first. A failed run is recorded with status =failed= and prunes
 * nothing.
 *
 * Returns: (transfer full) (nullable): the succeeded run, or NULL with the
 * failure recorded on a failed run
 */
VentureEntity *venture_backup_schedule_service_run(VentureBackupScheduleService *self, VentureBackupSchedule *schedule,
	GDateTime *as_of, const VentureActor *actor, GError **error);
/**
 * venture_backup_schedule_service_run_due:
 * @self: the service
 * @organization_id: legal entity whose schedules to dispatch, or 0 for every one
 * @as_of: (nullable): now when omitted
 * @actor: (nullable): attribution
 * @error: (out) (optional): return location for an error
 *
 * Runs every schedule whose cron-like expression is due, the way scheduled
 * report packs are dispatched, and stamps last-run-at. A failing backup is
 * recorded on its run and does not stop the sweep.
 *
 * Returns: number of schedules dispatched, or -1
 */
gint venture_backup_schedule_service_run_due(VentureBackupScheduleService *self, gint64 organization_id,
	GDateTime *as_of, const VentureActor *actor, GError **error);
/**
 * venture_backup_schedule_service_verify:
 * @self: the service
 * @run: a succeeded backup_run
 * @actor: (nullable): attribution
 * @error: (out) (optional): return location for an error
 *
 * Restores the run's snapshot into a temporary empty database and compares
 * its trial balance, ledger totals and document counts with the live
 * organization. The result (=tie= or =mismatch=) and the comparison report
 * are recorded on @run.
 *
 * Returns: TRUE when the comparison completed, FALSE when it could not run
 */
gboolean venture_backup_schedule_service_verify(VentureBackupScheduleService *self, VentureBackupRun *run,
	const VentureActor *actor, GError **error);
/**
 * venture_backup_schedule_service_restore_drill:
 * @self: the service
 * @run: the backup to restore
 * @name: (nullable): a name for the drill
 * @actor: (nullable): attribution
 * @error: (out) (optional): return location for an error
 *
 * The same restore and tie-out as verification, kept as its own named
 * backup_run of kind =drill= whose report is retained.
 *
 * Returns: (transfer full) (nullable): the drill run
 */
VentureEntity *venture_backup_schedule_service_restore_drill(VentureBackupScheduleService *self, VentureBackupRun *run,
	const gchar *name, const VentureActor *actor, GError **error);
/**
 * venture_backup_schedule_service_latest_run:
 * @self: the service
 * @organization_id: legal entity
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): the most recent succeeded, unpruned
 * backup of @organization_id, or NULL with a not-found error
 */
VentureEntity *venture_backup_schedule_service_latest_run(VentureBackupScheduleService *self, gint64 organization_id,
	GError **error);
/**
 * VentureBackupCompanionFunc:
 * @self: the service
 * @run: the installation run that just succeeded
 * @directory: where its file was written; companion files go beside it
 * @actor: (nullable): who asked for the backup
 * @user_data: what venture_backup_schedule_service_add_companion() was given
 *
 * Called on the main thread after an installation copy succeeds, to back up
 * files that live outside the database but belong to the installation --
 * the market-data series stores. It must not block on the copy: it records
 * each file as a running run with
 * venture_backup_schedule_service_begin_companion(), hands the copy to its
 * own thread and finishes the run later with
 * venture_backup_schedule_service_finish_companion(). It cannot fail the
 * installation run; a failure belongs on the companion's own run.
 */
typedef void (*VentureBackupCompanionFunc)(VentureBackupScheduleService *self, VentureBackupRun *run,
	const gchar *directory, const VentureActor *actor, gpointer user_data);
/**
 * venture_backup_schedule_service_add_companion:
 * @self: the service
 * @name: the companion's name; adding one under a name already held replaces it
 * @func: (scope notified): called after each installation copy
 * @user_data: (closure): passed to @func
 * @destroy: (nullable): frees @user_data
 *
 * Registers files an installation backup also covers. The backup module
 * knows nothing about what they are, which is what keeps it building and
 * working without the modules that own them.
 */
void venture_backup_schedule_service_add_companion(VentureBackupScheduleService *self, const gchar *name,
	VentureBackupCompanionFunc func, gpointer user_data, GDestroyNotify destroy);
/**
 * venture_backup_schedule_service_begin_companion:
 * @self: the service
 * @parent: the installation run the file is taken with
 * @scope: what the file is, e.g. =series=; never =organization= or =installation=
 * @name: the run's name, shown on the Backups page
 * @store_uuid: (nullable): which store or file set it copies, the key its
 *   retention is counted by
 * @actor: (nullable): attribution
 * @error: (out) (optional): return location for an error
 *
 * Records a companion copy as started: a backup_run of kind =backup= and
 * status =running=, under @parent's schedule and organization, naming
 * @parent in =parent-run-id=.
 *
 * Returns: (transfer full) (nullable): the running run
 */
VentureEntity *venture_backup_schedule_service_begin_companion(VentureBackupScheduleService *self, VentureBackupRun *parent,
	const gchar *scope, const gchar *name, const gchar *store_uuid, const VentureActor *actor, GError **error);
/**
 * venture_backup_schedule_service_finish_companion:
 * @self: the service
 * @run_id: a run venture_backup_schedule_service_begin_companion() made, still running
 * @path: (nullable): the file written; ignored on failure
 * @sha256: (nullable): its digest; ignored on failure
 * @size: its size in bytes
 * @failure: (nullable): why the copy failed, or %NULL when it succeeded
 * @actor: (nullable): attribution
 * @error: (out) (optional): return location for an error
 *
 * Stamps a companion run =succeeded= with its path, size and digest, then
 * prunes the schedule's succeeded runs of the same scope and store beyond
 * retention, oldest first -- the installation copy's own rule -- or stamps
 * it =failed= with @failure and prunes nothing. A run that is no longer
 * running is refused, so an answer cannot land twice.
 *
 * Returns: TRUE when the run was stamped
 */
gboolean venture_backup_schedule_service_finish_companion(VentureBackupScheduleService *self, gint64 run_id,
	const gchar *path, const gchar *sha256, gint64 size, const gchar *failure, const VentureActor *actor, GError **error);
/**
 * venture_backup_schedule_service_set_config:
 * @self: the service
 * @config: (nullable): configuration supplying backup.directory and backup.retention
 *
 * Without a configuration the defaults are the process working directory's
 * =backups= subdirectory and seven copies.
 */
void venture_backup_schedule_service_set_config(VentureBackupScheduleService *self, VentureConfig *config);
/**
 * venture_backup_schedule_actions_register:
 * @database: database owning the records
 */
void venture_backup_schedule_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
