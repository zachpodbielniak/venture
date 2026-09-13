/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

/* A module must expose all four field tables through the shared registry. */
static void
test_records(void)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	const gchar *names[] = { "fixed_asset", "depreciation_entry", "deferral", "deferral_entry" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}

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
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
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
save(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, entity, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static gint64
account(Fixture *f, const gchar *code, VentureAccountKind kind)
{
	g_autoptr(VentureEntity) entity = VENTURE_ENTITY(venture_account_new());
	g_object_set(entity, "organization-id", f->org, "name", code, "code", code,
		"kind", kind, "active", TRUE, NULL);
	save(f, entity);
	return venture_entity_get_id(entity);
}

static VentureEntity *
asset(Fixture *f, const gchar *tag)
{
	VentureEntity *entity = VENTURE_ENTITY(venture_fixed_asset_new());
	g_autoptr(VentureMoney) cost = venture_money_new_for_currency(110000, "USD");
	g_autoptr(VentureMoney) salvage = venture_money_new_zero("USD");
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_object_set(entity, "organization-id", f->org, "name", "Laptop", "tag", tag,
		"cost", cost, "salvage-value", salvage, "useful-life-months", (gint64)36,
		"acquired-at", date, "in-service-at", date,
		"asset-account-id", account(f, tag, VENTURE_ACCOUNT_KIND_ASSET),
		"accumulated-depreciation-account-id", account(f, "AD", VENTURE_ACCOUNT_KIND_ASSET),
		"depreciation-expense-account-id", account(f, "DE", VENTURE_ACCOUNT_KIND_EXPENSE), NULL);
	save(f, entity);
	return entity;
}

static void
place(Fixture *f, VentureEntity *entity)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(entity, "operation", "place", &error));
	g_assert_no_error(error);
	save(f, entity);
}

/* The final month takes the cents left by equal preceding installments. */
static void
test_schedule(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "LAPTOP");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEPRECIATION_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint64 sum = 0;
	guint i;
	(void)data;
	place(f, entity);
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 36);
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *period = NULL;
		g_object_get(g_ptr_array_index(rows, i), "amount", &amount, "period", &period, NULL);
		sum += amount->amount;
		g_assert_cmpint(amount->amount, ==, g_str_equal(period, "2028-12") ? 3075 : 3055);
	}
	g_assert_cmpint(sum, ==, 110000);
}

/* Generic status writes must never manufacture a service transition. */
static void
test_status_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "STATUS");
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(entity, "status", VENTURE_ASSET_STATUS_IN_SERVICE, NULL);
	g_assert_false(venture_database_save(f->db, entity, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureAssetService"));
}

static void
test_declining(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "DECLINING");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEPRECIATION_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint64 sum = 0;
	guint i;
	(void)data;
	g_object_set(entity, "method", VENTURE_ASSET_METHOD_DECLINING_BALANCE, NULL);
	save(f, entity);
	place(f, entity);
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 36);
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *period = NULL;
		g_object_get(g_ptr_array_index(rows, i), "amount", &amount, "period", &period, NULL);
		sum += amount->amount;
		if (g_str_equal(period, "2026-01"))
			g_assert_cmpint(amount->amount, ==, 6111);
	}
	g_assert_cmpint(sum, ==, 110000);
}

static void
test_deferral(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_deferral_new());
	g_autoptr(VentureMoney) total = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEFERRAL_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	gint64 sum = 0;
	guint i;
	(void)data;
	g_object_set(row, "organization-id", f->org, "description", "Accrued consulting",
		"total", total, "months", (gint64)3, "start", start,
		"kind", VENTURE_DEFERRAL_KIND_ACCRUAL,
		"source-account-id", account(f, "AC", VENTURE_ACCOUNT_KIND_LIABILITY),
		"target-account-id", account(f, "EX", VENTURE_ACCOUNT_KIND_EXPENSE), NULL);
	save(f, row);
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 3);
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(g_ptr_array_index(rows, i), "amount", &amount, NULL);
		sum += amount->amount;
	}
	g_assert_cmpint(sum, ==, 10000);
}

/* A period sweep retries safely and retains a source link on each journal. */
static void
test_sweep(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "SWEEP");
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	place(f, entity);
	current = venture_database_get(f->db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(entity), &error);
	g_assert_no_error(error);
	g_assert_true(venture_entity_set_field_from_string(current, "operation", "run-period:2026-01", &error));
	save(f, current);
	save(f, current);
	venture_query_set_organization(query, f->org);
	journals = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(journals->len, ==, 1);
	{
		g_autofree gchar *source = NULL;
		g_object_get(g_ptr_array_index(journals, 0), "source-type", &source, NULL);
		g_assert_cmpstr(source, ==, "depreciation_entry");
	}
}

static void
test_remove_history(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "RETAIN");
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	place(f, entity);
	current = venture_database_get(f->db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(entity), &error);
	g_assert_no_error(error);
	g_assert_false(venture_database_delete(f->db, current, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VentureAssetService"));
	g_clear_error(&error);
	g_object_set(current, "status", VENTURE_ASSET_STATUS_DRAFT, NULL);
	g_assert_false(venture_database_delete(f->db, current, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_closed_schedule(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "CLOSED");
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(VentureFiscalYear) year = NULL;
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureQuery) entries = venture_query_new(VENTURE_TYPE_DEPRECIATION_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	year = venture_period_service_generate(venture_period_service_get(f->db), f->org,
		"2026", start, VENTURE_PERIOD_MONTHLY, NULL, &error);
	g_assert_no_error(error);
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_order(q, "start-at", VENTURE_SORT_ASCENDING, &error));
	period = venture_database_find_one(f->db, q, &error);
	g_assert_no_error(error);
	g_object_set(period, "state", VENTURE_PERIOD_CLOSED, NULL);
	{
		VentureActor actor;
		actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
		actor.name = "bookkeeper";
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;
		g_assert_true(venture_database_save(f->db, period, &actor, &error));
		g_assert_no_error(error);
	}
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org, FALSE, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "closed"));
	g_clear_error(&error);
	{
		const gchar *names[] = { "fixed_assets", "deferrals" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(names); i++)
		{
			g_autoptr(VentureQuery) snapshot_query = venture_query_new(VENTURE_TYPE_REPORT_SNAPSHOT);
			g_autoptr(VentureEntity) snapshot = NULL;
			venture_query_set_organization(snapshot_query, f->org);
			g_assert_true(venture_query_add_filter_string(snapshot_query, "report", VENTURE_FILTER_OP_EQ, names[i], &error));
			snapshot = venture_database_find_one(f->db, snapshot_query, &error);
			g_assert_no_error(error);
			g_assert_nonnull(snapshot);
		}
	}
	place(f, entity);
	venture_query_set_organization(entries, f->org);
	g_assert_true(venture_query_add_filter_string(entries, "period", VENTURE_FILTER_OP_EQ, "2026-01", &error));
	rows = venture_database_find(f->db, entries, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 0);
}

static void
test_dispose(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "DISPOSE");
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 2, 1, 0, 0, 0);
	g_autoptr(VentureMoney) proceeds = venture_money_new_for_currency(50000, "USD");
	g_autoptr(GError) error = NULL;
	(void)data;
	place(f, entity);
	current = venture_database_get(f->db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(entity), &error);
	g_assert_no_error(error);
	g_object_set(current, "disposed-at", date, "disposal-proceeds", proceeds, NULL);
	g_assert_true(venture_entity_set_field_from_string(current, "operation", "write-off", &error));
	/* write-off must use zero proceeds, even if a stale form carries cash */
	g_clear_pointer(&proceeds, venture_money_free);
	proceeds = venture_money_new_zero("USD");
	g_object_set(current, "disposal-proceeds", proceeds, NULL);
	save(f, current);
}

static void
test_staged_action(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) original = asset(f, "STAGE");
	g_autoptr(VentureEntity) proposal = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	VentureConfirmation *confirmation;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	VentureActor actor;
	(void)data;
	proposal = venture_database_get(f->db, VENTURE_TYPE_FIXED_ASSET, venture_entity_get_id(original), &error);
	g_assert_no_error(error);
	g_object_set(proposal, "operation", "place", NULL);
	actor.kind = VENTURE_ACTOR_KIND_AI;
	actor.name = "assistant";
	actor.prompt = "Place the laptop in service";
	actor.request_id = NULL;
	actor.approved_by = NULL;
	confirmation = venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_UPDATE,
		proposal, original, &actor, "assistant", &error);
	g_assert_no_error(error);
	g_assert_nonnull(confirmation);
	g_assert_true(json_object_has_member(json_node_get_object(venture_confirmation_get_diff(confirmation)), "operation"));
	id = g_strdup(venture_confirmation_get_id(confirmation));
	g_assert_true(venture_confirmation_store_approve(store, id, "bookkeeper", &error));
	g_assert_no_error(error);
}

static void
test_reports(Fixture *f, gconstpointer data)
{
	VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
	VentureReport *assets = venture_report_registry_lookup(registry, "fixed_assets");
	VentureReport *deferrals = venture_report_registry_lookup(registry, "deferrals");
	gboolean financial = FALSE;
	(void)data;
	g_assert_nonnull(assets);
	g_assert_nonnull(deferrals);
	g_object_get(assets, "financial", &financial, NULL);
	g_assert_true(financial);
	g_object_get(deferrals, "financial", &financial, NULL);
	g_assert_true(financial);
}

static void
test_accrual_settlement(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_deferral_new());
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureMoney) total = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(GError) error = NULL;
	gint64 accrued = account(f, "AC", VENTURE_ACCOUNT_KIND_LIABILITY);
	(void)data;
	g_object_set(row, "organization-id", f->org, "description", "Consulting", "total", total,
		"months", (gint64)1, "start", start, "kind", VENTURE_DEFERRAL_KIND_ACCRUAL,
		"source-account-id", accrued, "target-account-id", account(f, "EX", VENTURE_ACCOUNT_KIND_EXPENSE), NULL);
	save(f, row);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	current = venture_database_get(f->db, VENTURE_TYPE_DEFERRAL, venture_entity_get_id(row), &error);
	g_assert_no_error(error);
	g_assert_true(venture_entity_set_field_from_string(current, "operation", "settle", &error));
	g_object_set(current, "settlement-account-id", account(f, "CASH", VENTURE_ACCOUNT_KIND_ASSET), NULL);
	save(f, current);
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(VentureMoney) balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db), accrued, f->org, "USD", now, &error);
		g_assert_no_error(error);
		g_assert_cmpint(balance->amount, ==, 0);
	}
}

static void
test_atomic_period(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) laptop = asset(f, "ATOMIC");
	g_autoptr(VentureEntity) deferral = VENTURE_ENTITY(venture_deferral_new());
	g_autoptr(VentureEntity) bad = NULL;
	g_autoptr(VentureMoney) total = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GError) error = NULL;
	gint64 accrued = account(f, "AC", VENTURE_ACCOUNT_KIND_LIABILITY);
	VentureAssetService *service = venture_asset_service_get(f->db);
	(void)data;
	place(f, laptop);
	g_object_set(deferral, "organization-id", f->org, "description", "Consulting", "total", total,
		"months", (gint64)3, "start", start, "kind", VENTURE_DEFERRAL_KIND_ACCRUAL,
		"source-account-id", accrued, "target-account-id", account(f, "EX", VENTURE_ACCOUNT_KIND_EXPENSE), NULL);
	save(f, deferral);
	bad = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, accrued, &error);
	g_assert_no_error(error);
	g_object_set(bad, "active", FALSE, NULL);
	save(f, bad);
	g_assert_cmpint(venture_asset_service_run_period(service, "2026-01", f->org, FALSE, NULL, &error), ==, -1);
	g_assert_nonnull(error);
	g_clear_error(&error);
	venture_query_set_organization(query, f->org);
	journals = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(journals->len, ==, 0);
	g_object_set(bad, "active", TRUE, NULL);
	save(f, bad);
	g_assert_cmpint(venture_asset_service_run_period(service, "2026-01", f->org, TRUE, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_clear_pointer(&journals, g_ptr_array_unref);
	journals = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(journals->len, ==, 0);
	g_assert_cmpint(venture_asset_service_run_period(service, "2026-01", f->org, FALSE, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(venture_asset_service_run_period(service, "2026-01", f->org, FALSE, NULL, &error), ==, 0);
	g_assert_no_error(error);
}

static void
test_migration_script(void)
{
	g_autofree gchar *sql = NULL;
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_true(g_file_get_contents("migrations/sqlite/000190_assets.sql", &sql, NULL, &error));
	g_assert_no_error(error);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	/* A disabled new module has no tables. A partial module is refused;
	 * rollback and retry leave unrelated historic values untouched. */
	g_assert_true(venture_database_execute(db, "CREATE TABLE old_cost (amount BIGINT); INSERT INTO old_cost VALUES (110000)", NULL, &error));
	g_assert_true(venture_database_execute(db, sql, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(db, "CREATE TABLE fixed_assets (id BIGINT)", NULL, &error));
	g_assert_true(venture_database_begin(db, &error));
	g_assert_false(venture_database_execute(db, sql, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	venture_database_rollback(db);
	g_assert_true(venture_database_execute(db, "CREATE TABLE depreciation_entries (id BIGINT); CREATE TABLE deferrals (id BIGINT); CREATE TABLE deferral_entries (id BIGINT)", NULL, &error));
	g_assert_true(venture_database_execute(db, sql, NULL, &error));
	g_assert_true(venture_database_execute(db, sql, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(OrmResult) result = venture_database_query_raw(db, "SELECT amount FROM old_cost", NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 110000);
	}
}

static GError *
veto_transition(VentureAssetService *service, VentureEntity *entity, const gchar *operation, gpointer data)
{
	(void)service;
	(void)entity;
	(void)operation;
	(void)data;
	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Asset review veto");
}

static void
test_transition_veto(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "VETO");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DEPRECIATION_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	VentureAssetService *service = venture_asset_service_get(f->db);
	(void)data;
	g_assert_cmpuint(g_signal_lookup("transition", VENTURE_TYPE_ASSET_SERVICE), !=, 0);
	g_signal_connect(service, "transition", G_CALLBACK(veto_transition), NULL);
	g_assert_false(venture_asset_service_place_in_service(service, entity, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	venture_query_set_organization(query, f->org);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 0);
}

static void
test_source_expense(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) expense = VENTURE_ENTITY(venture_expense_new());
	g_autoptr(VentureEntity) draft = NULL;
	g_autoptr(VentureMoney) cost = venture_money_new_for_currency(110000, "USD");
	g_autoptr(VentureMoney) copied = NULL;
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 3, 0, 0, 0);
	g_autoptr(GDateTime) acquired = NULL;
	g_autoptr(GError) error = NULL;
	gint64 source;
	gint status;
	(void)data;
	g_object_set(expense, "organization-id", f->org, "description", "Laptop", "amount", cost,
		"occurred-at", date, "deductibility", VENTURE_DEDUCTIBILITY_CAPITAL, NULL);
	save(f, expense);
	draft = venture_asset_service_create_from_expense(venture_asset_service_get(f->db), expense, "EXPENSE", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(draft);
	g_object_get(draft, "cost", &copied, "acquired-at", &acquired, "source-expense-id", &source, "status", &status, NULL);
	g_assert_true(venture_money_equal(cost, copied));
	g_assert_cmpint(g_date_time_compare(acquired, date), ==, 0);
	g_assert_cmpint(source, ==, venture_entity_get_id(expense));
	g_assert_cmpint(status, ==, VENTURE_ASSET_STATUS_DRAFT);
	g_clear_object(&draft);
	g_object_set(expense, "deductibility", VENTURE_DEDUCTIBILITY_FULL, NULL);
	save(f, expense);
	draft = venture_asset_service_create_from_expense(venture_asset_service_get(f->db), expense, "NOTCAPITAL", NULL, &error);
	g_assert_null(draft);
	g_assert_nonnull(error);
}

static void
test_prepayment_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_deferral_new());
	g_autoptr(VentureMoney) total = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = venture_context_parse_period(f->context, "2026-01", &error);
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(JsonNode) options = venture_json_parse("{\"as_of\":\"2026-01-31\"}", NULL);
	VentureReport *definition = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "deferrals");
	JsonObject *reported;
	(void)data;
	g_object_set(row, "organization-id", f->org, "description", "Annual insurance", "total", total,
		"months", (gint64)3, "start", start, "kind", VENTURE_DEFERRAL_KIND_PREPAYMENT,
		"source-account-id", account(f, "PRE", VENTURE_ACCOUNT_KIND_ASSET),
		"funding-account-id", account(f, "CASH", VENTURE_ACCOUNT_KIND_ASSET),
		"target-account-id", account(f, "EX", VENTURE_ACCOUNT_KIND_EXPENSE), NULL);
	save(f, row);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	/* A later release must not change the January report. */
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-02", f->org, FALSE, NULL, &error), ==, 1);
	report = venture_report_generate(definition, f->context, period, json_node_get_object(options), &error);
	g_assert_no_error(error);
	json = venture_report_result_to_json(report);
	reported = json_array_get_object_element(json_object_get_array_member(json_node_get_object(json), "rows"), 0);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(reported, "balance"), "amount"), ==, 6667);
}

static void
test_automation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "AUTOMATION");
	g_autofree gchar *directory = g_dir_make_tmp("venture-assets-XXXXXX", NULL);
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *arguments[] = { "2026-01", NULL };
	(void)data;
	place(f, entity);
	g_object_set(f->config, "state-dir", directory, "automation-enabled", TRUE, NULL);
	automation = venture_automation_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_automation_invoke(automation, "assets_run_period", arguments, &result, &error));
	g_assert_no_error(error);
	g_clear_object(&automation);
	venture_test_remove_tree(directory);
}

static void
test_accrued_income(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_deferral_new());
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) start = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	gint64 accrued = account(f, "AI", VENTURE_ACCOUNT_KIND_ASSET);
	(void)data;
	g_object_set(row, "organization-id", f->org, "description", "Accrued income", "total", amount,
		"months", (gint64)1, "start", start, "kind", VENTURE_DEFERRAL_KIND_ACCRUAL,
		"source-account-id", accrued, "target-account-id", account(f, "IN", VENTURE_ACCOUNT_KIND_INCOME), NULL);
	save(f, row);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db), accrued, f->org, "USD", now, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 10000);
}

typedef struct
{
	gboolean done;
	gchar *out;
	gchar *err;
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
http_request(VentureWebServer *server, const gchar *method, const gchar *path, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	SurfaceResult response;
	guint status;
	memset(&response, 0, sizeof(response));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, g_str_has_prefix(path, "/api/") ? "application/json" : "application/x-www-form-urlencoded", bytes);
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
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &response->out, &response->err, &response->error);
	response->done = TRUE;
}

static gboolean
cli_timeout(gpointer data)
{
	g_subprocess_force_exit(G_SUBPROCESS(data));
	return G_SOURCE_CONTINUE;
}

static void
run_cli(VentureWebServer *server, const gchar *noun, const gchar *verb, const gchar *id)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *argv[] = { "build/debug/venturectl", "--server", venture_web_server_get_base_url(server), "-f", "json", noun, verb, id, NULL };
	SurfaceResult response;
	guint timeout;
	memset(&response, 0, sizeof(response));
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "assets-private-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(response.error);
	if (!g_subprocess_get_successful(process))
		g_test_message("CLI output: %s; error: %s", response.out, response.err);
	g_assert_true(g_subprocess_get_successful(process));
	g_free(response.out);
	g_free(response.err);
}

static void
test_surfaces(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "SURFACES");
	g_autofree gchar *directory = g_dir_make_tmp("venture-assets-XXXXXX", NULL);
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(entity));
	g_autofree gchar *path = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(JsonNode) json = NULL;
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	gint kind = GPOINTER_TO_INT(data);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	if (kind == 0)
		run_cli(server, "asset", "place", id);
	else if (kind == 1)
	{
		g_autoptr(GPtrArray) pending = NULL;
		g_autofree gchar *confirmation_id = NULL;
		path = g_strdup_printf("/api/v1/fixed_assets/%s/place-in-service?stage=1", id);
		g_assert_cmpuint(http_request(server, "POST", path, "{}", &body), ==, SOUP_STATUS_ACCEPTED);
		pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(f->context));
		g_assert_cmpuint(pending->len, ==, 1);
		confirmation_id = g_strdup(venture_confirmation_get_id(g_ptr_array_index(pending, 0)));
		g_assert_true(venture_confirmation_store_approve(venture_context_get_confirmations(f->context), confirmation_id, "bookkeeper", &error));
		g_assert_no_error(error);
	}
	else
	{
		path = g_strdup_printf("/assets/%s/place-in-service", id);
		g_assert_cmpuint(http_request(server, "POST", path, "", NULL), ==, SOUP_STATUS_FOUND);
	}
	g_clear_pointer(&path, g_free);
	g_clear_pointer(&body, g_free);
	path = g_strdup_printf("/e/fixed_asset/%s", id);
	g_assert_cmpuint(http_request(server, "GET", path, NULL, &body), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "Dispose"));
	g_assert_nonnull(strstr(body, "2026-01"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/assets/run-period", "{\"period\":\"2026-01\",\"dry_run\":true}", &body), ==, SOUP_STATUS_OK);
	json = venture_json_parse(body, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(json), "entries"), ==, 1);
	run_cli(server, "assets", "run-period", "2026-01");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/assets/run-period", "{\"period\":\"2026-01\"}", &body), ==, SOUP_STATUS_OK);
	g_clear_pointer(&json, json_node_unref);
	json = venture_json_parse(body, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(json), "entries"), ==, 0);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/fixed_assets/%s/dispose", id);
	g_assert_cmpuint(http_request(server, "POST", path, "{\"disposal_proceeds\":\"0 USD\",\"disposed_at\":\"2026-02-01\"}", NULL), ==, SOUP_STATUS_OK);
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(directory);
}

static void
test_capitalization(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) expense = VENTURE_ENTITY(venture_expense_new());
	g_autoptr(VentureEntity) draft = NULL;
	g_autoptr(VentureMoney) cost = venture_money_new_for_currency(110000, "USD");
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 1, 3, 0, 0, 0);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	gint64 asset_account = account(f, "FIXED", VENTURE_ACCOUNT_KIND_ASSET);
	(void)data;
	g_object_set(expense, "organization-id", f->org, "description", "Laptop", "amount", cost,
		"occurred-at", date, "deductibility", VENTURE_DEDUCTIBILITY_CAPITAL, NULL);
	save(f, expense);
	draft = venture_asset_service_create_from_expense(venture_asset_service_get(f->db), expense, "CAPITALIZE", NULL, &error);
	g_assert_no_error(error);
	g_object_set(draft, "in-service-at", date, "useful-life-months", (gint64)36,
		"asset-account-id", asset_account,
		"accumulated-depreciation-account-id", account(f, "AD", VENTURE_ACCOUNT_KIND_ASSET),
		"depreciation-expense-account-id", account(f, "DE", VENTURE_ACCOUNT_KIND_EXPENSE), NULL);
	save(f, draft);
	place(f, draft);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db), asset_account, f->org, "USD", now, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, 110000);
}

static void
test_tag_and_module(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) first = VENTURE_ENTITY(venture_fixed_asset_new());
	g_autoptr(VentureEntity) second = VENTURE_ENTITY(venture_fixed_asset_new());
	g_autoptr(VentureEntity) other = VENTURE_ENTITY(venture_organization_new());
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(first, "organization-id", f->org, "name", "One", "tag", "TAG", NULL);
	g_object_set(second, "organization-id", f->org, "name", "Two", "tag", "TAG", NULL);
	save(f, first);
	g_assert_false(venture_database_save(f->db, second, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_object_set(other, "name", "Another organization", NULL);
	save(f, other);
	g_object_set(second, "organization-id", venture_entity_get_id(other), NULL);
	save(f, second);
	venture_config_set_module_enabled(f->config, "assets", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "fixed_asset"), ==, G_TYPE_INVALID);
	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "fixed_assets"));
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org, FALSE, NULL, &error), ==, -1);
	g_assert_nonnull(error);
}

static void
test_disposal_proceeds(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "PROCEEDS");
	g_autoptr(VentureMoney) proceeds = venture_money_new_for_currency(GPOINTER_TO_INT(data) ? 120000 : 50000, "USD");
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) date = g_date_time_new_utc(2026, 2, 1, 0, 0, 0);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	gint64 cash = account(f, "CASH", VENTURE_ACCOUNT_KIND_ASSET);
	gint64 gain_loss = account(f, "GAINLOSS", VENTURE_ACCOUNT_KIND_INCOME);
	g_object_set(entity, "proceeds-account-id", cash, "gain-loss-account-id", gain_loss, NULL);
	save(f, entity);
	place(f, entity);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_object_set(entity, "disposed-at", date, "disposal-proceeds", proceeds, NULL);
	g_assert_true(venture_asset_service_dispose(venture_asset_service_get(f->db), entity, FALSE, NULL, &error));
	g_assert_no_error(error);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db), cash, f->org, "USD", now, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, proceeds->amount);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db), gain_loss, f->org, "USD", now, &error);
	g_assert_no_error(error);
	g_assert_cmpint(balance->amount, ==, GPOINTER_TO_INT(data) ? -13055 : 56945);
}

static void
test_register_values(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) entity = asset(f, "REGISTER");
	g_autoptr(VentureEntity) discarded = venture_entity_duplicate(entity);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = venture_context_parse_period(f->context, "2026-01", &error);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) json = NULL;
	JsonArray *rows;
	guint i;
	(void)data;
	/* A soft-deleted draft retains its tag but contributes no book value. */
	g_object_set(discarded, "tag", "DISCARDED", NULL);
	save(f, discarded);
	g_assert_true(venture_database_delete(f->db, discarded, NULL, &error));
	g_assert_no_error(error);
	g_object_set(entity, "category", "Computers", NULL);
	save(f, entity);
	place(f, entity);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-01", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(venture_asset_service_run_period(venture_asset_service_get(f->db), "2026-02", f->org, FALSE, NULL, &error), ==, 1);
	g_assert_no_error(error);
	result = venture_report_generate(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "fixed_assets"), f->context, period, NULL, &error);
	g_assert_no_error(error);
	json = venture_report_result_to_json(result);
	rows = json_object_get_array_member(json_node_get_object(json), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	for (i = 0; i < 2; i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(row, "cost"), "amount"), ==, 110000);
		g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(row, "released"), "amount"), ==, 3055);
		g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(row, "balance"), "amount"), ==, 106945);
	}
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 1), "name"), ==, "Category total");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/assets/records", test_records);
	g_test_add("/assets/straight-line-exact", Fixture, NULL, setup, test_schedule, teardown);
	g_test_add("/assets/generic-status-refused", Fixture, NULL, setup, test_status_refused, teardown);
	g_test_add("/assets/declining-exact", Fixture, NULL, setup, test_declining, teardown);
	g_test_add("/assets/deferral-exact", Fixture, NULL, setup, test_deferral, teardown);
	g_test_add("/assets/sweep-idempotent", Fixture, NULL, setup, test_sweep, teardown);
	g_test_add("/assets/retain-history", Fixture, NULL, setup, test_remove_history, teardown);
	g_test_add("/assets/closed-schedule-shifts", Fixture, NULL, setup, test_closed_schedule, teardown);
	g_test_add("/assets/write-off", Fixture, NULL, setup, test_dispose, teardown);
	g_test_add("/assets/staged-action", Fixture, NULL, setup, test_staged_action, teardown);
	g_test_add("/assets/financial-reports", Fixture, NULL, setup, test_reports, teardown);
	g_test_add("/assets/accrual-settlement", Fixture, NULL, setup, test_accrual_settlement, teardown);
	g_test_add("/assets/period-atomic-dry-run", Fixture, NULL, setup, test_atomic_period, teardown);
	g_test_add_func("/assets/migration", test_migration_script);
	g_test_add("/assets/transition-veto", Fixture, NULL, setup, test_transition_veto, teardown);
	g_test_add("/assets/source-capital-expense", Fixture, NULL, setup, test_source_expense, teardown);
	g_test_add("/assets/prepayment-historical-report", Fixture, NULL, setup, test_prepayment_report, teardown);
	g_test_add("/assets/automation", Fixture, NULL, setup, test_automation, teardown);
	g_test_add("/assets/accrued-income", Fixture, NULL, setup, test_accrued_income, teardown);
	g_test_add("/assets/surfaces-cli", Fixture, GINT_TO_POINTER(0), setup, test_surfaces, teardown);
	g_test_add("/assets/surfaces-staged-rest", Fixture, GINT_TO_POINTER(1), setup, test_surfaces, teardown);
	g_test_add("/assets/surfaces-web", Fixture, GINT_TO_POINTER(2), setup, test_surfaces, teardown);
	g_test_add("/assets/source-capitalization", Fixture, NULL, setup, test_capitalization, teardown);
	g_test_add("/assets/tag-scope-module-switch", Fixture, NULL, setup, test_tag_and_module, teardown);
	g_test_add("/assets/disposal-gain", Fixture, GINT_TO_POINTER(1), setup, test_disposal_proceeds, teardown);
	g_test_add("/assets/disposal-loss", Fixture, GINT_TO_POINTER(0), setup, test_disposal_proceeds, teardown);
	g_test_add("/assets/register-values", Fixture, NULL, setup, test_register_values, teardown);
	return g_test_run();
}
