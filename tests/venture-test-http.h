/*
 * venture-test-http.h - A scripted HTTP server for tests of feed providers
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A SoupServer on a thread and a main context of its own, bound to port 0
 * on 127.0.0.1 (never a port derived from the pid), answering each path
 * from a table of routes the test fills before any request is made. It
 * records what arrived -- hits per path, and the last request headers and
 * query string per path -- so a test can assert both what the provider
 * sent and what it did with the answer.
 *
 * Header-only, like venture-test-util.h: every function is static inline,
 * so a test that uses part of it is not warned about the rest.
 *
 * Routes match the path alone; the query string is recorded, not matched.
 * A route can demand an Authorization header (anything else is a 401), and
 * answer 304 to a request carrying If-Modified-Since, which is how a feed's
 * conditional fetch is tested.
 */

#ifndef VENTURE_TEST_HTTP_H
#define VENTURE_TEST_HTTP_H

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>

/**
 * VentureTestRoute:
 * @status: the status answered
 * @body: the body answered
 * @content_type: its type; NULL for application/json
 * @headers: (nullable): more response headers, name, value pairs
 * @require_authorization: (nullable): the exact Authorization header the
 *   route wants; any other answers 401 with an empty body
 * @not_modified_when_asked: answer 304, no body, when If-Modified-Since
 *   arrives
 *
 * One path's answer. Set up before any request is made, then left alone:
 * the server thread reads it.
 */
typedef struct
{
	guint		 status;
	gchar		*body;
	gchar		*content_type;
	gchar	       **headers;
	gchar		*require_authorization;
	gboolean	 not_modified_when_asked;
} VentureTestRoute;

/**
 * VentureTestHttp:
 * @origin: "http://127.0.0.1:PORT", for feeds.allowed_origins
 *
 * The server; the other members are its own.
 */
typedef struct
{
	GMutex		 lock;
	GCond		 ready;
	GMainContext	*context;
	GMainLoop	*loop;
	GThread		*thread;
	gchar		*origin;
	GHashTable	*routes;	/* path -> VentureTestRoute */
	GHashTable	*hits;		/* path -> gint* */
	GHashTable	*seen;		/* "path\nheader" -> value; "path\n?" -> query */
} VentureTestHttp;

static inline void
venture_test_route_free(gpointer data)
{
	VentureTestRoute *route = data;

	g_free(route->body);
	g_free(route->content_type);
	g_strfreev(route->headers);
	g_free(route->require_authorization);
	g_free(route);
}

/* Remembers one request detail under the path, for the test to read. */
static inline void
venture_test_http_remember(
	VentureTestHttp	*http,
	const gchar	*path,
	const gchar	*what,
	const gchar	*value
){
	g_hash_table_replace(http->seen, g_strconcat(path, "\n", what, NULL), g_strdup(value));
}

static inline void
venture_test_http_handle(
	SoupServer		*server,
	SoupServerMessage	*message,
	const gchar		*path,
	GHashTable		*query,
	gpointer		 data
){
	VentureTestHttp *http = data;
	SoupMessageHeaders *request;
	SoupMessageHeaders *response;
	VentureTestRoute *route;
	const gchar *authorization;
	const gchar *ims;
	gint *hits;
	guint i;

	(void)server;
	(void)query;

	request = soup_server_message_get_request_headers(message);
	response = soup_server_message_get_response_headers(message);
	authorization = soup_message_headers_get_one(request, "Authorization");
	ims = soup_message_headers_get_one(request, "If-Modified-Since");

	g_mutex_lock(&http->lock);
	hits = g_hash_table_lookup(http->hits, path);

	if (NULL == hits)
	{
		hits = g_new0(gint, 1);
		g_hash_table_insert(http->hits, g_strdup(path), hits);
	}

	(*hits)++;
	venture_test_http_remember(http, path, "Authorization", authorization);
	venture_test_http_remember(http, path, "If-Modified-Since", ims);
	venture_test_http_remember(http, path, "Method", soup_server_message_get_method(message));
	venture_test_http_remember(http, path, "?", g_uri_get_query(soup_server_message_get_uri(message)));

	route = g_hash_table_lookup(http->routes, path);

	if (NULL == route)
	{
		g_mutex_unlock(&http->lock);
		soup_server_message_set_status(message, 404, NULL);
		return;
	}

	if ((NULL != route->require_authorization) &&
	    (0 != g_strcmp0(route->require_authorization, authorization)))
	{
		g_mutex_unlock(&http->lock);
		soup_server_message_set_status(message, 401, NULL);
		return;
	}

	if (route->not_modified_when_asked && (NULL != ims))
	{
		g_mutex_unlock(&http->lock);
		soup_server_message_set_status(message, 304, NULL);
		return;
	}

	for (i = 0; (NULL != route->headers) && (NULL != route->headers[i]) &&
	            (NULL != route->headers[i + 1]); i += 2)
		soup_message_headers_replace(response, route->headers[i], route->headers[i + 1]);

	soup_server_message_set_status(message, route->status, NULL);
	soup_server_message_set_response(message,
	                                 (NULL != route->content_type) ? route->content_type
	                                                               : "application/json",
	                                 SOUP_MEMORY_COPY, route->body, strlen(route->body));
	g_mutex_unlock(&http->lock);
}

static inline gpointer
venture_test_http_thread(gpointer data)
{
	VentureTestHttp *http = data;
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	gchar *origin;

	g_main_context_push_thread_default(http->context);
	server = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, venture_test_http_handle, http, NULL);

	/* Port 0: the kernel's choice, read back. */
	if (!soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error))
		g_error("the scripted server could not listen: %s", error->message);

	uris = soup_server_get_uris(server);
	origin = g_uri_to_string_partial(uris->data, G_URI_HIDE_PASSWORD);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	if (g_str_has_suffix(origin, "/"))
		origin[strlen(origin) - 1] = '\0';

	g_mutex_lock(&http->lock);
	http->origin = origin;
	g_cond_signal(&http->ready);
	g_mutex_unlock(&http->lock);

	g_main_loop_run(http->loop);

	soup_server_disconnect(server);
	g_main_context_pop_thread_default(http->context);

	return NULL;
}

/**
 * venture_test_http_start:
 * @http: zeroed storage
 *
 * Starts the server and waits, bounded, until it listens.
 */
static inline void
venture_test_http_start(VentureTestHttp *http)
{
	gint64 deadline;

	memset(http, 0, sizeof(*http));
	g_mutex_init(&http->lock);
	g_cond_init(&http->ready);
	http->context = g_main_context_new();
	http->loop = g_main_loop_new(http->context, FALSE);
	http->routes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, venture_test_route_free);
	http->hits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	http->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	g_mutex_lock(&http->lock);
	http->thread = g_thread_new("venture-test-http", venture_test_http_thread, http);
	deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

	while (NULL == http->origin)
		g_assert_true(g_cond_wait_until(&http->ready, &http->lock, deadline));

	g_mutex_unlock(&http->lock);
}

static inline gboolean
venture_test_http_quit(gpointer data)
{
	g_main_loop_quit(data);

	return G_SOURCE_REMOVE;
}

/**
 * venture_test_http_stop:
 * @http: a started server
 *
 * Stops it and frees what it holds. Safe on a server never started.
 */
static inline void
venture_test_http_stop(VentureTestHttp *http)
{
	if (NULL == http->thread)
		return;

	g_main_context_invoke(http->context, venture_test_http_quit, http->loop);
	g_thread_join(http->thread);
	http->thread = NULL;
	g_main_loop_unref(http->loop);
	g_main_context_unref(http->context);
	g_hash_table_unref(http->routes);
	g_hash_table_unref(http->hits);
	g_hash_table_unref(http->seen);
	g_free(http->origin);
	g_cond_clear(&http->ready);
	g_mutex_clear(&http->lock);
}

/**
 * venture_test_http_route:
 * @http: the server
 * @path: the path, without a query
 * @status: the status answered
 * @body: (nullable): the body; empty for %NULL
 *
 * Sets (or replaces) a path's answer.
 *
 * Returns: (transfer none): the route, to adjust before requests are made
 */
static inline VentureTestRoute *
venture_test_http_route(
	VentureTestHttp	*http,
	const gchar	*path,
	guint		 status,
	const gchar	*body
){
	VentureTestRoute *route;

	route = g_new0(VentureTestRoute, 1);
	route->status = status;
	route->body = g_strdup((NULL != body) ? body : "");

	g_mutex_lock(&http->lock);
	g_hash_table_replace(http->routes, g_strdup(path), route);
	g_mutex_unlock(&http->lock);

	return route;
}

/**
 * venture_test_http_route_file:
 * @http: the server
 * @path: the path
 * @status: the status
 * @directory: a fixture directory
 * @file: the file in it whose contents are the body
 *
 * Returns: (transfer none): the route
 */
static inline VentureTestRoute *
venture_test_http_route_file(
	VentureTestHttp	*http,
	const gchar	*path,
	guint		 status,
	const gchar	*directory,
	const gchar	*file
){
	g_autofree gchar *full = NULL;
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) error = NULL;

	full = g_build_filename(directory, file, NULL);

	if (!g_file_get_contents(full, &contents, NULL, &error))
		g_error("fixture %s: %s", full, error->message);

	return venture_test_http_route(http, path, status, contents);
}

/**
 * venture_test_http_hits:
 * @http: the server
 * @path: a path
 *
 * Returns: how many requests reached it
 */
static inline gint
venture_test_http_hits(
	VentureTestHttp	*http,
	const gchar	*path
){
	gint *hits;
	gint count;

	g_mutex_lock(&http->lock);
	hits = g_hash_table_lookup(http->hits, path);
	count = (NULL != hits) ? *hits : 0;
	g_mutex_unlock(&http->lock);

	return count;
}

/**
 * venture_test_http_seen:
 * @http: the server
 * @path: a path
 * @what: a request header's name ("Authorization", "If-Modified-Since"),
 *   "Method", or "?" for the query string
 *
 * Returns: (transfer full) (nullable): what the last request to @path had
 */
static inline gchar *
venture_test_http_seen(
	VentureTestHttp	*http,
	const gchar	*path,
	const gchar	*what
){
	g_autofree gchar *key = NULL;
	gchar *value;

	key = g_strconcat(path, "\n", what, NULL);
	g_mutex_lock(&http->lock);
	value = g_strdup(g_hash_table_lookup(http->seen, key));
	g_mutex_unlock(&http->lock);

	return value;
}

#endif /* VENTURE_TEST_HTTP_H */
