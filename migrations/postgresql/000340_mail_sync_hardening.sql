-- Inbound mail hardening is additive. The field tables add UIDVALIDITY, the
-- skip reason and duplicate link on mail_inbounds, backoff, lease, ignore
-- rules and the backfill date on mail_accounts, and dismissal on unmatched
-- senders. Old cursors ({"INBOX": 7}) and account:folder:uid keys stay
-- readable, so no row is rewritten or invented. Verify, when each table
-- exists, that its columns arrived together and that the UID key and the
-- sender address are still unique per organization: rerunning a sweep
-- safely rests on those two indexes. Absent tables mean the module is off.
CREATE TEMP TABLE venture_mail_sync_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_mail_sync_guard
SELECT CASE WHEN to_regclass('mail_inbounds') IS NOT NULL
 AND (to_regclass('uq_mail_inbounds_organization_uid_key') IS NULL
  OR to_regclass('idx_mail_inbounds_from_address') IS NULL
  OR (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema()
   AND table_name = 'mail_inbounds' AND column_name IN ('uid_validity', 'skip_reason', 'duplicate_of_id')) <> 3)
 THEN 0 ELSE 1 END;
INSERT INTO venture_mail_sync_guard
SELECT CASE WHEN to_regclass('mail_accounts') IS NOT NULL
 AND (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema()
  AND table_name = 'mail_accounts' AND column_name IN ('consecutive_failures', 'next_attempt_at',
  'sync_lease_until', 'ignore_patterns', 'internal_domains', 'sync_since')) <> 6
 THEN 0 ELSE 1 END;
INSERT INTO venture_mail_sync_guard
SELECT CASE WHEN to_regclass('mail_unmatched_senders') IS NOT NULL
 AND (to_regclass('uq_mail_unmatched_senders_organization_address') IS NULL
  OR NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema()
   AND table_name = 'mail_unmatched_senders' AND column_name = 'dismissed'))
 THEN 0 ELSE 1 END;
DROP TABLE venture_mail_sync_guard;
