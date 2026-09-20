/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-http-limits-private.h"

/* Soup's got-chunk signal follows its automatic accumulation. Disable that
 * accumulation before reading anything and retain only individually checked
 * chunks. Neither a route nor a plugin can opt out of this transport boundary. */
typedef struct _Limits Limits;
typedef struct {
	Limits *limits;
	GSocket *socket;
	SoupServerMessage *first, *current;
	GSource *deadline;
	gboolean headers, dispatching, rejecting;
	gsize buffered;
} Connection;
struct _Limits {
	grefcount refs;
	GMainContext *context;
	GHashTable *connections;
	gsize maximum_body, maximum_buffered, buffered;
	guint maximum_connections, timeout, rejecting;
};
typedef struct {
	Limits *limits;
	GIOStream *stream;
	GSocket *socket;
	GCancellable *cancel;
	GSource *deadline;
	const gchar *response;
} Rejection;
static void limits_unref(Limits *limits);
static void connection_reject(Connection *connection, guint status);
static void socket_abort(GSocket *socket)
{
	/* TLS backends can hold an in-flight read on the socket. Shutdown must
	 * reach that read before releasing our descriptor, including pre-hello. */
	g_socket_shutdown(socket, TRUE, TRUE, NULL);
	g_socket_close(socket, NULL);
}
static void source_clear(GSource **source)
{
	if (*source) { g_source_destroy(*source); g_source_unref(*source); *source = NULL; }
}
static void connection_free(gpointer data)
{
	Connection *connection = data;
	source_clear(&connection->deadline);
	connection->limits->buffered -= connection->buffered;
	g_signal_handlers_disconnect_by_data(connection->first, connection);
	if (connection->current) g_signal_handlers_disconnect_by_data(connection->current, connection);
	g_clear_object(&connection->first); g_clear_object(&connection->current);
	g_clear_object(&connection->socket); g_free(connection);
}
static void connection_disconnected(SoupServerMessage *message, gpointer data)
{
	Connection *connection = data;
	(void)message;
	if (!connection->rejecting) g_hash_table_remove(connection->limits->connections, connection->socket);
}
static void rejection_closed(GObject *object, GAsyncResult *result, gpointer data)
{
	Rejection *rejection = data;
	g_io_stream_close_finish(G_IO_STREAM(object), result, NULL);
	source_clear(&rejection->deadline);
	/* Closing the underlying socket is nonblocking even for a TLS peer that
	 * never reads its close-notify. No borrowed stream can outlive the bound. */
	socket_abort(rejection->socket);
	rejection->limits->rejecting--;
	limits_unref(rejection->limits);
	g_object_unref(rejection->stream); g_object_unref(rejection->socket);
	g_object_unref(rejection->cancel); g_free(rejection);
}
static void rejection_done(GObject *object, GAsyncResult *result, gpointer data)
{
	Rejection *rejection = data;
	g_output_stream_write_all_finish(G_OUTPUT_STREAM(object), result, NULL, NULL);
	/* TLS close-notify is part of a complete HTTP error response. The same
	 * one-second cancellable deadline bounds both write and orderly close. */
	g_main_context_push_thread_default(rejection->limits->context);
	g_io_stream_close_async(rejection->stream, G_PRIORITY_DEFAULT, rejection->cancel, rejection_closed, rejection);
	g_main_context_pop_thread_default(rejection->limits->context);
}
static gboolean rejection_timeout(gpointer data)
{
	Rejection *rejection = data;
	g_cancellable_cancel(rejection->cancel);
	socket_abort(rejection->socket);
	return G_SOURCE_REMOVE;
}
static void connection_reject(Connection *connection, guint status)
{
	Limits *limits = connection->limits;
	g_autoptr(SoupServerMessage) message = g_object_ref(connection->current);
	g_autoptr(GSocket) socket = g_object_ref(connection->socket);
	g_autoptr(GIOStream) stream = NULL;
	gboolean reply = connection->headers && !connection->dispatching &&
		soup_server_message_get_http_version(message) < SOUP_HTTP_2_0;
	connection->rejecting = TRUE;
	source_clear(&connection->deadline);
	soup_message_body_truncate(soup_server_message_get_request_body(message));
	/* A status alone does not stop Soup's non-Expect body reader. Steal only
	 * established HTTP/1 streams; this preserves TLS encryption. Unparsed or
	 * multiplexed connections close without inventing an HTTP/1 response. */
	if (reply) stream = soup_server_message_steal_connection(message);
	if (stream)
	{
		Rejection *rejection = g_new0(Rejection, 1);
		rejection->limits = limits; g_ref_count_inc(&limits->refs); limits->rejecting++;
		rejection->stream = g_steal_pointer(&stream); rejection->socket = g_object_ref(socket);
		rejection->cancel = g_cancellable_new();
		rejection->response = status == 413 ?
			"HTTP/1.1 413 Content Too Large\r\nConnection: close\r\nContent-Length: 0\r\nCache-Control: no-store\r\n\r\n" :
			status == 400 ?
			"HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\nCache-Control: no-store\r\n\r\n" :
			status == 503 ?
			"HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\nCache-Control: no-store\r\nRetry-After: 1\r\n\r\n" :
			"HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\nCache-Control: no-store\r\n\r\n";
		rejection->deadline = g_timeout_source_new(1000);
		g_source_set_callback(rejection->deadline, rejection_timeout, rejection, NULL);
		g_source_attach(rejection->deadline, limits->context);
		g_main_context_push_thread_default(limits->context);
		g_output_stream_write_all_async(g_io_stream_get_output_stream(rejection->stream), rejection->response,
			strlen(rejection->response), G_PRIORITY_DEFAULT, rejection->cancel, rejection_done, rejection);
		g_main_context_pop_thread_default(limits->context);
	}
	else socket_abort(socket);
	g_hash_table_remove(limits->connections, socket);
}
static gboolean connection_timeout(gpointer data)
{
	Connection *connection = data;
	connection_reject(connection, 408);
	return G_SOURCE_REMOVE;
}
static void deadline_reset(Connection *connection)
{
	source_clear(&connection->deadline);
	connection->deadline = g_timeout_source_new(connection->limits->timeout * 1000);
	g_source_set_callback(connection->deadline, connection_timeout, connection, NULL);
	g_source_attach(connection->deadline, connection->limits->context);
}
static void got_headers(SoupServerMessage *message, gpointer data)
{
	Connection *connection = data;
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
	connection->headers = TRUE;
	if (soup_server_message_get_http_version(message) >= SOUP_HTTP_2_0)
	{ connection_reject(connection, 503); return; }
	if (soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH)
	{
		goffset length = soup_message_headers_get_content_length(headers);
		if (length < 0) connection_reject(connection, 400);
		else if (length > (goffset)connection->limits->maximum_body) connection_reject(connection, 413);
	}
}
static void got_chunk(SoupServerMessage *message, GBytes *chunk, gpointer data)
{
	Connection *connection = data;
	SoupMessageBody *body = soup_server_message_get_request_body(message);
	gsize length = g_bytes_get_size(chunk);
	if (length > connection->limits->maximum_body - (gsize)body->length)
	{ connection_reject(connection, 413); return; }
	if (length > connection->limits->maximum_buffered - connection->limits->buffered)
	{ connection_reject(connection, 503); return; }
	connection->buffered += length; connection->limits->buffered += length;
	soup_message_body_append_bytes(body, chunk);
}
static void request_read(SoupServer *server, SoupServerMessage *message, gpointer data)
{
	Limits *limits = data;
	Connection *connection = g_hash_table_lookup(limits->connections, soup_server_message_get_socket(message));
	(void)server;
	if (!connection) return;
	/* This signal precedes the normal handler. Finalize checked chunks in the
	 * same body object HtmxRequest uses, including embedded NUL bytes. */
	soup_message_body_set_accumulate(soup_server_message_get_request_body(message), TRUE);
	g_bytes_unref(soup_message_body_flatten(soup_server_message_get_request_body(message)));
	limits->buffered -= connection->buffered; connection->buffered = 0;
	connection->dispatching = TRUE;
	source_clear(&connection->deadline);
}
static void request_finished(SoupServer *server, SoupServerMessage *message, gpointer data)
{
	Limits *limits = data;
	Connection *connection = g_hash_table_lookup(limits->connections, soup_server_message_get_socket(message));
	(void)server;
	if (!connection) return;
	soup_message_body_truncate(soup_server_message_get_request_body(message));
	connection->headers = FALSE; connection->dispatching = FALSE;
	deadline_reset(connection);
}
static gboolean close_excess(gpointer data)
{ socket_abort(G_SOCKET(data)); return G_SOURCE_REMOVE; }
static void request_started(SoupServer *server, SoupServerMessage *message, gpointer data)
{
	Limits *limits = data;
	GSocket *socket = soup_server_message_get_socket(message);
	Connection *connection = socket ? g_hash_table_lookup(limits->connections, socket) : NULL;
	(void)server;
	soup_message_body_set_accumulate(soup_server_message_get_request_body(message), FALSE);
	if (!socket) { soup_server_message_set_status(message, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL); return; }
	if (!connection)
	{
		if (g_hash_table_size(limits->connections) + limits->rejecting >= limits->maximum_connections)
		{
			GSource *source = g_idle_source_new();
			/* request-started may run inside TLS connection construction. Close
			 * on the next turn, before accepting a body or dispatching a route. */
			g_source_set_priority(source, G_PRIORITY_HIGH);
			g_source_set_callback(source, close_excess, g_object_ref(socket), g_object_unref);
			g_source_attach(source, limits->context); g_source_unref(source);
			soup_server_message_set_status(message, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
			return;
		}
		connection = g_new0(Connection, 1); connection->limits = limits;
		connection->socket = g_object_ref(socket); connection->first = g_object_ref(message);
		g_signal_connect(message, "disconnected", G_CALLBACK(connection_disconnected), connection);
		g_hash_table_insert(limits->connections, socket, connection);
	}
	if (connection->current && connection->current != connection->first)
		g_signal_handlers_disconnect_by_data(connection->current, connection);
	g_set_object(&connection->current, message);
	connection->headers = FALSE; connection->dispatching = FALSE;
	g_signal_connect(message, "got-headers", G_CALLBACK(got_headers), connection);
	g_signal_connect(message, "got-chunk", G_CALLBACK(got_chunk), connection);
	deadline_reset(connection);
}
static void limits_unref(Limits *limits)
{
	if (!g_ref_count_dec(&limits->refs)) return;
	g_hash_table_unref(limits->connections); g_main_context_unref(limits->context); g_free(limits);
}
static void limits_stop(gpointer data)
{
	Limits *limits = data;
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, limits->connections);
	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		Connection *connection = value;
		socket_abort(connection->socket);
	}
	g_hash_table_remove_all(limits->connections); limits_unref(limits);
}
gboolean venture_http_limits_validate(VentureConfig *config, GError **error)
{
	gint64 size, timeout, connections, buffered;
	if (g_getenv("SOUP_SERVER_HTTP2") != NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			"Experimental SOUP_SERVER_HTTP2 is unsupported; terminate HTTP/2 at the gateway and use HTTP/1 upstream");
		return FALSE;
	}
	g_object_get(config, "server-max-request-size-mb", &size, "server-request-timeout", &timeout,
		"server-max-connections", &connections, "server-max-buffered-request-mb", &buffered, NULL);
	if (size < 1 || size > 1024 || timeout < 1 || timeout > 3600 || connections < 1 || connections > 4096 || buffered < size || buffered > 4096 || (guint64)buffered > G_MAXSIZE / (1024 * 1024))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			"HTTP limits require max_request_size_mb=1..1024, request_timeout=1..3600, max_connections=1..4096 and max_buffered_request_mb=max_request_size_mb..4096");
		return FALSE;
	}
	return TRUE;
}
void venture_http_limits_install(SoupServer *server, VentureConfig *config)
{
	Limits *limits = g_new0(Limits, 1);
	gint64 size, timeout, connections, buffered;
	g_object_get(config, "server-max-request-size-mb", &size, "server-request-timeout", &timeout,
		"server-max-connections", &connections, "server-max-buffered-request-mb", &buffered, NULL);
	g_ref_count_init(&limits->refs); limits->context = g_main_context_ref_thread_default();
	limits->maximum_body = (gsize)size * 1024 * 1024;
	limits->maximum_buffered = (gsize)buffered * 1024 * 1024;
	limits->timeout = (guint)timeout; limits->maximum_connections = (guint)connections;
	limits->connections = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, connection_free);
	g_object_set_data_full(G_OBJECT(server), "venture-http-limits", limits, limits_stop);
	g_signal_connect(server, "request-started", G_CALLBACK(request_started), limits);
	g_signal_connect(server, "request-read", G_CALLBACK(request_read), limits);
	g_signal_connect(server, "request-finished", G_CALLBACK(request_finished), limits);
}
