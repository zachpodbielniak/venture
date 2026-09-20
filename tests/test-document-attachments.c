/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"
typedef struct { VentureDatabase *database; VentureContext *context; VentureConfig *config; gchar *directory, *root, *path; } Fixture;
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	(void)unused;
	f->directory = g_dir_make_tmp("venture-attachment-XXXXXX", NULL);
	f->root = g_build_filename(f->directory, "attachments", NULL); g_assert_cmpint(g_mkdir_with_parents(f->root, 0700), ==, 0);
	f->path = g_build_filename(f->root, "receipt.txt", NULL); g_assert_true(g_file_set_contents(f->path, "Original receipt", -1, NULL));
	f->config = venture_config_new(); g_object_set(f->config, "state-dir", f->directory, NULL);
	f->database = venture_test_accounting_database(&error); g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	g_object_set(other, "name", "Other organization", NULL); g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(other), NULL, &error));
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused; g_clear_object(&f->context); venture_test_accounting_database_cleanup(f->database); g_clear_object(&f->database); g_clear_object(&f->config);
	venture_test_remove_tree(f->directory); g_free(f->path); g_free(f->root); g_free(f->directory);
}
static VentureEntity *filed(Fixture *f)
{
	VentureEntity *document = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", (gint64)1, "title", "Receipt", "path", f->path, NULL);
	g_assert_true(venture_document_service_save_attachment(venture_document_service_get(f->database), document, f->root, NULL, NULL));
	return document;
}
static void test_generic_assignment(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) document = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", (gint64)1, "title", "Forged", "path", f->path, NULL);
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_assert_false(venture_database_save(f->database, document, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
static void test_ownership(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) document = filed(f), alias = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", (gint64)2, "title", "Alias", "path", f->path, NULL);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *moved = g_build_filename(f->root, "different.txt", NULL);
	(void)unused;
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->database), document, f->root, 100, &error);
	g_assert_no_error(error); g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), "Original receipt", strlen("Original receipt"));
	g_assert_false(venture_document_service_save_attachment(venture_document_service_get(f->database), alias, f->root, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(document, "path", moved, NULL); g_assert_false(venture_database_save(f->database, document, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(document, "path", f->path, "organization-id", (gint64)2, NULL);
	g_assert_false(venture_database_save(f->database, document, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
/* Legacy duplicates remain as history but must never authorize file reads. */
static void test_legacy_alias(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) document = filed(f), alias = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", (gint64)2, "title", "Legacy alias", NULL);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	(void)unused;
	g_assert_true(venture_database_save(f->database, alias, NULL, &error));
	sql = g_strdup_printf("UPDATE documents SET path=(SELECT path FROM documents WHERE id=%" G_GINT64_FORMAT ") WHERE id=%" G_GINT64_FORMAT,
		venture_entity_get_id(document), venture_entity_get_id(alias));
	g_assert_true(venture_database_execute(f->database, sql, NULL, &error));
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->database), document, f->root, 100, &error);
	g_assert_null(bytes); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_object_set(document, "title", "Preserved legacy metadata", NULL);
	g_assert_true(venture_database_save(f->database, document, NULL, &error)); g_assert_no_error(error);
}
static void test_symlink_and_size(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) document = filed(f);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *outside = g_build_filename(f->directory, "outside.txt", NULL);
	(void)unused;
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->database), document, f->root, 1, &error);
	g_assert_null(bytes); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_true(g_file_set_contents(outside, "Private bytes", -1, NULL)); g_assert_cmpint(g_unlink(f->path), ==, 0);
	g_assert_cmpint(symlink(outside, f->path), ==, 0);
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->database), document, f->root, 100, &error);
	g_assert_null(bytes); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	g_assert_cmpint(g_unlink(f->path), ==, 0); g_assert_cmpint(mkfifo(f->path, 0600), ==, 0);
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->database), document, f->root, 100, &error);
	g_assert_null(bytes); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
/* Valid nested historical storage stays readable; directory links do not. */
static void test_nested(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) document = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *nested = g_build_filename(f->root, "old", NULL);
	g_autofree gchar *moved = g_build_filename(f->directory, "moved", NULL);
	(void)unused;
	g_assert_cmpint(g_mkdir(nested, 0700), ==, 0);
	g_free(f->path); f->path = g_build_filename(nested, "receipt.txt", NULL);
	g_assert_true(g_file_set_contents(f->path, "Legacy receipt", -1, NULL)); document = filed(f);
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->database), document, f->root, 100, &error);
	g_assert_no_error(error); g_assert_nonnull(bytes); g_clear_pointer(&bytes, g_bytes_unref);
	g_assert_cmpint(g_rename(nested, moved), ==, 0); g_assert_cmpint(symlink(moved, nested), ==, 0);
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->database), document, f->root, 100, &error);
	g_assert_null(bytes); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/attachments/generic-assignment", Fixture, NULL, setup, test_generic_assignment, teardown);
	g_test_add("/attachments/ownership", Fixture, NULL, setup, test_ownership, teardown);
	g_test_add("/attachments/legacy-alias", Fixture, NULL, setup, test_legacy_alias, teardown);
	g_test_add("/attachments/symlink-size", Fixture, NULL, setup, test_symlink_and_size, teardown);
	g_test_add("/attachments/nested", Fixture, NULL, setup, test_nested, teardown);
	return g_test_run();
}
