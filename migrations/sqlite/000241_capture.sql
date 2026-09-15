-- Capture inbox tables come from field tables.
-- Both a disabled fresh module and a complete existing module are valid.
-- Never infer captured bills from a current expense or bill status.
CREATE TEMP TABLE venture_capture_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 1)));
INSERT INTO venture_capture_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('capture_items');
DROP TABLE venture_capture_upgrade_guard;
