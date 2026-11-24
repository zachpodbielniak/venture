-- A hosted business's leaving state (leave, close, return, purge) lives on
-- its organization: departure_state, left_at, returned_at and purged_at.
-- Reconciliation adds the columns from the field table; a new organization
-- is saved 'active'. Every organization from before this release is in
-- business, so it is set 'active'. The times stay null.
UPDATE organizations SET departure_state = 'active' WHERE departure_state IS NULL OR departure_state = '';
SELECT left_at, returned_at, purged_at FROM organizations WHERE 1 = 0;
