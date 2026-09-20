/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

/* The generic API, forms and reports must discover these declarations. */
static void test_sales_records(void)
{
	static const gchar *const names[] = { "sales_territory", "sales_quota", "sales_credit", "sales_assignment" };
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}
typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
} SalesFixture;
static void sales_setup(SalesFixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->db = venture_test_accounting_database(&error); g_assert_no_error(error);
	{ gboolean migrated = venture_database_migrate(f->db, venture_entity_registry_get_default(), &error); g_assert_no_error(error); g_assert_true(migrated); }
	f->config = venture_config_new(); f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}
static void sales_teardown(SalesFixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context); g_clear_object(&f->config);
	venture_test_accounting_database_cleanup(f->db); g_clear_object(&f->db);
}
static VentureEntity *sales_record(SalesFixture *f, const gchar *type, const gchar *name)
{
	GType kind = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	g_assert_cmpuint(kind, !=, G_TYPE_INVALID);
	return g_object_new(kind, "organization-id", f->org, "name", name, NULL);
}
static void sales_save(SalesFixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	gboolean result = venture_database_save(f->db, entity, NULL, &error);
	g_assert_no_error(error); g_assert_true(result);
}
static gint64 sales_int(VentureEntity *entity, const gchar *field)
{
	gint64 value = 0;
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(entity), field);
	if (G_TYPE_IS_ENUM(G_PARAM_SPEC_VALUE_TYPE(spec))) { gint enumeration = 0; g_object_get(entity, field, &enumeration, NULL); return enumeration; }
	g_object_get(entity, field, &value, NULL); return value;
}
static VentureEntity *sales_user(SalesFixture *f, const gchar *name, gint64 team)
{
	VentureEntity *user = g_object_new(VENTURE_TYPE_USER, "username", name, "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL, teammate = NULL;
	sales_save(f, user);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", venture_entity_get_id(user), "role", VENTURE_ORGANIZATION_ROLE_SALES, "active", TRUE, NULL);
	teammate = g_object_new(VENTURE_TYPE_TEAM_MEMBERSHIP, "organization-id", f->org,
		"user-id", venture_entity_get_id(user), "team-id", team, "active", TRUE, NULL);
	sales_save(f, member); sales_save(f, teammate); return user;
}
static GPtrArray *sales_rows(SalesFixture *f, const gchar *name)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	GPtrArray *result;
	venture_query_set_organization(query, f->org); venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	result = venture_database_find(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static VentureEntity *sales_territory(SalesFixture *f, const gchar *name, gint64 team)
{
	VentureEntity *territory = sales_record(f, "sales_territory", name);
	g_object_set(territory, "team-id", team, "active", TRUE, NULL); sales_save(f, territory); return territory;
}
static VentureEntity *sales_rule(SalesFixture *f, const gchar *name, gint64 territory, gint64 team, gint64 position)
{
	VentureEntity *rule = sales_record(f, "lead_routing_rule", name);
	g_object_set(rule, "territory-id", territory, "team-id", team, "position", position,
		"conditions", "source=website", "action", VENTURE_LEAD_ROUTING_ROUND_ROBIN, "active", TRUE, NULL);
	sales_save(f, rule); return rule;
}
static void test_sales_routing(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) team = sales_record(f, "team", "Sales");
	g_autoptr(VentureEntity) first = NULL, second = NULL, broad = NULL, narrow = NULL, early = NULL, later = NULL;
	g_autoptr(VentureEntity) lead = sales_record(f, "lead", "Website lead"), next = sales_record(f, "lead", "Next lead");
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	sales_save(f, team); first = sales_user(f, "rep-first", venture_entity_get_id(team)); second = sales_user(f, "rep-second", venture_entity_get_id(team));
	broad = sales_territory(f, "Broad", venture_entity_get_id(team)); narrow = sales_territory(f, "Priority", venture_entity_get_id(team));
	later = sales_rule(f, "Later overlapping rule", venture_entity_get_id(broad), venture_entity_get_id(team), 20);
	early = sales_rule(f, "First matching rule", venture_entity_get_id(narrow), venture_entity_get_id(team), 10);
	g_object_set(lead, "source", "website", NULL); g_object_set(next, "source", "website", NULL);
	sales_save(f, lead); sales_save(f, next);
	g_assert_cmpint(sales_int(lead, "territory-id"), ==, venture_entity_get_id(narrow));
	g_assert_cmpint(sales_int(lead, "owner-user-id"), ==, venture_entity_get_id(first));
	g_assert_cmpint(sales_int(next, "owner-user-id"), ==, venture_entity_get_id(second));
	g_object_set(narrow, "active", FALSE, NULL); sales_save(f, narrow);
	g_object_set(first, "active", FALSE, NULL); sales_save(f, first);
	g_assert_true(venture_lead_service_reroute(venture_database_get_lead_service(f->db), lead, NULL, &error)); g_assert_no_error(error);
	g_assert_cmpint(sales_int(lead, "territory-id"), ==, venture_entity_get_id(broad));
	g_assert_cmpint(sales_int(lead, "owner-user-id"), ==, venture_entity_get_id(second));
	history = sales_rows(f, "sales_assignment"); g_assert_cmpuint(history->len, ==, 3);
	g_assert_cmpint(sales_int(g_ptr_array_index(history, 2), "previous-territory-id"), ==, venture_entity_get_id(narrow));
	g_assert_cmpint(sales_int(g_ptr_array_index(history, 2), "territory-id"), ==, venture_entity_get_id(broad));
	{
		g_autoptr(VentureEntity) converted = NULL, deal = NULL;
		g_object_set(lead, "status", VENTURE_LEAD_QUALIFIED, NULL); sales_save(f, lead);
		converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), lead, NULL, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(converted);
		deal = venture_database_get(f->db, VENTURE_TYPE_DEAL, sales_int(converted, "converted-deal-id"), &error);
		g_assert_no_error(error); g_assert_nonnull(deal);
		g_assert_cmpint(sales_int(deal, "owner-user-id"), ==, venture_entity_get_id(second));
		g_assert_cmpint(sales_int(deal, "team-id"), ==, venture_entity_get_id(team));
		g_assert_cmpint(sales_int(deal, "territory-id"), ==, venture_entity_get_id(broad));
	}
	g_test_message("Overlapping territories used rule order and persisted rotation; rerouting skipped inactive territory/rep and retained assignment history");
}
static void sales_move(SalesFixture *f, VentureEntity **deal, gint kind)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(VentureEntity) stage = NULL;
	g_autoptr(VentureDeal) moved = NULL;
	g_autoptr(GError) error = NULL;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_int(query, "pipeline-id", VENTURE_FILTER_OP_EQ, sales_int(*deal, "pipeline-id"), NULL);
	venture_query_add_filter_int(query, "kind", VENTURE_FILTER_OP_EQ, kind, NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	stage = venture_database_find_one(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(stage);
	moved = venture_deal_service_move_stage(venture_database_get_deal_service(f->db), VENTURE_DEAL(*deal), venture_entity_get_id(stage), "Fixture transition", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(moved); g_set_object(deal, VENTURE_ENTITY(moved));
}
static void test_sales_credit(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) team = sales_record(f, "team", "Sales"), first = NULL, second = NULL, territory = NULL;
	g_autoptr(VentureEntity) deal = sales_record(f, "deal", "Booked deal");
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(12345, "USD"), amount = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	sales_save(f, team); first = sales_user(f, "rep-first", venture_entity_get_id(team)); second = sales_user(f, "rep-second", venture_entity_get_id(team));
	territory = sales_territory(f, "Territory", venture_entity_get_id(team));
	g_object_set(deal, "owner", "rep-first", "territory-id", venture_entity_get_id(territory), "value", value, NULL); sales_save(f, deal);
	sales_move(f, &deal, 1); credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 1);
	g_assert_cmpint(sales_int(g_ptr_array_index(credits, 0), "owner-user-id"), ==, venture_entity_get_id(first));
	g_object_get(g_ptr_array_index(credits, 0), "value", &amount, NULL); g_assert_cmpint(venture_money_get_amount(amount), ==, 12345); g_clear_pointer(&amount, venture_money_free);
	g_object_set(deal, "owner", "rep-second", NULL); sales_save(f, deal); g_clear_pointer(&credits, g_ptr_array_unref);
	credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 1);
	g_assert_cmpint(sales_int(g_ptr_array_index(credits, 0), "owner-user-id"), ==, venture_entity_get_id(first));
	{
		g_autoptr(VentureMoney) revised = venture_money_new_for_currency(54321, "EUR");
		g_object_set(deal, "value", revised, NULL);
		g_assert_false(venture_database_save(f->db, deal, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		g_object_set(deal, "value", value, NULL);
	}
	g_assert_false(venture_database_delete(f->db, deal, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	/* A caller cannot disguise a stored winner as open to delete it. */
	g_object_set(deal, "stage", VENTURE_DEAL_STAGE_LEAD, NULL);
	g_assert_false(venture_database_delete(f->db, deal, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(deal, "stage", VENTURE_DEAL_STAGE_WON, NULL);
	sales_move(f, &deal, 0);
	g_assert_false(venture_database_purge(f->db, deal, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	sales_move(f, &deal, 1); g_clear_pointer(&credits, g_ptr_array_unref);
	credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 3);
	g_object_get(g_ptr_array_index(credits, 1), "value", &amount, NULL); g_assert_cmpint(venture_money_get_amount(amount), ==, -12345);
	g_assert_cmpint(sales_int(g_ptr_array_index(credits, 1), "owner-user-id"), ==, venture_entity_get_id(first));
	g_assert_cmpint(sales_int(g_ptr_array_index(credits, 2), "owner-user-id"), ==, venture_entity_get_id(second));
	g_object_set(g_ptr_array_index(credits, 0), "owner-user-id", venture_entity_get_id(second), NULL);
	g_assert_false(venture_database_save(f->db, g_ptr_array_index(credits, 0), NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_test_message("Owner changes retained original booked credit; cancellation reversed that rep and a later win credited the new rep exactly once");
}
static VentureEntity *sales_quota(SalesFixture *f, const gchar *name, gint64 user, gint64 team,
	const gchar *currency, gint64 amount, GDateTime *start, GDateTime *end)
{
	VentureEntity *quota = sales_record(f, "sales_quota", name);
	g_autoptr(VentureMoney) target = venture_money_new_for_currency(amount, currency);
	g_object_set(quota, "owner-user-id", user, "team-id", team, "metric", "booked_revenue", "target", target, "starts-at", start, "ends-at", end, NULL);
	return quota;
}
static JsonNode *sales_report(SalesFixture *f, GDateTime *start, GDateTime *end)
{
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "sales_attainment");
	g_autoptr(VentureDateRange) period = venture_date_range_new(start, end);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, period, NULL, &error); g_assert_no_error(error); g_assert_nonnull(result);
	return venture_report_result_to_json(result);
}
static JsonObject *sales_quota_row(JsonNode *report, gint64 id)
{
	JsonArray *rows = json_object_get_array_member(json_node_get_object(report), "rows");
	guint i;
	for (i = 0; i < json_array_get_length(rows); i++) {
		JsonObject *row = json_array_get_object_element(rows, i);
		if (g_ascii_strtoll(json_object_get_string_member(row, "quota_id"), NULL, 10) == id) return row;
	}
	return NULL;
}
static gint64 sales_report_amount(JsonObject *row, const gchar *field)
{ return json_object_get_int_member(json_object_get_object_member(row, field), "amount"); }
static void test_sales_quotas(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) team = sales_record(f, "team", "Quota team"), user = NULL, quota = NULL, duplicate = NULL, quarterly = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), start = NULL, end = NULL, long_end = NULL, noon = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) zero = venture_money_new_for_currency(0, "USD");
	(void)data;
	start = g_date_time_new_utc(g_date_time_get_year(now), g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0);
	end = g_date_time_add_days(start, 1); long_end = g_date_time_add_days(start, 90); noon = g_date_time_add_hours(start, 12);
	sales_save(f, team); user = sales_user(f, "quota-rep", venture_entity_get_id(team));
	quota = sales_quota(f, "Daily quota", venture_entity_get_id(user), 0, "USD", 20000, start, end); sales_save(f, quota);
	duplicate = sales_quota(f, "Duplicate", venture_entity_get_id(user), 0, "USD", 30000, start, end);
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
	quarterly = sales_quota(f, "Independent larger window", venture_entity_get_id(user), 0, "USD", 90000, start, long_end); sales_save(f, quarterly);
	g_object_set(duplicate, "team-id", venture_entity_get_id(team), NULL);
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(duplicate, "team-id", (gint64)0, "starts-at", noon, NULL);
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(duplicate, "starts-at", start, "target", zero, NULL);
	g_assert_false(venture_database_save(f->db, duplicate, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_test_message("Exact duplicate quota periods were refused durably; overlapping independent windows were accepted; ambiguous recipient, non-midnight period and zero target failed");
}
static void test_sales_attainment(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) team = sales_record(f, "team", "Sales"), first = NULL, second = NULL, territory = NULL;
	g_autoptr(VentureEntity) quota = NULL, team_quota = NULL, deal = sales_record(f, "deal", "Booked USD"), open = sales_record(f, "deal", "Open USD"), euro = sales_record(f, "deal", "Unplanned EUR");
	g_autoptr(VentureMoney) booked = venture_money_new_for_currency(12345, "USD"), forecast = venture_money_new_for_currency(8000, "USD"), eur = venture_money_new_for_currency(2000, "EUR");
	g_autoptr(GDateTime) now = venture_time_now(), start = NULL, end = NULL;
	g_autoptr(JsonNode) report = NULL;
	JsonObject *row;
	JsonArray *rows;
	guint i;
	gboolean saw_eur = FALSE;
	(void)data;
	start = g_date_time_new_utc(g_date_time_get_year(now), g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0); end = g_date_time_add_days(start, 1);
	sales_save(f, team); first = sales_user(f, "rep-first", venture_entity_get_id(team)); second = sales_user(f, "rep-second", venture_entity_get_id(team));
	territory = sales_territory(f, "Sales territory", venture_entity_get_id(team));
	quota = sales_quota(f, "First representative", venture_entity_get_id(first), 0, "USD", 20000, start, end); sales_save(f, quota);
	team_quota = sales_quota(f, "Team target", 0, venture_entity_get_id(team), "USD", 30000, start, end); sales_save(f, team_quota);
	g_object_set(deal, "owner", "rep-first", "territory-id", venture_entity_get_id(territory), "value", booked, NULL); sales_save(f, deal); sales_move(f, &deal, 1);
	g_object_set(deal, "owner", "rep-second", NULL); sales_save(f, deal);
	g_object_set(open, "owner", "rep-first", "territory-id", venture_entity_get_id(territory), "value", forecast, "probability", (gint64)50, NULL); sales_save(f, open);
	g_object_set(euro, "owner", "rep-second", "territory-id", venture_entity_get_id(territory), "value", eur, NULL); sales_save(f, euro); sales_move(f, &euro, 1);
	g_object_set(first, "active", FALSE, NULL); sales_save(f, first);
	report = sales_report(f, start, end); row = sales_quota_row(report, venture_entity_get_id(quota)); g_assert_nonnull(row);
	g_assert_cmpint(sales_report_amount(row, "actual"), ==, 12345); g_assert_cmpint(sales_report_amount(row, "remaining"), ==, 7655);
	g_assert_cmpint(sales_report_amount(row, "open_weighted"), ==, 4000);
	g_assert_cmpfloat_with_epsilon(json_object_get_double_member(row, "attainment"), 0.61725, 0.000001);
	row = sales_quota_row(report, venture_entity_get_id(team_quota)); g_assert_nonnull(row); g_assert_cmpint(sales_report_amount(row, "actual"), ==, 12345);
	rows = json_object_get_array_member(json_node_get_object(report), "rows");
	for (i = 0; i < json_array_get_length(rows); i++) {
		row = json_array_get_object_element(rows, i);
		if (!g_strcmp0(json_object_get_string_member(row, "currency"), "EUR")) {
			g_assert_cmpstr(json_object_get_string_member(row, "status"), ==, "No quota");
			g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(row, "target")));
			g_assert_cmpint(sales_report_amount(row, "actual"), ==, 2000); saw_eur = TRUE;
		}
	}
	g_assert_true(saw_eur);
	g_test_message("Rep and team USD targets reconcile to original booked credit after reassignment/inactivation; weighted forecast remains separate and EUR reports no quota");
}
/* The deal service stamps new closes with its real clock. This private
 * historical fixture positions retained credits at exact report boundaries. */
static void sales_credit_time(SalesFixture *f, gint64 deal, GDateTime *when)
{
	g_autofree gchar *text = venture_time_to_string(when);
	g_autoptr(GError) error = NULL;
	GList *params = g_list_append(NULL, orm_value_new_string(text));
	const gchar *sql = venture_database_get_backend(f->db) == VENTURE_DATABASE_BACKEND_POSTGRES ?
		"UPDATE sales_credits SET credited_at = $1 WHERE deal_id = $2" : "UPDATE sales_credits SET credited_at = ? WHERE deal_id = ?";
	params = g_list_append(params, orm_value_new_integer(deal));
	g_assert_true(venture_database_execute(f->db, sql, params, &error)); g_assert_no_error(error);
	g_list_free_full(params, (GDestroyNotify)orm_value_free);
}
static void test_sales_boundaries(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) team = sales_record(f, "team", "Boundary team"), user = NULL, quota = NULL;
	g_autoptr(VentureEntity) included = sales_record(f, "deal", "At start"), excluded = sales_record(f, "deal", "At end");
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0), end = g_date_time_new_utc(2026, 2, 1, 0, 0, 0);
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(1000, "USD");
	g_autoptr(JsonNode) report = NULL;
	JsonObject *row;
	(void)data;
	sales_save(f, team); user = sales_user(f, "period-rep", venture_entity_get_id(team));
	quota = sales_quota(f, "January quota", venture_entity_get_id(user), 0, "USD", 10000, start, end); sales_save(f, quota);
	g_object_set(included, "owner", "period-rep", "value", value, "stage", VENTURE_DEAL_STAGE_WON, "closed-at", start, NULL); sales_save(f, included);
	g_object_set(excluded, "owner", "period-rep", "value", value, "stage", VENTURE_DEAL_STAGE_WON, "closed-at", end, NULL); sales_save(f, excluded);
	sales_credit_time(f, venture_entity_get_id(included), start); sales_credit_time(f, venture_entity_get_id(excluded), end);
	report = sales_report(f, start, end); row = sales_quota_row(report, venture_entity_get_id(quota)); g_assert_nonnull(row);
	g_assert_cmpint(sales_report_amount(row, "actual"), ==, 1000); g_assert_cmpint(json_object_get_int_member(row, "bookings"), ==, 1);
	g_test_message("A booking at the start belongs to the quota; the identical amount at its exclusive end does not");
}
static gboolean sales_reject_credit(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	(void)db; (void)entity; (void)previous; (void)data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Fixture rejected derived credit"); return FALSE;
}
static void test_sales_atomic(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) deal = sales_record(f, "deal", "Atomic win"), target = NULL, stored = NULL;
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(1000, "USD");
	g_autoptr(VentureDeal) moved = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(deal, "value", value, NULL); sales_save(f, deal);
	venture_query_set_organization(query, f->org); venture_query_add_filter_int(query, "pipeline-id", VENTURE_FILTER_OP_EQ, sales_int(deal, "pipeline-id"), NULL);
	venture_query_add_filter_int(query, "kind", VENTURE_FILTER_OP_EQ, 1, NULL); target = venture_database_find_one(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(target);
	venture_database_add_save_validator(f->db, venture_entity_registry_lookup(venture_entity_registry_get_default(), "sales_credit"), sales_reject_credit, NULL, NULL);
	moved = venture_deal_service_move_stage(venture_database_get_deal_service(f->db), VENTURE_DEAL(deal), venture_entity_get_id(target), "Refused win", NULL, &error);
	g_assert_null(moved); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	stored = venture_database_get(f->db, VENTURE_TYPE_DEAL, venture_entity_get_id(deal), &error); g_assert_no_error(error); g_assert_nonnull(stored);
	g_assert_cmpint(sales_int(stored, "stage"), ==, VENTURE_DEAL_STAGE_LEAD);
	credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 0);
	g_test_message("A refused derived credit rolled back the won transition and left no partial credit");
}
static void test_sales_foreign(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) foreign = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Foreign", "slug", "sales-foreign", "active", TRUE, NULL);
	g_autoptr(VentureEntity) team = sales_record(f, "team", "Foreign team"), territory = sales_record(f, "sales_territory", "Refused territory"), forged = sales_record(f, "sales_credit", "Forged history");
	g_autoptr(GError) error = NULL;
	(void)data;
	sales_save(f, foreign); venture_entity_set_organization_id(team, venture_entity_get_id(foreign)); sales_save(f, team);
	g_object_set(territory, "team-id", venture_entity_get_id(team), "active", TRUE, NULL);
	g_assert_false(venture_database_save(f->db, territory, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_false(venture_database_save(f->db, forged, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
static void test_sales_module_history(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) deal = sales_record(f, "deal", "Tracked before hiding"), late = sales_record(f, "deal", "Later observed booking");
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GPtrArray) credits = NULL;
	(void)data;
	g_object_set(deal, "value", value, NULL); sales_save(f, deal); sales_move(f, &deal, 1);
	venture_config_set_module_enabled(f->config, "sales_performance", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "sales_credit"), ==, G_TYPE_INVALID);
	sales_move(f, &deal, 0);
	g_object_set(late, "value", value, "stage", VENTURE_DEAL_STAGE_WON, NULL); sales_save(f, late);
	venture_config_set_module_enabled(f->config, "sales_performance", TRUE);
	credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 3);
	g_test_message("Hiding the initialized sales module did not leave a tracked cancellation unrecorded or destroy booking history");
}
/* A populated pre-feature installation has current winners but no evidence
 * from which to reconstruct the original representative or booking amount. */
static void test_sales_upgrade(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) deal = sales_record(f, "deal", "Historical winner");
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(1000, "USD");
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = g_date_time_new_utc(2020, 1, 1, 0, 0, 0), end = g_date_time_new_utc(2100, 1, 1, 0, 0, 0);
	g_autoptr(JsonNode) report = NULL;
	JsonArray *metrics;
	gboolean found = FALSE;
	guint i;
	(void)data;
	/* These tables belong only to this disposable fixture's schema. */
	venture_config_set_module_enabled(f->config, "sales_performance", FALSE);
	g_assert_true(venture_database_execute(f->db, "DROP TABLE sales_credits; DROP TABLE sales_assignments; DROP TABLE sales_quotas; DROP TABLE sales_territories", NULL, &error));
	g_assert_no_error(error);
	g_object_set(deal, "value", value, "stage", VENTURE_DEAL_STAGE_WON, NULL); sales_save(f, deal);
	venture_config_set_module_enabled(f->config, "sales_performance", TRUE);
	{ gboolean migrated = venture_database_migrate(f->db, venture_entity_registry_get_default(), &error); g_assert_no_error(error); g_assert_true(migrated); }
	credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 0);
	g_object_set(deal, "notes", "Historical description corrected", NULL); sales_save(f, deal);
	report = sales_report(f, start, end);
	metrics = json_object_get_array_member(json_node_get_object(report), "metrics");
	for (i = 0; i < json_array_get_length(metrics); i++) {
		JsonObject *metric = json_array_get_object_element(metrics, i);
		if (!g_strcmp0(json_object_get_string_member(metric, "key"), "uncaptured_wins")) {
			g_assert_cmpfloat(json_object_get_double_member(metric, "value"), ==, 1); found = TRUE;
		}
	}
	g_assert_true(found);
	sales_move(f, &deal, 0); g_clear_pointer(&credits, g_ptr_array_unref);
	credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 0);
	sales_move(f, &deal, 1); g_clear_pointer(&credits, g_ptr_array_unref);
	credits = sales_rows(f, "sales_credit"); g_assert_cmpuint(credits->len, ==, 1);
	g_test_message("Populated upgrade exposed an uncaptured historical win without inventing credit; only a later observed win created evidence");
}
static void test_sales_foreign_owner(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) outsider = g_object_new(VENTURE_TYPE_USER, "username", "foreign-private-user", "active", TRUE, NULL);
	g_autoptr(VentureEntity) deal = sales_record(f, "deal", "Refuse foreign representative");
	g_autoptr(GError) error = NULL;
	(void)data;
	sales_save(f, outsider);
	/* A globally existing username is not proof of organization membership,
	 * even when the proposed assignment has no team. */
	g_object_set(deal, "owner", "foreign-private-user", NULL);
	g_assert_false(venture_database_save(f->db, deal, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
static void test_sales_scoped(SalesFixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) team = sales_record(f, "team", "Scoped team"), user = NULL, territory = NULL, rule = NULL;
	g_autoptr(VentureEntity) foreign = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other business", "slug", "other-sales-business", "active", TRUE, NULL);
	g_autoptr(VentureEntity) foreign_deal = sales_record(f, "deal", "Private foreign winner"), lead = sales_record(f, "lead", "Scoped website inquiry");
	g_autoptr(VentureMoney) value = venture_money_new_for_currency(900, "USD");
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) start = g_date_time_new_utc(2020, 1, 1, 0, 0, 0), end = g_date_time_new_utc(2100, 1, 1, 0, 0, 0);
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(VentureEntity) quota = NULL;
	VentureAuthPrincipal actor;
	gchar name[] = "scoped-sales-member";
	(void)data;
	sales_save(f, team); user = sales_user(f, name, venture_entity_get_id(team));
	territory = sales_territory(f, "Scoped territory", venture_entity_get_id(team));
	rule = sales_rule(f, "Scoped website routing", venture_entity_get_id(territory), venture_entity_get_id(team), 10);
	sales_save(f, foreign); venture_entity_set_organization_id(foreign_deal, venture_entity_get_id(foreign));
	g_object_set(foreign_deal, "stage", VENTURE_DEAL_STAGE_WON, "value", value, NULL); sales_save(f, foreign_deal);
	quota = sales_quota(f, "Scoped personal target", venture_entity_get_id(user), 0, "USD", 10000, start, end); sales_save(f, quota);
	actor.user_id = venture_entity_get_id(user); actor.token_id = 0; actor.role = VENTURE_USER_ROLE_EDITOR;
	actor.name = name; actor.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &actor);
	/* The user cannot enumerate private user rows; the rota must resolve
	 * only verified membership internally and restore this caller scope. */
	g_object_set(lead, "source", "website", "team-id", venture_entity_get_id(team), NULL); sales_save(f, lead);
	g_assert_cmpint(sales_int(lead, "owner-user-id"), ==, venture_entity_get_id(user));
	rows = sales_rows(f, "sales_assignment"); g_assert_cmpuint(rows->len, ==, 1);
	report = sales_report(f, start, end);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(report), "rows")), ==, 1);
	g_assert_cmpint(sales_report_amount(sales_quota_row(report, venture_entity_get_id(quota)), "actual"), ==, 0);
	g_assert_nonnull(venture_access_policy_get_actor(venture_database_get_access_policy(f->db)));
	g_test_message("A sales member routed through verified private user identities while the report excluded another organization's booking and retained caller authority");
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sales-performance/records", test_sales_records);
	g_test_add("/sales-performance/routing", SalesFixture, NULL, sales_setup, test_sales_routing, sales_teardown);
	g_test_add("/sales-performance/credit", SalesFixture, NULL, sales_setup, test_sales_credit, sales_teardown);
	g_test_add("/sales-performance/quotas", SalesFixture, NULL, sales_setup, test_sales_quotas, sales_teardown);
	g_test_add("/sales-performance/attainment", SalesFixture, NULL, sales_setup, test_sales_attainment, sales_teardown);
	g_test_add("/sales-performance/boundaries", SalesFixture, NULL, sales_setup, test_sales_boundaries, sales_teardown);
	g_test_add("/sales-performance/atomic", SalesFixture, NULL, sales_setup, test_sales_atomic, sales_teardown);
	g_test_add("/sales-performance/foreign", SalesFixture, NULL, sales_setup, test_sales_foreign, sales_teardown);
	g_test_add("/sales-performance/module-history", SalesFixture, NULL, sales_setup, test_sales_module_history, sales_teardown);
	g_test_add("/sales-performance/upgrade", SalesFixture, NULL, sales_setup, test_sales_upgrade, sales_teardown);
	g_test_add("/sales-performance/foreign-owner", SalesFixture, NULL, sales_setup, test_sales_foreign_owner, sales_teardown);
	g_test_add("/sales-performance/scoped", SalesFixture, NULL, sales_setup, test_sales_scoped, sales_teardown);
	return g_test_run();
}
