-- Employee claims and payroll records come from field tables. Complete family or none.
CREATE TEMP TABLE venture_claims_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_claims_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('expense_claims', 'expense_claim_lines');
DROP TABLE venture_claims_upgrade_guard;
CREATE TEMP TABLE venture_payroll_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_payroll_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('payroll_runs', 'payroll_lines');
DROP TABLE venture_payroll_upgrade_guard;
