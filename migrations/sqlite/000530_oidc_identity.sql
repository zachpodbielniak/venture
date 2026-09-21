-- Identity metadata is optional. Existing users are never linked by email or
-- provisioned during migration; every link needs a verified login and consent.
CREATE TEMP TABLE venture_oidc_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_oidc_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'oidc_identities')
 OR (SELECT COUNT(*) FROM pragma_table_info('oidc_identities')
 WHERE name IN ('user_id', 'connection_id', 'issuer', 'subject', 'identity_key', 'active')) = 6 THEN 1 ELSE 0 END;
DROP TABLE venture_oidc_upgrade_guard;
