-- Explicit organization/purpose selections are metadata-owned. Existing host
-- credentials are never assigned to a tenant by migration; operators choose
-- each organization and purpose through its write-only settings workflow.
CREATE TEMP TABLE venture_ai_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_ai_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'ai_configurations')
 OR (SELECT COUNT(*) FROM pragma_table_info('ai_configurations')
 WHERE name IN ('configuration_key', 'purpose', 'mode', 'connection_id', 'grant_id')) = 5 THEN 1 ELSE 0 END;
DROP TABLE venture_ai_upgrade_guard;
