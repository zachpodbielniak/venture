-- requires-table: mail_messages
-- Intake copies must not survive erasure in immutable shared audit diffs.
UPDATE audit_entries SET target_label = 'Mail message #' || CAST(target_id AS TEXT),
 diff = CASE WHEN NOT json_valid(diff) OR diff IS NULL THEN NULL ELSE json_set(json_remove(diff, '$.to', '$.cc', '$.bcc', '$.reply_to', '$.subject', '$.text_body', '$.html_body', '$.attachments', '$.attributes', '$.last_error'), '$.private_content', json('{"changed":true,"redacted":true}')) END
WHERE target_type = 'mail_message' AND target_id IN
 (SELECT id FROM mail_messages WHERE related_type IN ('form','form_draft','form_pending','booking_reservation'));
