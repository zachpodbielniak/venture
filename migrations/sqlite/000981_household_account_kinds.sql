-- requires-table: bank_accounts
-- Preserve existing identities without guessing their household purpose.
UPDATE bank_accounts SET kind = 0 WHERE kind IS NULL;
