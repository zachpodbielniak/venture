-- Property metadata creates the table. Never guess which organization owns
-- legacy installation credentials. A disabled module may be enabled later.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'integration_connections')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'integration_connections'
     AND column_name IN ('provider', 'account_id', 'environment', 'enabled', 'credential_revision', 'sealed_settings')) <> 6 THEN
   RAISE EXCEPTION 'integration connection metadata is incomplete';
 END IF;
END $$;
