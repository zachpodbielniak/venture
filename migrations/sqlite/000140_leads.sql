-- Lead columns are reconciled from field metadata before this batch.
-- A disabled module may have no leads table. Existing CRM data is untouched.
CREATE TEMP TABLE venture_leads_schema_guard (valid INTEGER CHECK (valid = 1));
INSERT INTO venture_leads_schema_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'leads')
 OR ((SELECT COUNT(*) FROM pragma_table_info('leads') WHERE name IN
 ('company_name', 'email', 'owner', 'status', 'converted_company_id', 'converted_contact_id', 'converted_deal_id')) = 7
 AND EXISTS (SELECT 1 FROM pragma_table_info('interactions') WHERE name = 'lead_id'))
 THEN 1 ELSE 0 END;
DROP TABLE venture_leads_schema_guard;
