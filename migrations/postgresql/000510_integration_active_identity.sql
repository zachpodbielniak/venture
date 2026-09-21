-- Disabled modules acquire the same metadata index when enabled later.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'integration_connections')
   AND NOT EXISTS (SELECT 1 FROM pg_indexes WHERE schemaname = current_schema()
     AND indexname = 'uq_integration_connections_organization_provider_when_enabled') THEN
   RAISE EXCEPTION 'active integration account uniqueness is missing';
 END IF;
END $$;
