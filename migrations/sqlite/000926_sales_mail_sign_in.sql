-- requires-table: mail_oauth_providers
-- Additive: token mailbox sign-in and replies from the timeline
-- (docs/mail.org). Providers start empty -- none ships -- and every
-- existing account keeps its password binding (no sign-in provider). An
-- existing outbox message has no threading headers and is delivered
-- exactly as before.
SELECT name, authorize_url, token_url, scopes, active FROM mail_oauth_providers WHERE 1 = 0;
SELECT oauth_provider_id FROM mail_accounts WHERE 1 = 0;
SELECT in_reply_to, reply_references FROM mail_messages WHERE 1 = 0;
