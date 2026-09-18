-- CRM migration tables come from the crm_import module field tables:
-- one batch per manifest and one row per source id. Verify the pair is
-- present together, or absent when the module is disabled.
CREATE TEMP TABLE venture_crm_import_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_crm_import_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('crm_imports', 'crm_import_rows');
DROP TABLE venture_crm_import_upgrade_guard;
