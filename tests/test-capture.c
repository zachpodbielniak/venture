/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 vendor;
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
	g_autoptr(VentureEntity) vendor = NULL;
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	vendor = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org,
		"name", "Office Supply", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "capture_item"), !=, G_TYPE_INVALID);
}

static gint64
count_type(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(
		venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static void
test_receipt_becomes_expense(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1250, "USD");
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *status = NULL;
	g_autofree gchar *type = NULL;
	gint64 result_id = 0;
	when = g_date_time_new_from_iso8601("2026-01-15T00:00:00Z", NULL);
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"receipt", "Toner", "upload", 0, "Office Supply", amount, when,
		"front desk", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(item);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 1);
	json_object_set_string_member(options, "category", "office");
	result = venture_capture_service_convert(venture_capture_service_get(f->db),
		item, "expense", options, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(VENTURE_IS_EXPENSE(result));
	g_object_get(item, "status", &status, "result-type", &type, "result-id", &result_id, NULL);
	g_assert_cmpstr(status, ==, "converted");
	g_assert_cmpstr(type, ==, "expense");
	g_assert_cmpint(result_id, ==, venture_entity_get_id(result));
	g_assert_cmpint(count_type(f, "expense"), >=, 1);
}

static void
test_invoice_becomes_bill(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(4000, "USD");
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autofree gchar *bill_status = NULL;
	when = g_date_time_new_from_iso8601("2026-01-10T00:00:00Z", NULL);
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"supplier_invoice", "January paper", "email", 0, "Office Supply",
		amount, when, NULL, NULL, &error);
	g_assert_no_error(error);
	json_object_set_int_member(options, "company_id", f->vendor);
	result = venture_capture_service_convert(venture_capture_service_get(f->db),
		item, "vendor_bill", options, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(VENTURE_IS_VENDOR_BILL(result));
	g_object_get(result, "status", &bill_status, NULL);
	g_assert_cmpstr(bill_status, ==, "draft");
	g_assert_cmpint(count_type(f, "vendor_bill_line"), ==, 1);
}

static void
test_reject_and_generic_convert_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(100, "USD");
	g_autoptr(VentureEntity) item = NULL;
	g_autofree gchar *status = NULL;
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"receipt", "Coffee", "upload", 0, "Cafe", amount, NULL, NULL, NULL, &error);
	g_assert_true(venture_capture_service_reject(venture_capture_service_get(f->db),
		item, "personal", NULL, &error));
	g_object_get(item, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "rejected");
	g_object_set(item, "status", "converted", NULL);
	g_assert_false(venture_database_save(f->db, item, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureCaptureService"));
}

static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(100, "USD");
	g_autoptr(VentureEntity) item = NULL;
	venture_config_set_module_enabled(f->config, "capture", FALSE);
	item = venture_capture_service_ingest(venture_capture_service_get(f->db),
		"receipt", "X", "upload", 0, NULL, amount, NULL, NULL, NULL, &error);
	g_assert_null(item);
	g_assert_nonnull(error);
	venture_config_set_module_enabled(f->config, "capture", TRUE);
}

/* A receipt follows the selected books through capture and conversion. */
static void
test_scoped_capture(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1200, "USD");
	g_autoptr(GDateTime) when = venture_time_from_string("2026-01-15", NULL);
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) expense = NULL;
	gint64 org;
	(void)unused;
	g_object_set(other, "name", "Other capture", "legal-name", "Other capture", "default-currency", "USD", NULL);
	save(f, VENTURE_ENTITY(other));
	org = venture_entity_get_id(VENTURE_ENTITY(other));
	item = venture_capture_service_ingest_for_organization(venture_capture_service_get(f->db),
		org, "receipt", "Supplies", "upload", 0, NULL, amount, when, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(item);
	g_assert_cmpint(venture_entity_get_organization_id(item), ==, org);
	expense = venture_capture_service_convert(venture_capture_service_get(f->db), item, "expense", NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(expense);
	g_assert_cmpint(venture_entity_get_organization_id(expense), ==, org);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/capture/records", test_records);
	g_test_add("/capture/expense", Fixture, NULL, setup, test_receipt_becomes_expense, teardown);
	g_test_add("/capture/bill", Fixture, NULL, setup, test_invoice_becomes_bill, teardown);
	g_test_add("/capture/reject", Fixture, NULL, setup, test_reject_and_generic_convert_refused, teardown);
	g_test_add("/capture/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/capture/scoped", Fixture, NULL, setup, test_scoped_capture, teardown);
	return g_test_run();
}
