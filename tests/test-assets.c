/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

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
	g_assert_true(venture_database_save(f->db, entity, NULL, &error));
	g_assert_no_error(error);
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/assets/records", test_records);
	g_test_add("/assets/straight-line-exact", Fixture, NULL, setup, test_schedule, teardown);
	g_test_add("/assets/generic-status-refused", Fixture, NULL, setup, test_status_refused, teardown);
	return g_test_run();
}
