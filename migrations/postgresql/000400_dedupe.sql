-- The candidate table comes from the dedupe module field table, and the
-- merged_into_id columns on companies and contacts from additive metadata.
-- Verify the CRM columns are present together (or absent with the CRM off),
-- and the candidate table is present or, with the module disabled, absent.
-- Nothing is inferred or backfilled.
CREATE TEMP TABLE venture_dedupe_upgrade_guard (column_count INTEGER CHECK (column_count IN (0, 2)));
INSERT INTO venture_dedupe_upgrade_guard SELECT COUNT(*) FROM information_schema.columns
 WHERE table_schema = current_schema() AND column_name = 'merged_into_id'
 AND table_name IN ('companies', 'contacts');
DROP TABLE venture_dedupe_upgrade_guard;
CREATE TEMP TABLE venture_dedupe_table_guard (table_count INTEGER CHECK (table_count IN (0, 1)));
INSERT INTO venture_dedupe_table_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name = 'duplicate_candidates';
DROP TABLE venture_dedupe_table_guard;
