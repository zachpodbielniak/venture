-- Source rounding is a separate nullable money field from additive metadata.
-- Existing documents retain NULL (no adjustment); no history is recomputed.
CREATE TEMP TABLE venture_cutover_adjustment_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_cutover_adjustment_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'invoices')
 OR (SELECT COUNT(*) FROM pragma_table_info('invoices') WHERE name IN ('opening_adjustment_amount', 'opening_adjustment_currency', 'opening_adjustment_exponent')) = 3 THEN 1 ELSE 0 END;
INSERT INTO venture_cutover_adjustment_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'vendor_bills')
 OR (SELECT COUNT(*) FROM pragma_table_info('vendor_bills') WHERE name IN ('opening_adjustment_amount', 'opening_adjustment_currency', 'opening_adjustment_exponent')) = 3 THEN 1 ELSE 0 END;
DROP TABLE venture_cutover_adjustment_guard;
