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
} Fixture;

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	g_object_set(f->config, "payroll-enabled", TRUE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
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

static JsonNode *
sample_run(const gchar *key)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "run_key");
	json_builder_add_string_value(b, key);
	json_builder_set_member_name(b, "period_start");
	json_builder_add_string_value(b, "2026-01-01T00:00:00Z");
	json_builder_set_member_name(b, "period_end");
	json_builder_add_string_value(b, "2026-02-01T00:00:00Z");
	json_builder_set_member_name(b, "currency");
	json_builder_add_string_value(b, "USD");
	json_builder_set_member_name(b, "lines");
	json_builder_begin_array(b);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "employee");
	json_builder_add_string_value(b, "Ada");
	json_builder_set_member_name(b, "gross");
	json_builder_add_string_value(b, "5000.00 USD");
	json_builder_set_member_name(b, "employer_cost");
	json_builder_add_string_value(b, "400.00 USD");
	json_builder_set_member_name(b, "deductions");
	json_builder_add_string_value(b, "1000.00 USD");
	json_builder_set_member_name(b, "net");
	json_builder_add_string_value(b, "4000.00 USD");
	json_builder_set_member_name(b, "liabilities");
	json_builder_add_string_value(b, "1400.00 USD");
	json_builder_end_object(b);
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"payroll_run"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"payroll_line"), !=, G_TYPE_INVALID);
}

static void
test_default_off(void)
{
	g_autoptr(VentureModuleRegistry) registry = venture_module_registry_new();
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(GError) error = NULL;
	venture_module_registry_register_builtins(registry);
	g_assert_true(venture_module_registry_configure(registry, config, &error));
	g_assert_false(venture_module_registry_is_enabled(registry, "payroll"));
}

static void
test_import_and_duplicate(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) payload = sample_run("2026-01");
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autofree gchar *status = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	(void)unused;
	run = venture_payroll_service_import_json(venture_payroll_service_get(f->db),
		f->org, json_node_get_object(payload), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(run);
	g_object_get(run, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "imported");
	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		"payroll_run", venture_entity_get_id(run), f->org, &error);
	g_assert_cmpuint(journals->len, >=, 1);
	again = venture_payroll_service_import_json(venture_payroll_service_get(f->db),
		f->org, json_node_get_object(payload), NULL, &error);
	g_assert_null(again);
	g_assert_nonnull(error);
}

static void
test_csv_import(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) run = NULL;
	const gchar *csv =
		"employee,gross,employer_cost,deductions,net,liabilities\n"
		"Bea,2000.00 USD,160.00 USD,400.00 USD,1600.00 USD,560.00 USD\n";
	(void)unused;
	run = venture_payroll_service_import_csv(venture_payroll_service_get(f->db),
		f->org, "2026-02", "2026-02-01T00:00:00Z", "2026-03-01T00:00:00Z", "USD",
		csv, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(run);
}

static void
test_disburse_and_reverse(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) payload = sample_run("2026-03");
	g_autoptr(VentureEntity) run = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *employee = NULL;
	g_autoptr(VentureQuery) lines = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	guint before;
	(void)unused;
	run = venture_payroll_service_import_json(venture_payroll_service_get(f->db),
		f->org, json_node_get_object(payload), NULL, &error);
	g_assert_true(venture_payroll_service_disburse(venture_payroll_service_get(f->db),
		run, "all", NULL, &error));
	g_assert_no_error(error);
	g_object_get(run, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "disbursed");
	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		"payroll_run", venture_entity_get_id(run), f->org, &error);
	before = journals->len;
	g_assert_true(venture_payroll_service_reverse(venture_payroll_service_get(f->db),
		run, NULL, &error));
	g_clear_pointer(&status, g_free);
	g_object_get(run, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "reversed");
	g_clear_pointer(&journals, g_ptr_array_unref);
	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		"payroll_run", venture_entity_get_id(run), f->org, &error);
	g_assert_cmpuint(journals->len, >, before);
	lines = venture_query_new(VENTURE_TYPE_PAYROLL_LINE);
	venture_query_add_filter_int(lines, "run-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(run), NULL);
	rows = venture_database_find(f->db, lines, &error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_object_get(g_ptr_array_index(rows, 0), "employee", &employee, NULL);
	g_assert_cmpstr(employee, ==, "Ada");
}

static void
test_reconciliation_report(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) payload = sample_run("2026-04");
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	(void)unused;
	run = venture_payroll_service_import_json(venture_payroll_service_get(f->db),
		f->org, json_node_get_object(payload), NULL, &error);
	g_assert_nonnull(run);
	result = venture_report_generate(venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "payroll_reconciliation"),
		f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

static void
test_access_hides(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) payload = sample_run("2026-05");
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "sales", "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	VentureAuthPrincipal actor = { 0 };
	(void)unused;
	run = venture_payroll_service_import_json(venture_payroll_service_get(f->db),
		f->org, json_node_get_object(payload), NULL, &error);
	g_assert_nonnull(run);
	g_assert_true(venture_database_save(f->db, user, NULL, &error));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id",
		venture_entity_get_id(user), "organization-id", f->org, "active", TRUE,
		"role", VENTURE_ORGANIZATION_ROLE_SALES, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, &error));
	actor.authenticated = TRUE;
	actor.user_id = venture_entity_get_id(user);
	actor.role = VENTURE_USER_ROLE_EDITOR;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &actor);
	query = venture_query_new(VENTURE_TYPE_PAYROLL_RUN);
	venture_query_set_organization(query, f->org);
	rows = venture_access_policy_find(venture_database_get_access_policy(f->db), query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 0);
	g_assert_false(venture_access_policy_can(venture_database_get_access_policy(f->db),
		&actor, "read", run, &error));
}

static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) payload = sample_run("2026-06");
	g_autoptr(VentureEntity) run = NULL;
	(void)unused;
	g_object_set(f->config, "payroll-enabled", FALSE, NULL);
	run = venture_payroll_service_import_json(venture_payroll_service_get(f->db),
		f->org, json_node_get_object(payload), NULL, &error);
	g_assert_null(run);
	g_assert_nonnull(error);
	g_object_set(f->config, "payroll-enabled", TRUE, NULL);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/payroll/records", test_records);
	g_test_add_func("/payroll/default-off", test_default_off);
	g_test_add("/payroll/import-duplicate", Fixture, NULL, setup, test_import_and_duplicate, teardown);
	g_test_add("/payroll/csv", Fixture, NULL, setup, test_csv_import, teardown);
	g_test_add("/payroll/disburse-reverse", Fixture, NULL, setup, test_disburse_and_reverse, teardown);
	g_test_add("/payroll/report", Fixture, NULL, setup, test_reconciliation_report, teardown);
	g_test_add("/payroll/access", Fixture, NULL, setup, test_access_hides, teardown);
	g_test_add("/payroll/module-off", Fixture, NULL, setup, test_module_off, teardown);
	return g_test_run();
}
