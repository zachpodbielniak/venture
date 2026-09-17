-- Overdue-reminder tables come from the dunning module field tables, and
-- the invoice/company override columns from additive metadata. Verify the
-- pair is present together, or absent when the module is disabled.
CREATE TEMP TABLE venture_dunning_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_dunning_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('dunning_policies', 'dunning_events');
DROP TABLE venture_dunning_upgrade_guard;
