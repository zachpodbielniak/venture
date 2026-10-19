/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>

G_DEFINE_INTERFACE(VentureTelephonyConnector, venture_telephony_connector, G_TYPE_OBJECT)

static void
venture_telephony_connector_default_init(VentureTelephonyConnectorInterface *iface)
{
	(void)iface;
}

static gboolean
unsupported(VentureTelephonyConnector *self, const gchar *what, GError **error)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s cannot %s",
		venture_telephony_connector_get_label(self), what);
	return FALSE;
}

/* --- Events ------------------------------------------------------------- */

VentureTelephonyEvent *
venture_telephony_event_new(VentureTelephonyEventKind kind)
{
	VentureTelephonyEvent *event = g_new0(VentureTelephonyEvent, 1);
	event->kind = kind;
	return event;
}

void
venture_telephony_event_free(VentureTelephonyEvent *event)
{
	if (event == NULL)
		return;
	g_free(event->id);
	g_free(event->from);
	g_free(event->to);
	g_free(event->extension);
	g_clear_pointer(&event->occurred_at, g_date_time_unref);
	g_free(event->recording_url);
	g_free(event->body);
	g_clear_pointer(&event->media, g_ptr_array_unref);
	g_free(event->status);
	g_free(event->error);
	g_free(event);
}

/* --- The interface's wrappers ------------------------------------------- */

const gchar *
venture_telephony_connector_get_id(VentureTelephonyConnector *self)
{
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), NULL);
	return VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self)->get_id(self);
}

const gchar *
venture_telephony_connector_get_label(VentureTelephonyConnector *self)
{
	VentureTelephonyConnectorInterface *iface;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), NULL);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	return iface->get_label != NULL ? iface->get_label(self) : iface->get_id(self);
}

VentureTelephonyCapabilities
venture_telephony_connector_get_capabilities(VentureTelephonyConnector *self)
{
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), VENTURE_TELEPHONY_CAPABILITY_NONE);
	return VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self)->get_capabilities(self);
}

JsonNode *
venture_telephony_connector_settings_schema(VentureTelephonyConnector *self)
{
	VentureTelephonyConnectorInterface *iface;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), NULL);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	return iface->settings_schema != NULL ? iface->settings_schema(self)
		: json_from_string("{\"type\":\"object\",\"properties\":{}}", NULL);
}

gchar *
venture_telephony_connector_account_identity(VentureTelephonyConnector *self, JsonObject *settings, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), NULL);
	g_return_val_if_fail(settings != NULL, NULL);
	return VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self)->account_identity(self, settings, error);
}

const gchar *
venture_telephony_connector_default_base_url(VentureTelephonyConnector *self)
{
	VentureTelephonyConnectorInterface *iface;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), NULL);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	return iface->default_base_url != NULL ? iface->default_base_url(self) : NULL;
}

gboolean
venture_telephony_connector_click_to_call(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const gchar *extension, const gchar *destination, gchar **call_id, GError **error)
{
	VentureTelephonyConnectorInterface *iface;
	g_autofree gchar *id = NULL;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), FALSE);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	if (iface->click_to_call == NULL ||
		!(venture_telephony_connector_get_capabilities(self) & VENTURE_TELEPHONY_CAPABILITY_CLICK_TO_CALL))
		return unsupported(self, "place calls", error);
	if (!iface->click_to_call(self, link, extension, destination, &id, error))
		return FALSE;
	if (call_id != NULL)
		*call_id = g_steal_pointer(&id);
	return TRUE;
}

gboolean
venture_telephony_connector_send_message(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyOutbound *message, gchar **provider_id, GError **error)
{
	VentureTelephonyConnectorInterface *iface;
	VentureTelephonyCapabilities can;
	g_autofree gchar *id = NULL;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), FALSE);
	g_return_val_if_fail(message != NULL, FALSE);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	can = venture_telephony_connector_get_capabilities(self);
	if (iface->send_message == NULL || !(can & VENTURE_TELEPHONY_CAPABILITY_SMS))
		return unsupported(self, "send texts", error);
	if (message->media != NULL && message->media[0] != NULL && !(can & VENTURE_TELEPHONY_CAPABILITY_MMS))
		return unsupported(self, "send pictures", error);
	if (!iface->send_message(self, link, message, &id, error))
		return FALSE;
	if (provider_id != NULL)
		*provider_id = g_steal_pointer(&id);
	return TRUE;
}

gboolean
venture_telephony_connector_verify_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error)
{
	VentureTelephonyConnectorInterface *iface;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), FALSE);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	/* A provider that claims to sign must check; one that does not sign
	 * is held to the address token and the allowed senders instead. */
	if (venture_telephony_connector_get_capabilities(self) & VENTURE_TELEPHONY_CAPABILITY_SIGNED_WEBHOOK)
	{
		if (iface->verify_webhook == NULL)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The webhook signature cannot be checked");
			return FALSE;
		}
		return iface->verify_webhook(self, link, hook, error);
	}
	return iface->verify_webhook == NULL || iface->verify_webhook(self, link, hook, error);
}

GPtrArray *
venture_telephony_connector_parse_webhook(VentureTelephonyConnector *self, VentureTelephonyLink *link,
	const VentureTelephonyWebhook *hook, GError **error)
{
	VentureTelephonyConnectorInterface *iface;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), NULL);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	if (iface->parse_webhook == NULL)
	{
		unsupported(self, "receive webhooks", error);
		return NULL;
	}
	return iface->parse_webhook(self, link, hook, error);
}

const gchar *
venture_telephony_connector_webhook_reply(VentureTelephonyConnector *self)
{
	VentureTelephonyConnectorInterface *iface;
	g_return_val_if_fail(VENTURE_IS_TELEPHONY_CONNECTOR(self), NULL);
	iface = VENTURE_TELEPHONY_CONNECTOR_GET_IFACE(self);
	return iface->webhook_reply != NULL ? iface->webhook_reply(self) : NULL;
}

/* --- The registry --------------------------------------------------------- */

/* Filled by constructors before main() and read afterwards, so it needs no
 * lock: no provider registers from a thread. */
static GPtrArray *providers;

void
venture_telephony_provider_register(GType (*get_type)(void))
{
	guint i;
	if (get_type == NULL)
		return;
	if (providers == NULL)
		providers = g_ptr_array_new();
	for (i = 0; i < providers->len; i++)
		if (g_ptr_array_index(providers, i) == (gpointer)get_type)
			return;
	g_ptr_array_add(providers, (gpointer)get_type);
}

VentureTelephonyConnector *
venture_telephony_provider_new(const gchar *id)
{
	guint i;
	for (i = 0; id != NULL && providers != NULL && i < providers->len; i++)
	{
		GType (*get_type)(void) = (GType (*)(void))g_ptr_array_index(providers, i);
		g_autoptr(GObject) object = g_object_new(get_type(), NULL);
		if (VENTURE_IS_TELEPHONY_CONNECTOR(object) &&
			g_strcmp0(venture_telephony_connector_get_id(VENTURE_TELEPHONY_CONNECTOR(object)), id) == 0)
			return VENTURE_TELEPHONY_CONNECTOR(g_steal_pointer(&object));
	}
	return NULL;
}

static gint
compare_ids(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

gchar **
venture_telephony_provider_ids(void)
{
	GPtrArray *ids = g_ptr_array_new();
	guint i;
	for (i = 0; providers != NULL && i < providers->len; i++)
	{
		GType (*get_type)(void) = (GType (*)(void))g_ptr_array_index(providers, i);
		g_autoptr(GObject) object = g_object_new(get_type(), NULL);
		if (VENTURE_IS_TELEPHONY_CONNECTOR(object))
			g_ptr_array_add(ids, g_strdup(venture_telephony_connector_get_id(VENTURE_TELEPHONY_CONNECTOR(object))));
	}
	g_ptr_array_sort(ids, compare_ids);
	g_ptr_array_add(ids, NULL);
	return (gchar **)g_ptr_array_free(ids, FALSE);
}

/* --- The link: one account, one pinned origin ------------------------------ */

struct _VentureTelephonyLink {
	gchar *origin;		/* scheme://host[:port] */
	gchar *prefix;		/* the base address's path, without a trailing slash */
	gchar *host;
	gchar *number;
	GHashTable *settings;	/* name -> string value; cleared on free */
};

static gboolean
loopback(const gchar *host)
{
	return g_strcmp0(host, "127.0.0.1") == 0 || g_strcmp0(host, "::1") == 0 || g_strcmp0(host, "localhost") == 0;
}

static void
clear_secret(gpointer data)
{
	gchar *value = data;
	if (value != NULL)
		memset(value, 0, strlen(value));
	g_free(value);
}

VentureTelephonyLink *
venture_telephony_link_new(const gchar *base_url, JsonObject *settings, const gchar *number,
	gboolean allow_plaintext_loopback, GError **error)
{
	g_autoptr(GUri) uri = NULL;
	VentureTelephonyLink *link;
	const gchar *scheme, *host, *path;
	gint port;
	uri = base_url != NULL ? g_uri_parse(base_url, G_URI_FLAGS_NONE, NULL) : NULL;
	scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
	host = uri != NULL ? g_uri_get_host(uri) : NULL;
	if (uri == NULL || host == NULL || *host == '\0' || g_uri_get_userinfo(uri) != NULL ||
		g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
		!(g_strcmp0(scheme, "https") == 0 || (g_strcmp0(scheme, "http") == 0 && allow_plaintext_loopback && loopback(host))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The provider's address must be an https:// address with no user name, query or fragment");
		return NULL;
	}
	link = g_new0(VentureTelephonyLink, 1);
	port = g_uri_get_port(uri);
	link->origin = port > 0 ? g_strdup_printf("%s://%s:%d", scheme, host, port) : g_strdup_printf("%s://%s", scheme, host);
	path = g_uri_get_path(uri);
	link->prefix = g_strdup(path != NULL ? path : "");
	while (*link->prefix != '\0' && g_str_has_suffix(link->prefix, "/"))
		link->prefix[strlen(link->prefix) - 1] = '\0';
	link->host = g_strdup(host);
	link->number = g_strdup(number);
	link->settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, clear_secret);
	if (settings != NULL)
	{
		JsonObjectIter iter;
		const gchar *name;
		JsonNode *node;
		json_object_iter_init(&iter, settings);
		while (json_object_iter_next(&iter, &name, &node))
			if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
				g_hash_table_replace(link->settings, g_strdup(name), g_strdup(json_node_get_string(node)));
	}
	return link;
}

void
venture_telephony_link_free(VentureTelephonyLink *link)
{
	if (link == NULL)
		return;
	g_free(link->origin);
	g_free(link->prefix);
	g_free(link->host);
	g_free(link->number);
	g_hash_table_unref(link->settings);
	g_free(link);
}

const gchar *
venture_telephony_link_get_setting(VentureTelephonyLink *link, const gchar *name)
{
	g_return_val_if_fail(link != NULL, NULL);
	return g_hash_table_lookup(link->settings, name);
}

const gchar *
venture_telephony_link_get_number(VentureTelephonyLink *link)
{
	g_return_val_if_fail(link != NULL, NULL);
	return link->number;
}

gboolean
venture_telephony_link_request(VentureTelephonyLink *link, const gchar *method, const gchar *path,
	const gchar *authorization, const gchar *content_type, GBytes *body, guint *status, GBytes **response, GError **error)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(GBytes) answer = NULL;
	g_autofree gchar *url = NULL;
	GUri *final;
	g_return_val_if_fail(link != NULL && path != NULL && *path == '/', FALSE);
	*status = 0;
	*response = NULL;
	url = g_strconcat(link->origin, link->prefix, path, NULL);
	message = soup_message_new(method, url);
	if (message == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The provider's address cannot be used");
		return FALSE;
	}
	session = soup_session_new();
	soup_session_set_timeout(session, VENTURE_TELEPHONY_TIMEOUT_SECONDS);
	soup_session_set_idle_timeout(session, VENTURE_TELEPHONY_TIMEOUT_SECONDS);
	/* A redirect would carry the credentials to a host the answer chose. */
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "application/json");
	if (authorization != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", authorization);
	if (body != NULL)
		soup_message_set_request_body_from_bytes(message, content_type != NULL ? content_type : "application/octet-stream", body);
	answer = soup_session_send_and_read(session, message, NULL, &local);
	if (answer == NULL)
	{
		/* The transport's message can name the URL; ours names the host. */
		g_debug("Telephony request to %s failed: %s", link->host, local != NULL ? local->message : "no answer");
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Could not reach the provider at %s; nothing was retried", link->host);
		return FALSE;
	}
	final = soup_message_get_uri(message);
	if (final == NULL || g_strcmp0(g_uri_get_host(final), link->host) != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The provider answered from another address");
		return FALSE;
	}
	if (g_bytes_get_size(answer) > VENTURE_TELEPHONY_MAX_RESPONSE)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The provider's answer was too large");
		return FALSE;
	}
	*status = soup_message_get_status(message);
	*response = g_steal_pointer(&answer);
	return TRUE;
}

gboolean
venture_telephony_http_status_error(guint status, const gchar *provider, GError **error)
{
	const gchar *name = provider != NULL ? provider : "The provider";
	if (status >= 200 && status < 300)
		return TRUE;
	if (status == 401 || status == 403)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"%s refused the credentials; check them in Telephony settings", name);
	else if (status == 404)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "%s does not know that address or extension", name);
	else if (status == 429)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "%s is limiting requests; try again in a minute", name);
	else if (status >= 500)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "%s had a problem (HTTP %u); nothing was retried", name, status);
	else
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s refused the request (HTTP %u)", name, status);
	return FALSE;
}

gboolean
venture_telephony_signature_matches(const gchar *secret, GBytes *body, const gchar *signature)
{
	g_autofree gchar *expected = NULL;
	gsize i, length;
	guchar difference = 0;
	if (secret == NULL || *secret == '\0' || body == NULL || signature == NULL)
		return FALSE;
	expected = g_compute_hmac_for_data(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret),
		g_bytes_get_data(body, NULL), g_bytes_get_size(body));
	length = strlen(expected);
	if (strlen(signature) != length)
		return FALSE;
	for (i = 0; i < length; i++)
		difference |= (guchar)(g_ascii_tolower(signature[i]) ^ expected[i]);
	return difference == 0;
}
