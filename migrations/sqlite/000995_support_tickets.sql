-- requires-table: mail_accounts
-- Ticket intake fields are added by entity metadata; verify the paired schema.
SELECT creates_tickets FROM mail_accounts WHERE 1 = 0;
SELECT ticket_id FROM mail_inbounds WHERE 1 = 0;
SELECT sender_address FROM mail_messages WHERE 1 = 0;
-- documents belongs to another module. Absent is fine; a present table
-- must already carry ticket_id from the field metadata.
CREATE TEMP TABLE venture_support_documents_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_support_documents_guard (valid)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'documents')
 OR EXISTS (SELECT 1 FROM pragma_table_info('documents') WHERE name = 'ticket_id') THEN 1 ELSE 0 END;
DROP TABLE venture_support_documents_guard;
-- Ticket columns are entity metadata. The tickets table exists only while
-- that module is on, so this mail-account batch must not require it.
