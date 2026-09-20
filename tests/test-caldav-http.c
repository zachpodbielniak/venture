/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>

typedef struct {
	GMutex lock;
	GCond ready;
	GMainContext *context;
	GMainLoop *loop;
	gchar *origin;
	gchar *neighbor;
	gint neighbor_requests;
} DavHttp;

static void
neighbor_request(SoupServer *server, SoupServerMessage *message, const gchar *path,
	GHashTable *query, gpointer data)
{
	DavHttp *fixture = data;
	(void)server; (void)path; (void)query;
	g_atomic_int_inc(&fixture->neighbor_requests);
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "text/calendar", SOUP_MEMORY_STATIC, "private-neighbor", 16);
}
static void
origin_request(SoupServer *server, SoupServerMessage *message, const gchar *path,
	GHashTable *query, gpointer data)
{
	DavHttp *fixture = data;
	(void)server; (void)query;
	if (!g_strcmp0(path, "/redirect")) {
		soup_server_message_set_redirect(message, 302, fixture->neighbor);
		return;
	}
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "text/calendar", SOUP_MEMORY_STATIC, "ordinary-event", 14);
}
static gchar *
listen_origin(SoupServer *server)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;
	gchar *origin;
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	origin = g_uri_to_string(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	return origin;
}
/* Only these synthetic HTTP listeners use the helper thread; no repository,
 * service or authority scope crosses the application's database thread. */
static gpointer
dav_http_thread(gpointer data)
{
	DavHttp *fixture = data;
	g_autoptr(SoupServer) origin = NULL, neighbor = NULL;
	g_main_context_push_thread_default(fixture->context);
	origin = soup_server_new(NULL, NULL);
	neighbor = soup_server_new(NULL, NULL);
	soup_server_add_handler(origin, NULL, origin_request, fixture, NULL);
	soup_server_add_handler(neighbor, NULL, neighbor_request, fixture, NULL);
	g_mutex_lock(&fixture->lock);
	fixture->neighbor = listen_origin(neighbor);
	fixture->origin = listen_origin(origin);
	g_cond_signal(&fixture->ready);
	g_mutex_unlock(&fixture->lock);
	g_main_loop_run(fixture->loop);
	soup_server_disconnect(origin);
	soup_server_disconnect(neighbor);
	g_main_context_pop_thread_default(fixture->context);
	return NULL;
}
static gboolean stop_http(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}
static void
test_redirect(void)
{
	DavHttp fixture;
	GThread *thread;
	gint64 deadline;
	g_autoptr(VentureSoupCalDavClient) client = venture_soup_caldav_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ordinary = NULL, *redirected = NULL;
	g_mutex_init(&fixture.lock); g_cond_init(&fixture.ready);
	fixture.context = g_main_context_new(); fixture.loop = g_main_loop_new(fixture.context, FALSE);
	fixture.origin = NULL; fixture.neighbor = NULL; fixture.neighbor_requests = 0;
	g_mutex_lock(&fixture.lock);
	thread = g_thread_new("caldav-http-fixture", dav_http_thread, &fixture);
	deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
	while (!fixture.origin) g_assert_true(g_cond_wait_until(&fixture.ready, &fixture.lock, deadline));
	g_mutex_unlock(&fixture.lock);
	g_assert_true(venture_caldav_client_connect(VENTURE_CALDAV_CLIENT(client), fixture.origin, "fixture", "synthetic", NULL, &error));
	g_assert_no_error(error);
	ordinary = venture_caldav_client_fetch(VENTURE_CALDAV_CLIENT(client), "/ordinary", NULL, NULL, &error);
	g_assert_no_error(error); g_assert_cmpstr(ordinary, ==, "ordinary-event");
	redirected = venture_caldav_client_fetch(VENTURE_CALDAV_CLIENT(client), "/redirect", NULL, NULL, &error);
	g_assert_null(redirected);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED);
	g_assert_cmpint(g_atomic_int_get(&fixture.neighbor_requests), ==, 0);
	g_main_context_invoke(fixture.context, stop_http, fixture.loop);
	g_thread_join(thread);
	g_main_loop_unref(fixture.loop); g_main_context_unref(fixture.context);
	g_free(fixture.origin); g_free(fixture.neighbor);
	g_cond_clear(&fixture.ready); g_mutex_clear(&fixture.lock);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/caldav-http/redirect-authority", test_redirect);
	return g_test_run();
}
