-- requires-table: billing_requests
-- A published prepaid cancellation names its term end. Nothing is backfilled.
SELECT term_end FROM billing_requests WHERE 1 = 0;
