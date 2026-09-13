-- Validate additive token scope storage; legacy non-admin tokens require rotation.
SELECT membership_snapshot FROM api_tokens WHERE 1 = 0;
