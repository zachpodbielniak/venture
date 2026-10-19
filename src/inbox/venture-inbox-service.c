/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-inbox-service.c - Conversations: the thread, status, owner, replies
 *
 * A conversation row is bookkeeping about a contact -- who owns it, where
 * it stands, when they last wrote -- and is written only here. The
 * thread is never stored: it is asked of every registered source each
 * time, so a message corrected or removed in its own channel is
 * corrected or removed in the inbox too. Web chat is in
 * venture-inbox-chat.c.
 */
#include "venture-inbox-private.h"
#include <string.h>
#include <errno.h>
#include <sys/random.h>

G_DEFINE_FINAL_TYPE(VentureInboxService, venture_inbox_service, G_TYPE_OBJECT)

G_DEFINE_QUARK(venture-inbox-error-quark, venture_inbox_error)

typedef struct {
	gint64 start;
	guint count;
} InboxWindow;

static void
finalize(GObject *object)
{
	VentureInboxService *self = VENTURE_INBOX_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_weak_ref_clear(&self->context);
	g_clear_pointer(&self->windows, g_hash_table_unref);
	G_OBJECT_CLASS(venture_inbox_service_parent_class)->finalize(object);
}

static void venture_inbox_service_class_init(VentureInboxServiceClass *klass) { G_OBJECT_CLASS(klass)->finalize = finalize; }

static void
venture_inbox_service_init(VentureInboxService *self)
{
	g_weak_ref_init(&self->context, NULL);
	self->windows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

VentureInboxService *
venture_inbox_service_get(VentureDatabase *database)
{
	VentureInboxService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-inbox-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_INBOX_SERVICE, NULL);
		self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-inbox-service", self, g_object_unref);
	}
	return self;
}

/* --- Small helpers ------------------------------------------------------------ */

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}

static gint64
number(gpointer row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

const VentureActor *
venture_inbox_service_actor(const gchar *name)
{
	static VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = name != NULL ? name : "Inbox";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return &actor;
}

gboolean
venture_inbox_service_write(VentureInboxService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing++;
	ok = venture_database_save(self->database, row, actor != NULL ? actor : venture_inbox_service_actor(NULL), error);
	self->writing--;
	return ok;
}

gboolean
venture_inbox_service_allow(VentureInboxService *self, const gchar *key, guint limit, GDateTime *now)
{
	InboxWindow *window;
	gint64 at = g_date_time_to_unix(now);
	/* An address that never comes back must not be remembered forever. */
	if (g_hash_table_size(self->windows) > 20000)
	{
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, self->windows);
		while (g_hash_table_iter_next(&iter, NULL, &value))
			if (at - ((InboxWindow *)value)->start >= VENTURE_INBOX_CHAT_WINDOW_SECONDS)
				g_hash_table_iter_remove(&iter);
	}
	window = g_hash_table_lookup(self->windows, key);
	if (window == NULL)
	{
		window = g_new0(InboxWindow, 1);
		window->start = at;
		g_hash_table_insert(self->windows, g_strdup(key), window);
	}
	else if (at - window->start >= VENTURE_INBOX_CHAT_WINDOW_SECONDS || at < window->start)
	{
		window->start = at;
		window->count = 0;
	}
	if (window->count >= limit)
		return FALSE;
	window->count++;
	return TRUE;
}

gchar *
venture_inbox_random_token(gsize bytes, GError **error)
{
	guchar buffer[64];
	gsize used = 0, i;
	GString *text;
	g_return_val_if_fail(bytes > 0 && bytes <= sizeof buffer, NULL);
	while (used < bytes)
	{
		ssize_t n = getrandom(buffer + used, bytes - used, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0)
		{
			refuse(error, VENTURE_ERROR_FAILED, "No secure randomness is available");
			return NULL;
		}
		used += (gsize)n;
	}
	text = g_string_sized_new(bytes * 2);
	for (i = 0; i < bytes; i++)
		g_string_append_printf(text, "%02x", buffer[i]);
	memset(buffer, 0, sizeof buffer);
	return g_string_free(text, FALSE);
}

static VentureContext *
context_of(VentureInboxService *self)
{
	return g_weak_ref_get(&self->context);
}

static gboolean
source_on(VentureContext *context, const VentureInboxSource *source)
{
	if (source->module == NULL)
		return TRUE;
	return context != NULL && venture_context_module_enabled(context, source->module);
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "inbox_conversation") != G_TYPE_INVALID;
}

static gchar *
contact_key(gint64 contact_id)
{
	return g_strdup_printf("contact:%" G_GINT64_FORMAT, contact_id);
}

/* --- The write guard ----------------------------------------------------------- */

static gboolean
validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	VentureInboxService *self = data;
	(void)database;
	if (previous != NULL && venture_entity_get_organization_id(previous) != venture_entity_get_organization_id(row))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Inbox records cannot move to another organization");
	if (self->writing > 0)
		return TRUE;
	return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
		"Conversations and web chat are written only by the inbox; assign, set a status or reply from the conversation");
}

/* --- Keeping conversations up to date ------------------------------------------- */

static VentureEntity *
find_conversation(VentureInboxService *self, gint64 org, gint64 contact_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INBOX_CONVERSATION);
	g_autofree gchar *key = contact_key(contact_id);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "contact-key", VENTURE_FILTER_OP_EQ, key, NULL);
	return venture_database_find_one(self->database, query, error);
}

static gboolean
later(GDateTime *candidate, GDateTime *current)
{
	return candidate != NULL && (current == NULL || g_date_time_compare(candidate, current) >= 0);
}

/* Records one message on its contact's conversation, making it (open and
 * unassigned) if there is none. Something from the customer reopens a
 * waiting or closed conversation; our own messages never change the
 * status. Runs in the trusted scope: it is derived from a write that was
 * already allowed. */
static gboolean
touch(VentureInboxService *self, gint64 org, gint64 contact_id, const gchar *channel, GDateTime *at,
	gboolean inbound, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GDateTime) last = NULL, last_in = NULL, now = venture_time_now();
	g_autofree gchar *channels = NULL, *marker = NULL;
	gint status = VENTURE_INBOX_STATUS_OPEN;
	row = find_conversation(self, org, contact_id, error);
	if (row == NULL)
	{
		g_autofree gchar *key = NULL;
		if (error != NULL && *error != NULL)
			return FALSE;
		key = contact_key(contact_id);
		row = g_object_new(VENTURE_TYPE_INBOX_CONVERSATION, "organization-id", org, "contact-id", contact_id,
			"contact-key", key, "status", VENTURE_INBOX_STATUS_OPEN, "status-changed-at", now, "channels", ",", NULL);
	}
	g_object_get(row, "last-message-at", &last, "last-inbound-at", &last_in, "channels", &channels, "status", &status, NULL);
	marker = g_strdup_printf(",%s,", channel);
	if (channels == NULL || strstr(channels, marker) == NULL)
	{
		g_autofree gchar *grown = g_strdup_printf("%s%s,", venture_string_is_empty(channels) ? "," : channels, channel);
		g_object_set(row, "channels", grown, NULL);
	}
	if (later(at, last))
		g_object_set(row, "last-message-at", at, NULL);
	if (inbound)
	{
		if (later(at, last_in))
			g_object_set(row, "last-inbound-at", at, "last-inbound-channel", channel, NULL);
		if (status != VENTURE_INBOX_STATUS_OPEN)
			g_object_set(row, "status", VENTURE_INBOX_STATUS_OPEN, "status-changed-at", now, NULL);
	}
	return venture_inbox_service_write(self, row, venture_inbox_service_actor(NULL), error);
}

static void
saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	VentureInboxService *self = data;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	guint i;
	(void)database;
	if (!created || self->database == NULL || !module_on() || VENTURE_IS_INBOX_CONVERSATION(entity))
		return;
	context = context_of(self);
	if (context == NULL || !venture_context_module_enabled(context, "inbox"))
		return;
	sources = venture_inbox_sources();
	for (i = 0; i < sources->len; i++)
	{
		const VentureInboxSource *source = g_ptr_array_index(sources, i);
		g_autoptr(GDateTime) at = NULL;
		g_autoptr(GError) error = NULL;
		gint64 contact_id = 0;
		gboolean inbound = FALSE;
		if (source->observe == NULL || !source->observe(entity, &contact_id, &at, &inbound))
			continue;
		if (contact_id <= 0 || !source_on(context, source))
			break;
		if (at == NULL)
			at = venture_time_now();
		/* The message itself is saved; a conversation that could not be
		 * kept up to date is caught up the next time it is opened. */
		if (!touch(self, venture_entity_get_organization_id(entity), contact_id, source->channel, at, inbound, &error))
			g_message("The inbox could not record a %s message on contact %" G_GINT64_FORMAT ": %s",
				source->channel, contact_id, error != NULL ? error->message : "unknown");
		break;
	}
}

void
venture_inbox_install(VentureContext *context)
{
	VentureDatabase *database;
	VentureInboxService *self;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	database = venture_context_get_database(context);
	self = venture_inbox_service_get(database);
	g_weak_ref_set(&self->context, context);
	if (self->installed)
		return;
	self->installed = TRUE;
	venture_database_add_save_validator(database, VENTURE_TYPE_INBOX_CONVERSATION, validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_INBOX_CHAT_WIDGET, validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_INBOX_CHAT_SESSION, validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_INBOX_CHAT_MESSAGE, validate, self, NULL);
	g_signal_connect_object(database, "entity-saved", G_CALLBACK(saved), self, 0);
}

/* --- The thread ------------------------------------------------------------------ */

static gint
by_time(gconstpointer a, gconstpointer b)
{
	const VentureInboxItem *x = *(const VentureInboxItem *const *)a, *y = *(const VentureInboxItem *const *)b;
	gint order = g_date_time_compare(x->at, y->at);
	if (order != 0)
		return order;
	order = g_strcmp0(x->source->channel, y->source->channel);
	if (order != 0)
		return order;
	return venture_entity_get_id(x->record) < venture_entity_get_id(y->record) ? -1 :
		venture_entity_get_id(x->record) > venture_entity_get_id(y->record);
}

GPtrArray *
venture_inbox_service_thread(VentureInboxService *self, VentureEntity *contact, GDateTime *since, guint limit, GError **error)
{
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GPtrArray) items = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTACT(contact), NULL);
	context = context_of(self);
	if (self->database == NULL || context == NULL)
	{
		refuse(error, VENTURE_ERROR_CONFIG, "The inbox is not installed");
		return NULL;
	}
	if (limit == 0)
		limit = 200;
	limit = MIN(limit, 1000);
	items = g_ptr_array_new_with_free_func((GDestroyNotify)venture_inbox_item_free);
	sources = venture_inbox_sources();
	for (i = 0; i < sources->len; i++)
	{
		const VentureInboxSource *source = g_ptr_array_index(sources, i);
		if (!source_on(context, source))
			continue;
		if (!source->list(context, contact, since, limit, items, error))
			return NULL;
	}
	/* A source that did not honour the cursor is held to it here. */
	for (i = items->len; i > 0; i--)
	{
		VentureInboxItem *item = g_ptr_array_index(items, i - 1);
		if (item->at == NULL || item->record == NULL || (since != NULL && g_date_time_compare(item->at, since) <= 0))
			g_ptr_array_remove_index(items, i - 1);
	}
	g_ptr_array_sort(items, by_time);
	/* The newest are what a reader needs; the oldest go first. */
	if (items->len > limit)
		g_ptr_array_remove_range(items, 0, items->len - limit);
	return g_steal_pointer(&items);
}

/* --- Conversations ---------------------------------------------------------------- */

VentureEntity *
venture_inbox_service_conversation(VentureInboxService *self, VentureEntity *contact, gboolean create, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GPtrArray) items = NULL;
	gint64 org;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTACT(contact), NULL);
	if (self->database == NULL || !module_on())
	{
		refuse(error, VENTURE_ERROR_CONFIG, "Conversations are switched off");
		return NULL;
	}
	org = venture_entity_get_organization_id(contact);
	row = find_conversation(self, org, venture_entity_get_id(contact), error);
	if (row != NULL || (error != NULL && *error != NULL) || !create)
	{
		if (row == NULL && (error == NULL || *error == NULL))
			refuse(error, VENTURE_ERROR_NOT_FOUND, "There is no conversation with this contact yet");
		return g_steal_pointer(&row);
	}
	/* History from before the inbox: catch the conversation up from the
	 * thread, which is the same answer the observer would have built. */
	items = venture_inbox_service_thread(self, contact, NULL, 1000, error);
	if (items == NULL)
		return NULL;
	if (items->len == 0)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "There is no conversation with this contact yet");
		return NULL;
	}
	for (i = 0; i < items->len; i++)
	{
		VentureInboxItem *item = g_ptr_array_index(items, i);
		/* Old messages make it; only new ones reopen it. */
		if (!touch(self, org, venture_entity_get_id(contact), item->source->channel, item->at, item->inbound && i + 1 == items->len, error))
			return NULL;
	}
	return find_conversation(self, org, venture_entity_get_id(contact), error);
}

GPtrArray *
venture_inbox_service_list(VentureInboxService *self, const VentureInboxFilter *filter, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	g_return_val_if_fail(filter != NULL, NULL);
	if (self->database == NULL || !module_on())
	{
		refuse(error, VENTURE_ERROR_CONFIG, "Conversations are switched off");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_INBOX_CONVERSATION);
	if (filter->organization_id > 0)
		venture_query_set_organization(query, filter->organization_id);
	if (filter->view == VENTURE_INBOX_VIEW_MINE)
		venture_query_add_filter_int(query, "owner-id", VENTURE_FILTER_OP_EQ, MAX(filter->user_id, 1), NULL);
	else if (filter->view == VENTURE_INBOX_VIEW_UNASSIGNED)
		venture_query_add_filter_int(query, "owner-id", VENTURE_FILTER_OP_EQ, 0, NULL);
	if (filter->status >= VENTURE_INBOX_STATUS_OPEN && filter->status <= VENTURE_INBOX_STATUS_CLOSED)
		venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, filter->status, NULL);
	if (!venture_string_is_empty(filter->channel))
	{
		g_autofree gchar *pattern = NULL;
		if (venture_inbox_source_lookup(filter->channel) == NULL)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "There is no such channel");
			return NULL;
		}
		pattern = g_strdup_printf("%%,%s,%%", filter->channel);
		venture_query_add_filter_string(query, "channels", VENTURE_FILTER_OP_LIKE, pattern, NULL);
	}
	venture_query_add_order(query, "last-message-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, filter->limit == 0 ? 100 : MIN(filter->limit, 500));
	return venture_database_find(self->database, query, error);
}

/* Re-read under the caller's authority: a row the caller cannot read is
 * one they cannot change. */
static VentureEntity *
current(VentureInboxService *self, VentureEntity *conversation, GError **error)
{
	VentureEntity *row;
	if (!VENTURE_IS_INBOX_CONVERSATION(conversation) || !venture_entity_is_persisted(conversation))
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Conversation not found");
		return NULL;
	}
	row = venture_database_get(self->database, VENTURE_TYPE_INBOX_CONVERSATION, venture_entity_get_id(conversation), NULL);
	if (row == NULL || venture_entity_is_deleted(row))
	{
		g_clear_object(&row);
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Conversation not found");
	}
	return row;
}

/* Whether a person may own a conversation in @org: an active account
 * that runs the whole install, or an active member of the organization. */
static gboolean
may_own(VentureInboxService *self, gint64 org, gint64 user_id)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
	g_autoptr(VentureEntity) user = venture_database_get(self->database, VENTURE_TYPE_USER, user_id, NULL);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) membership = NULL;
	gboolean active = FALSE;
	gint role = VENTURE_USER_ROLE_VIEWER;
	if (user == NULL || venture_entity_is_deleted(user))
		return FALSE;
	g_object_get(user, "active", &active, "role", &role, NULL);
	if (!active || role == VENTURE_USER_ROLE_SERVICE)
		return FALSE;
	if (role == VENTURE_USER_ROLE_OWNER || role == VENTURE_USER_ROLE_ADMIN)
		return TRUE;
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, user_id, NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	membership = venture_database_find_one(self->database, query, NULL);
	return membership != NULL;
}

gboolean
venture_inbox_service_assign(VentureInboxService *self, VentureEntity *conversation, gint64 owner_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), FALSE);
	row = current(self, conversation, error);
	if (row == NULL)
		return FALSE;
	if (owner_id < 0 || (owner_id > 0 && !may_own(self, venture_entity_get_organization_id(row), owner_id)))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Only an active person in this organization can own its conversations");
	if (number(row, "owner-id") == owner_id)
		return TRUE;
	g_object_set(row, "owner-id", owner_id, NULL);
	return venture_inbox_service_write(self, row, actor, error);
}

gboolean
venture_inbox_service_set_status(VentureInboxService *self, VentureEntity *conversation, VentureInboxStatus status,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	gint was = VENTURE_INBOX_STATUS_OPEN;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), FALSE);
	if ((gint)status < VENTURE_INBOX_STATUS_OPEN || status > VENTURE_INBOX_STATUS_CLOSED)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A conversation is open, waiting or closed");
	row = current(self, conversation, error);
	if (row == NULL)
		return FALSE;
	g_object_get(row, "status", &was, NULL);
	if (was == (gint)status)
		return TRUE;
	g_object_set(row, "status", status, "status-changed-at", now, NULL);
	return venture_inbox_service_write(self, row, actor, error);
}

/* --- Replies ---------------------------------------------------------------------- */

const VentureInboxSource *
venture_inbox_service_reply_channel(VentureInboxService *self, VentureEntity *contact, const gchar *channel,
	VentureInboxItem **answering, GError **error)
{
	g_autoptr(GPtrArray) items = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	if (channel != NULL && *channel != '\0' && venture_inbox_source_lookup(channel) == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "There is no such channel");
		return NULL;
	}
	items = venture_inbox_service_thread(self, contact, NULL, 1000, error);
	if (items == NULL)
		return NULL;
	for (i = items->len; i > 0; i--)
	{
		VentureInboxItem *item = g_ptr_array_index(items, i - 1);
		if (!item->inbound || (channel != NULL && *channel != '\0' && g_strcmp0(item->source->channel, channel) != 0))
			continue;
		if (answering != NULL)
			*answering = venture_inbox_item_new(item->source, item->record, item->at, item->inbound);
		return item->source;
	}
	if (channel != NULL && *channel != '\0')
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Nothing has come from them by %s, so there is nothing to answer there", venture_inbox_source_lookup(channel)->label);
	else
		refuse(error, VENTURE_ERROR_VALIDATION, "Nothing has come from them yet, so there is nothing to answer");
	return NULL;
}

gchar *
venture_inbox_service_reply(VentureInboxService *self, VentureEntity *conversation, const gchar *channel,
	const gchar *body, GDateTime *now, const VentureActor *actor, gchar **used, GError **error)
{
	g_autoptr(VentureEntity) row = NULL, contact = NULL;
	g_autoptr(VentureInboxItem) answering = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autofree gchar *trimmed = NULL;
	const VentureInboxSource *source;
	gchar *outcome;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	trimmed = g_strstrip(g_strdup(body != NULL ? body : ""));
	if (*trimmed == '\0' || !g_utf8_validate(trimmed, -1, NULL))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Write the reply before sending it");
		return NULL;
	}
	row = current(self, conversation, error);
	if (row == NULL)
		return NULL;
	contact = venture_database_get(self->database, VENTURE_TYPE_CONTACT, number(row, "contact-id"), NULL);
	if (contact == NULL || venture_entity_is_deleted(contact))
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "This conversation's contact is gone");
		return NULL;
	}
	source = venture_inbox_service_reply_channel(self, contact, channel, &answering, error);
	if (source == NULL)
		return NULL;
	if (source->reply == NULL)
	{
		g_autofree gchar *label = g_ascii_strdown(source->label, -1);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Their latest message was a %s, which is answered by calling back; or choose another channel to reply in", label);
		return NULL;
	}
	context = context_of(self);
	outcome = source->reply(context, contact, answering, trimmed, now, actor, error);
	if (outcome != NULL && used != NULL)
		*used = g_strdup(source->channel);
	return outcome;
}
