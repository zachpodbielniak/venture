-- requires-table: form_rules
-- Rules and private branch accounting are reconciled from property metadata.
SELECT form_id, conditions, target_key FROM form_rules WHERE 1 = 0;
SELECT not_shown FROM form_submissions WHERE 1 = 0;
