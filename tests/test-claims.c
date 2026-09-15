/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"expense_claim"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"expense_claim_line"), !=, G_TYPE_INVALID);
}

static gint64
count_type(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(
		venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static VentureEntity *
make_claim(Fixture *f, const gchar *number, const gchar *date)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_iso8601(date, NULL);
	VentureEntity *claim = VENTURE_ENTITY(venture_expense_claim_new());
	venture_entity_set_organization_id(claim, f->org);
	g_object_set(claim, "number", number, "claim-date", when, "currency", "USD",
		"status", "draft", "settlement", "cash", NULL);
	save(f, claim);
	return claim;
}

static VentureEntity *
receipt_line(Fixture *f, gint64 claim_id, const gchar *desc, const gchar *amount, gint64 document_id)
{
	g_autoptr(VentureMoney) money = venture_money_from_string(amount, NULL, NULL);
	VentureEntity *line = VENTURE_ENTITY(venture_expense_claim_line_new());
	venture_entity_set_organization_id(line, f->org);
	g_object_set(line, "claim-id", claim_id, "kind", "receipt", "description", desc,
		"amount", money, "miles", "0", "document-id", document_id, NULL);
	return line;
}

static void
test_mileage_amount(void)
{
	g_autoptr(VentureExpenseClaimLine) line = venture_expense_claim_line_new();
	g_autoptr(VentureMoney) rate = venture_money_new_for_currency(67, "USD");
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(line, "kind", "mileage", "description", "Site visit",
		"miles", "100", "mileage-rate", rate, NULL);
	amount = venture_expense_claim_line_get_amount(line, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 6700);
}

static void
test_submit_approve_pay_cash(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) claim = make_claim(f, "CLM-1", "2026-02-01T00:00:00Z");
	g_autoptr(VentureEntity) line = receipt_line(f, venture_entity_get_id(claim), "Taxi", "12.50 USD", 0);
	g_autofree gchar *status = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	(void)unused;
	save(f, line);
	g_assert_true(venture_claims_service_submit(venture_claims_service_get(f->db),
		claim, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_claims_service_approve(venture_claims_service_get(f->db),
		claim, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_claims_service_pay(venture_claims_service_get(f->db),
		claim, NULL, &error));
	g_assert_no_error(error);
	g_object_get(claim, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "paid");
	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		"expense_claim", venture_entity_get_id(claim), f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(journals->len, >=, 1);
}

static void
test_payable_settlement(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) claim = make_claim(f, "CLM-AP", "2026-02-02T00:00:00Z");
	g_autoptr(VentureEntity) line = receipt_line(f, venture_entity_get_id(claim), "Hotel", "80.00 USD", 0);
	g_autofree gchar *status = NULL;
	(void)unused;
	g_object_set(claim, "settlement", "payable", NULL);
	save(f, claim);
	save(f, line);
	g_assert_true(venture_claims_service_submit(venture_claims_service_get(f->db), claim, NULL, &error));
	g_assert_true(venture_claims_service_approve(venture_claims_service_get(f->db), claim, NULL, &error));
	g_assert_true(venture_claims_service_pay(venture_claims_service_get(f->db), claim, NULL, &error));
	g_assert_no_error(error);
	g_object_get(claim, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "paid");
}

static void
test_duplicate_receipt_hash(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) claim = make_claim(f, "CLM-DUP", "2026-02-03T00:00:00Z");
	g_autoptr(VentureDocument) document = venture_document_new();
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	(void)unused;
	venture_entity_set_organization_id(VENTURE_ENTITY(document), f->org);
	g_object_set(document, "title", "Taxi receipt", "kind", "receipt", "hash", "deadbeef", NULL);
	save(f, VENTURE_ENTITY(document));
	first = receipt_line(f, venture_entity_get_id(claim), "Taxi", "12.50 USD",
		venture_entity_get_id(VENTURE_ENTITY(document)));
	g_assert_true(venture_database_save(f->db, first, NULL, &error));
	g_assert_no_error(error);
	second = receipt_line(f, venture_entity_get_id(claim), "Same taxi", "12.50 USD",
		venture_entity_get_id(VENTURE_ENTITY(document)));
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_nonnull(strstr(error->message, "duplicate"));
}

static void
test_closed_period_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) year = NULL;
	g_autoptr(GPtrArray) periods = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) claim = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(GDateTime) start = g_date_time_new_from_iso8601("2026-01-01T00:00:00Z", NULL);
	g_autoptr(GDateTime) end = g_date_time_new_from_iso8601("2027-01-01T00:00:00Z", NULL);
	VentureActor actor;
	(void)unused;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "closer";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "name", "FY2026",
		"organization-id", f->org, "start-at", start, "end-at", end,
		"period-length", VENTURE_PERIOD_MONTHLY, NULL);
	g_assert_true(venture_database_save(f->db, year, NULL, &error));
	g_assert_no_error(error);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	periods = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(periods->len, >=, 1);
	g_object_set(g_ptr_array_index(periods, 0), "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->db, g_ptr_array_index(periods, 0), &actor, &error));
	g_assert_no_error(error);
	claim = make_claim(f, "CLM-CLOSED", "2026-01-15T00:00:00Z");
	line = receipt_line(f, venture_entity_get_id(claim), "January lunch", "9.00 USD", 0);
	save(f, line);
	g_assert_false(venture_claims_service_submit(venture_claims_service_get(f->db),
		claim, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_generic_status_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) claim = make_claim(f, "CLM-GEN", "2026-02-04T00:00:00Z");
	(void)unused;
	g_object_set(claim, "status", "approved", NULL);
	g_assert_false(venture_database_save(f->db, claim, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureClaimsService"));
}

static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) claim = make_claim(f, "CLM-OFF", "2026-02-05T00:00:00Z");
	(void)unused;
	venture_config_set_module_enabled(f->config, "claims", FALSE);
	g_assert_false(venture_claims_service_submit(venture_claims_service_get(f->db),
		claim, NULL, &error));
	g_assert_nonnull(error);
	venture_config_set_module_enabled(f->config, "claims", TRUE);
}

static void
test_migration(Fixture *f, gconstpointer unused)
{
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	result = venture_database_query_raw(f->db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version = 280", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
	g_assert_cmpint(count_type(f, "expense_claim"), ==, 0);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} HttpResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpResult *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *path, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	HttpResult response;
	guint status;
	memset(&response, 0, sizeof(response));
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new("GET", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
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
test_web_claims(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autofree gchar *state_dir = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(VentureEntity) claim = make_claim(f, "CLM-WEB", "2026-02-06T00:00:00Z");
	guint16 port;
	(void)unused;
	state_dir = g_dir_make_tmp("venture-claims-XXXXXX", &error);
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_cmpuint(http_request(server, "/claims", &body), ==, 200);
	g_assert_nonnull(strstr(body, "Claims"));
	g_assert_nonnull(strstr(body, "CLM-WEB"));
	venture_web_server_stop(server);
	venture_test_remove_tree(state_dir);
	(void)claim;
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/claims/records", test_records);
	g_test_add_func("/claims/mileage", test_mileage_amount);
	g_test_add("/claims/pay-cash", Fixture, NULL, setup, test_submit_approve_pay_cash, teardown);
	g_test_add("/claims/pay-payable", Fixture, NULL, setup, test_payable_settlement, teardown);
	g_test_add("/claims/duplicate-hash", Fixture, NULL, setup, test_duplicate_receipt_hash, teardown);
	g_test_add("/claims/closed-period", Fixture, NULL, setup, test_closed_period_refused, teardown);
	g_test_add("/claims/generic-status", Fixture, NULL, setup, test_generic_status_refused, teardown);
	g_test_add("/claims/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/claims/migration", Fixture, NULL, setup, test_migration, teardown);
	g_test_add("/claims/web", Fixture, NULL, setup, test_web_claims, teardown);
	return g_test_run();
}
