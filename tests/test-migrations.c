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
		" ('00000000-0000-4000-8000-000000000013', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 0, 'owner', 'company', 1, 'owner');"
		"INSERT INTO notifications (uuid, organization_id, created_at, updated_at, version, user_id, title, actor) "
		"VALUES ('00000000-0000-4000-8000-000000000021', 1, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', 1, 1, 'Changed', 'token:deploy-bot');"
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
	approver = query_text(database, "SELECT approved_by FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000011'");
	approving_person = query_text(database, "SELECT approved_by FROM audit_entries WHERE uuid = '00000000-0000-4000-8000-000000000013'");
	g_assert_cmpstr(approver, ==, "API token");
	g_assert_cmpstr(approving_person, ==, "owner");
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
		"INSERT INTO organizations (id, uuid, name, slug, active) VALUES (31, 'pg-org', 'Legacy organization', 'pg-org', TRUE)", NULL, &error));
	g_assert_no_error(error);
	runner = venture_migrations_new(venture_database_get_connection(database), VENTURE_DATABASE_BACKEND_POSTGRES, &error);
	g_assert_no_error(error);
	g_assert_true(orm_migrator_up(runner, 0, &error));
	g_assert_no_error(error);
	g_assert_true(orm_migrator_up(runner, 0, &error));
	g_assert_no_error(error);
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
		g_clear_object(&context);
		venture_config_set_module_enabled(config, "billing", TRUE);
		venture_config_set_module_enabled(config, "quotes", TRUE);
	}
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
	g_test_add_data_func("/migrations/checksum", "UPDATE schema_migrations SET checksum = 'changed'", test_history_refusal);
	g_test_add_data_func("/migrations/unknown-version", "UPDATE schema_migrations SET version = 999999 WHERE version = 1", test_history_refusal);
	g_test_add_func("/migrations/batch-rollback-retry", test_batch_rollback);
	g_test_add_func("/migrations/nested-refused", test_nested_refused);
	return g_test_run();
}
