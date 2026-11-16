-- requires-table: ticket_comments
-- A ticket comment may carry the id it has in another system (external_ref),
-- unique within its organization when set; reconciliation adds the column and
-- the scoped unique index from the field table. No backfill.
SELECT external_ref FROM ticket_comments WHERE 1 = 0;
