-- requires-table: accounts
-- Existing books remain shared; never infer a personal owner from a creator.
UPDATE accounts SET private_owner_id = 0 WHERE private_owner_id IS NULL;
