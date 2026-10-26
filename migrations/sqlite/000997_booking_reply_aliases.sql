-- requires-table: mail_accounts
-- Entity metadata adds the fields; verify both sides of the scoped mailbox.
SELECT reply_alias_prefix FROM mail_accounts WHERE 1 = 0;
SELECT recipient_address FROM mail_inbounds WHERE 1 = 0;
