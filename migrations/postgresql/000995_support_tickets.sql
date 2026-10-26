-- requires-table: mail_accounts
-- Ticket intake fields are added by entity metadata; verify the paired schema.
SELECT creates_tickets FROM mail_accounts WHERE 1 = 0;
SELECT ticket_id FROM mail_inbounds WHERE 1 = 0;
SELECT sender_address FROM mail_messages WHERE 1 = 0;
-- documents belongs to another module. Absent is fine; a present table
-- must already carry ticket_id from the field metadata.
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM information_schema.tables
      WHERE table_schema = current_schema() AND table_name = 'documents')
    AND NOT EXISTS (SELECT 1 FROM information_schema.columns
      WHERE table_schema = current_schema() AND table_name = 'documents' AND column_name = 'ticket_id') THEN
    RAISE EXCEPTION 'support tickets require documents.ticket_id';
  END IF;
END $$;
-- Ticket columns are entity metadata. The tickets table exists only while
-- that module is on, so this mail-account batch must not require it.
