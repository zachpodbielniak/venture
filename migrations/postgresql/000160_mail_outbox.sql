-- Mail is additive. Verify its per-organization deduplication index after
-- schema reconciliation, including installations with the module disabled.
CREATE TEMP TABLE venture_mail_invariant (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_mail_invariant
SELECT CASE WHEN to_regclass('mail_messages') IS NOT NULL
 AND to_regclass('uq_mail_messages_organization_idempotency_key') IS NULL
 THEN 0 ELSE 1 END;
DROP TABLE venture_mail_invariant;
