-- requires-table: alert_rules
-- An alert rule may carry a Tallow formula (expression) every candidate
-- hit must pass; reconciliation adds the column from the field table. No
-- backfill: an empty expression passes every candidate, as before.
SELECT expression FROM alert_rules WHERE 1 = 0;
