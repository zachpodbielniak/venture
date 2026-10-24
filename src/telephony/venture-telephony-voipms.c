/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-telephony-voipms.c - VoIP.ms texts and pictures
 *
 * Sending is VoIP.ms's REST API: one form POST to /api/v1/rest.php with
 * the API user name and password in the body (never the address, where a
 * proxy would log them), method sendSMS or sendMMS, the line's number as
 * did and the recipient as dst, both ten digits. A text longer than one
 * SMS, or with a picture, goes as MMS.
 *
 * Receiving is the number's SMS URL callback: VoIP.ms calls this
 * organization's webhook address with the placeholders filled in and
 * expects "ok" back. It does not sign the request, so the long address
 * token and the allowed-sender list stand in for a signature. The REST
 * API does not report delivery, so a VoIP.ms text ends as sent.
 *
 * Field names are from VoIP.ms's public API documentation and are listed
 * in docs/telephony.org as needing confirmation against a live account.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>

#define VENTURE_TYPE_TELEPHONY_VOIPMS (venture_telephony_voipms_get_type())
G_DECLARE_FINAL_TYPE(VentureTelephonyVoipms, venture_telephony_voipms, VENTURE, TELEPHONY_VOIPMS, GObject)
struct _VentureTelephonyVoipms { GObject parent_instance; };
static void connector_init(VentureTelephonyConnectorInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureTelephonyVoipms, venture_telephony_voipms, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_TELEPHONY_CONNECTOR, connector_init))
static void venture_telephony_voipms_class_init(VentureTelephonyVoipmsClass *klass) { (void)klass; }
static void venture_telephony_voipms_init(VentureTelephonyVoipms *self) { (void)self; }

#define LABEL "VoIP.ms"
#define SMS_CHARACTERS 160
#define MMS_PICTURES 3

/* VoIP.ms answers HTTP 200 and says what went wrong in "status". */
static const struct { const gchar *status, *words; VentureError code; } refusals[] = {
	{ "invalid_credentials", "VoIP.ms refused the API user name or password; check them in Telephony settings", VENTURE_ERROR_PERMISSION_DENIED },
	{ "ip_not_enabled", "VoIP.ms refused this server's address; allow it in the VoIP.ms API settings", VENTURE_ERROR_PERMISSION_DENIED },
	{ "no_api", "API access is not enabled on this VoIP.ms account", VENTURE_ERROR_PERMISSION_DENIED },
	{ "invalid_dst", "The recipient is not a valid number for texting", VENTURE_ERROR_VALIDATION },
	{ "invalid_did", "The line's number is not a VoIP.ms number with texting turned on", VENTURE_ERROR_VALIDATION },
	{ "sms_toolong", "The text is too long for VoIP.ms", VENTURE_ERROR_VALIDATION },
	{ "mms_toolong", "The text is too long for VoIP.ms", VENTURE_ERROR_VALIDATION },
	{ "invalid_media", "VoIP.ms could not use the picture", VENTURE_ERROR_VALIDATION },
	{ "limit_reached", "VoIP.ms's sending limit was reached; try again later", VENTURE_ERROR_FAILED }
};

/* The callback's placeholders, in one table: the query name the callback
 * address uses for each, and what Venture reads it as. */
static const struct { const gchar *name, *meaning; } callback_fields[] = {
	{ "id", "id" }, { "from", "from" }, { "to", "to" }, { "message", "body" }, { "media", "media" }
};

static const gchar *get_id(VentureTelephonyConnector *self) { (void)self; return "voipms"; }
static const gchar *get_label(VentureTelephonyConnector *self) { (void)self; return LABEL; }
static const gchar *default_base_url(VentureTelephonyConnector *self) { (void)self; return "https://voip.ms"; }
static const gchar *webhook_reply(VentureTelephonyConnector *self) { (void)self; return "ok"; }

static VentureTelephonyCapabilities
get_capabilities(VentureTelephonyConnector *self)
{
	(void)self;
	return VENTURE_TELEPHONY_CAPABILITY_SMS | VENTURE_TELEPHONY_CAPABILITY_MMS;
}

static JsonNode *
settings_schema(VentureTelephonyConnector *self)
{
	(void)self;
	return json_from_string("{\"type\":\"object\",\"required\":[\"api_username\",\"api_password\"],\"properties\":{"
		"\"api_username\":{\"type\":\"string\",\"title\":\"API user name (your VoIP.ms sign-in email)\"},"
		"\"api_password\":{\"type\":\"string\",\"title\":\"API password\",\"x-sensitive\":true}}}", NULL);
}

static gchar *
account_identity(VentureTelephonyConnector *self, JsonObject *settings, GError **error)
{
	const gchar *user = venture_json_object_get_string(settings, "api_username", NULL);
	(void)self;
	if (venture_string_is_empty(user) || venture_string_is_empty(venture_json_object_get_string(settings, "api_password", NULL)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VoIP.ms needs the API user name and API password");
		return NULL;
	}
	return g_strdup(user);
}

/* VoIP.ms numbers are North American and written as ten digits. */
static gchar *
ten_digits(const gchar *e164, const gchar *which, GError **error)
{
	if (e164 == NULL || !g_str_has_prefix(e164, "+1") || strlen(e164) != 12)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VoIP.ms texts only North American numbers, and %s is not one", which);
		return NULL;
	}
	return g_strdup(e164 + 2);
}

static gboolean
send_message(VentureTelephonyConnector *self, VentureTelephonyLink *link, const VentureTelephonyOutbound *message,
	gchar **provider_id, GError **error)
{
	g_autoptr(GHashTable) form = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *did = ten_digits(message->from, "the line's number", error), *dst = NULL, *encoded = NULL, *text = NULL;
	g_autoptr(GBytes) body = NULL, answer = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	gboolean mms;
	const gchar *status;
	JsonObject *object;
	guint code = 0, i, pictures = 0;
	(void)self;
	if (did == NULL)
		return FALSE;
	dst = ten_digits(message->to, "the recipient's", error);
	if (dst == NULL)
		return FALSE;
	while (message->media != NULL && message->media[pictures] != NULL)
		pictures++;
	if (pictures > MMS_PICTURES)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VoIP.ms sends at most %d pictures in one text", MMS_PICTURES);
		return FALSE;
	}
	mms = pictures > 0 || g_utf8_strlen(message->body, -1) > SMS_CHARACTERS;
	g_hash_table_insert(form, g_strdup("api_username"), (gpointer)venture_telephony_link_get_setting(link, "api_username"));
	g_hash_table_insert(form, g_strdup("api_password"), (gpointer)venture_telephony_link_get_setting(link, "api_password"));
	g_hash_table_insert(form, g_strdup("method"), (gpointer)(mms ? "sendMMS" : "sendSMS"));
	g_hash_table_insert(form, g_strdup("did"), did);
	g_hash_table_insert(form, g_strdup("dst"), dst);
	g_hash_table_insert(form, g_strdup("message"), (gpointer)message->body);
	for (i = 0; i < pictures; i++)
		g_hash_table_insert(form, g_strdup_printf("media%u", i + 1), (gpointer)message->media[i]);
	encoded = soup_form_encode_hash(form);
	body = g_bytes_new(encoded, strlen(encoded));
	memset(encoded, 0, strlen(encoded));
	if (!venture_telephony_link_request(link, "POST", "/api/v1/rest.php", NULL, "application/x-www-form-urlencoded",
		body, &code, &answer, error) || !venture_telephony_http_status_error(code, LABEL, error))
		return FALSE;
	/* Soup's answer is bytes, not a C string. json_from_string() would read
	 * past the end, and a refusal would sometimes be stored as unreadable. */
	text = g_strndup(g_bytes_get_data(answer, NULL), g_bytes_get_size(answer));
	parsed = json_from_string(text, NULL);
	object = parsed != NULL && JSON_NODE_HOLDS_OBJECT(parsed) ? json_node_get_object(parsed) : NULL;
	status = object != NULL ? venture_json_object_get_string(object, "status", NULL) : NULL;
	if (g_strcmp0(status, "success") != 0)
	{
		gsize j;
		for (j = 0; j < G_N_ELEMENTS(refusals); j++)
			if (g_strcmp0(refusals[j].status, status) == 0)
			{
				g_set_error_literal(error, VENTURE_ERROR, refusals[j].code, refusals[j].words);
				return FALSE;
			}
		/* An unknown status is named only if it looks like one. */
		if (status != NULL && strlen(status) < 40 && strspn(status, "abcdefghijklmnopqrstuvwxyz_") == strlen(status))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "VoIP.ms refused the text (%s)", status);
		else
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "VoIP.ms gave an answer Venture could not read");
		return FALSE;
	}
	{
		JsonNode *id = json_object_get_member(object, mms ? "mms" : "sms");
		if (id != NULL && JSON_NODE_HOLDS_VALUE(id))
			*provider_id = json_node_get_value_type(id) == G_TYPE_STRING ? g_strdup(json_node_get_string(id))
				: g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(id));
		else
			*provider_id = g_strdup("");
	}
	return TRUE;
}

static const gchar *
field(GHashTable *values, const gchar *meaning)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(callback_fields); i++)
		if (g_str_equal(callback_fields[i].meaning, meaning))
			return g_hash_table_lookup(values, callback_fields[i].name);
	return NULL;
}

static GPtrArray *
parse_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link, const VentureTelephonyWebhook *hook, GError **error)
{
	g_autoptr(GHashTable) values = NULL;
	g_autoptr(VentureTelephonyEvent) event = venture_telephony_event_new(VENTURE_TELEPHONY_EVENT_MESSAGE);
	GPtrArray *events;
	const gchar *media;
	(void)self; (void)link;
	if (hook->query != NULL && g_hash_table_size(hook->query) > 0)
		values = g_hash_table_ref(hook->query);
	else if (hook->body != NULL && g_bytes_get_size(hook->body) > 0)
	{
		g_autofree gchar *text = g_strndup(g_bytes_get_data(hook->body, NULL), g_bytes_get_size(hook->body));
		values = soup_form_decode(text);
	}
	if (values == NULL || venture_string_is_empty(field(values, "id")) || venture_string_is_empty(field(values, "from")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The VoIP.ms callback needs id, from, to and message; see Telephony settings for the callback address");
		return NULL;
	}
	event->inbound = TRUE;
	event->id = g_strdup(field(values, "id"));
	event->from = g_strdup(field(values, "from"));
	event->to = g_strdup(field(values, "to"));
	event->body = g_strdup(field(values, "body") != NULL ? field(values, "body") : "");
	media = field(values, "media");
	if (!venture_string_is_empty(media))
	{
		g_auto(GStrv) urls = g_strsplit(media, ",", MMS_PICTURES + 1);
		guint i;
		event->media = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; urls[i] != NULL && i < MMS_PICTURES; i++)
			if (*g_strstrip(urls[i]) != '\0')
				g_ptr_array_add(event->media, g_strdup(urls[i]));
	}
	events = g_ptr_array_new_with_free_func((GDestroyNotify)venture_telephony_event_free);
	g_ptr_array_add(events, g_steal_pointer(&event));
	return events;
}

static void
connector_init(VentureTelephonyConnectorInterface *iface)
{
	iface->get_id = get_id;
	iface->get_label = get_label;
	iface->get_capabilities = get_capabilities;
	iface->settings_schema = settings_schema;
	iface->account_identity = account_identity;
	iface->default_base_url = default_base_url;
	iface->send_message = send_message;
	iface->parse_webhook = parse_webhook;
	iface->webhook_reply = webhook_reply;
}

VENTURE_TELEPHONY_PROVIDER(venture_telephony_voipms_get_type)
