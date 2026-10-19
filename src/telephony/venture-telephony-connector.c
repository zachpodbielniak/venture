/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
G_DEFINE_INTERFACE(VentureTelephonyConnector, venture_telephony_connector, G_TYPE_OBJECT)
static void venture_telephony_connector_default_init(VentureTelephonyConnectorInterface *iface) { (void)iface; }
static gboolean stub(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Telephony is not implemented yet");
	return FALSE;
}
VentureTelephonyEvent *venture_telephony_event_new(VentureTelephonyEventKind kind)
{
	VentureTelephonyEvent *event = g_new0(VentureTelephonyEvent, 1);
	event->kind = kind;
	return event;
}
void venture_telephony_event_free(VentureTelephonyEvent *event)
{
	if (event == NULL) return;
	g_free(event->id); g_free(event->from); g_free(event->to); g_free(event->extension);
	g_clear_pointer(&event->occurred_at, g_date_time_unref);
	g_free(event->recording_url); g_free(event->body);
	g_clear_pointer(&event->media, g_ptr_array_unref);
	g_free(event->status); g_free(event->error);
	g_free(event);
}
const gchar *venture_telephony_connector_get_id(VentureTelephonyConnector *self) { (void)self; return NULL; }
const gchar *venture_telephony_connector_get_label(VentureTelephonyConnector *self) { (void)self; return NULL; }
VentureTelephonyCapabilities venture_telephony_connector_get_capabilities(VentureTelephonyConnector *self) { (void)self; return 0; }
JsonNode *venture_telephony_connector_settings_schema(VentureTelephonyConnector *self) { (void)self; return NULL; }
gchar *venture_telephony_connector_account_identity(VentureTelephonyConnector *self, JsonObject *settings, GError **error) { (void)self; (void)settings; stub(error); return NULL; }
const gchar *venture_telephony_connector_default_base_url(VentureTelephonyConnector *self) { (void)self; return NULL; }
gboolean venture_telephony_connector_click_to_call(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const gchar *extension, const gchar *destination, gchar **call_id, GError **error)
{ (void)self; (void)link; (void)extension; (void)destination; (void)call_id; return stub(error); }
gboolean venture_telephony_connector_send_message(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyOutbound *message, gchar **provider_id, GError **error)
{ (void)self; (void)link; (void)message; (void)provider_id; return stub(error); }
gboolean venture_telephony_connector_verify_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error)
{ (void)self; (void)link; (void)hook; return stub(error); }
GPtrArray *venture_telephony_connector_parse_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error)
{ (void)self; (void)link; (void)hook; stub(error); return NULL; }
const gchar *venture_telephony_connector_webhook_reply(VentureTelephonyConnector *self) { (void)self; return NULL; }
void venture_telephony_provider_register(GType (*get_type)(void)) { (void)get_type; }
VentureTelephonyConnector *venture_telephony_provider_new(const gchar *id) { (void)id; return NULL; }
gchar **venture_telephony_provider_ids(void) { return g_new0(gchar *, 1); }
VentureTelephonyLink *venture_telephony_link_new(const gchar *base_url, JsonObject *settings,
	const gchar *number, gboolean allow_plaintext_loopback, GError **error)
{ (void)base_url; (void)settings; (void)number; (void)allow_plaintext_loopback; stub(error); return NULL; }
void venture_telephony_link_free(VentureTelephonyLink *link) { (void)link; }
const gchar *venture_telephony_link_get_setting(VentureTelephonyLink *link, const gchar *name) { (void)link; (void)name; return NULL; }
const gchar *venture_telephony_link_get_number(VentureTelephonyLink *link) { (void)link; return NULL; }
gboolean venture_telephony_link_request(VentureTelephonyLink *link, const gchar *method, const gchar *path,
	const gchar *authorization, const gchar *content_type, GBytes *body, guint *status, GBytes **response, GError **error)
{ (void)link; (void)method; (void)path; (void)authorization; (void)content_type; (void)body; *status = 0; *response = NULL; return stub(error); }
gboolean venture_telephony_http_status_error(guint status, const gchar *provider, GError **error) { (void)status; (void)provider; return stub(error); }
gboolean venture_telephony_signature_matches(const gchar *secret, GBytes *body, const gchar *signature) { (void)secret; (void)body; (void)signature; return FALSE; }
