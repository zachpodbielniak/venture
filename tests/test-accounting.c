/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

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
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	vendor = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org,
		"name", "Supplier", "kind", VENTURE_COMPANY_KIND_SUPPLIER, NULL);
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static JsonObject *
find_action(JsonNode *home, const gchar *kind)
{
	JsonArray *array;
	guint i;
	g_assert_true(JSON_NODE_HOLDS_ARRAY(home));
	array = json_node_get_array(home);
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);
		if (g_strcmp0(json_object_get_string_member(row, "kind"), kind) == 0)
			return row;
	}
	return NULL;
}

static void
test_empty_home_is_explainable(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) home = venture_accounting_home(f->context, f->org, &error);
	JsonObject *bills;
	g_assert_no_error(error);
	g_assert_nonnull(home);
	bills = find_action(home, "bills_to_pay");
	g_assert_nonnull(bills);
	g_assert_cmpint(json_object_get_int_member(bills, "count"), ==, 0);
	g_assert_nonnull(json_object_get_string_member(bills, "reason"));
	g_assert_nonnull(json_object_get_string_member(bills, "href"));
}

static void
test_guides_next_actions(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) bill = g_object_new(VENTURE_TYPE_VENDOR_BILL,
		"organization-id", f->org, "company-id", f->vendor, "number", "PAY-1",
		"currency", "USD", "status", "draft", NULL);
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(JsonNode) home = NULL;
	JsonObject *bills;
	g_assert_true(venture_entity_set_field_from_string(bill, "bill-date", "2026-01-01", &error));
	save(f, bill);
	line = g_object_new(VENTURE_TYPE_VENDOR_BILL_LINE, "organization-id", f->org,
		"bill-id", venture_entity_get_id(bill), "description", "Paper",
		"quantity", "1", NULL);
	g_assert_true(venture_entity_set_field_from_string(line, "unit-price", "40 USD", &error));
	save(f, line);
	event = g_object_new(VENTURE_TYPE_VENDOR_BILL_EVENT, "organization-id", f->org,
		"bill-id", venture_entity_get_id(bill), "vendor-id", f->vendor,
		"kind", "approve", "state", "approved", NULL);
	g_assert_true(venture_entity_set_field_from_string(event, "date", "2026-01-01", &error));
	save(f, event);
	home = venture_accounting_home(f->context, f->org, &error);
	g_assert_no_error(error);
	bills = find_action(home, "bills_to_pay");
	g_assert_nonnull(bills);
	g_assert_cmpint(json_object_get_int_member(bills, "count"), ==, 1);
	g_assert_nonnull(strstr(json_object_get_string_member(bills, "reason"), "approved"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/accounting/empty", Fixture, NULL, setup, test_empty_home_is_explainable, teardown);
	g_test_add("/accounting/bills", Fixture, NULL, setup, test_guides_next_actions, teardown);
	return g_test_run();
}
