-- Disabled installs retain local authentication unchanged. Metadata owns the
-- optional identity table; migration never infers identity links from email.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'oidc_identities')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'oidc_identities'
     AND column_name IN ('user_id', 'connection_id', 'issuer', 'subject', 'identity_key', 'active')) <> 6 THEN
   RAISE EXCEPTION 'OIDC identity metadata is incomplete';
 END IF;
END $$;
