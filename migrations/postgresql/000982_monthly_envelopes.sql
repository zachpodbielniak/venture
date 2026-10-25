-- requires-table: budget_lines
-- Existing business plans remain one-period lines, without rollover.
UPDATE budget_lines SET envelope = FALSE WHERE envelope IS NULL;
UPDATE budget_lines SET rollover = FALSE WHERE rollover IS NULL;
