-- requires-table: lightsite_billing_receipts
-- Hosted deferred enrollments made under the fourteen-day trial have no
-- subscription identity, so the renewal sweep would invoice them at trial
-- end before their site went live. Give exactly those the reserved
-- hosted:<business> identity the sweep skips until publication. Prepaid
-- enrollments, subscriptions with an identity of their own and receipts
-- are left as they are (docs/hosted-billing.org).
UPDATE customer_subscriptions s
SET external_id = 'hosted:' || r.business_id
FROM lightsite_billing_receipts r
WHERE (s.external_id IS NULL OR s.external_id = '')
AND r.organization_id = s.organization_id
AND r.outcome = 'enroll' AND r.idempotency_key = 'hosted:' || r.business_id || ':enroll'
AND (r.result::jsonb ->> 'subscription_id')::bigint = s.id
AND (r.result::jsonb -> 'payment_path') IS NULL;
