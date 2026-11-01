-- An owner may invite people to their own business. Reconciliation adds
-- the owner-invitation columns to tenant_invitations from the field table:
-- issuer_organization_id, issuer_owner_version, email, invitee_name,
-- revoked_at, idempotency_key, request_hash, send_key, mail_message_id and
-- sent_count. Nothing is backfilled: every existing invitation was made by
-- a workspace administrator, which issuer_organization_id 0 already says,
-- and none was ever revoked or mailed by this path.
SELECT issuer_organization_id, issuer_owner_version, email, invitee_name, revoked_at,
	idempotency_key, request_hash, send_key, mail_message_id, sent_count
FROM tenant_invitations WHERE 1 = 0;
