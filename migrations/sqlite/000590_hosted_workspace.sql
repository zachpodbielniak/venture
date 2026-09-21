-- Metadata creates the opt-in control tables. Existing local identities are
-- deliberately not promoted or linked to a tenant administrator on upgrade.
CREATE TEMP TABLE venture_tenant_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_tenant_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'tenant_workspaces')
 OR (SELECT COUNT(*) FROM pragma_table_info('tenant_workspaces')
 WHERE name IN ('binding_key', 'workspace_id', 'origin', 'state', 'administration_sequence')) = 5 THEN 1 ELSE 0 END;
DROP TABLE venture_tenant_upgrade_guard;
