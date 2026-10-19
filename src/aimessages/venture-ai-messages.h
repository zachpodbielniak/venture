/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-ai-messages.h - AI on inbound and outbound messages
 *
 * When a customer's message lands in a conversation the service reads it
 * (intent, and the details the message itself gives), drafts a reply,
 * routes the conversation, and lets the owner's auto-reply rules answer.
 * Calls with a transcript are summarised; a call without one says so.
 * Follow-ups for a contact or a deal are drafted from their records and
 * held to the grounding rule of designed email.
 *
 * Every AI message is a draft a person approves unless its rule sends on
 * its own, and every send goes through the channel's one send path (the
 * inbox reply: the outbox for email, the text path with its consent and
 * quiet hours). Nothing here talks to a provider of any kind but the AI.
 */
#ifndef VENTURE_AI_MESSAGES_H
#define VENTURE_AI_MESSAGES_H
G_BEGIN_DECLS

/**
 * VENTURE_AI_MESSAGES_NO_TRANSCRIPT:
 * What a call summary says when there is nothing to summarise.
 */
#define VENTURE_AI_MESSAGES_NO_TRANSCRIPT "No transcript"
/**
 * VENTURE_AI_MESSAGES_AUTO_WINDOW_SECONDS:
 * One rule answers one contact at most once in this many seconds.
 */
#define VENTURE_AI_MESSAGES_AUTO_WINDOW_SECONDS (12 * 3600)
/**
 * VENTURE_AI_MESSAGES_APPROVED_AUTO:
 * The approved-by of a message a rule sent on its own.
 */
#define VENTURE_AI_MESSAGES_APPROVED_AUTO "auto"

#define VENTURE_TYPE_AI_MESSAGES (venture_ai_messages_get_type())
G_DECLARE_FINAL_TYPE(VentureAiMessages, venture_ai_messages, VENTURE, AI_MESSAGES, GObject)

/**
 * venture_ai_messages_get:
 * @database: repository, weakly held
 * Returns: (transfer none): the repository's AI messages service
 */
VentureAiMessages *venture_ai_messages_get(VentureDatabase *database);
/**
 * venture_ai_messages_install:
 * @context: the wiring
 *
 * Installs the write guards and the observer that reads each new inbound
 * message after its transaction commits.
 */
void venture_ai_messages_install(VentureContext *context);
/**
 * venture_ai_messages_status:
 * @self: the service
 * @organization_id: the organization
 * @drafting: (out) (optional): whether the AI may draft (it is on and the
 *   organization's AI is configured)
 * @why: (out) (optional) (transfer full): when off, why, in plain words
 * Returns: whether AI messaging is on at all for the organization
 */
gboolean venture_ai_messages_status(VentureAiMessages *self, gint64 organization_id, gboolean *drafting, gchar **why);
/**
 * venture_ai_messages_set_off:
 * @self: the service
 * @organization_id: the organization
 * @off: the one switch: %TRUE stops every draft, summary and send
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: whether it was saved
 */
gboolean venture_ai_messages_set_off(VentureAiMessages *self, gint64 organization_id, gboolean off,
	const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_set_business_hours:
 * @self: the service
 * @organization_id: the organization
 * @hours: (nullable): for example "Mon-Fri 09:00-17:00; Sat 10:00-14:00"; empty for none
 * @actor: (nullable): audit actor
 * @error: (out) (optional): a malformed list
 * Returns: whether it was saved
 */
gboolean venture_ai_messages_set_business_hours(VentureAiMessages *self, gint64 organization_id, const gchar *hours,
	const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_business_hours_open:
 * @hours: (nullable): business hours as venture_ai_messages_set_business_hours() takes them
 * @zone: the organization's time zone
 * @at: the moment
 * @open: (out): whether @at is inside them
 * @error: (out) (optional): a malformed list
 * Returns: whether @hours could be read; empty hours are never open
 */
gboolean venture_ai_messages_business_hours_open(const gchar *hours, GTimeZone *zone, GDateTime *at, gboolean *open,
	GError **error);
/**
 * venture_ai_messages_save_rule:
 * @self: the service
 * @rule: an auto-reply rule, new or changed
 * @actor: (nullable): audit actor
 * @error: (out) (optional): a malformed rule
 * Returns: whether it was saved
 */
gboolean venture_ai_messages_save_rule(VentureAiMessages *self, VentureEntity *rule, const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_save_route:
 * @self: the service
 * @route: an intent route, new or changed
 * @actor: (nullable): audit actor
 * @error: (out) (optional): a malformed route
 * Returns: whether it was saved
 */
gboolean venture_ai_messages_save_route(VentureAiMessages *self, VentureEntity *route, const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_remove:
 * @self: the service
 * @row: a rule or a route
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: whether it was removed
 */
gboolean venture_ai_messages_remove(VentureAiMessages *self, VentureEntity *row, const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_inbound:
 * @self: the service
 * @record: a message a channel just recorded (a text, an email, a chat
 *   message or a call)
 * @now: the clock
 * @error: (out) (optional): storage failure
 *
 * What the observer does once the message is committed: summarise a
 * call, read the message, route the conversation and let the rules
 * answer. Runs at most once per message.
 *
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   the AI messages made, possibly none
 */
GPtrArray *venture_ai_messages_inbound(VentureAiMessages *self, VentureEntity *record, GDateTime *now, GError **error);
/**
 * venture_ai_messages_draft_reply:
 * @self: the service
 * @conversation: the conversation
 * @now: the clock
 * @actor: (nullable): who asked
 * @error: (out) (optional): off, no AI, nothing to answer, or ungrounded
 * Returns: (transfer full) (nullable): a new draft answering their latest message
 */
VentureEntity *venture_ai_messages_draft_reply(VentureAiMessages *self, VentureEntity *conversation, GDateTime *now,
	const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_follow_up:
 * @self: the service
 * @subject: a contact or a deal
 * @channel: "email" or "text"
 * @instruction: (nullable): what the follow-up should do, in a few words
 * @now: the clock
 * @actor: (nullable): who asked
 * @error: (out) (optional): off, no AI, no contact, or nothing grounded left
 * Returns: (transfer full) (nullable): a follow-up draft with a suggested next step
 */
VentureEntity *venture_ai_messages_follow_up(VentureAiMessages *self, VentureEntity *subject, const gchar *channel,
	const gchar *instruction, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_approve:
 * @self: the service
 * @message: a draft
 * @body: (nullable): the words as the person edited them; %NULL keeps the draft's
 * @now: the clock
 * @approver: who approved it
 * @error: (out) (optional): not a draft, off, or the send path's refusal
 *
 * Sends the draft through its channel's one send path. A refusal is
 * recorded on the message as well as returned.
 *
 * Returns: (transfer full) (nullable): the message in its resulting state
 */
VentureEntity *venture_ai_messages_approve(VentureAiMessages *self, VentureEntity *message, const gchar *body,
	GDateTime *now, const VentureActor *approver, GError **error);
/**
 * venture_ai_messages_discard:
 * @self: the service
 * @message: a draft
 * @actor: (nullable): who discarded it
 * @error: (out) (optional): not a draft
 * Returns: whether it is now discarded
 */
gboolean venture_ai_messages_discard(VentureAiMessages *self, VentureEntity *message, const VentureActor *actor, GError **error);
/**
 * venture_ai_messages_for_contact:
 * @self: the service
 * @contact: the contact
 * @error: (out) (optional): storage failure
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   the contact's AI messages, newest first, that the caller may read
 */
GPtrArray *venture_ai_messages_for_contact(VentureAiMessages *self, VentureEntity *contact, GError **error);
G_END_DECLS
#endif
