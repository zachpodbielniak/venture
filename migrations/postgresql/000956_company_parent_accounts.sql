-- requires-table: companies
-- Reconciliation adds the optional parent account; existing companies stay top-level.
SELECT parent_id FROM companies WHERE 1 = 0;
