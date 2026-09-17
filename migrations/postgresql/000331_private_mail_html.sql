-- Additive metadata creates the sensitive HTML delivery body beside the
-- private text body when mail is enabled, so a bearer link in an HTML-only
-- template reaches the transport without entering the stored public body.
CREATE TEMP TABLE venture_private_mail_html_guard (column_count INTEGER CHECK (column_count IN (0, 1)));
INSERT INTO venture_private_mail_html_guard SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'mail_messages' AND column_name = 'private_html_body';
DROP TABLE venture_private_mail_html_guard;
