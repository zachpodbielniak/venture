-- requires-table: form_drafts
-- Resume capabilities and private delivery metadata are derived from the field table.
SELECT resume_hash, resume_email, resume_saved_at, resume_sent_at, resume_send_count FROM form_drafts WHERE 1 = 0;
SELECT allow_resume, resume_days FROM forms WHERE 1 = 0;
