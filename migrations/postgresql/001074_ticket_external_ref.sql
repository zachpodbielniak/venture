-- requires-table: tickets
-- A ticket may carry the id it has in another system (external_ref), unique
-- within its organization when set; reconciliation adds the column and the
-- scoped unique index from the field table. No backfill: every existing
-- ticket is an ordinary one, with no reference.
SELECT external_ref FROM tickets WHERE 1 = 0;
