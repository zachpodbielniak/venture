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
	VentureEntity *period;
	gchar *state_dir;
	gchar *url;
} Fixture;

static VentureActor
actor(void)
{
	VentureActor result;
	result.kind = VENTURE_ACTOR_KIND_USER;
	result.name = "period-closer";
	result.prompt = NULL;
	result.request_id = NULL;
	result.approved_by = NULL;
	return result;
}

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = venture_time_from_string("2024-01-01", NULL);
	g_autoptr(VentureFiscalYear) year = NULL;
	g_autoptr(VentureQuery) query = NULL;
	VentureActor closer = actor();
	guint port = 45000 + (getpid() % 10000);

	fixture->state_dir = g_dir_make_tmp("venture-period-surfaces-XXXXXX", NULL);
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
	year = venture_period_service_generate(venture_period_service_get(fixture->database),
		1, "FY2024", start, VENTURE_PERIOD_MONTHLY, &closer, &error);
	g_assert_no_error(error);
	g_assert_nonnull(year);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	fixture->period = venture_database_find_one(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->period);
	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->session = soup_session_new();
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	venture_web_server_stop(fixture->server);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->period);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_test_remove_tree(fixture->state_dir);
	g_free(fixture->state_dir);
	g_free(fixture->url);
}

static void
close_period(Fixture *fixture, gboolean locked)
{
	g_autoptr(GError) error = NULL;
	VentureActor closer = actor();
	g_object_set(fixture->period, "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(fixture->database, fixture->period, &closer, &error));
	g_assert_no_error(error);
	if (locked)
	{
		g_object_set(fixture->period, "state", VENTURE_PERIOD_LOCKED, NULL);
		g_assert_true(venture_database_save(fixture->database, fixture->period, &closer, &error));
		g_assert_no_error(error);
	}
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
request(Fixture *fixture, const gchar *path, const gchar *mime, const gchar *body, gchar **response)
{
	g_autofree gchar *url = g_strconcat(fixture->url, path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("POST", url);
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
	Request result = { FALSE, NULL, NULL };
	guint status;

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_set_request_body_from_bytes(message, mime, bytes);
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
assert_empty(Fixture *fixture)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EXPENSE);
	g_autoptr(GError) error = NULL;
	g_assert_cmpint(venture_database_count(fixture->database, query, &error), ==, 0);
	g_assert_no_error(error);
}

static void
test_rest(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	close_period(fixture, GPOINTER_TO_INT(data));
	g_assert_cmpuint(request(fixture, "/api/v1/expense", "application/json",
		"{\"description\":\"Paper\",\"organization_id\":1,\"occurred_at\":\"2024-01-15\",\"amount\":\"10.00 USD\"}", &body), ==, 409);
	g_assert_nonnull(strstr(body, "FY2024 P01"));
	g_assert_nonnull(strstr(body, GPOINTER_TO_INT(data) ? "locked" : "closed"));
	assert_empty(fixture);
}

static void
test_web(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	guint status;
	close_period(fixture, GPOINTER_TO_INT(data));
	status = request(fixture, "/e/expense", "application/x-www-form-urlencoded",
		"description=Paper&organization_id=1&occurred-at=2024-01-15&amount=10.00+USD", &body);
	g_assert_cmpuint(status, >=, 400);
	g_assert_nonnull(strstr(body, "FY2024 P01"));
	g_assert_nonnull(strstr(body, GPOINTER_TO_INT(data) ? "locked" : "closed"));
	assert_empty(fixture);
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
		"create", "expense", "description=Paper", "organization_id=1",
		"occurred_at=2024-01-15", "amount=10.00 USD", NULL };

	close_period(fixture, GPOINTER_TO_INT(data));
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	g_assert_false(g_subprocess_get_successful(child));
	g_assert_nonnull(strstr(result.err, "FY2024 P01"));
	g_assert_nonnull(strstr(result.err, GPOINTER_TO_INT(data) ? "locked" : "closed"));
	g_free(result.out);
	g_free(result.err);
	assert_empty(fixture);
}

/* The assistant's approval path must recheck the period: a proposal made
 * before close must not become a back door when approved after close. */
static void
test_ai_approval(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(GError) error = NULL;
	VentureConfirmationStore *store = venture_context_get_confirmations(fixture->context);
	VentureConfirmation *confirmation;
	VentureActor origin = actor();
	origin.kind = VENTURE_ACTOR_KIND_AI;
	origin.name = "assistant";
	origin.prompt = "Record the January paper expense";
	g_object_set(expense, "description", "Paper", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(expense), "occurred-at", "2024-01-15", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(expense), "amount", "10.00 USD", NULL));
	confirmation = venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_CREATE,
		VENTURE_ENTITY(expense), NULL, &origin, "assistant", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	close_period(fixture, GPOINTER_TO_INT(data));
	g_assert_false(venture_confirmation_store_approve(store, venture_confirmation_get_id(confirmation), "owner", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "FY2024 P01"));
	g_assert_nonnull(strstr(error->message, GPOINTER_TO_INT(data) ? "locked" : "closed"));
	assert_empty(fixture);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(surface, function) \
	g_test_add("/periods/surfaces/" surface "/closed", Fixture, GINT_TO_POINTER(0), fixture_set_up, function, fixture_tear_down); \
	g_test_add("/periods/surfaces/" surface "/locked", Fixture, GINT_TO_POINTER(1), fixture_set_up, function, fixture_tear_down)
	ADD("rest", test_rest);
	ADD("web", test_web);
	ADD("cli", test_cli);
	ADD("ai", test_ai_approval);
#undef ADD
	return g_test_run();
}
