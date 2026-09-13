-- Field tables create the four records and organization-scoped tag constraint.
-- Both a disabled fresh module and a complete existing module are valid.
-- Never infer depreciation or releases from old expense statuses.
CREATE TEMP TABLE venture_assets_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 4)));
INSERT INTO venture_assets_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('fixed_assets', 'depreciation_entries', 'deferrals', 'deferral_entries');
DROP TABLE venture_assets_upgrade_guard;
