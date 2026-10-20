-- requires-table: goals
-- Existing goals keep manual numeric progress and no linked savings source.
UPDATE goals SET savings_account_id = 0 WHERE savings_account_id IS NULL;
UPDATE goals SET savings_envelope_id = 0 WHERE savings_envelope_id IS NULL;
