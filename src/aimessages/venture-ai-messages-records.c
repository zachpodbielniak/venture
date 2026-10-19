/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* AI on messages (docs/ai-messages.org). An AI message is both the draft a
 * person approves and the log of what happened to it: which rule made it,
 * which provider and model wrote it, why, who approved it (or "auto") and
 * what the channel's send path said. It is never the message itself: a
 * sent reply is the channel's own record (a text, an email, a chat
 * message), written by the channel's one send path. Rules and routes are
 * what an owner configures. All three are written only by the AI messages
 * service. */

static GType
register_enum(gsize *once, const gchar *name, const GEnumValue *values)
{
	if (g_once_init_enter(once))
		g_once_init_leave(once, g_enum_register_static(name, values));
	return *once;
}

GType
venture_ai_message_kind_get_type(void)
{
	static gsize type_id = 0;
	static const GEnumValue values[] = {
		{ VENTURE_AI_MESSAGE_KIND_REPLY, "VENTURE_AI_MESSAGE_KIND_REPLY", "reply" },
		{ VENTURE_AI_MESSAGE_KIND_AUTO_REPLY, "VENTURE_AI_MESSAGE_KIND_AUTO_REPLY", "auto_reply" },
		{ VENTURE_AI_MESSAGE_KIND_SUMMARY, "VENTURE_AI_MESSAGE_KIND_SUMMARY", "summary" },
		{ VENTURE_AI_MESSAGE_KIND_FOLLOW_UP, "VENTURE_AI_MESSAGE_KIND_FOLLOW_UP", "follow_up" },
		{ 0, NULL, NULL }
	};
	return register_enum(&type_id, "VentureAiMessageKind", values);
}

GType
venture_ai_message_state_get_type(void)
{
	static gsize type_id = 0;
	static const GEnumValue values[] = {
		{ VENTURE_AI_MESSAGE_STATE_DRAFT, "VENTURE_AI_MESSAGE_STATE_DRAFT", "draft" },
		{ VENTURE_AI_MESSAGE_STATE_SENT, "VENTURE_AI_MESSAGE_STATE_SENT", "sent" },
		{ VENTURE_AI_MESSAGE_STATE_HELD, "VENTURE_AI_MESSAGE_STATE_HELD", "held" },
		{ VENTURE_AI_MESSAGE_STATE_REFUSED, "VENTURE_AI_MESSAGE_STATE_REFUSED", "refused" },
		{ VENTURE_AI_MESSAGE_STATE_DISCARDED, "VENTURE_AI_MESSAGE_STATE_DISCARDED", "discarded" },
		{ VENTURE_AI_MESSAGE_STATE_NOTE, "VENTURE_AI_MESSAGE_STATE_NOTE", "note" },
		{ 0, NULL, NULL }
	};
	return register_enum(&type_id, "VentureAiMessageState", values);
}

GType
venture_ai_message_trigger_get_type(void)
{
	static gsize type_id = 0;
	static const GEnumValue values[] = {
		{ VENTURE_AI_MESSAGE_TRIGGER_AFTER_HOURS, "VENTURE_AI_MESSAGE_TRIGGER_AFTER_HOURS", "after_hours" },
		{ VENTURE_AI_MESSAGE_TRIGGER_MISSED_CALL, "VENTURE_AI_MESSAGE_TRIGGER_MISSED_CALL", "missed_call" },
		{ 0, NULL, NULL }
	};
	return register_enum(&type_id, "VentureAiMessageTrigger", values);
}

GType
venture_ai_message_wording_get_type(void)
{
	static gsize type_id = 0;
	static const GEnumValue values[] = {
		{ VENTURE_AI_MESSAGE_WORDING_TEMPLATE, "VENTURE_AI_MESSAGE_WORDING_TEMPLATE", "template" },
		{ VENTURE_AI_MESSAGE_WORDING_AI, "VENTURE_AI_MESSAGE_WORDING_AI", "ai" },
		{ 0, NULL, NULL }
	};
	return register_enum(&type_id, "VentureAiMessageWording", values);
}

/* The draft and its log in one row: the reason, the rule, the provider
 * and model, who approved it and what the send path answered. The
 * source is the message it answers or summarises, by reference. */
static const VentureFieldDecl message_fields[] = {
	VENTURE_FIELD_REF("conversation-id", "Conversation", NULL, "inbox_conversation", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("deal-id", "Deal", "The deal a follow-up is about", "deal", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("kind", "Kind", "reply, auto_reply, summary or follow_up", venture_ai_message_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("state", "State", "draft (waiting for approval), sent, held, refused, discarded or note", venture_ai_message_state_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("channel", "Channel", "email, text or chat; empty for a summary", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source-type", "Answers or summarises", "The record type of the message", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source-id", "Message", "The id of that message", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("rule-id", "Rule", "The auto-reply rule that made it", "ai_message_rule", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("intent", "Intent", "booking, quote, question, complaint, cancellation or other", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("details", "Details", "What the message itself says: name, phone, address, requested date, product or service"),
	VENTURE_FIELD_TEXT("body", "Words", "The draft, or the summary"),
	VENTURE_FIELD_TEXT("next-step", "Suggested next step", NULL),
	VENTURE_FIELD_TEXT("reason", "Reason", "Why it was made and what was decided, in plain words"),
	VENTURE_FIELD_TEXT("removed", "Removed by the grounding check", "Sentences naming a price, offer or date in neither the message nor the records"),
	VENTURE_FIELD("provider", "Provider", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("model", "Model", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("approved-by", "Approved by", "The person who approved it, or auto", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("decided-at", "Decided at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("outcome", "Outcome", "What the channel's send path said"),
	VENTURE_FIELD("source-key", "Once per message", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAiMessage, venture_ai_message, message_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "AI message", "AI messages");)

/* An owner's auto-reply rule. Auto-send is off by default: a rule makes
 * a draft for approval until somebody turns it on. */
static const VentureFieldDecl rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "For example After hours"),
	VENTURE_FIELD_ENUM("trigger", "When", "after_hours or missed_call", venture_ai_message_trigger_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("channel", "Answer by", "text, email or chat; empty answers in the channel they used", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("wording", "Words", "template (the words below) or ai (an AI draft)", venture_ai_message_wording_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("template", "Template", "The reply, when the words are the rule's own"),
	VENTURE_FIELD("auto-send", "Send without approval", "Off by default: the reply waits as a draft for a person", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", "Rules are tried in ascending order; the first that applies answers", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAiMessageRule, venture_ai_message_rule, rule_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Auto-reply rule", "Auto-reply rules");)

/* Which person or team answers a conversation of one intent. A message
 * no route claims falls through to the lead routing rules. */
static const VentureFieldDecl route_fields[] = {
	VENTURE_FIELD("intent", "Intent", "booking, quote, question, complaint, cancellation or other", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("owner-id", "Person", "Who answers it", "user", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("team-id", "Team", "Or a team, in turn", "team", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cursor", "Cursor", "Whose turn it is in the team", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAiMessageRoute, venture_ai_message_route, route_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Intent route", "Intent routes");)
