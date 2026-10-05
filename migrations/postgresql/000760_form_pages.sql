-- requires-table: form_drafts
-- Metadata reconciliation creates form_drafts and the draft lifetime column.
SELECT draft_minutes FROM forms WHERE 1 = 0;
SELECT form_id, version_id, expires_at, generation FROM form_drafts WHERE 1 = 0;
