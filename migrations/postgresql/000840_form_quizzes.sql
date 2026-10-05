-- requires-table: form_result_bands
-- Scoring declarations and immutable results follow the field tables.
SELECT form_id, key, minimum, maximum, message, redirect_url FROM form_result_bands WHERE 1 = 0;
SELECT quiz_enabled, show_answer_key, score_to_lead FROM forms WHERE 1 = 0;
SELECT scoring FROM form_fields WHERE 1 = 0;
SELECT scored, score, result_key, result_message, result_url FROM form_submissions WHERE 1 = 0;
