-- requires-table: tickets
-- A ticket may name the goal its work moves (goal_id); a milestone gains
-- the same column, which reconciliation adds wherever the milestones table
-- exists. Nothing is backfilled: which goal a past ticket served is not
-- knowable from the rows, and a guess would read as progress.
SELECT goal_id FROM tickets WHERE 1 = 0;
