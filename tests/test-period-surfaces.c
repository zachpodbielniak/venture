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
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	VentureActor closer = actor();
	gboolean started;
	guint port;
	/* Find an available port: concurrent PID namespaces have equal PIDs,
	 * and GTest resets its random stream before each case. */
	port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(port, >, 0);
	g_clear_object(&probe);

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
	g_assert_cmpuint(request(fixture, "POST", "/api/v1/expense", "application/json",
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
	status = request(fixture, "POST", "/e/expense", "application/x-www-form-urlencoded",
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

static void
test_report_as_of(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *live = NULL;
	const gchar *path = "/api/v1/reports/pnl?period=2024-01&as_of=2024-01-31";
	g_object_set(expense, "description", "Paper", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(expense), "occurred-at", "2024-01-15", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(expense), "amount", "10.00 USD", NULL));
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(expense), NULL, NULL));
	g_assert_cmpuint(request(fixture, "GET", path, NULL, NULL, &before), ==, 200);
	g_assert_true(venture_database_delete(fixture->database, VENTURE_ENTITY(expense), NULL, NULL));
	g_assert_cmpuint(request(fixture, "GET", path, NULL, NULL, &after), ==, 200);
	g_assert_cmpuint(request(fixture, "GET", "/api/v1/reports/pnl?period=2024-01", NULL, NULL, &live), ==, 200);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpstr(before, !=, live);
}

static void
test_invalid_cutoff(Fixture *fixture, gconstpointer data)
{
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureDateRange) range = venture_date_range_new_month(2024, 1, NULL);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(fixture->context), "pnl");
	json_object_set_string_member(options, "as_of", data);
	result = venture_report_generate(report, fixture->context, range, options, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
}

static void
test_timestamp_precision(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EXPENSE);
	g_autoptr(GDateTime) cutoff = venture_time_from_string("2024-01-15T12:00:00Z", NULL);
	g_autoptr(GDateTime) deleted = venture_time_from_string("2024-01-15T12:00:00.500000Z", NULL);
	g_object_set(expense, "description", "Paper", "organization-id", (gint64)1,
		"occurred-at", cutoff, "deleted-at", deleted, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(expense), NULL, NULL));
	venture_query_set_as_of(query, cutoff);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL), ==, 1);
	venture_query_set_as_of(query, deleted);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL), ==, 0);
}

/* February 1 belongs to the next period, including in the receivables
 * snapshot, whose query has no movement-date range of its own. */
static void
test_snapshot_boundary(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) range = venture_date_range_new_month(2024, 1, NULL);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureReportResult) expected = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_REPORT_SNAPSHOT);
	g_autoptr(VentureEntity) snapshot = NULL;
	g_autoptr(JsonNode) a = NULL;
	g_autoptr(JsonNode) b = NULL;
	g_autofree gchar *totals = NULL;
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(fixture->context), "receivables");
	g_object_set(customer, "name", "February customer", "organization-id", (gint64)1, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(customer), NULL, &error));
	g_assert_no_error(error);
	g_object_set(invoice, "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)), NULL);
	g_object_set(invoice, "number", "FEB1", "organization-id", (gint64)1, "status", VENTURE_INVOICE_STATUS_DRAFT, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", "2024-02-01", NULL));
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(invoice), NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
		g_autoptr(VentureMoney) price = venture_money_new_for_currency(10000, "USD");
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"organization-id", (gint64)1, "description", "February work", "quantity", 1.0, "unit-price", price, NULL);
		g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(line), NULL, NULL));
	}
	/* Issue through the shared lifecycle so the cutoff test has real evidence. */
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(invoice), NULL, &error));
	g_assert_no_error(error);
	json_object_set_string_member(options, "as_of", "2024-01-31");
	expected = venture_report_generate(report, fixture->context, range, options, NULL);
	g_assert_nonnull(expected);
	a = venture_report_result_to_json(expected);
	close_period(fixture, FALSE);
	venture_query_add_filter_string(query, "report", VENTURE_FILTER_OP_EQ, "receivables", NULL);
	snapshot = venture_database_find_one(fixture->database, query, NULL);
	g_assert_nonnull(snapshot);
	g_object_get(snapshot, "totals", &totals, NULL);
	b = venture_json_parse(totals, NULL);
	g_assert_nonnull(b);
	g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(a), "metrics"),
		json_object_get_member(json_node_get_object(b), "metrics")));
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
	g_test_add("/periods/surfaces/report/as-of", Fixture, NULL, fixture_set_up, test_report_as_of, fixture_tear_down);
	g_test_add("/periods/surfaces/report/invalid-day", Fixture, "2024-02-30", fixture_set_up, test_invalid_cutoff, fixture_tear_down);
	g_test_add("/periods/surfaces/report/invalid-suffix", Fixture, "2024-01-31junk", fixture_set_up, test_invalid_cutoff, fixture_tear_down);
	g_test_add("/periods/surfaces/report/snapshot-boundary", Fixture, NULL, fixture_set_up, test_snapshot_boundary, fixture_tear_down);
	g_test_add("/periods/surfaces/report/timestamp-precision", Fixture, NULL, fixture_set_up, test_timestamp_precision, fixture_tear_down);
#undef ADD
	return g_test_run();
}
