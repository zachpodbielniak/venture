-- requires-table: tickets
-- Additive: the event a ticket prepares. Existing tickets prepared no
-- event and stay empty; nothing is inferred from their titles or dates.
SELECT event_id FROM tickets WHERE 1 = 0;
