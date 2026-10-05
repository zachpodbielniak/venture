-- requires-table: lightsite_billing_receipts
-- Additive Lightsite billing receipts. The organization-scoped unique
-- idempotency key is what answers a retried instruction from its receipt
-- instead of starting a second subscription or issuing a second invoice.
SELECT idempotency_key, request_hash, business_id, plan_code, outcome, company_id, subscription_id, result, actor_user_id FROM lightsite_billing_receipts WHERE 1 = 0;
