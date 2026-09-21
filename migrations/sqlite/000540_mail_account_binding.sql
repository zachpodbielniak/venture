-- Metadata creates the delivery evidence columns before this checkpoint.
-- Old queued messages remain unbound until their first explicit organization
-- delivery. Historical sent messages are never assigned an inferred account.
CREATE TEMP TABLE venture_mail_binding_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_mail_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'mail_messages') OR EXISTS (SELECT 1 FROM pragma_table_info('mail_messages') WHERE name = 'connection_id') THEN 1 ELSE 0 END;
INSERT INTO venture_mail_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'mail_messages') OR EXISTS (SELECT 1 FROM pragma_table_info('mail_messages') WHERE name = 'connection_version') THEN 1 ELSE 0 END;
DROP TABLE venture_mail_binding_guard;
