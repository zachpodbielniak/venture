-- requires-table: audit_entries
-- Token labels belong to the owner-only token page, never its shared history.
-- Keep the fact of a rename, even when the token has since been deleted.
UPDATE audit_entries SET target_label = 'API token #' || CAST(target_id AS TEXT)
WHERE target_type = 'api_token';
UPDATE audit_entries
SET diff = jsonb_set(CAST(diff AS JSONB), '{name}',
  '{"changed":true,"redacted":true}'::jsonb)::text
WHERE target_type = 'api_token' AND NULLIF(diff, '') IS NOT NULL
  AND CAST(NULLIF(diff, '') AS JSONB) ? 'name';
