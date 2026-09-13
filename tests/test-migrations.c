/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <glib/gstdio.h>
#include "db/venture-migrations.h"
#include "venture-test-util.h"

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
		g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), >=, 2);
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
	runner = venture_migrations_new(venture_database_get_connection(database), VENTURE_DATABASE_BACKEND_POSTGRES, &error);
	g_assert_no_error(error);
	g_assert_true(orm_migrator_up(runner, 0, &error));
	g_assert_no_error(error);
	g_assert_true(orm_migrator_up(runner, 0, &error));
	g_assert_no_error(error);
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/migrations/generator", test_generator);
	g_test_add_func("/migrations/postgresql", test_postgresql);
	g_test_add_func("/migrations/upgrade-restart", test_upgrade_restart);
	g_test_add_data_func("/migrations/checksum", "UPDATE schema_migrations SET checksum = 'changed'", test_history_refusal);
	g_test_add_data_func("/migrations/unknown-version", "UPDATE schema_migrations SET version = 999999 WHERE version = 1", test_history_refusal);
	g_test_add_func("/migrations/batch-rollback-retry", test_batch_rollback);
	g_test_add_func("/migrations/nested-refused", test_nested_refused);
	return g_test_run();
}
