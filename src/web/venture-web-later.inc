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
 * from an idle source a moment later, as a fiber (core/venture-fiber.c):
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
	guint				 maintenance_depth;
	gboolean			 admitted;
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
	g_autoptr(HtmxContext) http = htmx_context_new(later->request);
	HtmxResponse *response;

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
	venture_fiber_wake(venture_fiber_new(venture_web_later_run, venture_web_later_done,
		venture_web_later_swap, data));
	return G_SOURCE_REMOVE;
}

/*
 * Answers @request with @callback from a fiber of the main loop, after
 * this dispatch has returned. Returns the placeholder the route answers
 * with. Call it from the route, inside the request pipeline.
 */
static HtmxResponse *
venture_web_answer_later(VentureWebServer *self, HtmxRequest *request,
	GHashTable *params, HtmxRouteCallback callback)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureWebLater *later = g_new0(VentureWebLater, 1);

	later->server = g_object_ref(self);
	later->request = g_object_ref(request);
	later->params = (NULL != params) ? g_hash_table_ref(params) : NULL;
	later->callback = callback;
	later->deferred = htmx_response_new_deferred();
	later->reader = venture_access_policy_snapshot(venture_database_get_access_policy(database));
	later->access = venture_access_state_new();
	/* The request pipeline lets go of its admission when this returns;
	 * the work is still to come, so it holds one of its own. */
	if (NULL != self->workspace_limiter)
	{
		self->workspace_http_active++;
		later->admitted = TRUE;
	}
	g_idle_add(venture_web_later_start, later);
	return g_object_ref(later->deferred);
}

/* A route that may wait on Stripe, registered to answer from a fiber. */
#define VENTURE_WEB_LATER_ROUTE(name, handler) \
	static HtmxResponse * \
	name(HtmxRequest *request, GHashTable *params, gpointer data) \
	{ \
		return venture_web_answer_later(data, request, params, handler); \
	}
