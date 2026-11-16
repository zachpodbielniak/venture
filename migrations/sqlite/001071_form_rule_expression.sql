-- requires-table: form_rules
-- A form rule may carry a Tallow formula (expression) used instead of its
-- conditions; reconciliation adds the column from the field table. No
-- backfill: every existing rule keeps its conditions, which an empty
-- expression leaves in charge.
SELECT expression FROM form_rules WHERE 1 = 0;
