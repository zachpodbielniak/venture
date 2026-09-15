/*
 * test-cutover.c - Guided Zoho Books / QuickBooks opening-balance cutover.
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

static JsonObject *
sample_payload(void)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	const gchar *json =
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
		"\"assets\":[],\"unsupported\":[\"payroll_item\"]}";
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
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
cash_balance(Fixture *f, const gchar *when)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) balance = NULL;
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, "1000", NULL));
	accounts = venture_database_find(f->db, q, NULL);
	g_assert_cmpuint(accounts->len, ==, 1);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		venture_entity_get_id(g_ptr_array_index(accounts, 0)), f->org, "USD", as_of, NULL);
	g_assert_nonnull(balance);
	return venture_money_get_amount(balance);
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
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&state, g_free);
	g_object_get(cutover, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "imported");
	g_assert_cmpint(cash_balance(f, "2026-01-01T00:00:00Z"), ==, 50000);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(rows->len, >, 0);
	g_assert_true(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&report, g_free);
	g_object_get(cutover, "reconciliation-report", &report, NULL);
	g_assert_nonnull(strstr(report, "Trial balance ties"));
	g_assert_true(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_clear_pointer(&state, g_free);
	g_object_get(cutover, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "active");
	g_assert_false(venture_cutover_service_rollback(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(error);
}

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
		g_object_get(g_ptr_array_index(invoices, 0), "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_VOID);
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
	const gchar *content_type, const gchar *body, gchar **out)
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

static void
test_surfaces(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autofree gchar *dir = g_dir_make_tmp("venture-cutover-XXXXXX", NULL);
	g_autofree gchar *body = NULL;
	g_autofree gchar *payload = NULL;
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
	g_assert_cmpuint(http_request(server, "GET", "/", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Cutover"));
	g_clear_pointer(&body, g_free);
	{
		g_autoptr(JsonObject) object = sample_payload();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, json_object_ref(object));
		payload = venture_json_to_string(node, FALSE);
	}
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/preview",
		"application/json", payload, &body), ==, 200);
	g_assert_nonnull(strstr(body, "preview"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "GET", "/e/accounting_cutover/1", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Mapped source JSON"));
	g_assert_nonnull(strstr(body, "name=\"action\""));
	g_assert_nonnull(strstr(body, "import"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_cutovers/1/import",
		"application/json", "{}", &body), ==, 200);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/cutover/1/action",
		"application/x-www-form-urlencoded", "action=reconcile", NULL), ==, 302);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
}

static JsonObject *
parse_json(const gchar *json)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static void
test_refuse_unimported(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = parse_json(
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"open_ap\":[{\"source_id\":\"b1\",\"amount\":\"10 USD\"}]}");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_null(cutover);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "open_ap"));
}

static void
test_currency_required(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = parse_json(
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"bank_balances\":[{\"source_id\":\"bank-1\",\"name\":\"Checking\","
		"\"account_code\":\"1000\",\"amount\":\"500\"}]}");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_false(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "currency"));
}

static void
test_exact_opening_tax(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = parse_json(
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"customers\":[{\"source_id\":\"cust-1\",\"name\":\"Acme\"}],"
		"\"open_ar\":[{\"source_id\":\"inv-1\",\"customer_source_id\":\"cust-1\","
		"\"number\":\"OB-2\",\"net\":\"33.33 USD\",\"tax\":\"2.50 USD\","
		"\"date\":\"2025-12-15\"}]}");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
	g_autoptr(VentureMoney) tax = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_true(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
	venture_query_set_limit(query, 0);
	events = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(events->len, ==, 1);
	g_object_get(g_ptr_array_index(events, 0), "tax-amount", &tax, NULL);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 250);
	g_assert_true(venture_cutover_service_reconcile(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_no_error(error);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/cutover/preview-import-activate", Fixture, NULL, setup, test_preview_import_activate, teardown);
	g_test_add("/cutover/idempotent-rollback", Fixture, NULL, setup, test_idempotent_rollback, teardown);
	g_test_add("/cutover/generic-write", Fixture, NULL, setup, test_generic_write_refused, teardown);
	g_test_add("/cutover/refuse-unimported", Fixture, NULL, setup, test_refuse_unimported, teardown);
	g_test_add("/cutover/currency-required", Fixture, NULL, setup, test_currency_required, teardown);
	g_test_add("/cutover/exact-opening-tax", Fixture, NULL, setup, test_exact_opening_tax, teardown);
	g_test_add("/cutover/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	return g_test_run();
}
