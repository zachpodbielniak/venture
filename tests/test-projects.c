/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

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
	f->db = venture_test_accounting_database(&error);
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
	venture_test_accounting_database_cleanup(f->db);
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
	/* Approval freezes labour cost; later rate edits cannot rewrite profit. */
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(time), "actual-cost"));
	{
		g_autoptr(VentureMoney) actual = NULL;
		g_object_get(time, "actual-cost", &actual, NULL);
		g_assert_nonnull(actual);
		g_assert_cmpint(venture_money_get_amount(actual), ==, 10000);
	}
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

static VentureEntity *
new_project(Fixture *f, const gchar *name)
{
	VentureEntity *project = g_object_new(VENTURE_TYPE_CLIENT_PROJECT,
		"organization-id", f->org, "customer-id", f->customer,
		"name", name, "currency", "USD", NULL);
	save(f, project);
	return project;
}

static VentureEntity *
new_rate(Fixture *f, VentureEntity *project, gint64 billing, gint64 cost)
{
	g_autoptr(VentureMoney) bill_money = venture_money_new_for_currency(billing, "USD");
	g_autoptr(VentureMoney) cost_money = venture_money_new_for_currency(cost, "USD");
	VentureEntity *rate = g_object_new(VENTURE_TYPE_PROJECT_RATE,
		"organization-id", f->org, "project-id", venture_entity_get_id(project),
		"role", "staff", "billing-rate", bill_money, "cost-rate", cost_money, NULL);
	save(f, rate);
	return rate;
}

static VentureEntity *
new_time(Fixture *f, VentureEntity *project, VentureEntity *rate, gint64 minutes)
{
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 12, 0, 0, 0);
	return g_object_new(VENTURE_TYPE_PROJECT_TIME,
		"organization-id", f->org, "project-id", venture_entity_get_id(project),
		"rate-id", venture_entity_get_id(rate), "minutes", minutes, "occurred-at", date, NULL);
}

static JsonNode *
profitability(Fixture *f, VentureDateRange *period)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) report = venture_report_generate(
		venture_report_registry_lookup(venture_context_get_report_registry(f->context), "project_margin"),
		f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	return venture_report_result_to_json(report);
}

static JsonObject *
first_row(JsonNode *node)
{
	return json_array_get_object_element(json_object_get_array_member(json_node_get_object(node), "rows"), 0);
}

static void
assert_money(JsonObject *row, const gchar *field, gint64 amount)
{
	JsonObject *money = json_object_get_object_member(row, field);
	g_assert_cmpint(json_object_get_int_member(money, "amount"), ==, amount);
	g_assert_cmpstr(json_object_get_string_member(money, "currency"), ==, "USD");
}

/* Approval is a financial evidence boundary on every writer, including
 * the convenience save API and deletion of an altered in-memory record. */
static void
test_approval_guards(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) project = new_project(f, "Protected");
	g_autoptr(VentureEntity) other = new_project(f, "Other");
	g_autoptr(VentureEntity) rate = new_rate(f, project, 15000, 10000);
	g_autoptr(VentureEntity) time = new_time(f, project, rate, 60);
	g_autoptr(VentureMoney) foreign = venture_money_new_for_currency(10000, "EUR");
	g_autoptr(VentureMoney) actual = NULL;
	VentureProjectService *service = venture_project_service_get(f->db);
	(void)data;
	g_object_set(time, "approved", TRUE, NULL);
	g_assert_false(venture_project_service_save(service, time, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(time, "approved", FALSE, "project-id", venture_entity_get_id(other), NULL);
	g_assert_false(venture_project_service_approve_time(service, time, NULL, &error));
	g_assert_nonnull(strstr(error->message, "this project"));
	g_clear_error(&error);
	g_object_set(time, "project-id", venture_entity_get_id(project), NULL);
	g_object_set(rate, "cost-rate", foreign, NULL);
	g_assert_false(venture_database_save(f->db, rate, NULL, &error));
	g_clear_error(&error);
	g_clear_object(&rate);
	rate = venture_database_get(f->db, VENTURE_TYPE_PROJECT_RATE, (gint64)1, &error);
	g_assert_no_error(error);
	g_assert_true(venture_project_service_approve_time(service, time, NULL, &error));
	g_assert_no_error(error);
	g_object_get(time, "actual-cost", &actual, NULL);
	g_assert_cmpint(venture_money_get_amount(actual), ==, 10000);
	g_object_set(time, "minutes", (gint64)120, NULL);
	g_assert_false(venture_database_save(f->db, time, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_object_set(time, "approved", FALSE, NULL);
	g_assert_false(venture_database_delete(f->db, time, NULL, &error));
	g_clear_error(&error);
	g_assert_false(venture_database_restore(f->db, time, NULL, &error));
	g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, time, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(venture_database_purge(f->db, project, NULL, &error));
	g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, rate, NULL, &error));
	g_assert_nonnull(error);
}

/* Later rates and invoices cannot move cost or unbilled work out of the
 * period being examined. Profit is billed revenue minus incurred cost. */
static void
test_profitability_period(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) project = new_project(f, "Period evidence");
	g_autoptr(VentureEntity) rate = new_rate(f, project, 15000, 10000);
	g_autoptr(VentureEntity) time = new_time(f, project, rate, 60);
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureMoney) new_cost = venture_money_new_for_currency(20000, "USD");
	g_autoptr(VentureMoney) budget = venture_money_new_for_currency(100000, "USD");
	g_autoptr(GDateTime) february = g_date_time_new_utc(2026, 2, 1, 0, 0, 0);
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autoptr(VentureDateRange) january = venture_date_range_new_month(2026, 1, utc);
	g_autoptr(JsonNode) json = NULL;
	JsonObject *row;
	(void)data;
	g_object_set(project, "budget", budget, NULL);
	save(f, project);
	g_assert_true(venture_project_service_approve_time(venture_project_service_get(f->db), time, NULL, &error));
	g_object_set(rate, "cost-rate", new_cost, NULL);
	save(f, rate);
	invoice = venture_project_service_bill(venture_project_service_get(f->db), venture_entity_get_id(project), february, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	json = profitability(f, january);
	row = first_row(json);
	assert_money(row, "budget", 100000);
	assert_money(row, "billed", 0);
	assert_money(row, "unbilled", 15000);
	assert_money(row, "actual_cost", 10000);
	assert_money(row, "profit", -10000);
	g_assert_nonnull(strstr(json_object_get_string_member(row, "sources"), "project_time:"));
	g_clear_pointer(&json, json_node_unref);
	json = profitability(f, NULL);
	row = first_row(json);
	assert_money(row, "billed", 15000);
	assert_money(row, "unbilled", 0);
	assert_money(row, "actual_cost", 10000);
	assert_money(row, "profit", 5000);
	g_assert_nonnull(strstr(json_object_get_string_member(row, "sources"), "project_billing:"));
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PROJECT_BILLING);
		g_autoptr(GPtrArray) allocations = venture_database_find(f->db, query, &error);
		VentureEntity *allocation = g_ptr_array_index(allocations, 0);
		g_assert_false(venture_database_delete(f->db, allocation, NULL, &error));
		g_clear_error(&error);
		g_assert_false(venture_database_purge(f->db, allocation, NULL, &error));
		g_clear_error(&error);
		g_assert_false(venture_project_service_save(venture_project_service_get(f->db), allocation, NULL, &error));
		g_assert_nonnull(error);
	}
}

/* A migration must not fabricate yesterday's labour cost from today's
 * rate. Unknown actual cost makes total profit unknown, not overstated. */
static void
test_historical_cost_unknown(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) project = new_project(f, "Legacy");
	g_autoptr(VentureEntity) rate = new_rate(f, project, 15000, 10000);
	g_autoptr(VentureEntity) time = new_time(f, project, rate, 60);
	g_autoptr(JsonNode) json = NULL;
	JsonObject *row;
	(void)data;
	g_assert_true(venture_project_service_approve_time(venture_project_service_get(f->db), time, NULL, &error));
	g_assert_true(venture_database_execute(f->db, "ALTER TABLE project_times DROP COLUMN actual_cost_amount", NULL, &error));
	g_assert_true(venture_database_execute(f->db, "ALTER TABLE project_times DROP COLUMN actual_cost_currency", NULL, &error));
	g_assert_true(venture_database_execute(f->db, "ALTER TABLE project_times DROP COLUMN actual_cost_exponent", NULL, &error));
	/* Model the entire old ledger prefix. Leaving later migrations recorded
	 * creates a forbidden history gap once another feature lands after 460. */
	g_assert_true(venture_database_execute(f->db, "DELETE FROM schema_migrations WHERE version >= 460", NULL, &error));
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	json = profitability(f, NULL);
	row = first_row(json);
	g_assert_cmpint(json_object_get_int_member(row, "unknown_cost"), ==, 1);
	g_assert_true(json_object_get_null_member(row, "actual_cost"));
	g_assert_true(json_object_get_null_member(row, "profit"));
	assert_money(row, "known_cost", 0);
	assert_money(row, "unbilled", 15000);
	/* Historical projects could omit currency. Filling it must agree with
	 * retained amounts, and must not relabel USD labour as EUR. */
	g_assert_true(venture_database_execute(f->db, "UPDATE client_projects SET currency = NULL", NULL, &error));
	g_clear_object(&project);
	project = venture_database_get(f->db, VENTURE_TYPE_CLIENT_PROJECT, (gint64)1, &error);
	g_assert_no_error(error);
	g_object_set(project, "currency", "EUR", NULL);
	g_assert_false(venture_database_save(f->db, project, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(project, "currency", "USD", NULL);
	save(f, project);
}

/* Fractional hours use half-to-even exact integer money; overflow cannot
 * persist either the approval flag or a partial cost calculation. */
static void
test_approval_rounding(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) project = new_project(f, "Rounding");
	g_autoptr(VentureEntity) rate = new_rate(f, project, 90, 30);
	g_autoptr(VentureEntity) time = new_time(f, project, rate, 1);
	g_autoptr(VentureMoney) bill = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) huge = venture_money_new_for_currency(G_MAXINT64, "USD");
	gboolean approved = TRUE;
	(void)data;
	g_assert_true(venture_project_service_approve_time(venture_project_service_get(f->db), time, NULL, &error));
	g_object_get(time, "amount", &bill, "actual-cost", &cost, NULL);
	g_assert_cmpint(venture_money_get_amount(bill), ==, 2);
	g_assert_cmpint(venture_money_get_amount(cost), ==, 0);
	g_object_set(rate, "cost-rate", huge, NULL);
	save(f, rate);
	g_clear_object(&time);
	time = new_time(f, project, rate, 120);
	g_assert_false(venture_project_service_approve_time(venture_project_service_get(f->db), time, NULL, &error));
	g_assert_nonnull(error);
	g_assert_false(venture_entity_is_persisted(time));
	g_object_get(time, "approved", &approved, NULL);
	g_assert_false(approved);
}

/* Nonbillable costs still consume the project's budget; reference checks
 * cannot let another organization's customer become its owner. */
static void
test_cost_and_organization(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) project = new_project(f, "Internal costs");
	g_autoptr(VentureOrganization) other_org = venture_organization_new();
	g_autoptr(VentureCompany) other_customer = venture_company_new();
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(1, "USD");
	g_autoptr(JsonNode) json = NULL;
	guint i;
	(void)data;
	g_object_set(other_org, "name", "Other organization", "slug", "other-project-org", NULL);
	save(f, VENTURE_ENTITY(other_org));
	g_object_set(other_customer, "name", "Other customer", "organization-id",
		venture_entity_get_id(VENTURE_ENTITY(other_org)), NULL);
	save(f, VENTURE_ENTITY(other_customer));
	g_object_set(project, "customer-id", venture_entity_get_id(VENTURE_ENTITY(other_customer)), NULL);
	g_assert_false(venture_database_save(f->db, project, NULL, &error));
	g_assert_nonnull(strstr(error->message, "organization"));
	g_clear_error(&error);
	g_object_set(project, "customer-id", f->customer, NULL);
	/* Report source reads must not silently stop at the generic page limit. */
	for (i = 0; i < 105; i++)
	{
		g_autoptr(VentureEntity) cost = g_object_new(VENTURE_TYPE_PROJECT_COST,
			"organization-id", f->org, "project-id", venture_entity_get_id(project),
			"amount", amount, "billable", FALSE, NULL);
		save(f, cost);
	}
	json = profitability(f, NULL);
	assert_money(first_row(json), "actual_cost", 105);
	assert_money(first_row(json), "profit", -105);
	assert_money(first_row(json), "unbilled", 0);
}

/* Generic actions are the operator surface. They retain role checks,
 * organization authority and the confirmation queue's stale-write refusal. */
static void
test_project_actions(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) project = new_project(f, "Action project");
	g_autoptr(VentureEntity) rate = new_rate(f, project, 15000, 10000);
	g_autoptr(VentureEntity) time = new_time(f, project, rate, 60);
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER,
		"username", "project-finance", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autofree gchar *confirmation_id = NULL;
	VentureActionRegistry *registry = venture_database_get_action_registry(f->db);
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	VentureAction *approve = venture_action_registry_lookup(registry, "project_time", "approve");
	VentureConfirmation *confirmation;
	VentureAuthPrincipal principal;
	VentureActor actor;
	(void)data;
	save(f, time);
	save(f, VENTURE_ENTITY(user));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(user),
		"organization-id", f->org, "role", VENTURE_ORGANIZATION_ROLE_SALES, "active", TRUE, NULL);
	save(f, member);
	principal.authenticated = TRUE;
	principal.user_id = venture_entity_get_id(user);
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = NULL;
	venture_auth_to_actor(&principal, &actor);
	g_assert_nonnull(approve);
	result = venture_action_registry_perform(registry, "project_time", venture_entity_get_id(time), "approve",
		params, NULL, VENTURE_USER_ROLE_VIEWER, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	result = venture_action_registry_perform(registry, "project_time", venture_entity_get_id(time), "approve",
		params, &actor, principal.role, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_object(&scope);
	g_object_set(member, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, NULL);
	save(f, member);
	confirmation = venture_confirmation_store_stage_action(store, approve, time, params, NULL,
		VENTURE_USER_ROLE_OWNER, "test", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	confirmation_id = g_strdup(venture_confirmation_get_id(confirmation));
	g_object_set(time, "minutes", (gint64)30, NULL);
	save(f, time);
	g_assert_false(venture_confirmation_store_approve_as(store, confirmation_id, "owner", VENTURE_USER_ROLE_OWNER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	result = venture_action_registry_perform(registry, "project_time", venture_entity_get_id(time), "approve",
		params, &actor, principal.role, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_clear_object(&result);
	{
		JsonNode *date = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(date, "2026-01-12");
		g_hash_table_insert(params, g_strdup("date"), date);
	}
	result = venture_action_registry_perform(registry, "client_project", venture_entity_get_id(project), "bill",
		params, &actor, principal.role, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(VENTURE_IS_INVOICE(result));
	g_assert_cmpint(venture_entity_get_organization_id(result), ==, f->org);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/projects/records", test_records);
	g_test_add("/projects/bill-time-cost", Fixture, NULL, setup, test_bill_time_and_cost, teardown);
	g_test_add("/projects/generic-billing-refused", Fixture, NULL, setup, test_double_bill_refused, teardown);
	g_test_add("/projects/approval-guards", Fixture, NULL, setup, test_approval_guards, teardown);
	g_test_add("/projects/profitability-period", Fixture, NULL, setup, test_profitability_period, teardown);
	g_test_add("/projects/historical-cost-unknown", Fixture, NULL, setup, test_historical_cost_unknown, teardown);
	g_test_add("/projects/approval-rounding", Fixture, NULL, setup, test_approval_rounding, teardown);
	g_test_add("/projects/cost-and-organization", Fixture, NULL, setup, test_cost_and_organization, teardown);
	g_test_add("/projects/actions", Fixture, NULL, setup, test_project_actions, teardown);
	return g_test_run();
}
