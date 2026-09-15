-- Supplier portal and operator-defined accounting fields come from field tables.
-- Each family is all-or-nothing; a disabled module leaves its tables absent.
CREATE TEMP TABLE venture_supplier_portal_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 1)));
INSERT INTO venture_supplier_portal_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name = 'supplier_portal_accesses';
DROP TABLE venture_supplier_portal_upgrade_guard;
CREATE TEMP TABLE venture_custom_fields_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 3)));
INSERT INTO venture_custom_fields_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('accounting_custom_fields', 'accounting_layouts', 'custom_field_values');
DROP TABLE venture_custom_fields_upgrade_guard;
