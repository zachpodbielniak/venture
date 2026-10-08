-- requires-table: billing_requests
-- Existing requests keep catalogue trial semantics. No financial history is fabricated.
SELECT defer_days FROM billing_requests WHERE 1 = 0;
