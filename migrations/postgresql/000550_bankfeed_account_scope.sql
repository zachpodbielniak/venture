-- requires-table: bank_connections
-- Keep the existing unique constraint and explicitly partition its derived key.
-- An inconsistent historical identity fails atomically; no account is discarded.
UPDATE bank_connections SET connection_key = CAST(organization_id AS TEXT) || ':' || provider || ':' || provider_account_id;
