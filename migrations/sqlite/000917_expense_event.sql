-- requires-table: expenses
-- Additive: the planner event an expense paid for. Existing expenses
-- paid for no event; none is guessed from a description or a date.
SELECT event_id FROM expenses WHERE 1 = 0;
