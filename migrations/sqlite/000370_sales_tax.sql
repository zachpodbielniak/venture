-- Sales-tax jurisdiction and rule tables come from the sales_tax module
-- field tables, and the company, product, invoice-line and credit columns
-- from additive metadata. Verify the pair is present together, or absent
-- when the module is disabled.
CREATE TEMP TABLE venture_sales_tax_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_sales_tax_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('tax_jurisdictions', 'tax_rules');
DROP TABLE venture_sales_tax_upgrade_guard;
-- The address, exemption and frozen-rate columns live on core record types,
-- so they are present whatever the sales_tax switch says. Each table is
-- checked on its own because invoicing and receivables can be disabled
-- separately: an absent table is accepted, a present one must carry them.
-- Nothing is backfilled. An existing line keeps jurisdiction 0 and rate 0,
-- which reads as "no jurisdiction was in force when this was issued" and
-- lands the line on the return's unassigned row rather than inventing a
-- jurisdiction for tax that was frozen before any rule existed.
CREATE TEMP TABLE venture_sales_tax_column_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_sales_tax_column_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'companies')
 OR (SELECT COUNT(*) FROM pragma_table_info('companies')
     WHERE name IN ('tax_exemption_number', 'address_state', 'address_county', 'address_city')) = 4
 THEN 1 ELSE 0 END;
INSERT INTO venture_sales_tax_column_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'products')
 OR EXISTS (SELECT 1 FROM pragma_table_info('products') WHERE name = 'tax_exempt') THEN 1 ELSE 0 END;
INSERT INTO venture_sales_tax_column_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'invoice_lines')
 OR (SELECT COUNT(*) FROM pragma_table_info('invoice_lines')
     WHERE name IN ('tax_jurisdiction_id', 'tax_rate_scaled', 'tax_exempt')) = 3
 THEN 1 ELSE 0 END;
INSERT INTO venture_sales_tax_column_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'customer_credits')
 OR EXISTS (SELECT 1 FROM pragma_table_info('customer_credits') WHERE name = 'tax_jurisdiction_id') THEN 1 ELSE 0 END;
DROP TABLE venture_sales_tax_column_guard;
