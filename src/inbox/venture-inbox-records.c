/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* One inbox (docs/inbox.org). A conversation is the per-contact state the
 * inbox adds -- status, owner and the times it sorts by -- and never a
 * copy of a message: the thread is read from each channel's own records
 * (mail interactions, call activities, text messages, chat messages) when
 * it is shown. Web chat is the one channel that had no records of its own,
 * so its intake, its visitor links and its messages are here. */

GType
venture_inbox_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_INBOX_STATUS_OPEN, "VENTURE_INBOX_STATUS_OPEN", "open" },
			{ VENTURE_INBOX_STATUS_WAITING, "VENTURE_INBOX_STATUS_WAITING", "waiting" },
			{ VENTURE_INBOX_STATUS_CLOSED, "VENTURE_INBOX_STATUS_CLOSED", "closed" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureInboxStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

/* The contact key makes it one conversation per contact per organization,
 * however many channels and however often two writers race to make it.
 * Written only by the inbox service. */
static const VentureFieldDecl conversation_fields[] = {
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", "open, waiting (on the customer) or closed", venture_inbox_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("owner-id", "Owner", "The person answering it; empty is unassigned", "user", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("status-changed-at", "Status changed", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-message-at", "Last message", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("last-inbound-at", "Last from them", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-inbound-channel", "Last channel they used", "email, text, call or chat", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("channels", "Channels", "Every channel seen, as ,email,text,", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("contact-key", "One per contact", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInboxConversation, venture_inbox_conversation, conversation_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Conversation", "Conversations");)

/* One web chat intake an organization embeds on its site. The token is
 * in the embed code, so it is public by nature; what it opens is only a
 * form. Written only by the inbox service. */
static const VentureFieldDecl widget_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this chat, for example Website chat"),
	VENTURE_FIELD("token", "Address token", "The public part of the chat address", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_TEXT("greeting", "Greeting", "Shown above the form"),
	VENTURE_FIELD_TEXT("allowed-origins", "Sites that may show it", "Origins allowed to frame the chat, one per line, for example https://www.example.com; empty allows any"),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInboxChatWidget, venture_inbox_chat_widget, widget_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Web chat", "Web chats");)

/* One visitor's chat: what they gave and the hash of their conversation
 * link's token (the token itself is only in the link they were sent to).
 * The link shows this session's messages and nothing else of the contact. */
static const VentureFieldDecl session_fields[] = {
	VENTURE_FIELD_REF("widget-id", "Web chat", NULL, "inbox_chat_widget", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("visitor-name", "Name they gave", NULL),
	VENTURE_FIELD("visitor-email", "Email they gave", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("visitor-phone", "Phone they gave", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("token-hash", "Link token hash", NULL, VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("last-message-at", "Last message", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInboxChatSession, venture_inbox_chat_session, session_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Chat visitor", "Chat visitors");)

/* One chat message, either way. This is the chat channel's own record,
 * the way a text message is the text channel's. */
static const VentureFieldDecl message_fields[] = {
	VENTURE_FIELD_REF("session-id", "Chat", NULL, "inbox_chat_session", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("inbound", "From the visitor", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("body", "Message", NULL),
	VENTURE_FIELD("author", "Written by", "The person who answered; empty for the visitor", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sent-at", "Sent at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInboxChatMessage, venture_inbox_chat_message, message_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Chat message", "Chat messages");)
