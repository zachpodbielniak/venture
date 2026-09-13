/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

static void
test_records(void)
{
	const gchar *names[] = { "bank_account", "bank_statement", "bank_transaction", "bank_match", "reconciliation" };
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
	/* The first row succeeds, then the duplicate must undo all its writes. */
	result = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-11,-5,Other,x,new-id\n2026-01-10,-10,Fee,x,ofx-1\n", "-15 USD", &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_STATEMENT), ==, 1);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_BANK_TRANSACTION), ==, 1);
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
	for (i = 0; i < 4; i++)
	{
		g_autoptr(VentureExpense) expense = venture_expense_new();
		g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, i == 3 ? 16 : 10, 0, 0, 0);
		g_object_set(expense, "description", "Candidate", "organization-id", f->org,
			"amount", i == 2 ? near : ten, "occurred-at", date, NULL);
		g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(expense), NULL, &error));
	}
	statement = bank_import(f, "csv", "date,amount,memo,ref,id\n2026-01-10,-10,Fee,x,candidate\n", "-10 USD", &error);
	g_assert_no_error(error);
	transaction = venture_database_get(f->database, VENTURE_TYPE_BANK_TRANSACTION, 1, &error);
	candidates = venture_bank_transaction_candidates(f->database, transaction, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(candidates->len, ==, 3);
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
	/* A source rule that posts to another bank must not be matched here. */
	result = bank_action(f, "create", 1, "{\"type\":\"expense\"}", &error);
	g_assert_null(result); g_assert_nonnull(error);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_EXPENSE), ==, 0);
	g_assert_cmpuint(bank_count(f, VENTURE_TYPE_JOURNAL), ==, 0);
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/banking/ofx-currency", BankFixture, NULL, bank_setup, test_ofx_currency, bank_teardown);
	g_test_add("/banking/posting-account", BankFixture, NULL, bank_setup, test_bank_posting_account, bank_teardown);
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
	g_test_add_func("/banking/disabled-upgrade", test_disabled_upgrade);
	g_test_add_func("/banking/records", test_records);
	g_test_add_func("/banking/report", test_report_registration);
	g_test_add_func("/banking/generic-match-refused", test_match_guard);
	g_test_add_func("/banking/import-match-reconcile", test_import_match_reconcile);
	return g_test_run();
}
