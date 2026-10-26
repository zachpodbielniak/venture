/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

/* An existing document keeps its metadata and gains an empty payload. A
 * later imported payload survives a new connection, including with CRM
 * import switched off. This is an upgrade, not just fresh schema coverage. */
static void
test_attachment_upgrade(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-crm-storage-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/database.db", directory);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	guint run;
	g_object_set(config, "state-dir", directory, NULL);
	database = venture_database_new(uri, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	{
		g_autoptr(VentureDocument) document = venture_document_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(document), venture_context_get_default_organization_id(context));
		g_object_set(document, "title", "Existing synthetic document", "url", "https://example.test/evidence", NULL);
		g_assert_true(venture_database_save(database, VENTURE_ENTITY(document), NULL, &error));
		g_assert_no_error(error);
		id = venture_entity_get_id(VENTURE_ENTITY(document));
	}
	/* History is a prefix, so every version after the attachment check
	 * has to be forgotten with it. Reconciliation puts the column back
	 * and 000958 only verifies that; 000980 is safe to run again. */
	g_assert_true(venture_database_execute(database,
		"ALTER TABLE documents DROP COLUMN content_base64;"
		"DELETE FROM schema_migrations WHERE version >= 958", NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&context);
	g_clear_object(&database);
	for (run = 0; run < 3; run++)
	{
		g_autoptr(VentureEntity) document = NULL;
		g_autofree gchar *title = NULL, *url = NULL, *payload = NULL;
		venture_config_set_module_enabled(config, "crm_import", run != 1);
		database = venture_database_new(uri, &error);
		g_assert_no_error(error);
		context = venture_context_new(config, database);
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		g_assert_no_error(error);
		document = venture_database_get(database, VENTURE_TYPE_DOCUMENT, id, &error);
		g_assert_no_error(error);
		g_assert_nonnull(document);
		g_object_get(document, "title", &title, "url", &url, "content-base64", &payload, NULL);
		g_assert_cmpstr(title, ==, "Existing synthetic document");
		g_assert_cmpstr(url, ==, "https://example.test/evidence");
		if (run == 0)
		{
			g_assert_true(payload == NULL || *payload == '\0');
			g_object_set(document, "content-base64", "U3ludGhldGljCg==", NULL);
			g_assert_true(venture_database_save(database, document, NULL, &error));
			g_assert_no_error(error);
		}
		else
			g_assert_cmpstr(payload, ==, "U3ludGhldGljCg==");
		g_clear_object(&document);
		g_clear_object(&context);
		g_clear_object(&database);
	}
	venture_test_remove_tree(directory);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/crm-import-storage/upgrade-restart-disabled", test_attachment_upgrade);
	return g_test_run();
}
