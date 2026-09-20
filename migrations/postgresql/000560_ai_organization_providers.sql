-- No ambient credentials or tenant eligibility are inferred on upgrade.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'ai_configurations')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'ai_configurations'
     AND column_name IN ('configuration_key', 'purpose', 'mode', 'connection_id', 'grant_id')) <> 5 THEN
   RAISE EXCEPTION 'Organization AI configuration metadata is incomplete';
 END IF;
END $$;
