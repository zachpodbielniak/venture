/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"
#include <glib/gstdio.h>
#if defined(VENTURE_HAVE_POPPLER) && defined(VENTURE_HAVE_OCR_RASTER)
#include <cairo.h>
#include <cairo-pdf.h>
#endif
#include <unistd.h>
#include <fcntl.h>

/* Generic actions are the contract shared by the UI, API, CLI and assistant. */
static void
test_actions_registered(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_nonnull(venture_action_registry_lookup(venture_database_get_action_registry(database), "document", "ocr_extract"));
}

/* Optional local tools must never become prerequisites of ordinary startup. */
static void test_optional_defaults(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	g_autoptr(GError) error = NULL;
	g_object_set(config, "ocr-executable", "/nonexistent/ocr", NULL);
	venture_module_registry_register_builtins(modules);
	g_assert_true(venture_module_registry_configure(modules, config, &error));
	g_assert_no_error(error); g_assert_false(venture_module_registry_is_enabled(modules, "ocr"));
	g_assert_true(venture_config_validate(config, &error)); g_assert_no_error(error);
	g_object_set(config, "ocr-enabled", TRUE, NULL);
	g_assert_false(venture_config_validate(config, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

#if defined(VENTURE_HAVE_POPPLER) && defined(VENTURE_HAVE_OCR_RASTER)
typedef struct {
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	VentureOcrService *service;
	gchar *directory, *binary, *arguments;
} Fixture;
static void set_engine_script(Fixture *f, const gchar *body)
{
	g_autofree gchar *script = g_strconcat("#!/bin/sh\nif [ \"$1\" = --version ]; then printf 'fixture OCR 1\\n'; exit 0; fi\n", body, NULL);
	g_assert_true(g_file_set_contents(f->binary, script, -1, NULL));
	g_assert_cmpint(g_chmod(f->binary, 0700), ==, 0);
}
static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *attachments = NULL, *quoted = NULL, *script = NULL;
	(void)data;
	f->directory = g_dir_make_tmp("venture-ocr-test-XXXXXX", NULL);
	f->binary = g_build_filename(f->directory, "fake-tesseract", NULL);
	f->arguments = g_build_filename(f->directory, "arguments", NULL);
	quoted = g_shell_quote(f->arguments);
	script = g_strdup_printf("printf '%%s\\n' \"$@\" > %s\nprintf 'Synthetic receipt 12.50 USD\\n'\n", quoted);
	set_engine_script(f, script);
	attachments = g_build_filename(f->directory, "attachments", NULL);
	g_assert_cmpint(g_mkdir_with_parents(attachments, 0700), ==, 0);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "ocr-executable", f->binary, "ocr-enabled", TRUE, NULL);
	venture_config_set_module_enabled(f->config, "capture", TRUE);
	venture_config_set_module_enabled(f->config, "ocr", TRUE);
	f->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	g_assert_true(venture_database_migrate(f->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->service = venture_ocr_service_get(f->database);
}
static void teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context); venture_test_accounting_database_cleanup(f->database); g_clear_object(&f->database); g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_free(f->binary); g_free(f->arguments); g_free(f->directory);
}
static VentureEntity *image_document(Fixture *f, const gchar *name)
{
	g_autofree gchar *path = g_build_filename(f->directory, "attachments", name, NULL);
	VentureEntity *document = VENTURE_ENTITY(venture_document_new());
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 40, 40);
	g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(surface);
	g_object_set(document, "organization-id", (gint64)1, "title", name, "path", path, "mime-type", "image/png", NULL);
	{
		g_autofree gchar *root = g_path_get_dirname(path);
		g_assert_true(venture_document_service_save_attachment(venture_document_service_get(f->database), document, root, NULL, NULL));
	}
	return document;
}
static void assert_state(VentureEntity *row, const gchar *expected)
{
	g_autofree gchar *state = NULL;
	g_object_get(row, "state", &state, NULL); g_assert_cmpstr(state, ==, expected);
}
/* The stored result feeds the existing document/AI path without posting books. */
static void test_extract_review_replay(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "receipt.png"), job = NULL, done = NULL, replay = NULL, reviewed = NULL, forced = NULL, second = NULL, refreshed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL, *args = NULL, *hash = NULL;
	gint64 pages;
	(void)data;
	job = venture_ocr_service_queue(f->service, document, "eng", FALSE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(job); assert_state(job, "pending");
	done = venture_ocr_service_step(f->service, job, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(done); assert_state(done, "succeeded");
	g_object_get(done, "completed-pages", &pages, "source-hash", &hash, NULL);
	g_assert_cmpint(pages, ==, 1); g_assert_cmpuint(strlen(hash), ==, 64);
	refreshed = venture_database_get(f->database, VENTURE_TYPE_DOCUMENT, venture_entity_get_id(document), &error);
	g_object_get(refreshed, "extracted-text", &text, NULL); g_assert_nonnull(strstr(text, "12.50 USD"));
	g_assert_true(g_file_get_contents(f->arguments, &args, NULL, NULL));
	g_assert_nonnull(strstr(args, "\nstdout\n-l\neng\n"));
	replay = venture_ocr_service_queue(f->service, refreshed, "eng", FALSE, NULL, &error);
	g_assert_no_error(error); g_assert_cmpint(venture_entity_get_id(replay), ==, venture_entity_get_id(done));
	reviewed = venture_ocr_service_control(f->service, done, "review", "Corrected 12.60 USD", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(reviewed);
	g_clear_object(&refreshed);
	refreshed = venture_database_get(f->database, VENTURE_TYPE_DOCUMENT, venture_entity_get_id(document), &error);
	forced = venture_ocr_service_queue(f->service, refreshed, "eng", TRUE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(forced);
	second = venture_ocr_service_step(f->service, forced, NULL, NULL, &error);
	g_assert_no_error(error); assert_state(second, "succeeded");
	g_clear_object(&refreshed); g_clear_pointer(&text, g_free);
	refreshed = venture_database_get(f->database, VENTURE_TYPE_DOCUMENT, venture_entity_get_id(document), &error);
	g_object_get(refreshed, "extracted-text", &text, NULL); g_assert_cmpstr(text, ==, "Corrected 12.60 USD");
	{
		g_autoptr(VentureQuery) expenses = venture_query_new(VENTURE_TYPE_EXPENSE);
		g_assert_cmpint(venture_database_count(f->database, expenses, NULL), ==, 0);
	}
}
static void test_failure_retry_cancel(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "retry.png"), job = NULL, failed = NULL, retry = NULL, cancelled = NULL, resumed = NULL, done = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	set_engine_script(f, "exit 3\n");
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error); g_assert_no_error(error);
	failed = venture_ocr_service_step(f->service, job, NULL, NULL, &error); g_assert_no_error(error); assert_state(failed, "failed");
	retry = venture_ocr_service_control(f->service, failed, "retry", NULL, NULL, &error); g_assert_no_error(error);
	/* Retained work can be cancelled even if the provider was removed. */
	g_object_set(f->config, "ocr-executable", "/nonexistent/ocr", NULL);
	cancelled = venture_ocr_service_control(f->service, retry, "cancel", NULL, NULL, &error); g_assert_no_error(error); assert_state(cancelled, "cancelled");
	g_object_set(f->config, "ocr-executable", f->binary, NULL);
	done = venture_ocr_service_step(f->service, cancelled, NULL, NULL, &error); g_assert_null(done); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT); g_clear_error(&error);
	resumed = venture_ocr_service_control(f->service, cancelled, "retry", NULL, NULL, &error); g_assert_no_error(error);
	set_engine_script(f, "printf 'Recovered receipt\\n'\n");
	done = venture_ocr_service_step(f->service, resumed, NULL, NULL, &error); g_assert_no_error(error); assert_state(done, "succeeded");
	g_object_set(done, "text", "forged", NULL);
	g_assert_false(venture_database_save(f->database, done, NULL, &error)); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
static void test_bounds_and_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "bounded.png"), job = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL, *link = g_build_filename(f->directory, "attachments", "link.png", NULL);
	(void)data;
	job = venture_ocr_service_queue(f->service, document, "eng; touch /tmp/forbidden", FALSE, NULL, &error);
	g_assert_null(job); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	venture_entity_set_organization_id(document, 999);
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error);
	g_assert_null(job); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	venture_entity_set_organization_id(document, 1);
	g_object_get(document, "path", &path, NULL); g_assert_cmpint(symlink(path, link), ==, 0);
	g_object_set(document, "path", link, NULL);
	g_assert_false(venture_database_save(f->database, document, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	job = venture_ocr_service_batch(f->service, 1, 0, 26, NULL, FALSE, NULL, &error);
	g_assert_null(job); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}
static void test_pdf_resume(Fixture *f, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(f->directory, "attachments", "mixed.pdf", NULL), *text = NULL;
	g_autoptr(VentureEntity) document = VENTURE_ENTITY(venture_document_new()), job = NULL, first = NULL, second = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/restart.db", f->directory);
	g_autoptr(VentureDatabase) database = venture_database_new(uri, &error);
	g_autoptr(VentureContext) context = venture_context_new(f->config, database);
	VentureOcrService *service = venture_ocr_service_get(database);
	cairo_surface_t *surface = cairo_pdf_surface_create(path, 200, 100);
	cairo_t *cr = cairo_create(surface);
	gint64 cursor;
	(void)data;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	cairo_move_to(cr, 10, 30); cairo_show_text(cr, "Native invoice 100"); cairo_show_page(cr);
	cairo_set_source_rgb(cr, 0, 0, 0); cairo_rectangle(cr, 10, 10, 20, 20); cairo_fill(cr); cairo_show_page(cr);
	cairo_destroy(cr); cairo_surface_destroy(surface);
	g_object_set(document, "organization-id", (gint64)1, "title", "Mixed PDF", "path", path, "mime-type", "application/pdf", NULL);
	{
		g_autofree gchar *root = g_path_get_dirname(path);
		g_assert_true(venture_document_service_save_attachment(venture_document_service_get(database), document, root, NULL, &error));
	}
	job = venture_ocr_service_queue(service, document, NULL, FALSE, NULL, &error); g_assert_no_error(error);
	first = venture_ocr_service_step(service, job, NULL, NULL, &error); g_assert_no_error(error); assert_state(first, "running");
	g_object_get(first, "completed-pages", &cursor, NULL); g_assert_cmpint(cursor, ==, 1);
	g_assert_false(g_file_test(f->arguments, G_FILE_TEST_EXISTS));
	/* Close the repository and services: progress must survive a real restart. */
	g_clear_object(&first); g_clear_object(&context); g_clear_object(&database);
	database = venture_database_new(uri, &error); g_assert_no_error(error);
	context = venture_context_new(f->config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	service = venture_ocr_service_get(database);
	first = venture_database_get(database, VENTURE_TYPE_OCR_JOB, venture_entity_get_id(job), &error);
	second = venture_ocr_service_step(service, first, NULL, NULL, &error); g_assert_no_error(error); assert_state(second, "succeeded");
	g_object_get(second, "text", &text, NULL);
	g_assert_nonnull(strstr(text, "Native invoice 100")); g_assert_nonnull(strstr(text, "Synthetic receipt"));
	g_assert_null(strstr(strstr(text, "Native invoice 100") + 1, "Native invoice 100"));
}
static void test_deadline(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "deadline.png");
	g_autoptr(VentureOcrEngine) engine = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL, *contents = NULL, *text = NULL;
	gsize length;
	gint64 start;
	(void)data;
	set_engine_script(f, "while :; do :; done\n");
	engine = venture_ocr_local_new(f->binary, 50, &error); g_assert_no_error(error);
	g_object_get(document, "path", &path, NULL); g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
	bytes = g_bytes_new(contents, length); start = g_get_monotonic_time();
	text = venture_ocr_engine_extract(engine, bytes, "image/png", 0, "eng", NULL, &error);
	g_assert_null(text); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_USEC_PER_SEC);
}
static void test_batch_failure_isolation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) good = image_document(f, "batch.png"), bad = image_document(f, "gone.png"), batch = NULL, first = NULL, second = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	gint64 failed, cursor;
	VentureCaptureService *capture = venture_capture_service_get(f->database);
	g_autoptr(VentureEntity) one = venture_capture_service_ingest_for_organization(capture, 1, "receipt", "Bad", "test", venture_entity_get_id(bad), NULL, NULL, NULL, NULL, NULL, &error);
	g_autoptr(VentureEntity) two = venture_capture_service_ingest_for_organization(capture, 1, "receipt", "Good", "test", venture_entity_get_id(good), NULL, NULL, NULL, NULL, NULL, &error);
	(void)data;
	g_assert_no_error(error); g_assert_nonnull(one); g_assert_nonnull(two);
	g_object_get(bad, "path", &path, NULL); g_assert_cmpint(g_unlink(path), ==, 0);
	batch = venture_ocr_service_batch(f->service, 1, 0, 25, NULL, FALSE, NULL, &error); g_assert_no_error(error);
	first = venture_ocr_service_batch_step(f->service, batch, FALSE, NULL, &error); g_assert_no_error(error); assert_state(first, "running");
	second = venture_ocr_service_batch_step(f->service, first, FALSE, NULL, &error); g_assert_no_error(error); assert_state(second, "succeeded");
	g_object_get(second, "failed", &failed, "cursor", &cursor, NULL); g_assert_cmpint(failed, ==, 1); g_assert_cmpint(cursor, ==, 2);
}

static gpointer cancel_soon(gpointer data)
{ g_usleep(100000); g_cancellable_cancel(data); return NULL; }
static void test_process_bounds_and_cancel(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "cancel.png"), job = NULL, result = NULL, retry = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	GThread *thread;
	(void)data;
	set_engine_script(f, "while :; do :; done\n");
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error); g_assert_no_error(error);
	/* This worker only signals cancellation; it never accesses records. */
	thread = g_thread_new("cancel-ocr", cancel_soon, cancellable);
	result = venture_ocr_service_step(f->service, job, cancellable, NULL, &error); g_thread_join(thread);
	g_assert_no_error(error); assert_state(result, "cancelled");
	retry = venture_ocr_service_control(f->service, result, "retry", NULL, NULL, &error); g_assert_no_error(error); g_clear_object(&result);
	set_engine_script(f, "i=0; while [ \"$i\" -lt 20000 ]; do printf '1234567890123456789012345678901234567890123456789012345678901234567890'; i=$((i+1)); done\n");
	result = venture_ocr_service_step(f->service, retry, NULL, NULL, &error); g_assert_no_error(error); assert_state(result, "failed");
}
static void test_source_bounds(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "huge.png"), job = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	cairo_surface_t *surface;
	gint fd;
	guint page;
	cairo_t *cr;
	(void)data;
	g_object_get(document, "path", &path, NULL);
	surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, VENTURE_OCR_MAX_DIMENSION + 1, 1);
	g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS); cairo_surface_destroy(surface);
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error); g_assert_no_error(error); assert_state(job, "failed");
	g_clear_object(&job);
	fd = g_open(path, O_WRONLY | O_TRUNC, 0600); g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(ftruncate(fd, VENTURE_OCR_MAX_BYTES + 1), ==, 0); close(fd);
	job = venture_ocr_service_queue(f->service, document, NULL, TRUE, NULL, &error); g_assert_no_error(error); assert_state(job, "failed");
	g_clear_object(&job);
	surface = cairo_pdf_surface_create(path, 50, 50); cr = cairo_create(surface);
	for (page = 0; page <= VENTURE_OCR_MAX_PAGES; page++) cairo_show_page(cr);
	cairo_destroy(cr); cairo_surface_destroy(surface);
	g_object_set(document, "mime-type", "application/pdf", NULL);
	g_assert_true(venture_database_save(f->database, document, NULL, &error));
	job = venture_ocr_service_queue(f->service, document, NULL, TRUE, NULL, &error); g_assert_no_error(error); assert_state(job, "failed");
}
static void test_disabled_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "ordinary.png"), item = NULL, job = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(job); g_clear_object(&job);
	venture_config_set_module_enabled(f->config, "ocr", FALSE);
	g_object_set(f->config, "ocr-executable", "/nonexistent/ocr", NULL);
	g_assert_false(venture_ocr_service_enabled(f->service));
	item = venture_capture_service_ingest_for_organization(venture_capture_service_get(f->database), 1,
		"receipt", "Ordinary capture", "test", venture_entity_get_id(document), NULL, NULL, NULL, NULL, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(item);
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error);
	g_assert_null(job); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG); g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "ocr", TRUE);
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error);
	g_assert_null(job); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void test_stale_review(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "review.png"), job = NULL, done = NULL, current = NULL;
	g_autoptr(JsonNode) parameters = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	VentureConfirmation *confirmation;
	VentureConfirmationStore *store = venture_context_get_confirmations(f->context);
	(void)data;
	job = venture_ocr_service_queue(f->service, document, NULL, FALSE, NULL, &error);
	done = venture_ocr_service_step(f->service, job, NULL, NULL, &error); g_assert_no_error(error);
	current = venture_database_get(f->database, VENTURE_TYPE_DOCUMENT, venture_entity_get_id(document), &error);
	json_node_take_object(parameters, json_object_new());
	json_object_set_int_member(json_node_get_object(parameters), "job_id", venture_entity_get_id(done));
	json_object_set_string_member(json_node_get_object(parameters), "text", "Proposed correction");
	params = venture_action_parameters_from_json(parameters, &error);
	confirmation = venture_confirmation_store_stage_action(store,
		venture_action_registry_lookup(venture_database_get_action_registry(f->database), "document", "ocr_review"),
		current, params, NULL, VENTURE_USER_ROLE_EDITOR, "test", &error);
	g_assert_no_error(error); g_assert_nonnull(confirmation); id = g_strdup(venture_confirmation_get_id(confirmation));
	g_object_set(current, "extracted-text", "Newer reviewed correction", NULL);
	g_assert_true(venture_database_save(f->database, current, NULL, &error));
	g_assert_false(venture_confirmation_store_approve_as(store, id, "local", VENTURE_USER_ROLE_OWNER, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}
static void test_real_engine(Fixture *f, gconstpointer data)
{
	const gchar *binary = g_getenv("VENTURE_TEST_OCR_TESSERACT");
	g_autoptr(VentureOcrEngine) engine = NULL;
	g_autoptr(GBytes) image_bytes = NULL, pdf_bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *image_path = g_build_filename(f->directory, "real.png", NULL), *pdf_path = g_build_filename(f->directory, "mixed-real.pdf", NULL);
	g_autofree gchar *contents = NULL, *image_text = NULL, *native_text = NULL, *scanned_text = NULL;
	gsize size;
	cairo_surface_t *image, *pdf;
	cairo_t *cr;
	(void)data;
	if (binary == NULL) { g_test_skip("Set VENTURE_TEST_OCR_TESSERACT for an explicitly provisioned local engine"); return; }
	engine = venture_ocr_local_new(binary, 30000, &error); g_assert_no_error(error); g_assert_nonnull(engine);
	image = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1200, 250); cr = cairo_create(image);
	cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr); cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 54); cairo_move_to(cr, 30, 100); cairo_show_text(cr, "RECEIPT TOTAL 12.50 USD"); cairo_destroy(cr);
	g_assert_cmpint(cairo_surface_write_to_png(image, image_path), ==, CAIRO_STATUS_SUCCESS);
	pdf = cairo_pdf_surface_create(pdf_path, 600, 150); cr = cairo_create(pdf);
	cairo_move_to(cr, 20, 30); cairo_show_text(cr, "Native page 123"); cairo_show_page(cr);
	cairo_scale(cr, 0.5, 0.5); cairo_set_source_surface(cr, image, 0, 0); cairo_paint(cr); cairo_show_page(cr);
	cairo_destroy(cr); cairo_surface_destroy(pdf); cairo_surface_destroy(image);
	g_assert_true(g_file_get_contents(image_path, &contents, &size, &error)); image_bytes = g_bytes_new_take(g_steal_pointer(&contents), size);
	g_assert_true(g_file_get_contents(pdf_path, &contents, &size, &error)); pdf_bytes = g_bytes_new_take(g_steal_pointer(&contents), size);
	image_text = venture_ocr_engine_extract(engine, image_bytes, "image/png", 0, "eng", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(strstr(image_text, "12.50"));
	g_assert_cmpuint(venture_ocr_engine_inspect(engine, pdf_bytes, "application/pdf", &error), ==, 2);
	native_text = venture_ocr_engine_extract(engine, pdf_bytes, "application/pdf", 0, "eng", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(strstr(native_text, "Native page 123"));
	scanned_text = venture_ocr_engine_extract(engine, pdf_bytes, "application/pdf", 1, "eng", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(strstr(scanned_text, "12.50"));
	g_test_message("Real engine provenance: %s", venture_ocr_engine_describe(engine));
}
typedef struct { gboolean done; GBytes *body; GError *error; } HttpResult;
static void http_done(GObject *source, GAsyncResult *result, gpointer data)
{ HttpResult *out = data; out->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &out->error); out->done = TRUE; }
static gchar *http_get(SoupSession *session, guint port, const gchar *path)
{
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", port, path);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	HttpResult out;
	gchar *body;
	out.done = FALSE; out.body = NULL; out.error = NULL;
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &out);
	while (!out.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(out.error); g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	body = g_strndup(g_bytes_get_data(out.body, NULL), g_bytes_get_size(out.body)); g_bytes_unref(out.body); return body;
}
static gboolean form_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{ (void)action; (void)entity; (void)actor; (void)error; return TRUE; }
static VentureEntity *form_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{ (void)action; (void)params; (void)actor; (void)error; return g_object_ref(entity); }
static void test_action_forms(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) document = image_document(f, "Visible source"), outside = NULL;
	g_autoptr(VentureOrganization) organization = venture_organization_new();
	g_autoptr(GPtrArray) fields = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL, *body = NULL;
	VentureFieldSpec *field;
	guint port;
	(void)data;
	g_object_set(organization, "name", "Other organization", NULL); g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(organization), NULL, &error));
	outside = VENTURE_ENTITY(venture_document_new());
	g_object_set(outside, "title", "OUTSIDE_REFERENCE_SECRET", "organization-id", venture_entity_get_id(VENTURE_ENTITY(organization)), NULL);
	g_assert_true(venture_database_save(f->database, outside, NULL, &error));
	g_ptr_array_add(fields, venture_field_spec_new("notes", "Notes", VENTURE_FIELD_KIND_TEXT));
	field = venture_field_spec_new("choice", "Choice", VENTURE_FIELD_KIND_ENUM); field->choices = g_strsplit("first,second", ",", -1); g_ptr_array_add(fields, field);
	field = venture_field_spec_new("document_id", "Document", VENTURE_FIELD_KIND_REFERENCE); field->reference_type = g_strdup("document"); g_ptr_array_add(fields, field);
	action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "document", "name", "test_form", "label", "Test form", "description", "Exercise generic action controls", "parameters", fields, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	{
		gboolean registered = venture_action_registry_register(venture_database_get_action_registry(f->database), action, form_allowed, form_invoke, NULL, NULL, &error);
		g_assert_no_error(error); g_assert_true(registered);
	}
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error); g_assert_no_error(error); g_clear_object(&listener);
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error); g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	path = g_strdup_printf("/e/document/%" G_GINT64_FORMAT, venture_entity_get_id(document)); body = http_get(session, port, path);
	g_assert_nonnull(strstr(body, "<textarea name=\"notes\"")); g_assert_nonnull(strstr(body, "<select name=\"choice\""));
	g_assert_nonnull(strstr(body, "<option value=\"second\"")); g_assert_nonnull(strstr(body, "<select name=\"document_id\""));
	g_assert_null(strstr(body, "OUTSIDE_REFERENCE_SECRET")); g_clear_pointer(&body, g_free);
	body = http_get(session, port, "/e/capture_item"); g_assert_nonnull(strstr(body, "data-record-action=\"ocr_extract_all\""));
	g_clear_pointer(&body, g_free); body = http_get(session, port, "/capture"); g_assert_nonnull(strstr(body, "data-record-action=\"ocr_extract_all\""));
	venture_web_server_stop(server);
}

#endif

static void test_missing_engine(void)
{
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_ocr_local_check("/nonexistent/venture-test-tesseract", &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ocr/actions", test_actions_registered);
	g_test_add_func("/ocr/optional-defaults", test_optional_defaults);
	g_test_add_func("/ocr/missing-engine", test_missing_engine);
#if defined(VENTURE_HAVE_POPPLER) && defined(VENTURE_HAVE_OCR_RASTER)
	g_test_add("/ocr/extract-review-replay", Fixture, NULL, setup, test_extract_review_replay, teardown);
	g_test_add("/ocr/failure-retry-cancel", Fixture, NULL, setup, test_failure_retry_cancel, teardown);
	g_test_add("/ocr/bounds-scope", Fixture, NULL, setup, test_bounds_and_scope, teardown);
	g_test_add("/ocr/pdf-resume", Fixture, NULL, setup, test_pdf_resume, teardown);
	g_test_add("/ocr/deadline", Fixture, NULL, setup, test_deadline, teardown);
	g_test_add("/ocr/process-bounds-cancel", Fixture, NULL, setup, test_process_bounds_and_cancel, teardown);
	g_test_add("/ocr/disabled-capture", Fixture, NULL, setup, test_disabled_capture, teardown);
	g_test_add("/ocr/source-bounds", Fixture, NULL, setup, test_source_bounds, teardown);
	g_test_add("/ocr/batch-failure-isolation", Fixture, NULL, setup, test_batch_failure_isolation, teardown);
	g_test_add("/ocr/stale-review", Fixture, NULL, setup, test_stale_review, teardown);
	g_test_add("/ocr/action-forms", Fixture, NULL, setup, test_action_forms, teardown);
	g_test_add("/ocr/real-engine", Fixture, NULL, setup, test_real_engine, teardown);

#endif
	return g_test_run();
}
