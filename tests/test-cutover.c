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

/* Preview writes a cutover row per opening source id so import can match
 * them. What breaks if this regresses: a payload that names bills, credits
 * and assets would preview with no rows and import would look untracked. */
static void
test_preview_opening_rows(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = parse_json(
		"{\"source\":\"zoho_books\",\"cutoff\":\"2026-01-01\","
		"\"open_ap\":[{\"source_id\":\"b1\",\"amount\":\"10 USD\"}],"
		"\"credits\":[{\"source_id\":\"cr1\"}],\"assets\":[{\"source_id\":\"as1\"}]}");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
	g_autoptr(GPtrArray) rows = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db),
		f->org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(cutover);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(rows->len, ==, 3);
}

static gint64
account_balance(Fixture *f, const gchar *code, const gchar *when)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GDateTime) as_of = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) balance = NULL;
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, code, NULL));
	accounts = venture_database_find(f->db, q, NULL);
	g_assert_cmpuint(accounts->len, ==, 1);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		venture_entity_get_id(g_ptr_array_index(accounts, 0)), f->org, "USD", as_of, NULL);
	g_assert_nonnull(balance);
	return venture_money_get_amount(balance);
}

static void
asset_accounts(Fixture *f)
{
	const gchar *codes[] = { "1500", "1590", "6850" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(codes); i++)
	{
		g_autoptr(VentureEntity) account = VENTURE_ENTITY(venture_account_new());
		g_autoptr(GError) error = NULL;
		g_object_set(account, "organization-id", f->org, "name", codes[i], "code", codes[i],
			"kind", i == 2 ? VENTURE_ACCOUNT_KIND_EXPENSE : VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
		g_assert_true(venture_database_save(f->db, account, NULL, &error));
		g_assert_no_error(error);
	}
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

static VentureEntity *
import_payload(Fixture *f, const gchar *json, gboolean expect_ok, GError **error)
{
	g_autoptr(JsonObject) payload = parse_json(json);
	VentureEntity *cutover;
	VentureActor actor;
	actor_init(&actor);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(f->db), f->org, payload, &actor, error);
	g_assert_nonnull(cutover);
	g_assert_cmpint(venture_cutover_service_import(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, error), ==, expect_ok);
	return cutover;
}

static guint
count_type(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_limit(query, 0);
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

/* Rules 1-3 and the widened reconcile: bills keep their number, dates and
 * frozen tax; a partial payment is an opening payment against the full bill;
 * credits become unapplied balances; the asset's history is one opening
 * balance and the next run continues from the cutoff. */
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
	g_object_get(bill, "status", &status, "bill-date", &bill_date, NULL);
	g_assert_cmpstr(status, ==, "partially_paid");
	g_assert_cmpint(g_date_time_get_month(bill_date), ==, 12);
	g_assert_cmpint(g_date_time_get_day_of_month(bill_date), ==, 10);
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
	g_object_get(cutover, "reconciliation-report", &report, NULL);
	g_assert_nonnull(strstr(report, "Open AP 27500"));
	g_assert_nonnull(strstr(report, "Credits 7000"));
	g_assert_nonnull(strstr(report, "Asset NBV 235000"));
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

/* Rule 2: a credit larger than its counterpart's open balance is refused by
 * name, and the whole import rolls back with it. */
static void
test_openings_credit_exceeds(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	(void)data;
	cutover = import_payload(f, OPENING_HEAD
		"\"credits\":[{\"source_id\":\"cr-9\",\"kind\":\"customer\",\"customer_source_id\":\"cust-1\","
		"\"date\":\"2025-12-20\",\"amount\":\"200 USD\"}]}", FALSE, &error);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "credit-within-open-balance"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_CUSTOMER_CREDIT), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
}

/* Rule 5: a failure after the first bill is written leaves nothing. */
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
	g_assert_nonnull(strstr(error->message, "open-ap-balance"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_VENDOR_BILL_EVENT), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_object_get(cutover, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "preview");
}

/* Rule 4: a subledger that drifted after import fails reconcile, and an
 * unreconciled batch cannot activate. */
static void
test_openings_out_of_balance(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) cutover = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureBillPayment) payment = venture_bill_payment_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 2, 0, 0, 0);
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
	g_assert_nonnull(strstr(error->message, "opening AP"));
	g_clear_error(&error);
	g_assert_false(venture_cutover_service_activate(venture_cutover_service_get(f->db),
		VENTURE_ACCOUNTING_CUTOVER(cutover), &actor, &error));
	g_assert_nonnull(error);
}

/* Rule 4: rollback before activation reverses every opening journal, voids
 * the bills, retires the asset and marks the rows; nothing is deleted. */
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
	g_assert_cmpint(account_balance(f, "3000", "2026-01-01T00:00:00Z"), ==, 0);
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
	bill = find_one(f, VENTURE_TYPE_VENDOR_BILL, "number", "B-1");
	g_object_get(bill, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "void");
	asset = find_one(f, VENTURE_TYPE_FIXED_ASSET, "tag", "LAPTOP-1");
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

/* Rule 3: accumulated depreciation outside zero to cost less salvage is
 * refused by name, and an in-service date after the cutoff is refused. */
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
		"\"in_service_at\":\"2026-06-01\",\"method\":\"straight_line\",\"useful_life_months\":12,"
		"\"asset_account_code\":\"1500\",\"accumulated_depreciation_account_code\":\"1590\","
		"\"depreciation_expense_account_code\":\"6850\"}]}", FALSE, &error);
	g_assert_nonnull(strstr(error->message, "in service on or before the cutoff"));
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
	(void)cutover;
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
	g_test_add("/cutover/preview-opening-rows", Fixture, NULL, setup, test_preview_opening_rows, teardown);
	g_test_add("/cutover/currency-required", Fixture, NULL, setup, test_currency_required, teardown);
	g_test_add("/cutover/exact-opening-tax", Fixture, NULL, setup, test_exact_opening_tax, teardown);
	g_test_add("/cutover/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	g_test_add("/cutover/openings-import", Fixture, NULL, setup, test_openings_import, teardown);
	g_test_add("/cutover/openings-credit-exceeds", Fixture, NULL, setup, test_openings_credit_exceeds, teardown);
	g_test_add("/cutover/openings-atomic", Fixture, NULL, setup, test_openings_atomic, teardown);
	g_test_add("/cutover/openings-out-of-balance", Fixture, NULL, setup, test_openings_out_of_balance, teardown);
	g_test_add("/cutover/openings-rollback", Fixture, NULL, setup, test_openings_rollback, teardown);
	g_test_add("/cutover/openings-asset-refusals", Fixture, NULL, setup, test_openings_asset_refusals, teardown);
	g_test_add("/cutover/openings-module-off", Fixture, NULL, setup, test_openings_module_off, teardown);
	g_test_add("/cutover/openings-credit-kind", Fixture, NULL, setup, test_openings_credit_kind, teardown);
	return g_test_run();
}
