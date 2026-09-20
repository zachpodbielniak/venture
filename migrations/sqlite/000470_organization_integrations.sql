-- Property metadata creates the table, even on upgrades. Installation-wide
-- credentials are intentionally not assigned to an arbitrary organization.
-- Refuse incomplete metadata when the module is enabled; a disabled module
-- has no table and may be enabled later through ordinary schema reconciliation.
CREATE TEMP TABLE venture_integration_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_integration_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'integration_connections')
 OR (SELECT COUNT(*) FROM pragma_table_info('integration_connections')
 WHERE name IN ('provider', 'account_id', 'environment', 'enabled', 'credential_revision', 'sealed_settings')) = 6 THEN 1 ELSE 0 END;
DROP TABLE venture_integration_upgrade_guard;
