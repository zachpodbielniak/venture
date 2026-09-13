-- Additive banking fields come from the module's field tables. Upgrades must
-- expose the complete evidence family, or none when the module is disabled.
-- No statement or financial history is inferred from operational records.
CREATE TEMP TABLE banking_upgrade_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO banking_upgrade_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 5) THEN 1 ELSE 0 END
FROM sqlite_master WHERE type = 'table' AND name IN
('bank_accounts', 'bank_statements', 'bank_transactions', 'bank_matches', 'reconciliations');
DROP TABLE banking_upgrade_check;
