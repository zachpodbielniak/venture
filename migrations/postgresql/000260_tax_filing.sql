-- Tax filing packs and contractor TIN/1099 records come from field tables.
-- Complete family or none when the optional tax_filing module is off.
CREATE TEMP TABLE tax_filing_upgrade_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO tax_filing_upgrade_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 3) THEN 1 ELSE 0 END
FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('tax_filings', 'contractor_tax_forms', 'contractor_tax_packs');
DROP TABLE tax_filing_upgrade_check;
