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

	fixture->state_dir = g_dir_make_tmp("venture-journal-actions-XXXXXX", NULL);
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

static gint64
account(Fixture *fixture, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, 1, NULL);
	row = venture_database_find_one(fixture->database, query, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}
static gint64
draft(Fixture *fixture, gboolean balanced)
{
	g_autoptr(VentureJournal) journal = venture_journal_new();
	g_autoptr(GDateTime) when = venture_time_from_string("2026-01-10", NULL);
	g_autoptr(GError) error = NULL;
	guint i;
	gint64 id;
	g_object_set(journal, "organization-id", (gint64)1, "source-type", "organization",
		"source-id", (gint64)1, "currency", "USD", "occurred-at", when, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(journal), NULL, &error));
	g_assert_no_error(error);
	id = venture_entity_get_id(VENTURE_ENTITY(journal));
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureJournalLine) line = venture_journal_line_new();
		g_autoptr(VentureMoney) amount = venture_money_new_for_currency(i && !balanced ? 900 : 1000, "USD");
		g_object_set(line, "journal-id", id, "organization-id", (gint64)1,
			"account-id", account(fixture, i ? "4000" : "1000"), "amount", amount,
			"side", i ? VENTURE_LEDGER_SIDE_CREDIT : VENTURE_LEDGER_SIDE_DEBIT, NULL);
		g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(line), NULL, &error));
		g_assert_no_error(error);
	}
	return id;
}
static gint
state(Fixture *fixture, gint64 id)
{
	g_autoptr(VentureEntity) journal = venture_database_get(fixture->database, VENTURE_TYPE_JOURNAL, id, NULL);
	gint result;
	g_object_get(journal, "state", &result, NULL);
	return result;
}
static gchar *
action_path(gint64 id, const gchar *action)
{
	return g_strdup_printf("/api/v1/journal/%" G_GINT64_FORMAT "/actions/%s", id, action);
}
static void
test_post_reverse(Fixture *fixture, gconstpointer data)
{
	gint64 id = draft(fixture, TRUE);
	g_autofree gchar *path = action_path(id, "post");
	g_autofree gchar *body = NULL;
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 200);
	g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_POSTED);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 409);
	g_clear_pointer(&path, g_free);
	path = action_path(id, "reverse");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{\"occurred_at\":\"2026-01-09\"}", &body), ==, 422);
	g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_POSTED);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{\"occurred_at\":\"2026-01-11\",\"memo\":\"Correction\"}", &body), ==, 200);
	g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_REVERSED);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 409);
}
static void
test_unbalanced(Fixture *fixture, gconstpointer data)
{
	gint64 id = draft(fixture, FALSE);
	g_autofree gchar *path = action_path(id, "post");
	g_autofree gchar *body = NULL;
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 422);
	g_assert_nonnull(strstr(body, "balance"));
	g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_DRAFT);
}
static gchar *
stage(Fixture *fixture, gint64 id, const gchar *action)
{
	g_autofree gchar *verb = g_strconcat(action, "?stage=1", NULL);
	g_autofree gchar *path = action_path(id, verb);
	g_autofree gchar *body = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *confirmation;
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 202);
	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	confirmation = json_object_get_object_member(json_node_get_object(json_parser_get_root(parser)), "confirmation");
	return g_strdup_printf("/api/v1/confirmations/%s/approve", json_object_get_string_member(confirmation, "id"));
}
static void
close_january(Fixture *fixture)
{
	g_autoptr(GDateTime) start = venture_time_from_string("2026-01-01", NULL);
	g_autoptr(VentureFiscalYear) year = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor closer;
	closer.kind = VENTURE_ACTOR_KIND_USER;
	closer.name = "journal-reviewer";
	closer.prompt = NULL;
	closer.request_id = NULL;
	closer.approved_by = NULL;
	year = venture_period_service_generate(venture_period_service_get(fixture->database), 1, "FY2026", start, VENTURE_PERIOD_MONTHLY, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(year);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(fixture->database, query, &error);
	g_assert_no_error(error);
	g_object_set(period, "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(fixture->database, period, &closer, &error));
	g_assert_no_error(error);
}
static void
test_confirmation(Fixture *fixture, gconstpointer data)
{
	gint64 id = draft(fixture, TRUE);
	g_autofree gchar *path = stage(fixture, id, "post");
	g_autofree gchar *body = NULL;
	g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_DRAFT);
	if (GPOINTER_TO_INT(data))
	{
		g_autofree gchar *direct = action_path(id, "post");
		close_january(fixture);
		g_assert_cmpuint(request(fixture, "POST", direct, "application/json", "{}", &body), ==, 409);
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 409);
		g_assert_nonnull(strstr(body, "FY2026"));
		g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_DRAFT);
		return;
	}
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 200);
	g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_POSTED);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = stage(fixture, id, "reverse");
	g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 200);
	g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_REVERSED);
}
static void
test_create_post(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *input = g_strdup_printf("{\"organization_id\":1,\"source_type\":\"organization\",\"source_id\":1,\"currency\":\"USD\",\"occurred_at\":\"2026-01-10\",\"lines\":[{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"debit\",\"amount\":\"10 USD\"},{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"credit\",\"amount\":\"10 USD\"}]}", account(fixture, "1000"), GPOINTER_TO_INT(data) == 1 ? (gint64)999999 : account(fixture, "4000"));
	g_autoptr(VentureQuery) journals = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(VentureQuery) lines = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	guint status = request(fixture, "POST", GPOINTER_TO_INT(data) == 2 ? "/api/v1/journals/post?stage=1" : "/api/v1/journals/post", "application/json", input, &body);
	if (GPOINTER_TO_INT(data) == 2)
	{
		g_autoptr(JsonNode) response = venture_json_parse(body, NULL);
		JsonObject *confirmation = json_object_get_object_member(json_node_get_object(response), "confirmation");
		g_autofree gchar *path = g_strdup_printf("/api/v1/confirmations/%s/approve", json_object_get_string_member(confirmation, "id"));
		g_assert_cmpuint(status, ==, 202);
		g_assert_cmpint(venture_database_count(fixture->database, journals, NULL), ==, 0);
		g_assert_cmpint(venture_database_count(fixture->database, lines, NULL), ==, 0);
		g_clear_pointer(&body, g_free);
		g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 200);
		g_assert_cmpint(venture_database_count(fixture->database, journals, NULL), ==, 1);
		g_assert_cmpint(venture_database_count(fixture->database, lines, NULL), ==, 2);
		return;
	}
	if (GPOINTER_TO_INT(data) == 1)
	{
		g_assert_cmpuint(status, >=, 400);
		g_assert_cmpint(venture_database_count(fixture->database, journals, NULL), ==, 0);
		g_assert_cmpint(venture_database_count(fixture->database, lines, NULL), ==, 0);
		g_test_message("Bad account response: %s", body);
		g_assert_nonnull(strstr(body, "999999"));
	}
	else
	{
		g_assert_cmpuint(status, ==, 201);
		g_assert_nonnull(strstr(body, "posted"));
		g_assert_cmpint(venture_database_count(fixture->database, journals, NULL), ==, 1);
		g_assert_cmpint(venture_database_count(fixture->database, lines, NULL), ==, 2);
	}
}
static void
test_tools(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(VentureMcpCatalog) catalog = NULL;
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(JsonNode) tools = NULL;
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/schema", NULL, NULL, &body), ==, 200);
	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	catalog = venture_mcp_catalog_new_from_schema(json_parser_get_root(parser), NULL);
	g_assert_true(venture_mcp_catalog_has_tool(catalog, "venture_journal_post"));
	g_object_set(fixture->config, "ai-provider", "ollama", NULL);
	ai = venture_ai_service_new(fixture->context, NULL);
	g_assert_nonnull(ai);
	tools = venture_ai_service_describe_tools(ai);
	g_clear_pointer(&body, g_free);
	body = venture_json_to_string(tools, FALSE);
	g_assert_nonnull(strstr(body, "venture_journal_post"));
	venture_config_set_module_enabled(fixture->config, "ledger", FALSE);
	g_clear_pointer(&tools, json_node_unref);
	tools = venture_ai_service_describe_tools(ai);
	g_clear_pointer(&body, g_free);
	body = venture_json_to_string(tools, FALSE);
	g_assert_null(strstr(body, "venture_journal_post"));
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
	gint mode = GPOINTER_TO_INT(data);
	gint64 id = mode == 2 ? 0 : draft(fixture, TRUE);
	g_autofree gchar *journal_arg = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *args[] = { "build/debug/venturectl", "--server", fixture->url,
		mode == 3 ? "--stage" : "--format=json", "act", "journal", id_text,
		mode == 1 ? "reverse" : mode == 2 ? "create_and_post" : "post", NULL, NULL };
	if (mode == 1)
	{
		g_autofree gchar *path = action_path(id, "post");
		g_autofree gchar *body = NULL;
		g_assert_cmpuint(request(fixture, "POST", path, "application/json", "{}", &body), ==, 200);
	}
	if (mode == 2)
	{
		journal_arg = g_strdup_printf("journal={\"organization_id\":1,\"source_type\":\"organization\",\"source_id\":1,\"currency\":\"USD\",\"occurred_at\":\"2026-01-10\",\"lines\":[{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"debit\",\"amount\":\"10 USD\"},{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"credit\",\"amount\":\"10 USD\"}]}", account(fixture, "1000"), account(fixture, "4000"));
		args[8] = journal_arg;
	}


	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_assert_true(g_subprocess_get_successful(child));
	if (mode == 3)
	{
		VentureConfirmationStore *store = venture_context_get_confirmations(fixture->context);
		g_autoptr(GPtrArray) pending = venture_confirmation_store_list_pending(store);
		g_autofree gchar *confirmation_id = NULL;
		g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_DRAFT);
		g_assert_cmpuint(pending->len, ==, 1);
		confirmation_id = g_strdup(venture_confirmation_get_id(g_ptr_array_index(pending, 0)));
		g_assert_true(venture_confirmation_store_approve_as(store, confirmation_id, "reviewer", VENTURE_USER_ROLE_OWNER, &error));
		g_assert_no_error(error);
		g_assert_cmpint(state(fixture, id), ==, VENTURE_JOURNAL_POSTED);
	}
	else g_assert_nonnull(strstr(result.out, "posted"));
	g_free(result.out);
	g_free(result.err);

}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/journal-actions/post-reverse", Fixture, NULL, fixture_set_up, test_post_reverse, fixture_tear_down);
	g_test_add("/journal-actions/unbalanced", Fixture, NULL, fixture_set_up, test_unbalanced, fixture_tear_down);
	g_test_add("/journal-actions/confirmation", Fixture, NULL, fixture_set_up, test_confirmation, fixture_tear_down);
	g_test_add("/journal-actions/closed-period", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_confirmation, fixture_tear_down);
	g_test_add("/journal-actions/create-post", Fixture, NULL, fixture_set_up, test_create_post, fixture_tear_down);
	g_test_add("/journal-actions/create-post-staged", Fixture, GINT_TO_POINTER(2), fixture_set_up, test_create_post, fixture_tear_down);
	g_test_add("/journal-actions/atomic-failure", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_create_post, fixture_tear_down);
	g_test_add("/journal-actions/cli", Fixture, NULL, fixture_set_up, test_cli, fixture_tear_down);
	g_test_add("/journal-actions/cli-reverse", Fixture, GINT_TO_POINTER(1), fixture_set_up, test_cli, fixture_tear_down);
	g_test_add("/journal-actions/cli-create-post", Fixture, GINT_TO_POINTER(2), fixture_set_up, test_cli, fixture_tear_down);
	g_test_add("/journal-actions/cli-stage", Fixture, GINT_TO_POINTER(3), fixture_set_up, test_cli, fixture_tear_down);
	g_test_add("/journal-actions/tools", Fixture, NULL, fixture_set_up, test_tools, fixture_tear_down);
	return g_test_run();
}
