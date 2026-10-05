-- requires-table: forms
-- Reconciliation adds opens_at. Existing forms have no opening restriction.
SELECT opens_at, closes_at, response_limit, unique_email_field FROM forms WHERE 1 = 0;
