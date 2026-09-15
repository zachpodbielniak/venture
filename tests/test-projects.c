/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

static void
test_records(void)
{
	const gchar *names[] = { "client_project", "project_rate", "project_time", "project_cost", "project_billing" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 customer;
	gint64 venture;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureVenture) venture = venture_venture_new();
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(customer, "name", "Client", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(customer), f->org);
	save(f, VENTURE_ENTITY(customer));
	f->customer = venture_entity_get_id(VENTURE_ENTITY(customer));
	g_object_set(venture, "name", "Studio", "slug", "studio", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), f->org);
	save(f, VENTURE_ENTITY(venture));
	f->venture = venture_entity_get_id(VENTURE_ENTITY(venture));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static gint64
count_type(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	return venture_database_count(f->db, query, NULL);
}

/* Approved time and a billable cost become one issued invoice, once. */
static void
test_bill_time_and_cost(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) project = g_object_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "client_project"), NULL);
	g_autoptr(VentureEntity) rate = g_object_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_rate"), NULL);
	g_autoptr(VentureEntity) time = g_object_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_time"), NULL);
	g_autoptr(VentureEntity) cost = g_object_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_cost"), NULL);
	g_autoptr(VentureExpense) expense = venture_expense_new();
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureMoney) hundred = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureMoney) rate_money = venture_money_new_for_currency(15000, "USD");
	g_autoptr(VentureMoney) cost_money = venture_money_new_for_currency(2500, "USD");
	g_autoptr(VentureMoney) budget = venture_money_new_for_currency(100000, "USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 12, 0, 0, 0);
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	gint64 invoice_id;
	(void)data;
	venture_entity_set_organization_id(project, f->org);
	g_object_set(project, "name", "Website", "venture-id", f->venture, "customer-id", f->customer,
		"currency", "USD", "billing-kind", "time", "budget", budget, NULL);
	g_assert_true(venture_project_service_save(venture_project_service_get(f->db), project, NULL, &error));
	venture_entity_set_organization_id(rate, f->org);
	g_object_set(rate, "project-id", venture_entity_get_id(project), "role", "engineer",
		"billing-rate", rate_money, "cost-rate", hundred, NULL);
	save(f, rate);
	venture_entity_set_organization_id(time, f->org);
	g_object_set(time, "project-id", venture_entity_get_id(project), "rate-id", venture_entity_get_id(rate),
		"minutes", (gint64)60, "occurred-at", date, NULL);
	g_assert_true(venture_project_service_approve_time(venture_project_service_get(f->db), time, NULL, &error));
	g_assert_no_error(error);
	g_object_set(expense, "description", "Stock photos", "organization-id", f->org, "amount", cost_money, "occurred-at", date, NULL);
	save(f, VENTURE_ENTITY(expense));
	venture_entity_set_organization_id(cost, f->org);
	g_object_set(cost, "project-id", venture_entity_get_id(project), "expense-id", venture_entity_get_id(VENTURE_ENTITY(expense)),
		"amount", cost_money, "billable", TRUE, "occurred-at", date, NULL);
	save(f, cost);
	invoice = venture_project_service_bill(venture_project_service_get(f->db), venture_entity_get_id(project), date, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	invoice_id = venture_entity_get_id(invoice);
	balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(f->db), invoice_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 17500);
	g_assert_cmpuint(count_type(f, venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_billing")), ==, 2);
	g_clear_object(&invoice);
	invoice = venture_project_service_bill(venture_project_service_get(f->db), venture_entity_get_id(project), date, NULL, &error);
	g_assert_null(invoice);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	report = venture_report_generate(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "project_margin"),
		f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	{
		g_autoptr(VentureEntity) extra = g_object_new(VENTURE_TYPE_PROJECT_TIME,
			"organization-id", f->org, "project-id", venture_entity_get_id(project),
			"rate-id", venture_entity_get_id(rate), "minutes", (gint64)60, "occurred-at", date, NULL);
		/* More approved work on the same day must not collide with the prior invoice number. */
		g_assert_true(venture_project_service_approve_time(venture_project_service_get(f->db), extra, NULL, &error));
		invoice = venture_project_service_bill(venture_project_service_get(f->db), venture_entity_get_id(project), date, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(invoice);
	}
}

static void
test_double_bill_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) billing = g_object_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_billing"), NULL);
	(void)data;
	venture_entity_set_organization_id(billing, f->org);
	g_object_set(billing, "project-id", (gint64)1, "source-type", "project_time", "source-id", (gint64)1, NULL);
	g_assert_false(venture_database_save(f->db, billing, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureProjectService"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/projects/records", test_records);
	g_test_add("/projects/bill-time-cost", Fixture, NULL, setup, test_bill_time_and_cost, teardown);
	g_test_add("/projects/generic-billing-refused", Fixture, NULL, setup, test_double_bill_refused, teardown);
	return g_test_run();
}
