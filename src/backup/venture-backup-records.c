/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static const VentureFieldDecl backup_fields[] = {
	VENTURE_FIELD("format", "Format", "json or csv", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_TEXT("payload", "Payload", "Exported accounting pack"),
	VENTURE_FIELD("state", "State", "exported or restored", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("checksum", "Checksum", "SHA-256 of the payload", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAccountingBackup, venture_accounting_backup, backup_fields)

/* A schedule is operator configuration and stays generically editable; the
 * cron expression is validated by the schedule service on every save. */
static const VentureFieldDecl backup_schedule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Scheduled backup"),
	VENTURE_FIELD("scope", "Scope", "organization (a version-4 accounting snapshot) or installation (the whole database)",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("schedule", "Schedule", "Five numeric or * cron fields, or daily; interpreted in UTC. Empty means manual only",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("retention", "Retention", "How many successful backups to keep; older files are pruned oldest-first. Zero uses backup.retention",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("destination", "Destination", "Directory the files are written to. Empty uses backup.directory",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("verify", "Verify", "Restore every scheduled snapshot into an empty database and tie it out",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-run-at", "Last run", "When the sweep last dispatched this schedule",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBackupSchedule, venture_backup_schedule, backup_schedule_fields)

/* A run is evidence and is written only by VentureBackupScheduleService. */
static const VentureFieldDecl backup_run_fields[] = {
	VENTURE_FIELD_REF("schedule-id", "Schedule", "Schedule that produced this run; empty for a drill on an ad-hoc run",
		"backup_schedule", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("kind", "Kind", "backup or drill", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("name", "Name", "Schedule name, or the drill's name",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("scope", "Scope", "organization or installation", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("path", "Path", "Where the file was written", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("started-at", "Started", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("finished-at", "Finished", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("size", "Size", "Bytes written", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sha256", "SHA-256", "Digest of the file as written", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("status", "Status", "running, succeeded, failed or pruned", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("message", "Message", "Why a run failed"),
	VENTURE_FIELD("verified-at", "Verified", "When the snapshot was last restored and compared",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("verification", "Verification", "tie, mismatch, or empty when never verified",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("report", "Report", "JSON comparison of trial balance, ledger totals and document counts",
		VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("source-run-id", "Source run", "The backup a drill restored", "backup_run", VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureBackupRun, venture_backup_run, backup_run_fields)
