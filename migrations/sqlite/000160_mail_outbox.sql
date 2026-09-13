-- Mail is additive. Verify its per-organization deduplication index after
-- schema reconciliation, including installations with the module disabled.
CREATE TEMP TABLE venture_mail_invariant (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_mail_invariant
SELECT CASE WHEN EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'mail_messages')
 AND NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_mail_messages_organization_idempotency_key')
 THEN 0 ELSE 1 END;
DROP TABLE venture_mail_invariant;
