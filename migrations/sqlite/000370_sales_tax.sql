-- Sales-tax jurisdiction and rule tables come from the sales_tax module
-- field tables, and the company, product, invoice-line and credit columns
-- from additive metadata. Verify the pair is present together, or absent
-- when the module is disabled.
CREATE TEMP TABLE venture_sales_tax_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_sales_tax_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('tax_jurisdictions', 'tax_rules');
DROP TABLE venture_sales_tax_upgrade_guard;
