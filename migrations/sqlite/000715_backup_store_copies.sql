-- requires-table: backup_runs
-- An installation backup now copies each market-data series store beside
-- the database, one backup_run of scope "series" per store, naming the run
-- it was taken with (parent_run_id) and the store it copies (store_uuid).
-- Both columns come from the field table and reconciliation adds them. A
-- run recorded before this release is a database copy or a snapshot, whose
-- parent and store are rightly empty, so nothing is backfilled: selecting
-- the two columns is the check that the table is the one this build reads.
CREATE TEMP TABLE venture_backup_store_copies_guard AS
 SELECT parent_run_id, store_uuid FROM backup_runs WHERE 1 = 0;
DROP TABLE venture_backup_store_copies_guard;
