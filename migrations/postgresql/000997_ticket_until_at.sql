-- requires-table: tickets
-- A ticket may say when its work ends (until_at) beside when it is due;
-- reconciliation adds the column from the field table. Nothing is
-- backfilled: an end time written into a description is a sentence, and
-- reading a time out of it would be a guess.
SELECT until_at FROM tickets WHERE 1 = 0;
