/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <libsoup/soup.h>
#include <openssl/rand.h>
#include <string.h>

#define FED_HTTP_LIMIT (1024 * 1024)

typedef struct
{
	GMainLoop *loop;
	GCancellable *cancel;
	GInputStream *stream;
	GByteArray *bytes;
	GError *error;
} FedTransfer;

static void fed_read_done(GObject *source, GAsyncResult *result, gpointer data);

/* Bounded streaming, including responses without Content-Length. The total
 * deadline below also bounds a peer sending one byte just before each timeout. */
static void
fed_read_next(FedTransfer *transfer)
{
	g_input_stream_read_bytes_async(transfer->stream, 8192, G_PRIORITY_DEFAULT,
		transfer->cancel, fed_read_done, transfer);
}

static void
fed_read_done(GObject *source, GAsyncResult *result, gpointer data)
{
	FedTransfer *transfer = data;
	g_autoptr(GBytes) bytes = NULL;
	gsize length;
	const guint8 *buffer;
	bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &transfer->error);
	if (!bytes)
	{
		g_main_loop_quit(transfer->loop);
		return;
	}
	buffer = g_bytes_get_data(bytes, &length);
	if (length > FED_HTTP_LIMIT - transfer->bytes->len)
		g_set_error_literal(&transfer->error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "Federation response exceeds 1 MiB");
	else
		g_byte_array_append(transfer->bytes, buffer, (guint)length);
	if (!length || transfer->error)
		g_main_loop_quit(transfer->loop);
	else
		fed_read_next(transfer);
}

static void
fed_sent(GObject *source, GAsyncResult *result, gpointer data)
{
	FedTransfer *transfer = data;
	transfer->stream = soup_session_send_finish(SOUP_SESSION(source), result, &transfer->error);
	if (!transfer->stream)
		g_main_loop_quit(transfer->loop);
	else
		fed_read_next(transfer);
}

static gboolean
fed_timeout(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
	return G_SOURCE_CONTINUE;
}

static JsonNode *
fed_http(SoupSession *session, SoupMessage *message, const gchar *pin, const gchar *binding, GError **error)
{
	FedTransfer transfer;
	g_autoptr(JsonParser) parser = NULL;
	guint timer;
	JsonNode *answer = NULL;

	transfer.loop = g_main_loop_new(NULL, FALSE);
	transfer.cancel = g_cancellable_new();
	transfer.stream = NULL;
	transfer.bytes = g_byte_array_new();
	transfer.error = NULL;
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	timer = g_timeout_add_seconds(15, fed_timeout, transfer.cancel);
	soup_session_send_async(session, message, G_PRIORITY_DEFAULT, transfer.cancel, fed_sent, &transfer);
	g_main_loop_run(transfer.loop);
	g_source_remove(timer);
	if (transfer.error)
		g_propagate_error(error, transfer.error);
	else if (soup_message_get_status(message) != 200)
		g_set_error(error, VENTURE_ERROR,
			soup_message_get_status(message) == 409 ? VENTURE_ERROR_CONFLICT : VENTURE_ERROR_NETWORK,
			"Federation peer returned HTTP %u", soup_message_get_status(message));
	else
	{
		g_autoptr(GBytes) received = g_bytes_new(transfer.bytes->data, transfer.bytes->len);
		const gchar *signature = soup_message_headers_get_one(soup_message_get_response_headers(message), "X-Venture-Federation-Signature");
		if (!venture_federation_verify_response(pin, received, binding, signature, error))
			goto finished;
		parser = json_parser_new();
		if (!transfer.bytes->len || memchr(transfer.bytes->data, 0, transfer.bytes->len) ||
			!json_parser_load_from_data(parser, (const gchar *)transfer.bytes->data, transfer.bytes->len, NULL) ||
			!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION, "Invalid federation response");
		else
			answer = json_node_copy(json_parser_get_root(parser));
	}
finished:
	g_clear_object(&transfer.stream);
	g_object_unref(transfer.cancel);
	g_byte_array_unref(transfer.bytes);
	g_main_loop_unref(transfer.loop);
	return answer;
}

JsonNode *
venture_federation_request(VentureContext *context, gint64 peer_id,
	JsonNode *operation, GError **error)
{
	g_autoptr(VentureEntity) peer = NULL;
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) discovery = NULL;
	g_autoptr(SoupMessage) request = NULL;
	g_autoptr(GProxyResolver) proxy = NULL;
	g_autoptr(GTlsDatabase) trust = NULL;
	g_autoptr(JsonNode) identity = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *origin = NULL;
	g_autofree gchar *pin = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *signature = NULL;
	g_autofree gchar *ca_file = NULL;
	g_autofree gchar *nonce = NULL;
	guint8 nonce_bytes[32];
	g_autofree gchar *digest = NULL;
	gboolean active;
	JsonObject *remote;
	VentureDatabase *database = venture_context_get_database(context);

	if (!venture_context_module_enabled(context, "federation"))
		goto refused;
	peer = venture_database_get(database, VENTURE_TYPE_FEDERATION_PEER, peer_id, error);
	if (!peer)
		return NULL;
	g_object_get(peer, "origin", &origin, "public-key", &pin, "active", &active, NULL);
	if (!active || venture_entity_is_deleted(peer))
		goto refused;
	/* Outbound destinations are owner-pinned, never supplied by a remote
	 * payload or redirect. This deliberately supports private-network peers. */
	session = soup_session_new_with_options("timeout", (guint)15, NULL);
	proxy = g_simple_proxy_resolver_new(NULL, NULL);
	soup_session_set_proxy_resolver(session, proxy);
	g_object_get(venture_context_get_config(context), "federation-ca-file", &ca_file, NULL);
	if (!venture_string_is_empty(ca_file))
	{
		trust = g_tls_file_database_new(ca_file, error);
		if (!trust)
			return NULL;
		soup_session_set_tls_database(session, trust);
	}
	/* An unpredictable nonce prevents a TLS intermediary prefetching a
	 * signed discovery response for a future request. Hex is URI-safe. */
	if (RAND_bytes(nonce_bytes, sizeof(nonce_bytes)) != 1)
		goto refused;
	nonce = g_compute_checksum_for_data(G_CHECKSUM_SHA256, nonce_bytes, sizeof(nonce_bytes));
	url = g_strconcat(origin, "/federation/v1/identity?nonce=", nonce, NULL);
	discovery = soup_message_new("GET", url);
	if (!discovery || g_strcmp0(g_uri_get_scheme(soup_message_get_uri(discovery)), "https"))
		goto refused;
	identity = fed_http(session, discovery, pin, nonce, error);
	if (!identity)
		return NULL;
	remote = json_node_get_object(identity);
	if (g_strcmp0(venture_json_object_get_string(remote, "origin", ""), origin) ||
		g_strcmp0(venture_json_object_get_string(remote, "public_key", ""), pin))
		goto refused;
	/* The nested loop may have processed an owner revoking or rotating this
	 * peer. Do not send a request under an obsolete authorization decision. */
	current = venture_database_get(database, VENTURE_TYPE_FEDERATION_PEER, peer_id, NULL);
	if (!current || venture_entity_is_deleted(current) ||
		venture_entity_get_version(current) != venture_entity_get_version(peer))
		goto refused;
	body = venture_federation_sign(context, identity, operation, &signature, error);
	if (!body)
		return NULL;
	g_free(url);
	url = g_strconcat(origin, "/federation/v1/request", NULL);
	request = soup_message_new("POST", url);
	soup_message_set_request_body_from_bytes(request, "application/json", body);
	soup_message_headers_replace(soup_message_get_request_headers(request), "X-Venture-Federation-Signature", signature);
	digest = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, body);
	answer = fed_http(session, request, pin, digest, error);
	g_clear_object(&current);
	current = venture_database_get(database, VENTURE_TYPE_FEDERATION_PEER, peer_id, NULL);
	if (answer && (!current || venture_entity_is_deleted(current) ||
		venture_entity_get_version(current) != venture_entity_get_version(peer)))
		goto refused;
	return g_steal_pointer(&answer);
refused:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"Federation peer is disabled, changed, or failed identity verification");
	return NULL;
}
