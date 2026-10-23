-- requires-table: ai_messages
-- Additive: AI on messages (docs/ai-messages.org). AI messages, auto-reply
-- rules and intent routes all start empty: nothing is drafted for history
-- already in a conversation, no rule answers and no route assigns. The
-- organization's switch arrives not set (AI messages on, drafting only
-- where its AI is configured) and its business hours empty (no after-hours
-- replies until they are set).
SELECT conversation_id, contact_id, deal_id, kind, state, channel, source_type, source_id, rule_id, intent, details, body, next_step, reason, removed, provider, model, approved_by, decided_at, outcome, source_key FROM ai_messages WHERE 1 = 0;
SELECT name, trigger, channel, wording, template, auto_send, position, active FROM ai_message_rules WHERE 1 = 0;
SELECT intent, owner_id, team_id, cursor, position, active FROM ai_message_routes WHERE 1 = 0;
SELECT ai_messages_off, business_hours FROM organizations WHERE 1 = 0;
