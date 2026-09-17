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
SELECT CASE WHEN EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'mail_inbounds')
 AND (NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_mail_inbounds_organization_uid_key')
  OR NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'idx_mail_inbounds_from_address')
  OR (SELECT COUNT(*) FROM pragma_table_info('mail_inbounds') WHERE name IN ('uid_validity', 'skip_reason', 'duplicate_of_id')) <> 3)
 THEN 0 ELSE 1 END;
INSERT INTO venture_mail_sync_guard
SELECT CASE WHEN EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'mail_accounts')
 AND (SELECT COUNT(*) FROM pragma_table_info('mail_accounts') WHERE name IN ('consecutive_failures', 'next_attempt_at',
  'sync_lease_until', 'ignore_patterns', 'internal_domains', 'sync_since')) <> 6
 THEN 0 ELSE 1 END;
INSERT INTO venture_mail_sync_guard
SELECT CASE WHEN EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'mail_unmatched_senders')
 AND (NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_mail_unmatched_senders_organization_address')
  OR NOT EXISTS (SELECT 1 FROM pragma_table_info('mail_unmatched_senders') WHERE name = 'dismissed'))
 THEN 0 ELSE 1 END;
DROP TABLE venture_mail_sync_guard;
