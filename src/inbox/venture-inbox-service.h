/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-inbox-service.h - One inbox: a conversation per contact
 *
 * The service keeps each contact's conversation -- status, owner and the
 * times it sorts by -- and assembles the thread from every registered
 * source when asked. A new inbound record on any channel makes the
 * conversation (open and unassigned) or reopens a waiting or closed one.
 * Replies go out through the source of the message being answered, so
 * each channel's own send path, consent and limits apply. It also owns
 * web chat, the one channel that had no records of its own.
 *
 * Customer conversations only: staff notifications (watches, mentions,
 * the sidebar Inbox) are venture-notify.c and are not here.
 */
#ifndef VENTURE_INBOX_SERVICE_H
#define VENTURE_INBOX_SERVICE_H
G_BEGIN_DECLS

/**
 * VENTURE_INBOX_CHAT_NAME_MAX:
 * The longest name a chat visitor may give, in characters.
 */
#define VENTURE_INBOX_CHAT_NAME_MAX 100
/**
 * VENTURE_INBOX_CHAT_EMAIL_MAX:
 * The longest email address a chat visitor may give, in bytes.
 */
#define VENTURE_INBOX_CHAT_EMAIL_MAX 254
/**
 * VENTURE_INBOX_CHAT_PHONE_MAX:
 * The longest phone number a chat visitor may give, in bytes.
 */
#define VENTURE_INBOX_CHAT_PHONE_MAX 32
/**
 * VENTURE_INBOX_CHAT_BODY_MAX:
 * The longest chat message, in characters.
 */
#define VENTURE_INBOX_CHAT_BODY_MAX 2000
/**
 * VENTURE_INBOX_CHAT_MAX_REQUEST:
 * The largest request the public chat reads, in bytes.
 */
#define VENTURE_INBOX_CHAT_MAX_REQUEST 16384
/**
 * VENTURE_INBOX_CHAT_WINDOW_SECONDS:
 * The window the chat limits count in.
 */
#define VENTURE_INBOX_CHAT_WINDOW_SECONDS 600
/**
 * VENTURE_INBOX_CHAT_STARTS_PER_WINDOW:
 * How many chats one address may start on one chat in a window.
 */
#define VENTURE_INBOX_CHAT_STARTS_PER_WINDOW 5
/**
 * VENTURE_INBOX_CHAT_MESSAGES_PER_WINDOW:
 * How many visitor messages one chat takes in a window.
 */
#define VENTURE_INBOX_CHAT_MESSAGES_PER_WINDOW 20

/**
 * VENTURE_INBOX_ERROR:
 * The inbox's own error domain, for what #VentureError has no word for.
 */
#define VENTURE_INBOX_ERROR (venture_inbox_error_quark())
/**
 * venture_inbox_error_quark:
 * Returns: the inbox error domain
 */
GQuark venture_inbox_error_quark(void);
/**
 * VentureInboxError:
 * @VENTURE_INBOX_ERROR_RATE_LIMITED: too many chats or messages from one
 *   place in the window; try again later
 *
 * Inbox refusals.
 */
typedef enum {
	VENTURE_INBOX_ERROR_RATE_LIMITED
} VentureInboxError;

/**
 * VentureInboxView:
 * @VENTURE_INBOX_VIEW_ALL: every conversation
 * @VENTURE_INBOX_VIEW_MINE: the ones a person owns
 * @VENTURE_INBOX_VIEW_UNASSIGNED: the ones nobody owns
 *
 * Whose conversations a list shows.
 */
typedef enum {
	VENTURE_INBOX_VIEW_ALL,
	VENTURE_INBOX_VIEW_MINE,
	VENTURE_INBOX_VIEW_UNASSIGNED
} VentureInboxView;

/**
 * VentureInboxFilter:
 * @organization_id: one organization, or zero for every one the caller may read
 * @view: whose
 * @user_id: the person for %VENTURE_INBOX_VIEW_MINE
 * @status: a #VentureInboxStatus, or -1 for any
 * @channel: (nullable): only conversations that used this channel
 * @limit: the most rows, 1 to 500; zero is 100
 *
 * What a conversation list shows.
 */
typedef struct {
	gint64 organization_id;
	VentureInboxView view;
	gint64 user_id;
	gint status;
	const gchar *channel;
	guint limit;
} VentureInboxFilter;

/**
 * VentureInboxChatIntake:
 * @name: what the visitor calls themselves
 * @email: (nullable): their email address
 * @phone: (nullable): their phone number
 * @body: their first message
 * @trap: (nullable): the hidden field a person leaves empty
 * @remote: (nullable): the address they connected from, for the limits
 *
 * What a visitor typed into the chat form, untrusted.
 */
typedef struct {
	const gchar *name;
	const gchar *email;
	const gchar *phone;
	const gchar *body;
	const gchar *trap;
	const gchar *remote;
} VentureInboxChatIntake;

#define VENTURE_TYPE_INBOX_SERVICE (venture_inbox_service_get_type())
G_DECLARE_FINAL_TYPE(VentureInboxService, venture_inbox_service, VENTURE, INBOX_SERVICE, GObject)

/**
 * venture_inbox_service_get:
 * @database: repository, weakly held
 * Returns: (transfer none): the repository's inbox service
 */
VentureInboxService *venture_inbox_service_get(VentureDatabase *database);
/**
 * venture_inbox_install:
 * @context: the wiring
 *
 * Installs the write guards and the observer that opens and reopens
 * conversations when a new message arrives on any channel.
 */
void venture_inbox_install(VentureContext *context);
/**
 * venture_inbox_service_thread:
 * @self: the service
 * @contact: the contact
 * @since: (nullable): only what happened after this
 * @limit: the most items, 1 to 1000; zero is 200
 * @error: (out) (optional): storage failure
 * Returns: (transfer container) (element-type VentureInboxItem) (nullable):
 *   every channel's items, oldest first
 */
GPtrArray *venture_inbox_service_thread(VentureInboxService *self, VentureEntity *contact, GDateTime *since,
	guint limit, GError **error);
/**
 * venture_inbox_service_conversation:
 * @self: the service
 * @contact: the contact
 * @create: make it when the contact has messages but no conversation yet
 * @error: (out) (optional): not found, or storage failure
 * Returns: (transfer full) (nullable): the contact's conversation
 */
VentureEntity *venture_inbox_service_conversation(VentureInboxService *self, VentureEntity *contact,
	gboolean create, GError **error);
/**
 * venture_inbox_service_list:
 * @self: the service
 * @filter: what to show
 * @error: (out) (optional): storage failure
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   conversations the caller may read, latest message first
 */
GPtrArray *venture_inbox_service_list(VentureInboxService *self, const VentureInboxFilter *filter, GError **error);
/**
 * venture_inbox_service_assign:
 * @self: the service
 * @conversation: the conversation
 * @owner_id: a person in the organization, or zero to unassign
 * @actor: (nullable): who did it, for the audit trail
 * @error: (out) (optional): refusal
 * Returns: whether the owner changed (or already was @owner_id)
 */
gboolean venture_inbox_service_assign(VentureInboxService *self, VentureEntity *conversation, gint64 owner_id,
	const VentureActor *actor, GError **error);
/**
 * venture_inbox_service_set_status:
 * @self: the service
 * @conversation: the conversation
 * @status: open, waiting or closed
 * @now: the clock
 * @actor: (nullable): who did it, for the audit trail
 * @error: (out) (optional): refusal
 * Returns: whether the status is now @status
 */
gboolean venture_inbox_service_set_status(VentureInboxService *self, VentureEntity *conversation,
	VentureInboxStatus status, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_inbox_service_reply_channel:
 * @self: the service
 * @contact: the contact
 * @channel: (nullable): a channel to answer in instead of the latest one
 * @answering: (out) (optional) (transfer full): the message being answered
 * @error: (out) (optional): nothing to answer, in plain words
 * Returns: (transfer none) (nullable): the source a reply goes through:
 *   the channel of the customer's latest message, or @channel
 */
const VentureInboxSource *venture_inbox_service_reply_channel(VentureInboxService *self, VentureEntity *contact,
	const gchar *channel, VentureInboxItem **answering, GError **error);
/**
 * venture_inbox_service_reply:
 * @self: the service
 * @conversation: the conversation
 * @channel: (nullable): a channel instead of the latest one
 * @body: the reply
 * @now: the clock
 * @actor: (nullable): who wrote it
 * @used: (out) (optional) (transfer full): the channel it went through
 * @error: (out) (optional): refusal in plain words
 *
 * Replies in the channel of the customer's latest message through that
 * channel's own send path. The status is left as it is.
 *
 * Returns: (transfer full) (nullable): what happened, in plain words
 */
gchar *venture_inbox_service_reply(VentureInboxService *self, VentureEntity *conversation, const gchar *channel,
	const gchar *body, GDateTime *now, const VentureActor *actor, gchar **used, GError **error);

/**
 * venture_inbox_chat_widget_create:
 * @self: the service
 * @organization_id: the organization
 * @name: (nullable): what to call it
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): a new active web chat with its public token
 */
VentureEntity *venture_inbox_chat_widget_create(VentureInboxService *self, gint64 organization_id, const gchar *name,
	const VentureActor *actor, GError **error);
/**
 * venture_inbox_chat_widget_update:
 * @self: the service
 * @widget: the web chat
 * @greeting: (nullable): the words above the form
 * @allowed_origins: (nullable): the sites that may frame it, one per line
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: whether it was saved
 */
gboolean venture_inbox_chat_widget_update(VentureInboxService *self, VentureEntity *widget, const gchar *greeting,
	const gchar *allowed_origins, const VentureActor *actor, GError **error);
/**
 * venture_inbox_chat_widget_set_active:
 * @self: the service
 * @widget: the web chat
 * @active: whether it takes chats
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: whether it was saved
 */
gboolean venture_inbox_chat_widget_set_active(VentureInboxService *self, VentureEntity *widget, gboolean active,
	const VentureActor *actor, GError **error);
/**
 * venture_inbox_chat_widget_lookup:
 * @self: the service
 * @token: (nullable): the public token from the address
 * Returns: (transfer full) (nullable): the active web chat
 */
VentureEntity *venture_inbox_chat_widget_lookup(VentureInboxService *self, const gchar *token);
/**
 * venture_inbox_chat_start:
 * @self: the service
 * @widget_token: the web chat's public token
 * @intake: what the visitor typed
 * @now: the clock
 * @visitor_token: (out) (transfer full) (nullable): the secret for the
 *   visitor's own link, shown only to them
 * @error: (out) (optional): unknown chat (not found), a bad field
 *   (validation) or too many (%VENTURE_INBOX_ERROR_RATE_LIMITED)
 *
 * Matches the visitor to a contact by email, then phone, the way lead
 * capture compares them, or makes one; then records their first message.
 * A filled-in trap field returns %NULL with no error and keeps nothing.
 *
 * Returns: (transfer full) (nullable): the visitor's chat
 */
VentureEntity *venture_inbox_chat_start(VentureInboxService *self, const gchar *widget_token,
	const VentureInboxChatIntake *intake, GDateTime *now, gchar **visitor_token, GError **error);
/**
 * venture_inbox_chat_session_lookup:
 * @self: the service
 * @visitor_token: (nullable): the secret from the visitor's link
 * Returns: (transfer full) (nullable): their chat
 */
VentureEntity *venture_inbox_chat_session_lookup(VentureInboxService *self, const gchar *visitor_token);
/**
 * venture_inbox_chat_post:
 * @self: the service
 * @session: the visitor's chat
 * @body: their message
 * @remote: (nullable): where they connected from
 * @now: the clock
 * @error: (out) (optional): a bad message, or too many
 * Returns: (transfer full) (nullable): the message
 */
VentureEntity *venture_inbox_chat_post(VentureInboxService *self, VentureEntity *session, const gchar *body,
	const gchar *remote, GDateTime *now, GError **error);
/**
 * venture_inbox_chat_answer:
 * @self: the service
 * @session: the visitor's chat
 * @body: the reply
 * @author: (nullable): who wrote it, as people see it
 * @now: the clock
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: (transfer full) (nullable): the reply, which the visitor sees on their link
 */
VentureEntity *venture_inbox_chat_answer(VentureInboxService *self, VentureEntity *session, const gchar *body,
	const gchar *author, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * venture_inbox_chat_messages:
 * @self: the service
 * @session: the visitor's chat
 * @after_id: only messages with a larger id; zero for all
 * @error: (out) (optional): storage failure
 * Returns: (transfer full) (element-type VentureEntity) (nullable): this
 *   chat's messages, oldest first
 */
GPtrArray *venture_inbox_chat_messages(VentureInboxService *self, VentureEntity *session, gint64 after_id, GError **error);
G_END_DECLS
#endif
