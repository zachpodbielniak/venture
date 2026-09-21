-- Preserve existing identity ownership. Hosted adoption is an explicit local
-- operator action after configuring the immutable workspace UUID and origin.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'tenant_workspaces')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'tenant_workspaces'
     AND column_name IN ('binding_key', 'workspace_id', 'origin', 'state', 'administration_sequence')) <> 5 THEN
   RAISE EXCEPTION 'Hosted workspace metadata is incomplete';
 END IF;
END $$;
