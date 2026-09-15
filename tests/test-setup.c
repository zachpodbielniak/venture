/*
 * test-setup.c - Guided accounting setup and configurable control accounts.
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

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "bookkeeper";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static gint64
new_org(Fixture *f, const gchar *slug)
{
	g_autoptr(VentureOrganization) org = venture_organization_new();
	g_autoptr(GError) error = NULL;
	g_object_set(org, "name", slug, "slug", slug, "kind", VENTURE_ORGANIZATION_KIND_SOLE_PROPRIETOR,
		"default-currency", "USD", "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(org), NULL, &error));
	g_assert_no_error(error);
	return venture_entity_get_id(VENTURE_ENTITY(org));
}

static JsonObject *
setup_payload(void)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	const gchar *json =
		"{\"legal_name\":\"Northwind LLC\",\"book_currency\":\"USD\","
		"\"fiscal_year_start\":\"2026-01-01\",\"basis\":\"accrual\","
		"\"tax_profile\":\"standard\",\"chart_template\":\"standard\","
		"\"bank_name\":\"Checking\",\"opening_cash\":\"0 USD\",\"period_length\":\"monthly\"}";
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static gboolean
step_done(JsonNode *checklist, const gchar *key)
{
	JsonArray *steps = json_node_get_array(checklist);
	guint i;
	for (i = 0; i < json_array_get_length(steps); i++)
	{
		JsonObject *step = json_array_get_object_element(steps, i);
		if (g_strcmp0(json_object_get_string_member(step, "key"), key) == 0)
			return json_object_get_boolean_member(step, "done");
	}
	return FALSE;
}

static void
test_checklist(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) checklist = NULL;
	g_autoptr(GError) error = NULL;
	gint64 org = new_org(f, "fresh-co");
	(void)data;
	checklist = venture_setup_service_checklist(venture_setup_service_get(f->db), org, &error);
	g_assert_no_error(error);
	g_assert_false(step_done(checklist, "control_accounts"));
	g_assert_false(step_done(checklist, "fiscal_year"));
	g_assert_false(step_done(checklist, "banks"));
	g_assert_true(step_done(checklist, "legal_entity"));
}

static void
test_validation_refusal(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(JsonObject) payload = setup_payload();
	g_autoptr(VentureEntity) setup = NULL;
	g_autoptr(GError) error = NULL;
	gint64 org = new_org(f, "unguided");
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	setup = venture_setup_service_preview(venture_setup_service_get(f->db), org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(setup);
	g_object_set(customer, "name", "Buyer", "organization-id", org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), NULL, &error));
	g_object_set(invoice, "number", "INV-1", "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"organization-id", org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", "2026-01-10", NULL));
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(invoice), NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "control account"));
	g_assert_true(strstr(error->message, "invoice") != NULL);
}

static void
test_wrong_kind_map(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureAccountingControlMap) map = venture_accounting_control_map_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) sales = NULL;
	(void)data;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "4000", NULL));
	sales = venture_database_find_one(f->db, query, &error);
	g_assert_nonnull(sales);
	g_object_set(map, "organization-id", f->org, "classification", "cash",
		"account-id", venture_entity_get_id(sales), "subject-type", "organization", NULL);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(map), NULL, &error));
	g_assert_nonnull(strstr(error->message, "cash"));
	g_assert_nonnull(strstr(error->message, "asset"));
}

static gint64
cell_amount(VentureReportResult *r, const gchar *key)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
	{
		const GValue *name = venture_report_result_get_cell(r, i, "key");
		if (name != NULL && g_strcmp0(g_value_get_string(name), key) == 0)
		{
			const GValue *value = venture_report_result_get_cell(r, i, "current");
			g_assert_nonnull(value);
			return ((const VentureMoney *)g_value_get_boxed(value))->amount;
		}
	}
	g_error("missing row %s", key);
	return 0;
}

static void
complete_org(Fixture *f, gint64 org, VentureActor *actor)
{
	g_autoptr(JsonObject) payload = setup_payload();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) setup = NULL;
	setup = venture_setup_service_preview(venture_setup_service_get(f->db), org, payload, actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_setup_service_complete(venture_setup_service_get(f->db),
		VENTURE_ACCOUNTING_SETUP(setup), actor, &error));
	g_assert_no_error(error);
}

static void
test_custom_code_still_cash(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureEntity) cash = NULL;
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(5000, "USD");
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601("2026-01-15T00:00:00Z", NULL);
	VentureJournalLine *line;
	gint64 org = new_org(f, "custom-chart");
	gint64 cash_id, income_id;
	VentureActor actor;
	VentureReport *report;
	(void)data;
	actor_init(&actor);
	complete_org(f, org, &actor);
	cash_id = venture_setup_resolve_account(f->db, org, "cash", "organization", 0, NULL, &error);
	income_id = venture_setup_resolve_account(f->db, org, "income", "organization", 0, NULL, &error);
	g_assert_cmpint(cash_id, >, 0);
	cash = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, cash_id, &error);
	g_object_set(cash, "code", "CASH-OP", NULL);
	g_assert_true(venture_database_save(f->db, cash, &actor, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_setup_resolve_account(f->db, org, "cash", "organization", 0, NULL, &error), ==, cash_id);
	g_object_set(header, "organization-id", org, "source-type", "organization", "source-id", org,
		"occurred-at", when, "currency", "USD", "memo", "Custom cash sale", NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", cash_id, "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", amount, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", income_id, "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", amount, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), header, lines, NULL, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	period = venture_context_parse_period(f->context, "2026-01", &error);
	json_object_set_int_member(options, "organization_id", org);
	json_object_set_string_member(options, "currency", "USD");
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "cash_flow");
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell_amount(result, "cash_end"), ==, 5000);
	g_assert_true(venture_setup_account_classified(f->db, org, cash_id, "cash", NULL));
}

static void
test_new_business(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = setup_payload();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) setup = NULL;
	g_autoptr(JsonNode) checklist = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureInvoice) invoice = venture_invoice_new();
	g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
	g_autoptr(VenturePayment) payment = venture_payment_new();
	g_autoptr(JsonObject) args = json_object_new();
	g_autoptr(VentureQuery) banks = venture_query_new(VENTURE_TYPE_BANK_ACCOUNT);
	g_autoptr(GPtrArray) bank_rows = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	g_autoptr(VentureEntity) created = NULL;
	g_autoptr(VentureQuery) txns = NULL;
	g_autoptr(GPtrArray) txn_rows = NULL;
	gint64 org = new_org(f, "northwind");
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	setup = venture_setup_service_preview(venture_setup_service_get(f->db), org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_true(venture_setup_service_complete(venture_setup_service_get(f->db),
		VENTURE_ACCOUNTING_SETUP(setup), &actor, &error));
	g_assert_no_error(error);
	checklist = venture_setup_service_checklist(venture_setup_service_get(f->db), org, &error);
	g_assert_true(step_done(checklist, "legal_entity"));
	g_assert_true(step_done(checklist, "book_currency"));
	g_assert_true(step_done(checklist, "fiscal_year"));
	g_assert_true(step_done(checklist, "basis"));
	g_assert_true(step_done(checklist, "tax_profile"));
	g_assert_true(step_done(checklist, "chart_template"));
	g_assert_true(step_done(checklist, "banks"));
	g_assert_true(step_done(checklist, "opening_balances"));
	g_assert_true(step_done(checklist, "control_accounts"));
	venture_query_set_organization(banks, org);
	bank_rows = venture_database_find(f->db, banks, &error);
	g_assert_cmpuint(bank_rows->len, ==, 1);
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "0 USD");
	json_object_set_string_member(args, "closing_balance", "-10 USD");
	json_object_set_string_member(args, "format", "csv");
	json_object_set_string_member(args, "data", "Date,Amount,Memo,Ref,ID\n2026-01-22,-10,Supplies,January,exp-1\n");
	statement = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db), "import",
		venture_entity_get_id(g_ptr_array_index(bank_rows, 0)), args, &actor, &error);
	g_assert_no_error(error);
	txns = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	venture_query_set_organization(txns, org);
	txn_rows = venture_database_find(f->db, txns, &error);
	g_assert_cmpuint(txn_rows->len, ==, 1);
	json_object_set_string_member(args, "type", "expense");
	created = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db), "create",
		venture_entity_get_id(g_ptr_array_index(txn_rows, 0)), args, &actor, &error);
	g_assert_no_error(error);
	g_clear_object(&created);
	created = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db), "reconcile",
		venture_entity_get_id(statement), args, &actor, &error);
	g_assert_no_error(error);
	g_object_set(customer, "name", "Harbor Co", "organization-id", org, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(customer), &actor, &error));
	g_object_set(invoice, "number", "NW-1", "company-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"organization-id", org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(invoice), "issued-at", "2026-01-10", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), &actor, &error));
	g_assert_no_error(error);
	g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "description", "Work",
		"quantity", 1.0, "organization-id", org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(line), "unit-price", "40 USD", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(line), &actor, &error));
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(invoice), &actor, &error));
	g_object_set(payment, "customer-id", venture_entity_get_id(VENTURE_ENTITY(customer)),
		"invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)), "method", "transfer",
		"organization-id", org, NULL);
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "amount", "40 USD", NULL));
	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(payment), "date", "2026-01-20", NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(payment), &actor, &error));
	g_assert_no_error(error);
	g_assert_nonnull(created);
}

typedef struct { GBytes *bytes; GError *error; gboolean done; } HttpDone;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpDone *done = data;
	done->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &done->error);
	done->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *uri = g_strdup_printf("%s%s", venture_web_server_get_base_url(server), path);
	HttpDone response = { 0 };
	guint status;
	message = soup_message_new(method, uri);
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
	g_autofree gchar *dir = g_dir_make_tmp("venture-setup-XXXXXX", NULL);
	g_autofree gchar *body = NULL;
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
	g_assert_nonnull(strstr(body, "Setup"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "GET", "/setup", NULL, NULL, &body), ==, 200);
	g_assert_true(strstr(body, "Accounting setup") != NULL || strstr(body, "preview") != NULL);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/accounting_setups/preview",
		"application/json", "{\"legal_name\":\"Shown LLC\",\"book_currency\":\"USD\"}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "preview"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "GET", "/e/accounting_setup/1", NULL, NULL, &body), ==, 200);
	g_assert_nonnull(strstr(body, "Accounting setup"));
	g_assert_nonnull(strstr(body, "Complete setup"));
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/setup/checklist", Fixture, NULL, setup, test_checklist, teardown);
	g_test_add("/setup/validation-refusal", Fixture, NULL, setup, test_validation_refusal, teardown);
	g_test_add("/setup/wrong-kind-map", Fixture, NULL, setup, test_wrong_kind_map, teardown);
	g_test_add("/setup/custom-code-still-cash", Fixture, NULL, setup, test_custom_code_still_cash, teardown);
	g_test_add("/setup/new-business", Fixture, NULL, setup, test_new_business, teardown);
	g_test_add("/setup/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	return g_test_run();
}
