-- requires-table: lead_scoring_rules
-- A lead scoring rule may carry a Tallow formula (expression) that gives
-- its points; reconciliation adds the column from the field table. No
-- backfill: an empty expression keeps the rule's fixed points.
SELECT expression FROM lead_scoring_rules WHERE 1 = 0;
