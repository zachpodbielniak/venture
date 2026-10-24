-- requires-table: lightsite_billing_receipts
-- Hosted deferred enrollments made under the fourteen-day trial have no
-- subscription identity, so the renewal sweep would invoice them at trial
-- end before their site went live. Give exactly those the reserved
-- hosted:<business> identity the sweep skips until publication. Prepaid
-- enrollments, subscriptions with an identity of their own and receipts
-- are left as they are (docs/hosted-billing.org).
UPDATE customer_subscriptions
SET external_id = (
	SELECT 'hosted:' || r.business_id FROM lightsite_billing_receipts r
	WHERE r.organization_id = customer_subscriptions.organization_id
	AND r.outcome = 'enroll' AND r.idempotency_key = 'hosted:' || r.business_id || ':enroll'
	AND json_valid(r.result) AND json_extract(r.result, '$.subscription_id') = customer_subscriptions.id
	AND json_type(r.result, '$.payment_path') IS NULL
	LIMIT 1)
WHERE (external_id IS NULL OR external_id = '')
AND EXISTS (
	SELECT 1 FROM lightsite_billing_receipts r
	WHERE r.organization_id = customer_subscriptions.organization_id
	AND r.outcome = 'enroll' AND r.idempotency_key = 'hosted:' || r.business_id || ':enroll'
	AND json_valid(r.result) AND json_extract(r.result, '$.subscription_id') = customer_subscriptions.id
	AND json_type(r.result, '$.payment_path') IS NULL);
