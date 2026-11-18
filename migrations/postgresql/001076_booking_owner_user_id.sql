-- requires-table: booking_pages
-- A booking page can name its owner by account: owner_user_id. The save
-- writes that member's username into owner. Reconciliation adds the
-- column from the field table. Existing pages keep their owner; 0 means
-- the owner was named by username. No backfill.
SELECT owner_user_id FROM booking_pages WHERE 1 = 0;
