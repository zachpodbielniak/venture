-- Invoice lines may name their income and discount accounts, and an expense
-- its expense account, from additive field metadata. Each table is checked
-- on its own because receivables and money can be disabled separately: an
-- absent table is accepted, a present one must carry the columns. Existing
-- rows keep NULL, which posts exactly as before.
CREATE TEMP TABLE venture_line_accounts_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_line_accounts_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'invoice_lines')
 OR (EXISTS (SELECT 1 FROM pragma_table_info('invoice_lines') WHERE name = 'income_account_id')
 AND EXISTS (SELECT 1 FROM pragma_table_info('invoice_lines') WHERE name = 'discount_account_id')) THEN 1 ELSE 0 END;
INSERT INTO venture_line_accounts_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'expenses')
 OR EXISTS (SELECT 1 FROM pragma_table_info('expenses') WHERE name = 'expense_account_id') THEN 1 ELSE 0 END;
DROP TABLE venture_line_accounts_guard;
