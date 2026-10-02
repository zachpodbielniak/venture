-- requires-table: form_groups
-- Repeating groups and question references are reconciled from metadata.
SELECT form_id, key, min_rows, max_rows FROM form_groups WHERE 1 = 0;
SELECT group_id FROM form_fields WHERE 1 = 0;
