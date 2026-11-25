/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-stripe-transport.c - A Stripe call that leaves the main loop
 * serving everybody else
 *
 * stripe-glib's client is synchronous, and its network transport waits
 * for Stripe on a private context so it dispatches nothing of ours. On
 * the web server's thread that wait was the whole server's: one Stripe
 * call that took thirty seconds to time out held every other request for
 * those thirty seconds.
 *
 * This transport sends on a thread of its own. Called from a fiber (a web
 * route that may wait on Stripe runs as one, src/web/venture-web-later.inc)
 * it yields while it waits: the main loop serves everybody, and the fiber
 * is woken when its own answer arrives, so two waits end in the order
 * their answers come. Called on a plain stack inside the main loop -- a
 * timer, a sweep -- it runs that loop while it waits instead: the nested
 * wait the bank feed sync and the webhook test already use, which other
 * requests on open connections get through but which ends only after
 * every wait begun inside it. Either way the caller gets its answer
 * synchronously.
 *
 * It waits either way only with no database transaction open -- a request
 * served meanwhile would otherwise write inside the caller's transaction
 * -- and the nested wait only on the thread that owns the default main
 * context, inside that loop. Anywhere else it waits as before.
 *
 * Its timeout is the client's: the request's deadline (request_timeout,
 * thirty seconds unless configured) bounds every attempt, retries
 * included, and the inner transport enforces it.
 */

#include <venture.h>
#include <stripe-glib.h>
#include "core/venture-fiber-private.h"

typedef struct
{
	GObject		 parent;
	VentureDatabase	*database;	/* weak: the transport never keeps it */
	StripeTransport	*inner;
} VentureStripeTransport;

typedef struct
{
	GObjectClass parent;
} VentureStripeTransportClass;

GType venture_stripe_transport_get_type(void);
static void venture_stripe_transport_iface_init(StripeTransportInterface *iface);

G_DEFINE_TYPE_WITH_CODE(VentureStripeTransport, venture_stripe_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(STRIPE_TYPE_TRANSPORT, venture_stripe_transport_iface_init))

/* One call, shared with the thread that makes it. The request and the
 * cancellable belong to the caller, which does not return before the
 * thread has finished with them. */
typedef struct
{
	StripeTransport			*inner;
	const StripeHttpRequest		*request;
	GCancellable			*cancellable;
	GMainContext			*context;
	VentureFiber			*fiber;		/* woken when the answer is in */
	StripeResponse			*response;
	GError				*error;
	gint				 done;
} Call;

static gboolean
call_wake(gpointer data)
{
	venture_fiber_wake(data);
	return G_SOURCE_REMOVE;
}

static gpointer
call_run(gpointer data)
{
	Call *call = data;

	call->response = STRIPE_TRANSPORT_GET_IFACE(call->inner)->send(
		call->inner, call->request, call->cancellable, &call->error);
	g_atomic_int_set(&call->done, 1);
	if (NULL != call->fiber)
	{
		/* A source, run by the main loop on its own thread: the fiber
		 * yielded before this thread could have started. */
		GSource *source = g_idle_source_new();

		g_source_set_priority(source, G_PRIORITY_DEFAULT);
		g_source_set_callback(source, call_wake, call->fiber, NULL);
		g_source_set_static_name(source, "venture-stripe-answer");
		g_source_attach(source, call->context);
		g_source_unref(source);
	}
	else
		g_main_context_wakeup(call->context);
	return NULL;
}

/* A request served meanwhile would otherwise write inside the caller's
 * transaction. */
static gboolean
no_transaction(VentureStripeTransport *self)
{
	return NULL == self->database || !venture_database_has_transaction(self->database);
}

/* Whether a nested loop may serve other work while this call waits. */
static gboolean
may_serve_meanwhile(VentureStripeTransport *self, GMainContext *context)
{
	g_autoptr(GMainContext) current = g_main_context_ref_thread_default();

	if (current != context || !g_main_context_is_owner(context))
		return FALSE;
	return no_transaction(self);
}

static StripeResponse *
venture_stripe_transport_send(
	StripeTransport		 *transport,
	const StripeHttpRequest	 *request,
	GCancellable		 *cancellable,
	GError			**error
){
	VentureStripeTransport *self = (VentureStripeTransport *)transport;
	GMainContext *context = g_main_context_default();
	Call call = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0 };
	VentureFiber *fiber = venture_fiber_current();
	GThread *thread;

	if (NULL != fiber ? !no_transaction(self) : !may_serve_meanwhile(self, context))
		return STRIPE_TRANSPORT_GET_IFACE(self->inner)->send(self->inner, request, cancellable, error);

	call.inner = self->inner;
	call.request = request;
	call.cancellable = cancellable;
	call.context = context;
	call.fiber = fiber;
	thread = g_thread_try_new("venture-stripe", call_run, &call, NULL);
	if (NULL == thread)
		return STRIPE_TRANSPORT_GET_IFACE(self->inner)->send(self->inner, request, cancellable, error);

	if (NULL != fiber)
	{
		/* Exactly one wake comes, from call_wake, and only once this
		 * fiber has given the thread back. */
		venture_fiber_yield();
		g_warn_if_fail(g_atomic_int_get(&call.done));
	}
	else
	{
		/* call_run wakes the context after it sets done, so a finish
		 * that lands between this test and the poll ends the poll. */
		while (!g_atomic_int_get(&call.done))
			g_main_context_iteration(context, TRUE);
	}
	g_thread_join(thread);

	if (NULL != call.error)
		g_propagate_error(error, call.error);
	return call.response;
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
	self->inner = (NULL != inner) ? g_object_ref(inner) : stripe_soup_transport_new();
	return STRIPE_TRANSPORT(self);
}
