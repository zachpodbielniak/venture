-- requires-table: telephony_accounts
-- Additive: phone lines, per-person extensions and text messages
-- (docs/telephony.org). All three start empty: no line is connected, no
-- extension is guessed and no text is invented. The organization's phone
-- country is empty (US), its text quiet hours are empty (21:00-08:00 in
-- the recipient's own time) and recording links arrive off.
SELECT name, provider, base_url, phone_number, connection_id, webhook_token_hash, webhook_sources, media_hosts, active, last_error FROM telephony_accounts WHERE 1 = 0;
SELECT user_id, extension, user_key FROM telephony_extensions WHERE 1 = 0;
SELECT account_id, contact_id, direction, from_number, to_number, body, media, media_documents, state, hold_until, provider_message_id, message_key, origin, keyword, auto_reply, error, sent_at, delivered_at, received_at, inbox_ready FROM text_messages WHERE 1 = 0;
SELECT telephony_country, telephony_quiet_hours, telephony_recording_links FROM organizations WHERE 1 = 0;
