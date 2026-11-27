/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* A Stripe call that does not answer must hold up its own request and no
 * other. These send real HTTP requests, each from a thread of its own,
 * while the main thread runs the server. The fake provider holds a call
 * until the test releases it, so what is asserted is what was answered
 * while a call was held -- never how long anything took, which a loaded
 * build machine would make flaky. */

/* Runs the main loop until @done says so. A ticker keeps the loop waking
 * so a condition another thread changes is seen; the give-up is a hang
 * guard for a regression, not a measurement. */
static gboolean
keep_ticking(gpointer data)
{
	(void)data;
	return G_SOURCE_CONTINUE;
}

static void
run_until(gboolean (*done)(gpointer), gpointer data)
{
	gint64 give_up = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	guint ticker = g_timeout_add(20, keep_ticking, NULL);
	while (!done(data)) {
		g_assert_cmpint(g_get_monotonic_time(), <, give_up);
		g_main_context_iteration(NULL, TRUE);
	}
	g_source_remove(ticker);
}

/* --- The provider that waits for the test ---------------------------------- */

/* Holds the next @calls Stripe calls until each is released. */
static void
fake_hold(FakeTransport *transport, gint calls)
{
	g_mutex_lock(&transport->hold_lock);
	transport->holding = TRUE;
	transport->hold_limit = calls;
	transport->held = 0;
	transport->released = 0;
	g_mutex_unlock(&transport->hold_lock);
}

/* Lets the call that took @ticket answer. */
static void
fake_release(FakeTransport *transport, gint ticket)
{
	g_mutex_lock(&transport->hold_lock);
	transport->released |= 1u << ticket;
	g_cond_broadcast(&transport->hold_cond);
	g_mutex_unlock(&transport->hold_lock);
}

/* Lets every held call answer, and every later one pass. */
static void
fake_release_all(FakeTransport *transport)
{
	g_mutex_lock(&transport->hold_lock);
	transport->holding = FALSE;
	transport->hold_times_out = FALSE;
	g_cond_broadcast(&transport->hold_cond);
	g_mutex_unlock(&transport->hold_lock);
}

static gint
fake_held(FakeTransport *transport)
{
	gint held;
	g_mutex_lock(&transport->hold_lock);
	held = transport->held;
	g_mutex_unlock(&transport->hold_lock);
	return held;
}

typedef struct { FakeTransport *transport; gint calls; } HeldWait;

static gboolean
held_reached(gpointer data)
{
	HeldWait *wait = data;
	return fake_held(wait->transport) >= wait->calls;
}

/* Serves until @calls calls are being held. */
static void
fake_wait_held(FakeTransport *transport, gint calls)
{
	HeldWait wait = { transport, calls };
	run_until(held_reached, &wait);
}

/* --- Clients on threads of their own --------------------------------------- */

typedef struct {
	const gchar *method, *content_type, *body, *host, *origin, *auth;
	gchar *url;
	GCancellable *cancel;	/* set: the client may give up; a failure is expected */
	guint status;
	gchar *reply;
	gint done;
	GThread *thread;
} SlowCall;

static gpointer
slow_call_run(gpointer data)
{
	SlowCall *call = data;
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 60, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(call->method, call->url);
	g_autoptr(GBytes) reply = NULL;
	g_autoptr(GError) error = NULL;
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (call->host) soup_message_headers_replace(headers, "Host", call->host);
	if (call->origin) soup_message_headers_replace(headers, "Origin", call->origin);
	if (call->auth) soup_message_headers_replace(headers, "Authorization", call->auth);
	if (call->body) {
		g_autoptr(GBytes) payload = g_bytes_new(call->body, strlen(call->body));
		soup_message_set_request_body_from_bytes(message, call->content_type, payload);
	}
	reply = soup_session_send_and_read(session, message, call->cancel, &error);
	if (!call->cancel)
		g_assert_no_error(error);
	call->status = soup_message_get_status(message);
	if (reply)
		call->reply = g_strndup(g_bytes_get_data(reply, NULL), g_bytes_get_size(reply));
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

static gboolean
slow_call_answered(gpointer data)
{
	return g_atomic_int_get(&((SlowCall *)data)->done);
}

/* Serves until @call has its answer, then joins its thread. */
static void
slow_call_finish(SlowCall *call)
{
	run_until(slow_call_answered, call);
	g_thread_join(call->thread);
	call->thread = NULL;
	g_clear_pointer(&call->url, g_free);
}

static void
slow_call_clear(SlowCall *call)
{
	g_clear_pointer(&call->reply, g_free);
	g_clear_object(&call->cancel);
}

/*
 * @method @path on @server waits on @transport's next call; a GET of
 * @probe sent while that call is held is answered 200 before the route is,
 * and the route, once the call is released, answers @expected.
 */
static void
assert_route_unblocked_by(VentureWebServer *server, FakeTransport *transport, const gchar *method,
	const gchar *path, const gchar *content_type, const gchar *body, const gchar *host,
	const gchar *origin, const gchar *auth, guint expected, const gchar *probe)
{
	SlowCall route = { 0 }, health = { 0 };
	route.method = method; route.content_type = content_type; route.body = body;
	route.host = host; route.origin = origin; route.auth = auth;
	health.host = host;
	fake_hold(transport, 1);
	slow_call_start(&route, server, path);
	/* A route that never reaches the provider fails here, not by hanging. */
	fake_wait_held(transport, 1);
	slow_call_start(&health, server, probe);
	slow_call_finish(&health);
	g_test_message("%s %s held on Stripe; GET %s answered %u meanwhile", method, path, probe, health.status);
	g_assert_cmpuint(health.status, ==, 200);
	g_assert_false(g_atomic_int_get(&route.done));
	fake_release_all(transport);
	slow_call_finish(&route);
	g_assert_cmpuint(route.status, ==, expected);
	slow_call_clear(&route);
	slow_call_clear(&health);
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
