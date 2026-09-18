/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BACKUP_RECORDS_H
#define VENTURE_BACKUP_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACCOUNTING_BACKUP (venture_accounting_backup_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingBackup, venture_accounting_backup, ACCOUNTING_BACKUP)
#define VENTURE_TYPE_BACKUP_SCHEDULE (venture_backup_schedule_get_type())
VENTURE_DECLARE_ENTITY(VentureBackupSchedule, venture_backup_schedule, BACKUP_SCHEDULE)
#define VENTURE_TYPE_BACKUP_RUN (venture_backup_run_get_type())
VENTURE_DECLARE_ENTITY(VentureBackupRun, venture_backup_run, BACKUP_RUN)
G_END_DECLS
#endif
