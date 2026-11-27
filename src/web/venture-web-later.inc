/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Answers that wait on an outside provider, run as fibers of the main
 * loop rather than inside the dispatch that read the request.
 *
 * libsoup reads a new connection's request, and htmx-glib runs its route,
 * inside the listener's own source. GLib does not dispatch a source again
 * while it is still dispatching, so a route that waits there leaves every
 * connection not yet accepted waiting with it. And a wait in a nested
 * main loop ends only after every wait that started inside it.
 *
 * So a route that may wait on Stripe answers with a placeholder and runs
 * from a source a moment later, as a fiber (core/venture-fiber.c):
 * still on the main thread, with the database and every handler where
 * they belong, but on a stack of its own. When the Stripe transport has
 * to wait it yields, the loop goes on serving everybody, and the fiber is
 * woken when its own answer arrives -- not when somebody else's does.
 *
 * It runs under the scopes the request pipeline gave the route: the
 * reader the pipeline authenticated (the snapshot taken as the route was
 * called, the same thing the read workers judge by), and the tenant
 * request scope, which for these routes -- none of them admits a support
 * capability -- is the plain one. Between turns both are the fiber's own
 * (venture_web_later_swap()). Hosted admission keeps counting it until it
 * is answered, and before the answer escapes the hosted lifecycle is
 * checked again and a browser error becomes a page, as the pipeline does
 * for a synchronous route.
 */

#include "core/venture-fiber-private.h"
#include "web/venture-web-later-private.h"

typedef struct
{
	VentureWebServer		*server;
	HtmxRequest			*request;
	GHashTable			*params;
	HtmxRouteCallback		 callback;
	HtmxResponse			*deferred;
	HtmxResponse			*response;
	VentureAccessSnapshot		*reader;
	VentureAccessState		*access;
	VentureTenantSupportScope	*support;	/* not a reference */
	VentureFiber			*fiber;		/* until it is started */
	guint				 maintenance_depth;
	gboolean			 admitted;
	gboolean			 refused;	/* the pipeline answered otherwise */
} VentureWebLater;

static void
venture_web_later_free(VentureWebLater *later)
{
	g_clear_object(&later->server);
	g_clear_object(&later->request);
	g_clear_pointer(&later->params, g_hash_table_unref);
	g_clear_object(&later->deferred);
	g_clear_object(&later->response);
	g_clear_pointer(&later->reader, venture_access_snapshot_free);
	g_clear_pointer(&later->access, venture_access_state_free);
	g_free(later);
}

/* Puts the fiber's state in place, or takes it out again. */
static void
venture_web_later_swap(gpointer data)
{
	VentureWebLater *later = data;
	VentureDatabase *database = venture_context_get_database(later->server->context);

	venture_access_policy_exchange(venture_database_get_access_policy(database), later->access);
	venture_tenant_service_exchange_request(venture_tenant_service_get(database),
		&later->support, &later->maintenance_depth);
}

/* The fiber: the route, then what the pipeline does after it. */
static void
venture_web_later_run(gpointer data)
{
	VentureWebLater *later = data;
	VentureWebServer *self = later->server;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureTenantService *tenant = venture_tenant_service_get(database);
	g_autoptr(VentureTenantSupportScope) support_scope = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(HtmxContext) http = NULL;
	HtmxResponse *response;

	/* The client already has its refusal (venture_web_later_settle()):
	 * the work is not to be done, and what it would answer goes nowhere. */
	if (later->refused)
	{
		later->response = htmx_response_new();
		return;
	}
	http = htmx_context_new(later->request);
	if (venture_tenant_service_is_enabled(tenant))
		support_scope = venture_tenant_service_enter_request(tenant, NULL, NULL,
			htmx_request_get_path(later->request), NULL);
	scope = venture_access_policy_enter_snapshot(venture_database_get_access_policy(database), later->reader);
	response = later->callback(later->request, later->params, self);
	g_clear_object(&scope);

	htmx_context_set_response(http, (NULL != response) ? response : venture_web_error_response(NULL));
	if (venture_web_hosted_finish(self, http))
		venture_web_error_for_browser(self, http);
	later->response = g_object_ref(htmx_context_get_response(http));
}

/* Back on the waker's stack, the fiber finished: answer and let go. */
static void
venture_web_later_done(gpointer data)
{
	VentureWebLater *later = data;

	if (later->admitted)
		later->server->workspace_http_active--;
	htmx_response_complete(later->deferred, later->response);
	venture_web_later_free(later);
}

static gboolean
venture_web_later_start(gpointer data)
{
	VentureWebLater *later = data;
	VentureFiber *fiber = later->fiber;

	/* The fiber may finish, and free @later, before this returns. */
	later->fiber = NULL;
	venture_fiber_wake(fiber);
	return G_SOURCE_REMOVE;
}

/*
 * Called by the outermost middleware with the response @request is to be
 * answered with. If the pipeline put something else in the placeholder's
 * place after the route -- the hosted lifecycle check refusing it, say --
 * the route's work is never done: it would reach Stripe for an answer
 * nobody is waiting for.
 */
static void
venture_web_later_settle(HtmxRequest *request, HtmxResponse *response)
{
	VentureWebLater *later = g_object_steal_data(G_OBJECT(request), "venture-web-later");

	if (NULL != later && response != later->deferred)
		later->refused = TRUE;
}

/*
 * Answers @request with @callback from a fiber of the main loop, after
 * this dispatch has returned. Returns the placeholder the route answers
 * with. Call it from the route, inside the request pipeline.
 */
HtmxResponse *
venture_web_answer_later(VentureWebServer *self, HtmxRequest *request,
	GHashTable *params, HtmxRouteCallback callback)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureWebLater *later = g_new0(VentureWebLater, 1);
	GSource *source;

	/* Made now, so it is counted from the moment the request is taken
	 * (shutdown waits for it) and a stack that cannot be had is a
	 * refusal here, not an abort or a hung client later. */
	later->fiber = venture_fiber_new(venture_web_later_run, venture_web_later_done,
		venture_web_later_swap, later);
	if (NULL == later->fiber)
	{
		HtmxResponse *busy = htmx_response_new_with_content("<p>The server is busy. Try again shortly.</p>");

		g_free(later);
		htmx_response_set_status(busy, 503);
		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}
	later->server = g_object_ref(self);
	later->request = g_object_ref(request);
	later->params = (NULL != params) ? g_hash_table_ref(params) : NULL;
	later->callback = callback;
	later->deferred = htmx_response_new_deferred();
	/* The disconnect hint, where the route can find it from the request
	 * (venture_web_later_get_cancellable()): a query cancels on it. */
	g_object_set_data_full(G_OBJECT(request), "venture-web-later-cancellable",
		g_object_ref(htmx_response_get_cancellable(later->deferred)), g_object_unref);
	later->reader = venture_access_policy_snapshot(venture_database_get_access_policy(database));
	later->access = venture_access_state_new();
	/* For venture_web_later_settle(), in this same dispatch, before the
	 * fiber starts. */
	g_object_set_data(G_OBJECT(request), "venture-web-later", later);
	/* The request pipeline lets go of its admission when this returns;
	 * the work is still to come, so it holds one of its own. */
	if (NULL != self->workspace_limiter)
	{
		self->workspace_http_active++;
		later->admitted = TRUE;
	}
	/* Default priority, as every other request's dispatch: an idle source
	 * would wait behind all of them and starve on a busy server. */
	source = g_idle_source_new();
	g_source_set_priority(source, G_PRIORITY_DEFAULT);
	g_source_set_callback(source, venture_web_later_start, later, NULL);
	g_source_set_static_name(source, "venture-web-later");
	g_source_attach(source, NULL);
	g_source_unref(source);
	return g_object_ref(later->deferred);
}

/*
 * The disconnect hint of a request answered from a fiber: cancelled when
 * libsoup reports the client gone (htmx_response_get_cancellable(), whose
 * caveats apply). NULL for a request answered any other way.
 */
static GCancellable *
venture_web_later_get_cancellable(HtmxRequest *request)
{
	return g_object_get_data(G_OBJECT(request), "venture-web-later-cancellable");
}

/* A route that may wait on Stripe, registered to answer from a fiber. */
#define VENTURE_WEB_LATER_ROUTE(name, handler) \
	static HtmxResponse * \
	name(HtmxRequest *request, GHashTable *params, gpointer data) \
	{ \
		return venture_web_answer_later(data, request, params, handler); \
	}
