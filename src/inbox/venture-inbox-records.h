/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_INBOX_RECORDS_H
#define VENTURE_INBOX_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS

/**
 * VentureInboxStatus:
 * @VENTURE_INBOX_STATUS_OPEN: needs an answer from us; the default
 * @VENTURE_INBOX_STATUS_WAITING: waiting on the customer
 * @VENTURE_INBOX_STATUS_CLOSED: nothing more to do
 *
 * Where a conversation stands. Open is the zero value, so a conversation
 * made without saying is open.
 */
typedef enum {
	VENTURE_INBOX_STATUS_OPEN,
	VENTURE_INBOX_STATUS_WAITING,
	VENTURE_INBOX_STATUS_CLOSED
} VentureInboxStatus;
/**
 * venture_inbox_status_get_type:
 * Returns: the open/waiting/closed enumeration
 */
GType venture_inbox_status_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_INBOX_CONVERSATION (venture_inbox_conversation_get_type())
VENTURE_DECLARE_ENTITY(VentureInboxConversation, venture_inbox_conversation, INBOX_CONVERSATION)
#define VENTURE_TYPE_INBOX_CHAT_WIDGET (venture_inbox_chat_widget_get_type())
VENTURE_DECLARE_ENTITY(VentureInboxChatWidget, venture_inbox_chat_widget, INBOX_CHAT_WIDGET)
#define VENTURE_TYPE_INBOX_CHAT_SESSION (venture_inbox_chat_session_get_type())
VENTURE_DECLARE_ENTITY(VentureInboxChatSession, venture_inbox_chat_session, INBOX_CHAT_SESSION)
#define VENTURE_TYPE_INBOX_CHAT_MESSAGE (venture_inbox_chat_message_get_type())
VENTURE_DECLARE_ENTITY(VentureInboxChatMessage, venture_inbox_chat_message, INBOX_CHAT_MESSAGE)

G_END_DECLS
#endif
