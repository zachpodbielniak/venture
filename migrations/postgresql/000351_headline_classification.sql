-- Cost of revenue flags and campaign links come from the expense, tax
-- category and vendor bill line field tables. Each table has all of its new
-- columns or none (a disabled module leaves its table absent). Nothing is
-- backfilled: an unflagged historical row is not cost of revenue, and no
-- campaign is inferred for spend that never named one.
CREATE TEMP TABLE venture_headline_expense_guard (column_count INTEGER CHECK (column_count IN (0, 2)));
INSERT INTO venture_headline_expense_guard SELECT COUNT(*) FROM information_schema.columns
 WHERE table_schema = current_schema() AND table_name = 'expenses'
 AND column_name IN ('cost_of_revenue', 'campaign_id');
DROP TABLE venture_headline_expense_guard;
CREATE TEMP TABLE venture_headline_category_guard (column_count INTEGER CHECK (column_count IN (0, 1)));
INSERT INTO venture_headline_category_guard SELECT COUNT(*) FROM information_schema.columns
 WHERE table_schema = current_schema() AND table_name = 'tax_categories'
 AND column_name = 'cost_of_revenue';
DROP TABLE venture_headline_category_guard;
CREATE TEMP TABLE venture_headline_bill_line_guard (column_count INTEGER CHECK (column_count IN (0, 2)));
INSERT INTO venture_headline_bill_line_guard SELECT COUNT(*) FROM information_schema.columns
 WHERE table_schema = current_schema() AND table_name = 'vendor_bill_lines'
 AND column_name IN ('cost_of_revenue', 'campaign_id');
DROP TABLE venture_headline_bill_line_guard;
