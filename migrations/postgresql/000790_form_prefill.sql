-- requires-table: forms
-- Metadata adds opt-in prefill and private personal-link bindings.
SELECT one_per_contact, one_per_link FROM forms WHERE 1 = 0;
SELECT allow_prefill, contact_field FROM form_fields WHERE 1 = 0;
SELECT contact_id, personal_hash FROM form_submissions WHERE 1 = 0;
SELECT contact_id FROM form_drafts WHERE 1 = 0;
