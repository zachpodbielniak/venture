-- Scheduled-backup tables come from the backup module field tables:
-- backup_schedules (operator configuration) and backup_runs (evidence).
-- Verify the pair is present together, or absent when the module is
-- disabled; nothing is backfilled, since a run that never happened has
-- no evidence to invent.
CREATE TEMP TABLE venture_backup_schedule_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_backup_schedule_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('backup_schedules', 'backup_runs');
DROP TABLE venture_backup_schedule_upgrade_guard;
