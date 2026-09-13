-- Field metadata owns additive schema; check the installed lead contract.
-- An absent leads table is valid when the module is disabled.
DO $$
BEGIN
 IF to_regclass('leads') IS NOT NULL AND
 ((SELECT COUNT(*) FROM information_schema.columns
   WHERE table_schema = current_schema() AND table_name = 'leads'
   AND column_name IN ('company_name', 'email', 'owner', 'status',
   'converted_company_id', 'converted_contact_id', 'converted_deal_id')) <> 7
 OR NOT EXISTS (SELECT 1 FROM information_schema.columns
   WHERE table_schema = current_schema() AND table_name = 'interactions' AND column_name = 'lead_id'))
 THEN RAISE EXCEPTION 'Lead schema reconciliation is incomplete';
 END IF;
END $$;
