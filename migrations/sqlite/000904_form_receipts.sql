-- requires-table: form_receipts
-- Additive private acceptance evidence, including consumed identities after erasure.
SELECT form_id, nonce_hash, answers_hash, request_hash, response_id FROM form_receipts WHERE 1 = 0;
