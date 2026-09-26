-- Metadata adds who wrote in and what they wrote to each form submission.
-- Earlier submissions keep them empty: the message went to the lead's notes,
-- which may have been edited since, and a merged capture never kept it, so
-- there is nothing trustworthy to copy back.
CREATE TEMP TABLE venture_submission_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_submission_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'attribution_submissions')
 OR (SELECT COUNT(*) FROM pragma_table_info('attribution_submissions')
 WHERE name IN ('sender_name', 'sender_email', 'message')) = 3 THEN 1 ELSE 0 END;
DROP TABLE venture_submission_upgrade_guard;
