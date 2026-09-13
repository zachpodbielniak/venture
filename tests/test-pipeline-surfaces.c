/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *url;
} Fixture;

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	gboolean started;
	guint port;
	/* Find an available port: concurrent PID namespaces have equal PIDs,
	 * and GTest resets its random stream before each case. */
	port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(port, >, 0);
	g_clear_object(&probe);

	fixture->state_dir = g_dir_make_tmp("venture-pipeline-surfaces-XXXXXX", NULL);
	fixture->url = g_strdup_printf("http://127.0.0.1:%u", port);
	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir,
		"server-bind-address", "127.0.0.1", "server-port", (gint64)port,
		"security-require-auth", FALSE, NULL);
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	started = venture_web_server_start(fixture->server, &error);
	g_assert_no_error(error);
	g_assert_true(started);
	fixture->session = soup_session_new();
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	venture_web_server_stop(fixture->server);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_test_remove_tree(fixture->state_dir);
	g_free(fixture->state_dir);
	g_free(fixture->url);
}

typedef struct
{
	gboolean done;
	GBytes *body;
	GError *error;
} Request;

static void
request_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Request *request = data;
	request->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &request->error);
	request->done = TRUE;
}

static guint
request(Fixture *fixture, const gchar *method, const gchar *path, const gchar *mime, const gchar *body, gchar **response)
{
	g_autofree gchar *url = g_strconcat(fixture->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Request result = { FALSE, NULL, NULL };
	guint status;

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, mime, bytes);
	}
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	*response = g_strndup(g_bytes_get_data(result.body, NULL), g_bytes_get_size(result.body));
	status = soup_message_get_status(message);
	g_bytes_unref(result.body);
	return status;
}


static void
test_rest_move(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *payload = NULL;
	g_autoptr(VentureDeal) deal = venture_deal_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(VentureEntity) stage = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(deal, "name", "HTTP move", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(deal), NULL, &error));
	venture_query_add_filter_int(query, "position", VENTURE_FILTER_OP_EQ, 1, NULL);
	stage = venture_database_find_one(fixture->database, query, &error);
	g_assert_nonnull(stage);
	path = g_strdup_printf("/api/v1/deals/%" G_GINT64_FORMAT "/move%s", venture_entity_get_id(VENTURE_ENTITY(deal)), GPOINTER_TO_INT(data) ? "?stage=1" : "");
	payload = g_strdup_printf("{\"stage_id\":%" G_GINT64_FORMAT ",\"note\":\"Qualified\"}", venture_entity_get_id(stage));
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", payload, &body), ==, GPOINTER_TO_INT(data) ? 202 : 200);
	if (GPOINTER_TO_INT(data))
	{
		g_autoptr(GPtrArray) pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(fixture->context));
		VentureConfirmation *confirmation;
		g_autofree gchar *id = NULL;
		g_assert_cmpuint(pending->len, ==, 1);
		{
			g_autoptr(VentureEntity) current = venture_database_get(fixture->database, VENTURE_TYPE_DEAL, venture_entity_get_id(VENTURE_ENTITY(deal)), &error);
			gint64 original_stage, current_stage;
			g_object_get(deal, "stage-id", &original_stage, NULL);
			g_object_get(current, "stage-id", &current_stage, NULL);
			g_assert_cmpint(current_stage, ==, original_stage);
		}
		confirmation = g_ptr_array_index(pending, 0);
		id = g_strdup(venture_confirmation_get_id(confirmation));
		g_assert_true(venture_confirmation_store_approve(venture_context_get_confirmations(fixture->context), id, "ben", &error));
		g_assert_no_error(error);
	}
	else
		g_assert_nonnull(strstr(body, "qualified"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/deals", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "name=\"stage_id\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/reports/forecast?period=all_time&pipeline_id=999999", NULL, NULL, &body), ==, 200);
	{
		g_autoptr(JsonNode) parsed = json_from_string(body, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(parsed), "rows")), ==, 0);
	}

}
typedef struct
{
	gboolean done;
	gchar *out;
	gchar *err;
	GError *error;
} CliResult;

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliResult *outcome = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &outcome->out, &outcome->err, &outcome->error);
	outcome->done = TRUE;
}

static void
test_cli(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	CliResult result = { FALSE, NULL, NULL, NULL };
	g_autoptr(VentureDeal) deal = venture_deal_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(VentureEntity) stage = NULL;
	g_autofree gchar *deal_id = NULL;
	g_autofree gchar *stage_id = NULL;
	const gchar *args[] = { "build/debug/venturectl", "--server", fixture->url,
		"deal", "move", NULL, NULL, "CLI qualification", NULL };
	(void)data;
	g_object_set(deal, "name", "CLI move", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(deal), NULL, &error));
	venture_query_add_filter_int(query, "position", VENTURE_FILTER_OP_EQ, 1, NULL);
	stage = venture_database_find_one(fixture->database, query, &error);
	g_assert_nonnull(stage);
	deal_id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(deal)));
	stage_id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(stage));
	args[5] = deal_id;
	args[6] = stage_id;

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(strstr(result.out, "qualified"));
	g_free(result.out);
	g_free(result.err);

}


int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/pipeline-surfaces/rest", Fixture, NULL, fixture_set_up, test_rest_move, fixture_tear_down);
	g_test_add("/pipeline-surfaces/staged", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_rest_move, fixture_tear_down);
	g_test_add("/pipeline-surfaces/cli", Fixture, NULL, fixture_set_up, test_cli, fixture_tear_down);
	return g_test_run();
}
