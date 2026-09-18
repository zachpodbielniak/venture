-- The second-factor tables come from the mfa module field tables: the
-- per-user secret row, the recovery codes and the organization policy.
-- Verify the three are present together, or absent when the module is
-- disabled, and that a present secret row carries the encrypted secret and
-- the replay counter; a half-upgraded install must not start.
CREATE TEMP TABLE venture_mfa_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 3)));
INSERT INTO venture_mfa_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('user_mfas', 'mfa_recovery_codes', 'mfa_policies');
DROP TABLE venture_mfa_upgrade_guard;
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM information_schema.tables
      WHERE table_schema = current_schema() AND table_name = 'user_mfas')
    AND (SELECT COUNT(*) FROM information_schema.columns
      WHERE table_schema = current_schema() AND table_name = 'user_mfas'
      AND column_name IN ('secret_ref', 'enabled', 'enrolled_at', 'last_used_counter')) <> 4 THEN
    RAISE EXCEPTION 'mfa requires user_mfas.secret_ref, enabled, enrolled_at and last_used_counter';
  END IF;
  IF EXISTS (SELECT 1 FROM information_schema.tables
      WHERE table_schema = current_schema() AND table_name = 'mfa_recovery_codes')
    AND (SELECT COUNT(*) FROM information_schema.columns
      WHERE table_schema = current_schema() AND table_name = 'mfa_recovery_codes'
      AND column_name IN ('code_hash', 'used_at')) <> 2 THEN
    RAISE EXCEPTION 'mfa requires mfa_recovery_codes.code_hash and used_at';
  END IF;
END $$;
