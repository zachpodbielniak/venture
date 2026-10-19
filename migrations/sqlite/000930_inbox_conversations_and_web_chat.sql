-- requires-table: inbox_conversations
-- Additive: one inbox (docs/inbox.org). Conversations, web chats, chat
-- visitors and chat messages all start empty: no conversation is invented
-- for history already on a contact's timeline (one is made when the
-- contact next writes, or when someone opens their conversation), no web
-- chat is switched on and nothing is copied from mail, calls or texts.
SELECT contact_id, status, owner_id, status_changed_at, last_message_at, last_inbound_at, last_inbound_channel, channels, contact_key FROM inbox_conversations WHERE 1 = 0;
SELECT name, token, greeting, allowed_origins, active FROM inbox_chat_widgets WHERE 1 = 0;
SELECT widget_id, contact_id, visitor_name, visitor_email, visitor_phone, token_hash, last_message_at FROM inbox_chat_sessions WHERE 1 = 0;
SELECT session_id, contact_id, inbound, body, author, sent_at FROM inbox_chat_messages WHERE 1 = 0;
