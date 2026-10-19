/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-telephony-connector.h - The phone provider interface
 *
 * A phone provider (a PBX for calls, a carrier for texts) is one GObject
 * implementing VentureTelephonyConnector, in one file, that registers
 * itself with VENTURE_TELEPHONY_PROVIDER(). Nothing else names it: the
 * settings page, the service and the webhook find it through the registry
 * by its id. A provider builds requests and reads answers; the service
 * owns credentials, consent, quiet hours, logging and idempotency.
 */
#ifndef VENTURE_TELEPHONY_CONNECTOR_H
#define VENTURE_TELEPHONY_CONNECTOR_H
#include <glib-object.h>
#include <json-glib/json-glib.h>
G_BEGIN_DECLS

/**
 * VentureTelephonyCapabilities:
 * @VENTURE_TELEPHONY_CAPABILITY_NONE: nothing
 * @VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL: rings a user's extension, then dials out
 * @VENTURE_TELEPHONY_CAPABILITY_CALL_EVENTS: reports finished and missed calls
 * @VENTURE_TELEPHONY_CAPABILITY_SMS: sends and receives texts
 * @VENTURE_TELEPHONY_CAPABILITY_MMS: sends and receives pictures
 * @VENTURE_TELEPHONY_CAPABILITY_DELIVERY_STATUS: reports whether a text arrived
 * @VENTURE_TELEPHONY_CAPABILITY_SIGNED_WEBHOOK: signs what it posts to the webhook
 *
 * What a provider can do. A provider without a signed webhook relies on
 * the long unguessable address token and the allowed-sender list.
 */
typedef enum {
	VENTURE_TELEPHONY_CAPABILITY_NONE = 0,
	VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL = 1 << 0,
	VENTURE_TELEPHONY_CAPABILITY_CALL_EVENTS = 1 << 1,
	VENTURE_TELEPHONY_CAPABILITY_SMS = 1 << 2,
	VENTURE_TELEPHONY_CAPABILITY_MMS = 1 << 3,
	VENTURE_TELEPHONY_CAPABILITY_DELIVERY_STATUS = 1 << 4,
	VENTURE_TELEPHONY_CAPABILITY_SIGNED_WEBHOOK = 1 << 5
} VentureTelephonyCapabilities;

/**
 * VENTURE_TELEPHONY_TIMEOUT_SECONDS:
 *
 * How long one provider request may take. There are no retries: a failed
 * click-to-call is the user's to repeat, and a failed text stays failed
 * with its reason rather than risking a double send.
 */
#define VENTURE_TELEPHONY_TIMEOUT_SECONDS 15
/**
 * VENTURE_TELEPHONY_MAX_RESPONSE:
 *
 * The largest provider answer or picture read, in bytes.
 */
#define VENTURE_TELEPHONY_MAX_RESPONSE (5 * 1024 * 1024)

/**
 * VentureTelephonyLink:
 *
 * One provider account's address and decrypted settings, for the length of
 * one operation. Opaque; never logged, never shown.
 */
typedef struct _VentureTelephonyLink VentureTelephonyLink;

/**
 * VentureTelephonyWebhook:
 * @method: GET or POST
 * @remote_address: (nullable): the peer that connected
 * @query: (nullable): decoded query string
 * @headers: (nullable): request headers, names in lower case
 * @content_type: (nullable): the body's type
 * @body: (nullable): the exact body bytes
 *
 * One inbound webhook request, as the provider sent it.
 */
typedef struct {
	const gchar *method;
	const gchar *remote_address;
	GHashTable *query;
	GHashTable *headers;
	const gchar *content_type;
	GBytes *body;
} VentureTelephonyWebhook;

/**
 * VentureTelephonyEventKind:
 * @VENTURE_TELEPHONY_EVENT_CALL: a call ended or was missed
 * @VENTURE_TELEPHONY_EVENT_MESSAGE: a text arrived
 * @VENTURE_TELEPHONY_EVENT_STATUS: a sent text was delivered or failed
 *
 * What a webhook reported.
 */
typedef enum {
	VENTURE_TELEPHONY_EVENT_CALL,
	VENTURE_TELEPHONY_EVENT_MESSAGE,
	VENTURE_TELEPHONY_EVENT_STATUS
} VentureTelephonyEventKind;

/**
 * VentureTelephonyEvent:
 * @kind: what happened
 * @id: the provider's call or message id, the replay identity
 * @from: (nullable): the calling or sending number, as the provider wrote it
 * @to: (nullable): the called or receiving number
 * @extension: (nullable): the extension that took or made the call
 * @inbound: whether the call or text came in
 * @answered: whether a call was answered
 * @voicemail: whether a call went to voicemail
 * @duration: seconds talked
 * @occurred_at: (nullable): when it started
 * @recording_url: (nullable): the provider's recording page
 * @body: (nullable): a text's words
 * @media: (nullable) (element-type utf8): picture addresses
 * @status: (nullable): delivered or failed, for a status event
 * @error: (nullable): the provider's reason for a failure
 *
 * One fact a provider reported, read into Venture's terms.
 */
typedef struct {
	VentureTelephonyEventKind kind;
	gchar *id;
	gchar *from;
	gchar *to;
	gchar *extension;
	gboolean inbound;
	gboolean answered;
	gboolean voicemail;
	gint64 duration;
	GDateTime *occurred_at;
	gchar *recording_url;
	gchar *body;
	GPtrArray *media;
	gchar *status;
	gchar *error;
} VentureTelephonyEvent;
/**
 * venture_telephony_event_new:
 * @kind: what happened
 * Returns: (transfer full): an empty event
 */
VentureTelephonyEvent *venture_telephony_event_new(VentureTelephonyEventKind kind);
/**
 * venture_telephony_event_free:
 * @event: (nullable): an event
 */
void venture_telephony_event_free(VentureTelephonyEvent *event);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureTelephonyEvent, venture_telephony_event_free)

/**
 * VentureTelephonyOutbound:
 * @from: the line's number, E.164
 * @to: the recipient, E.164
 * @body: the words
 * @media: (nullable) (array zero-terminated=1): public picture addresses
 *
 * One text to send.
 */
typedef struct {
	const gchar *from;
	const gchar *to;
	const gchar *body;
	const gchar *const *media;
} VentureTelephonyOutbound;

#define VENTURE_TYPE_TELEPHONY_CONNECTOR (venture_telephony_connector_get_type())
G_DECLARE_INTERFACE(VentureTelephonyConnector, venture_telephony_connector, VENTURE, TELEPHONY_CONNECTOR, GObject)

/**
 * VentureTelephonyConnectorInterface:
 * @parent_iface: the parent
 * @get_id: the stable provider id, for example "3cx"
 * @get_label: the name people see
 * @get_capabilities: what it can do
 * @settings_schema: the credential fields, as a JSON schema object whose
 *   properties carry "title" and "x-sensitive"
 * @account_identity: the safe account name from the settings
 * @default_base_url: (nullable): the provider's address when the account names none
 * @click_to_call: ring @extension, then dial @destination
 * @send_message: send one text; set the provider's message id
 * @verify_webhook: check a signature, when the provider signs
 * @parse_webhook: read the request into #VentureTelephonyEvent values
 * @webhook_reply: (nullable): the body the provider expects back
 *
 * A phone provider.
 */
struct _VentureTelephonyConnectorInterface {
	GTypeInterface parent_iface;
	const gchar *(*get_id)(VentureTelephonyConnector *self);
	const gchar *(*get_label)(VentureTelephonyConnector *self);
	VentureTelephonyCapabilities (*get_capabilities)(VentureTelephonyConnector *self);
	JsonNode *(*settings_schema)(VentureTelephonyConnector *self);
	gchar *(*account_identity)(VentureTelephonyConnector *self, JsonObject *settings, GError **error);
	const gchar *(*default_base_url)(VentureTelephonyConnector *self);
	gboolean (*click_to_call)(VentureTelephonyConnector *self, VentureTelephonyLink *link,
		const gchar *extension, const gchar *destination, gchar **call_id, GError **error);
	gboolean (*send_message)(VentureTelephonyConnector *self, VentureTelephonyLink *link,
		const VentureTelephonyOutbound *message, gchar **provider_id, GError **error);
	gboolean (*verify_webhook)(VentureTelephonyConnector *self, VentureTelephonyLink *link,
		const VentureTelephonyWebhook *hook, GError **error);
	GPtrArray *(*parse_webhook)(VentureTelephonyConnector *self, VentureTelephonyLink *link,
		const VentureTelephonyWebhook *hook, GError **error);
	const gchar *(*webhook_reply)(VentureTelephonyConnector *self);
};

/**
 * venture_telephony_connector_get_id:
 * @self: a provider
 * Returns: its stable id
 */
const gchar *venture_telephony_connector_get_id(VentureTelephonyConnector *self);
/**
 * venture_telephony_connector_get_label:
 * @self: a provider
 * Returns: its name
 */
const gchar *venture_telephony_connector_get_label(VentureTelephonyConnector *self);
/**
 * venture_telephony_connector_get_capabilities:
 * @self: a provider
 * Returns: what it can do
 */
VentureTelephonyCapabilities venture_telephony_connector_get_capabilities(VentureTelephonyConnector *self);
/**
 * venture_telephony_connector_settings_schema:
 * @self: a provider
 * Returns: (transfer full): its credential fields
 */
JsonNode *venture_telephony_connector_settings_schema(VentureTelephonyConnector *self);
/**
 * venture_telephony_connector_account_identity:
 * @self: a provider
 * @settings: the settings as entered
 * @error: (out) (optional): a required field is missing
 * Returns: (transfer full) (nullable): the account name, never a secret
 */
gchar *venture_telephony_connector_account_identity(VentureTelephonyConnector *self, JsonObject *settings, GError **error);
/**
 * venture_telephony_connector_default_base_url:
 * @self: a provider
 * Returns: (nullable): its public address, or %NULL when every account has its own
 */
const gchar *venture_telephony_connector_default_base_url(VentureTelephonyConnector *self);
/**
 * venture_telephony_connector_click_to_call:
 * @self: a provider
 * @link: the account
 * @extension: the user's extension, rung first
 * @destination: the number dialled once it answers, E.164
 * @call_id: (out) (optional) (transfer full): the provider's call id
 * @error: (out) (optional): refusal in plain words
 * Returns: whether the provider accepted the call
 */
gboolean venture_telephony_connector_click_to_call(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const gchar *extension, const gchar *destination, gchar **call_id, GError **error);
/**
 * venture_telephony_connector_send_message:
 * @self: a provider
 * @link: the account
 * @message: the text
 * @provider_id: (out) (optional) (transfer full): the provider's message id
 * @error: (out) (optional): refusal in plain words
 * Returns: whether the provider accepted it
 */
gboolean venture_telephony_connector_send_message(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyOutbound *message, gchar **provider_id, GError **error);
/**
 * venture_telephony_connector_verify_webhook:
 * @self: a provider
 * @link: the account the address token named
 * @hook: the request
 * @error: (out) (optional): why it is refused
 * Returns: whether the request is the provider's; TRUE for a provider that does not sign
 */
gboolean venture_telephony_connector_verify_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error);
/**
 * venture_telephony_connector_parse_webhook:
 * @self: a provider
 * @link: the account
 * @hook: the request
 * @error: (out) (optional): unreadable
 * Returns: (transfer full) (element-type VentureTelephonyEvent) (nullable): the events
 */
GPtrArray *venture_telephony_connector_parse_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error);
/**
 * venture_telephony_connector_webhook_reply:
 * @self: a provider
 * Returns: (nullable): the body it expects from the webhook, or %NULL for JSON
 */
const gchar *venture_telephony_connector_webhook_reply(VentureTelephonyConnector *self);

/**
 * venture_telephony_provider_register:
 * @get_type: the provider's get_type function
 *
 * Adds a provider to the process-wide registry. Called by
 * VENTURE_TELEPHONY_PROVIDER() before main(); a second registration of the
 * same type is ignored.
 */
void venture_telephony_provider_register(GType (*get_type)(void));
/**
 * venture_telephony_provider_new:
 * @id: (nullable): a provider id
 * Returns: (transfer full) (nullable): a new instance, or %NULL when none has that id
 */
VentureTelephonyConnector *venture_telephony_provider_new(const gchar *id);
/**
 * venture_telephony_provider_ids:
 * Returns: (transfer full): every registered provider id, sorted
 */
gchar **venture_telephony_provider_ids(void);

/**
 * VENTURE_TELEPHONY_PROVIDER:
 * @get_type_func: the provider type's get_type function
 *
 * Put once at the end of a provider's file. The server and the tests link
 * the whole library, so the constructor runs in every binary that has the
 * provider; no list anywhere names it.
 */
#define VENTURE_TELEPHONY_PROVIDER(get_type_func) \
	static void get_type_func##_register(void) __attribute__((constructor)); \
	static void get_type_func##_register(void) { venture_telephony_provider_register(get_type_func); }

/**
 * venture_telephony_link_new:
 * @base_url: the provider's origin; HTTPS, or loopback HTTP when allowed
 * @settings: (nullable): decrypted settings; copied
 * @number: (nullable): the line's own number
 * @allow_plaintext_loopback: the operator's loopback test switch
 * @error: (out) (optional): an address that may not be used
 * Returns: (transfer full) (nullable): the link
 */
VentureTelephonyLink *venture_telephony_link_new(const gchar *base_url, JsonObject *settings,
	const gchar *number, gboolean allow_plaintext_loopback, GError **error);
/**
 * venture_telephony_link_free:
 * @link: (nullable): a link; its settings are cleared
 */
void venture_telephony_link_free(VentureTelephonyLink *link);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureTelephonyLink, venture_telephony_link_free)
/**
 * venture_telephony_link_get_setting:
 * @link: a link
 * @name: a setting
 * Returns: (nullable): its value; a secret is never to be logged
 */
const gchar *venture_telephony_link_get_setting(VentureTelephonyLink *link, const gchar *name);
/**
 * venture_telephony_link_get_number:
 * @link: a link
 * Returns: (nullable): the line's number
 */
const gchar *venture_telephony_link_get_number(VentureTelephonyLink *link);
/**
 * venture_telephony_link_request:
 * @link: a link
 * @method: GET or POST
 * @path: path and query under the pinned origin
 * @authorization: (nullable): the Authorization header
 * @content_type: (nullable): the body's type
 * @body: (nullable): the body
 * @status: (out): the HTTP status
 * @response: (out) (transfer full): the answer, at most %VENTURE_TELEPHONY_MAX_RESPONSE
 * @error: (out) (optional): could not reach the provider, in words with no secret
 *
 * One request to the account's own origin: no redirects (a redirect would
 * carry credentials to a host the answer chose), no retries, a timeout.
 *
 * Returns: whether an answer arrived (of any status)
 */
gboolean venture_telephony_link_request(VentureTelephonyLink *link, const gchar *method, const gchar *path,
	const gchar *authorization, const gchar *content_type, GBytes *body, guint *status, GBytes **response, GError **error);
/**
 * venture_telephony_http_status_error:
 * @status: an HTTP status
 * @provider: the provider's name, for the message
 * @error: (out) (optional): the refusal in plain words
 * Returns: TRUE for a 2xx status, otherwise FALSE with @error set
 */
gboolean venture_telephony_http_status_error(guint status, const gchar *provider, GError **error);
/**
 * venture_telephony_signature_matches:
 * @secret: the shared secret
 * @body: the exact bytes
 * @signature: (nullable): hex HMAC-SHA256 the provider sent
 * Returns: whether they match, compared in constant time
 */
gboolean venture_telephony_signature_matches(const gchar *secret, GBytes *body, const gchar *signature);
G_END_DECLS
#endif
