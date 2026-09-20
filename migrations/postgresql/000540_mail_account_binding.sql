-- Metadata creates delivery evidence. Never infer historical account identity.
CREATE TEMP TABLE venture_mail_binding_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_mail_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'mail_messages') OR EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'mail_messages' AND column_name = 'connection_id') THEN 1 ELSE 0 END;
INSERT INTO venture_mail_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'mail_messages') OR EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'mail_messages' AND column_name = 'connection_version') THEN 1 ELSE 0 END;
DROP TABLE venture_mail_binding_guard;
