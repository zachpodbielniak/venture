/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Covers src/banking/venture-bank-records.c and venture-bank-match-service.c */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

static void
test_records(void)
{
	const gchar *names[] = { "bank_account", "bank_statement", "bank_transaction", "bank_match",
		"reconciliation", "bank_rule", "bank_transfer" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	}
}

/* Generic writes must never manufacture matched evidence. */
static void
test_match_guard(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureBankMatch) match = venture_bank_match_new();
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	venture_entity_set_organization_id(VENTURE_ENTITY(match), venture_context_get_default_organization_id(context));
	g_object_set(match, "record-type", "expense", "record-id", (gint64)1, "kind", "exact", NULL);
	g_assert_false(venture_database_save(db, VENTURE_ENTITY(match), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureBankMatchService"));
}

/* The public action is also the boundary used by HTTP and CLI adapters. */

static void
test_import_match_reconcile(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureBankAccount) bank = venture_bank_account_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GPtrArray) transactions = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	g_autoptr(VentureEntity) statement = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	VentureBankMatchService *service;
	gint64 org, id;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", &error));
	accounts = venture_database_find(db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(accounts->len, ==, 1);
	g_object_set(bank, "name", "Checking", "account-id", venture_entity_get_id(g_ptr_array_index(accounts, 0)),
		"currency", "USD", "date-column", "Date", "amount-column", "Amount",
		"description-column", "Memo", "reference-column", "Ref", "external-id-column", "ID",
		"date-format", "%Y-%m-%d", "sign-convention", "normal", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bank), org);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(bank), NULL, &error));
	service = venture_database_get_bank_match_service(db);
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "0 USD");
	json_object_set_string_member(args, "closing_balance", "-10 USD");
	json_object_set_string_member(args, "format", "csv");
	json_object_set_string_member(args, "data", "Date,Amount,Memo,Ref,ID\n2026-01-10,-10,Fee,January,fee-1\n");
	statement = venture_bank_match_service_execute(service, "import", venture_entity_get_id(VENTURE_ENTITY(bank)), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	result = venture_bank_match_service_execute(service, "import", venture_entity_get_id(VENTURE_ENTITY(bank)), args, NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	transactions = venture_database_find(db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(transactions->len, ==, 1);
	id = venture_entity_get_id(g_ptr_array_index(transactions, 0));
	result = venture_bank_match_service_execute(service, "reconcile", venture_entity_get_id(statement), args, NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* Creation must post through the expense service and match atomically. */
	json_object_set_string_member(args, "type", "expense");
	result = venture_bank_match_service_execute(service, "create", id, args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "unmatch", id, args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	candidates = venture_bank_transaction_candidates(db, result, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(candidates->len, ==, 1);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "auto", venture_entity_get_id(statement), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "reconcile", venture_entity_get_id(statement), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_object_get(result, "difference", &balance, NULL);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
	g_clear_object(&result);
	result = venture_bank_match_service_execute(service, "unmatch", id, args, NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
}

static void
test_report_registration(void)
{
	g_autoptr(VentureReportRegistry) registry = venture_report_registry_new();
	venture_report_registry_register_builtins(registry);
	g_assert_nonnull(venture_report_registry_lookup(registry, "bank_reconciliation"));
}

typedef struct
{
	VentureDatabase *database;
	VentureConfig *config;
	VentureContext *context;
	VentureEntity *bank;
	gint64 org;
} BankFixture;

static void
bank_setup(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	(void)data;
	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	f->config = venture_config_new();
	f->context = venture_context_new(f->config, f->database);
	f->org = venture_context_get_default_organization_id(f->context);
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", &error));
	accounts = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(accounts->len, ==, 1);
	f->bank = VENTURE_ENTITY(venture_bank_account_new());
	venture_entity_set_organization_id(f->bank, f->org);
	g_object_set(f->bank, "name", "Bank", "currency", "USD", "account-id", venture_entity_get_id(g_ptr_array_index(accounts, 0)),
		"date-column", "date", "amount-column", "amount", "description-column", "memo", "reference-column", "ref",
		"external-id-column", "id", "date-format", "%Y-%m-%d", "sign-convention", "normal", NULL);
	g_assert_true(venture_database_save(f->database, f->bank, NULL, &error));
	g_assert_no_error(error);
}

static void
bank_teardown(BankFixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->bank);
	g_clear_object(&f->context);
	g_clear_object(&f->config);
	g_clear_object(&f->database);
}

static VentureEntity *
bank_action(BankFixture *f, const gchar *action, gint64 id, const gchar *json, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return venture_bank_match_service_execute(venture_database_get_bank_match_service(f->database), action, id,
		json_node_get_object(json_parser_get_root(parser)), NULL, error);
}

static void post_cash(BankFixture *f, const gchar *when, gint64 amount, gboolean debit);

static VentureEntity *
bank_import(BankFixture *f, const gchar *format, const gchar *data, const gchar *closing, GError **error)
{
	g_autoptr(JsonObject) args = json_object_new();
	json_object_set_string_member(args, "format", format);
	json_object_set_string_member(args, "data", data);
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "0 USD");
	json_object_set_string_member(args, "closing_balance", closing);
	return venture_bank_match_service_execute(venture_database_get_bank_match_service(f->database), "import",
		venture_entity_get_id(f->bank), args, NULL, error);
}

static gboolean journal_credits_account(BankFixture *f, gint64 account_id);

static guint
bank_count(BankFixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) found = NULL;
	venture_query_set_limit(query, 0);
	found = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	return found->len;
}

static void
test_account_evidence_guard(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) fake = venture_money_new_for_currency(9000, "USD");
	(void)data;
	g_object_set(f->bank, "last-statement-balance", fake, NULL);
	g_assert_false(venture_database_save(f->database, f->bank, NULL, &error));
	g_assert_nonnull(error);
}

/* GDateDay is narrower than an input integer: validate before converting,
 * or an invalid CSV day can pass GDate and abort in GDateTime. */
static void
test_invalid_calendar_day(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	(void)data;
	g_object_set(f->bank, "date-format", "%m/%d/%Y", NULL);
	g_assert_true(venture_database_save(f->database, f->bank, NULL, &error));
	g_assert_no_error(error);
	statement = bank_import(f, "csv",
		"date,amount,memo,ref,id\n01/257/2026,-1,Fee,January,invalid-date\n",
		"-1 USD", &error);
	g_assert_null(statement);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_STATEMENT), ==, 0);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_TRANSACTION), ==, 0);
}

static void
test_ofx_rollback(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	const gchar *ofx = "OFXHEADER:100\n<OFX><BANKTRANLIST><STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>ofx-1<MEMO>Fee</STMTTRN></BANKTRANLIST></OFX>";
	(void)data;
	statement = bank_import(f, "ofx", ofx, "-10 USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	result = bank_import(f, "qfx", ofx, "-10 USD", &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* Overlapping FITIDs are skipped; a new identity in the same file is kept. */
	result = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-11,-5,Other,x,new-id\n2026-01-10,-10,Fee,x,ofx-1\n", "-15 USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_STATEMENT), ==, 2);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_TRANSACTION), ==, 2);
	result = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-11,-5 EUR,Other,x,new-id\n", "-5 USD", &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	result = bank_action(f, "exclude", 1, "{}", &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	result = bank_action(f, "exclude", 1, "{\"reason\":\"bank duplicate\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	/* Excluding a real movement cannot manufacture a balanced reconciliation. */
	result = bank_action(f, "reconcile", venture_entity_get_id(statement), "{}", &error);
	g_assert_null(result);
	g_assert_nonnull(error);
}

static void
test_split_and_atomic_creation(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	g_autoptr(VentureExpense) first = venture_expense_new(), second = venture_expense_new();
	g_autoptr(VentureMoney) four = venture_money_new_for_currency(400, "USD"), six = venture_money_new_for_currency(600, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 10, 0, 0, 0);
	(void)data;
	venture_entity_set_organization_id(VENTURE_ENTITY(first), f->org);
	venture_entity_set_organization_id(VENTURE_ENTITY(second), f->org);
	g_object_set(first, "description", "First", "amount", four, "occurred-at", date, NULL);
	g_object_set(second, "description", "Second", "amount", six, "occurred-at", date, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(first), NULL, &error));
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(second), NULL, &error));
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Split,x,split-1\n", "-10 USD", &error);
	g_assert_no_error(error);
	result = bank_action(f, "match", 1, "{\"parts\":[{\"type\":\"expense\",\"id\":1},{\"type\":\"expense\",\"id\":999}]}", &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 0);
	result = bank_action(f, "match", 1, "{\"parts\":[{\"type\":\"expense\",\"id\":1},{\"type\":\"expense\",\"id\":2}]}", &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 2);
	result = bank_action(f, "create", 1, "{\"type\":\"expense\"}", &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 2);
	result = bank_action(f, "reconcile", venture_entity_get_id(statement), "{}", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_false(venture_database_delete(f->database, result, NULL, &error));
	g_assert_nonnull(error);
}
typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
	gchar *out;
	gchar *err;
} SurfaceResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response;

	response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SurfaceResult response;
	guint status;

	memset(&response, 0, sizeof(response));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	status = soup_message_get_status(message);
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return status;
}

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response;

	response = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &response->out, &response->err, &response->error);
	response->done = TRUE;
}

static gboolean
cli_timeout(gpointer data)
{
	g_subprocess_force_exit(G_SUBPROCESS(data));
	return G_SOURCE_CONTINUE;
}

static gchar *
run_cli(const gchar *const *argv, const gchar *input, gboolean success)
{
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	SurfaceResult response;
	guint timeout;

	memset(&response, 0, sizeof(response));
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	/* MCP insists on a token even when the private loopback fixture has
	 * authentication disabled. Never inherit a real install's credential. */
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "banking-test-only", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, input, NULL, cli_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(response.error);
	if (g_subprocess_get_successful(process) != success)
		g_test_message("CLI stdout: %s; stderr: %s", response.out, response.err);
	g_assert_cmpint(g_subprocess_get_successful(process), ==, success);
	g_free(response.err);
	return response.out;
}

static VentureWebServer *
start_server(BankFixture *f, gchar **state_dir)
{
	g_autoptr(GSocketListener) listener = NULL;
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	guint16 port;

	*state_dir = g_dir_make_tmp("venture-banking-XXXXXX", &error);
	g_assert_no_error(error);
	listener = g_socket_listener_new();
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", *state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}


static void
test_surfaces(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	g_autofree gchar *directory = NULL, *body = NULL;
	g_autofree gchar *cli = g_canonicalize_filename("build/debug/venturectl", NULL);
	const gchar *unmatch_argv[] = { cli, "--server", NULL, "-f", "json", "bank", "unmatch", "1", NULL };
	const gchar *auto_argv[] = { cli, "--server", NULL, "-f", "json", "bank", "match", "AUTO", "1", NULL };
	(void)data;
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Fee,x,web-1\n", "-10 USD", &error);
	g_assert_no_error(error);
	server = start_server(f, &directory);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_transactions/1/exclude", "application/json", "{\"reason\":\"review\"}", NULL), ==, 200);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_transactions/1/unmatch", "application/json", "{}", NULL), ==, 200);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_transactions/1/create", "application/json", "{\"type\":\"expense\"}", &body), ==, 200);
	g_clear_pointer(&body, g_free);
	unmatch_argv[2] = venture_web_server_get_base_url(server);
	auto_argv[2] = venture_web_server_get_base_url(server);
	body = run_cli(unmatch_argv, NULL, TRUE);
	g_clear_pointer(&body, g_free);
	body = run_cli(auto_argv, NULL, TRUE);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 1);
	g_assert_cmpuint(http_request(server, "POST", "/banking/1/action", "application/x-www-form-urlencoded", "action=unmatch", NULL), ==, 302);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 0);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_statements/1/auto", "application/json", "{}", NULL), ==, 200);
	g_assert_cmpuint(http_request(server, "GET", "/api/v1/reports/bank_reconciliation?statement_id=1", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "difference"));
	g_assert_nonnull(strstr(body, "outstanding_checks"));
	g_assert_nonnull(strstr(body, "deposits_in_transit"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "GET", "/e/bank_statement/1", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "reopen"));
	g_assert_nonnull(strstr(body, "Reopen reason"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_statements/1/reconcile", "application/json", "{}", NULL), ==, 200);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_transactions/1/unmatch", "application/json", "{}", NULL), ==, 422);
	venture_config_set_module_enabled(f->config, "banking", FALSE);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_transactions/1/unmatch", "application/json", "{}", NULL), ==, 404);
	venture_config_set_module_enabled(f->config, "banking", TRUE);
	venture_web_server_stop(server);
	g_clear_object(&server);
	g_object_set(f->config, "security-require-auth", TRUE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/bank_transactions/1/match", "application/json", "{}", NULL), ==, 401);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(directory);
}

static gboolean
reject_bank_match(VentureDatabase *db, VentureEntity *record, VentureEntity *previous, gpointer data, GError **error)
{
	(void)db; (void)record; (void)previous; (void)data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "Injected match persistence failure");
	return FALSE;
}

static void
test_posting_rollback(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	guint before;
	(void)data;
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Fee,x,rollback\n", "-10 USD", &error);
	g_assert_no_error(error);
	before = bank_count(f, VENTURE_TYPE_AUDIT_ENTRY);
	venture_database_add_save_validator(f->database, VENTURE_TYPE_BANK_MATCH, reject_bank_match, NULL, NULL);
	result = bank_action(f, "create", 1, "{\"type\":\"expense\"}", &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 0);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 0);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_AUDIT_ENTRY), ==, before);
}

static void
test_receipt(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	(void)data;
	g_object_set(customer, "name", "Customer", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(customer), NULL, &error));
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,10,Receipt,x,receipt\n", "10 USD", &error);
	g_assert_no_error(error);
	result = bank_action(f, "create", 1, "{\"type\":\"receipt\",\"customer_id\":1}", &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_PAYMENT), ==, 1);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 1);
	result = bank_action(f, "reconcile", venture_entity_get_id(statement), "{}", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
}

static void
test_period_guard(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(GDateTime) end = g_date_time_add_years(start, 1);
	g_autoptr(VentureEntity) year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "name", "2026", "organization-id", f->org,
		"start-at", start, "end-at", end, "period-length", VENTURE_PERIOD_MONTHLY, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	VentureActor actor;
	(void)data;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "bank-test";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	g_assert_true(venture_database_save(f->database, year, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, &error));
	periods = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_object_set(g_ptr_array_index(periods, 0), "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->database, g_ptr_array_index(periods, 0), &actor, &error));
	g_assert_no_error(error);
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Fee,x,closed\n", "-10 USD", &error);
	g_assert_no_error(error);
	result = bank_action(f, "create", 1, "{\"type\":\"expense\"}", &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 0);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 0);
}

static gint64
bank_account_code(BankFixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, &error));
	accounts = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(accounts->len, ==, 1);
	return venture_entity_get_id(g_ptr_array_index(accounts, 0));
}

static VentureEntity *
bank_issue(BankFixture *f, const gchar *number, const gchar *amount)
{
	g_autoptr(VentureEntity) invoice = g_object_new(VENTURE_TYPE_INVOICE, NULL);
	g_autoptr(VentureEntity) line = g_object_new(VENTURE_TYPE_INVOICE_LINE, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 8, 0, 0, 0);
	venture_entity_set_organization_id(invoice, f->org);
	g_object_set(invoice, "number", number, "company-id", (gint64)1, "issued-at", date, "due-at", date, NULL);
	g_assert_true(venture_database_save(f->database, invoice, NULL, &error));
	venture_entity_set_organization_id(line, f->org);
	money = venture_money_from_string(amount, "USD", &error);
	g_assert_no_error(error);
	g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", number,
		"quantity", 1.0, "unit-price", money, NULL);
	g_assert_true(venture_database_save(f->database, line, NULL, &error));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(f->database, invoice, NULL, &error));
	return g_steal_pointer(&invoice);
}

/* One deposit can clear several receipts; a fee is an explicit approved journal. */
static void
test_grouped_deposit_adjustment(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureEntity) first = NULL, second = NULL;
	g_autoptr(VentureEntity) pay_a = g_object_new(VENTURE_TYPE_PAYMENT, NULL);
	g_autoptr(VentureEntity) pay_b = g_object_new(VENTURE_TYPE_PAYMENT, NULL);
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 10, 0, 0, 0);
	g_autoptr(VentureMoney) forty = venture_money_new_for_currency(4000, "USD");
	g_autoptr(VentureMoney) sixty = venture_money_new_for_currency(6000, "USD");
	g_autofree gchar *json = NULL;
	(void)data;
	g_object_set(customer, "name", "Grouped", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(customer), NULL, &error));
	first = bank_issue(f, "GRP-A", "40 USD");
	second = bank_issue(f, "GRP-B", "60 USD");
	venture_entity_set_organization_id(pay_a, f->org);
	venture_entity_set_organization_id(pay_b, f->org);
	g_object_set(pay_a, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"invoice-id", venture_entity_get_id(first), "amount", forty, "date", date, "method", "transfer", NULL);
	g_object_set(pay_b, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"invoice-id", venture_entity_get_id(second), "amount", sixty, "date", date, "method", "transfer", NULL);
	g_assert_true(venture_database_save(f->database, pay_a, NULL, &error));
	g_assert_true(venture_database_save(f->database, pay_b, NULL, &error));
	statement = bank_import(f, "csv",
		"date,amount,memo,ref,id\n2026-01-10,97,Stripe payout,batch,group-1\n", "97 USD", &error);
	g_assert_no_error(error);
	json = g_strdup_printf(
		"{\"parts\":[{\"type\":\"payment\",\"id\":%" G_GINT64_FORMAT "},"
		"{\"type\":\"payment\",\"id\":%" G_GINT64_FORMAT "},"
		"{\"type\":\"adjustment\",\"amount\":\"-3 USD\",\"account_id\":%" G_GINT64_FORMAT ",\"description\":\"processor fee\"}]}",
		venture_entity_get_id(pay_a), venture_entity_get_id(pay_b), bank_account_code(f, "6000"));
	result = bank_action(f, "match", 1, json, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 3);
	g_clear_object(&result);
	result = bank_action(f, "reconcile", venture_entity_get_id(statement), "{}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

static void
test_refund_and_journal_candidates(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL, transaction = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) payment = g_object_new(VENTURE_TYPE_PAYMENT, NULL);
	g_autoptr(VentureEntity) refund = g_object_new(VENTURE_TYPE_REFUND, NULL);
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 10, 0, 0, 0);
	g_autoptr(VentureMoney) ten = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GPtrArray) allocations = NULL;
	guint i;
	gboolean saw_refund = FALSE, saw_journal = FALSE;
	(void)data;
	g_object_set(customer, "name", "Refunded", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(customer), NULL, &error));
	invoice = bank_issue(f, "REF-1", "10 USD");
	venture_entity_set_organization_id(payment, f->org);
	g_object_set(payment, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"invoice-id", venture_entity_get_id(invoice), "amount", ten, "date", date, "method", "transfer", NULL);
	g_assert_true(venture_database_save(f->database, payment, NULL, &error));
	allocations = venture_database_find(f->database, venture_query_new(VENTURE_TYPE_PAYMENT_ALLOCATION), &error);
	g_assert_cmpuint(allocations->len, ==, 1);
	venture_entity_set_organization_id(refund, f->org);
	g_object_set(refund, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"allocation-id", venture_entity_get_id(g_ptr_array_index(allocations, 0)),
		"amount", ten, "date", date, NULL);
	g_assert_true(venture_database_save(f->database, refund, NULL, &error));
	post_cash(f, "2026-01-10T00:00:00Z", 1000, FALSE);
	statement = bank_import(f, "csv",
		"date,amount,memo,ref,id\n2026-01-10,-10,Customer refund,r,ref-out\n", "-10 USD", &error);
	g_assert_no_error(error);
	transaction = venture_database_get(f->database, VENTURE_TYPE_BANK_TRANSACTION, 1, &error);
	candidates = venture_bank_transaction_candidates(f->database, transaction, &error);
	g_assert_no_error(error);
	for (i = 0; i < candidates->len; i++)
	{
		const gchar *name = venture_entity_get_entity_name(g_ptr_array_index(candidates, i));
		if (!strcmp(name, "refund")) saw_refund = TRUE;
		if (!strcmp(name, "journal")) saw_journal = TRUE;
	}
	g_assert_true(saw_refund);
	g_assert_true(saw_journal);
	{
		g_autofree gchar *match = g_strdup_printf("{\"parts\":[{\"type\":\"refund\",\"id\":%" G_GINT64_FORMAT "}]}",
			venture_entity_get_id(refund));
		result = bank_action(f, "match", 1, match, &error);
	}
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

static void
test_searchable_window(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, transaction = NULL;
	g_autoptr(GPtrArray) wide = NULL, narrow = NULL, searched = NULL;
	g_autoptr(VentureMoney) ten = venture_money_new_for_currency(1000, "USD");
	guint i;
	(void)data;
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureExpense) expense = venture_expense_new();
		g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, i == 0 ? 10 : 16, 12, 0, 0);
		g_object_set(expense, "description", i == 0 ? "Alpha fee" : "Beta charge",
			"organization-id", f->org, "amount", ten, "occurred-at", date, NULL);
		g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(expense), NULL, &error));
	}
	g_object_set(f->bank, "match-window-days", (gint64)1, NULL);
	g_assert_true(venture_database_save(f->database, f->bank, NULL, &error));
	g_assert_no_error(error);
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Fee,x,search-1\n", "-10 USD", &error);
	g_assert_no_error(error);
	transaction = venture_database_get(f->database, VENTURE_TYPE_BANK_TRANSACTION, 1, &error);
	wide = venture_bank_transaction_candidates(f->database, transaction, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(wide->len, ==, 1);
	g_clear_object(&f->bank);
	f->bank = venture_database_get(f->database, VENTURE_TYPE_BANK_ACCOUNT, 1, &error);
	g_object_set(f->bank, "match-window-days", (gint64)10, NULL);
	g_assert_true(venture_database_save(f->database, f->bank, NULL, &error));
	g_assert_no_error(error);
	narrow = venture_bank_transaction_candidates_search(f->database, transaction, "alpha", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(narrow->len, ==, 1);
	searched = venture_bank_transaction_candidates_search(f->database, transaction, "missing", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(searched->len, ==, 0);
}

static void
test_partial_installments(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureMoney) ten = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 10, 0, 0, 0);
	(void)data;
	g_object_set(expense, "description", "Installments", "organization-id", f->org, "amount", ten, "occurred-at", date, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(expense), NULL, &error));
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-4,First,x,part-1\n2026-01-11,-6,Second,x,part-2\n", "-10 USD", &error);
	g_assert_no_error(error);
	result = bank_action(f, "match", 1, "{\"parts\":[{\"type\":\"expense\",\"id\":1,\"amount\":\"-4 USD\"}]}", &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	result = bank_action(f, "match", 2, "{\"parts\":[{\"type\":\"expense\",\"id\":1,\"amount\":\"-6 USD\"}]}", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 2);
}

/* Retained deleted records are audit evidence, never live match targets. */
static void
test_deleted_match_target(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureMoney) ten = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 10, 0, 0, 0);
	(void)data;
	g_object_set(expense, "description", "Deleted", "organization-id", f->org, "amount", ten, "occurred-at", date, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(expense), NULL, &error));
	g_assert_true(venture_database_delete(f->database, VENTURE_ENTITY(expense), NULL, &error));
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Fee,x,deleted-match\n", "-10 USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	result = bank_action(f, "match", 1, "{\"parts\":[{\"type\":\"expense\",\"id\":1}]}", &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 0);
}

static void
test_candidate_windows(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL, transaction = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autoptr(VentureMoney) ten = venture_money_new_for_currency(1000, "USD");
	g_autoptr(VentureMoney) near = venture_money_new_for_currency(900, "USD");
	guint i;
	(void)data;
	for (i = 0; i < 5; i++)
	{
		g_autoptr(VentureExpense) expense = venture_expense_new();
		g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, i == 3 ? 15 : i == 4 ? 16 : 10, 12, 0, 0);
		g_object_set(expense, "description", "Candidate", "organization-id", f->org,
			"amount", i == 2 ? near : ten, "occurred-at", date, NULL);
		g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(expense), NULL, &error));
	}
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Fee,x,candidate\n", "-10 USD", &error);
	g_assert_no_error(error);
	transaction = venture_database_get(f->database, VENTURE_TYPE_BANK_TRANSACTION, 1, &error);
	candidates = venture_bank_transaction_candidates(f->database, transaction, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(candidates->len, ==, 4);
	result = bank_action(f, "auto", venture_entity_get_id(statement), "{}", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 0);
}

static void
test_ofx_currency(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) result = NULL;
	(void)data;
	result = bank_import(f, "ofx", "<OFX><CURDEF>EUR<STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>eur</STMTTRN></OFX>", "-10 USD", &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_STATEMENT), ==, 0);
}

static void
test_bank_posting_account(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccount) account = venture_account_new();
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	(void)data;
	g_object_set(account, "name", "Other bank", "code", "1010", "organization-id", f->org,
		"kind", VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(account), NULL, &error));
	g_clear_object(&f->bank);
	f->bank = VENTURE_ENTITY(venture_bank_account_new());
	g_object_set(f->bank, "name", "Other bank", "organization-id", f->org, "currency", "USD",
		"account-id", venture_entity_get_id(VENTURE_ENTITY(account)), NULL);
	g_assert_true(venture_database_save(f->database, f->bank, NULL, &error));
	statement = bank_import(f, "ofx", "<OFX><STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>other</STMTTRN></OFX>", "-10 USD", &error);
	g_assert_no_error(error);
	/* Creation must credit this bank's ledger account, not hardcoded 1000. */
	result = bank_action(f, "create", 1, "{\"type\":\"expense\"}", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 1);
	g_assert_true(journal_credits_account(f, venture_entity_get_id(VENTURE_ENTITY(account))));
}

static gint64
bank_book(BankFixture *f, const gchar *cutoff)
{
	g_autoptr(GDateTime) date = venture_time_from_string(cutoff, NULL);
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	gint64 account_id;
	g_object_get(f->bank, "account-id", &account_id, NULL);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->database),
		account_id, f->org, "USD", date, &error);
	g_assert_no_error(error);
	return venture_money_get_amount(balance);
}

static void
post_cash(BankFixture *f, const gchar *when, gint64 amount, gboolean debit)
{
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) date = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(amount, "USD");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GError) error = NULL;
	gint64 cash_id, offset_id;
	VentureJournalLine *line;
	g_object_get(f->bank, "account-id", &cash_id, NULL);
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "3000", &error));
	accounts = venture_database_find(f->database, query, &error);
	g_assert_cmpuint(accounts->len, ==, 1);
	offset_id = venture_entity_get_id(g_ptr_array_index(accounts, 0));
	g_object_set(header, "organization-id", f->org, "source-type", "organization",
		"source-id", f->org, "occurred-at", date, "currency", "USD", NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", debit ? cash_id : offset_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", debit ? offset_id : cash_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->database), header, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

/* Statement 1000 versus books 900 is explained by an outstanding check of 100.
 * Clearing it on the next statement removes it from outstanding. */
static void
test_outstanding_check(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureMoney) outstanding = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	(void)data;
	post_cash(f, "2025-12-31T00:00:00Z", 100000, TRUE);
	{
		g_autoptr(VentureExpense) expense = venture_expense_new();
		g_autoptr(VentureMoney) hundred = venture_money_new_for_currency(10000, "USD");
		g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 15, 0, 0, 0);
		g_object_set(expense, "description", "Uncleared check", "organization-id", f->org,
			"amount", hundred, "occurred-at", date, NULL);
		g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(expense), NULL, &error));
	}
	g_assert_cmpint(bank_book(f, "2026-01-31T23:59:59Z"), ==, 90000);
	json_object_set_string_member(args, "format", "csv");
	json_object_set_string_member(args, "data", "date,amount,memo,ref,id\n2026-01-20,0,placeholder,none,ph-1\n");
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "1000 USD");
	json_object_set_string_member(args, "closing_balance", "1000 USD");
	statement = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->database),
		"import", venture_entity_get_id(f->bank), args, NULL, &error);
	g_assert_no_error(error);
	g_clear_object(&result);
	result = bank_action(f, "exclude", 1, "{\"reason\":\"placeholder line\"}", &error);
	g_assert_no_error(error);
	g_clear_object(&result);
	result = bank_action(f, "reconcile", venture_entity_get_id(statement), "{}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_object_get(result, "outstanding-checks", &outstanding, NULL);
	g_assert_cmpint(venture_money_get_amount(outstanding), ==, 10000);
	g_assert_cmpint(bank_book(f, "2026-01-31T23:59:59Z"), ==, 90000);
}

static void
test_reopen_reconciliation(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autofree gchar *state = NULL;
	(void)data;
	statement = bank_import(f, "ofx", "<OFX><STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>reopen</STMTTRN></OFX>", "-10 USD", &error);
	g_assert_no_error(error);
	result = bank_action(f, "create", 1, "{\"type\":\"expense\"}", &error);
	g_assert_no_error(error);
	g_clear_object(&result);
	result = bank_action(f, "reconcile", venture_entity_get_id(statement), "{}", &error);
	g_assert_no_error(error);
	g_clear_object(&result);
	result = bank_action(f, "reopen", venture_entity_get_id(statement), "{\"reason\":\"statement revised\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_object_get(result, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "reopened");
	g_clear_error(&error);
	g_clear_object(&result);
	result = bank_action(f, "unmatch", 1, "{}", &error);
	g_assert_no_error(error);
}

static void
test_open_reconciliation(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	g_autofree gchar *state = NULL;
	(void)data;
	statement = bank_import(f, "ofx", "<OFX><STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>open</STMTTRN></OFX>", "-10 USD", &error);
	g_assert_no_error(error);
	result = bank_action(f, "reconcile", venture_entity_get_id(statement), "{\"state\":\"open\"}", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_object_get(result, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "open");
}

static void
test_disabled_upgrade(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(OrmResult) result = NULL;
	venture_config_set_module_enabled(config, "banking", FALSE);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_execute(db, "CREATE TABLE banking_old_probe (amount BIGINT); INSERT INTO banking_old_probe VALUES (12345)", NULL, &error));
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE name = 'bank_transactions'", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
	g_clear_object(&result);
	venture_config_set_module_enabled(config, "banking", TRUE);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(db, "SELECT amount FROM banking_old_probe", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 12345);
}

static VentureEntity *
save_rule(BankFixture *f, const gchar *name, const gchar *merchant, gint64 priority, gboolean enabled)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *rule = VENTURE_ENTITY(venture_bank_rule_new());
	venture_entity_set_organization_id(rule, f->org);
	g_object_set(rule, "name", name, "merchant", merchant, "priority", priority, "enabled", enabled,
		"action", "categorize", "category", "SUPPLIES", "create-type", "expense",
		"bank-account-id", venture_entity_get_id(f->bank), NULL);
	g_assert_true(venture_database_save(f->database, rule, NULL, &error));
	g_assert_no_error(error);
	return rule;
}

static gboolean
journal_credits_account(BankFixture *f, gint64 account_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	guint i;
	venture_query_set_limit(query, 0);
	lines = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < lines->len; i++)
	{
		gint64 line_account = 0;
		gint side = 0;
		g_object_get(g_ptr_array_index(lines, i), "account-id", &line_account, "side", &side, NULL);
		if (line_account == account_id && side == VENTURE_LEDGER_SIDE_CREDIT)
			return TRUE;
	}
	return FALSE;
}

/* Preview against historical rows must not post; enabling uses that sample. */
static void
test_rule_preview(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, rule = NULL, result = NULL;
	g_autofree gchar *preview = NULL;
	(void)data;
	statement = bank_import(f, "csv",
		"date,amount,memo,ref,id\n2026-01-10,-10,STARBUCKS STORE 12,x,sb-1\n2026-01-11,-5,PAYROLL,x,pr-1\n",
		"-15 USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	rule = save_rule(f, "Coffee", "STARBUCKS", 10, FALSE);
	result = bank_action(f, "preview", venture_entity_get_id(rule), "{}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_object_get(result, "last-preview", &preview, NULL);
	g_assert_nonnull(preview);
	g_assert_nonnull(strstr(preview, "STARBUCKS"));
	g_assert_null(strstr(preview, "PAYROLL"));
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 0);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 0);
	g_clear_object(&result);
	result = bank_action(f, "enable", venture_entity_get_id(rule), "{}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

/* Unique rule matches categorize in bulk; two rules on one row stay reviewable. */
static void
test_bulk_categorize(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) statement = NULL, coffee = NULL, other = NULL, result = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	g_autoptr(GPtrArray) rows = NULL;
	guint unmatched = 0, i;
	g_autofree gchar *state = NULL;
	(void)data;
	statement = bank_import(f, "csv",
		"date,amount,memo,ref,id\n2026-01-10,-10,STARBUCKS A,x,sb-a\n2026-01-11,-8,STARBUCKS B,x,sb-b\n2026-01-12,-4,AMBIGUOUS,x,amb-1\n",
		"-22 USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(statement);
	coffee = save_rule(f, "Coffee", "STARBUCKS", 10, FALSE);
	other = save_rule(f, "Also coffee", "STARBUCKS", 10, FALSE);
	g_assert_nonnull(bank_action(f, "preview", venture_entity_get_id(coffee), "{}", &error));
	g_clear_error(&error);
	g_assert_nonnull(bank_action(f, "enable", venture_entity_get_id(coffee), "{}", &error));
	g_clear_error(&error);
	g_assert_nonnull(bank_action(f, "preview", venture_entity_get_id(other), "{}", &error));
	g_clear_error(&error);
	g_assert_nonnull(bank_action(f, "enable", venture_entity_get_id(other), "{}", &error));
	g_clear_error(&error);
	g_clear_object(&result);
	result = bank_action(f, "bulk", venture_entity_get_id(f->bank), "{\"mode\":\"categorize\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 0);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *row_state = NULL;
		g_object_get(g_ptr_array_index(rows, i), "state", &row_state, NULL);
		if (!g_strcmp0(row_state, "unmatched"))
			unmatched++;
	}
	g_assert_cmpuint(unmatched, ==, 3);
	{
		gint64 other_id = venture_entity_get_id(other);
		g_clear_object(&other);
		other = venture_database_get(f->database, VENTURE_TYPE_BANK_RULE, other_id, &error);
		g_assert_no_error(error);
		g_object_set(other, "enabled", FALSE, NULL);
		g_assert_true(venture_database_save(f->database, other, NULL, &error));
		g_assert_no_error(error);
	}
	g_clear_object(&result);
	g_clear_error(&error);
	result = bank_action(f, "bulk", venture_entity_get_id(f->bank), "{\"mode\":\"categorize\"}", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 2);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 2);
	g_clear_object(&query);
	g_clear_pointer(&rows, g_ptr_array_unref);
	query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, "amb-1", &error));
	rows = venture_database_find(f->database, query, &error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_object_get(g_ptr_array_index(rows, 0), "state", &state, NULL);
	g_assert_cmpstr(state, ==, "unmatched");
	g_clear_object(&query);
	g_clear_pointer(&rows, g_ptr_array_unref);
	g_clear_object(&result);
	g_clear_error(&error);
	query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, "sb-a", &error));
	rows = venture_database_find(f->database, query, &error);
	g_assert_cmpuint(rows->len, ==, 1);
	result = bank_action(f, "reverse", venture_entity_get_id(g_ptr_array_index(rows, 0)), "{}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_MATCH), ==, 1);
}

/* Overlapping statement windows must not duplicate FITIDs. */
static void
test_overlap_import(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) first = NULL, overlap = NULL, mapped = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	(void)data;
	first = bank_import(f, "ofx",
		"<OFX><STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>overlap-1<MEMO>Fee</STMTTRN></OFX>",
		"-10 USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_TRANSACTION), ==, 1);
	overlap = bank_import(f, "ofx",
		"<OFX><STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>overlap-1<MEMO>Fee</STMTTRN></OFX>",
		"-10 USD", &error);
	g_assert_null(overlap);
	g_assert_nonnull(error);
	g_assert_true(strstr(error->message, "already imported") != NULL ||
		strstr(error->message, "no new") != NULL);
	g_clear_error(&error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_TRANSACTION), ==, 1);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_STATEMENT), ==, 1);
	json_object_set_string_member(args, "data", "Posted,Debit,Credit,Payee,Ref\n2026-01-11,5,,Shop,r1\n");
	mapped = bank_action(f, "map", venture_entity_get_id(f->bank),
		"{\"data\":\"Posted,Debit,Credit,Payee,Ref\\n2026-01-11,5,,Shop,r1\\n\",\"apply\":true,"
		"\"date_column\":\"Posted\",\"debit_column\":\"Debit\",\"credit_column\":\"Credit\","
		"\"description_column\":\"Payee\",\"reference_column\":\"Ref\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(mapped);
}

static gint64
account_by_code(BankFixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, &error));
	accounts = venture_database_find(f->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(accounts->len, ==, 1);
	return venture_entity_get_id(g_ptr_array_index(accounts, 0));
}

/* A transfer posts both ledger sides and refuses a second insert of the same key. */
static void
test_transfer_balanced(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccount) other_ledger = venture_account_new();
	g_autoptr(VentureBankAccount) dest = venture_bank_account_new();
	g_autoptr(VentureEntity) result = NULL, again = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	g_autofree gchar *payload = NULL;
	gint64 dest_id, source_cash, dest_cash;
	(void)data;
	g_object_set(other_ledger, "name", "Savings", "code", "1010", "organization-id", f->org,
		"kind", VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(other_ledger), NULL, &error));
	venture_entity_set_organization_id(VENTURE_ENTITY(dest), f->org);
	g_object_set(dest, "name", "Savings", "currency", "USD",
		"account-id", venture_entity_get_id(VENTURE_ENTITY(other_ledger)),
		"date-column", "date", "amount-column", "amount", "description-column", "memo",
		"reference-column", "ref", "external-id-column", "id", "date-format", "%Y-%m-%d",
		"sign-convention", "normal", NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(dest), NULL, &error));
	dest_id = venture_entity_get_id(VENTURE_ENTITY(dest));
	payload = g_strdup_printf("{\"counterparty_bank_account_id\":%" G_GINT64_FORMAT
		",\"amount\":\"100 USD\",\"date\":\"2026-01-10\"}", dest_id);
	result = bank_action(f, "transfer", venture_entity_get_id(f->bank), payload, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_TRANSFER), ==, 1);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_JOURNAL), ==, 1);
	source_cash = account_by_code(f, "1000");
	dest_cash = venture_entity_get_id(VENTURE_ENTITY(other_ledger));
	g_assert_true(journal_credits_account(f, source_cash));
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		gboolean debit_dest = FALSE;
		guint i;
		venture_query_set_limit(query, 0);
		lines = venture_database_find(f->database, query, &error);
		for (i = 0; i < lines->len; i++)
		{
			gint64 line_account = 0;
			gint side = 0;
			g_object_get(g_ptr_array_index(lines, i), "account-id", &line_account, "side", &side, NULL);
			if (line_account == dest_cash && side == VENTURE_LEDGER_SIDE_DEBIT)
				debit_dest = TRUE;
		}
		g_assert_true(debit_dest);
	}
	again = bank_action(f, "transfer", venture_entity_get_id(f->bank), payload, &error);
	g_assert_null(again);
	g_assert_nonnull(error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_TRANSFER), ==, 1);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_JOURNAL), ==, 1);
}

/* Bank-created payments credit the bank's ledger account, not hardcoded 1000. */
static void
test_payment_cash_account(BankFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccount) account = venture_account_new();
	g_autoptr(VentureEntity) statement = NULL, result = NULL;
	gint64 cash_id;
	(void)data;
	g_object_set(account, "name", "Card", "code", "1020", "organization-id", f->org,
		"kind", VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(account), NULL, &error));
	cash_id = venture_entity_get_id(VENTURE_ENTITY(account));
	g_clear_object(&f->bank);
	f->bank = VENTURE_ENTITY(venture_bank_account_new());
	g_object_set(f->bank, "name", "Card", "organization-id", f->org, "currency", "USD",
		"account-id", cash_id, "date-column", "date", "amount-column", "amount",
		"description-column", "memo", "reference-column", "ref", "external-id-column", "id",
		"date-format", "%Y-%m-%d", "sign-convention", "normal", NULL);
	g_assert_true(venture_database_save(f->database, f->bank, NULL, &error));
	statement = bank_import(f, "ofx",
		"<OFX><STMTTRN><DTPOSTED>20260110<TRNAMT>-10<FITID>card-pay</STMTTRN></OFX>",
		"-10 USD", &error);
	g_assert_no_error(error);
	result = bank_action(f, "create", 1, "{\"type\":\"expense\"}", &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 1);
	g_assert_true(journal_credits_account(f, cash_id));
	g_assert_false(journal_credits_account(f, account_by_code(f, "1000")));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/banking/invalid-calendar-day", BankFixture, NULL, bank_setup, test_invalid_calendar_day, bank_teardown);
	g_test_add("/banking/deleted-match-target", BankFixture, NULL, bank_setup, test_deleted_match_target, bank_teardown);
	g_test_add("/banking/ofx-currency", BankFixture, NULL, bank_setup, test_ofx_currency, bank_teardown);
	g_test_add("/banking/posting-account", BankFixture, NULL, bank_setup, test_bank_posting_account, bank_teardown);
	g_test_add("/banking/outstanding-check", BankFixture, NULL, bank_setup, test_outstanding_check, bank_teardown);
	g_test_add("/banking/reopen-reconciliation", BankFixture, NULL, bank_setup, test_reopen_reconciliation, bank_teardown);
	g_test_add("/banking/open-reconciliation", BankFixture, NULL, bank_setup, test_open_reconciliation, bank_teardown);
	g_test_add("/banking/candidate-windows", BankFixture, NULL, bank_setup, test_candidate_windows, bank_teardown);
	g_test_add("/banking/partial-installments", BankFixture, NULL, bank_setup, test_partial_installments, bank_teardown);
	g_test_add("/banking/posting-rollback", BankFixture, NULL, bank_setup, test_posting_rollback, bank_teardown);
	g_test_add("/banking/receipt", BankFixture, NULL, bank_setup, test_receipt, bank_teardown);
	g_test_add("/banking/period-guard", BankFixture, NULL, bank_setup, test_period_guard, bank_teardown);
	g_test_add("/banking/surfaces", BankFixture, NULL, bank_setup, test_surfaces, bank_teardown);
	g_test_add("/banking/account-evidence", BankFixture, NULL, bank_setup, test_account_evidence_guard, bank_teardown);
	g_test_add("/banking/ofx-rollback", BankFixture, NULL, bank_setup, test_ofx_rollback, bank_teardown);
	g_test_add("/banking/split-atomic", BankFixture, NULL, bank_setup, test_split_and_atomic_creation, bank_teardown);
	g_test_add("/banking/rule-preview", BankFixture, NULL, bank_setup, test_rule_preview, bank_teardown);
	g_test_add("/banking/bulk-categorize", BankFixture, NULL, bank_setup, test_bulk_categorize, bank_teardown);
	g_test_add("/banking/overlap-import", BankFixture, NULL, bank_setup, test_overlap_import, bank_teardown);
	g_test_add("/banking/transfer-balanced", BankFixture, NULL, bank_setup, test_transfer_balanced, bank_teardown);
	g_test_add("/banking/payment-cash-account", BankFixture, NULL, bank_setup, test_payment_cash_account, bank_teardown);
	g_test_add("/banking/grouped-deposit-adjustment", BankFixture, NULL, bank_setup, test_grouped_deposit_adjustment, bank_teardown);
	g_test_add("/banking/refund-and-journal-candidates", BankFixture, NULL, bank_setup, test_refund_and_journal_candidates, bank_teardown);
	g_test_add("/banking/searchable-window", BankFixture, NULL, bank_setup, test_searchable_window, bank_teardown);
	g_test_add_func("/banking/disabled-upgrade", test_disabled_upgrade);
	g_test_add_func("/banking/records", test_records);
	g_test_add_func("/banking/report", test_report_registration);
	g_test_add_func("/banking/generic-match-refused", test_match_guard);
	g_test_add_func("/banking/import-match-reconcile", test_import_match_reconcile);
	return g_test_run();
}
