-- A plan price can carry a tax rate: plan_prices.tax_code_id, from the field
-- table. Existing prices keep none, so their invoices go on being taxed by
-- the customer's address exactly as before; no rate is invented for them.
CREATE TEMP TABLE venture_price_tax_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_price_tax_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'plan_prices')
 OR (SELECT COUNT(*) FROM pragma_table_info('plan_prices') WHERE name = 'tax_code_id') = 1
 THEN 1 ELSE 0 END;
DROP TABLE venture_price_tax_upgrade_guard;
