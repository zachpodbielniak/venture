/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-inbox-source.h - A channel's way into the one inbox
 *
 * Each channel (email, texts, calls, web chat, and whatever comes next)
 * is one VentureInboxSource in that channel's own file, registered with
 * VENTURE_INBOX_SOURCE(). The inbox never names a channel: it asks every
 * registered source for a contact's items, lets each render its own row,
 * and hands a reply to the source of the message being answered. A
 * source lists the channel's existing records by reference; nothing is
 * copied into the inbox.
 */
#ifndef VENTURE_INBOX_SOURCE_H
#define VENTURE_INBOX_SOURCE_H
#include <glib-object.h>
G_BEGIN_DECLS

typedef struct _VentureInboxSource VentureInboxSource;

/**
 * VentureInboxItem:
 * @source: the channel it came through
 * @record: the channel's own record (a mail interaction, a call activity,
 *   a text message, a chat message), referenced
 * @at: when it happened
 * @inbound: whether the customer sent it
 *
 * One message in a conversation, by reference.
 */
typedef struct {
	const VentureInboxSource *source;
	VentureEntity *record;
	GDateTime *at;
	gboolean inbound;
} VentureInboxItem;

/**
 * venture_inbox_item_new:
 * @source: the channel
 * @record: the record; a reference is taken
 * @at: when; a reference is taken
 * @inbound: whether the customer sent it
 * Returns: (transfer full): the item
 */
VentureInboxItem *venture_inbox_item_new(const VentureInboxSource *source, VentureEntity *record, GDateTime *at, gboolean inbound);
/**
 * venture_inbox_item_free:
 * @item: (nullable): an item
 */
void venture_inbox_item_free(VentureInboxItem *item);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureInboxItem, venture_inbox_item_free)

/**
 * VentureInboxListFunc:
 * @context: the wiring
 * @contact: the contact
 * @since: (nullable): only items after this time
 * @limit: the most items wanted
 * @items: (element-type VentureInboxItem): where to add them, in any order
 * @error: (out) (optional): storage failure
 *
 * Lists one contact's items in this channel, from the channel's records.
 * Returns: whether listing worked
 */
typedef gboolean (*VentureInboxListFunc)(VentureContext *context, VentureEntity *contact, GDateTime *since,
	guint limit, GPtrArray *items, GError **error);
/**
 * VentureInboxObserveFunc:
 * @record: a record just created
 * @contact_id: (out): the contact it belongs to
 * @at: (out) (transfer full): when it happened
 * @inbound: (out): whether the customer sent it
 *
 * Whether a newly written record is a message in this channel. The inbox
 * calls it for every new record, so it decides from the record alone.
 * Returns: %TRUE when it is
 */
typedef gboolean (*VentureInboxObserveFunc)(VentureEntity *record, gint64 *contact_id, GDateTime **at, gboolean *inbound);
/**
 * VentureInboxRenderFunc:
 * @context: the wiring
 * @item: one of this channel's items
 * @html: where the row's inner HTML goes; everything escaped
 */
typedef void (*VentureInboxRenderFunc)(VentureContext *context, const VentureInboxItem *item, GString *html);
/**
 * VentureInboxSummaryFunc:
 * @context: the wiring
 * @item: one of this channel's items
 * Returns: (transfer full): the item as one plain line of text
 */
typedef gchar *(*VentureInboxSummaryFunc)(VentureContext *context, const VentureInboxItem *item);
/**
 * VentureInboxReplyFunc:
 * @context: the wiring
 * @contact: the contact
 * @answering: (nullable): the customer's latest message in this channel;
 *   %NULL when someone chose the channel and they have not written in it
 * @body: the reply, already checked to have words
 * @now: the clock
 * @actor: (nullable): who wrote it
 * @error: (out) (optional): refusal in plain words
 *
 * Sends a reply through the channel's own send path.
 * Returns: (transfer full) (nullable): what happened, in plain words
 */
typedef gchar *(*VentureInboxReplyFunc)(VentureContext *context, VentureEntity *contact, const VentureInboxItem *answering,
	const gchar *body, GDateTime *now, const VentureActor *actor, GError **error);
/**
 * VentureInboxActionFunc:
 * @context: the wiring
 * @contact: the contact
 * @html: where the action's HTML goes
 *
 * What to offer instead of a reply box for a channel that is not
 * answered by writing, such as calling back.
 */
typedef void (*VentureInboxActionFunc)(VentureContext *context, VentureEntity *contact, GString *html);

/**
 * VentureInboxSource:
 * @channel: the stable channel id, for example "email"
 * @label: the name people see
 * @module: (nullable): the module that must be on for the channel to show
 * @list: lists a contact's items
 * @observe: (nullable): recognises a new record as an item, so a new
 *   message opens or reopens the conversation
 * @render: renders one item's row
 * @summary: one item as a line of text, for lists and the API
 * @reply: (nullable): answers in this channel; %NULL when it cannot be
 * @action: (nullable): what to offer when it cannot be answered by writing
 *
 * One channel of the inbox.
 */
struct _VentureInboxSource {
	const gchar *channel;
	const gchar *label;
	const gchar *module;
	VentureInboxListFunc list;
	VentureInboxObserveFunc observe;
	VentureInboxRenderFunc render;
	VentureInboxSummaryFunc summary;
	VentureInboxReplyFunc reply;
	VentureInboxActionFunc action;
};

/**
 * venture_inbox_source_register:
 * @source: a static source; registering it twice changes nothing
 */
void venture_inbox_source_register(const VentureInboxSource *source);
/**
 * venture_inbox_source_lookup:
 * @channel: (nullable): a channel id
 * Returns: (transfer none) (nullable): its source
 */
const VentureInboxSource *venture_inbox_source_lookup(const gchar *channel);
/**
 * venture_inbox_sources:
 * Returns: (transfer container) (element-type VentureInboxSource): every
 *   registered source, by channel id
 */
GPtrArray *venture_inbox_sources(void);

/**
 * VENTURE_INBOX_SOURCE:
 * @symbol: a static #VentureInboxSource
 *
 * Put once at the end of a channel's file. The server and the tests link
 * the whole library, so the constructor runs in every binary that has the
 * channel; no list anywhere names it.
 */
#define VENTURE_INBOX_SOURCE(symbol) \
	static void symbol##_register(void) __attribute__((constructor)); \
	static void symbol##_register(void) { venture_inbox_source_register(&symbol); }

/**
 * VentureInboxPanelFunc:
 * @context: the wiring
 * @conversation: the conversation being shown
 * @contact: its contact
 * @may_edit: whether the viewer may change things (an editor)
 * @html: where the panel's HTML goes; everything escaped
 *
 * Adds a card to the conversation page, above the reply box.
 */
typedef void (*VentureInboxPanelFunc)(VentureContext *context, VentureEntity *conversation, VentureEntity *contact,
	gboolean may_edit, GString *html);
/**
 * VentureInboxPanel:
 * @id: a stable id, for ordering
 * @module: (nullable): the module that must be on for it to show
 * @render: renders the card
 *
 * A feature's card on the conversation page, such as AI drafts.
 */
typedef struct {
	const gchar *id;
	const gchar *module;
	VentureInboxPanelFunc render;
} VentureInboxPanel;
/**
 * venture_inbox_panel_register:
 * @panel: a static panel; registering it twice changes nothing
 */
void venture_inbox_panel_register(const VentureInboxPanel *panel);
/**
 * venture_inbox_panels:
 * Returns: (transfer container) (element-type VentureInboxPanel): every
 *   registered panel, by id
 */
GPtrArray *venture_inbox_panels(void);
/**
 * VENTURE_INBOX_PANEL:
 * @symbol: a static #VentureInboxPanel
 *
 * Put once at the end of the feature's file, like VENTURE_INBOX_SOURCE().
 */
#define VENTURE_INBOX_PANEL(symbol) \
	static void symbol##_register(void) __attribute__((constructor)); \
	static void symbol##_register(void) { venture_inbox_panel_register(&symbol); }

G_END_DECLS
#endif
