/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Stubs: the tests build against these and fail until the service is written. */
#include <venture.h>

struct _VentureAiMessages {
	GObject parent_instance;
	VentureDatabase *database;
};
G_DEFINE_FINAL_TYPE(VentureAiMessages, venture_ai_messages, G_TYPE_OBJECT)
static void venture_ai_messages_class_init(VentureAiMessagesClass *klass) { (void)klass; }
static void venture_ai_messages_init(VentureAiMessages *self) { (void)self; }

static gboolean
todo(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Not written yet");
	return FALSE;
}

VentureAiMessages *
venture_ai_messages_get(VentureDatabase *database)
{
	VentureAiMessages *self = g_object_get_data(G_OBJECT(database), "venture-ai-messages");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_AI_MESSAGES, NULL);
		self->database = database;
		g_object_set_data_full(G_OBJECT(database), "venture-ai-messages", self, g_object_unref);
	}
	return self;
}
void venture_ai_messages_install(VentureContext *context) { (void)context; }
gboolean venture_ai_messages_status(VentureAiMessages *self, gint64 organization_id, gboolean *drafting, gchar **why)
{ (void)self; (void)organization_id; if (drafting) *drafting = FALSE; if (why) *why = NULL; return FALSE; }
gboolean venture_ai_messages_set_off(VentureAiMessages *self, gint64 organization_id, gboolean off, const VentureActor *actor, GError **error)
{ (void)self; (void)organization_id; (void)off; (void)actor; return todo(error); }
gboolean venture_ai_messages_set_business_hours(VentureAiMessages *self, gint64 organization_id, const gchar *hours, const VentureActor *actor, GError **error)
{ (void)self; (void)organization_id; (void)hours; (void)actor; return todo(error); }
gboolean venture_ai_messages_business_hours_open(const gchar *hours, GTimeZone *zone, GDateTime *at, gboolean *open, GError **error)
{ (void)hours; (void)zone; (void)at; *open = FALSE; return todo(error); }
gboolean venture_ai_messages_save_rule(VentureAiMessages *self, VentureEntity *rule, const VentureActor *actor, GError **error)
{ (void)self; (void)rule; (void)actor; return todo(error); }
gboolean venture_ai_messages_save_route(VentureAiMessages *self, VentureEntity *route, const VentureActor *actor, GError **error)
{ (void)self; (void)route; (void)actor; return todo(error); }
gboolean venture_ai_messages_remove(VentureAiMessages *self, VentureEntity *row, const VentureActor *actor, GError **error)
{ (void)self; (void)row; (void)actor; return todo(error); }
GPtrArray *venture_ai_messages_inbound(VentureAiMessages *self, VentureEntity *record, GDateTime *now, GError **error)
{ (void)self; (void)record; (void)now; todo(error); return NULL; }
VentureEntity *venture_ai_messages_draft_reply(VentureAiMessages *self, VentureEntity *conversation, GDateTime *now, const VentureActor *actor, GError **error)
{ (void)self; (void)conversation; (void)now; (void)actor; todo(error); return NULL; }
VentureEntity *venture_ai_messages_follow_up(VentureAiMessages *self, VentureEntity *subject, const gchar *channel, const gchar *instruction, GDateTime *now, const VentureActor *actor, GError **error)
{ (void)self; (void)subject; (void)channel; (void)instruction; (void)now; (void)actor; todo(error); return NULL; }
VentureEntity *venture_ai_messages_approve(VentureAiMessages *self, VentureEntity *message, const gchar *body, GDateTime *now, const VentureActor *approver, GError **error)
{ (void)self; (void)message; (void)body; (void)now; (void)approver; todo(error); return NULL; }
gboolean venture_ai_messages_discard(VentureAiMessages *self, VentureEntity *message, const VentureActor *actor, GError **error)
{ (void)self; (void)message; (void)actor; return todo(error); }
GPtrArray *venture_ai_messages_for_contact(VentureAiMessages *self, VentureEntity *contact, GError **error)
{ (void)self; (void)contact; return g_ptr_array_new_with_free_func(g_object_unref); (void)error; }
