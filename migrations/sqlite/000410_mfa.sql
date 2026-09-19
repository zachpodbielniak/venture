-- The second-factor tables come from the mfa module field tables: the
-- per-user secret row, the recovery codes and the organization policy.
-- Verify the three are present together, or absent when the module is
-- disabled, and that a present secret row carries the encrypted secret and
-- the replay counter; a half-upgraded install must not start.
CREATE TEMP TABLE venture_mfa_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 3)));
INSERT INTO venture_mfa_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('user_mfas', 'mfa_recovery_codes', 'mfa_policies');
DROP TABLE venture_mfa_upgrade_guard;
CREATE TEMP TABLE venture_mfa_column_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_mfa_column_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'user_mfas')
 OR (SELECT COUNT(*) FROM pragma_table_info('user_mfas')
     WHERE name IN ('secret_ref', 'enabled', 'enrolled_at', 'last_used_counter')) = 4 THEN 1 ELSE 0 END;
INSERT INTO venture_mfa_column_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'mfa_recovery_codes')
 OR (SELECT COUNT(*) FROM pragma_table_info('mfa_recovery_codes')
     WHERE name IN ('code_hash', 'used_at')) = 2 THEN 1 ELSE 0 END;
DROP TABLE venture_mfa_column_guard;
