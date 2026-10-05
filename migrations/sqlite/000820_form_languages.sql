-- requires-table: form_translations
-- Public wording is versioned; answers keep their original stable identities.
SELECT form_id, language, text_key, text FROM form_translations WHERE 1 = 0;
SELECT default_language FROM forms WHERE 1 = 0;
SELECT language FROM form_submissions WHERE 1 = 0;
