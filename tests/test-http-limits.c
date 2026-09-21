/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

/* A real socket must be rejected before either a future plugin route or generic
 * record writer can see its body. Handler-side Content-Length checks miss this. */
typedef struct { VentureConfig *config; VentureDatabase *database; VentureContext *context; VentureWebServer *server; gchar *state; guint port; guint writes; gboolean tls; GMainContext *owner; const gchar *nested_request; gchar *nested_response; } Fixture;
typedef struct { guint port; gchar *request; gchar *response; gint done; GError *error; guint delay; guint timeout; gboolean tls; } Exchange;
static gchar *exchange(Fixture *f, const gchar *request);
static gchar *probe(Fixture *f, const gchar *request, guint seconds);
static HtmxResponse *write_handler(HtmxRequest *request, GHashTable *params, gpointer data)
{
	Fixture *f = data;
	GBytes *body = htmx_request_get_body_bytes(request);
	(void)params;
	f->writes++;
	if (f->nested_request)
	{
		const gchar *request_text = f->nested_request;
		f->nested_request = NULL;
		/* Recorded, never asserted: see test_nested_dispatch for why a route
		 * that talks to its own server gets no answer while it blocks here. */
		f->nested_response = probe(f, request_text, 1);
	}
	return htmx_response_new_with_content(body && g_bytes_get_size(body) == 3 && !memcmp(g_bytes_get_data(body, NULL), "abc", 3) ? "yes" : "bad");
}
static HtmxResponse *error_handler(HtmxRequest *request, GHashTable *params, gpointer data)
{
	HtmxResponse *response = htmx_response_new_with_content("broken");
	(void)request; (void)params; (void)data;
	htmx_response_set_status(response, 500);
	return response;
}
static void tls_event(GSocketClient *client, GSocketClientEvent event, GSocketConnectable *connectable, GIOStream *connection, gpointer data)
{
	(void)client; (void)connectable;
	if (event == G_SOCKET_CLIENT_TLS_HANDSHAKING) g_tls_connection_set_database(G_TLS_CONNECTION(connection), G_TLS_DATABASE(data));
}
static gpointer exchange_thread(gpointer data)
{
	Exchange *x = data;
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GString) response = g_string_new(NULL);
	g_autoptr(GTlsDatabase) trust = NULL;
	gchar buffer[1024];
	gssize count;
	g_socket_client_set_timeout(client, x->timeout);
	if (x->tls)
	{
		g_autofree gchar *path = g_canonicalize_filename("tests/fixtures/federation/tls-cert.pem", NULL);
		trust = g_tls_file_database_new(path, &x->error);
		g_assert_no_error(x->error);
		g_socket_client_set_tls(client, TRUE);
		g_signal_connect(client, "event", G_CALLBACK(tls_event), trust);
	}
	connection = g_socket_client_connect_to_host(client, "127.0.0.1", (guint16)x->port, NULL, &x->error);
	if (connection)
	{
		g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)), x->request, strlen(x->request), NULL, NULL, &x->error);
		if (x->delay) g_usleep(x->delay * G_USEC_PER_SEC);
		while (!x->error && (count = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(connection)), buffer, sizeof(buffer), NULL, &x->error)) > 0)
			g_string_append_len(response, buffer, count);
		g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	}
	x->response = g_string_free(g_steal_pointer(&response), FALSE);
	g_atomic_int_set(&x->done, 1);
	return NULL;
}
static gchar *run_exchange(Fixture *f, const gchar *request, guint seconds, GError **error)
{
	Exchange x;
	GThread *thread;
	gint64 deadline = g_get_monotonic_time() + (seconds + 3) * G_USEC_PER_SEC;
	x.port = f->port; x.request = (gchar *)request; x.response = NULL; x.done = 0; x.error = NULL;
	x.delay = 0; x.timeout = seconds; x.tls = f->tls;
	thread = g_thread_new("http-limit-client", exchange_thread, &x);
	while (!g_atomic_int_get(&x.done) && g_get_monotonic_time() < deadline)
	{ g_main_context_iteration(f->owner, FALSE); g_usleep(1000); }
	g_assert_true(g_atomic_int_get(&x.done)); g_thread_join(thread);
	if (g_error_matches(x.error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED)) g_clear_error(&x.error);
	if (x.error) g_propagate_error(error, x.error);
	return x.response;
}
static gchar *exchange(Fixture *f, const gchar *request)
{
	g_autoptr(GError) error = NULL;
	gchar *response = run_exchange(f, request, 5, &error);
	g_assert_no_error(error);
	return response;
}
/* A bounded attempt that is allowed to go unanswered: the caller decides what
 * an empty response means. Its own errors are discarded, not asserted. */
static gchar *probe(Fixture *f, const gchar *request, guint seconds)
{
	g_autoptr(GError) error = NULL;
	return run_exchange(f, request, seconds, &error);
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocket) reserve = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error);
	g_autoptr(GInetAddress) address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autoptr(GSocketAddress) requested = g_inet_socket_address_new(address, 0), actual = NULL;
	(void)data;
	g_assert_no_error(error);
	g_assert_true(g_socket_bind(reserve, requested, FALSE, &error));
	actual = g_socket_get_local_address(reserve, &error); g_assert_no_error(error);
	f->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(actual));
	g_assert_true(g_socket_close(reserve, &error));
	f->owner = g_main_context_ref_thread_default();
	f->state = g_dir_make_tmp("venture-http-limits-XXXXXX", &error); g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state, "server-port", (gint64)f->port,
		"security-require-auth", FALSE, "server-max-request-size-mb", (gint64)1,
		"server-request-timeout", (gint64)(data == GINT_TO_POINTER(2) ? 5 : 1), "server-max-connections", (gint64)2, "server-max-buffered-request-mb", (gint64)1, NULL);
	f->tls = data == GINT_TO_POINTER(1);
	if (f->tls)
	{
		g_autofree gchar *cert = g_canonicalize_filename("tests/fixtures/federation/tls-cert.pem", NULL);
		g_autofree gchar *key = g_canonicalize_filename("tests/fixtures/federation/tls-key.pem", NULL);
		g_object_set(f->config, "server-tls-certificate", cert, "server-tls-private-key", key, NULL);
	}
	f->database = venture_database_new("sqlite://:memory:", &error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	venture_web_server_add_classified_route(f->server, HTMX_METHOD_POST, "/fixture/write", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, write_handler, f);
	venture_web_server_add_classified_route(f->server, HTMX_METHOD_GET, "/fixture/error", VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, error_handler, f);
	g_assert_true(venture_web_server_start(f->server, &error)); g_assert_no_error(error);
}
static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->server) { venture_web_server_stop(f->server); g_object_unref(f->server); } g_object_unref(f->context);
	g_object_unref(f->database); g_object_unref(f->config); g_clear_pointer(&f->nested_response, g_free);
	venture_test_remove_tree(f->state); g_free(f->state); g_main_context_unref(f->owner);
}
static void test_declared(Fixture *f, gconstpointer data)
{
	g_autofree gchar *response = exchange(f, "POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\nConnection: close\r\n\r\n");
	(void)data;
	g_assert_nonnull(strstr(response, " 413 ")); g_assert_cmpuint(f->writes, ==, 0);
}
static void test_ordinary(Fixture *f, gconstpointer data)
{
	g_autofree gchar *response = exchange(f, "POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n1\r\na\r\n2\r\nbc\r\n0\r\n\r\n");
	(void)data;
	g_assert_nonnull(strstr(response, " 200 ")); g_assert_nonnull(strstr(response, "yes")); g_assert_cmpuint(f->writes, ==, 1);
}

static void test_chunked_overflow(Fixture *f, gconstpointer data)
{
	g_autoptr(GString) request = g_string_new("POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n100001\r\n");
	g_autofree gchar *body = g_strnfill(1048577, 'x'), *response = NULL;
	(void)data;
	g_string_append(request, body); g_string_append(request, "\r\n0\r\n\r\n");
	response = exchange(f, request->str);
	g_assert_nonnull(strstr(response, " 413 ")); g_assert_cmpuint(f->writes, ==, 0);
}
static void test_slow_body(Fixture *f, gconstpointer data)
{
	g_autofree gchar *response = exchange(f, "POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\na");
	(void)data;
	g_assert_nonnull(strstr(response, " 408 ")); g_assert_cmpuint(f->writes, ==, 0);
}
static void test_slow_headers(Fixture *f, gconstpointer data)
{
	g_autofree gchar *response = exchange(f, "POST /fixture/write HTTP/1.1\r\nHost:");
	(void)data;
	g_assert_cmpstr(response, ==, ""); g_assert_cmpuint(f->writes, ==, 0);
}
static void test_idle(Fixture *f, gconstpointer data)
{
	g_autofree gchar *response = exchange(f, "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
	(void)data;
	g_assert_nonnull(strstr(response, " 200 ")); g_assert_cmpuint(f->writes, ==, 0);
}
static void test_idle_after_error(Fixture *f, gconstpointer data)
{
	/* Soup reports a 500 as request-aborted, not request-finished, and the
	 * connection stays keep-alive. The idle deadline must still be re-armed:
	 * the client reads until the server closes, and the exchange helper's
	 * five-second client timeout fails the case if the server never does. */
	g_autofree gchar *response = exchange(f, "GET /fixture/error HTTP/1.1\r\nHost: localhost\r\n\r\n");
	(void)data;
	g_assert_nonnull(strstr(response, " 500 ")); g_assert_cmpuint(f->writes, ==, 0);
}

static void pump(void)
{
	gint64 until = g_get_monotonic_time() + 30000;
	while (g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
}
static void test_connections(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) first = NULL, second = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *response = NULL;
	Fixture neighbor;
	(void)data;
	g_socket_client_set_timeout(client, 3);
	first = g_socket_client_connect_to_host(client, "127.0.0.1", (guint16)f->port, NULL, &error); g_assert_no_error(error);
	second = g_socket_client_connect_to_host(client, "127.0.0.1", (guint16)f->port, NULL, &error); g_assert_no_error(error);
	pump();
	response = exchange(f, "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	g_assert_true(response[0] == '\0' || strstr(response, " 503 ") != NULL);
	/* The cap belongs to this listening server, not a process-global count
	 * that could let one workspace consume its neighbor's admission slots. */
	memset(&neighbor, 0, sizeof(neighbor)); setup(&neighbor, NULL);
	g_clear_pointer(&response, g_free);
	response = exchange(&neighbor, "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	g_assert_nonnull(strstr(response, " 200 ")); teardown(&neighbor, NULL);
	g_io_stream_close(G_IO_STREAM(first), NULL, NULL); g_io_stream_close(G_IO_STREAM(second), NULL, NULL); pump();
	g_clear_pointer(&response, g_free);
	response = exchange(f, "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	g_assert_nonnull(strstr(response, " 200 "));
}
static void test_tls_stall(Fixture *f, gconstpointer data)
{
	g_autofree gchar *response = NULL;
	(void)data;
	f->tls = FALSE; response = exchange(f, ""); f->tls = TRUE;
	g_assert_cmpstr(response, ==, "");
	g_clear_pointer(&response, g_free);
	response = exchange(f, "GET /api/v1/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	g_assert_nonnull(strstr(response, " 200 "));
}
static void test_generic(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(GPtrArray) before = NULL, after = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *response = NULL, *request = NULL;
	const gchar *json = "{\"name\":\"Ordinary HTTP\",\"active\":true}";
	(void)data;
	before = venture_database_find(f->database, query, &error); g_assert_no_error(error);
	response = exchange(f, "POST /api/v1/organization HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 1048577\r\nConnection: close\r\n\r\n");
	g_assert_nonnull(strstr(response, " 413 "));
	after = venture_database_find(f->database, query, &error); g_assert_no_error(error); g_assert_cmpuint(after->len, ==, before->len);
	request = g_strdup_printf("POST /api/v1/organization HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(json), json);
	g_clear_pointer(&response, g_free); response = exchange(f, request);
	g_assert_nonnull(strstr(response, " 201 "));
	g_clear_pointer(&after, g_ptr_array_unref); after = venture_database_find(f->database, query, &error);
	g_assert_no_error(error); g_assert_cmpuint(after->len, ==, before->len + 1);
}


static void test_aggregate(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) held = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = g_strnfill(600000, 'x'), *request = NULL, *response = NULL;
	(void)data;
	g_socket_client_set_timeout(client, 3);
	held = g_socket_client_connect_to_host(client, "127.0.0.1", (guint16)f->port, NULL, &error); g_assert_no_error(error);
	request = g_strdup_printf("POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 700000\r\nConnection: close\r\n\r\n%s", body);
	g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(held)), request, strlen(request), NULL, NULL, &error)); g_assert_no_error(error);
	pump();
	response = exchange(f, request);
	/* Either concurrently readable socket may reach the aggregate ceiling
	 * first. Require a real 503, without assuming socket scheduling order. */
	if (!strstr(response, " 503 "))
	{
		gchar rejected[1024];
		gssize length;
		g_assert_nonnull(strstr(response, " 408 "));
		length = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(held)), rejected, sizeof(rejected) - 1, NULL, &error);
		g_assert_no_error(error); g_assert_cmpint(length, >, 0); rejected[length] = '\0';
		g_assert_nonnull(strstr(rejected, " 503 "));
	}
	g_assert_cmpuint(f->writes, ==, 0);
	g_io_stream_close(G_IO_STREAM(held), NULL, NULL); pump();
	g_clear_pointer(&request, g_free); g_clear_pointer(&response, g_free);
	request = g_strdup_printf("POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 600000\r\nConnection: close\r\n\r\n%s", body);
	response = exchange(f, request); g_assert_nonnull(strstr(response, " 200 ")); g_assert_cmpuint(f->writes, ==, 1);
	/* Successful dispatch also releases the receive budget, not only abort. */
	g_clear_pointer(&response, g_free); response = exchange(f, request);
	g_assert_nonnull(strstr(response, " 200 ")); g_assert_cmpuint(f->writes, ==, 2);
}


static void test_timeout_budget(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = g_strnfill(600000, 'x'), *request = NULL, *response = NULL;
	(void)data;
	request = g_strdup_printf("POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 700000\r\nConnection: close\r\n\r\n%s", body);
	response = exchange(f, request); g_assert_nonnull(strstr(response, " 408 ")); g_assert_cmpuint(f->writes, ==, 0);
	g_clear_pointer(&request, g_free); g_clear_pointer(&response, g_free);
	request = g_strdup_printf("POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 600000\r\nConnection: close\r\n\r\n%s", body);
	response = exchange(f, request); g_assert_nonnull(strstr(response, " 200 ")); g_assert_cmpuint(f->writes, ==, 1);
}
/* libsoup 3 runs a route synchronously on the server's main context, so a route
 * that blocks waiting on this same server gets no answer while it blocks:
 * iterating the context from inside the handler does not get the nested
 * connection served. Nothing in VENTURE issues an in-process request from a
 * handler for exactly that reason. What the limits must guarantee is that the
 * attempt costs them nothing -- whether the abandoned connection is dispatched
 * once the handler returns or dropped when its client gives up, the receive
 * budget is released and the connection slot is reclaimed, so ordinary traffic
 * of the same full size is served immediately afterwards. Which of those two
 * the socket wins is a race, so it is bounded rather than pinned. */
static void test_nested_dispatch(Fixture *f, gconstpointer data)
{
	g_autofree gchar *body = g_strnfill(600000, 'x'), *request = NULL, *response = NULL;
	guint settled;
	(void)data;
	request = g_strdup_printf("POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 600000\r\nConnection: close\r\n\r\n%s", body);
	f->nested_request = request;
	response = exchange(f, request);
	/* The outer request is unharmed by what its route attempted, and the
	 * blocked route got nothing back inside its own bounded second. */
	g_assert_nonnull(strstr(response, " 200 "));
	g_assert_nonnull(f->nested_response);
	g_assert_cmpstr(f->nested_response, ==, "");
	pump();
	settled = f->writes;
	g_assert_cmpuint(settled, >=, 1);
	g_assert_cmpuint(settled, <=, 2);
	/* The receive budget and the connection slots survived the abandoned
	 * attempt: a further full-size request is served, and exactly once. */
	g_clear_pointer(&response, g_free);
	response = exchange(f, request);
	g_assert_nonnull(strstr(response, " 200 "));
	g_assert_cmpuint(f->writes, ==, settled + 1);
}
static void test_active_teardown(Fixture *f, gconstpointer data)
{
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GError) error = NULL;
	gpointer weak;
	gchar byte;
	(void)data;
	g_socket_client_set_timeout(client, 3);
	connection = g_socket_client_connect_to_host(client, "127.0.0.1", (guint16)f->port, NULL, &error); g_assert_no_error(error);
	pump();
	weak = f->server; g_object_add_weak_pointer(G_OBJECT(f->server), &weak);
	venture_web_server_stop(f->server); g_clear_object(&f->server); pump();
	g_assert_null(weak);
	g_assert_cmpint(g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(connection)), &byte, 1, NULL, &error), ==, 0); g_assert_no_error(error);
}
static void test_negative_length(Fixture *f, gconstpointer data)
{
	g_autofree gchar *response = exchange(f, "POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\nConnection: close\r\n\r\n");
	(void)data;
	g_assert_nonnull(strstr(response, " 400 ")); g_assert_cmpuint(f->writes, ==, 0);
}

static void test_invalid(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TENANT_WORKSPACE);
	g_autoptr(GPtrArray) records = NULL;
	static const struct { const gchar *property; gint64 value; } invalid[] = {
		{ "server-max-request-size-mb", 0 }, { "server-request-timeout", 0 },
		{ "server-max-connections", 0 }, { "server-max-buffered-request-mb", 1 },
		{ "server-max-buffered-request-mb", G_MAXINT64 },
		{ "hosted-http-requests-per-minute", 0 }, { "hosted-http-requests-per-minute", 1000001 },
		{ "hosted-http-burst", 0 }, { "hosted-http-burst", 1000001 },
		{ "hosted-http-concurrency", 0 }, { "hosted-http-concurrency", 257 }
	};
	guint i;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	g_object_set(config, "hosted-enabled", TRUE,
		"hosted-workspace-id", "0c505b10-4160-4e35-bda6-dd073544caed", "hosted-origin", "https://limits.example.test", NULL);
	context = venture_context_new(config, database);
	for (i = 0; i < G_N_ELEMENTS(invalid); i++)
	{
		gint64 original;
		g_object_get(config, invalid[i].property, &original, NULL);
		g_object_set(config, invalid[i].property, invalid[i].value, NULL);
		server = venture_web_server_new(context, &error);
		g_assert_null(server); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); g_clear_error(&error);
		g_object_set(config, invalid[i].property, original, NULL);
	}
	records = venture_database_find(database, query, &error); g_assert_no_error(error); g_assert_cmpuint(records->len, ==, 0);
}

static void test_private_context(void)
{
	g_autoptr(GMainContext) owner = g_main_context_new();
	Fixture f;
	g_autofree gchar *response = NULL;
	memset(&f, 0, sizeof(f));
	g_main_context_push_thread_default(owner); setup(&f, NULL); g_main_context_pop_thread_default(owner);
	/* Dispatching a private context does not push it as thread-default. The
	 * transport must retain the listener's owner for timers and async close. */
	response = exchange(&f, "POST /fixture/write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\na");
	g_assert_nonnull(strstr(response, " 408 ")); g_assert_cmpuint(f.writes, ==, 0);
	teardown(&f, NULL);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/http-limits/declared", Fixture, NULL, setup, test_declared, teardown);
	g_test_add("/http-limits/ordinary-chunked", Fixture, NULL, setup, test_ordinary, teardown);
	g_test_add("/http-limits/chunked-overflow", Fixture, NULL, setup, test_chunked_overflow, teardown);
	g_test_add("/http-limits/slow-body", Fixture, NULL, setup, test_slow_body, teardown);
	g_test_add("/http-limits/slow-headers", Fixture, NULL, setup, test_slow_headers, teardown);
	g_test_add("/http-limits/idle", Fixture, NULL, setup, test_idle, teardown);
	g_test_add("/http-limits/idle-after-error", Fixture, NULL, setup, test_idle_after_error, teardown);
	g_test_add_func("/http-limits/invalid-before-binding", test_invalid);
	g_test_add("/http-limits/tls-ordinary", Fixture, GINT_TO_POINTER(1), setup, test_ordinary, teardown);
	g_test_add("/http-limits/tls-declared", Fixture, GINT_TO_POINTER(1), setup, test_declared, teardown);
	g_test_add("/http-limits/tls-slow-body", Fixture, GINT_TO_POINTER(1), setup, test_slow_body, teardown);
	g_test_add("/http-limits/connections-and-neighbor", Fixture, GINT_TO_POINTER(2), setup, test_connections, teardown);
	g_test_add("/http-limits/tls-handshake-stall", Fixture, GINT_TO_POINTER(1), setup, test_tls_stall, teardown);
	g_test_add("/http-limits/generic-record", Fixture, NULL, setup, test_generic, teardown);
	g_test_add("/http-limits/aggregate-release", Fixture, GINT_TO_POINTER(2), setup, test_aggregate, teardown);
	g_test_add("/http-limits/timeout-budget-release", Fixture, NULL, setup, test_timeout_budget, teardown);
	g_test_add("/http-limits/nested-dispatch", Fixture, NULL, setup, test_nested_dispatch, teardown);
	g_test_add("/http-limits/active-teardown", Fixture, NULL, setup, test_active_teardown, teardown);
	g_test_add("/http-limits/negative-length", Fixture, NULL, setup, test_negative_length, teardown);
	g_test_add_func("/http-limits/private-context", test_private_context);
	return g_test_run();
}
