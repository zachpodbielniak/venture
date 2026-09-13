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

	fixture->state_dir = g_dir_make_tmp("venture-action-surfaces-XXXXXX", NULL);
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

static gboolean
allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	g_object_get(entity, "name", &name, NULL);
	if (0 != g_strcmp0(name, "Reviewed")) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Already reviewed");
	return FALSE;
}
static VentureEntity *
invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	g_object_set(entity, "name", "Reviewed", NULL);
	if (!venture_database_save(venture_action_get_data(action), entity, actor, error)) return NULL;
	return g_object_ref(entity);
}
static void
install(Fixture *fixture)
{
	g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "organization",
		"name", "review", "label", "Review", "description", "Review this organization", "stageable", TRUE, NULL);
	g_assert_true(venture_action_registry_register(venture_database_get_action_registry(fixture->database),
		action, allowed, invoke, fixture->database, NULL, NULL));
}
static void
test_rest(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	install(fixture);
	g_assert_cmpuint(request(fixture, "POST", "/api/v1/organization/1/actions/review", "application/json", "{}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "Reviewed"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", "/api/v1/organization/1/actions/review", "application/json", "{}", &body), ==, 409);
	g_assert_nonnull(strstr(body, "conflict"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", "/api/v1/organization/1/actions/missing", "application/json", "{}", &body), ==, 404);
}
static void
test_schema(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(VentureMcpCatalog) catalog = NULL;
	g_autoptr(JsonNode) tools = NULL;
	install(fixture);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/schema/organization", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "\"actions\""));
	g_assert_nonnull(strstr(body, "Review this organization"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/schema", NULL, NULL, &body), ==, 200);
	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	catalog = venture_mcp_catalog_new_from_schema(json_parser_get_root(parser), NULL);
	g_assert_true(venture_mcp_catalog_has_tool(catalog, "venture_organization_review"));
	tools = venture_mcp_catalog_get_tools(catalog);
	g_clear_pointer(&body, g_free);
	body = venture_json_to_string(tools, FALSE);
	g_assert_nonnull(strstr(body, "venture_organization_review"));
}
static void
test_web(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	install(fixture);
	g_assert_cmpuint(request(fixture, "GET", "/e/organization/1", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "data-record-action=\"review\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", "/api/v1/organization/1/actions/review", "application/json", "{}", &body), ==, 200);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "GET", "/e/organization/1", NULL, NULL, &body), ==, 200);
	g_assert_null(strstr(body, "data-record-action=\"review\""));
}
static void
test_confirmation(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *path = NULL;
	g_autoptr(VentureEntity) before = NULL;
	g_autofree gchar *name = NULL;
	JsonObject *confirmation;
	install(fixture);
	g_assert_cmpuint(request(fixture, "POST", "/api/v1/organization/1/actions/review?stage=1", "application/json", "{}", &body), ==, 202);
	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	confirmation = json_object_get_object_member(json_node_get_object(json_parser_get_root(parser)), "confirmation");
	g_assert_cmpstr(json_object_get_string_member(confirmation, "action"), ==, "action");
	path = g_strdup_printf("/api/v1/confirmations/%s/approve", json_object_get_string_member(confirmation, "id"));
	before = venture_database_get(fixture->database, VENTURE_TYPE_ORGANIZATION, 1, NULL);
	g_object_get(before, "name", &name, NULL);
	g_assert_cmpstr(name, !=, "Reviewed");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 200);
	g_clear_object(&before);
	g_clear_pointer(&name, g_free);
	before = venture_database_get(fixture->database, VENTURE_TYPE_ORGANIZATION, 1, NULL);
	g_object_get(before, "name", &name, NULL);
	g_assert_cmpstr(name, ==, "Reviewed");
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
	const gchar *args[] = { "build/debug/venturectl", "--server", fixture->url,
		"act", "organization", "1", "review", NULL };

	install(fixture);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_nonnull(strstr(result.out, "Reviewed"));
	g_free(result.out);
	g_free(result.err);

}

static void
test_assistant(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureAiService) service = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_autofree gchar *text = NULL;
	install(fixture);
	g_object_set(fixture->config, "ai-provider", "ollama", NULL);
	service = venture_ai_service_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(service);
	tools = venture_ai_service_describe_tools(service);
	text = venture_json_to_string(tools, FALSE);
	g_assert_nonnull(strstr(text, "venture_organization_review"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/action-surfaces/rest", Fixture, NULL, fixture_set_up, test_rest, fixture_tear_down);
	g_test_add("/action-surfaces/schema-mcp", Fixture, NULL, fixture_set_up, test_schema, fixture_tear_down);
	g_test_add("/action-surfaces/web", Fixture, NULL, fixture_set_up, test_web, fixture_tear_down);
	g_test_add("/action-surfaces/confirmation", Fixture, NULL, fixture_set_up, test_confirmation, fixture_tear_down);
	g_test_add("/action-surfaces/cli", Fixture, NULL, fixture_set_up, test_cli, fixture_tear_down);
	g_test_add("/action-surfaces/assistant", Fixture, NULL, fixture_set_up, test_assistant, fixture_tear_down);
	return g_test_run();
}
