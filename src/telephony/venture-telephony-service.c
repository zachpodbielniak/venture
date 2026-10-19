/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
struct _VentureTelephonyService { GObject parent_instance; VentureDatabase *database; };
G_DEFINE_FINAL_TYPE(VentureTelephonyService, venture_telephony_service, G_TYPE_OBJECT)
static void venture_telephony_service_class_init(VentureTelephonyServiceClass *klass) { (void)klass; }
static void venture_telephony_service_init(VentureTelephonyService *self) { (void)self; }
static gboolean stub(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Telephony is not implemented yet");
	return FALSE;
}
VentureTelephonyService *venture_telephony_service_get(VentureDatabase *database)
{
	VentureTelephonyService *self = g_object_get_data(G_OBJECT(database), "venture-telephony-service");
	if (!self) {
		self = g_object_new(VENTURE_TYPE_TELEPHONY_SERVICE, NULL);
		self->database = database;
		g_object_set_data_full(G_OBJECT(database), "venture-telephony-service", self, g_object_unref);
	}
	return self;
}
void venture_telephony_install(VentureContext *context) { (void)context; }
VentureEntity *venture_telephony_service_connect(VentureTelephonyService *self, gint64 organization_id,
	const gchar *provider, JsonObject *settings, gint64 account_id, const VentureActor *actor,
	gchar **webhook_token, GError **error)
{ (void)self; (void)organization_id; (void)provider; (void)settings; (void)account_id; (void)actor; (void)webhook_token; stub(error); return NULL; }
gchar *venture_telephony_service_new_webhook_token(VentureTelephonyService *self, VentureEntity *account,
	const VentureActor *actor, GError **error)
{ (void)self; (void)account; (void)actor; stub(error); return NULL; }
gboolean venture_telephony_service_disconnect(VentureTelephonyService *self, VentureEntity *account,
	const VentureActor *actor, GError **error)
{ (void)self; (void)account; (void)actor; return stub(error); }
VentureEntity *venture_telephony_service_line(VentureTelephonyService *self, gint64 organization_id,
	VentureTelephonyCapabilities needs, GError **error)
{ (void)self; (void)organization_id; (void)needs; stub(error); return NULL; }
gchar *venture_telephony_service_extension(VentureTelephonyService *self, gint64 organization_id, gint64 user_id)
{ (void)self; (void)organization_id; (void)user_id; return NULL; }
gchar *venture_telephony_service_click_to_call(VentureTelephonyService *self, VentureEntity *subject,
	gint64 user_id, const gchar *number, const VentureActor *actor, GError **error)
{ (void)self; (void)subject; (void)user_id; (void)number; (void)actor; stub(error); return NULL; }
VentureEntity *venture_telephony_service_match_contact(VentureTelephonyService *self, gint64 organization_id,
	const gchar *number, GError **error)
{ (void)self; (void)organization_id; (void)number; (void)error; return NULL; }
JsonNode *venture_telephony_service_receive(VentureTelephonyService *self, const gchar *token,
	const VentureTelephonyWebhook *hook, GDateTime *now, gchar **reply, GError **error)
{ (void)self; (void)token; (void)hook; (void)now; (void)reply; stub(error); return NULL; }
VentureEntity *venture_telephony_service_queue_text(VentureTelephonyService *self, VentureEntity *contact,
	const gchar *body, const gchar *const *media, const gchar *origin, GDateTime *now,
	const VentureActor *actor, GError **error)
{ (void)self; (void)contact; (void)body; (void)media; (void)origin; (void)now; (void)actor; stub(error); return NULL; }
VentureEntity *venture_telephony_service_send_text(VentureTelephonyService *self, VentureEntity *contact,
	const gchar *body, gint64 hosted_image_id, GDateTime *now, const VentureActor *actor, GError **error)
{ (void)self; (void)contact; (void)body; (void)hosted_image_id; (void)now; (void)actor; stub(error); return NULL; }
gint venture_telephony_service_deliver_due(VentureTelephonyService *self, gint64 organization_id,
	GDateTime *now, guint limit, const VentureActor *actor, GError **error)
{ (void)self; (void)organization_id; (void)now; (void)limit; (void)actor; stub(error); return -1; }
GDateTime *venture_telephony_service_quiet_until(VentureTelephonyService *self, VentureEntity *contact,
	GDateTime *now, GError **error)
{ (void)self; (void)contact; (void)now; (void)error; return NULL; }
gboolean venture_telephony_service_record_consent(VentureTelephonyService *self, VentureEntity *contact,
	gboolean granted, const gchar *evidence, const gchar *key, GDateTime *at, const VentureActor *actor, GError **error)
{ (void)self; (void)contact; (void)granted; (void)evidence; (void)key; (void)at; (void)actor; return stub(error); }
gboolean venture_telephony_service_has_consent(VentureTelephonyService *self, VentureEntity *contact, GError **error)
{ (void)self; (void)contact; (void)error; return FALSE; }
GPtrArray *venture_telephony_service_thread(VentureTelephonyService *self, VentureEntity *contact,
	guint limit, GError **error)
{ (void)self; (void)contact; (void)limit; stub(error); return NULL; }
