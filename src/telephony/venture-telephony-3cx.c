/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-telephony-3cx.c - The 3CX phone system
 *
 * Click-to-call goes through 3CX's call control API: a token by OAuth
 * client credentials (POST /connect/token), then POST
 * /callcontrol/{extension}/makecall with the destination. 3CX rings the
 * extension's own phone first and dials the destination once it answers,
 * which is the order a click-to-call needs.
 *
 * Finished and missed calls arrive by the 3CX CRM integration's call
 * journal, configured to post to this organization's webhook address.
 * 3CX does not sign that request, so the address's long token and the
 * allowed-sender list are what keep it honest.
 *
 * Every field name below is from 3CX's public documentation and is listed
 * in docs/telephony.org as needing confirmation against a live system.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>

#define VENTURE_TYPE_TELEPHONY_3CX (venture_telephony_3cx_get_type())
G_DECLARE_FINAL_TYPE(VentureTelephony3cx, venture_telephony_3cx, VENTURE, TELEPHONY_3CX, GObject)
struct _VentureTelephony3cx { GObject parent_instance; };
static void connector_init(VentureTelephonyConnectorInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureTelephony3cx, venture_telephony_3cx, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_TELEPHONY_CONNECTOR, connector_init))
static void venture_telephony_3cx_class_init(VentureTelephony3cxClass *klass) { (void)klass; }
static void venture_telephony_3cx_init(VentureTelephony3cx *self) { (void)self; }

#define LABEL "3CX"

/* The call journal's fields, in one table: what 3CX's CRM template calls
 * each variable, and what Venture reads it as. */
static const struct { const gchar *field, *meaning; } journal_fields[] = {
	{ "CallID", "id" },
	{ "CallType", "type" },
	{ "Number", "number" },
	{ "Agent", "extension" },
	{ "Duration", "duration" },
	{ "CallStartTimeUTC", "started" },
	{ "RecordingUrl", "recording" }
};

/* CallType values: which way, and whether anybody talked. */
static const struct { const gchar *type; gboolean inbound, answered; } call_types[] = {
	{ "Inbound", TRUE, TRUE },
	{ "Outbound", FALSE, TRUE },
	{ "Missed", TRUE, FALSE },
	{ "Notanswered", FALSE, FALSE }
};

static const gchar *get_id(VentureTelephonyConnector *self) { (void)self; return "3cx"; }
static const gchar *get_label(VentureTelephonyConnector *self) { (void)self; return LABEL; }

static VentureTelephonyCapabilities
get_capabilities(VentureTelephonyConnector *self)
{
	(void)self;
	return VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL | VENTURE_TELEPHONY_CAPABILITY_CALL_EVENTS;
}

static JsonNode *
settings_schema(VentureTelephonyConnector *self)
{
	(void)self;
	return json_from_string("{\"type\":\"object\",\"required\":[\"client_id\",\"client_secret\"],\"properties\":{"
		"\"client_id\":{\"type\":\"string\",\"title\":\"Client ID (the 3CX API integration's)\"},"
		"\"client_secret\":{\"type\":\"string\",\"title\":\"API key\",\"x-sensitive\":true}}}", NULL);
}

static gchar *
account_identity(VentureTelephonyConnector *self, JsonObject *settings, GError **error)
{
	const gchar *id = venture_json_object_get_string(settings, "client_id", NULL);
	(void)self;
	if (venture_string_is_empty(id) || venture_string_is_empty(venture_json_object_get_string(settings, "client_secret", NULL)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "3CX needs the API integration's client ID and API key");
		return NULL;
	}
	return g_strdup(id);
}

/* A reason the PBX gave, made safe to show: one line, bounded. */
static gchar *
reason_text(const gchar *text)
{
	gchar *line = g_utf8_make_valid(text != NULL ? text : "", -1), *p;
	for (p = line; *p != '\0'; p++)
		if ((guchar)*p < 0x20) *p = ' ';
	if (g_utf8_strlen(line, -1) > 200)
	{
		gchar *cut = g_utf8_substring(line, 0, 200);
		g_free(line);
		line = cut;
	}
	return line;
}

static gchar *
token(VentureTelephonyLink *link, GError **error)
{
	g_autofree gchar *form = soup_form_encode("grant_type", "client_credentials",
		"client_id", venture_telephony_link_get_setting(link, "client_id"),
		"client_secret", venture_telephony_link_get_setting(link, "client_secret"), NULL);
	g_autoptr(GBytes) body = g_bytes_new(form, strlen(form));
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	g_autofree gchar *text = NULL;
	guint status = 0;
	const gchar *access;
	/* The refusal body is never shown: a PBX may echo what it was sent. */
	if (!venture_telephony_link_request(link, "POST", "/connect/token", NULL,
		"application/x-www-form-urlencoded", body, &status, &answer, error) ||
		!venture_telephony_http_status_error(status == 400 ? 401 : status, LABEL, error))
		return NULL;
	/* Soup's answer is bytes, not a C string. */
	text = g_strndup(g_bytes_get_data(answer, NULL), g_bytes_get_size(answer));
	parsed = json_from_string(text, NULL);
	access = parsed != NULL && JSON_NODE_HOLDS_OBJECT(parsed) ?
		venture_json_object_get_string(json_node_get_object(parsed), "access_token", NULL) : NULL;
	if (venture_string_is_empty(access))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "3CX did not issue a token");
		return NULL;
	}
	return g_strdup(access);
}

static gboolean
click_to_call(VentureTelephonyConnector *self, VentureTelephonyLink *link, const gchar *extension,
	const gchar *destination, gchar **call_id, GError **error)
{
	g_autofree gchar *access = NULL, *authorization = NULL, *path = NULL, *json = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) request = NULL, parsed = NULL;
	g_autoptr(GBytes) body = NULL, answer = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *object, *result;
	guint status = 0;
	const gchar *p;
	(void)self;
	/* The extension goes in the path: digits only, so it cannot be one. */
	for (p = extension; p != NULL && *p != '\0'; p++)
		if (!g_ascii_isdigit(*p)) break;
	if (venture_string_is_empty(extension) || *p != '\0' || strlen(extension) > 16)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A 3CX extension is digits only, for example 101");
		return FALSE;
	}
	access = token(link, error);
	if (access == NULL)
		return FALSE;
	authorization = g_strdup_printf("Bearer %s", access);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "destination");
	json_builder_add_string_value(builder, destination);
	json_builder_end_object(builder);
	request = json_builder_get_root(builder);
	json = venture_json_to_string(request, FALSE);
	body = g_bytes_new(json, strlen(json));
	path = g_strdup_printf("/callcontrol/%s/makecall", extension);
	if (!venture_telephony_link_request(link, "POST", path, authorization, "application/json", body, &status, &answer, error) ||
		!venture_telephony_http_status_error(status, LABEL, error))
		return FALSE;
	text = g_strndup(g_bytes_get_data(answer, NULL), g_bytes_get_size(answer));
	parsed = json_from_string(text, NULL);
	object = parsed != NULL && JSON_NODE_HOLDS_OBJECT(parsed) ? json_node_get_object(parsed) : NULL;
	if (object == NULL || g_strcmp0(venture_json_object_get_string(object, "finalstatus", NULL), "Success") != 0)
	{
		g_autofree gchar *why = reason_text(object != NULL ? venture_json_object_get_string(object, "reasontext",
			venture_json_object_get_string(object, "reason", "no reason given")) : "an unreadable answer");
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "3CX could not place the call: %s", why);
		return FALSE;
	}
	result = json_object_has_member(object, "result") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "result")) ?
		json_object_get_object_member(object, "result") : NULL;
	if (result != NULL && json_object_has_member(result, "callid"))
		*call_id = g_strdup_printf("%" G_GINT64_FORMAT, json_object_get_int_member(result, "callid"));
	else
		*call_id = g_strdup("");
	return TRUE;
}

static const gchar *
journal_value(JsonObject *json, GHashTable *form, const gchar *meaning)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(journal_fields); i++)
		if (g_str_equal(journal_fields[i].meaning, meaning))
		{
			const gchar *name = journal_fields[i].field;
			if (json != NULL && json_object_has_member(json, name) && JSON_NODE_HOLDS_VALUE(json_object_get_member(json, name)))
			{
				JsonNode *node = json_object_get_member(json, name);
				return json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
			}
			return form != NULL ? g_hash_table_lookup(form, name) : NULL;
		}
	return NULL;
}

static gint64
seconds(const gchar *text)
{
	g_auto(GStrv) parts = NULL;
	gint64 total = 0;
	guint i;
	if (venture_string_is_empty(text))
		return 0;
	parts = g_strsplit(text, ":", 3);
	for (i = 0; parts[i] != NULL; i++)
		total = total * 60 + g_ascii_strtoll(parts[i], NULL, 10);
	return CLAMP(total, 0, 86400);
}

static GDateTime *
started(const gchar *text)
{
	g_autofree gchar *iso = NULL;
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	if (venture_string_is_empty(text))
		return NULL;
	iso = g_strdup(text);
	if (strlen(iso) > 10 && iso[10] == ' ')
		iso[10] = 'T';
	return g_date_time_new_from_iso8601(iso, utc);
}

static GPtrArray *
parse_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link, const VentureTelephonyWebhook *hook, GError **error)
{
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GHashTable) form = NULL;
	g_autoptr(VentureTelephonyEvent) event = venture_telephony_event_new(VENTURE_TELEPHONY_EVENT_CALL);
	GPtrArray *events;
	JsonObject *json = NULL;
	const gchar *type, *number, *id;
	gsize i;
	(void)self; (void)link;
	if (hook->body != NULL && g_bytes_get_size(hook->body) > 0)
	{
		g_autofree gchar *text = g_strndup(g_bytes_get_data(hook->body, NULL), g_bytes_get_size(hook->body));
		if (hook->content_type != NULL && strstr(hook->content_type, "json") != NULL)
		{
			root = json_from_string(text, NULL);
			if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The 3CX call journal is not a JSON object");
				return NULL;
			}
			json = json_node_get_object(root);
		}
		else
			form = soup_form_decode(text);
	}
	else if (hook->query != NULL)
		form = g_hash_table_ref(hook->query);
	type = journal_value(json, form, "type");
	number = journal_value(json, form, "number");
	for (i = 0; i < G_N_ELEMENTS(call_types); i++)
		if (g_strcmp0(call_types[i].type, type) == 0)
			break;
	if (i == G_N_ELEMENTS(call_types) || venture_string_is_empty(number))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The 3CX call journal needs CallType (Inbound, Outbound, Missed or Notanswered) and Number");
		return NULL;
	}
	event->inbound = call_types[i].inbound;
	event->answered = call_types[i].answered;
	if (event->inbound) event->from = g_strdup(number);
	else event->to = g_strdup(number);
	event->extension = g_strdup(journal_value(json, form, "extension"));
	event->duration = seconds(journal_value(json, form, "duration"));
	event->occurred_at = started(journal_value(json, form, "started"));
	event->recording_url = g_strdup(journal_value(json, form, "recording"));
	id = journal_value(json, form, "id");
	if (!venture_string_is_empty(id))
		event->id = g_strdup(id);
	else
	{
		/* Without a call id the journal's own facts are the identity. */
		g_autofree gchar *facts = g_strdup_printf("%s|%s|%s|%s", type, number,
			journal_value(json, form, "extension") ? journal_value(json, form, "extension") : "",
			journal_value(json, form, "started") ? journal_value(json, form, "started") : "");
		event->id = g_compute_checksum_for_string(G_CHECKSUM_SHA256, facts, -1);
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
	iface->click_to_call = click_to_call;
	iface->parse_webhook = parse_webhook;
}

VENTURE_TELEPHONY_PROVIDER(venture_telephony_3cx_get_type)
