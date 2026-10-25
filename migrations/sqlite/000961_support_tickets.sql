-- requires-table: mail_accounts
-- Ticket intake fields are added by entity metadata; verify the paired schema.
SELECT creates_tickets FROM mail_accounts WHERE 1 = 0;
SELECT ticket_id FROM mail_inbounds WHERE 1 = 0;
SELECT ticket_id FROM documents WHERE 1 = 0;
SELECT sender_address FROM mail_messages WHERE 1 = 0;
-- Ticket columns are entity metadata. The tickets table exists only while
-- that module is on, so this mail-account batch must not require it.
