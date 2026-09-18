-- Deal lines come from the pipelines module field table; tracked links and
-- tracking events from the sequences module field tables. Each module's
-- tables are additive and present together, or absent when that module is
-- disabled. Verify both invariants; nothing is backfilled.
CREATE TEMP TABLE venture_deal_lines_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 1)));
INSERT INTO venture_deal_lines_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('deal_lines');
DROP TABLE venture_deal_lines_upgrade_guard;
CREATE TEMP TABLE venture_sequence_tracking_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_sequence_tracking_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('sequence_links', 'sequence_tracking_events');
DROP TABLE venture_sequence_tracking_upgrade_guard;
