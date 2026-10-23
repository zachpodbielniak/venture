-- requires-table: quotes
-- Reconciliation adds the optional address; existing quotes remain unspecified.
SELECT service_address FROM quotes WHERE 1 = 0;
