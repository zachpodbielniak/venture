/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A Stripe call that does not answer must hold up its own request and no
 * other. These send real HTTP requests, each from a thread of its own so
 * its clock is not held up by the main loop it is measuring, while the
 * main thread runs the server. */

typedef struct {
	const gchar *method, *content_type, *body, *host, *origin, *auth;
	gchar *url;
	FakeTransport *wait_for;	/* send once this many of its calls stall */
	gint wait_stalls;
	guint status;
	gint64 started, ended;
	gint done;
	GThread *thread;
} SlowCall;

static gpointer
slow_call_run(gpointer data)
{
	SlowCall *call = data;
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 30, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(call->method, call->url);
	g_autoptr(GBytes) reply = NULL;
	g_autoptr(GError) error = NULL;
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	gint64 give_up = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
	while (call->wait_for && g_atomic_int_get(&call->wait_for->stalled) < call->wait_stalls) {
		/* A route that never reaches the provider fails here, not by hanging. */
		g_assert_cmpint(g_get_monotonic_time(), <, give_up);
		g_usleep(10000);
	}
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (call->host) soup_message_headers_replace(headers, "Host", call->host);
	if (call->origin) soup_message_headers_replace(headers, "Origin", call->origin);
	if (call->auth) soup_message_headers_replace(headers, "Authorization", call->auth);
	if (call->body) {
		g_autoptr(GBytes) payload = g_bytes_new(call->body, strlen(call->body));
		soup_message_set_request_body_from_bytes(message, call->content_type, payload);
	}
	call->started = g_get_monotonic_time();
	reply = soup_session_send_and_read(session, message, NULL, &error);
	call->ended = g_get_monotonic_time();
	g_assert_no_error(error);
	call->status = soup_message_get_status(message);
	g_atomic_int_set(&call->done, 1);
	g_main_context_wakeup(NULL);
	return NULL;
}

static void
slow_call_start(SlowCall *call, VentureWebServer *server, const gchar *path)
{
	call->url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	if (!call->method) call->method = call->body ? "POST" : "GET";
	call->thread = g_thread_new("slow-call", slow_call_run, call);
}

/* Runs the server until every call has its answer. The loop sleeps in
 * poll() as a server's does, which is where the read workers' gate opens;
 * each call wakes it when it is answered. */
static void
slow_calls_finish(SlowCall *const *calls, guint n)
{
	guint i, done = 0;
	for (;;) {
		for (done = 0, i = 0; i < n; i++) done += g_atomic_int_get(&calls[i]->done) ? 1 : 0;
		if (done == n) break;
		g_main_context_iteration(NULL, TRUE);
	}
	for (i = 0; i < n; i++) {
		g_thread_join(calls[i]->thread);
		g_clear_pointer(&calls[i]->url, g_free);
	}
}

static gint64
slow_call_ms(const SlowCall *call)
{
	return (call->ended - call->started) / 1000;
}

/* @transport's next call waits each of @stalls_us in turn (0 ends it). */
static void
slow_provider(FakeTransport *transport, guint first_us, guint second_us)
{
	memset(transport->stalls_us, 0, sizeof transport->stalls_us);
	transport->stalls_us[0] = first_us;
	transport->stalls_us[1] = second_us;
	g_atomic_int_set(&transport->stall_next, 0);
	g_atomic_int_set(&transport->stalled, 0);
}

/*
 * @method @path on @server waits two seconds on @transport's next call; a
 * GET of @probe sent while it waits is answered 200 in under one second,
 * and the route then answers @expected.
 */
static void
assert_route_unblocked_by(VentureWebServer *server, FakeTransport *transport, const gchar *method,
	const gchar *path, const gchar *content_type, const gchar *body, const gchar *host,
	const gchar *origin, const gchar *auth, guint expected, const gchar *probe)
{
	SlowCall route = { 0 }, health = { 0 };
	SlowCall *calls[2];
	route.method = method; route.content_type = content_type; route.body = body;
	route.host = host; route.origin = origin; route.auth = auth;
	health.host = host; health.wait_for = transport; health.wait_stalls = 1;
	slow_provider(transport, 2 * G_USEC_PER_SEC, 0);
	slow_call_start(&route, server, path);
	slow_call_start(&health, server, probe);
	calls[0] = &route; calls[1] = &health;
	slow_calls_finish(calls, 2);
	g_test_message("%s %s: %u after %" G_GINT64_FORMAT " ms; GET %s: %u after %" G_GINT64_FORMAT " ms",
		method, path, route.status, slow_call_ms(&route), probe, health.status, slow_call_ms(&health));
	g_assert_cmpuint(health.status, ==, 200);
	g_assert_cmpint(health.ended - health.started, <, G_USEC_PER_SEC);
	g_assert_cmpint(health.ended, <, route.ended);
	g_assert_cmpuint(route.status, ==, expected);
	slow_provider(transport, 0, 0);
}

/* The same, probed with a health check. */
static void
assert_route_unblocked(VentureWebServer *server, FakeTransport *transport, const gchar *method,
	const gchar *path, const gchar *content_type, const gchar *body, const gchar *host,
	const gchar *origin, const gchar *auth, guint expected)
{
	assert_route_unblocked_by(server, transport, method, path, content_type, body, host, origin, auth,
		expected, "/api/v1/health");
}
