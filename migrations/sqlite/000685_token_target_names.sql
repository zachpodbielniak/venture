-- requires-table: audit_entries
-- Token labels belong to the owner-only token page, never its shared history.
-- Keep the fact of a rename, even when the token has since been deleted.
UPDATE audit_entries SET target_label = 'API token #' || CAST(target_id AS TEXT)
WHERE target_type = 'api_token';
UPDATE audit_entries
SET diff = json_set(diff, '$.name', json('{"changed":true,"redacted":true}'))
WHERE target_type = 'api_token' AND json_valid(diff)
  AND json_type(diff, '$.name') IS NOT NULL;
