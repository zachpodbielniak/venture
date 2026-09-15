-- Additive metadata creates the sensitive delivery body when mail is enabled.
CREATE TEMP TABLE venture_private_mail_guard (column_count INTEGER CHECK (column_count IN (0, 1)));
INSERT INTO venture_private_mail_guard SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'mail_messages' AND column_name = 'private_text_body';
DROP TABLE venture_private_mail_guard;
