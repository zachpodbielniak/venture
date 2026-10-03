-- requires-table: form_uploads
-- Private upload bytes are linked only after validated submission.
SELECT form_id, version_id, response_id, document_id, token_hash, expires_at FROM form_uploads WHERE 1 = 0;
SELECT file_max_bytes, file_max_count, file_types FROM form_fields WHERE 1 = 0;
SELECT upload_quota_bytes, upload_client_hourly_bytes FROM forms WHERE 1 = 0;
