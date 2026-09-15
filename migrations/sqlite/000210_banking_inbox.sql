-- Categorization rules and transfers are additive field-table types.
-- Banking remains complete (all evidence tables) or absent when disabled.
CREATE TEMP TABLE banking_inbox_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO banking_inbox_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 7) THEN 1 ELSE 0 END
FROM sqlite_master WHERE type = 'table' AND name IN
('bank_accounts', 'bank_statements', 'bank_transactions', 'bank_matches', 'reconciliations', 'bank_rules', 'bank_transfers');
DROP TABLE banking_inbox_check;
