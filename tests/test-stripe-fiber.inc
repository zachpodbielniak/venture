/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The Stripe transport as a fiber sees it: retries that pause yielded,
 * the request deadline, and the transaction that keeps a fiber blocking.
 * Stripe here is either the offline FakeTransport (held until released)
 * or a stub HTTP server on a thread of its own, reached through VENTURE's
 * own network send with stripe-glib's test_base_url. */

/* --- A stub Stripe on its own thread --------------------------------------- */

typedef struct {
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	SoupServer *server;
	guint16 port;
	gint ready;
	gboolean never;			/* never answer; answered 503 at stop */
	const gchar *retry_after;	/* the first answer is 429 with this */
	GMutex lock;
	gint requests;
	gint64 first_us, second_us;	/* when the first two arrived */
	GPtrArray *paused;
} StripeStub;

static void
stub_handle(SoupServer *server, SoupServerMessage *message, const char *path,
	GHashTable *query, gpointer data)
{
	StripeStub *stub = data;
	gint request;
	(void)server; (void)path; (void)query;
	g_mutex_lock(&stub->lock);
	request = ++stub->requests;
	if (1 == request) stub->first_us = g_get_monotonic_time();
	if (2 == request) stub->second_us = g_get_monotonic_time();
	g_mutex_unlock(&stub->lock);
	g_main_context_wakeup(NULL);
	if (stub->never) {
		soup_server_message_pause(message);
		g_ptr_array_add(stub->paused, g_object_ref(message));
		return;
	}
	if (1 == request && stub->retry_after) {
		soup_message_headers_replace(soup_server_message_get_response_headers(message), "Retry-After", stub->retry_after);
		soup_message_headers_replace(soup_server_message_get_response_headers(message), "Request-Id", "req_stubRetry");
		soup_server_message_set_response(message, "application/json", SOUP_MEMORY_STATIC,
			"{\"error\":{\"type\":\"rate_limit_error\"}}", strlen("{\"error\":{\"type\":\"rate_limit_error\"}}"));
		soup_server_message_set_status(message, 429, NULL);
		return;
	}
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_STATIC,
		"{\"id\":\"acct_stub\",\"object\":\"account\"}", strlen("{\"id\":\"acct_stub\",\"object\":\"account\"}"));
	soup_server_message_set_status(message, 200, NULL);
}

static gpointer
stub_run(gpointer data)
{
	StripeStub *stub = data;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	guint i;
	g_main_context_push_thread_default(stub->context);
	stub->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(stub->server, NULL, stub_handle, stub, NULL);
	g_assert_true(soup_server_listen_local(stub->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(stub->server);
	stub->port = (guint16)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	g_atomic_int_set(&stub->ready, 1);
	g_main_loop_run(stub->loop);
	for (i = 0; i < stub->paused->len; i++) {
		SoupServerMessage *held = g_ptr_array_index(stub->paused, i);
		soup_server_message_set_status(held, 503, NULL);
		soup_server_message_unpause(held);
	}
	g_ptr_array_set_size(stub->paused, 0);
	soup_server_disconnect(stub->server);
	g_clear_object(&stub->server);
	g_main_context_pop_thread_default(stub->context);
	return NULL;
}

static void
stub_start(StripeStub *stub, gboolean never, const gchar *retry_after)
{
	memset(stub, 0, sizeof *stub);
	g_mutex_init(&stub->lock);
	stub->never = never;
	stub->retry_after = retry_after;
	stub->paused = g_ptr_array_new_with_free_func(g_object_unref);
	stub->context = g_main_context_new();
	stub->loop = g_main_loop_new(stub->context, FALSE);
	stub->thread = g_thread_new("stripe-stub", stub_run, stub);
	while (!g_atomic_int_get(&stub->ready)) g_usleep(1000);
}

static gboolean
stub_quit(gpointer data)
{
	g_main_loop_quit(((StripeStub *)data)->loop);
	return G_SOURCE_REMOVE;
}

static void
stub_stop(StripeStub *stub)
{
	g_main_context_invoke(stub->context, stub_quit, stub);
	g_thread_join(stub->thread);
	g_main_loop_unref(stub->loop);
	g_main_context_unref(stub->context);
	g_ptr_array_unref(stub->paused);
	g_mutex_clear(&stub->lock);
}

static gint
stub_requests(StripeStub *stub)
{
	gint requests;
	g_mutex_lock(&stub->lock);
	requests = stub->requests;
	g_mutex_unlock(&stub->lock);
	return requests;
}

/* A client the way VENTURE makes every one (venture_stripe_client_new()),
 * talking to @stub through the network send, or to @fake when given. */
static StripeClient *
stub_client(Fixture *f, StripeStub *stub, guint timeout, FakeTransport *fake)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = stub
		? g_strdup_printf("secret_key: 'sk_test_stub'\npublishable_key: 'pk_test_stub'\nwebhook_signing_secrets: ['whsec_stub']\n"
			"api_version: '2024-06-20'\ntest_base_url: 'http://127.0.0.1:%u'\nrequest_timeout: %u\n", stub->port, timeout)
		: g_strdup_printf("secret_key: 'sk_test_stub'\npublishable_key: 'pk_test_stub'\nwebhook_signing_secrets: ['whsec_stub']\n"
			"api_version: '2024-06-20'\nrequest_timeout: %u\n", timeout);
	StripeClient *client = venture_stripe_client_new(f->database, yaml, fake ? STRIPE_TRANSPORT(fake) : NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(client);
	return client;
}

/* stripe.request.retried as the audit handler sees it. */
typedef struct { guint seen; JsonObject *last; } Retried;

static void
count_retried(StripeClient *client, const gchar *name, JsonObject *payload, gpointer data)
{
	Retried *retried = data;
	(void)client;
	if (g_strcmp0(name, "stripe.request.retried")) return;
	retried->seen++;
	g_clear_pointer(&retried->last, json_object_unref);
	retried->last = json_object_ref(payload);
}

/* --- One Stripe call in a fiber -------------------------------------------- */

typedef struct {
	StripeClient *client;
	VentureDatabase *transaction;	/* set: the call is made inside one */
	StripeResource *result;
	GError *error;
	gboolean done;
	gboolean probed;		/* a main-loop probe ran before it finished */
	gboolean probed_before_done;
} FiberCall;

static void
fiber_call_run(gpointer data)
{
	FiberCall *call = data;
	g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_ACCOUNT_RETRIEVE);
	if (call->transaction) g_assert_true(venture_database_begin(call->transaction, NULL));
	call->result = venture_stripe_execute(call->client, request, NULL, &call->error);
	if (call->transaction) venture_database_rollback(call->transaction);
}

static void
fiber_call_done(gpointer data)
{
	FiberCall *call = data;
	call->probed_before_done = call->probed;
	call->done = TRUE;
}

static gboolean
fiber_call_finished(gpointer data)
{
	return ((FiberCall *)data)->done;
}

static gboolean
fiber_call_probe(gpointer data)
{
	((FiberCall *)data)->probed = TRUE;
	return G_SOURCE_REMOVE;
}

static void
fiber_call_clear(FiberCall *call)
{
	g_clear_pointer(&call->result, stripe_resource_free);
	g_clear_error(&call->error);
	g_clear_object(&call->client);
}

/* Starts @call as a fiber; returns whether it finished without yielding. */
static gboolean
fiber_call_start(FiberCall *call)
{
	VentureFiber *fiber;
	venture_fiber_init();
	fiber = venture_fiber_new(fiber_call_run, fiber_call_done, NULL, call);
	g_assert_nonnull(fiber);
	venture_fiber_wake(fiber);
	return call->done;
}

static gboolean
stub_saw_one(gpointer data)
{
	return stub_requests(data) >= 1;
}

/*
 * Stripe answers 429 with Retry-After: 2. stripe-glib would sleep those two
 * seconds on the caller's stack -- the main thread, the whole server. The
 * transport pauses yielded instead: a health check sent during the pause
 * is answered before the retry is even sent, and the retry comes no sooner
 * than Stripe asked. A plain caller still gets its retry, asleep.
 */
static void
test_fiber_retry_after(Fixture *f, gconstpointer data)
{
	StripeStub stub;
	FiberCall call = { 0 };
	SlowCall health = { 0 };
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_dir = NULL;
	(void)data;
	Retried retried = { 0, NULL };
	stub_start(&stub, FALSE, "2");
	server = start_server(f, &state_dir);
	call.client = stub_client(f, &stub, 30, NULL);
	g_signal_connect(call.client, "event", G_CALLBACK(count_retried), &retried);
	g_assert_false(fiber_call_start(&call));
	run_until(stub_saw_one, &stub);
	slow_call_start(&health, server, "/api/v1/health");
	slow_call_finish(&health);
	g_assert_cmpuint(health.status, ==, 200);
	/* Answered within the pause: the retry has not been sent yet. */
	g_assert_cmpint(stub_requests(&stub), ==, 1);
	g_assert_false(call.done);
	run_until(fiber_call_finished, &call);
	g_assert_no_error(call.error);
	g_assert_nonnull(call.result);
	g_assert_cmpstr(stripe_resource_get_string(call.result, "id"), ==, "acct_stub");
	g_assert_cmpint(stub_requests(&stub), ==, 2);
	/* A lower bound only: load can make it later, never sooner. */
	g_assert_cmpint(stub.second_us - stub.first_us, >=, 2 * G_USEC_PER_SEC);
	/* Announced as stripe-glib announced its own retries, once. */
	g_assert_cmpuint(retried.seen, ==, 1);
	g_assert_cmpint(json_object_get_int_member(retried.last, "operation"), ==, STRIPE_ACCOUNT_RETRIEVE);
	g_assert_cmpint(json_object_get_int_member(retried.last, "status"), ==, 429);
	g_assert_cmpint(json_object_get_int_member(retried.last, "attempt"), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(retried.last, "request_id"), ==, "req_stubRetry");
	g_assert_true(json_object_has_member(retried.last, "duration_ms"));
	g_assert_true(json_object_has_member(retried.last, "response_bytes"));
	g_clear_pointer(&retried.last, json_object_unref);
	fiber_call_clear(&call);
	slow_call_clear(&health);
	stub_stop(&stub);

	/* On a plain stack: the same retry, slept through. */
	stub_start(&stub, FALSE, "1");
	{
		g_autoptr(StripeClient) client = stub_client(f, &stub, 30, NULL);
		g_autoptr(StripeRequest) request = stripe_request_new(STRIPE_ACCOUNT_RETRIEVE);
		g_autoptr(GError) error = NULL;
		g_autoptr(StripeResource) result = venture_stripe_execute(client, request, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
		g_assert_cmpint(stub_requests(&stub), ==, 2);
		g_assert_cmpint(stub.second_us - stub.first_us, >=, G_USEC_PER_SEC);
	}
	stub_stop(&stub);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
}

/* A Retry-After that cannot fit before the deadline ends the call at once,
 * as a timeout that keeps the operation's key, rather than a pause that
 * would outlive it. Stripe is asked once. */
static void
test_fiber_retry_budget(Fixture *f, gconstpointer data)
{
	StripeStub stub;
	FiberCall call = { 0 };
	(void)data;
	stub_start(&stub, FALSE, "5");
	call.client = stub_client(f, &stub, 2, NULL);
	if (!fiber_call_start(&call))
		run_until(fiber_call_finished, &call);
	g_assert_null(call.result);
	g_assert_error(call.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
	g_assert_cmpint(stub_requests(&stub), ==, 1);
	fiber_call_clear(&call);
	stub_stop(&stub);
}

/* Stripe never answers. The request's own deadline (request_timeout, here
 * one second) ends the call -- the network send's timer, not a fake's
 * answer -- and the main loop served others all the while. */
static void
test_fiber_deadline(Fixture *f, gconstpointer data)
{
	StripeStub stub;
	FiberCall call = { 0 };
	(void)data;
	stub_start(&stub, TRUE, NULL);
	call.client = stub_client(f, &stub, 1, NULL);
	g_assert_false(fiber_call_start(&call));
	g_idle_add(fiber_call_probe, &call);
	run_until(fiber_call_finished, &call);
	g_assert_true(call.probed_before_done);
	g_assert_null(call.result);
	g_assert_error(call.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
	g_assert_cmpint(stub_requests(&stub), ==, 1);
	fiber_call_clear(&call);
	stub_stop(&stub);
}

typedef struct { FakeTransport *fake; gint seen; } Releaser;

/* Off the main thread, which is blocked: releases the held call once it
 * is being held. */
static gpointer
release_when_held(gpointer data)
{
	Releaser *releaser = data;
	gint64 give_up = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
	while (fake_held(releaser->fake) < 1) {
		g_assert_cmpint(g_get_monotonic_time(), <, give_up);
		g_usleep(1000);
	}
	releaser->seen = 1;
	fake_release_all(releaser->fake);
	return NULL;
}

/*
 * With a transaction open, a fiber's Stripe call does not yield: whatever
 * the loop served meanwhile would write inside that transaction. It blocks
 * on its own stack exactly as a plain caller does -- the wake returns only
 * once the call is over. Without one, the same call yields.
 */
static void
test_fiber_transaction_blocks(Fixture *f, gconstpointer data)
{
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	FiberCall call = { 0 };
	Releaser releaser = { fake, 0 };
	GThread *thread;
	(void)data;
	fake_hold(fake, 1);
	thread = g_thread_new("release", release_when_held, &releaser);
	call.client = stub_client(f, NULL, 30, fake);
	call.transaction = f->database;
	g_assert_true(fiber_call_start(&call));
	g_thread_join(thread);
	g_assert_cmpint(releaser.seen, ==, 1);
	g_assert_no_error(call.error);
	g_assert_nonnull(call.result);
	fiber_call_clear(&call);

	/* No transaction: it yields, and is woken once released. The first
	 * half's transaction must be cleared, or this call blocks on its own
	 * stack with nobody left to release it. */
	fake_hold(fake, 1);
	call.client = stub_client(f, NULL, 30, fake);
	call.transaction = NULL;
	call.done = FALSE;
	g_assert_false(fiber_call_start(&call));
	fake_wait_held(fake, 1);
	g_assert_false(call.done);
	fake_release_all(fake);
	run_until(fiber_call_finished, &call);
	g_assert_no_error(call.error);
	g_assert_nonnull(call.result);
	fiber_call_clear(&call);
}

/* --- Wakes that land inside somebody else's transaction -------------------- */

static void
nested_done(gpointer data)
{
	FiberCall *call = data;
	call->done = TRUE;
}

/*
 * A nested main loop (a bank feed sync, a webhook test) may run with a
 * transaction open, and it dispatches the answer's wake. The fiber is not
 * resumed there: it would write into that transaction. It is resumed once
 * the transaction has ended.
 */
static void
test_fiber_wake_waits_for_transaction(Fixture *f, gconstpointer data)
{
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	FiberCall call = { 0 };
	VentureFiber *fiber;
	gint64 give_up;
	(void)data;
	venture_fiber_init();
	fake_hold(fake, 1);
	call.client = stub_client(f, NULL, 30, fake);
	fiber = venture_fiber_new(fiber_call_run, nested_done, NULL, &call);
	venture_fiber_wake(fiber);
	g_assert_false(call.done);
	fake_wait_held(fake, 1);
	/* Somebody else's transaction, with a loop running inside it. */
	g_assert_true(venture_database_begin(f->database, NULL));
	fake_release_all(fake);
	give_up = g_get_monotonic_time() + G_USEC_PER_SEC / 2;
	while (g_get_monotonic_time() < give_up)
		g_main_context_iteration(NULL, FALSE);
	g_assert_false(call.done);
	venture_database_rollback(f->database);
	run_until(fiber_call_finished, &call);
	g_assert_no_error(call.error);
	g_assert_nonnull(call.result);
	fiber_call_clear(&call);
}

/* A fiber is counted from its making to its end, which is what a stop
 * waits on. */
static void
test_fiber_count(Fixture *f, gconstpointer data)
{
	g_autoptr(GObject) transport = g_object_new(fake_transport_get_type(), NULL);
	FakeTransport *fake = (FakeTransport *)transport;
	FiberCall call = { 0 };
	guint before;
	(void)data;
	venture_fiber_init();
	before = venture_fiber_count();
	fake_hold(fake, 1);
	call.client = stub_client(f, NULL, 30, fake);
	g_assert_false(fiber_call_start(&call));
	g_assert_cmpuint(venture_fiber_count(), ==, before + 1);
	fake_wait_held(fake, 1);
	fake_release_all(fake);
	run_until(fiber_call_finished, &call);
	g_assert_cmpuint(venture_fiber_count(), ==, before);
	fiber_call_clear(&call);
}

static gboolean
no_fibers(gpointer data)
{
	(void)data;
	return 0 == venture_fiber_count();
}

/* --- Who a fiber is, while it waits ---------------------------------------- */

/* A user with an API token, a member of @organization with @role. */
static gchar *
fiber_member_token(Fixture *f, const gchar *username, gint64 organization, VentureOrganizationRole role, gint64 *token_id)
{
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", username, "active", TRUE,
		"role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	save(f, user);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(user),
		"organization-id", organization, "active", TRUE, "role", role, NULL);
	save(f, member);
	g_object_set(token, "name", username, "user-id", venture_entity_get_id(user), "role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token);
	save(f, VENTURE_ENTITY(token));
	*token_id = venture_entity_get_id(VENTURE_ENTITY(token));
	return secret;
}

static VentureWebServer *
start_server_signed_in(Fixture *f, gchar **state_dir)
{
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	*state_dir = g_dir_make_tmp("venture-stripe-XXXXXX", &error);
	g_assert_no_error(error);
	g_object_set(f->config, "state-dir", *state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", TRUE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

/*
 * While one user's checkout is suspended on Stripe, the main loop serves
 * others under their own identity, not the fiber's: an anonymous read of a
 * signed-in route is refused, a second user from another business reads
 * that business's records only, and the fiber's own writes after the wait
 * still carry its user as their audit actor.
 */
static void
test_fiber_isolation(Fixture *f, VentureStripeService *service, FakeTransport *fake, VentureEntity *invoice)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) business = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Second business", "slug", "second", NULL);
	g_autoptr(VentureCompany) theirs = venture_company_new();
	g_autoptr(GPtrArray) audits = NULL;
	g_autofree gchar *state_dir = NULL, *first = NULL, *second = NULL, *first_auth = NULL, *second_auth = NULL;
	g_autofree gchar *api = g_strdup_printf("/api/v1/invoices/%" G_GINT64_FORMAT "/checkout", venture_entity_get_id(invoice));
	g_autofree gchar *first_actor = NULL;
	g_autofree gchar *their_path = NULL, *our_path = NULL;
	SlowCall route = { 0 }, anonymous = { 0 }, other = { 0 }, across = { 0 };
	gint64 first_token = 0, second_token = 0;
	guint i, mine = 0;
	/* The provider as production builds one, from the business's sealed
	 * connection; only its HTTP goes to the fake. */
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(JsonNode) settings = binding_settings("whsec_original");
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(VentureStripeService) provider = NULL;
	(void)service;
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->database), key, &error));
	connection = venture_stripe_settings_configure(f->database, f->organization_id, settings, 0, 0, STRIPE_TRANSPORT(fake), NULL, &error);
	g_assert_no_error(error);
	provider = venture_stripe_service_for_organization(f->database, f->organization_id, STRIPE_TRANSPORT(fake), &error);
	g_assert_no_error(error);
	save(f, business);
	g_object_set(theirs, "name", "Their customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(theirs), venture_entity_get_id(business));
	save(f, VENTURE_ENTITY(theirs));
	first = fiber_member_token(f, "finance-first", f->organization_id, VENTURE_ORGANIZATION_ROLE_FINANCE, &first_token);
	second = fiber_member_token(f, "editor-second", venture_entity_get_id(business), VENTURE_ORGANIZATION_ROLE_EDITOR, &second_token);
	first_auth = g_strconcat("Bearer ", first, NULL);
	second_auth = g_strconcat("Bearer ", second, NULL);
	first_actor = g_strdup_printf("API token #%" G_GINT64_FORMAT, first_token);
	venture_context_set_stripe_service(f->context, provider);
	server = start_server_signed_in(f, &state_dir);

	route.method = "POST"; route.content_type = "application/json"; route.body = "{}"; route.auth = first_auth;
	fake_hold(fake, 1);
	slow_call_start(&route, server, api);
	fake_wait_held(fake, 1);
	/* Nobody signed in: refused, though the suspended fiber is somebody. */
	slow_call_start(&anonymous, server, "/api/v1/company");
	slow_call_finish(&anonymous);
	g_assert_cmpuint(anonymous.status, ==, 401);
	/* Another business's member: its own records, none of the first's,
	 * and not the first business's when it asks for them. */
	their_path = g_strdup_printf("/api/v1/company?organization_id=%" G_GINT64_FORMAT, venture_entity_get_id(business));
	our_path = g_strdup_printf("/api/v1/company?organization_id=%" G_GINT64_FORMAT, f->organization_id);
	other.auth = second_auth;
	slow_call_start(&other, server, their_path);
	slow_call_finish(&other);
	g_test_message("Second business's read during the wait: %u %s", other.status, other.reply);
	g_assert_cmpuint(other.status, ==, 200);
	g_assert_nonnull(strstr(other.reply, "Their customer"));
	g_assert_null(strstr(other.reply, "\"Customer\""));
	across.auth = second_auth;
	slow_call_start(&across, server, our_path);
	slow_call_finish(&across);
	g_test_message("Second business asking for the first's: %u", across.status);
	g_assert_true(across.status != 200 || !strstr(across.reply, "\"Customer\""));
	g_assert_false(g_atomic_int_get(&route.done));
	fake_release_all(fake);
	slow_call_finish(&route);
	g_assert_cmpuint(route.status, ==, 200);

	/* The checkout's records, written before and after the wait, are the
	 * first user's: the second user's request ran in between. */
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(f->database), NULL);
		audits = rows(f, "audit_entry");
	}
	for (i = 0; i < audits->len; i++) {
		g_autofree gchar *target = NULL, *actor = NULL;
		g_object_get(g_ptr_array_index(audits, i), "target-type", &target, "actor", &actor, NULL);
		if (g_strcmp0(target, "stripe_checkout")) continue;
		g_assert_cmpstr(actor, ==, first_actor);
		mine++;
	}
	g_test_message("%u checkout audit entries, every one by %s", mine, first_actor);
	g_assert_cmpuint(mine, >=, 2);
	slow_call_clear(&route);
	slow_call_clear(&anonymous);
	slow_call_clear(&other);
	slow_call_clear(&across);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
}

/*
 * The client gives up while its checkout is suspended on Stripe. The fiber
 * still finishes -- Stripe was asked, and what it answered is recorded --
 * its answer goes nowhere, and the server goes on: the next request is
 * served, and asking again reuses the session opened for the client that
 * left instead of opening a second one.
 */
static void
test_fiber_disconnect(Fixture *f, VentureStripeService *service, FakeTransport *fake, VentureEntity *invoice)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *api = g_strdup_printf("/api/v1/invoices/%" G_GINT64_FORMAT "/checkout", venture_entity_get_id(invoice));
	SlowCall route = { 0 };
	venture_context_set_stripe_service(f->context, service);
	server = start_server(f, &state_dir);
	route.method = "POST"; route.content_type = "application/json"; route.body = "{}";
	route.cancel = g_cancellable_new();
	fake_hold(fake, 1);
	slow_call_start(&route, server, api);
	fake_wait_held(fake, 1);
	g_cancellable_cancel(route.cancel);
	slow_call_finish(&route);
	g_assert_cmpuint(fake->checkouts, ==, 0);
	fake_release_all(fake);
	run_until(no_fibers, NULL);
	g_assert_cmpuint(fake->checkouts, ==, 1);
	g_assert_cmpuint(http_request(server, "GET", "/api/v1/health", NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(http_request(server, "POST", api, "application/json", "{}", NULL), ==, 200);
	g_assert_cmpuint(fake->checkouts, ==, 1);
	slow_call_clear(&route);
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
}
