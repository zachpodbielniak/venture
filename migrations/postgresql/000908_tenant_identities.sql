-- Additive provider identity links: the trusted identity provider's subject
-- that acts as one local user. Rows are written only by the trusted sign-up
-- and by the person's own signed-in link; nothing is inferred from email.
SELECT user_id, issuer, subject, identity_key, active FROM tenant_identities WHERE 1 = 0;
