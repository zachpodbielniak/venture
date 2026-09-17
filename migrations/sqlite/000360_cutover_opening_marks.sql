-- Migrated invoices, bills and credits carry opening_at from additive field
-- metadata. Each table is verified independently because receivables and
-- payables can be disabled on their own: an absent table is accepted, a
-- present one must have the column. Existing rows keep NULL, meaning "issued
-- in VENTURE"; no historical document is reclassified as an opening balance.
CREATE TEMP TABLE venture_cutover_opening_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_cutover_opening_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'invoices')
 OR EXISTS (SELECT 1 FROM pragma_table_info('invoices') WHERE name = 'opening_at') THEN 1 ELSE 0 END;
INSERT INTO venture_cutover_opening_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'vendor_bills')
 OR EXISTS (SELECT 1 FROM pragma_table_info('vendor_bills') WHERE name = 'opening_at') THEN 1 ELSE 0 END;
INSERT INTO venture_cutover_opening_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'customer_credits')
 OR EXISTS (SELECT 1 FROM pragma_table_info('customer_credits') WHERE name = 'opening_at') THEN 1 ELSE 0 END;
INSERT INTO venture_cutover_opening_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'vendor_credits')
 OR EXISTS (SELECT 1 FROM pragma_table_info('vendor_credits') WHERE name = 'opening_at') THEN 1 ELSE 0 END;
DROP TABLE venture_cutover_opening_guard;
