-- Additive books records: budgets, equity, group and tax depreciation.
-- Field tables create the columns. Empty or complete families are valid.
-- Never invent historical budget, equity or tax-book journals.
CREATE TEMP TABLE venture_books_upgrade_guard (table_count INTEGER CHECK (table_count BETWEEN 0 AND 8));
INSERT INTO venture_books_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN (
	'budgets', 'budget_lines', 'equity_transactions',
	'intercompany_links', 'eliminations', 'tax_depreciation_entries');
DROP TABLE venture_books_upgrade_guard;
