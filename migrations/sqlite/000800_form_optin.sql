-- requires-table: form_pendings
-- Unconfirmed signups are metadata-owned working copies.
SELECT email, token_hash, expires_at, confirming FROM form_pendings WHERE 1 = 0;
SELECT double_opt_in, optin_email_field, optin_list_id, public_origin FROM forms WHERE 1 = 0;
