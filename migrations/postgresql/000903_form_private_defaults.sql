-- requires-table: form_versions
-- Older unreleased form snapshots may have published private defaults.
UPDATE form_fields SET default_value = NULL WHERE sensitive = TRUE;
UPDATE form_versions SET definition = jsonb_set(definition::jsonb, '{fields}', COALESCE((SELECT jsonb_agg(CASE WHEN item->>'sensitive' = 'true' THEN item - 'default_value' ELSE item END ORDER BY ordinal) FROM jsonb_array_elements(definition::jsonb->'fields') WITH ORDINALITY AS q(item, ordinal)), '[]'::jsonb))::text WHERE definition IS NOT NULL AND jsonb_typeof(definition::jsonb->'fields') = 'array';
UPDATE audit_entries SET diff = ((diff::jsonb - 'default_value') || '{"private_default":{"changed":true,"redacted":true}}'::jsonb)::text
WHERE target_type = 'form_field' AND target_id IN (SELECT id FROM form_fields WHERE sensitive = TRUE) AND diff IS NOT NULL;
