-- requires-table: stripe_prepayments
-- New collection journals have no historical payments to synthesize.
SELECT subscription_id, legs, published_at, next_attempt_at, lease_until
FROM stripe_prepayments WHERE 1 = 0;
