/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-stripe-transport.c - A Stripe call that leaves the main loop
 * serving everybody else
 *
 * stripe-glib's client is synchronous: it calls its transport on the
 * caller's stack and, between retries, sleeps there too. On the web
 * server's thread either wait was the whole server's: one Stripe call that
 * took thirty seconds to time out, or one 429 that asked for thirty seconds'
 * patience, held every other request for those thirty seconds.
 *
 * Every client VENTURE makes therefore has stripe-glib's own retries off
 * (venture_stripe_client_new(): retry_count 0) and sends through this
 * transport, which does two things:
 *
 * - Called from a fiber (a web route that may wait on Stripe runs as one,
 *   src/web/venture-web-later.inc) with no database transaction open, it
 *   sends on a pooled thread and yields while it waits. The main loop
 *   serves everybody, and the fiber is woken when its own answer arrives,
 *   so two waits end in the order their answers come.
 *
 * - It retries a 429 or a 5xx itself, as stripe-glib would have: at most
 *   twice, backing off from 200 ms with jitter, never sooner than the
 *   answer's Retry-After, and never past the request's deadline. A fiber
 *   waits out the pause on a timeout source, yielded; anybody else sleeps.
 *
 * Anything else -- a plain stack (a timer, a sweep, a non-fiber route), a
 * fiber with a transaction open, another thread -- sends on its own stack
 * and blocks, exactly as before this transport existed. There is no
 * nested main loop here: one would dispatch the next request while the
 * current handler is still on the stack, which VENTURE does not do (see
 * venture-automation.c, "the nested main loop dispatches the next HTTP
 * request while the current one is still on the stack").
 *
 * stripe-glib's StripeResponse is opaque: its status and Retry-After cannot
 * be read through the library's API. So the network send is VENTURE's own
 * (venture-stripe-http.inc, the library's soup transport with the status
 * kept, there only until stripe-glib has getters for them), and the
 * retries can see what they need. An injected transport -- a test's
 * offline one -- is opaque in the same way and is never retried.
 *
 * Each retry is announced as stripe-glib announced its own: the client's
 * "event" signal, stripe.request.retried, the same fields. The operation it
 * names comes from venture_stripe_execute(), through which VENTURE makes
 * every Stripe call; the transport alone is never told it.
 *
 * Its deadline is the client's: request_timeout, thirty seconds unless
 * configured, bounds every attempt and every pause, retries included.
 */

#include <venture.h>
#include <stripe-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include "core/venture-fiber-private.h"
#include "stripe/venture-stripe-transport-private.h"

/* stripe-glib's defaults for retry_count and retry_delay_ms, which VENTURE
 * never configured: the retries move here unchanged. */
#define VENTURE_STRIPE_RETRIES (2u)
#define VENTURE_STRIPE_RETRY_DELAY_MS (200u)

/* A Retry-After past this is a refusal, not a pause (stripe-glib's rule). */
#define VENTURE_STRIPE_RETRY_AFTER_MAX_S (600u)

/* The largest answer read, as stripe-glib's own transport bounds it. */
#define VENTURE_STRIPE_BODY_MAX (4u * 1024u * 1024u)

/* Threads that may wait on Stripe at once. More waiting fibers queue for a
 * thread rather than each starting one. */
#define VENTURE_STRIPE_THREADS (8)

/* How soon a wake that found a transaction open looks again. */
#define VENTURE_STRIPE_REARM_MS (10u)

typedef struct
{
	GObject		 parent;
	VentureDatabase	*database;	/* weak: the transport never keeps it */
	StripeTransport	*inner;		/* NULL: VENTURE's own network send */
	StripeClient	*client;	/* weak: the client that sends through it */
} VentureStripeTransport;

typedef struct
{
	GObjectClass parent;
} VentureStripeTransportClass;

GType venture_stripe_transport_get_type(void);
static void venture_stripe_transport_iface_init(StripeTransportInterface *iface);

G_DEFINE_TYPE_WITH_CODE(VentureStripeTransport, venture_stripe_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(STRIPE_TYPE_TRANSPORT, venture_stripe_transport_iface_init))

/* What one attempt brought back. A status of 0 is an injected transport's
 * opaque answer, which is never retried. */
typedef struct
{
	StripeResponse	*response;
	guint		 status;
	gchar		*retry_after;
	gchar		*request_id;
	gsize		 bytes;
	GError		*error;
} Answer;

static void
answer_clear(Answer *answer)
{
	g_clear_pointer(&answer->retry_after, g_free);
	g_clear_pointer(&answer->request_id, g_free);
}

#include "venture-stripe-http.inc"

/* One attempt, on whatever thread calls it. */
static void
attempt_send(StripeTransport *inner, const StripeHttpRequest *request,
	GCancellable *cancellable, Answer *answer)
{
	if (NULL != inner)
		answer->response = STRIPE_TRANSPORT_GET_IFACE(inner)->send(inner, request, cancellable, &answer->error);
	else
		http_send(request, cancellable, answer);
}

/* --- Waiting as a fiber ---------------------------------------------------- */

/* Somebody waiting yielded. Lives on that fiber's stack, which stays mapped
 * until the fiber is woken; nothing may touch it after the wake. */
typedef struct
{
	VentureFiber	*fiber;
	VentureDatabase	*database;
	GMainContext	*context;
} Waiter;

/*
 * Wakes the waiting fiber from the main loop -- unless a transaction is open
 * there. A nested main loop elsewhere (a bank feed sync, a webhook test)
 * may be the one dispatching this, inside its caller's transaction, and a
 * fiber resumed there would write into it. It looks again shortly instead.
 */
static gboolean
waiter_wake(gpointer data)
{
	Waiter *waiter = data;
	VentureFiber *fiber = waiter->fiber;

	if (NULL != waiter->database && venture_database_has_transaction(waiter->database))
	{
		GSource *again = g_timeout_source_new(VENTURE_STRIPE_REARM_MS);

		g_source_set_priority(again, G_PRIORITY_DEFAULT);
		g_source_set_callback(again, waiter_wake, waiter, NULL);
		g_source_set_static_name(again, "venture-stripe-wake");
		g_source_attach(again, waiter->context);
		g_source_unref(again);
		return G_SOURCE_REMOVE;
	}
	venture_fiber_wake(fiber);
	return G_SOURCE_REMOVE;
}

/* Schedules waiter_wake after @delay_ms (0: as soon as the loop gets to it,
 * at default priority, so a busy server cannot starve it as an idle would). */
static void
waiter_schedule(Waiter *waiter, guint delay_ms)
{
	GSource *source = (0 == delay_ms) ? g_idle_source_new() : g_timeout_source_new(delay_ms);

	g_source_set_priority(source, G_PRIORITY_DEFAULT);
	g_source_set_callback(source, waiter_wake, waiter, NULL);
	g_source_set_static_name(source, "venture-stripe-wake");
	g_source_attach(source, waiter->context);
	g_source_unref(source);
}

/* One attempt handed to a pooled thread. The request and the cancellable
 * belong to the fiber, which is not resumed before the thread is done. */
typedef struct
{
	Waiter				 waiter;
	StripeTransport			*inner;
	const StripeHttpRequest		*request;
	GCancellable			*cancellable;
	Answer				*answer;
	gint				 done;
} Call;

static void
call_run(gpointer data, gpointer unused)
{
	Call *call = data;

	(void)unused;
	attempt_send(call->inner, call->request, call->cancellable, call->answer);
	g_atomic_int_set(&call->done, 1);
	/* A source the main loop runs on its own thread: the fiber yielded
	 * before this thread could have started. After this, the call is the
	 * fiber's again and this thread touches nothing of it. */
	waiter_schedule(&call->waiter, 0);
}

static GThreadPool *
call_pool(void)
{
	static gsize ready = 0;
	static GThreadPool *pool = NULL;

	if (g_once_init_enter(&ready))
	{
		/* Shared, bounded: at most this many Stripe waits at once. */
		pool = g_thread_pool_new(call_run, NULL, VENTURE_STRIPE_THREADS, FALSE, NULL);
		g_once_init_leave(&ready, 1);
	}
	return pool;
}

/* One attempt on a pooled thread, the fiber yielded meanwhile. */
static void
attempt_yielding(VentureStripeTransport *self, VentureFiber *fiber, const StripeHttpRequest *request,
	GCancellable *cancellable, Answer *answer)
{
	Call call;

	memset(&call, 0, sizeof call);
	call.waiter.fiber = fiber;
	call.waiter.database = self->database;
	call.waiter.context = g_main_context_default();
	call.inner = self->inner;
	call.request = request;
	call.cancellable = cancellable;
	call.answer = answer;
	if (!g_thread_pool_push(call_pool(), &call, NULL))
	{
		/* No thread to be had: wait as a plain caller does. */
		attempt_send(self->inner, request, cancellable, answer);
		return;
	}
	/* Exactly one wake comes, from waiter_wake, and only once the thread
	 * has finished with the call. */
	venture_fiber_yield();
	g_warn_if_fail(g_atomic_int_get(&call.done));
}

/* --- Retries ----------------------------------------------------------------- */

/* Milliseconds a Retry-After asks for: 0 for none or unreadable, G_MAXUINT
 * for more than ten minutes, which ends the retries (stripe-glib's rule). */
static guint
retry_after_ms(const gchar *value)
{
	g_autoptr(GDateTime) date = NULL;
	gint64 difference;

	if (NULL == value || strlen(value) > 128)
		return 0;
	if (g_ascii_isdigit(*value))
	{
		gchar *end = NULL;
		guint64 seconds = g_ascii_strtoull(value, &end, 10);

		if ('\0' == *end)
			return (seconds > VENTURE_STRIPE_RETRY_AFTER_MAX_S) ? G_MAXUINT : (guint)(seconds * 1000u);
	}
	date = soup_date_time_new_from_http_string(value);
	if (NULL == date)
		return 0;
	difference = g_date_time_to_unix(date) - g_get_real_time() / G_USEC_PER_SEC;
	if (difference > (gint64)VENTURE_STRIPE_RETRY_AFTER_MAX_S)
		return G_MAXUINT;
	return (difference > 0) ? (guint)(difference * 1000) : 0;
}

static void
deadline_error(GError **error)
{
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Provider deadline exhausted");
}

static gboolean
cancelled_error(GCancellable *cancellable, GError **error)
{
	if (NULL == cancellable || !g_cancellable_is_cancelled(cancellable))
		return FALSE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled");
	return TRUE;
}

/* The pause before a retry: yielded on a timeout source for a fiber, slept
 * for anybody else in steps that notice cancellation. */
static gboolean
pause_retry(VentureStripeTransport *self, VentureFiber *fiber, guint delay_ms,
	GCancellable *cancellable, GError **error)
{
	if (NULL != fiber)
	{
		Waiter waiter;

		waiter.fiber = fiber;
		waiter.database = self->database;
		waiter.context = g_main_context_default();
		waiter_schedule(&waiter, delay_ms);
		venture_fiber_yield();
	}
	else
	{
		gint64 end = g_get_monotonic_time() + (gint64)delay_ms * 1000;

		while (g_get_monotonic_time() < end)
		{
			gint64 remaining;

			if (cancelled_error(cancellable, error))
				return FALSE;
			remaining = end - g_get_monotonic_time();
			if (remaining > 0)
				g_usleep((gulong)MIN((gint64)10000, remaining));
		}
	}
	return !cancelled_error(cancellable, error);
}

/* --- Announcing a retry --------------------------------------------------- */

/* The operation venture_stripe_execute() is carrying on this thread. Read
 * once, at the start of a send and before any yield, so a fiber that set
 * it and another that set it after cannot be confused. */
typedef struct
{
	StripeClient	*client;
	guint		 operation;
	gint64		 started;
} Operation;

static GPrivate operation_running;

/* stripe-glib's safe() for a request id: nothing that could carry a key,
 * a webhook secret or a run of digits, only Stripe's own "req_" ids. */
static gboolean
request_id_safe(const gchar *value, const gchar *authorization)
{
	const gchar *secret = (NULL != authorization && g_str_has_prefix(authorization, "Bearer "))
		? authorization + strlen("Bearer ") : NULL;
	const gchar *p;
	guint digits = 0;

	if (NULL == value || !g_str_has_prefix(value, "req_") || strlen(value) > 255)
		return FALSE;
	if ((NULL != secret && '\0' != *secret && NULL != strstr(value, secret)) ||
		NULL != strstr(value, "sk_") || NULL != strstr(value, "pk_") ||
		NULL != strstr(value, "whsec_") || NULL != strstr(value, "rk_"))
		return FALSE;
	for (p = value; '\0' != *p; p++)
	{
		if (!g_ascii_isalnum(*p) && NULL == strchr("_-.[]", *p))
			return FALSE;
		digits = g_ascii_isdigit(*p) ? digits + 1 : 0;
		if (digits >= 6)
			return FALSE;
	}
	return TRUE;
}

/*
 * What stripe-glib emitted before its own pause, emitted here before ours:
 * stripe.request.retried on the client's "event" signal, with the same
 * fields (operation, status, duration_ms since the operation started,
 * response_bytes, attempt, and the request id when safe). The Stripe
 * service's audit handler records it as before. Emitted on the caller's
 * thread, where stripe-glib emitted it.
 */
static void
announce_retry(const Operation *operation, const Answer *answer, guint attempt,
	const StripeHttpRequest *request)
{
	g_autoptr(JsonObject) payload = json_object_new();

	json_object_set_int_member(payload, "operation", operation->operation);
	json_object_set_int_member(payload, "status", answer->status);
	json_object_set_int_member(payload, "duration_ms",
		MAX((gint64)0, (g_get_monotonic_time() - operation->started) / 1000));
	json_object_set_int_member(payload, "response_bytes", (gint64)answer->bytes);
	json_object_set_int_member(payload, "attempt", attempt);
	if (request_id_safe(answer->request_id, request->authorization))
		json_object_set_string_member(payload, "request_id", answer->request_id);
	g_signal_emit_by_name(operation->client, "event::stripe.request.retried", "stripe.request.retried", payload);
}

StripeResource *
venture_stripe_execute(
	StripeClient		 *client,
	const StripeRequest	 *request,
	GCancellable		 *cancellable,
	GError			**error
){
	Operation operation;
	StripeResource *result;

	operation.client = client;
	operation.operation = (NULL != request) ? (guint)request->operation : (guint)STRIPE_N_OPERATIONS;
	operation.started = g_get_monotonic_time();
	/* Cleared, not restored: a frame this pointed into is gone once its
	 * call returns, and every call sets its own before the send reads it. */
	g_private_set(&operation_running, &operation);
	result = stripe_client_execute(client, request, cancellable, error);
	g_private_set(&operation_running, NULL);
	return result;
}

/* --- Sending ---------------------------------------------------------------- */

static StripeResponse *
venture_stripe_transport_send(
	StripeTransport		 *transport,
	const StripeHttpRequest	 *request,
	GCancellable		 *cancellable,
	GError			**error
){
	VentureStripeTransport *self = (VentureStripeTransport *)transport;
	VentureFiber *fiber = venture_fiber_current();
	const Operation *running = g_private_get(&operation_running);
	Operation operation;
	gboolean announce;
	guint attempt;

	/* Only an operation venture_stripe_execute() is carrying on this very
	 * client can be announced: anything else has no operation to name. */
	announce = NULL != running && NULL != self->client && running->client == self->client;
	if (announce)
		operation = *running;
	else
		memset(&operation, 0, sizeof operation);

	/* A request served meanwhile would otherwise write inside this
	 * caller's transaction: with one open, even a fiber blocks. */
	if (NULL != fiber && NULL != self->database && venture_database_has_transaction(self->database))
		fiber = NULL;

	for (attempt = 0; ; attempt++)
	{
		Answer answer;
		guint delay, server_delay;
		gboolean retryable;

		memset(&answer, 0, sizeof answer);
		if (cancelled_error(cancellable, error))
			return NULL;
		if (g_get_monotonic_time() >= request->deadline_us)
		{
			deadline_error(error);
			return NULL;
		}
		if (NULL != fiber)
			attempt_yielding(self, fiber, request, cancellable, &answer);
		else
			attempt_send(self->inner, request, cancellable, &answer);
		if (NULL == answer.response)
		{
			answer_clear(&answer);
			if (NULL == answer.error)
				g_set_error_literal(&answer.error, STRIPE_ERROR, STRIPE_ERROR_TRANSPORT, "Transport failed");
			g_propagate_error(error, answer.error);
			return NULL;
		}

		/* Only rate limits and server errors are worth another try, and
		 * only a known status: an injected transport's answer is opaque. */
		retryable = 429 == answer.status || (answer.status >= 500 && answer.status <= 599);
		server_delay = retryable ? retry_after_ms(answer.retry_after) : 0;
		if (!retryable || attempt >= VENTURE_STRIPE_RETRIES || G_MAXUINT == server_delay)
		{
			answer_clear(&answer);
			return answer.response;
		}

		/* Exponential from 200 ms with up to as much again in jitter, and
		 * never sooner than Stripe asked. A pause that cannot fit before
		 * the deadline ends the operation, as stripe-glib's does. */
		delay = VENTURE_STRIPE_RETRY_DELAY_MS * (1u << attempt);
		delay = MAX(server_delay, delay + (guint)g_random_int_range(0, (gint32)delay + 1));
		if (g_get_monotonic_time() + (gint64)delay * 1000 >= request->deadline_us)
		{
			stripe_response_free(answer.response);
			answer_clear(&answer);
			deadline_error(error);
			return NULL;
		}
		if (announce)
			announce_retry(&operation, &answer, attempt + 1, request);
		stripe_response_free(answer.response);
		answer_clear(&answer);
		g_debug("Stripe answered %u; retrying in %u ms", answer.status, delay);
		if (!pause_retry(self, fiber, delay, cancellable, error))
			return NULL;
	}
}

static void
venture_stripe_transport_iface_init(StripeTransportInterface *iface)
{
	iface->send = venture_stripe_transport_send;
}

static void
venture_stripe_transport_dispose(GObject *object)
{
	VentureStripeTransport *self = (VentureStripeTransport *)object;

	if (NULL != self->database)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	self->database = NULL;
	if (NULL != self->client)
		g_object_remove_weak_pointer(G_OBJECT(self->client), (gpointer *)&self->client);
	self->client = NULL;
	g_clear_object(&self->inner);
	G_OBJECT_CLASS(venture_stripe_transport_parent_class)->dispose(object);
}

static void
venture_stripe_transport_class_init(VentureStripeTransportClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = venture_stripe_transport_dispose;
}

static void
venture_stripe_transport_init(VentureStripeTransport *self)
{
	(void)self;
}

StripeTransport *
venture_stripe_transport_new(VentureDatabase *database, StripeTransport *inner)
{
	VentureStripeTransport *self;

	g_return_val_if_fail(NULL == database || VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(NULL == inner || STRIPE_IS_TRANSPORT(inner), NULL);

	self = g_object_new(venture_stripe_transport_get_type(), NULL);
	self->database = database;
	if (NULL != database)
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
	self->inner = (NULL != inner) ? g_object_ref(inner) : NULL;
	return STRIPE_TRANSPORT(self);
}

StripeClient *
venture_stripe_client_new(VentureDatabase *database, const gchar *yaml,
	StripeTransport *transport, GError **error)
{
	g_autoptr(StripeTransport) waiting = NULL;
	g_autofree gchar *configured = NULL;
	StripeClient *client;

	g_return_val_if_fail(NULL != yaml, NULL);

	/* The transport retries, yielding while it pauses; stripe-glib's own
	 * retries would sleep on the caller's stack. */
	configured = g_strconcat(yaml, g_str_has_suffix(yaml, "\n") ? "" : "\n", "retry_count: 0\n", NULL);
	waiting = venture_stripe_transport_new(database, transport);
	client = stripe_client_new(configured, waiting, NULL, error);
	if (NULL != client)
	{
		/* Weak: the client owns the transport. Lets a retry be announced on
		 * the client that is making the operation. */
		VentureStripeTransport *own = (VentureStripeTransport *)waiting;

		own->client = client;
		g_object_add_weak_pointer(G_OBJECT(client), (gpointer *)&own->client);
	}
	return client;
}
