/*
 * test-cutover.c - Guided Zoho Books / QuickBooks / Xero opening-balance cutover.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

/* A small company whose trial balance ties: 500 cash, 105 receivable, 605
 * equity. */
static const gchar *const SAMPLE =
	"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
	"\"chart\":[{\"source_id\":\"c-cash\",\"code\":\"1000\",\"name\":\"Cash\",\"kind\":\"asset\"},"
	"{\"source_id\":\"c-ar\",\"code\":\"1100\",\"name\":\"AR\",\"kind\":\"asset\"},"
	"{\"source_id\":\"c-income\",\"code\":\"4000\",\"name\":\"Income\",\"kind\":\"income\"},"
	"{\"source_id\":\"c-equity\",\"code\":\"3000\",\"name\":\"Equity\",\"kind\":\"equity\"}],"
	"\"customers\":[{\"source_id\":\"cust-1\",\"name\":\"Acme\"}],"
	"\"vendors\":[{\"source_id\":\"vend-1\",\"name\":\"Supplier\"}],"
	"\"items\":[{\"source_id\":\"item-1\",\"name\":\"Work\"}],"
	"\"open_ar\":[{\"source_id\":\"inv-1\",\"customer_source_id\":\"cust-1\","
	"\"number\":\"OB-1\",\"amount\":\"105 USD\",\"net\":\"100 USD\",\"tax\":\"5 USD\",\"date\":\"2025-12-15\"}],"
	"\"open_ap\":[],\"credits\":[],\"bank_balances\":[{\"source_id\":\"bank-1\","
	"\"name\":\"Checking\",\"account_code\":\"1000\",\"amount\":\"500 USD\"}],"
	"\"assets\":[],"
	"\"trial_balance\":[{\"account_code\":\"1000\",\"debit\":\"500 USD\"},"
	"{\"account_code\":\"1100\",\"debit\":\"105 USD\"},{\"account_code\":\"3000\",\"credit\":\"605 USD\"}],"
	"\"unsupported\":[\"payroll_item\"]}";

static JsonObject *
parse_json(const gchar *json)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static JsonObject *
sample_payload(void)
{
	return parse_json(SAMPLE);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "migrator";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static gint64
account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, code, NULL));
	accounts = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(accounts);
	g_assert_cmpuint(accounts->len, ==, 1);
	return venture_entity_get_id(g_ptr_array_index(accounts, 0));
}

static gint64
balance_in(Fixture *f, const gchar *code, const gchar *currency, const gchar *when)
{
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		account_id(f, code), f->org, currency, as_of, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balance);
	return venture_money_get_amount(balance);
}

static gint64
account_balance(Fixture *f, const gchar *code, const gchar *when)
{
	return balance_in(f, code, "USD", when);
}

static gint64
cash_balance(Fixture *f, const gchar *when)
{
	return account_balance(f, "1000", when);
}

static void
make_account(Fixture *f, const gchar *code, VentureAccountKind kind)
{
	g_autoptr(VentureEntity) account = VENTURE_ENTITY(venture_account_new());
	g_autoptr(GError) error = NULL;
	g_object_set(account, "organization-id", f->org, "name", code, "code", code, "kind", kind, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, account, NULL, &error));
	g_assert_no_error(error);
}

static gchar *
report_of(VentureEntity *cutover)
{
	gchar *report = NULL;
	g_object_get(cutover, "reconciliation-report", &report, NULL);
	return report;
}

static gchar *
state_of(VentureEntity *cutover)
{
	gchar *state = NULL;
	g_object_get(cutover, "state", &state, NULL);
	return state;
}

static VentureEntity *
preview_payload(Fixture *f, const gchar *json)
{
	g_autoptr(JsonObject) payload = parse_json(json);
	g_autoptr(GError) error = NULL;
	VentureEntity *cutover;
	VentureActor actor;
	actor_init(&actor);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db), f->org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cutover);
	return cutover;
}

static VentureEntity *
import_payload(Fixture *f, const gchar *json, gboolean expect_ok, GError **error)
{
	VentureEntity *cutover = preview_payload(f, json);
	VentureActor actor;
	actor_init(&actor);
	g_assert_cmpint(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, error), ==, expect_ok);
	return cutover;
}

static void
reconcile_ok(Fixture *f, VentureEntity *cutover)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	actor_init(&actor);
	/* The refusal names the failing checks; show them before asserting. */
	if (!venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error))
		g_assert_no_error(error);
	g_assert_no_error(error);
}

static guint
count_type(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(rows);
	return rows->len;
}

static VentureEntity *
find_one(Fixture *f, GType type, const gchar *field, const gchar *value)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL));
	rows = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 1);
	return g_object_ref(g_ptr_array_index(rows, 0));
}

/* The preview row for one source row of @cutover. */
static VentureEntity *
preview_row(Fixture *f, VentureEntity *cutover, const gchar *source_type, guint position)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	guint i, seen = 0;
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_int(query, "cutover-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(cutover), NULL));
	g_assert_true(venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL));
	rows = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(rows);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *type = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "source-type", &type, "status", &status, NULL);
		if (g_strcmp0(type, source_type) != 0 || (g_strcmp0(status, "preview") != 0 && g_strcmp0(status, "exception") != 0))
			continue;
		if (seen++ == position)
			return g_object_ref(g_ptr_array_index(rows, i));
	}
	g_assert_not_reached();
	return NULL;
}

static void
assert_row_exception(Fixture *f, VentureEntity *cutover, const gchar *source_type, guint position, const gchar *needle)
{
	g_autoptr(VentureEntity) row = preview_row(f, cutover, source_type, position);
	g_autofree gchar *exception = NULL;
	g_autofree gchar *status = NULL;
	g_object_get(row, "exception", &exception, "status", &status, NULL);
	if (exception == NULL || strstr(exception, needle) == NULL)
		g_error("%s[%u] exception \"%s\" lacks \"%s\"", source_type, position, exception != NULL ? exception : "", needle);
	g_assert_cmpstr(status, ==, "exception");
}

static void
test_preview_import_activate(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = sample_payload();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *report = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cutover);
	g_object_get(cutover, "state", &state, "reconciliation-report", &report, NULL);
	g_assert_cmpstr(state, ==, "preview");
	g_assert_nonnull(strstr(report, "payroll_item"));
	g_assert_nonnull(strstr(report, "items are not imported"));
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&state, g_free);
	g_object_get(cutover, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "imported");
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 50000);
	/* The trial balance posts equity; receivables and cash come from their
	 * subledgers and are not posted a second time. */
	g_assert_cmpint(account_balance(f, "1100", "2026-01-01T00:00:00Z"), ==, 10500);
	g_assert_cmpint(account_balance(f, "3000", "2026-01-01T00:00:00Z"), ==, -60500);
	g_assert_cmpint(account_balance(f, "3900", "2026-01-01T00:00:00Z"), ==, 0);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(rows->len, >, 0);
	g_assert_true(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&report, g_free);
	g_object_get(cutover, "reconciliation-report", &report, NULL);
	/* Reconcile appends: the preview's evidence is still there. */
	g_assert_nonnull(strstr(report, "Cutover preview"));
	g_assert_nonnull(strstr(report, "Trial balance ties"));
	g_assert_nonnull(strstr(report, "PASS open_ar USD: expected 105.00 USD, subledger 105.00 USD"));
	g_assert_nonnull(strstr(report, "PASS clearing 3900 USD"));
	g_assert_true(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_clear_pointer(&state, g_free);
	g_object_get(cutover, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "active");
	g_assert_false(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(error);
}

/* Two batches of one payload write one set of documents, and a repeated
 * rollback never reverses the reversal. What breaks if this regresses:
 * reimporting creates a second invoice, or a retry recreates opening cash. */
static void
test_idempotent_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = sample_payload();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	first = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(first), &actor, &error));
	second = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(second), &actor, &error));
	venture_query_set_limit(query, 0);
	invoices = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(invoices->len, ==, 1);
	g_assert_true(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(first), &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_true(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(first), &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 0);
	g_clear_pointer(&invoices, g_ptr_array_unref);
	invoices = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(invoices->len, ==, 1);
	{
		gint status = 0;
		g_autofree gchar *number = NULL;
		g_autofree gchar *expected = g_strdup_printf("OB-1-rb%" G_GINT64_FORMAT, venture_entity_get_id(first));
		g_object_get(g_ptr_array_index(invoices, 0), "status", &status, "number", &number, NULL);
		g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_VOID);
		g_assert_cmpstr(number, ==, expected);
	}
}

static void
test_generic_write_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccountingCutover) cutover = venture_accounting_cutover_new();
	g_autoptr(GDateTime) cutoff = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	(void)data;
	g_object_set(cutover, "source", "zoho_books", "cutoff", cutoff, "state", "reconciled", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(cutover), f->org);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(cutover), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureCutoverService"));
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} SurfaceResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, const gchar *cookie, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SurfaceResult response;
	guint status;
	memset(&response, 0, sizeof(response));
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
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

static gchar *
run_cli(const gchar *const *args, gboolean expect_ok)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	CliResult result = { FALSE, NULL, NULL, NULL };
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	child = g_subprocess_launcher_spawnv(launcher, args, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &result);
	while (!result.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (g_subprocess_get_successful(child) != expect_ok)
		g_error("venturectl %s: %s", args[3], result.err);
	g_free(result.err);
	return result.out;
}

/* The pages and routes, including that a preview lands in the legal entity
 * the operator selected. What breaks if this regresses: a subsidiary's
 * opening balances are posted into the parent's ledger. */
static void
test_surfaces(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autofree gchar *dir = g_dir_make_tmp("venture-cutover-XXXXXX", NULL);
	g_autofree gchar *body = NULL;
	g_autofree gchar *payload = NULL;
	g_autofree gchar *cookie = NULL;
	g_autoptr(VentureOrganization) subsidiary = venture_organization_new();
	g_autoptr(VentureEntity) batch = NULL;
	guint port;
	(void)data;
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_cmpuint(http_request(server, "GET", "/", NULL, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Cutover"));
	g_clear_pointer(&body, g_free);
	{
		g_autoptr(JsonObject) object = sample_payload();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, json_object_ref(object));
		payload = venture_json_to_string(node, FALSE);
	}
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/preview",
		"application/json", payload, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "preview"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "GET", "/e/accounting_cutover/1", NULL, NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Mapped source JSON"));
	g_assert_nonnull(strstr(body, "name=\"action\""));
	g_assert_nonnull(strstr(body, "rollback_preflight"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/1/import",
		"application/json", "{}", NULL, &body), ==, 200);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/1/rollback_preflight",
		"application/json", "{}", NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Rollback preflight"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/cutover/1/action",
		"application/x-www-form-urlencoded", "action=reconcile", NULL, NULL), ==, 302);
	g_assert_cmpuint(http_request(server, "GET", "/api/v1/accounting_cutovers/template/trial_balance",
		NULL, NULL, NULL, &body), ==, 200);
	g_assert_cmpstr(body, ==, "account_code,name,currency,debit,credit\r\n");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/csv", "application/json",
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"build_only\":true,"
		"\"files\":{\"bank_balances\":\"source_id,name,account_code,amount\\r\\nb1,Checking,1000,\\\"1,500.00 USD\\\"\\r\\n\"}}",
		NULL, &body), ==, 200);
	/* build_only hands back the payload, cells as text for preview to judge. */
	g_assert_nonnull(strstr(body, "1,500.00 USD"));
	g_assert_null(strstr(body, "\"state\""));
	g_clear_pointer(&body, g_free);
	/* The CLI reads the CSV and the server builds the payload from it. */
	{
		g_autofree gchar *file = g_build_filename(dir, "bank.csv", NULL);
		g_autofree gchar *argument = g_strdup_printf("bank_balances=@%s", file);
		g_autofree gchar *out = NULL;
		const gchar *templ[] = { "build/debug/venturectl", "--server", venture_web_server_get_base_url(server),
			"cutover", "template", "bank_balances", NULL };
		const gchar *csv[] = { "build/debug/venturectl", "--server", venture_web_server_get_base_url(server),
			"cutover", "csv", "source=generic", "cutoff=2026-01-01", "currency=USD", argument, "build_only=true", NULL };
		const gchar *bad[] = { "build/debug/venturectl", "--server", venture_web_server_get_base_url(server),
			"cutover", "csv", "source=generic", "cutoff=2026-01-01", "ledger=@/dev/null", "dangling", NULL };
		g_assert_true(g_file_set_contents(file, "source_id,name,account_code,amount\nb1,Checking,1000,250\n", -1, NULL));
		out = run_cli(templ, TRUE);
		g_assert_cmpstr(out, ==, "source_id,name,account_code,currency,amount\r\n");
		g_clear_pointer(&out, g_free);
		out = run_cli(csv, TRUE);
		g_assert_nonnull(strstr(out, "Checking"));
		g_assert_nonnull(strstr(out, "250"));
		g_clear_pointer(&out, g_free);
		out = run_cli(bad, FALSE);
	}
	g_object_set(subsidiary, "name", "Subsidiary", "default-currency", "USD", "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(subsidiary), NULL, &error));
	g_assert_no_error(error);
	cookie = g_strdup_printf("venture_entity=%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(subsidiary)));
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/preview",
		"application/json", payload, cookie, &body), ==, 200);
	g_clear_pointer(&body, g_free);
	batch = venture_database_get(f->db, VENTURE_TYPE_ACCOUNTING_CUTOVER, 2, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_organization_id(batch), ==, venture_entity_get_id(VENTURE_ENTITY(subsidiary)));
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/preview",
		"application/json", payload, "venture_entity=all", &body), !=, 200);
	g_clear_pointer(&body, g_free);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
}

/* Preview writes a cutover row per source row, with its located problems.
 * What breaks if this regresses: a payload with bad rows previews clean and
 * the operator learns about the first problem only at import. */
static void
test_preview_opening_rows(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	(void)data;
	cutover = preview_payload(f,
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"open_ap\":[{\"source_id\":\"b1\",\"amount\":\"10 USD\"}],"
		"\"credits\":[{\"source_id\":\"cr1\"}],\"assets\":[{\"source_id\":\"as1\"}]}");
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_cmpuint(rows->len, ==, 3);
	assert_row_exception(f, cutover, "open_ap", 0, "vendor_source_id or vendor_name");
	assert_row_exception(f, cutover, "credit", 0, "amount is required");
	assert_row_exception(f, cutover, "asset", 0, "in_service_at is required");
}

static void
test_currency_required(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	(void)data;
	cutover = import_payload(f,
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"bank_balances\":[{\"source_id\":\"bank-1\",\"name\":\"Checking\","
		"\"account_code\":\"1000\",\"amount\":\"500\"}]}", FALSE, &error);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "bank_balances[0] bank-1: amount"));
	g_assert_nonnull(strstr(error->message, "currency"));
}

static void
test_exact_opening_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
	g_autoptr(VentureMoney) tax = NULL;
	(void)data;
	cutover = import_payload(f,
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"customers\":[{\"source_id\":\"cust-1\",\"name\":\"Acme\"}],"
		"\"open_ar\":[{\"source_id\":\"inv-1\",\"customer_source_id\":\"cust-1\","
		"\"number\":\"OB-2\",\"net\":\"33.33 USD\",\"tax\":\"2.50 USD\","
		"\"date\":\"2025-12-15\"}]}", TRUE, &error);
	g_assert_no_error(error);
	venture_query_set_limit(query, 0);
	events = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(events->len, ==, 1);
	g_object_get(g_ptr_array_index(events, 0), "tax-amount", &tax, NULL);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 250);
	reconcile_ok(f, cutover);
}

#define OPENING_HEAD \
	"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\"," \
	"\"customers\":[{\"source_id\":\"cust-1\",\"name\":\"Acme\"}]," \
	"\"vendors\":[{\"source_id\":\"vend-1\",\"name\":\"Supplier\"}]," \
	"\"open_ar\":[{\"source_id\":\"inv-1\",\"customer_source_id\":\"cust-1\"," \
	"\"number\":\"OB-1\",\"net\":\"100 USD\",\"tax\":\"5 USD\",\"date\":\"2025-12-15\"}]," \
	"\"bank_balances\":[{\"source_id\":\"bank-1\",\"name\":\"Checking\",\"account_code\":\"1000\",\"amount\":\"500 USD\"}],"
#define OPENING_AP \
	"\"open_ap\":[{\"source_id\":\"bill-1\",\"vendor_source_id\":\"vend-1\",\"number\":\"B-1\"," \
	"\"date\":\"2025-12-10\",\"due_date\":\"2026-01-09\",\"paid\":\"30 USD\",\"balance\":\"75 USD\"," \
	"\"lines\":[{\"description\":\"Hosting\",\"amount\":\"100\",\"tax\":\"5\"}]}," \
	"{\"source_id\":\"bill-2\",\"vendor_source_id\":\"vend-1\",\"number\":\"B-2\"," \
	"\"date\":\"2025-12-20\",\"lines\":[{\"description\":\"Parts\",\"amount\":\"200 USD\"}]}],"
#define OPENING_CREDITS \
	"\"credits\":[{\"source_id\":\"cr-1\",\"kind\":\"customer\",\"customer_source_id\":\"cust-1\"," \
	"\"number\":\"CN-1\",\"date\":\"2025-12-20\",\"amount\":\"20 USD\"}," \
	"{\"source_id\":\"cr-2\",\"kind\":\"vendor\",\"vendor_source_id\":\"vend-1\"," \
	"\"date\":\"2025-12-21\",\"amount\":\"50 USD\"}],"
#define OPENING_ASSETS \
	"\"assets\":[{\"source_id\":\"as-1\",\"tag\":\"LAPTOP-1\",\"name\":\"Laptop\",\"cost\":\"3600 USD\"," \
	"\"in_service_at\":\"2025-01-01\",\"method\":\"straight_line\",\"useful_life_months\":36," \
	"\"accumulated_depreciation\":\"1250 USD\",\"asset_account_code\":\"1500\"," \
	"\"accumulated_depreciation_account_code\":\"1590\",\"depreciation_expense_account_code\":\"6850\"}]}"

static void
asset_accounts(Fixture *f)
{
	make_account(f, "1500", VENTURE_ACCOUNT_KIND_ASSET);
	make_account(f, "1590", VENTURE_ACCOUNT_KIND_ASSET);
	make_account(f, "6850", VENTURE_ACCOUNT_KIND_EXPENSE);
}

static gint64
entry_amount(Fixture *f, const gchar *period, gint expected_state)
{
	g_autoptr(VentureEntity) row = find_one(f, VENTURE_TYPE_DEPRECIATION_ENTRY, "period", period);
	g_autoptr(VentureMoney) amount = NULL;
	gint state;
	g_object_get(row, "amount", &amount, "state", &state, NULL);
	g_assert_cmpint(state, ==, expected_state);
	return venture_money_get_amount(amount);
}

/* Bills keep their number, dates and frozen tax; a partial payment is an
 * opening payment against the full bill; credits become unapplied balances;
 * the asset's history is one opening balance and the next run continues from
 * the cutoff. None of it touches income, expense or tax. */
static void
test_openings_import(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureEntity) asset = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) bill_date = NULL;
	g_autoptr(GDateTime) opening = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *report = NULL;
	gint method;
	gint state;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	asset_accounts(f);
	cutover = import_payload(f, OPENING_HEAD OPENING_AP OPENING_CREDITS OPENING_ASSETS, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 2);
	bill = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-1");
	g_object_get(bill, "status", &status, "bill-date", &bill_date, "opening-at", &opening, NULL);
	g_assert_cmpstr(status, ==, "partially_paid");
	g_assert_cmpint(g_date_time_get_month(bill_date), ==, 12);
	g_assert_cmpint(g_date_time_get_day_of_month(bill_date), ==, 10);
	g_assert_nonnull(opening);
	g_assert_cmpint(g_date_time_get_year(opening), ==, 2026);
	balance = venture_payables_service_bill_balance(venture_payables_service_get(f->db),
		venture_entity_get_id(bill), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 7500);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL_EVENT), ==, 2);
	line = find_one(f, VENTURE_TYPE_VENDOR_BILL_LINE, "description", "Hosting");
	g_object_get(line, "tax-amount", &amount, NULL);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 500);
	g_clear_pointer(&amount, venture_money_free);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_BILL_PAYMENT), ==, 1);
	payment = find_one(f, VENTURE_TYPE_BILL_PAYMENT, "method", "opening");
	g_object_get(payment, "amount", &amount, NULL);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 3000);
	g_clear_pointer(&amount, venture_money_free);
	/* The paid portion is already inside the source bank balance. */
	g_assert_cmpint(account_balance(f, "1000", "2026-01-01T00:00:00Z"), ==, 50000);
	g_assert_cmpint(account_balance(f, "2000", "2026-01-01T00:00:00Z"), ==, -22500);
	/* The source system already expensed the bills and recognised the
	 * invoice and its tax. */
	g_assert_cmpint(account_balance(f, "6900", "2026-12-31T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "4000", "2026-12-31T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "2100", "2026-12-31T00:00:00Z"), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_CUSTOMER_CREDIT), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_CREDIT), ==, 2);
	{
		g_autoptr(VentureEntity) credit = find_one(f, VENTURE_TYPE_CUSTOMER_CREDIT, "reference", "CN-1");
		g_autoptr(VentureEntity) vendor_credit = find_one(f, VENTURE_TYPE_VENDOR_CREDIT, "reference", "cr-2");
		g_autoptr(VentureMoney) remaining = NULL;
		g_autoptr(VentureMoney) vendor_remaining = NULL;
		g_object_get(credit, "remaining", &remaining, NULL);
		g_object_get(vendor_credit, "remaining", &vendor_remaining, NULL);
		g_assert_cmpint(venture_money_get_amount(remaining), ==, 2000);
		g_assert_cmpint(venture_money_get_amount(vendor_remaining), ==, 5000);
	}
	asset = find_one(f, VENTURE_TYPE_FIXED_ASSET, "tag", "LAPTOP-1");
	g_object_get(asset, "status", &state, "method", &method, NULL);
	g_assert_cmpint(state, ==, VENTURE_ASSET_STATUS_IN_SERVICE);
	g_assert_cmpint(method, ==, VENTURE_ASSET_METHOD_STRAIGHT_LINE);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_DEPRECIATION_ENTRY), ==, 25);
	g_assert_cmpint(entry_amount(f, "2025-12", VENTURE_SCHEDULE_STATE_POSTED), ==, 125000);
	g_assert_cmpint(account_balance(f, "1500", "2026-01-01T00:00:00Z"), ==, 360000);
	g_assert_cmpint(account_balance(f, "1590", "2026-01-01T00:00:00Z"), ==, -125000);
	g_assert_true(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "PASS open_ap USD: expected 275.00 USD, subledger 275.00 USD"));
	g_assert_nonnull(strstr(report, "PASS credits USD: expected 70.00 USD, subledger 70.00 USD"));
	g_assert_nonnull(strstr(report, "PASS assets USD: expected 2350.00 USD, subledger 2350.00 USD"));
	/* Without a trial balance the clearing residual is reported, not hidden. */
	g_assert_nonnull(strstr(report, "No trial balance: opening clearing 3900 holds"));
	/* Reimporting the same source ids is a no-op. */
	again = import_payload(f, OPENING_HEAD OPENING_AP OPENING_CREDITS OPENING_ASSETS, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 2);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_BILL_PAYMENT), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_CUSTOMER_CREDIT), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_CREDIT), ==, 2);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_FIXED_ASSET), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_DEPRECIATION_ENTRY), ==, 25);
	g_assert_cmpint(account_balance(f, "1500", "2026-01-01T00:00:00Z"), ==, 360000);
	/* The first post-cutover run spreads (3600 - 1250) over the 24 months
	 * left: 97.91, never the recomputed 100.00 of a fresh schedule. */
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org,
		FALSE, &actor, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(entry_amount(f, "2026-01", VENTURE_SCHEDULE_STATE_POSTED), ==, 9791);
	g_assert_cmpint(account_balance(f, "1590", "2026-02-01T00:00:00Z"), ==, -134791);
	g_assert_true(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
}

/* A credit larger than what its party owes is an unapplied balance, such as a
 * customer's retainer with no invoice. What breaks if this regresses: a real
 * credit is refused and the migration cannot be completed. */
static void
test_openings_credit_exceeds(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	g_autofree gchar *report = NULL;
	(void)data;
	cutover = import_payload(f, OPENING_HEAD
		"\"credits\":[{\"source_id\":\"cr-9\",\"kind\":\"customer\",\"customer_source_id\":\"cust-1\","
		"\"date\":\"2025-12-20\",\"amount\":\"200 USD\"}]}", TRUE, &error);
	g_assert_no_error(error);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "credit-within-open-balance"));
	credit = find_one(f, VENTURE_TYPE_CUSTOMER_CREDIT, "reference", "cr-9");
	g_object_get(credit, "remaining", &remaining, NULL);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 20000);
	g_assert_cmpint(account_balance(f, "1100", "2026-01-01T00:00:00Z"), ==, -9500);
	reconcile_ok(f, cutover);
}

/* A row problem stops import before its first write. */
static void
test_openings_atomic(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autofree gchar *state = NULL;
	(void)data;
	cutover = import_payload(f, OPENING_HEAD
		"\"open_ap\":[{\"source_id\":\"bill-1\",\"vendor_source_id\":\"vend-1\",\"number\":\"B-1\","
		"\"date\":\"2025-12-10\",\"lines\":[{\"amount\":\"100 USD\"}]},"
		"{\"source_id\":\"bill-3\",\"vendor_source_id\":\"vend-1\",\"number\":\"B-3\","
		"\"date\":\"2025-12-10\",\"paid\":\"500 USD\",\"lines\":[{\"amount\":\"100 USD\"}]}]}", FALSE, &error);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "open_ap[1] bill-3"));
	g_assert_nonnull(strstr(error->message, "open-ap-balance"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL_EVENT), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_JOURNAL), ==, 0);
	state = state_of(cutover);
	g_assert_cmpstr(state, ==, "preview");
}

static GError *
veto_bank(VenturePostingService *service, VentureJournal *journal, GPtrArray *lines, gpointer data)
{
	g_autofree gchar *memo = NULL;
	(void)service;
	(void)lines;
	(void)data;
	g_object_get(journal, "memo", &memo, NULL);
	if (memo != NULL && g_str_has_prefix(memo, "Opening bank balance"))
		return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "test veto");
	return NULL;
}

/* A failure after the invoices, bills and payments are written leaves none of
 * them behind. What breaks if this regresses: a half-imported batch whose
 * reimport skips the documents it already wrote. */
static void
test_openings_write_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	gulong handler;
	(void)data;
	handler = g_signal_connect(venture_database_get_posting_service(f->db), "posting", G_CALLBACK(veto_bank), NULL);
	cutover = import_payload(f, OPENING_HEAD OPENING_AP "\"credits\":[]}", FALSE, &error);
	g_signal_handler_disconnect(venture_database_get_posting_service(f->db), handler);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "test veto"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_BILL_PAYMENT), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMPANY), ==, 0);
}

/* Drift in the opening position -- a payment dated before the cutoff --
 * fails reconcile, and the failure is kept as evidence. What breaks if this
 * regresses: a batch activates on figures that no longer match the source. */
static void
test_openings_out_of_balance(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureBillPayment) payment = venture_bill_payment_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2025, 12, 28, 0, 0, 0);
	g_autofree gchar *report = NULL;
	g_autofree gchar *state = NULL;
	gint64 vendor_id = 0;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = import_payload(f, OPENING_HEAD OPENING_AP "\"credits\":[]}", TRUE, &error);
	g_assert_no_error(error);
	bill = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-2");
	g_object_get(bill, "company-id", &vendor_id, NULL);
	g_object_set(payment, "vendor-id", vendor_id, "bill-id", venture_entity_get_id(bill),
		"amount", amount, "date", date, "method", "transfer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
	g_assert_true(venture_payables_service_apply_payment(venture_payables_service_get(f->db), payment, NULL, &actor, &error));
	g_assert_no_error(error);
	g_assert_false(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(strstr(error->message, "open_ap USD expected 275.00 USD, subledger 175.00 USD"));
	g_clear_error(&error);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "FAIL open_ap USD"));
	g_assert_nonnull(strstr(report, "open_ap[1] bill-2: expected 200.00 USD, subledger 100.00 USD, difference -100.00 USD"));
	state = state_of(cutover);
	g_assert_cmpstr(state, ==, "imported");
	g_assert_false(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(error);
}

/* A receipt dated on the cutoff day, recorded in VENTURE after import, is the
 * new period's cash. What breaks if this regresses: the first real receipt
 * of the new books fails the opening bank and AR reconcile. */
static void
test_post_cutover_activity_is_not_drift(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(5000, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	gint64 customer = 0;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = import_payload(f, SAMPLE, TRUE, &error);
	g_assert_no_error(error);
	invoice = find_one(f, VENTURE_TYPE_INVOICE, "number", "OB-1");
	g_object_get(invoice, "company-id", &customer, NULL);
	g_object_set(payment, "customer-id", customer, "invoice-id", venture_entity_get_id(invoice),
		"amount", amount, "date", date, "method", "transfer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
	g_assert_true(venture_settlement_service_apply_payment(venture_settlement_service_get(f->db), payment, NULL, &actor, &error));
	g_assert_no_error(error);
	/* The balance query includes the cutoff instant; reconcile does not
	 * count the new receipt as part of the opening. */
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 55000);
	reconcile_ok(f, cutover);
	/* Collecting a migrated invoice is not revenue again. */
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_SALE), ==, 0);
}

/* Rollback reverses every opening journal, voids the documents with their
 * numbers freed, retires the asset and marks the rows; nothing is deleted. */
static void
test_openings_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) asset = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *suffixed = NULL;
	gint state;
	guint i, imported = 0;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	asset_accounts(f);
	cutover = import_payload(f, OPENING_HEAD OPENING_AP OPENING_CREDITS OPENING_ASSETS, TRUE, &error);
	g_assert_no_error(error);
	g_assert_true(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(account_balance(f, "1000", "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "1100", "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "2000", "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "1500", "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "1590", "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "3900", "2026-01-01T00:00:00Z"), ==, 0);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *row_status = NULL;
		gint64 record_id = 0;
		g_object_get(g_ptr_array_index(rows, i), "status", &row_status, "record-id", &record_id, NULL);
		if (record_id > 0)
		{
			imported++;
			g_assert_cmpstr(row_status, ==, "rolled_back");
		}
	}
	g_assert_cmpuint(imported, >=, 8);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 2);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_BILL_PAYMENT), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_FIXED_ASSET), ==, 1);
	suffixed = g_strdup_printf("B-1-rb%" G_GINT64_FORMAT, venture_entity_get_id(cutover));
	bill = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", suffixed);
	g_object_get(bill, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "void");
	g_clear_pointer(&suffixed, g_free);
	suffixed = g_strdup_printf("LAPTOP-1-rb%" G_GINT64_FORMAT, venture_entity_get_id(cutover));
	asset = find_one(f, VENTURE_TYPE_FIXED_ASSET, "tag", suffixed);
	g_object_get(asset, "status", &state, NULL);
	g_assert_cmpint(state, ==, VENTURE_ASSET_STATUS_WRITTEN_OFF);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org,
		FALSE, &actor, &error), ==, 0);
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) credit = find_one(f, VENTURE_TYPE_CUSTOMER_CREDIT, "reference", "CN-1");
		g_autoptr(VentureMoney) remaining = NULL;
		g_autoptr(VentureInvoice) invoice = venture_invoice_new();
		g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
		g_autoptr(VenturePaymentAllocation) allocation = venture_payment_allocation_new();
		g_autoptr(VentureMoney) net = venture_money_new_for_currency(10000, "USD");
		g_autoptr(VentureMoney) tax = venture_money_new_for_currency(0, "USD");
		g_autoptr(VentureMoney) apply = venture_money_new_for_currency(2000, "USD");
		g_autoptr(GDateTime) issued = g_date_time_new_utc(2026, 1, 2, 0, 0, 0);
		gint64 customer_id = 0;

		/* What breaks if this regresses: the reversed credit still
		 * looks unapplied and would settle a later invoice. */
		g_object_get(credit, "remaining", &remaining, "customer-id", &customer_id, NULL);
		g_assert_cmpint(venture_money_get_amount(remaining), ==, 0);
		g_object_set(invoice, "number", "LIVE-1", "company-id", customer_id, "issued-at", issued, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(invoice), f->org);
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), &actor, &error));
		g_assert_no_error(error);
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", "Work", "quantity", 1.0, "unit-price", net, "income-amount", net,
			"tax-amount", tax, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(line), f->org);
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), &actor, &error));
		g_assert_no_error(error);
		g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
		g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), &actor, &error));
		g_assert_no_error(error);
		g_object_set(allocation, "credit-id", venture_entity_get_id(credit),
			"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "amount", apply,
			"date", issued, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(allocation), f->org);
		g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(allocation), &actor, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
}

/* Accumulated depreciation outside zero to cost less salvage is refused by
 * name, and an in-service date at or after the cutoff is not an opening. */
static void
test_openings_asset_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	(void)data;
	asset_accounts(f);
	cutover = import_payload(f, OPENING_HEAD
		"\"open_ap\":[],\"credits\":[],"
		"\"assets\":[{\"source_id\":\"as-9\",\"tag\":\"X\",\"name\":\"X\",\"cost\":\"100 USD\","
		"\"in_service_at\":\"2025-01-01\",\"method\":\"straight_line\",\"useful_life_months\":12,"
		"\"accumulated_depreciation\":\"100 USD\",\"salvage\":\"10 USD\","
		"\"asset_account_code\":\"1500\",\"accumulated_depreciation_account_code\":\"1590\","
		"\"depreciation_expense_account_code\":\"6850\"}]}", FALSE, &error);
	g_assert_nonnull(strstr(error->message, "opening-accumulated-within-basis"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_FIXED_ASSET), ==, 0);
	g_clear_error(&error);
	g_clear_object(&cutover);
	cutover = import_payload(f, OPENING_HEAD
		"\"open_ap\":[],\"credits\":[],"
		"\"assets\":[{\"source_id\":\"as-8\",\"tag\":\"Y\",\"name\":\"Y\",\"cost\":\"100 USD\","
		"\"in_service_at\":\"2026-01-01\",\"method\":\"straight_line\",\"useful_life_months\":12,"
		"\"asset_account_code\":\"1500\",\"accumulated_depreciation_account_code\":\"1590\","
		"\"depreciation_expense_account_code\":\"6850\"}]}", FALSE, &error);
	g_assert_nonnull(strstr(error->message, "assets[0] as-8: in_service_at must be before the cutoff"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_FIXED_ASSET), ==, 0);
}

/* A payload that names assets while the module is off is refused rather than
 * imported as a draft with no register. */
static void
test_openings_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = parse_json(OPENING_HEAD
		"\"open_ap\":[],\"credits\":[],"
		"\"assets\":[{\"source_id\":\"as-1\",\"tag\":\"LAPTOP-1\",\"name\":\"Laptop\",\"cost\":\"100 USD\","
		"\"in_service_at\":\"2025-01-01\",\"useful_life_months\":12,"
		"\"asset_account_code\":\"1500\",\"accumulated_depreciation_account_code\":\"1590\","
		"\"depreciation_expense_account_code\":\"6850\"}]}");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	venture_config_set_module_enabled(f->config, "assets", FALSE);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_null(cutover);
	g_assert_nonnull(strstr(error->message, "assets require the assets module"));
}

/* kind must be customer or vendor; a typo is not inferred. */
static void
test_openings_credit_kind(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	(void)data;
	cutover = import_payload(f, OPENING_HEAD
		"\"credits\":[{\"source_id\":\"cr-x\",\"kind\":\"note\",\"customer_source_id\":\"cust-1\","
		"\"date\":\"2025-12-20\",\"amount\":\"1 USD\"}]}", FALSE, &error);
	g_assert_nonnull(strstr(error->message, "credit-identity"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_CUSTOMER_CREDIT), ==, 0);
}

static GPtrArray *
fiscal_periods(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL));
	return venture_database_find(f->db, query, NULL);
}

static void
fiscal_year(Fixture *f, const gchar *name, gint year, gint month, VenturePeriodLength length)
{
	g_autoptr(GDateTime) start = g_date_time_new_utc(year, month, 1, 0, 0, 0);
	g_autoptr(VentureFiscalYear) created = NULL;
	g_autoptr(GError) error = NULL;
	created = venture_period_service_generate(venture_period_service_get(f->db), f->org, name, start,
		length, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(created);
}

static void
close_period(Fixture *f, VentureEntity *period)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	actor_init(&actor);
	g_object_set(period, "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->db, period, &actor, &error));
	g_assert_no_error(error);
}

/*
 * The opening posting rule. A July cutover with March receivables of 40,000
 * net and 3,200 tax, and a May bill: the documents keep their dates for
 * aging, but the journals land at the cutoff against clearing. What breaks if
 * this regresses: this year's P&L gains 40,000 of revenue the source already
 * reported, the tax return shows 3,200 already filed, and a closed March
 * refuses the import outright.
 */
static void
test_opening_mode_keeps_pnl_and_tax_clean(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(GDateTime) issued = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) opening = NULL;
	g_autoptr(GDateTime) event_date = NULL;
	g_autoptr(VentureQuery) journals = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) posted = NULL;
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	guint i;
	(void)data;
	fiscal_year(f, "2026", 2026, 1, VENTURE_PERIOD_MONTHLY);
	periods = fiscal_periods(f);
	for (i = 0; i < 3; i++)
		close_period(f, g_ptr_array_index(periods, i));
	cutover = import_payload(f,
		"{\"source\":\"quickbooks\",\"cutoff\":\"2026-07-01\",\"currency\":\"USD\","
		"\"customers\":[{\"source_id\":\"cust-1\",\"name\":\"Northwind\"}],"
		"\"vendors\":[{\"source_id\":\"vend-1\",\"name\":\"Contoso\"}],"
		"\"open_ar\":[{\"source_id\":\"inv-40\",\"customer_source_id\":\"cust-1\",\"number\":\"INV-40\","
		"\"date\":\"2026-03-15\",\"due_date\":\"2026-04-14\",\"net\":\"40000 USD\",\"tax\":\"3200 USD\"}],"
		"\"open_ap\":[{\"source_id\":\"bill-7\",\"vendor_source_id\":\"vend-1\",\"number\":\"B-7\","
		"\"date\":\"2026-05-10\",\"lines\":[{\"description\":\"Hosting\",\"amount\":\"1000\",\"tax\":\"80\"}]}]}",
		TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(account_balance(f, "4000", "2026-12-31T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "2100", "2026-12-31T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "6900", "2026-12-31T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "1100", "2026-07-01T00:00:00Z"), ==, 4320000);
	g_assert_cmpint(account_balance(f, "2000", "2026-07-01T00:00:00Z"), ==, -108000);
	g_assert_cmpint(account_balance(f, "3900", "2026-07-01T00:00:00Z"), ==, -4212000);
	/* Nothing is in the ledger before the cutoff. */
	g_assert_cmpint(account_balance(f, "1100", "2026-06-30T23:59:59Z"), ==, 0);
	venture_query_set_organization(journals, f->org);
	venture_query_set_limit(journals, 0);
	posted = venture_database_find(f->db, journals, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(posted->len, >, 0);
	for (i = 0; i < posted->len; i++)
	{
		g_autoptr(GDateTime) when = NULL;
		g_object_get(g_ptr_array_index(posted, i), "occurred-at", &when, NULL);
		g_assert_cmpint(g_date_time_get_month(when), ==, 7);
		g_assert_cmpint(g_date_time_get_day_of_month(when), ==, 1);
	}
	invoice = find_one(f, VENTURE_TYPE_INVOICE, "number", "INV-40");
	g_object_get(invoice, "issued-at", &issued, "due-at", &due, "opening-at", &opening, NULL);
	g_assert_cmpint(g_date_time_get_month(issued), ==, 3);
	g_assert_cmpint(g_date_time_get_day_of_month(due), ==, 14);
	g_assert_cmpint(g_date_time_get_month(opening), ==, 7);
	event = find_one(f, VENTURE_TYPE_INVOICE_EVENT, "kind", "issue");
	g_object_get(event, "date", &event_date, NULL);
	g_assert_cmpint(g_date_time_get_month(event_date), ==, 3);
	{
		static const gchar *const months[] = { "2026-03", "2026-05", "2026-07" };
		VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "tax_liability");
		guint m;
		g_assert_nonnull(report);
		for (m = 0; m < G_N_ELEMENTS(months); m++)
		{
			g_autoptr(VentureDateRange) period = venture_date_range_parse(months[m], utc, 1, &error);
			g_autoptr(VentureReportResult) result = NULL;
			g_assert_no_error(error);
			result = venture_report_generate(report, f->context, period, NULL, &error);
			g_assert_no_error(error);
			g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 0);
		}
	}
	/* The US filing pack reads frozen issue-event tax directly; March's pack
	 * must not carry the migrated invoice's 3,200. */
	{
		g_autoptr(GDateTime) march = g_date_time_new_utc(2026, 3, 1, 0, 0, 0);
		g_autoptr(GDateTime) april = g_date_time_new_utc(2026, 4, 1, 0, 0, 0);
		g_autoptr(VentureEntity) filing = NULL;
		g_autofree gchar *pack = NULL;
		VentureActor clerk;
		actor_init(&clerk);
		filing = venture_tax_filing_service_prepare(venture_tax_filing_service_get(f->db), f->org, "US", "US-NY",
			0, march, april, &clerk, &error);
		g_assert_no_error(error);
		g_object_get(filing, "json-pack", &pack, NULL);
		g_assert_null(strstr(pack, "3200"));
		g_assert_null(strstr(pack, "invoice_event_tax"));
	}
	{
		g_autoptr(VentureEntity) opening_bill = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-7");
		g_autoptr(VentureEntity) stamped = NULL;
		g_object_set(opening_bill, "memo", "edited after the cutover", NULL);
		/* A generic edit is judged at the cutoff instant, not May. */
		g_assert_true(venture_database_save(f->db, opening_bill, NULL, &error));
		g_assert_no_error(error);
		g_object_set(opening_bill, "opening-at", NULL, NULL);
		g_assert_false(venture_database_save(f->db, opening_bill, NULL, &error));
		g_assert_nonnull(strstr(error->message, "Opening balance marks"));
		g_clear_error(&error);
		(void)stamped;
	}
	reconcile_ok(f, cutover);
}

/*
 * The trial balance posts what no subledger owns and asserts what one does.
 * What breaks if this regresses: the "trial balance ties" line appears for a
 * migration whose receivables disagree with the source ledger.
 */
static void
test_trial_balance_ties(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	(void)data;
	cutover = import_payload(f, OPENING_HEAD
		"\"trial_balance\":[{\"account_code\":\"1000\",\"debit\":\"500 USD\"},"
		"{\"account_code\":\"1100\",\"debit\":\"105 USD\"},{\"account_code\":\"2500\",\"credit\":\"200 USD\"},"
		"{\"account_code\":\"3000\",\"credit\":\"405 USD\"}]}", TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(account_balance(f, "2500", "2026-01-01T00:00:00Z"), ==, -20000);
	g_assert_cmpint(account_balance(f, "3000", "2026-01-01T00:00:00Z"), ==, -40500);
	g_assert_cmpint(account_balance(f, "1100", "2026-01-01T00:00:00Z"), ==, 10500);
	g_assert_cmpint(account_balance(f, "1000", "2026-01-01T00:00:00Z"), ==, 50000);
	g_assert_cmpint(account_balance(f, "3900", "2026-01-01T00:00:00Z"), ==, 0);
	reconcile_ok(f, cutover);
}

static void
test_trial_balance_mismatch(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) unbalanced = NULL;
	g_autofree gchar *report = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	unbalanced = import_payload(f, OPENING_HEAD
		"\"trial_balance\":[{\"account_code\":\"1000\",\"debit\":\"500 USD\"},"
		"{\"account_code\":\"3000\",\"credit\":\"400 USD\"}]}", FALSE, &error);
	g_assert_nonnull(strstr(error->message, "debits exceed credits by 100.00 USD in USD"));
	g_clear_error(&error);
	cutover = import_payload(f, OPENING_HEAD
		"\"trial_balance\":[{\"account_code\":\"1000\",\"debit\":\"500 USD\"},"
		"{\"account_code\":\"1100\",\"debit\":\"150 USD\"},{\"account_code\":\"3000\",\"credit\":\"650 USD\"}]}", TRUE, &error);
	g_assert_no_error(error);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "control account 1100: the trial balance says 150.00 USD but the imported subledgers will hold 105.00 USD"));
	g_clear_pointer(&report, g_free);
	g_assert_false(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(strstr(error->message, "reconcile failed"));
	g_clear_error(&error);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "FAIL trial_balance 1100 (receivables) USD: expected 150.00 USD, ledger 105.00 USD, difference -45.00 USD"));
	g_assert_nonnull(strstr(report, "FAIL clearing 3900 USD: expected 0.00 USD, ledger 45.00 USD"));
	g_assert_null(strstr(report, "Trial balance ties"));
}

/* 1,000 USD and 800 EUR of receivables reconcile per currency. What breaks
 * if this regresses: a mixed-currency payload fails reconcile forever. */
static void
test_multi_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autofree gchar *report = NULL;
	(void)data;
	cutover = import_payload(f,
		"{\"source\":\"xero\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"customers\":[{\"source_id\":\"cust-1\",\"name\":\"Acme\"}],"
		"\"open_ar\":[{\"source_id\":\"inv-u\",\"customer_source_id\":\"cust-1\",\"number\":\"U-1\",\"date\":\"2025-12-01\",\"amount\":\"1000 USD\"},"
		"{\"source_id\":\"inv-e\",\"customer_source_id\":\"cust-1\",\"number\":\"E-1\",\"date\":\"2025-12-02\",\"amount\":\"800 EUR\"}],"
		"\"bank_balances\":[{\"source_id\":\"b-usd\",\"name\":\"USD account\",\"account_code\":\"1000\",\"amount\":\"500 USD\"},"
		"{\"source_id\":\"b-eur\",\"name\":\"EUR account\",\"account_code\":\"1000\",\"amount\":\"300 EUR\"}]}", TRUE, &error);
	g_assert_no_error(error);
	reconcile_ok(f, cutover);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "PASS open_ar USD: expected 1000.00 USD"));
	g_assert_nonnull(strstr(report, "PASS open_ar EUR: expected 800.00 EUR"));
	g_assert_nonnull(strstr(report, "PASS bank 1000 EUR: expected 300.00 EUR, ledger 300.00 EUR"));
	g_assert_cmpint(balance_in(f, "1100", "EUR", "2026-01-01T00:00:00Z"), ==, 80000);
}

/*
 * Rollback after the books moved on: a migrated vendor credit applied to a
 * migrated bill, a real payment on another bill, and depreciation already run
 * for January. What breaks if this regresses: rollback is refused forever
 * ("Only an unpaid bill can be voided"), or leaves 97.91 of depreciation on
 * an asset whose cost was reversed.
 */
static void
test_rollback_unwinds_activity(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) bill_one = NULL;
	g_autoptr(VentureEntity) bill_two = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(GPtrArray) blockers = NULL;
	g_autoptr(VentureBillPaymentAllocation) allocation = venture_bill_payment_allocation_new();
	g_autoptr(VentureBillPayment) payment = venture_bill_payment_new();
	g_autoptr(VentureMoney) fifty = venture_money_new_for_currency(5000, "USD");
	g_autoptr(VentureMoney) hundred = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) jan2 = g_date_time_new_utc(2026, 1, 2, 0, 0, 0);
	g_autoptr(GDateTime) jan5 = g_date_time_new_utc(2026, 1, 5, 0, 0, 0);
	g_autofree gchar *number = NULL;
	gint64 vendor = 0;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	asset_accounts(f);
	cutover = import_payload(f, OPENING_HEAD OPENING_AP
		"\"credits\":[{\"source_id\":\"cr-2\",\"kind\":\"vendor\",\"vendor_source_id\":\"vend-1\","
		"\"date\":\"2025-12-21\",\"amount\":\"50 USD\"}]," OPENING_ASSETS, TRUE, &error);
	g_assert_no_error(error);
	bill_one = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-1");
	bill_two = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-2");
	credit = find_one(f, VENTURE_TYPE_VENDOR_CREDIT, "reference", "cr-2");
	g_object_get(bill_two, "company-id", &vendor, NULL);
	g_object_set(allocation, "credit-id", venture_entity_get_id(credit), "bill-id", venture_entity_get_id(bill_one),
		"amount", fifty, "date", jan5, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(allocation), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(allocation), &actor, &error));
	g_assert_no_error(error);
	g_object_set(payment, "vendor-id", vendor, "bill-id", venture_entity_get_id(bill_two), "amount", hundred,
		"date", jan2, "method", "transfer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
	g_assert_true(venture_payables_service_apply_payment(venture_payables_service_get(f->db), payment, NULL, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org,
		FALSE, &actor, &error), ==, 1);
	g_assert_no_error(error);
	blockers = venture_cutover_service_rollback_preflight(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(blockers->len, ==, 0);
	g_assert_true(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	number = g_strdup_printf("B-1-rb%" G_GINT64_FORMAT, venture_entity_get_id(cutover));
	g_clear_object(&bill_one);
	bill_one = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", number);
	{
		g_autofree gchar *status = NULL;
		g_object_get(bill_one, "status", &status, NULL);
		g_assert_cmpstr(status, ==, "void");
	}
	/* The real payment still left the bank; the supplier now holds it as an
	 * unapplied deposit rather than against a bill that no longer exists. */
	g_assert_cmpint(account_balance(f, "1000", "2026-02-01T00:00:00Z"), ==, -10000);
	g_assert_cmpint(account_balance(f, "2000", "2026-02-01T00:00:00Z"), ==, 10000);
	g_assert_cmpint(account_balance(f, "3900", "2026-02-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "6850", "2026-02-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "1590", "2026-02-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "1500", "2026-02-01T00:00:00Z"), ==, 0);
	{
		g_autoptr(VentureEntity) deposit = find_one(f, VENTURE_TYPE_VENDOR_CREDIT, "kind", "deposit");
		g_autoptr(VentureMoney) remaining = NULL;
		g_object_get(deposit, "remaining", &remaining, NULL);
		g_assert_cmpint(venture_money_get_amount(remaining), ==, 10000);
	}
}

/* What cannot be unwound is listed before anything changes. What breaks if
 * this regresses: a rollback stops halfway through a closed January. */
static void
test_rollback_preflight_blockers(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureCustomerCredit) credit = venture_customer_credit_new();
	g_autoptr(VenturePaymentAllocation) allocation = venture_payment_allocation_new();
	g_autoptr(VentureMoney) ten = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GDateTime) jan3 = g_date_time_new_utc(2026, 1, 3, 0, 0, 0);
	g_autoptr(GPtrArray) blockers = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autofree gchar *report = NULL;
	gint64 customer = 0;
	gint status;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	fiscal_year(f, "2025", 2025, 7, VENTURE_PERIOD_QUARTERLY);
	cutover = import_payload(f, SAMPLE, TRUE, &error);
	g_assert_no_error(error);
	invoice = find_one(f, VENTURE_TYPE_INVOICE, "number", "OB-1");
	g_object_get(invoice, "company-id", &customer, NULL);
	g_object_set(credit, "customer-id", customer, "date", jan3, "amount", ten, "kind", "credit_note",
		"reference", "LIVE-CN", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(credit), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(credit), &actor, &error));
	g_assert_no_error(error);
	g_object_set(allocation, "credit-id", venture_entity_get_id(VENTURE_ENTITY(credit)),
		"invoice-id", venture_entity_get_id(invoice), "amount", ten, "date", jan3, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(allocation), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(allocation), &actor, &error));
	g_assert_no_error(error);
	periods = fiscal_periods(f);
	close_period(f, g_ptr_array_index(periods, 0));
	close_period(f, g_ptr_array_index(periods, 1));
	close_period(f, g_ptr_array_index(periods, 2));
	blockers = venture_cutover_service_rollback_preflight(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(blockers->len, >=, 2);
	g_assert_false(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(strstr(error->message, "rollback is blocked"));
	g_assert_nonnull(strstr(error->message, "an applied credit cannot be unapplied"));
	g_assert_nonnull(strstr(error->message, "cannot be posted"));
	g_clear_error(&error);
	g_clear_object(&invoice);
	invoice = find_one(f, VENTURE_TYPE_INVOICE, "number", "OB-1");
	g_object_get(invoice, "status", &status, NULL);
	g_assert_cmpint(status, !=, VENTURE_INVOICE_STATUS_VOID);
	{
		g_autofree gchar *state = state_of(cutover);
		g_assert_cmpstr(state, ==, "imported");
	}
}

/* Every row problem is collected and located, including JSON a spreadsheet
 * export produces. What breaks if this regresses: import stops at the first
 * bad row, or a numeric amount reads as "monetary amount is required". */
static void
test_preview_collects_located_problems(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	asset_accounts(f);
	cutover = preview_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"vendors\":[{\"source_id\":\"vend-1\",\"name\":\"Supplier\"},{\"source_id\":\"vend-2\",\"name\":\"Other\"}],"
		"\"open_ar\":[{\"source_id\":\"inv-1\",\"customer_name\":\"Acme\",\"number\":1001,\"amount\":105.0}],"
		"\"open_ap\":[{\"source_id\":\"bill-1\",\"vendor_source_id\":\"vend-1\",\"number\":1001,\"date\":\"2025-12-01\","
		"\"lines\":[{\"amount\":105.0}]},"
		"{\"source_id\":\"bill-2\",\"vendor_source_id\":\"vend-2\",\"number\":\"1001\",\"date\":\"2025-12-02\","
		"\"lines\":[{\"amount\":\"20.00\"}]},"
		"{\"source_id\":\"bill-1\",\"vendor_source_id\":\"vend-1\",\"number\":\"1002\",\"date\":\"2025-12-02\","
		"\"lines\":[{\"amount\":\"20.00\"}]},42],"
		"\"assets\":[{\"source_id\":\"as-1\",\"cost\":\"100 USD\",\"in_service_at\":\"2025-01-01\",\"useful_life_months\":\"36\","
		"\"asset_account_code\":\"1500\",\"accumulated_depreciation_account_code\":\"1590\","
		"\"depreciation_expense_account_code\":\"6850\"}]}");
	assert_row_exception(f, cutover, "open_ar", 0, "date is required");
	assert_row_exception(f, cutover, "open_ap", 1, "bill numbers are unique per organization");
	assert_row_exception(f, cutover, "open_ap", 2, "source_id also used by open_ap[0]");
	assert_row_exception(f, cutover, "open_ap", 3, "must be an object, not a number");
	{
		g_autoptr(VentureEntity) row = preview_row(f, cutover, "open_ap", 0);
		g_autoptr(VentureEntity) asset_row = preview_row(f, cutover, "asset", 0);
		g_autofree gchar *status = NULL;
		g_autofree gchar *asset_status = NULL;
		g_object_get(row, "status", &status, NULL);
		g_object_get(asset_row, "status", &asset_status, NULL);
		g_assert_cmpstr(status, ==, "preview");
		g_assert_cmpstr(asset_status, ==, "preview");
	}
	g_assert_false(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(strstr(error->message, "4 row problems; nothing was imported"));
	g_assert_nonnull(strstr(error->message, "open_ar[0] inv-1: date is required"));
	g_assert_nonnull(strstr(error->message, "open_ap[1] bill-2: number 1001"));
	g_assert_nonnull(strstr(error->message, "open_ap[3]: must be an object"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 0);
}

/* A rolled-back batch frees its numbers and tags, and a later batch leaves
 * rows another live batch owns out of its expectations. What breaks if this
 * regresses: correcting one row means renumbering every document. */
static void
test_reimport_after_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) third = NULL;
	g_autoptr(VentureEntity) live = NULL;
	g_autofree gchar *report = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	asset_accounts(f);
	first = import_payload(f, OPENING_HEAD OPENING_AP "\"credits\":[]," OPENING_ASSETS, TRUE, &error);
	g_assert_no_error(error);
	g_assert_true(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(first), &actor, &error));
	g_assert_no_error(error);
	second = import_payload(f, OPENING_HEAD OPENING_AP "\"credits\":[]," OPENING_ASSETS, TRUE, &error);
	g_assert_no_error(error);
	reconcile_ok(f, second);
	live = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-1");
	g_clear_object(&live);
	live = find_one(f, VENTURE_TYPE_FIXED_ASSET, "tag", "LAPTOP-1");
	/* The customer and vendor created by the first batch are matched, not
	 * duplicated, by the external id they were stamped with. */
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMPANY), ==, 2);
	third = import_payload(f, OPENING_HEAD "\"open_ap\":[]}", TRUE, &error);
	g_assert_no_error(error);
	reconcile_ok(f, third);
	report = report_of(third);
	g_assert_nonnull(strstr(report, "Owned by other batches and not counted here:"));
	g_assert_nonnull(strstr(report, "open_ar[0] inv-1: already imported by cutover batch"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 2);
}

/* Locale-formatted amounts are refused unless the payload declares its
 * separators. What breaks if this regresses: "1.234,56 EUR" imports as 1.23
 * and "1,50 EUR" as 150.00. */
static void
test_strict_money(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(VentureEntity) european = NULL;
	(void)data;
	refused = preview_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"bank_balances\":["
		"{\"source_id\":\"b0\",\"name\":\"A\",\"account_code\":\"1000\",\"amount\":\"1.234,56 EUR\"},"
		"{\"source_id\":\"b1\",\"name\":\"B\",\"account_code\":\"1000\",\"amount\":\"1,50 EUR\"},"
		"{\"source_id\":\"b2\",\"name\":\"C\",\"account_code\":\"1000\",\"amount\":\"\xe2\x82\xac" "1.234\"},"
		"{\"source_id\":\"b3\",\"name\":\"D\",\"account_code\":\"1000\",\"amount\":\"100.00 CR\"},"
		"{\"source_id\":\"b4\",\"name\":\"E\",\"account_code\":\"1000\",\"amount\":\"1.005 USD\"},"
		"{\"source_id\":\"b5\",\"name\":\"F\",\"account_code\":\"1000\",\"amount\":\"1.5 JPY\"},"
		"{\"source_id\":\"b6\",\"name\":\"G\",\"account_code\":\"1000\",\"amount\":\"12.34 usd\"}]}");
	assert_row_exception(f, refused, "bank", 0, "is not an amount");
	assert_row_exception(f, refused, "bank", 1, "is not an amount");
	assert_row_exception(f, refused, "bank", 2, "must include an ISO currency");
	assert_row_exception(f, refused, "bank", 3, "must include an ISO currency");
	assert_row_exception(f, refused, "bank", 4, "has 3 decimal places but USD has 2");
	assert_row_exception(f, refused, "bank", 5, "has 1 decimal places but JPY has 0");
	assert_row_exception(f, refused, "bank", 6, "lowercase");
	european = import_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"decimal_separator\":\",\",\"thousands_separator\":\".\","
		"\"bank_balances\":[{\"source_id\":\"b0\",\"name\":\"Konto\",\"account_code\":\"1000\",\"amount\":\"1.234,56 EUR\"}]}",
		TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance_in(f, "1000", "EUR", "2026-01-01T00:00:00Z"), ==, 123456);
}

/* Activation re-runs reconcile and closes every period through the cutoff.
 * What breaks if this regresses: a batch reconciled in the morning activates
 * on figures an afternoon payment changed, and December stays open under the
 * opening balances. */
static void
test_activation_rechecks(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10500, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2025, 12, 20, 0, 0, 0);
	g_autofree gchar *state = NULL;
	gint64 customer = 0;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = import_payload(f, SAMPLE, TRUE, &error);
	g_assert_no_error(error);
	reconcile_ok(f, cutover);
	invoice = find_one(f, VENTURE_TYPE_INVOICE, "number", "OB-1");
	g_object_get(invoice, "company-id", &customer, NULL);
	g_object_set(payment, "customer-id", customer, "invoice-id", venture_entity_get_id(invoice),
		"amount", amount, "date", date, "method", "transfer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(payment), f->org);
	g_assert_true(venture_settlement_service_apply_payment(venture_settlement_service_get(f->db), payment, NULL, &actor, &error));
	g_assert_no_error(error);
	g_assert_false(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(strstr(error->message, "activation refused"));
	state = state_of(cutover);
	g_assert_cmpstr(state, ==, "imported");
}

static void
test_activation_closes_periods(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) one = venture_money_new_for_currency(100, "USD");
	g_autoptr(GDateTime) december = g_date_time_new_utc(2025, 12, 31, 0, 0, 0);
	VentureJournalLine *line;
	guint i;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	fiscal_year(f, "FY2026", 2025, 7, VENTURE_PERIOD_QUARTERLY);
	cutover = import_payload(f, SAMPLE, TRUE, &error);
	g_assert_no_error(error);
	reconcile_ok(f, cutover);
	g_assert_true(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	periods = fiscal_periods(f);
	for (i = 0; i < periods->len; i++)
	{
		gint state;
		g_object_get(g_ptr_array_index(periods, i), "state", &state, NULL);
		g_assert_cmpint(state, ==, i < 2 ? VENTURE_PERIOD_CLOSED : VENTURE_PERIOD_OPEN);
	}
	g_object_set(header, "organization-id", f->org, "source-type", "organization", "source-id", f->org,
		"occurred-at", december, "currency", "USD", "memo", "late", NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account_id(f, "1000"), "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", one, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account_id(f, "3000"), "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", one, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), header, lines, NULL, &actor, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

/* Chart rows create missing accounts, account_map translates source codes,
 * and party details reach the companies created for them. */
static void
test_chart_map_and_parties(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) existing = VENTURE_ENTITY(venture_company_new());
	g_autoptr(VentureEntity) acme = NULL;
	g_autoptr(VentureEntity) created = NULL;
	g_autoptr(VentureEntity) ambiguous = NULL;
	g_autofree gchar *email = NULL;
	g_autofree gchar *address = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *external = NULL;
	guint i;
	(void)data;
	g_object_set(existing, "name", "ACME   corp", NULL);
	venture_entity_set_organization_id(existing, f->org);
	g_assert_true(venture_database_save(f->db, existing, NULL, &error));
	g_assert_no_error(error);
	cutover = import_payload(f,
		"{\"source\":\"quickbooks\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"account_map\":{\"QB-CASH\":\"1010\"},"
		"\"chart\":[{\"code\":\"QB-CASH\",\"name\":\"Operating account\",\"kind\":\"asset\"}],"
		"\"customers\":[{\"source_id\":\"c-acme\",\"name\":\"Acme Corp\"},"
		"{\"source_id\":\"c-new\",\"name\":\"Globex\",\"email\":\"ap@globex.example\",\"address\":\"1 Main St\",\"terms\":\"Net 30\"}],"
		"\"open_ar\":[{\"source_id\":\"i-1\",\"customer_source_id\":\"c-acme\",\"number\":\"A-1\",\"date\":\"2025-12-01\",\"amount\":\"10 USD\"},"
		"{\"source_id\":\"i-2\",\"customer_source_id\":\"c-new\",\"number\":\"G-1\",\"date\":\"2025-12-01\",\"amount\":\"20 USD\"},"
		"{\"source_id\":\"i-3\",\"customer_name\":\"Walk-in\",\"number\":\"W-1\",\"date\":\"2025-12-01\",\"amount\":\"1 USD\"},"
		"{\"source_id\":\"i-4\",\"customer_name\":\"walk-in\",\"number\":\"W-2\",\"date\":\"2025-12-02\",\"amount\":\"2 USD\"}],"
		"\"bank_balances\":[{\"source_id\":\"b-1\",\"name\":\"Operating\",\"account_code\":\"QB-CASH\",\"amount\":\"75 USD\"}]}",
		TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(account_balance(f, "1010", "2026-01-01T00:00:00Z"), ==, 7500);
	acme = find_one(f, VENTURE_TYPE_INVOICE, "number", "A-1");
	{
		gint64 company = 0;
		g_object_get(acme, "company-id", &company, NULL);
		g_assert_cmpint(company, ==, venture_entity_get_id(existing));
	}
	created = find_one(f, VENTURE_TYPE_COMPANY, "name", "Globex");
	g_object_get(created, "email", &email, "address", &address, "notes", &notes, "external-id", &external, NULL);
	g_assert_cmpstr(email, ==, "ap@globex.example");
	g_assert_cmpstr(address, ==, "1 Main St");
	g_assert_nonnull(strstr(notes, "Net 30"));
	g_assert_cmpstr(external, ==, "c-new");
	/* Rows with only a name share one company per name. */
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMPANY), ==, 3);
	reconcile_ok(f, cutover);
	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureEntity) twin = VENTURE_ENTITY(venture_company_new());
		g_object_set(twin, "name", "Beta", NULL);
		venture_entity_set_organization_id(twin, f->org);
		g_assert_true(venture_database_save(f->db, twin, NULL, &error));
		g_assert_no_error(error);
	}
	ambiguous = preview_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\","
		"\"open_ar\":[{\"source_id\":\"i-9\",\"customer_name\":\"beta\",\"number\":\"Z-1\",\"date\":\"2025-12-01\",\"amount\":\"1 USD\"}]}");
	assert_row_exception(f, ambiguous, "open_ar", 0, "matches 2 companies by name");
}

/* Receivable rows need a date, carry their due date, record what was paid,
 * and total one way for import and reconcile. */
static void
test_ar_dates_paid_and_totals(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) payment = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autoptr(GDateTime) event_due = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	(void)data;
	refused = preview_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"open_ar\":[{\"source_id\":\"x\",\"customer_name\":\"Acme\",\"number\":\"X-1\",\"amount\":\"100\"},"
		"{\"source_id\":\"y\",\"customer_name\":\"Acme\",\"number\":\"Y-1\",\"date\":\"2025-12-01\",\"amount\":\"100\",\"net\":\"90\",\"tax\":\"5\"},"
		"{\"source_id\":\"z\",\"customer_name\":\"Acme\",\"number\":\"Z-1\",\"date\":\"2026-01-01\",\"amount\":\"100\"}]}");
	assert_row_exception(f, refused, "open_ar", 0, "date is required");
	assert_row_exception(f, refused, "open_ar", 1, "is not net plus tax");
	assert_row_exception(f, refused, "open_ar", 2, "date must be before the cutoff");
	cutover = import_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"open_ar\":[{\"source_id\":\"inv-1\",\"customer_name\":\"Acme\",\"number\":\"OB-1\",\"date\":\"2025-11-15\","
		"\"due_date\":\"2025-12-15\",\"amount\":\"100\",\"tax\":\"5\",\"paid\":\"30\",\"balance\":\"70\"}],"
		"\"bank_balances\":[{\"source_id\":\"b\",\"name\":\"Checking\",\"account_code\":\"1000\",\"amount\":\"500\"}]}",
		TRUE, &error);
	g_assert_no_error(error);
	invoice = find_one(f, VENTURE_TYPE_INVOICE, "number", "OB-1");
	g_object_get(invoice, "due-at", &due, NULL);
	g_assert_nonnull(due);
	g_assert_cmpint(g_date_time_get_day_of_month(due), ==, 15);
	event = find_one(f, VENTURE_TYPE_INVOICE_EVENT, "kind", "issue");
	g_object_get(event, "due-at", &event_due, "amount", &total, NULL);
	g_assert_nonnull(event_due);
	/* amount is the gross total: 100 including the 5 of tax, not 105. */
	g_assert_cmpint(venture_money_get_amount(total), ==, 10000);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db),
		venture_entity_get_id(invoice), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 7000);
	payment = find_one(f, VENTURE_TYPE_PAYMENT, "method", "opening");
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 50000);
	g_assert_cmpint(account_balance(f, "1100", "2026-01-01T00:00:00Z"), ==, 7000);
	reconcile_ok(f, cutover);
}

/* A retainer with no invoice, a supplier deposit, a credit card owing money
 * and a receivable that is money owed back. What breaks if this regresses:
 * any of these legitimate balances is refused. */
static void
test_prepayments_and_negatives(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) deposit = NULL;
	g_autoptr(VentureEntity) vendor_deposit = NULL;
	g_autoptr(VentureEntity) owed_back = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(VentureMoney) remaining = NULL;
	(void)data;
	make_account(f, "2300", VENTURE_ACCOUNT_KIND_LIABILITY);
	cutover = import_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"credits\":[{\"source_id\":\"ret-1\",\"kind\":\"customer\",\"type\":\"prepayment\",\"customer_name\":\"Retainer Co\","
		"\"date\":\"2025-11-02\",\"amount\":\"2000\"},"
		"{\"source_id\":\"dep-1\",\"kind\":\"vendor\",\"type\":\"prepayment\",\"vendor_name\":\"Landlord\","
		"\"date\":\"2025-10-01\",\"amount\":\"1500\"}],"
		"\"open_ar\":[{\"source_id\":\"neg-1\",\"customer_name\":\"Overpaid Inc\",\"number\":\"OP-1\",\"date\":\"2025-12-03\",\"amount\":\"-40\"}],"
		"\"bank_balances\":[{\"source_id\":\"b\",\"name\":\"Checking\",\"account_code\":\"1000\",\"amount\":\"3000\"},"
		"{\"source_id\":\"card\",\"name\":\"Visa\",\"account_code\":\"2300\",\"amount\":\"-1800\"}]}", TRUE, &error);
	g_assert_no_error(error);
	deposit = find_one(f, VENTURE_TYPE_CUSTOMER_CREDIT, "kind", "deposit");
	g_object_get(deposit, "remaining", &remaining, NULL);
	g_assert_cmpint(venture_money_get_amount(remaining), ==, 200000);
	vendor_deposit = find_one(f, VENTURE_TYPE_VENDOR_CREDIT, "kind", "deposit");
	g_assert_nonnull(vendor_deposit);
	owed_back = find_one(f, VENTURE_TYPE_CUSTOMER_CREDIT, "reference", "OP-1");
	g_assert_nonnull(owed_back);
	/* Cash is the bank balance alone; the prepayments were already in it. */
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 300000);
	g_assert_cmpint(account_balance(f, "2300", "2026-01-01T00:00:00Z"), ==, -180000);
	g_assert_cmpint(account_balance(f, "1100", "2026-01-01T00:00:00Z"), ==, -204000);
	g_assert_cmpint(account_balance(f, "2000", "2026-01-01T00:00:00Z"), ==, 150000);
	reconcile_ok(f, cutover);
	refused = preview_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"open_ap\":[{\"source_id\":\"b\",\"vendor_name\":\"Landlord\",\"number\":\"N-1\",\"date\":\"2025-12-01\","
		"\"lines\":[{\"amount\":\"-10\"}]}]}");
	assert_row_exception(f, refused, "open_ap", 0, "record a supplier credit under credits with kind vendor");
}

/* Two banks reconcile per GL account; two banks silently sharing the default
 * are refused. What breaks if this regresses: Checking 500 and Savings 200
 * both fail reconcile against a 700 total. */
static void
test_bank_per_account(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) ambiguous = NULL;
	g_autofree gchar *report = NULL;
	(void)data;
	make_account(f, "1010", VENTURE_ACCOUNT_KIND_ASSET);
	cutover = import_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"bank_balances\":[{\"source_id\":\"chk\",\"name\":\"Checking\",\"account_code\":\"1000\",\"amount\":\"500\"},"
		"{\"source_id\":\"sav\",\"name\":\"Savings\",\"account_code\":\"1010\",\"amount\":\"200\"}]}", TRUE, &error);
	g_assert_no_error(error);
	reconcile_ok(f, cutover);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "PASS bank 1000 USD: expected 500.00 USD, ledger 500.00 USD"));
	g_assert_nonnull(strstr(report, "PASS bank 1010 USD: expected 200.00 USD, ledger 200.00 USD"));
	ambiguous = preview_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"bank_balances\":[{\"source_id\":\"a\",\"name\":\"One\",\"amount\":\"1\"},"
		"{\"source_id\":\"b\",\"name\":\"Two\",\"amount\":\"2\"}]}");
	assert_row_exception(f, ambiguous, "bank", 1, "has no account_code");
}

/* Over the row limit a batch is refused whole, with advice to split it. */
static void
test_row_limit(Fixture *f, gconstpointer data)
{
	g_autoptr(GString) json = g_string_new("{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"customers\":[");
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	guint i;
	(void)data;
	for (i = 0; i < 5001; i++)
		g_string_append_printf(json, "%s{\"source_id\":\"c-%u\",\"name\":\"C %u\"}", i > 0 ? "," : "", i, i);
	g_string_append(json, "]}");
	payload = parse_json(json->str);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db), f->org, payload, NULL, &error);
	g_assert_null(cutover);
	g_assert_nonnull(strstr(error->message, "5001 rows is over the 5000-row limit"));
	g_assert_nonnull(strstr(error->message, "split the migration into batches"));
}

static GPtrArray *
csv_rows(const gchar *const *lines)
{
	GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	guint i;
	for (i = 0; lines[i] != NULL; i++)
		g_ptr_array_add(rows, g_strsplit(lines[i], ",", -1));
	return rows;
}

/* CSV exports become the same payload JSON, bill lines grouped by source id,
 * and preview like any payload. What breaks if this regresses: a spreadsheet
 * migration needs hand-written JSON, or a misspelled column drops a figure. */
static void
test_csv_payload(Fixture *f, gconstpointer data)
{
	static const gchar *const vendors[] = { "source_id,name,email", "vend-1,Supplier,ap@supplier.example", NULL };
	static const gchar *const open_ap[] = {
		"source_id,vendor_source_id,number,date,paid,line_description,line_amount,line_tax",
		"bill-1,vend-1,B-1,2025-12-10,30,Hosting,100,5",
		"bill-1,vend-1,,,,Support,50,",
		"bill-2,vend-1,B-2,2025-12-20,,Parts,200,", NULL
	};
	static const gchar *const bank[] = { "Source ID,Name,Account code,Amount", "bank-1,Checking,QB-1000,500", NULL };
	static const gchar *const map[] = { "source_code,venture_code", "QB-1000,1000", NULL };
	static const gchar *const typo[] = { "source_id,name,ammount", "b,Checking,5", NULL };
	g_autoptr(GHashTable) tables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	g_autoptr(JsonObject) settings = parse_json("{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\"}");
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *template = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	template = venture_cutover_csv_template("open_ap");
	g_assert_nonnull(strstr(template, "line_amount"));
	g_assert_null(venture_cutover_csv_template("ledger"));
	g_hash_table_insert(tables, g_strdup("vendors"), csv_rows(vendors));
	g_hash_table_insert(tables, g_strdup("open_ap"), csv_rows(open_ap));
	g_hash_table_insert(tables, g_strdup("bank_balances"), csv_rows(bank));
	g_hash_table_insert(tables, g_strdup("account_map"), csv_rows(map));
	payload = venture_cutover_service_payload_from_csv(venture_cutover_service_get(f->db), settings, tables, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(payload, "open_ap")), ==, 2);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db), f->org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	bill = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-1");
	balance = venture_payables_service_bill_balance(venture_payables_service_get(f->db),
		venture_entity_get_id(bill), NULL, &error);
	g_assert_no_error(error);
	/* 100 + 5 + 50 less 30 paid. */
	g_assert_cmpint(venture_money_get_amount(balance), ==, 12500);
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 50000);
	reconcile_ok(f, cutover);
	g_clear_pointer(&tables, g_hash_table_unref);
	g_clear_pointer(&payload, json_object_unref);
	tables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	g_hash_table_insert(tables, g_strdup("bank_balances"), csv_rows(typo));
	g_hash_table_insert(tables, g_strdup("ledger"), csv_rows(map));
	payload = venture_cutover_service_payload_from_csv(venture_cutover_service_get(f->db), settings, tables, &error);
	g_assert_null(payload);
	g_assert_nonnull(strstr(error->message, "bank_balances.csv: unknown column \"ammount\""));
	g_assert_nonnull(strstr(error->message, "ledger is not a cutover CSV section"));
}

static gint64
stock_item(Fixture *f, const gchar *sku)
{
	g_autoptr(VentureProduct) product = venture_product_new();
	g_autoptr(VentureInventoryItem) item = venture_inventory_item_new();
	g_autoptr(GError) error = NULL;
	g_object_set(product, "name", sku, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(product), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(product), NULL, &error));
	g_assert_no_error(error);
	g_object_set(item, "product-id", venture_entity_get_id(VENTURE_ENTITY(product)), "sku", sku, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(item), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(item), NULL, &error));
	g_assert_no_error(error);
	return venture_entity_get_id(VENTURE_ENTITY(item));
}

#define STOCK_PAYLOAD \
	"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\"," \
	"\"inventory\":[{\"source_id\":\"stk-1\",\"sku\":\"WIDGET\",\"quantity\":10,\"unit_cost\":\"12.50\"}]," \
	"\"trial_balance\":[{\"account_code\":\"1200\",\"debit\":\"125\"},{\"account_code\":\"3000\",\"credit\":\"125\"}]}"

/* Stock on hand is received at the cutoff against clearing, reconciles as a
 * control account, and returns through the inventory service on rollback.
 * What breaks if this regresses: opening stock has no cost layer to sell
 * from, or its value sits in goods received not invoiced forever. */
static void
test_inventory_on_hand(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(GPtrArray) blockers = NULL;
	g_autofree gchar *report = NULL;
	g_autofree gchar *grni = NULL;
	gint64 item;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	refused = preview_payload(f,
		"{\"source\":\"generic\",\"cutoff\":\"2026-01-01\",\"currency\":\"USD\","
		"\"inventory\":[{\"source_id\":\"stk-9\",\"sku\":\"NOPE\",\"quantity\":0,\"unit_cost\":\"1\"}]}");
	assert_row_exception(f, refused, "inventory", 0, "quantity must be a positive whole number");
	item = stock_item(f, "WIDGET");
	cutover = import_payload(f, STOCK_PAYLOAD, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_inventory_service_on_hand(venture_inventory_service_get(f->db), item, NULL, &error), ==, 10);
	g_assert_cmpint(account_balance(f, "1200", "2026-01-01T00:00:00Z"), ==, 12500);
	/* The inventory service creates goods received not invoiced under the
	 * organization-scoped code when the chart lacks it. */
	grni = g_strdup_printf("%" G_GINT64_FORMAT ":2010", f->org);
	g_assert_cmpint(account_balance(f, grni, "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "3900", "2026-01-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "3000", "2026-01-01T00:00:00Z"), ==, -12500);
	reconcile_ok(f, cutover);
	report = report_of(cutover);
	g_assert_nonnull(strstr(report, "PASS inventory USD: expected 125.00 USD, subledger 125.00 USD"));
	g_assert_nonnull(strstr(report, "PASS trial_balance 1200 (inventory) USD"));
	blockers = venture_cutover_service_rollback_preflight(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(blockers->len, ==, 0);
	g_assert_true(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_inventory_service_on_hand(venture_inventory_service_get(f->db), item, NULL, &error), ==, 0);
	g_assert_cmpint(account_balance(f, "1200", "2026-02-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, grni, "2026-02-01T00:00:00Z"), ==, 0);
	g_assert_cmpint(account_balance(f, "3900", "2026-02-01T00:00:00Z"), ==, 0);
}

static void
test_inventory_sold_blocks_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(GPtrArray) blockers = NULL;
	g_autoptr(GDateTime) jan5 = g_date_time_new_utc(2026, 1, 5, 0, 0, 0);
	gint64 item;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	item = stock_item(f, "WIDGET");
	cutover = import_payload(f, STOCK_PAYLOAD, TRUE, &error);
	g_assert_no_error(error);
	g_assert_true(venture_inventory_service_issue(venture_inventory_service_get(f->db), item, 3, jan5,
		"organization", f->org, &actor, NULL, &error));
	g_assert_no_error(error);
	blockers = venture_cutover_service_rollback_preflight(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(blockers->len, ==, 1);
	g_assert_nonnull(strstr(g_ptr_array_index(blockers, 0), "7 of the 10 units imported are still on hand"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/cutover/preview-import-activate", Fixture, NULL, setup, test_preview_import_activate, teardown);
	g_test_add("/cutover/idempotent-rollback", Fixture, NULL, setup, test_idempotent_rollback, teardown);
	g_test_add("/cutover/generic-write", Fixture, NULL, setup, test_generic_write_refused, teardown);
	g_test_add("/cutover/preview-opening-rows", Fixture, NULL, setup, test_preview_opening_rows, teardown);
	g_test_add("/cutover/currency-required", Fixture, NULL, setup, test_currency_required, teardown);
	g_test_add("/cutover/exact-opening-tax", Fixture, NULL, setup, test_exact_opening_tax, teardown);
	g_test_add("/cutover/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	g_test_add("/cutover/openings-import", Fixture, NULL, setup, test_openings_import, teardown);
	g_test_add("/cutover/openings-credit-exceeds", Fixture, NULL, setup, test_openings_credit_exceeds, teardown);
	g_test_add("/cutover/openings-atomic", Fixture, NULL, setup, test_openings_atomic, teardown);
	g_test_add("/cutover/openings-write-failure", Fixture, NULL, setup, test_openings_write_failure, teardown);
	g_test_add("/cutover/openings-out-of-balance", Fixture, NULL, setup, test_openings_out_of_balance, teardown);
	g_test_add("/cutover/post-cutover-activity", Fixture, NULL, setup, test_post_cutover_activity_is_not_drift, teardown);
	g_test_add("/cutover/openings-rollback", Fixture, NULL, setup, test_openings_rollback, teardown);
	g_test_add("/cutover/openings-asset-refusals", Fixture, NULL, setup, test_openings_asset_refusals, teardown);
	g_test_add("/cutover/openings-module-off", Fixture, NULL, setup, test_openings_module_off, teardown);
	g_test_add("/cutover/openings-credit-kind", Fixture, NULL, setup, test_openings_credit_kind, teardown);
	g_test_add("/cutover/opening-mode", Fixture, NULL, setup, test_opening_mode_keeps_pnl_and_tax_clean, teardown);
	g_test_add("/cutover/trial-balance-ties", Fixture, NULL, setup, test_trial_balance_ties, teardown);
	g_test_add("/cutover/trial-balance-mismatch", Fixture, NULL, setup, test_trial_balance_mismatch, teardown);
	g_test_add("/cutover/multi-currency", Fixture, NULL, setup, test_multi_currency, teardown);
	g_test_add("/cutover/rollback-unwinds", Fixture, NULL, setup, test_rollback_unwinds_activity, teardown);
	g_test_add("/cutover/rollback-blockers", Fixture, NULL, setup, test_rollback_preflight_blockers, teardown);
	g_test_add("/cutover/located-problems", Fixture, NULL, setup, test_preview_collects_located_problems, teardown);
	g_test_add("/cutover/reimport-after-rollback", Fixture, NULL, setup, test_reimport_after_rollback, teardown);
	g_test_add("/cutover/strict-money", Fixture, NULL, setup, test_strict_money, teardown);
	g_test_add("/cutover/activation-rechecks", Fixture, NULL, setup, test_activation_rechecks, teardown);
	g_test_add("/cutover/activation-closes-periods", Fixture, NULL, setup, test_activation_closes_periods, teardown);
	g_test_add("/cutover/chart-map-parties", Fixture, NULL, setup, test_chart_map_and_parties, teardown);
	g_test_add("/cutover/ar-dates-paid-totals", Fixture, NULL, setup, test_ar_dates_paid_and_totals, teardown);
	g_test_add("/cutover/prepayments-negatives", Fixture, NULL, setup, test_prepayments_and_negatives, teardown);
	g_test_add("/cutover/bank-per-account", Fixture, NULL, setup, test_bank_per_account, teardown);
	g_test_add("/cutover/row-limit", Fixture, NULL, setup, test_row_limit, teardown);
	g_test_add("/cutover/csv-payload", Fixture, NULL, setup, test_csv_payload, teardown);
	g_test_add("/cutover/inventory-on-hand", Fixture, NULL, setup, test_inventory_on_hand, teardown);
	g_test_add("/cutover/inventory-sold-blocks-rollback", Fixture, NULL, setup, test_inventory_sold_blocks_rollback, teardown);
	return g_test_run();
}
