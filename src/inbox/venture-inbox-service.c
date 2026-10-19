/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-inbox-service.c - Conversations, their status and owner, replies
 * and web chat. Not yet implemented: every call refuses.
 */
#include <venture.h>

struct _VentureInboxService {
	GObject parent_instance;
	VentureDatabase *database;	/* weak */
};
G_DEFINE_FINAL_TYPE(VentureInboxService, venture_inbox_service, G_TYPE_OBJECT)

G_DEFINE_QUARK(venture-inbox-error-quark, venture_inbox_error)

static void venture_inbox_service_class_init(VentureInboxServiceClass *klass) { (void)klass; }
static void venture_inbox_service_init(VentureInboxService *self) { (void)self; }

static gboolean
unbuilt(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "The inbox is not built yet");
	return FALSE;
}

VentureInboxService *
venture_inbox_service_get(VentureDatabase *database)
{
	VentureInboxService *self = g_object_get_data(G_OBJECT(database), "venture-inbox-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_INBOX_SERVICE, NULL);
		self->database = database;
		g_object_set_data_full(G_OBJECT(database), "venture-inbox-service", self, g_object_unref);
	}
	return self;
}

void venture_inbox_install(VentureContext *context) { (void)context; }

GPtrArray *
venture_inbox_service_thread(VentureInboxService *self, VentureEntity *contact, GDateTime *since, guint limit, GError **error)
{
	(void)self; (void)contact; (void)since; (void)limit;
	unbuilt(error);
	return NULL;
}

VentureEntity *
venture_inbox_service_conversation(VentureInboxService *self, VentureEntity *contact, gboolean create, GError **error)
{
	(void)self; (void)contact; (void)create;
	unbuilt(error);
	return NULL;
}

GPtrArray *
venture_inbox_service_list(VentureInboxService *self, const VentureInboxFilter *filter, GError **error)
{
	(void)self; (void)filter;
	unbuilt(error);
	return NULL;
}

gboolean
venture_inbox_service_assign(VentureInboxService *self, VentureEntity *conversation, gint64 owner_id,
	const VentureActor *actor, GError **error)
{
	(void)self; (void)conversation; (void)owner_id; (void)actor;
	return unbuilt(error);
}

gboolean
venture_inbox_service_set_status(VentureInboxService *self, VentureEntity *conversation, VentureInboxStatus status,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	(void)self; (void)conversation; (void)status; (void)now; (void)actor;
	return unbuilt(error);
}

const VentureInboxSource *
venture_inbox_service_reply_channel(VentureInboxService *self, VentureEntity *contact, const gchar *channel,
	VentureInboxItem **answering, GError **error)
{
	(void)self; (void)contact; (void)channel; (void)answering;
	unbuilt(error);
	return NULL;
}

gchar *
venture_inbox_service_reply(VentureInboxService *self, VentureEntity *conversation, const gchar *channel,
	const gchar *body, GDateTime *now, const VentureActor *actor, gchar **used, GError **error)
{
	(void)self; (void)conversation; (void)channel; (void)body; (void)now; (void)actor; (void)used;
	unbuilt(error);
	return NULL;
}

VentureEntity *
venture_inbox_chat_widget_create(VentureInboxService *self, gint64 organization_id, const gchar *name,
	const VentureActor *actor, GError **error)
{
	(void)self; (void)organization_id; (void)name; (void)actor;
	unbuilt(error);
	return NULL;
}

gboolean
venture_inbox_chat_widget_update(VentureInboxService *self, VentureEntity *widget, const gchar *greeting,
	const gchar *allowed_origins, const VentureActor *actor, GError **error)
{
	(void)self; (void)widget; (void)greeting; (void)allowed_origins; (void)actor;
	return unbuilt(error);
}

gboolean
venture_inbox_chat_widget_set_active(VentureInboxService *self, VentureEntity *widget, gboolean active,
	const VentureActor *actor, GError **error)
{
	(void)self; (void)widget; (void)active; (void)actor;
	return unbuilt(error);
}

VentureEntity *
venture_inbox_chat_widget_lookup(VentureInboxService *self, const gchar *token)
{
	(void)self; (void)token;
	return NULL;
}

VentureEntity *
venture_inbox_chat_start(VentureInboxService *self, const gchar *widget_token, const VentureInboxChatIntake *intake,
	GDateTime *now, gchar **visitor_token, GError **error)
{
	(void)self; (void)widget_token; (void)intake; (void)now; (void)visitor_token;
	unbuilt(error);
	return NULL;
}

VentureEntity *
venture_inbox_chat_session_lookup(VentureInboxService *self, const gchar *visitor_token)
{
	(void)self; (void)visitor_token;
	return NULL;
}

VentureEntity *
venture_inbox_chat_post(VentureInboxService *self, VentureEntity *session, const gchar *body, const gchar *remote,
	GDateTime *now, GError **error)
{
	(void)self; (void)session; (void)body; (void)remote; (void)now;
	unbuilt(error);
	return NULL;
}

VentureEntity *
venture_inbox_chat_answer(VentureInboxService *self, VentureEntity *session, const gchar *body, const gchar *author,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	(void)self; (void)session; (void)body; (void)author; (void)now; (void)actor;
	unbuilt(error);
	return NULL;
}

GPtrArray *
venture_inbox_chat_messages(VentureInboxService *self, VentureEntity *session, gint64 after_id, GError **error)
{
	(void)self; (void)session; (void)after_id;
	unbuilt(error);
	return NULL;
}
