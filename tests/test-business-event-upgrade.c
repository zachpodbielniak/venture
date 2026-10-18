/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

static gint64
scalar(VentureDatabase *database, const gchar *sql)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = venture_database_query_raw(database, sql, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_true(orm_result_next(result));
	return orm_row_get_integer(orm_result_get_row(result), 0);
}

static void
test_upgrade(void)
{
	const gchar *external = g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI");
	g_autofree gchar *directory = g_dir_make_tmp("venture-event-upgrade-XXXXXX", NULL);
	g_autofree gchar *uri = external ? g_strdup(external) : g_strdup_printf("sqlite://%s/events.db", directory);
	g_autoptr(VentureDatabase) database = external ? venture_test_accounting_database(NULL) : venture_database_new(uri, NULL);
	g_autofree gchar *schema = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(VentureWebhook) hook = NULL;
	g_autoptr(GError) error = NULL;
	gint64 organization;
	guint run;
	g_assert_nonnull(database);
	schema = g_strdup(g_object_get_data(G_OBJECT(database), "accounting-test-schema"));
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	organization = venture_context_get_default_organization_id(context);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Historical inquiry", "email", "reader@example.test", NULL);
	hook = g_object_new(VENTURE_TYPE_WEBHOOK, "organization-id", organization, "name", "Existing receiver", "url", "https://receiver.example.test", "active", FALSE, NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(hook), NULL, &error));
	g_assert_no_error(error);
	/* Reconstruct the preceding schema, preserving the historical source and
	 * subscription. An upgrade must not invent business events for old data. */
	g_assert_true(venture_database_execute(database,
		"DROP TABLE business_event_deliveries; DROP TABLE business_events;"
		"ALTER TABLE webhooks DROP COLUMN business_events;"
		"DELETE FROM schema_migrations WHERE version IN (919, 920)", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&context); g_clear_object(&database);
	for (run = 0; run < 2; run++)
	{
		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		if (schema != NULL)
		{
			g_autofree gchar *sql = g_strdup_printf("SET search_path TO %s", schema);
			g_assert_true(venture_database_execute(database, sql, NULL, &error));
			g_assert_no_error(error);
			g_object_set_data_full(G_OBJECT(database), "accounting-test-schema", g_strdup(schema), g_free);
		}
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		g_assert_cmpint(scalar(database, "SELECT COUNT(*) FROM leads WHERE name='Historical inquiry' AND email='reader@example.test'"), ==, 1);
		g_assert_cmpint(scalar(database, "SELECT COUNT(*) FROM business_events"), ==, 0);
		g_assert_cmpint(scalar(database, "SELECT COUNT(*) FROM webhooks WHERE business_events=TRUE"), ==, 0);
		g_assert_cmpint(scalar(database, "SELECT COUNT(*) FROM schema_migrations WHERE version IN (919,920)"), ==, 2);
		if (run == 1) venture_test_accounting_database_cleanup(database);
		g_clear_object(&database);
	}
	venture_test_remove_tree(directory);
}

static void
test_without_webhooks(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_test_accounting_database(NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmInspector) inspector = NULL;
	venture_config_set_module_enabled(config, "webhooks", FALSE);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	inspector = orm_inspector_new(venture_database_get_connection(database), &error);
	g_assert_no_error(error);
	g_assert_false(orm_inspector_has_table(inspector, "webhooks", NULL, &error));
	g_assert_no_error(error);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", venture_context_get_default_organization_id(context), "name", "Example inquiry", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(scalar(database, "SELECT COUNT(*) FROM business_events"), ==, 1);
	venture_config_set_module_enabled(config, "webhooks", TRUE);
	g_clear_object(&context);
	venture_test_accounting_database_cleanup(database);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/business-event-upgrade/history", test_upgrade);
	g_test_add_func("/business-event-upgrade/without-webhooks", test_without_webhooks);
	return g_test_run();
}
