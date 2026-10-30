/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * A company is a customer, a supplier, or both.
 *
 * An owner's Customers list held every company -- suppliers and prospects
 * too -- and a company had one relationship, so one you sell to and buy
 * from could only be filed as one of them: either its bills were refused
 * or it fell out of the customer reports. What breaks if these regress:
 * the Customers or Suppliers list shows the wrong companies; a company
 * that is both is missing from one; a bill from a customer who also
 * supplies you is refused; a row saved before the flags existed stops
 * counting as what its relationship says.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
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
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
company(Fixture *f, const gchar *name, const gchar *kind, gboolean supplier)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *row = venture_entity_registry_create(venture_entity_registry_get_default(), "company", NULL);
	venture_entity_set_organization_id(row, f->org);
	g_assert_true(venture_entity_set_field_from_string(row, "name", name, &error));
	g_assert_true(venture_entity_set_field_from_string(row, "kind", kind, &error));
	g_assert_no_error(error);
	if (supplier)
		g_object_set(row, "supplier", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, row, NULL, &error)); g_assert_no_error(error);
	return row;
}

/* The names of the companies whose @flag is set, sorted. */
static gchar *
listed(Fixture *f, const gchar *flag)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) names = g_string_new(NULL);
	guint i;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, flag, VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	for (i = 0; i < rows->len; i++) {
		g_autofree gchar *name = NULL;
		g_object_get(g_ptr_array_index(rows, i), "name", &name, NULL);
		g_string_append_printf(names, "%s%s", names->len ? "," : "", name);
	}
	return g_strdup(names->str);
}

/* Saving a relationship of customer or supplier sets its flag, from any
 * writer; a prospect is neither. */
static void
test_relationship_sets_flag(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) buyer = company(f, "Buyer", "customer", FALSE);
	g_autoptr(VentureEntity) seller = company(f, "Seller", "supplier", FALSE);
	g_autoptr(VentureEntity) maybe = company(f, "Maybe", "prospect", FALSE);
	gboolean customer = FALSE, supplier = FALSE;
	(void)data;
	g_object_get(buyer, "customer", &customer, "supplier", &supplier, NULL);
	g_assert_true(customer); g_assert_false(supplier);
	g_object_get(seller, "customer", &customer, "supplier", &supplier, NULL);
	g_assert_false(customer); g_assert_true(supplier);
	g_object_get(maybe, "customer", &customer, "supplier", &supplier, NULL);
	g_assert_false(customer); g_assert_false(supplier);
}

/* A company you sell to and buy from is in both lists; the others are in
 * one or neither. */
static void
test_both_lists(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = company(f, "Both Ways", "customer", TRUE);
	g_autoptr(VentureEntity) b = company(f, "Only Buys", "customer", FALSE);
	g_autoptr(VentureEntity) c = company(f, "Only Sells", "supplier", FALSE);
	g_autoptr(VentureEntity) d = company(f, "Not Yet", "prospect", FALSE);
	g_autofree gchar *customers = listed(f, "customer");
	g_autofree gchar *suppliers = listed(f, "supplier");
	(void)data;
	g_assert_cmpstr(customers, ==, "Both Ways,Only Buys");
	g_assert_cmpstr(suppliers, ==, "Both Ways,Only Sells");
	g_assert_true(venture_company_is_customer(a)); g_assert_true(venture_company_is_supplier(a));
	g_assert_false(venture_company_is_supplier(b));
	g_assert_false(venture_company_is_customer(c));
	g_assert_false(venture_company_is_customer(d)); g_assert_false(venture_company_is_supplier(d));
}

/* A row saved before the flags existed still counts as what its
 * relationship says. */
static void
test_legacy_relationship(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) old = g_object_new(VENTURE_TYPE_COMPANY, "name", "Old supplier",
		"kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	(void)f; (void)data;
	g_assert_true(venture_company_is_supplier(old));
	g_assert_false(venture_company_is_customer(old));
}

/* A bill is accepted from a customer who also supplies you, and refused
 * from one who does not. */
static void
test_bill_from_customer_who_supplies(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) both = company(f, "Both Ways", "customer", TRUE);
	g_autoptr(VentureEntity) buyer = company(f, "Only Buys", "customer", FALSE);
	g_autoptr(GError) error = NULL;
	VentureEntity *vendors[] = { both, buyer };
	guint i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(vendors); i++) {
		g_autoptr(VentureEntity) bill = venture_entity_registry_create(venture_entity_registry_get_default(), "vendor_bill", NULL);
		g_autoptr(VentureEntity) event = venture_entity_registry_create(venture_entity_registry_get_default(), "vendor_bill_event", NULL);
		g_autoptr(VentureEntity) line = venture_entity_registry_create(venture_entity_registry_get_default(), "vendor_bill_line", NULL);
		g_autofree gchar *number = g_strdup_printf("B-%u", i);
		gboolean saved;
		venture_entity_set_organization_id(bill, f->org);
		g_object_set(bill, "number", number, "company-id", venture_entity_get_id(vendors[i]),
			"currency", "USD", "status", "draft", NULL);
		g_assert_true(venture_entity_set_field_from_string(bill, "bill-date", "2026-01-01", NULL));
		g_assert_true(venture_entity_set_field_from_string(bill, "due-date", "2026-01-31", NULL));
		g_assert_true(venture_database_save(f->db, bill, NULL, &error)); g_assert_no_error(error);
		venture_entity_set_organization_id(line, f->org);
		g_object_set(line, "bill-id", venture_entity_get_id(bill), "description", "Supplies", "quantity", "1", NULL);
		g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "40 USD", NULL));
		g_assert_true(venture_database_save(f->db, line, NULL, &error)); g_assert_no_error(error);
		venture_entity_set_organization_id(event, f->org);
		g_object_set(event, "bill-id", venture_entity_get_id(bill), "vendor-id", venture_entity_get_id(vendors[i]),
			"kind", "approve", "state", "approved", NULL);
		g_assert_true(venture_entity_set_field_from_string(event, "date", "2026-01-01", NULL));
		saved = venture_database_save(f->db, event, NULL, &error);
		if (i == 0) {
			g_assert_no_error(error);
			g_assert_true(saved);
		} else {
			g_assert_false(saved);
			g_assert_nonnull(error);
			g_assert_nonnull(strstr(error->message, "supplier"));
		}
		g_clear_error(&error);
	}
}

/* An install upgraded with companies already filed: each customer and
 * supplier is flagged as one, a prospect as neither, and the upgrade
 * leaves no company with an unset flag. */
static void
test_upgrade_backfill(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-company-roles-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) seen = g_string_new(NULL);
	database = venture_database_new(uri, &error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"INSERT INTO companies (uuid, organization_id, created_at, updated_at, version, name, kind, customer, supplier) VALUES "
		"('c-1', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'Buyer', 'customer', NULL, NULL),"
		"('c-2', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'Seller', 'supplier', NULL, NULL),"
		"('c-3', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'Maybe', 'prospect', NULL, NULL);"
		"DELETE FROM schema_migrations WHERE version >= 980", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&database);
	database = venture_database_new(uri, &error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(database,
		"SELECT name || ':' || CAST(customer AS TEXT) || CAST(supplier AS TEXT) FROM companies ORDER BY uuid", NULL, &error);
	g_assert_no_error(error);
	while (orm_result_next(result))
		g_string_append_printf(seen, "%s ", orm_row_get_string(orm_result_get_row(result), 0));
	g_assert_cmpstr(seen->str, ==, "Buyer:10 Seller:01 Maybe:00 ");
	g_clear_object(&result);
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

typedef struct { gboolean done; GBytes *body; GError *error; } Reply;

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

/* The page at @path, with sign-in off. */
static gchar *
page(VentureWebServer *server, const gchar *path)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	Reply reply = { FALSE, NULL, NULL };
	gchar *text;
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, received, &reply);
	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	text = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	g_bytes_unref(reply.body);
	return text;
}

/* Company names in filter choices are not rows in the result set. */
static gchar *
list_rows(const gchar *html)
{
	const gchar *table = strstr(html, "data-list data-server-sort");
	const gchar *start;
	const gchar *end;

	g_assert_nonnull(table);
	start = strstr(table, "<tbody>");
	g_assert_nonnull(start);
	end = strstr(start, "</tbody>");
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

/* The companies list offers Customers and Suppliers as views of itself,
 * and each holds exactly its companies under its own heading. */
static void
test_list_views(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = company(f, "Both Ways", "customer", TRUE);
	g_autoptr(VentureEntity) b = company(f, "Only Buys", "customer", FALSE);
	g_autoptr(VentureEntity) c = company(f, "Only Sells", "supplier", FALSE);
	g_autoptr(VentureEntity) d = company(f, "Not Yet", "prospect", FALSE);
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-company-views-XXXXXX", NULL);
	g_autofree gchar *all = NULL, *customers = NULL, *suppliers = NULL;
	g_autofree gchar *customer_rows = NULL, *supplier_rows = NULL;
	(void)data;
	g_object_set(f->config, "state-dir", directory, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	all = page(server, "/e/company");
	g_assert_nonnull(strstr(all, "href=\"/e/company?customer=true\">Customers</a>"));
	g_assert_nonnull(strstr(all, "href=\"/e/company?supplier=true\">Suppliers</a>"));
	g_assert_nonnull(strstr(all, "Not Yet"));
	customers = page(server, "/e/company?customer=true");
	customer_rows = list_rows(customers);
	g_assert_nonnull(strstr(customers, "<h1>Customers</h1>"));
	g_assert_nonnull(strstr(customers, "aria-current=\"page\" href=\"/e/company?customer=true\""));
	g_assert_nonnull(strstr(customer_rows, "Both Ways")); g_assert_nonnull(strstr(customer_rows, "Only Buys"));
	g_assert_null(strstr(customer_rows, "Only Sells")); g_assert_null(strstr(customer_rows, "Not Yet"));
	/* The sidebar calls the customers list Customers, and marks it. */
	g_assert_nonnull(strstr(customers, "<a class=\"nav-item active\" href=\"/e/company?customer=true\">"));
	suppliers = page(server, "/e/company?supplier=true");
	supplier_rows = list_rows(suppliers);
	g_assert_nonnull(strstr(suppliers, "<h1>Suppliers</h1>"));
	g_assert_nonnull(strstr(supplier_rows, "Both Ways")); g_assert_nonnull(strstr(supplier_rows, "Only Sells"));
	g_assert_null(strstr(supplier_rows, "Only Buys"));
	g_assert_nonnull(strstr(suppliers, "<a class=\"nav-item active\" href=\"/e/company?supplier=true\">"));
	venture_web_server_stop(server);
	venture_test_remove_tree(directory);
}

static void
test_people_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) linked = company(f, "Linked Customer", "customer", FALSE);
	g_autoptr(VentureEntity) shared = company(f, "Shared Customer", "customer", FALSE);
	g_autoptr(VentureEntity) bare = company(f, "Bare Customer", "customer", FALSE);
	g_autoptr(VentureEntity) supplier = company(f, "Supplier", "supplier", FALSE);
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("venture-people-report-XXXXXX", NULL);
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) json = NULL;
	JsonArray *rows;
	JsonObject *row;

	(void)data;
	g_object_set(shared, "email", "shared", NULL);
	g_assert_true(venture_database_save(f->db, shared, NULL, &error)); g_assert_no_error(error);
	g_object_set(bare, "phone", "555-0100", NULL);
	g_assert_true(venture_database_save(f->db, bare, NULL, &error)); g_assert_no_error(error);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org,
		"name", "Linked Person", "company-id", venture_entity_get_id(linked),
		"email", "linked", NULL);
	g_assert_true(venture_database_save(f->db, contact, NULL, &error)); g_assert_no_error(error);
	g_clear_object(&contact);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org,
		"name", "Shared Person", "email", "SHARED", NULL);
	g_assert_true(venture_database_save(f->db, contact, NULL, &error)); g_assert_no_error(error);
	g_object_set(f->config, "state-dir", directory, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error)); g_assert_no_error(error);
	path = g_strdup_printf("/api/v1/reports/people?organization_id=%" G_GINT64_FORMAT, f->org);
	body = page(server, path);
	json = venture_json_parse(body, &error); g_assert_no_error(error);
	rows = json_object_get_array_member(json_node_get_object(json), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	row = json_array_get_object_element(rows, 0);
	g_assert_cmpstr(json_object_get_string_member(row, "type"), ==, "contact");
	g_assert_cmpstr(json_object_get_string_member(row, "name"), ==, "Linked Person");
	g_assert_cmpstr(json_object_get_string_member(row, "email"), ==, "linked");
	g_assert_cmpstr(json_object_get_string_member(row, "phone"), ==, "");
	g_assert_cmpstr(json_object_get_string_member(row, "source"), ==, "contact");
	row = json_array_get_object_element(rows, 1);
	g_assert_cmpstr(json_object_get_string_member(row, "name"), ==, "Shared Person");
	row = json_array_get_object_element(rows, 2);
	g_assert_cmpstr(json_object_get_string_member(row, "type"), ==, "company");
	g_assert_cmpstr(json_object_get_string_member(row, "name"), ==, "Bare Customer");
	g_assert_cmpstr(json_object_get_string_member(row, "phone"), ==, "555-0100");
	g_assert_cmpstr(json_object_get_string_member(row, "source"), ==, "customer");
	venture_web_server_stop(server);
	venture_test_remove_tree(directory);
}

/* A truncated people list would omit a customer who has no contact.
 * The report refuses past the aggregate bound instead. */
static void
test_people_report_bound(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	VentureReportResult *result;

	(void)data;
	g_object_unref(company(f, "One", "customer", FALSE));
	g_object_unref(company(f, "Two", "customer", FALSE));
	venture_aggregate_set_max_rows(1);
	json_object_set_int_member(options, "organization_id", f->org);
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "people");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, NULL, options, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "cannot return a partial list"));
	venture_aggregate_set_max_rows(0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/company-roles/relationship-sets-flag", Fixture, NULL, setup, test_relationship_sets_flag, teardown);
	g_test_add("/company-roles/both-lists", Fixture, NULL, setup, test_both_lists, teardown);
	g_test_add("/company-roles/legacy-relationship", Fixture, NULL, setup, test_legacy_relationship, teardown);
	g_test_add("/company-roles/bill-from-customer-who-supplies", Fixture, NULL, setup, test_bill_from_customer_who_supplies, teardown);
	g_test_add("/company-roles/list-views", Fixture, NULL, setup, test_list_views, teardown);
	g_test_add("/company-roles/people-report", Fixture, NULL, setup, test_people_report, teardown);
	g_test_add("/company-roles/people-report-bound", Fixture, NULL, setup, test_people_report_bound, teardown);
	g_test_add_func("/company-roles/upgrade-backfill", test_upgrade_backfill);
	return g_test_run();
}
