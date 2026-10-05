-- Additive trusted-service sign-up receipts. The unique idempotency key is
-- what answers a retried sign-up from its receipt instead of creating a
-- second user or business.
SELECT idempotency_key, request_hash, user_id, business_id, linked, issuer, subject, actor_user_id FROM tenant_signups WHERE 1 = 0;
