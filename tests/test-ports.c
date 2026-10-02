/*
 * test-ports.c - How test servers get their ports
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * Test servers used to choose their ports in advance -- mostly
 * 20000 + (getpid() + K) % 20000, sometimes by binding a probe socket and
 * closing it again -- and every so often a fixture failed to start because
 * something else held the port: another suite in another worktree or PID
 * namespace (equal pids, equal ports), a demo, a second server started by
 * the same process, a socket in TIME_WAIT. The failure passed on a rerun,
 * which is what made it expensive.
 *
 * A VentureWebServer configured with port 0 now binds whatever the kernel
 * picks and reports it through venture_web_server_get_port() and the base
 * URL, so an in-process fixture has no window at all. A subprocess that
 * must be told its port up front uses venture_test_free_port() and retries
 * on "Cannot listen on" -- which is why that message is pinned here.
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	gchar			*state_dir;
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
} Fixture;

static void
fixture_set_up(
	Fixture			*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;

	(void)data;
	fixture->state_dir = g_dir_make_tmp("venture-ports-XXXXXX", &error);
	g_assert_no_error(error);
	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-require-auth", FALSE,
	             NULL);
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
}

static void
fixture_tear_down(
	Fixture			*fixture,
	gconstpointer	 data
){
	(void)data;
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_test_remove_tree(fixture->state_dir);
	g_clear_pointer(&fixture->state_dir, g_free);
}

/* --- Talking to a server on this thread's main loop ------------------------ */

typedef struct
{
	gboolean	 done;
	gboolean	 timed_out;
	GBytes		*body;
	GError		*error;
} Reply;

static void
reply_received(
	GObject			*source,
	GAsyncResult	*result,
	gpointer		 data
){
	Reply *reply = data;

	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static gboolean
reply_deadline(gpointer data)
{
	Reply *reply = data;

	reply->timed_out = TRUE;
	return G_SOURCE_REMOVE;
}

/*
 * GET @url/api/v1/health and return the status.
 *
 * The server answers on this thread's main context, so a blocking send
 * would wait on itself; the loop is driven here instead, under a deadline,
 * because a test that can hang is worse than one that fails.
 */
static guint
health(
	const gchar	*base_url
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10, NULL);
	g_autofree gchar *url = g_strconcat(base_url, "/api/v1/health", NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Reply reply = { FALSE, FALSE, NULL, NULL };
	guint deadline;

	deadline = g_timeout_add_seconds(15, reply_deadline, &reply);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, reply_received, &reply);
	while (!reply.done && !reply.timed_out)
		g_main_context_iteration(NULL, TRUE);
	g_assert_false(reply.timed_out);
	g_source_remove(deadline);
	g_assert_no_error(reply.error);
	g_bytes_unref(reply.body);

	return soup_message_get_status(message);
}

/* A loopback listener holding @port, or any port when @port is 0. */
static GSocketListener *
occupy(
	guint16	 port,
	guint16	*held
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autoptr(GSocketAddress) wanted = g_inet_socket_address_new(loopback, port);
	g_autoptr(GSocketAddress) effective = NULL;

	g_assert_true(g_socket_listener_add_address(listener, wanted, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_TCP, NULL, &effective, &error));
	g_assert_no_error(error);
	*held = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));

	return g_steal_pointer(&listener);
}

/* --- The tests -------------------------------------------------------------- */

/*
 * Port 0 starts on a real port and says which: get_port() and the base
 * URL both name it, and the server answers there. What breaks: every
 * fixture in the suite reads its port back this way, so a server that
 * reported 0, or the configured value, would send every request nowhere.
 */
static void
test_port_zero_reports_bound_port(
	Fixture			*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autofree gchar *expected = NULL;
	guint16 port;

	(void)data;
	server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	/* Before the start it can only say what it was configured with. */
	g_assert_cmpuint(venture_web_server_get_port(server), ==, 0);

	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	port = venture_web_server_get_port(server);
	g_assert_cmpuint(port, >, 0);

	expected = g_strdup_printf("http://127.0.0.1:%u", (guint)port);
	g_assert_cmpstr(venture_web_server_get_base_url(server), ==, expected);
	g_assert_cmpuint(health(venture_web_server_get_base_url(server)), ==, 200);

	venture_web_server_stop(server);
}

/*
 * Two servers in one process, both on port 0, get two ports and both
 * answer. Several suites start more than one server per process; with a
 * port derived from the pid the second one collided with the first, or
 * with the first one's TIME_WAIT.
 */
static void
test_two_servers_one_process(
	Fixture			*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) first = NULL;
	g_autoptr(VentureWebServer) second = NULL;

	(void)data;
	first = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	second = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(first, &error));
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(second, &error));
	g_assert_no_error(error);

	g_assert_cmpuint(venture_web_server_get_port(first), !=, venture_web_server_get_port(second));
	g_assert_cmpuint(health(venture_web_server_get_base_url(first)), ==, 200);
	g_assert_cmpuint(health(venture_web_server_get_base_url(second)), ==, 200);

	venture_web_server_stop(second);
	venture_web_server_stop(first);
}

/*
 * The flake, reproduced: something already holds the port a fixture would
 * have chosen. A server told that port fails to start, and says so with
 * "Cannot listen on" -- the phrase the subprocess retry in test-docs keys
 * on, so changing the message without it turns a retried collision into a
 * hard failure. A server on port 0 beside the squatter starts regardless.
 */
static void
test_occupied_port(
	Fixture			*fixture,
	gconstpointer	 data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) squatter = NULL;
	g_autoptr(VentureWebServer) refused = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	guint16 held = 0;

	(void)data;
	squatter = occupy(0, &held);
	g_assert_cmpuint(held, >, 0);

	g_object_set(fixture->config, "server-port", (gint64)held, NULL);
	refused = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_false(venture_web_server_start(refused, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK);
	g_assert_nonnull(strstr(error->message, "Cannot listen on"));
	g_clear_error(&error);

	g_object_set(fixture->config, "server-port", (gint64)0, NULL);
	server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(venture_web_server_get_port(server), !=, held);
	g_assert_cmpuint(health(venture_web_server_get_base_url(server)), ==, 200);

	venture_web_server_stop(server);
	g_socket_listener_close(squatter);
}

/*
 * venture_test_free_port() hands back a port that can be bound, and never
 * one somebody is listening on. What breaks: a subprocess given a held
 * port fails to start on every attempt, and the bounded retry turns that
 * into a failure of a test that has nothing to do with ports.
 */
static void
test_free_port_helper(void)
{
	g_autoptr(GSocketListener) squatter = NULL;
	guint16 held = 0;
	guint i;

	squatter = occupy(0, &held);

	for (i = 0; i < 64; i++)
	{
		g_autoptr(GSocketListener) taker = NULL;
		guint16 port = venture_test_free_port();
		guint16 bound = 0;

		g_assert_cmpuint(port, >, 0);
		g_assert_cmpuint(port, !=, held);
		/* It must still be bindable: that is the whole promise. */
		taker = occupy(port, &bound);
		g_assert_cmpuint(bound, ==, port);
		g_socket_listener_close(taker);
	}

	g_socket_listener_close(squatter);
}

/*
 * No test derives a port from its pid again. Concurrent suites in other
 * worktrees or PID namespaces have equal pids and so equal ports, and the
 * arithmetic cannot see any other listener; the collision passes on a
 * rerun, which is what makes it expensive. The rule is kept by reading
 * the test sources, since the next fixture copied from an old one is the
 * way it would come back.
 */
static void
test_no_pid_derived_ports(void)
{
	const gchar *pid_call = "getpid" "()";
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *name;

	dir = g_dir_open("tests", 0, &error);
	if (NULL == dir)
	{
		g_test_skip("tests/ is not reachable from the working directory");
		return;
	}

	while (NULL != (name = g_dir_read_name(dir)))
	{
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		g_auto(GStrv) lines = NULL;
		guint i;

		if (!g_str_has_suffix(name, ".c") && !g_str_has_suffix(name, ".h") &&
		    !g_str_has_suffix(name, ".sh"))
			continue;
		path = g_build_filename("tests", name, NULL);
		g_assert_true(g_file_get_contents(path, &text, NULL, &error));
		g_assert_no_error(error);

		lines = g_strsplit(text, "\n", -1);
		for (i = 0; NULL != lines[i]; i++)
		{
			gchar *lower;
			gboolean suspect;

			if (NULL == strstr(lines[i], pid_call))
				continue;
			lower = g_ascii_strdown(lines[i], -1);
			suspect = NULL != strstr(lower, "port");
			g_free(lower);
			if (suspect)
				g_error("%s:%u derives a port from the pid: %s", path, i + 1, lines[i]);
		}
	}
}

gint
main(
	gint	 argc,
	gchar	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/ports/zero-reports-bound-port", Fixture, NULL,
	           fixture_set_up, test_port_zero_reports_bound_port, fixture_tear_down);
	g_test_add("/ports/two-servers-one-process", Fixture, NULL,
	           fixture_set_up, test_two_servers_one_process, fixture_tear_down);
	g_test_add("/ports/occupied-port", Fixture, NULL,
	           fixture_set_up, test_occupied_port, fixture_tear_down);
	g_test_add_func("/ports/free-port-helper", test_free_port_helper);
	g_test_add_func("/ports/no-pid-derived-ports", test_no_pid_derived_ports);

	return g_test_run();
}
