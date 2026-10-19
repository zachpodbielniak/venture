/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_AI_MESSAGES_RECORDS_H
#define VENTURE_AI_MESSAGES_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS

/**
 * VentureAiMessageKind:
 * @VENTURE_AI_MESSAGE_KIND_REPLY: a drafted answer to something a customer sent
 * @VENTURE_AI_MESSAGE_KIND_AUTO_REPLY: an answer an owner's auto-reply rule made
 * @VENTURE_AI_MESSAGE_KIND_SUMMARY: a call or voicemail summary; never sent
 * @VENTURE_AI_MESSAGE_KIND_FOLLOW_UP: a follow-up to a contact or a deal
 *
 * What an AI message is for.
 */
typedef enum {
	VENTURE_AI_MESSAGE_KIND_REPLY,
	VENTURE_AI_MESSAGE_KIND_AUTO_REPLY,
	VENTURE_AI_MESSAGE_KIND_SUMMARY,
	VENTURE_AI_MESSAGE_KIND_FOLLOW_UP
} VentureAiMessageKind;
/**
 * venture_ai_message_kind_get_type:
 * Returns: the reply/auto_reply/summary/follow_up enumeration
 */
GType venture_ai_message_kind_get_type(void) G_GNUC_CONST;

/**
 * VentureAiMessageState:
 * @VENTURE_AI_MESSAGE_STATE_DRAFT: waiting for a person to approve it; the default
 * @VENTURE_AI_MESSAGE_STATE_SENT: handed to the channel's send path
 * @VENTURE_AI_MESSAGE_STATE_HELD: the send path held it for quiet hours
 * @VENTURE_AI_MESSAGE_STATE_REFUSED: the send path or the grounding check refused it
 * @VENTURE_AI_MESSAGE_STATE_DISCARDED: a person threw it away
 * @VENTURE_AI_MESSAGE_STATE_NOTE: a summary, which is read rather than sent
 *
 * Where an AI message stands. Draft is the zero value, so nothing is
 * ever sent by default.
 */
typedef enum {
	VENTURE_AI_MESSAGE_STATE_DRAFT,
	VENTURE_AI_MESSAGE_STATE_SENT,
	VENTURE_AI_MESSAGE_STATE_HELD,
	VENTURE_AI_MESSAGE_STATE_REFUSED,
	VENTURE_AI_MESSAGE_STATE_DISCARDED,
	VENTURE_AI_MESSAGE_STATE_NOTE
} VentureAiMessageState;
/**
 * venture_ai_message_state_get_type:
 * Returns: the draft/sent/held/refused/discarded/note enumeration
 */
GType venture_ai_message_state_get_type(void) G_GNUC_CONST;

/**
 * VentureAiMessageTrigger:
 * @VENTURE_AI_MESSAGE_TRIGGER_AFTER_HOURS: a customer wrote outside the
 *   organization's business hours
 * @VENTURE_AI_MESSAGE_TRIGGER_MISSED_CALL: a customer's call went unanswered
 *
 * When an auto-reply rule answers.
 */
typedef enum {
	VENTURE_AI_MESSAGE_TRIGGER_AFTER_HOURS,
	VENTURE_AI_MESSAGE_TRIGGER_MISSED_CALL
} VentureAiMessageTrigger;
/**
 * venture_ai_message_trigger_get_type:
 * Returns: the after_hours/missed_call enumeration
 */
GType venture_ai_message_trigger_get_type(void) G_GNUC_CONST;

/**
 * VentureAiMessageWording:
 * @VENTURE_AI_MESSAGE_WORDING_TEMPLATE: the rule's own words
 * @VENTURE_AI_MESSAGE_WORDING_AI: a draft the AI writes for the message
 *
 * Where an auto-reply's words come from.
 */
typedef enum {
	VENTURE_AI_MESSAGE_WORDING_TEMPLATE,
	VENTURE_AI_MESSAGE_WORDING_AI
} VentureAiMessageWording;
/**
 * venture_ai_message_wording_get_type:
 * Returns: the template/ai enumeration
 */
GType venture_ai_message_wording_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_AI_MESSAGE (venture_ai_message_get_type())
VENTURE_DECLARE_ENTITY(VentureAiMessage, venture_ai_message, AI_MESSAGE)
#define VENTURE_TYPE_AI_MESSAGE_RULE (venture_ai_message_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureAiMessageRule, venture_ai_message_rule, AI_MESSAGE_RULE)
#define VENTURE_TYPE_AI_MESSAGE_ROUTE (venture_ai_message_route_get_type())
VENTURE_DECLARE_ENTITY(VentureAiMessageRoute, venture_ai_message_route, AI_MESSAGE_ROUTE)

G_END_DECLS
#endif
