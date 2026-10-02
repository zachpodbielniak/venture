-- requires-table: form_fields
-- Marketing permission is an explicit opt-in, never inferred from an old
-- privacy acknowledgement. Reconciliation supplies the new column.
UPDATE form_fields SET marketing_consent = 0 WHERE marketing_consent IS NULL;
