-- requires-table: form_versions
-- Older unreleased form snapshots may have published private defaults.
UPDATE form_fields SET default_value = NULL WHERE sensitive = 1;
UPDATE form_versions SET definition = json_set(definition, '$.fields', json((SELECT json_group_array(json(CASE WHEN json_extract(value, '$.sensitive') = 1 THEN json_remove(value, '$.default_value') ELSE value END)) FROM json_each(definition, '$.fields')))) WHERE json_valid(definition) AND json_type(definition, '$.fields') = 'array';
UPDATE audit_entries SET diff = json_set(json_remove(diff, '$.default_value'), '$.private_default', json('{"changed":true,"redacted":true}'))
WHERE target_type = 'form_field' AND target_id IN (SELECT id FROM form_fields WHERE sensitive = 1) AND json_valid(diff);
