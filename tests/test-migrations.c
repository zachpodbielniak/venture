/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <glib/gstdio.h>
#include "db/venture-migrations.h"
#include "venture-test-util.h"

/* Every shipped migration must be recorded, even as modules add migrations. */
static guint
migration_count(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDir) directory = g_dir_open("migrations/sqlite", 0, &error);
	const gchar *name;
	guint count = 0;
	g_assert_no_error(error);
	g_assert_nonnull(directory);
	while ((name = g_dir_read_name(directory)) != NULL)
		if (g_str_has_suffix(name, ".sql"))
			count++;
	return count;
}

/* Use a file and reconnect: an in-memory second run cannot demonstrate
 * that a deployed release recognizes another process's migration history. */
static void
test_upgrade_restart(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;
	guint run;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"CREATE TABLE venture_schema_version (version INTEGER NOT NULL);"
		"INSERT INTO venture_schema_version VALUES (1);"
		"CREATE TABLE historical_data (amount BIGINT); INSERT INTO historical_data VALUES (12345)", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&database);
	for (run = 0; run < 2; run++)
	{
		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		result = venture_database_query_raw(database,
			"SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations", NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, migration_count());
		g_clear_object(&result);
		result = venture_database_query_raw(database,
			"SELECT amount FROM historical_data", NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 12345);
		g_clear_object(&result);
		result = venture_database_query_raw(database,
			"SELECT CAST(COUNT(*) AS BIGINT) FROM venture_schema_version", NULL, &error);
		g_assert_no_error(error);
		g_assert_true(orm_result_next(result));
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
		g_clear_object(&result);
		g_clear_object(&database);
	}
	venture_test_remove_tree(directory);
}

/* The single text value of @sql. */
static gchar *
query_text(VentureDatabase *database, const gchar *sql)
{
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;

	result = venture_database_query_raw(database, sql, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	return g_strdup(orm_row_get_string(orm_result_get_row(result), 0));
}

/* Whether @table exists, through the same introspection startup uses. */
static gboolean
table_exists(VentureDatabase *database, const gchar *table)
{
	g_autoptr(OrmInspector) inspector = NULL;
	g_autoptr(GError) error = NULL;
	gboolean exists;

	inspector = orm_inspector_new(venture_database_get_connection(database), &error);
	g_assert_no_error(error);
	exists = orm_inspector_has_table(inspector, table, NULL, &error);
	g_assert_no_error(error);
	return exists;
}

/*
 * Products and inventory items as an install from before categories and
 * locations were records left them: free text, with the untidiness real
 * text has -- stray spaces, a duplicate, a deleted row, a subcategory with
 * no category, a blank and a NULL, and a second organization using the
 * same names. Plain columns only, so the same rows seed SQLite and
 * PostgreSQL.
 */
static const gchar taxonomy_rows[] =
	"INSERT INTO products (uuid, organization_id, created_at, updated_at, version, venture_id, name, category, subcategory, deleted_at) VALUES "
	"('tax-p1', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Peacebloom', 'Materials', 'Herbs', NULL),"
	"('tax-p2', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Copper ore', ' Materials ', 'Ore', NULL),"
	"('tax-p3', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Linen', 'Materials', NULL, NULL),"
	"('tax-p4', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Old potion', 'Crafted', 'Potions', '2026-02-01T00:00:00Z'),"
	"('tax-p5', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Stray', '', 'Orphan', NULL),"
	"('tax-p6', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Silverleaf', 'Materials', 'Herbs', NULL),"
	"('tax-p7', 2, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 2, 'Other shop herb', 'Materials', 'Herbs', NULL);"
	"INSERT INTO inventory_items (uuid, organization_id, created_at, updated_at, version, product_id, location) VALUES "
	"('tax-i1', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Bank'),"
	"('tax-i2', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 2, ' Bank'),"
	"('tax-i3', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 3, 'Alt 1'),"
	"('tax-i4', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 4, ''),"
	"('tax-i5', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 5, NULL),"
	"('tax-i6', 2, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 7, 'Bank')";

/*
 * What 000695 and 000700 make of taxonomy_rows, on either backend. If this
 * regresses, an upgraded install either loses the grouping its products
 * had (no category_id) or merges two organizations' trees into one.
 */
static void
check_taxonomy_backfill(VentureDatabase *database)
{
	static const struct {
		const gchar *sql;
		const gchar *expected;
	} checks[] = {
		/* Materials, Crafted, Herbs, Ore, Potions; Materials and Herbs again for org 2. */
		{ "SELECT CAST(COUNT(*) AS TEXT) FROM categories", "7" },
		{ "SELECT CAST(COUNT(DISTINCT uuid) AS TEXT) FROM categories", "7" },
		{ "SELECT CAST(COUNT(*) AS TEXT) FROM categories WHERE applies_to = 'product' AND version = 1 AND created_at IS NOT NULL AND deleted_at IS NULL", "7" },
		{ "SELECT CAST(COUNT(*) AS TEXT) FROM categories WHERE name = 'Orphan'", "0" },
		{ "SELECT c.name || '<' || p.name FROM products x JOIN categories c ON c.id = x.category_id JOIN categories p ON p.id = c.parent_id WHERE x.uuid = 'tax-p1'", "Herbs<Materials" },
		{ "SELECT c.name || '<' || p.name FROM products x JOIN categories c ON c.id = x.category_id JOIN categories p ON p.id = c.parent_id WHERE x.uuid = 'tax-p2'", "Ore<Materials" },
		{ "SELECT c.name || '<' || p.name FROM products x JOIN categories c ON c.id = x.category_id JOIN categories p ON p.id = c.parent_id WHERE x.uuid = 'tax-p4'", "Potions<Crafted" },
		{ "SELECT c.name || ':' || CAST(COALESCE(c.parent_id, 0) AS TEXT) FROM products x JOIN categories c ON c.id = x.category_id WHERE x.uuid = 'tax-p3'", "Materials:0" },
		{ "SELECT CAST(COALESCE(category_id, 0) AS TEXT) FROM products WHERE uuid = 'tax-p5'", "0" },
		{ "SELECT CAST(COUNT(DISTINCT category_id) AS TEXT) FROM products WHERE uuid IN ('tax-p1', 'tax-p6')", "1" },
		/* The same names in another organization are its own tree. */
		{ "SELECT CAST(c.organization_id AS TEXT) || CAST(p.organization_id AS TEXT) FROM products x JOIN categories c ON c.id = x.category_id JOIN categories p ON p.id = c.parent_id WHERE x.uuid = 'tax-p7'", "22" },
		/* The text the operator typed is left exactly as it was. */
		{ "SELECT category FROM products WHERE uuid = 'tax-p2'", " Materials " },
		/* Bank and Alt 1 for org 1, Bank for org 2. */
		{ "SELECT CAST(COUNT(*) AS TEXT) FROM locations", "3" },
		{ "SELECT CAST(COUNT(*) AS TEXT) FROM locations WHERE version = 1 AND COALESCE(parent_id, 0) = 0 AND deleted_at IS NULL", "3" },
		{ "SELECT CAST(COUNT(DISTINCT location_id) AS TEXT) FROM inventory_items WHERE uuid IN ('tax-i1', 'tax-i2')", "1" },
		{ "SELECT l.name FROM inventory_items i JOIN locations l ON l.id = i.location_id WHERE i.uuid = 'tax-i3'", "Alt 1" },
		{ "SELECT CAST(COALESCE(location_id, 0) AS TEXT) FROM inventory_items WHERE uuid = 'tax-i4'", "0" },
		{ "SELECT CAST(COALESCE(location_id, 0) AS TEXT) FROM inventory_items WHERE uuid = 'tax-i5'", "0" },
		{ "SELECT CAST(l.organization_id AS TEXT) FROM inventory_items i JOIN locations l ON l.id = i.location_id WHERE i.uuid = 'tax-i6'", "2" },
		{ "SELECT location FROM inventory_items WHERE uuid = 'tax-i2'", " Bank" },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(checks); i++)
	{
		g_autofree gchar *value = query_text(database, checks[i].sql);

		if (0 != g_strcmp0(value, checks[i].expected))
			g_error("%s: expected \"%s\", got \"%s\"", checks[i].sql,
			        checks[i].expected, value);
	}
}

/*
 * API token names written into the audit log and the inbox before actors
 * were named by number are rewritten on upgrade: a name one token holds
 * becomes "API token #<id>", a name two share becomes "API token", and a
 * person's name is left alone. If this regresses, the names an owner gave
 * their tokens stay readable by every viewer in every old entry.
 */
static void
test_token_actor_names(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *unique = NULL, *shared = NULL, *person = NULL, *inbox = NULL, *expected = NULL, *id = NULL;
	g_autofree gchar *approver = NULL, *approving_person = NULL;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"INSERT INTO api_tokens (uuid, organization_id, created_at, updated_at, version, name, prefix, token_hash) "
		"VALUES ('00000000-0000-4000-8000-000000000001', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'deploy-bot', 'aaaaaaaa', 'x'),"
		" ('00000000-0000-4000-8000-000000000002', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'laptop', 'bbbbbbbb', 'y'),"
		" ('00000000-0000-4000-8000-000000000003', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'laptop', 'cccccccc', 'z');"
		"INSERT INTO audit_entries (uuid, organization_id, created_at, updated_at, version, action, actor, target_type, target_id, approved_by) "
		"VALUES ('00000000-0000-4000-8000-000000000011', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 0, 'token:deploy-bot', 'company', 1, 'token:laptop'),"
		" ('00000000-0000-4000-8000-000000000012', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 0, 'token:laptop', 'company', 1, NULL),"
		" ('00000000-0000-4000-8000-000000000013', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 0, 'owner', 'company', 1, 'owner'),"
		/* A person may be called anything; only the exact prefix is a
		 * token, on SQLite as on PostgreSQL. */
		" ('00000000-0000-4000-8000-000000000014', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 0, 'Token:laptop', 'company', 1, NULL);"
		"INSERT INTO notifications (uuid, organization_id, created_at, updated_at, version, user_id, title, actor) "
		"VALUES ('00000000-0000-4000-8000-000000000021', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'token:deploy-bot assigned you: Fix token:deploy-bot', 'token:deploy-bot'),"
		" ('00000000-0000-4000-8000-000000000022', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'token:laptop updated Human company', 'token:laptop');"
		"UPDATE audit_entries SET target_type = 'api_token', target_id = 999, "
		"target_label = 'historical private name', "
		"diff = '{\"name\":{\"from\":\"old private name\",\"to\":\"historical private name\"},\"active\":{\"from\":true,\"to\":false}}' "
		"WHERE uuid = '00000000-0000-4000-8000-000000000012';"
		"UPDATE audit_entries SET target_label = 'Human company', "
		"diff = '{\"name\":{\"from\":\"Old company\",\"to\":\"Human company\"}}' "
		"WHERE uuid = '00000000-0000-4000-8000-000000000013';"
		/* Everything from 640 on, not just the two: an install that had
		 * not reached 640 had not applied any later script either, and the
		 * migrator refuses an older script once a newer one is recorded. */
		"DELETE FROM schema_migrations WHERE version >= 640", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&database);

	/* A restart applies the scripts again, as an upgrade would. */
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	id = query_text(database, "SELECT CAST(id AS TEXT) FROM api_tokens WHERE name = 'deploy-bot'");
	expected = g_strdup_printf("API token #%s", id);
	unique = query_text(database, "SELECT actor FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000011'");
	shared = query_text(database, "SELECT actor FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000012'");
	person = query_text(database, "SELECT actor FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000013'");
	inbox = query_text(database, "SELECT actor FROM notifications WHERE uuid = '00000000-0000-4000-8000-000000000021'");
	g_assert_cmpstr(unique, ==, expected);
	g_assert_cmpstr(shared, ==, "API token");
	g_assert_cmpstr(person, ==, "owner");
	g_assert_cmpstr(inbox, ==, expected);
	{
		g_autofree gchar *title = query_text(database,
			"SELECT title FROM notifications WHERE uuid = '00000000-0000-4000-8000-000000000021'");
		g_autofree gchar *shared_title = query_text(database,
			"SELECT title FROM notifications WHERE uuid = '00000000-0000-4000-8000-000000000022'");
		g_autofree gchar *shared_inbox = query_text(database,
			"SELECT actor FROM notifications WHERE uuid = '00000000-0000-4000-8000-000000000022'");
		g_autofree gchar *capitalised = query_text(database,
			"SELECT actor FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000014'");
		g_autofree gchar *assigned = g_strdup_printf("%s assigned you: Fix token:deploy-bot", expected);

		/* The inbox title led with the same name. Only that leading
		 * actor is the token's; the ticket's own label is left as is. */
		g_assert_cmpstr(title, ==, assigned);
		g_assert_cmpstr(shared_title, ==, "API token updated Human company");
		g_assert_cmpstr(shared_inbox, ==, "API token");
		g_assert_cmpstr(capitalised, ==, "Token:laptop");
	}
	approver = query_text(database, "SELECT approved_by FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000011'");
	approving_person = query_text(database, "SELECT approved_by FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000013'");
	g_assert_cmpstr(approver, ==, "API token");
	g_assert_cmpstr(approving_person, ==, "owner");
	{
		g_autofree gchar *label = query_text(database,
			"SELECT target_label FROM audit_entries WHERE target_type = 'api_token'");
		g_autofree gchar *text = query_text(database,
			"SELECT diff FROM audit_entries WHERE target_type = 'api_token'");
		g_autofree gchar *unchanged = query_text(database,
			"SELECT diff FROM audit_entries WHERE target_label = 'Human company'");
		g_autoptr(JsonNode) diff = venture_json_parse(text, NULL);
		JsonObject *change = json_object_get_object_member(json_node_get_object(diff), "name");

		/* The token need not still exist. Preserve the change and other
		 * fields, while removing both historical names from its diff. */
		g_assert_cmpstr(label, ==, "API token #999");
		g_assert_null(strstr(text, "private name"));
		g_assert_true(json_object_get_boolean_member(change, "redacted"));
		g_assert_true(json_object_get_boolean_member(change, "changed"));
		g_assert_nonnull(json_object_get_member(json_node_get_object(diff), "active"));
		g_assert_nonnull(strstr(unchanged, "Human company"));
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
	}

	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

/* A downgrade or edited migration must fail before schema reconciliation
 * can recreate a missing application table. */
static void
test_history_refusal(gconstpointer data)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = venture_company_new();
	g_autofree gchar *drop = g_strdup_printf("DROP TABLE %s", venture_entity_get_table_name(VENTURE_ENTITY(company)));
	g_autofree gchar *lookup = g_strdup_printf("SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE name = '%s'",
		venture_entity_get_table_name(VENTURE_ENTITY(company)));
	const gchar *mutation = data;

	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database, mutation, NULL, &error));
	g_assert_true(venture_database_execute(database, drop, NULL, &error));
	g_assert_false(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	result = venture_database_query_raw(database,
		lookup, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
}

static gboolean
apply_batch(OrmConnection *connection, OrmDialect *dialect, GError **error)
{
	(void)dialect;
	return venture_migrations_execute_sql(connection,
		"CREATE TABLE migration_probe (value TEXT);"
		"INSERT INTO migration_probe VALUES ('quoted;semicolon');"
		"INSERT INTO missing_migration_table VALUES (1);", error);
}

/* The first successful statements must roll back with the failing one,
 * and an unsuccessful version must remain pending on the next attempt. */
static void
test_batch_rollback(void)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(OrmMigration) migration = NULL;
	g_autoptr(OrmMigrator) runner = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GArray) applied = NULL;
	g_autoptr(GArray) pending = NULL;
	g_autoptr(GError) error = NULL;

	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	migration = orm_migration_new_callback(1, "failing batch", "fixture batch v1", apply_batch, NULL);
	runner = orm_migrator_new(venture_database_get_connection(database), &migration, 1, &error);
	g_assert_no_error(error);
	g_assert_false(orm_migrator_up(runner, 0, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(orm_migrator_status(runner, &applied, &pending, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(applied->len, ==, 0);
	g_assert_cmpuint(pending->len, ==, 1);
	result = venture_database_query_raw(database,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE name = 'migration_probe'", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
	/* Repair the cause and retry exactly the same script. */
	g_assert_true(venture_database_execute(database, "CREATE TABLE missing_migration_table (value INTEGER)", NULL, &error));
	g_assert_true(orm_migrator_up(runner, 0, &error));
	g_assert_no_error(error);
	g_clear_object(&result);
	result = venture_database_query_raw(database, "SELECT value FROM migration_probe", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpstr(orm_row_get_string(orm_result_get_row(result), 0), ==, "quoted;semicolon");
}

static void
test_nested_refused(void)
{
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_begin(database, &error));
	g_assert_false(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	venture_database_rollback(database);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

/* Optional module migrations cannot create tables outside the type registry,
 * but a missing table must not hide malformed guards or SQL failures. */
static void
check_optional_table(VentureDatabase *database)
{
	OrmConnection *connection = venture_database_get_connection(database);
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = NULL;
	const gchar *sql = "-- requires-table: optional_migration_fixture\nUPDATE optional_migration_fixture SET value = 2";
	g_assert_true(venture_migrations_execute_sql(connection, sql, &error));
	g_assert_no_error(error);
	g_assert_false(venture_migrations_execute_sql(connection,
		"-- requires-table: bad-name\nSELECT 1", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_database_execute(database,
		"CREATE TABLE optional_migration_fixture (value BIGINT); INSERT INTO optional_migration_fixture VALUES (1)", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_migrations_execute_sql(connection, sql, &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(database, "SELECT value FROM optional_migration_fixture", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 2);
	g_clear_object(&result);
	g_assert_false(venture_migrations_execute_sql(connection,
		"-- requires-table: optional_migration_fixture\nUPDATE absent_migration_fixture SET value = 4", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_database_execute(database, "DROP TABLE optional_migration_fixture", NULL, &error));
	g_assert_no_error(error);
}

static void
test_optional_table(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	check_optional_table(database);
}

/* A disabled module's absent table has no historical rows to backfill. Its
 * applied guarded version must remain valid when the module is enabled later. */
static void
test_optional_module(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(OrmInspector) inspector = NULL;
	g_autoptr(OrmResult) result = NULL;
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_assert_no_error(error);
	venture_entity_registry_set_type_module(registry, "stripe_checkout", "stripe", FALSE);
	g_assert_true(venture_database_migrate(database, registry, &error));
	g_assert_no_error(error);
	inspector = orm_inspector_new(venture_database_get_connection(database), &error);
	g_assert_no_error(error);
	g_assert_false(orm_inspector_has_table(inspector, "stripe_checkouts", NULL, &error));
	g_assert_no_error(error);
	venture_entity_registry_set_type_module(registry, "stripe_checkout", "stripe", TRUE);
	g_assert_true(venture_database_migrate(database, registry, &error));
	g_assert_no_error(error);
	g_assert_true(orm_inspector_has_table(inspector, "stripe_checkouts", NULL, &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(database, "SELECT CAST(COUNT(*) AS BIGINT) FROM stripe_checkouts", NULL, &error);
	g_assert_no_error(error); g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
}

/* Opt-in PostgreSQL coverage uses a unique schema and never changes an
 * existing application's tables, even when the server is shared. */
static void
test_postgresql(void)
{
	const gchar *uri = g_getenv("VENTURE_TEST_MIGRATION_POSTGRES_URI");
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(OrmMigrator) runner = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *schema = NULL;
	g_autofree gchar *setup = NULL;
	g_autofree gchar *cleanup = NULL;

	if (uri == NULL)
	{
		g_test_skip("Set VENTURE_TEST_MIGRATION_POSTGRES_URI for a disposable PostgreSQL server");
		return;
	}
	uuid = g_uuid_string_random();
	g_strdelimit(uuid, "-", '_');
	schema = g_strconcat("venture_migration_", uuid, NULL);
	setup = g_strdup_printf("CREATE SCHEMA %s; SET search_path TO %s", schema, schema);
	cleanup = g_strdup_printf("DROP SCHEMA %s CASCADE", schema);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database, setup, NULL, &error));
	g_assert_no_error(error);
	check_optional_table(database);
	/* Startup reconciles generated tables before applying data migrations.
	 * Access backfills need the users/organizations/token schema present. */
	g_assert_true(venture_schema_create_all(venture_database_get_connection(database),
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"INSERT INTO users (id, uuid, username, role, active) VALUES (17, 'pg-owner', 'pg-owner', 'owner', TRUE);"
		"INSERT INTO organizations (id, uuid, name, slug, active) VALUES (31, 'pg-org', 'Legacy organization', 'pg-org', TRUE);"
		/* Token targets can outlive the token itself. Shared history must
		 * retain the event without retaining either of its private names. */
		"INSERT INTO audit_entries (uuid, organization_id, action, actor, target_type, target_id, target_label, diff) VALUES "
		"('pg-token-audit', 31, 'update', 'owner', 'api_token', 999, 'Private token name', "
		"'{\"name\":{\"from\":\"Old private name\",\"to\":\"Private token name\"},\"active\":{\"from\":true,\"to\":false}}'),"
		"('pg-company-audit', 31, 'update', 'owner', 'company', 888, 'Public company name', "
		"'{\"name\":{\"from\":\"Old public name\",\"to\":\"Public company name\"}}'),"
		/* Not JSON, on another type: 000685 must not cast it. Nor is a
		 * capitalised prefix a token's, here or on SQLite. */
		"('pg-plain-audit', 31, 'update', 'Token:pg-bot', 'company', 887, 'Plain', 'not json');"
		"INSERT INTO api_tokens (uuid, organization_id, name, prefix, token_hash) VALUES "
		"('pg-token', 31, 'pg-bot', 'pgpgpgpg', 'pg-hash');"
		"INSERT INTO notifications (uuid, organization_id, user_id, title, actor) VALUES "
		"('pg-note', 31, 17, 'token:pg-bot updated Public company name', 'token:pg-bot');"
		"INSERT INTO subscription_events (uuid, organization_id, subscription_id, kind, to_status, invoice_id, final_invoice_id) VALUES "
		"('pg-recurring-event', 31, 77, 'renewed', 'active', 701, 0),"
		"('pg-final-event', 31, 77, 'cancelled', 'cancelled', 0, 0);"
		"INSERT INTO billing_requests (uuid, organization_id, subscription_id, action, processed, invoice_id, at) VALUES "
		"('pg-recurring-request', 31, 77, 'renew', 1, 701, '2026-01-01T00:00:00Z'),"
		"('pg-final-request', 31, 77, 'cancel', 1, 702, '2026-01-15T00:00:00Z')", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database, taxonomy_rows, NULL, &error));
	g_assert_no_error(error);
	runner = venture_migrations_new(venture_database_get_connection(database), VENTURE_DATABASE_BACKEND_POSTGRES, &error);
	g_assert_no_error(error);
	g_assert_true(orm_migrator_up(runner, 0, &error));
	g_assert_no_error(error);
	g_assert_true(orm_migrator_up(runner, 0, &error));
	g_assert_no_error(error);
	{
		g_autofree gchar *label = query_text(database,
			"SELECT target_label FROM audit_entries WHERE uuid = 'pg-token-audit'");
		g_autofree gchar *text = query_text(database,
			"SELECT diff FROM audit_entries WHERE uuid = 'pg-token-audit'");
		g_autofree gchar *company = query_text(database,
			"SELECT diff FROM audit_entries WHERE uuid = 'pg-company-audit'");
		g_autoptr(JsonNode) diff = venture_json_parse(text, NULL);
		JsonObject *change = json_object_get_object_member(json_node_get_object(diff), "name");

		g_assert_cmpstr(label, ==, "API token #999");
		g_assert_null(strstr(text, "private"));
		g_assert_null(strstr(text, "Private"));
		g_assert_true(json_object_get_boolean_member(change, "redacted"));
		g_assert_true(json_object_get_boolean_member(change, "changed"));
		g_assert_nonnull(json_object_get_member(json_node_get_object(diff), "active"));
		g_assert_nonnull(strstr(company, "Public company name"));
	}
	{
		g_autofree gchar *id = query_text(database, "SELECT CAST(id AS TEXT) FROM api_tokens WHERE uuid = 'pg-token'");
		g_autofree gchar *title = query_text(database, "SELECT title FROM notifications WHERE uuid = 'pg-note'");
		g_autofree gchar *actor = query_text(database, "SELECT actor FROM notifications WHERE uuid = 'pg-note'");
		g_autofree gchar *plain = query_text(database, "SELECT actor FROM audit_entries WHERE uuid = 'pg-plain-audit'");
		g_autofree gchar *expected_actor = g_strdup_printf("API token #%s", id);
		g_autofree gchar *expected_title = g_strdup_printf("API token #%s updated Public company name", id);

		g_assert_cmpstr(actor, ==, expected_actor);
		g_assert_cmpstr(title, ==, expected_title);
		g_assert_cmpstr(plain, ==, "Token:pg-bot");
	}
	{
		g_autofree gchar *final_invoice = query_text(database,
			"SELECT CAST(final_invoice_id AS TEXT) FROM subscription_events WHERE uuid = 'pg-final-event'");
		g_autofree gchar *recurring_invoice = query_text(database,
			"SELECT CAST(final_invoice_id AS TEXT) FROM subscription_events WHERE uuid = 'pg-recurring-event'");

		/* A completed cancellation names its final usage invoice, not
		 * the recurring invoice already owned by a renewal event. */
		g_assert_cmpstr(final_invoice, ==, "702");
		g_assert_cmpstr(recurring_invoice, ==, "0");
	}
	check_taxonomy_backfill(database);
	result = venture_database_query_raw(database,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM organization_memberships WHERE user_id = 17 AND organization_id = 31 AND role = 'owner' AND active", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
	g_clear_object(&result);
	result = venture_database_query_raw(database, "SELECT CAST(COUNT(*) AS BIGINT) FROM venture_schema_version", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
	g_clear_object(&result);
	g_clear_object(&runner);
	g_assert_true(venture_database_execute(database, cleanup, NULL, &error));
	g_assert_no_error(error);
}

/* A missing backend or duplicate version must fail at build time rather
 * than silently shipping a binary with an incomplete migration history. */
static void
test_generator(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migration-generator-XXXXXX", NULL);
	g_autofree gchar *sqlite = g_build_filename(directory, "sqlite", NULL);
	g_autofree gchar *postgres = g_build_filename(directory, "postgresql", NULL);
	g_autofree gchar *first = g_build_filename(sqlite, "000001_first.sql", NULL);
	g_autofree gchar *pair = g_build_filename(postgres, "000001_first.sql", NULL);
	g_autofree gchar *duplicate = g_build_filename(sqlite, "000001_duplicate.sql", NULL);
	g_autofree gchar *duplicate_pair = g_build_filename(postgres, "000001_duplicate.sql", NULL);
	g_autoptr(GError) error = NULL;
	guint attempt;

	g_assert_cmpint(g_mkdir_with_parents(sqlite, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(postgres, 0700), ==, 0);
	g_assert_true(g_file_set_contents(first, "SELECT 1;\n", -1, &error));
	for (attempt = 0; attempt < 3; attempt++)
	{
		g_autoptr(GSubprocess) process = NULL;
		g_autofree gchar *output = NULL;
		g_autofree gchar *diagnostic = NULL;
		if (attempt == 1)
			g_assert_true(g_file_set_contents(pair, "SELECT 1;\n", -1, &error));
		if (attempt == 2)
		{
			g_assert_true(g_file_set_contents(duplicate, "SELECT 2;\n", -1, &error));
			g_assert_true(g_file_set_contents(duplicate_pair, "SELECT 2;\n", -1, &error));
		}
		process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
			&error, "bash", "tools/venture-migrations.sh", directory, NULL);
		g_assert_no_error(error);
		g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &output, &diagnostic, &error));
		g_assert_no_error(error);
		g_assert_cmpint(g_subprocess_get_successful(process), ==, attempt == 1);
		if (attempt == 1)
		{
			g_assert_nonnull(strstr(output, "venture_migrations_sqlite"));
			g_assert_nonnull(strstr(output, "venture_migrations_postgresql"));
		}
		else
			g_assert_cmpstr(diagnostic, !=, "");
	}
	venture_test_remove_tree(directory);
}

/*
 * The quote-subscription and metered-usage scripts on an upgraded
 * database: a price and a subscription from before read back flat and
 * unlinked -- nothing is invented for them -- a restart is a no-op, and
 * with billing and quotes switched off, when none of their tables exist,
 * the scripts still apply. If this regresses, an upgrade fabricates usage
 * or quote links for old rows, or an install without billing cannot start.
 */
static void
test_quote_subscriptions_and_usage(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureEntity) price = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *applied = NULL, *linked = NULL;
	guint run;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"INSERT INTO plans (uuid, organization_id, created_at, updated_at, version, name, code, active) "
		"VALUES ('00000000-0000-4000-8000-000000000031', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'Old', 'old', 1);"
		"INSERT INTO plan_prices (uuid, organization_id, created_at, updated_at, version, plan_id, currency, interval, "
		"amount_amount, amount_currency, amount_exponent, active) "
		"VALUES ('00000000-0000-4000-8000-000000000032', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, "
		"(SELECT id FROM plans WHERE code = 'old'), 'USD', 0, 3000, 'USD', 2, 1);"
		"DELETE FROM schema_migrations WHERE version >= 670", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&database);
	for (run = 0; run < 2; run++)
	{
		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		g_clear_pointer(&applied, g_free);
		applied = query_text(database, "SELECT CAST(COUNT(*) AS TEXT) FROM schema_migrations WHERE version IN (670, 675, 678)");
		g_assert_cmpstr(applied, ==, "3");
		g_clear_object(&database);
	}
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PLAN_PRICE);
		price = venture_database_find_one(database, query, &error);
		g_assert_no_error(error);
	}
	g_assert_false(venture_plan_price_is_metered(VENTURE_PLAN_PRICE(price)));
	linked = query_text(database, "SELECT CAST(COUNT(*) AS TEXT) FROM usage_records");
	g_assert_cmpstr(linked, ==, "0");
	g_clear_object(&database);
	venture_test_remove_tree(directory);

	/* No billing, no quotes: their tables are absent and the scripts pass. */
	{
		g_autoptr(VentureConfig) config = venture_config_new();
		g_autoptr(VentureContext) context = NULL;
		g_autoptr(VentureDatabase) bare = NULL;
		venture_config_set_module_enabled(config, "billing", FALSE);
		venture_config_set_module_enabled(config, "quotes", FALSE);
		bare = venture_database_new("sqlite://:memory:", &error);
		g_assert_no_error(error);
		context = venture_context_new(config, bare);
		g_assert_true(venture_database_migrate(bare, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		/* Otherwise this passes just as well with both modules on. */
		g_assert_false(table_exists(bare, "plan_prices"));
		g_assert_false(table_exists(bare, "quote_lines"));
		/* While the context lives: it is what lifts the registry mask,
		 * which would otherwise hide billing from every later test. */
		venture_config_set_module_enabled(config, "billing", TRUE);
		venture_config_set_module_enabled(config, "quotes", TRUE);
		g_clear_object(&context);
	}
}

/*
 * An install that used billing and Stripe and then switched both off
 * still holds their tables, and startup must still succeed. The field
 * tables of a hidden type are what gain final_invoice_id and
 * method_label; if those tables were left behind, 000678's and 000690's
 * UPDATEs would name a column that does not exist and the server would
 * refuse to start. Worse, recording the scripts as skipped would mean the
 * final-invoice backfill never ran once billing came back on.
 */
static void
test_disabled_module_upgrade(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *final_invoice = NULL, *recurring_invoice = NULL, *labels = NULL;
	gboolean ok;

	/* Stripe is off by default, so its tables need asking for. */
	g_object_set(config, "stripe-enabled", TRUE, NULL);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	/* The shape of a database from before 678: neither column, and
	 * neither script recorded. */
	ok = venture_database_execute(database,
		"INSERT INTO subscription_events (uuid, organization_id, created_at, updated_at, version, "
		"subscription_id, kind, to_status, invoice_id) VALUES "
		"('00000000-0000-4000-8000-000000000041', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 77, 'renewed', 'active', 701),"
		" ('00000000-0000-4000-8000-000000000042', 1, '2026-01-15T00:00:00Z', '2026-01-15T00:00:00Z', 1, 77, 'cancelled', 'cancelled', 0);"
		"INSERT INTO billing_requests (uuid, organization_id, created_at, updated_at, version, "
		"subscription_id, action, processed, invoice_id, at) VALUES "
		"('00000000-0000-4000-8000-000000000043', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 77, 'renew', 1, 701, '2026-01-01T00:00:00Z'),"
		" ('00000000-0000-4000-8000-000000000044', 1, '2026-01-15T00:00:00Z', '2026-01-15T00:00:00Z', 1, 77, 'cancel', 1, 702, '2026-01-15T00:00:00Z');"
		"DROP INDEX IF EXISTS idx_subscription_events_final_invoice_id;"
		"ALTER TABLE subscription_events DROP COLUMN final_invoice_id;"
		"ALTER TABLE stripe_authorizations DROP COLUMN method_label;"
		"DELETE FROM schema_migrations WHERE version >= 678", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	g_clear_object(&context);
	g_clear_object(&database);

	/* Upgrade with both modules off. */
	venture_config_set_module_enabled(config, "billing", FALSE);
	g_object_set(config, "stripe-enabled", FALSE, NULL);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"subscription_event"), ==, G_TYPE_INVALID);
	ok = venture_database_migrate(database, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	/* The backfill ran now, from the rows that were there: the one
	 * processed cancellation whose invoice no event already owns. */
	final_invoice = query_text(database,
		"SELECT CAST(final_invoice_id AS TEXT) FROM subscription_events WHERE uuid = '00000000-0000-4000-8000-000000000042'");
	recurring_invoice = query_text(database,
		"SELECT CAST(COALESCE(final_invoice_id, 0) AS TEXT) FROM subscription_events WHERE uuid = '00000000-0000-4000-8000-000000000041'");
	g_assert_cmpstr(final_invoice, ==, "702");
	g_assert_cmpstr(recurring_invoice, ==, "0");
	labels = query_text(database,
		"SELECT CAST(COUNT(*) AS TEXT) FROM pragma_table_info('stripe_authorizations') WHERE name = 'method_label'");
	g_assert_cmpstr(labels, ==, "1");
	/* Put back what the defaults say, while the context can lift it. */
	venture_config_set_module_enabled(config, "billing", TRUE);
	g_clear_object(&context);
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

/*
 * An install upgrading into categories and locations: the rows are there
 * before the scripts run, as they would be, and a restart runs nothing
 * twice. The rows the scripts insert must be whole records -- read back
 * through the ORM and walked into a path -- not just rows SQL can count.
 */
static void
test_taxonomy_backfill(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	guint run;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database, taxonomy_rows, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"DELETE FROM schema_migrations WHERE version >= 695", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&database);

	for (run = 0; run < 2; run++)
	{
		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		check_taxonomy_backfill(database);
		g_clear_object(&database);
	}

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	{
		g_autofree gchar *id_text = query_text(database,
			"SELECT CAST(category_id AS TEXT) FROM products WHERE uuid = 'tax-p1'");
		g_autofree gchar *location_text = query_text(database,
			"SELECT CAST(location_id AS TEXT) FROM inventory_items WHERE uuid = 'tax-i3'");
		g_autofree gchar *path = NULL;
		g_autoptr(VentureEntity) location = NULL;
		gboolean active = FALSE;

		path = venture_category_path(database, VENTURE_TYPE_CATEGORY,
			g_ascii_strtoll(id_text, NULL, 10), &error);
		g_assert_no_error(error);
		g_assert_cmpstr(path, ==, "Materials / Herbs");
		location = venture_database_get(database, VENTURE_TYPE_LOCATION,
			g_ascii_strtoll(location_text, NULL, 10), &error);
		g_assert_no_error(error);
		g_assert_nonnull(location);
		g_object_get(location, "active", &active, NULL);
		g_assert_true(active);
		g_assert_nonnull(venture_entity_get_uuid(location));
	}
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

/*
 * The same upgrade with sales switched off. products and inventory_items
 * are still there -- sales was used, then turned off -- but locations has
 * never been created, because reconciliation never creates a hidden
 * type's table. If 000695 assumed that table, startup would fail; if the
 * guard skipped it, the backfill would be recorded as done and never run
 * once sales came back. Categories are core, so their table is there
 * either way.
 */
static void
test_taxonomy_sales_off(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	/* The shape of a database from before 695: no reference columns, no
	 * locations table, neither script recorded. */
	g_assert_true(venture_database_execute(database, taxonomy_rows, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"DROP INDEX IF EXISTS idx_products_category_id;"
		"ALTER TABLE products DROP COLUMN category_id;"
		"DROP INDEX IF EXISTS idx_inventory_items_location_id;"
		"ALTER TABLE inventory_items DROP COLUMN location_id;"
		"DROP TABLE locations;"
		"DELETE FROM schema_migrations WHERE version >= 695", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&context);
	g_clear_object(&database);

	venture_config_set_module_enabled(config, "sales", FALSE);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	/* Finance and everything above it need sales; resolving says so and
	 * switches them off too, which is exactly the install being tested. */
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*requires \"sales\"*");
	context = venture_context_new(config, database);
	g_test_assert_expected_messages();
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"location"), ==, G_TYPE_INVALID);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(table_exists(database, "locations"));
	check_taxonomy_backfill(database);

	/* Back on: the table the script made is the table sales uses. */
	venture_config_set_module_enabled(config, "sales", TRUE);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LOCATION);
		g_autoptr(GPtrArray) rows = NULL;

		venture_query_set_limit(query, 0);
		rows = venture_database_find(database, query, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(rows->len, ==, 3);
	}
	g_clear_object(&context);
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

/*
 * A currency defined before book treatment existed has a NULL column once
 * reconciliation adds it; 000705 makes it valued, which is what every
 * currency behaved as, and a restart runs nothing twice. If this regresses
 * an upgraded currency reads a treatment nobody chose -- or the column
 * holds a value the ledger does not know.
 */
static void
test_currency_book_treatment(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	guint run;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"INSERT INTO currencies (uuid, organization_id, created_at, updated_at, version, code, name, exponent) "
		"VALUES ('cur-1', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'TICKET', 'Ticket', 0);"
		"UPDATE currencies SET book_treatment = NULL;"
		"DELETE FROM schema_migrations WHERE version >= 705", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&database);
	for (run = 0; run < 2; run++)
	{
		g_autofree gchar *nulls = NULL;
		g_autofree gchar *value = NULL;

		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		nulls = query_text(database,
			"SELECT CAST(COUNT(*) AS TEXT) FROM currencies WHERE book_treatment IS NULL");
		g_assert_cmpstr(nulls, ==, "0");
		value = query_text(database,
			"SELECT CAST(book_treatment AS TEXT) FROM currencies WHERE code = 'TICKET'");
		g_assert_cmpstr(value, ==, "0");
		g_clear_object(&database);
	}
	venture_test_remove_tree(directory);
}

/*
 * An account written before holdings existed has a NULL allow_negative
 * once reconciliation adds the column; 000710 makes it FALSE, the safe
 * value, and a restart runs nothing twice. It has no location either, so
 * the floor never judges it. If this regresses the column holds a value
 * the ledger does not read, or a restart fails on the history.
 */
static void
test_account_holdings(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	guint run;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"INSERT INTO accounts (uuid, organization_id, created_at, updated_at, version, code, name, kind) "
		"VALUES ('acct-1', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 'OLD-1', 'Cash', 0);"
		"UPDATE accounts SET allow_negative = NULL, location_id = NULL;"
		"DELETE FROM schema_migrations WHERE version >= 710", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&database);
	for (run = 0; run < 2; run++)
	{
		g_autofree gchar *nulls = NULL;
		g_autofree gchar *value = NULL;

		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		nulls = query_text(database,
			"SELECT CAST(COUNT(*) AS TEXT) FROM accounts WHERE allow_negative IS NULL");
		g_assert_cmpstr(nulls, ==, "0");
		value = query_text(database,
			"SELECT CAST(allow_negative AS TEXT) FROM accounts WHERE code = 'OLD-1'");
		g_assert_cmpstr(value, ==, "0");
		g_clear_object(&database);
	}
	venture_test_remove_tree(directory);
}

/*
 * 000710 where the accounts table is absent: accounts belong to the
 * finance module, and reconciliation never creates a hidden type's table.
 * The script declares the one table it backfills, so the runner records it
 * as done instead of failing on UPDATE accounts -- an absent table held no
 * rows to backfill -- and a table that arrives later gets the column's
 * FALSE default from its field table. Run through the migrator itself:
 * startup's seeds after it are a separate matter. If this regresses, the
 * upgrade refuses on a database whose books were never kept.
 */
static void
test_account_holdings_without_accounts(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(OrmMigrator) runner = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *recorded = NULL;
	gboolean migrated;

	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"DROP TABLE accounts;"
		"DELETE FROM schema_migrations WHERE version >= 710", NULL, &error));
	g_assert_no_error(error);

	runner = venture_migrations_new(venture_database_get_connection(database),
		venture_database_get_backend(database), &error);
	g_assert_no_error(error);
	migrated = orm_migrator_up(runner, 0, &error);
	g_assert_no_error(error);
	g_assert_true(migrated);
	g_assert_false(table_exists(database, "accounts"));
	recorded = query_text(database,
		"SELECT CAST(COUNT(*) AS TEXT) FROM schema_migrations WHERE version = 710");
	g_assert_cmpstr(recorded, ==, "1");
	g_clear_object(&runner);
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

/*
 * A fresh install that never had finance on: accounts and tax_categories
 * are finance's, and reconciliation never creates a hidden type's table,
 * so the seeds after the migrations must skip what is not there instead
 * of counting rows in a table that does not exist. Then the same database
 * with finance switched on: reconciliation creates the tables whole and
 * the next start seeds the chart of accounts and the tax categories, as a
 * fresh install with finance on would have had them. If this regresses, a
 * server configured without the books refuses to start at all -- or,
 * with the seed skipped for good, the books arrive empty.
 */
static void
test_fresh_without_finance(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	gboolean migrated;

	venture_config_set_module_enabled(config, "finance", FALSE);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	/* Everything built on finance goes with it, and resolving says so. */
	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING, "*requires \"finance\"*");
	context = venture_context_new(config, database);
	g_test_assert_expected_messages();
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"account"), ==, G_TYPE_INVALID);
	migrated = venture_database_migrate(database, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(migrated);
	g_assert_false(table_exists(database, "accounts"));
	g_assert_false(table_exists(database, "tax_categories"));
	/* A restart with finance still off is the same start. */
	migrated = venture_database_migrate(database, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(migrated);

	venture_config_set_module_enabled(config, "finance", TRUE);
	migrated = venture_database_migrate(database, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(migrated);
	{
		g_autofree gchar *accounts = query_text(database,
			"SELECT CAST(COUNT(*) AS TEXT) FROM accounts WHERE code IN ('1000', '4000', '7600')");
		g_autofree gchar *categories = query_text(database,
			"SELECT CAST(COUNT(*) AS TEXT) FROM tax_categories WHERE code = 'MEALS'");

		g_assert_cmpstr(accounts, ==, "3");
		g_assert_cmpstr(categories, ==, "1");
	}
	g_clear_object(&context);
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

/* How many times @version is recorded as applied. */
static gint64
applied(VentureDatabase *database, gint64 version)
{
	g_autofree gchar *sql = g_strdup_printf(
		"SELECT CAST(COUNT(*) AS TEXT) FROM schema_migrations WHERE version = %" G_GINT64_FORMAT, version);
	g_autofree gchar *text = query_text(database, sql);

	return g_ascii_strtoll(text, NULL, 10);
}

/*
 * 000711 and 000720 pin what the forms module relies on: a question's key unique per
 * form, deleted questions included, and the columns the public door reads.
 * With forms switched off the tables are absent and the script passes and
 * is recorded; switched on later, reconciliation makes the tables with the
 * index, which is why the script need not. A schema missing the index --
 * a restore from somewhere else, a hand edit -- is refused, not trusted.
 */
static void
test_forms_schema(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-migrations-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autofree gchar *script = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	guint run;
	gboolean migrated;

	/* Forms off: nothing to check, and it is recorded as done. */
	venture_config_set_module_enabled(config, "forms", FALSE);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	migrated = venture_database_migrate(database, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(migrated);
	g_assert_false(table_exists(database, "form_fields"));
	g_assert_false(table_exists(database, "form_versions"));
	g_assert_false(table_exists(database, "form_drafts"));
	g_assert_false(table_exists(database, "form_rules"));
	g_assert_false(table_exists(database, "form_groups"));
	g_assert_false(table_exists(database, "form_pendings"));
	g_assert_cmpint(applied(database, 711), ==, 1);
	g_assert_cmpint(applied(database, 720), ==, 1);
	g_assert_cmpint(applied(database, 730), ==, 1);
	g_assert_cmpint(applied(database, 740), ==, 1);
	g_assert_cmpint(applied(database, 741), ==, 1);
	g_assert_cmpint(applied(database, 750), ==, 1);
	g_assert_cmpint(applied(database, 760), ==, 1);
	g_assert_cmpint(applied(database, 770), ==, 1);
	g_assert_cmpint(applied(database, 780), ==, 1);
	g_assert_cmpint(applied(database, 790), ==, 1);
	g_assert_cmpint(applied(database, 791), ==, 1);
	g_assert_cmpint(applied(database, 792), ==, 1);
	g_assert_cmpint(applied(database, 800), ==, 1);
	g_clear_object(&context);
	g_clear_object(&database);

	/* On, twice: the tables and the index arrive, nothing runs again. */
	venture_config_set_module_enabled(config, "forms", TRUE);
	for (run = 0; run < 2; run++)
	{
		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		context = venture_context_new(config, database);
		migrated = venture_database_migrate(database, venture_entity_registry_get_default(), &error);
		g_assert_no_error(error);
		g_assert_true(migrated);
		g_assert_true(table_exists(database, "form_fields"));
		g_assert_true(table_exists(database, "form_versions"));
		g_assert_true(table_exists(database, "form_drafts"));
		g_assert_true(table_exists(database, "form_rules"));
		g_assert_true(table_exists(database, "form_groups"));
		g_assert_true(table_exists(database, "form_pendings"));
		g_assert_cmpint(applied(database, 711), ==, 1);
		g_assert_cmpint(applied(database, 720), ==, 1);
		g_assert_cmpint(applied(database, 730), ==, 1);
		g_assert_cmpint(applied(database, 740), ==, 1);
		g_assert_cmpint(applied(database, 741), ==, 1);
		g_assert_cmpint(applied(database, 750), ==, 1);
		g_assert_cmpint(applied(database, 760), ==, 1);
		g_assert_cmpint(applied(database, 770), ==, 1);
		g_assert_cmpint(applied(database, 780), ==, 1);
		g_assert_cmpint(applied(database, 790), ==, 1);
		g_assert_cmpint(applied(database, 791), ==, 1);
		g_assert_cmpint(applied(database, 792), ==, 1);
		g_assert_cmpint(applied(database, 800), ==, 1);
		{
			g_autofree gchar *index = query_text(database,
				"SELECT CAST(COUNT(*) AS TEXT) FROM sqlite_master WHERE type = 'index' "
				"AND name = 'uq_form_fields_organization_form_id_key'");

			g_assert_cmpstr(index, ==, "1");
		}
		g_clear_object(&context);
		g_clear_object(&database);
	}

	/* The script itself refuses a schema without the index. */
	g_assert_true(g_file_get_contents("migrations/sqlite/000711_forms.sql", &script, NULL, &error));
	g_assert_no_error(error);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database, script, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"DROP INDEX uq_form_fields_organization_form_id_key", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_execute(database, script, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_pointer(&script, g_free);

	/* 000720 likewise refuses responses that cannot say their version. */
	g_assert_true(g_file_get_contents("migrations/sqlite/000720_form_versions.sql", &script, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database, script, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_execute(database,
		"DROP INDEX IF EXISTS idx_form_submissions_version_id;"
		"ALTER TABLE form_submissions DROP COLUMN version_id", NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_database_execute(database, script, NULL, &error));
	g_assert_nonnull(error);
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/migrations/generator", test_generator);
	g_test_add_func("/migrations/optional-table", test_optional_table);
	g_test_add_func("/migrations/optional-module", test_optional_module);
	g_test_add_func("/migrations/postgresql", test_postgresql);
	g_test_add_func("/migrations/upgrade-restart", test_upgrade_restart);
	g_test_add_func("/migrations/token-actor-names", test_token_actor_names);
	g_test_add_func("/migrations/quote-subscriptions-and-usage", test_quote_subscriptions_and_usage);
	g_test_add_func("/migrations/disabled-module-upgrade", test_disabled_module_upgrade);
	g_test_add_func("/migrations/taxonomy-backfill", test_taxonomy_backfill);
	g_test_add_func("/migrations/taxonomy-sales-off", test_taxonomy_sales_off);
	g_test_add_func("/migrations/account-holdings", test_account_holdings);
	g_test_add_func("/migrations/account-holdings-without-accounts",
	                test_account_holdings_without_accounts);
	g_test_add_func("/migrations/fresh-without-finance", test_fresh_without_finance);
	g_test_add_func("/migrations/currency-book-treatment", test_currency_book_treatment);
	g_test_add_data_func("/migrations/checksum", "UPDATE schema_migrations SET checksum = 'changed'", test_history_refusal);
	g_test_add_data_func("/migrations/unknown-version", "UPDATE schema_migrations SET version = 999999 WHERE version = 1", test_history_refusal);
	g_test_add_func("/migrations/batch-rollback-retry", test_batch_rollback);
	g_test_add_func("/migrations/nested-refused", test_nested_refused);
	g_test_add_func("/migrations/forms-schema", test_forms_schema);
	return g_test_run();
}
