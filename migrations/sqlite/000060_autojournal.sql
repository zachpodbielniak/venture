-- Profiles and refunded_at are additive metadata-derived schema. Existing
-- refunds retain NULL dates: no historical cash event is invented on upgrade.
-- A historical disabled sales table is not reconciled. Only an installed
-- posting profile implies that the new sale column must already be present.
CREATE TEMP TABLE autojournal_schema_check (valid INTEGER CHECK (valid = 1));
INSERT INTO autojournal_schema_check
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE name = 'posting_profiles')
 OR NOT EXISTS (SELECT 1 FROM sqlite_master WHERE name = 'sales')
 OR EXISTS (SELECT 1 FROM pragma_table_info('sales') WHERE name = 'refunded_at')
 THEN 1 ELSE 0 END;
INSERT INTO autojournal_schema_check
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE name = 'posting_profiles')
 OR (SELECT COUNT(*) FROM pragma_table_info('posting_profiles')
 WHERE name IN ('profile_key', 'refunds_account_id', 'expense_categories')) = 3
 THEN 1 ELSE 0 END;
DROP TABLE autojournal_schema_check;
