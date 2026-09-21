/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "venture-test-util.h"

static gchar *client_path;
typedef struct { gboolean done; gboolean ok; gchar *output; gchar *errors; GError *error; } ClientResult;
typedef struct { guint requests; gboolean redirect; gboolean cookie_seen; } ServerResult;

static void
client_done(GObject *source, GAsyncResult *result, gpointer data)
{
	ClientResult *outcome = data;
	outcome->ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
		&outcome->output, &outcome->errors, &outcome->error);
	outcome->done = TRUE;
}

static gboolean
client_deadline(gpointer data)
{
	g_object_set_data(G_OBJECT(data), "deadline-exceeded", GINT_TO_POINTER(1));
	g_subprocess_force_exit(G_SUBPROCESS(data));
	return G_SOURCE_REMOVE;
}

static gboolean
run_client(const gchar *origin, const gchar *file)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	ClientResult result = { FALSE, FALSE, NULL, NULL, NULL };
	gboolean success;
	guint deadline;
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawn(launcher, &error, client_path, "--server", origin,
		"--session-file", file, "--format", "json", "health", NULL);
	g_assert_no_error(error); g_assert_nonnull(child);
	deadline = g_timeout_add_seconds(10, client_deadline, child);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, client_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_null(g_object_get_data(G_OBJECT(child), "deadline-exceeded"));
	g_source_remove(deadline);
	g_assert_no_error(result.error); g_assert_true(result.ok);
	success = g_subprocess_get_successful(child);
	g_free(result.output); g_free(result.errors);
	return success;
}

static void
server_request(SoupServer *server, SoupServerMessage *message, const gchar *path,
	GHashTable *query, gpointer data)
{
	ServerResult *result = data;
	const gchar *cookie = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "Cookie");
	(void)server; (void)query;
	result->requests++;
	result->cookie_seen = g_strcmp0(cookie, "venture_session=synthetic-fixture") == 0;
	if (result->redirect && g_strcmp0(path, "/api/v1/health") == 0) {
		soup_server_message_set_status(message, 302, NULL);
		soup_message_headers_replace(soup_server_message_get_response_headers(message), "Location", "/credential-trap");
		return;
	}
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_STATIC, "{\"status\":\"ok\"}", 15);
}

/* A copied session cannot be sent to a different selected origin, inherited
 * through a redirect, or read from a permissive/link/FIFO credential file. */
static void
test_session_transport(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autofree gchar *directory = g_dir_make_tmp("venture-cli-session-XXXXXX", &error);
	g_autofree gchar *file = g_build_filename(directory, "session.json", NULL);
	g_autofree gchar *link_path = g_build_filename(directory, "linked.json", NULL);
	g_autofree gchar *fifo = g_build_filename(directory, "fifo", NULL);
	g_autofree gchar *origin = NULL, *contents = NULL;
	GSList *uris;
	ServerResult result = { 0, FALSE, FALSE };
	g_assert_no_error(error);
	soup_server_add_handler(server, NULL, server_request, &result, NULL);
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)); g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	origin = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	contents = g_strdup_printf("{\"origin\":\"%s\",\"cookie\":\"venture_session=synthetic-fixture\"}", origin);
	g_assert_true(g_file_set_contents(file, contents, -1, &error)); g_assert_no_error(error);
	g_assert_cmpint(g_chmod(file, 0600), ==, 0);
	g_assert_true(run_client(origin, file)); g_assert_cmpuint(result.requests, ==, 1); g_assert_true(result.cookie_seen);
	result.redirect = TRUE;
	g_assert_false(run_client(origin, file)); g_assert_cmpuint(result.requests, ==, 2);
	g_assert_false(run_client("https://another.example.test", file)); g_assert_cmpuint(result.requests, ==, 2);
	g_assert_cmpint(g_chmod(file, 0644), ==, 0);
	g_assert_false(run_client(origin, file));
	g_assert_cmpint(g_chmod(file, 0600), ==, 0);
	g_assert_cmpint(symlink(file, link_path), ==, 0);
	g_assert_false(run_client(origin, link_path)); g_assert_cmpint(g_unlink(link_path), ==, 0);
	g_assert_cmpint(link(file, link_path), ==, 0);
	g_assert_false(run_client(origin, file)); g_assert_cmpint(g_unlink(link_path), ==, 0);
	g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
	g_assert_false(run_client(origin, fifo));
	g_assert_cmpuint(result.requests, ==, 2);
	soup_server_disconnect(server);
	venture_test_remove_tree(directory);
}

int
main(int argc, char **argv)
{
	g_autofree gchar *program = g_canonicalize_filename(argv[0], NULL);
	g_autofree gchar *directory = g_path_get_dirname(program);
	g_autofree gchar *parent = g_path_get_dirname(directory);
	gint result;
	g_test_init(&argc, &argv, NULL);
	client_path = g_build_filename(parent, "venturectl", NULL);
	g_test_add_func("/cli/session-transport", test_session_transport);
	result = g_test_run();
	g_free(client_path);
	return result;
}
