/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
struct _VentureOcrService { GObject parent; VentureDatabase *database; VentureConfig *config; VentureOcrEngine *engine; VentureEntity *writing; gchar *local_executable; gboolean custom_engine, busy; };
G_DEFINE_FINAL_TYPE(VentureOcrService, venture_ocr_service, G_TYPE_OBJECT)
static void service_finalize(GObject *object)
{
	VentureOcrService *self = VENTURE_OCR_SERVICE(object);
	g_clear_object(&self->config); g_clear_object(&self->engine); g_free(self->local_executable);
	if (self->database != NULL) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_ocr_service_parent_class)->finalize(object);
}
static void venture_ocr_service_class_init(VentureOcrServiceClass *klass) { G_OBJECT_CLASS(klass)->finalize = service_finalize; }
static void venture_ocr_service_init(VentureOcrService *self) { (void)self; }
static gboolean refuse(GError **error, VentureError code, const gchar *message)
{ g_set_error_literal(error, VENTURE_ERROR, code, message); return FALSE; }
static gboolean validate(VentureDatabase *db, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	VentureOcrService *self = data;
	(void)db; (void)previous;
	if (self->writing != row) return refuse(error, VENTURE_ERROR_VALIDATION, "OCR jobs and batches are written through their service actions");
	return TRUE;
}
VentureOcrService *venture_ocr_service_get(VentureDatabase *database)
{
	VentureOcrService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-ocr-service");
	if (self == NULL) {
		self = g_object_new(VENTURE_TYPE_OCR_SERVICE, NULL); self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-ocr-service", self, g_object_unref);
		venture_database_add_save_validator(database, VENTURE_TYPE_OCR_JOB, validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_OCR_BATCH, validate, self, NULL);
	}
	return self;
}
void venture_ocr_service_configure(VentureOcrService *self, VentureConfig *config)
{ g_return_if_fail(VENTURE_IS_OCR_SERVICE(self)); g_set_object(&self->config, config); }
gboolean venture_ocr_service_enabled(VentureOcrService *self)
{
	gboolean enabled = FALSE;
	g_return_val_if_fail(VENTURE_IS_OCR_SERVICE(self), FALSE);
	if (self->config != NULL) g_object_get(self->config, "ocr-enabled", &enabled, NULL);
	return enabled &&
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "ocr_job") != G_TYPE_INVALID;
}
void venture_ocr_service_set_engine(VentureOcrService *self, VentureOcrEngine *engine)
{ g_return_if_fail(VENTURE_IS_OCR_SERVICE(self)); g_return_if_fail(!self->busy); g_set_object(&self->engine, engine); self->custom_engine = engine != NULL; }
static gboolean ready(VentureOcrService *self, gboolean require_engine, GError **error)
{
	g_autofree gchar *executable = NULL;
	if (self->busy || self->database == NULL || self->config == NULL) return refuse(error, VENTURE_ERROR_CONFLICT, "OCR service is unavailable or already running");
	if (!venture_ocr_service_enabled(self))
		return refuse(error, VENTURE_ERROR_CONFIG, "The OCR module is disabled");
	if (!require_engine) return TRUE;
	g_object_get(self->config, "ocr-executable", &executable, NULL);
	if (!self->custom_engine && g_strcmp0(executable, self->local_executable) != 0) {
		g_clear_object(&self->engine);
		g_free(self->local_executable); self->local_executable = g_strdup(executable);
	}
	if (self->engine == NULL) {
		self->engine = venture_ocr_local_new(executable, 30000, error);
	}
	return self->engine != NULL;
}
static gboolean save(VentureOcrService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = row; ok = venture_database_save(self->database, row, actor, error); self->writing = NULL; return ok;
}
static VentureEntity *live_record(VentureOcrService *self, VentureEntity *record, GError **error)
{
	g_autoptr(VentureEntity) live = venture_database_get(self->database, G_OBJECT_TYPE(record), venture_entity_get_id(record), error);
	if (live == NULL) return NULL;
	if (venture_entity_is_deleted(live) || venture_entity_get_organization_id(live) != venture_entity_get_organization_id(record)) {
		refuse(error, VENTURE_ERROR_NOT_FOUND, "OCR record is unavailable in this organization"); return NULL;
	}
	if (venture_entity_get_version(live) != venture_entity_get_version(record)) {
		refuse(error, VENTURE_ERROR_CONFLICT, "OCR record changed; reload before acting"); return NULL;
	}
	return g_steal_pointer(&live);
}
static GBytes *source_bytes(VentureOcrService *self, VentureEntity *document, GError **error)
{
	g_autofree gchar *root = g_build_filename(venture_config_get_state_dir(self->config), "attachments", NULL);
	return venture_document_service_read_attachment(venture_document_service_get(self->database), document, root, VENTURE_OCR_MAX_BYTES, error);
}
static gchar *language_for(VentureOcrService *self, const gchar *language, GError **error)
{
	gchar *result = NULL;
	if (language != NULL) result = g_strdup(language); else g_object_get(self->config, "ocr-language", &result, NULL);
	if (result == NULL || strlen(result) > 128 || !g_regex_match_simple("^[A-Za-z0-9_]+(\\+[A-Za-z0-9_]+)*$", result, 0, 0)) {
		g_free(result); refuse(error, VENTURE_ERROR_VALIDATION, "OCR language must name installed languages joined with +"); return NULL;
	}
	return result;
}
VentureEntity *venture_ocr_service_queue(VentureOcrService *self, VentureEntity *document,
	const gchar *language, gboolean force, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) live = NULL, prior = NULL, job = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_OCR_JOB);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) extraction_error = NULL;
	g_autofree gchar *hash = NULL, *lang = NULL, *mime = NULL, *title = NULL, *prior_hash = NULL, *prior_language = NULL, *prior_state = NULL, *applied = NULL;
	gboolean reviewed = FALSE;
	guint pages;
	g_return_val_if_fail(VENTURE_IS_OCR_SERVICE(self) && VENTURE_IS_DOCUMENT(document), NULL);
	if (!ready(self, TRUE, error)) return NULL;
	live = live_record(self, document, error); if (live == NULL) return NULL;
	lang = language_for(self, language, error); if (lang == NULL) return NULL;
	bytes = source_bytes(self, live, &extraction_error);
	hash = bytes != NULL ? g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes) : g_strdup("");
	g_object_get(live, "mime-type", &mime, "title", &title, NULL);
	pages = bytes != NULL ? venture_ocr_engine_inspect(self->engine, bytes, mime, &extraction_error) : 0;
	venture_query_set_organization(query, venture_entity_get_organization_id(live));
	venture_query_set_limit(query, 1);
	venture_query_add_filter_int(query, "document-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(live), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	prior = venture_database_find_one(self->database, query, error); if (error != NULL && *error != NULL) return NULL;
	if (prior != NULL) {
		g_object_get(prior, "source-hash", &prior_hash, "language", &prior_language, "state", &prior_state, "reviewed", &reviewed, "applied-hash", &applied, NULL);
		if (!force && g_strcmp0(hash, prior_hash) == 0 && g_strcmp0(lang, prior_language) == 0 && g_strcmp0(prior_state, "cancelled") != 0)
			return g_steal_pointer(&prior);
	}
	job = VENTURE_ENTITY(venture_ocr_job_new());
	g_object_set(job, "organization-id", venture_entity_get_organization_id(live), "title", title,
		"document-id", venture_entity_get_id(live), "state", extraction_error != NULL ? "failed" : "pending", "language", lang,
		"error", extraction_error != NULL ? extraction_error->message : NULL,
		"source-hash", hash, "pages", (gint64)pages, "engine", venture_ocr_engine_describe(self->engine),
		"applied-hash", !reviewed ? applied : NULL, NULL);
	if (!save(self, job, actor, error)) return NULL;
	return g_steal_pointer(&job);
}
static gboolean apply_text(VentureOcrService *self, VentureEntity *job, gboolean explicit_review,
	const gchar *correction, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) document = NULL;
	g_autofree gchar *text = NULL, *current = NULL, *expected = NULL, *hash = NULL, *new_hash = NULL;
	gint64 document_id;
	g_object_get(job, "document-id", &document_id, "text", &text, "applied-hash", &expected, NULL);
	document = venture_database_get(self->database, VENTURE_TYPE_DOCUMENT, document_id, error);
	if (document == NULL) return FALSE;
	if (venture_entity_is_deleted(document) || venture_entity_get_organization_id(document) != venture_entity_get_organization_id(job))
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "OCR document is unavailable in this organization");
	g_object_get(document, "extracted-text", &current, NULL);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, current != NULL ? current : "", -1);
	if (!explicit_review && !venture_string_is_empty(current) && g_strcmp0(hash, expected) != 0) return TRUE;
	if (correction != NULL) { g_free(text); text = g_strdup(correction); }
	if (text == NULL) text = g_strdup("");
	if (strlen(text) > VENTURE_OCR_MAX_TEXT || !g_utf8_validate(text, -1, NULL))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Reviewed text exceeds the OCR text bound");
	g_object_set(document, "extracted-text", text, NULL);
	if (!venture_database_save(self->database, document, actor, error)) return FALSE;
	new_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
	g_object_set(job, "applied-hash", new_hash, "reviewed", explicit_review, NULL);
	return TRUE;
}
VentureEntity *venture_ocr_service_step(VentureOcrService *self, VentureEntity *job,
	GCancellable *cancellable, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL, document = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *state = NULL, *mime = NULL, *language = NULL, *hash = NULL, *expected = NULL, *text = NULL, *page = NULL, *combined = NULL, *engine = NULL;
	gint64 document_id, cursor, pages, attempts;
	gboolean ok = FALSE;
	g_return_val_if_fail(VENTURE_IS_OCR_SERVICE(self) && VENTURE_IS_OCR_JOB(job), NULL);
	if (!ready(self, TRUE, error)) return NULL;
	row = live_record(self, job, error); if (row == NULL) return NULL;
	g_object_get(row, "state", &state, "document-id", &document_id, "completed-pages", &cursor,
		"pages", &pages, "attempts", &attempts, "language", &language, "source-hash", &expected, "text", &text, "engine", &engine, NULL);
	if (g_strcmp0(state, "succeeded") == 0) return g_steal_pointer(&row);
	if (!(g_strcmp0(state, "pending") == 0 || g_strcmp0(state, "running") == 0)) {
		refuse(error, VENTURE_ERROR_CONFLICT, "Retry a failed or cancelled job before stepping it"); return NULL;
	}
	g_object_set(row, "state", "running", "attempts", attempts + 1, "error", NULL, NULL);
	self->busy = TRUE;
	if (!save(self, row, actor, error)) { self->busy = FALSE; return NULL; }
	document = venture_database_get(self->database, VENTURE_TYPE_DOCUMENT, document_id, &failure);
	if (document != NULL && (venture_entity_is_deleted(document) || venture_entity_get_organization_id(document) != venture_entity_get_organization_id(row))) {
		refuse(&failure, VENTURE_ERROR_NOT_FOUND, "OCR document is unavailable in this organization"); g_clear_object(&document);
	}
	if (document != NULL) bytes = source_bytes(self, document, &failure);
	if (g_strcmp0(engine, venture_ocr_engine_describe(self->engine)) != 0 && failure == NULL)
		refuse(&failure, VENTURE_ERROR_CONFLICT, "OCR engine changed; queue a fresh extraction");
	if (bytes != NULL && failure == NULL) {
		hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
		if (!venture_string_is_empty(expected) && g_strcmp0(hash, expected) != 0) refuse(&failure, VENTURE_ERROR_CONFLICT, "Attachment changed; queue a new extraction");
		else {
			g_object_get(document, "mime-type", &mime, NULL);
			if (pages == 0) {
				pages = venture_ocr_engine_inspect(self->engine, bytes, mime, &failure);
				if (pages > 0) g_object_set(row, "pages", pages, "source-hash", hash, NULL);
			}
			if (failure == NULL) page = venture_ocr_engine_extract(self->engine, bytes, mime, (guint)cursor, language, cancellable, &failure);
		}
	}
	if (page != NULL) {
		combined = g_strconcat(text != NULL ? text : "", cursor > 0 ? "\n\n" : "", page, NULL);
		if (strlen(combined) > VENTURE_OCR_MAX_TEXT) refuse(&failure, VENTURE_ERROR_VALIDATION, "Document text exceeds the OCR text bound");
	}
	if (failure != NULL || combined == NULL) {
		g_object_set(row, "state", failure != NULL && g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? "cancelled" : "failed",
			"error", failure != NULL ? failure->message : "Document or engine output is unavailable", NULL);
		ok = save(self, row, actor, error);
	} else if (venture_database_begin(self->database, error)) {
		g_object_set(row, "completed-pages", cursor + 1, "text", combined, "state", cursor + 1 == pages ? "succeeded" : "running", NULL);
		if (cursor + 1 == pages) {
			g_autoptr(GDateTime) now = venture_time_now();
			g_object_set(row, "extracted-at", now, NULL);
			ok = apply_text(self, row, FALSE, NULL, actor, error);
		} else ok = TRUE;
		if (ok) ok = save(self, row, actor, error);
		if (ok) ok = venture_database_commit(self->database, error); else venture_database_rollback(self->database);
	}
	self->busy = FALSE;
	return ok ? g_steal_pointer(&row) : NULL;
}
VentureEntity *venture_ocr_service_control(VentureOcrService *self, VentureEntity *job,
	const gchar *operation, const gchar *reviewed_text, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *state = NULL;
	gboolean ok = FALSE;
	g_return_val_if_fail(VENTURE_IS_OCR_SERVICE(self) && VENTURE_IS_OCR_JOB(job) && operation != NULL, NULL);
	if (!ready(self, FALSE, error)) return NULL;
	row = live_record(self, job, error); if (row == NULL) return NULL;
	g_object_get(row, "state", &state, NULL);
	if (!venture_database_begin(self->database, error)) return NULL;
	if (g_str_equal(operation, "cancel") && !g_str_equal(state, "succeeded")) { g_object_set(row, "state", "cancelled", NULL); ok = TRUE; }
	else if (g_str_equal(operation, "retry") && (g_str_equal(state, "failed") || g_str_equal(state, "cancelled"))) {
		g_object_set(row, "state", "pending", "error", NULL, NULL); ok = TRUE;
	} else if (g_str_equal(operation, "review") && g_str_equal(state, "succeeded")) ok = apply_text(self, row, TRUE, reviewed_text, actor, error);
	else refuse(error, VENTURE_ERROR_CONFLICT, "OCR operation does not apply to this state");
	if (ok) ok = save(self, row, actor, error);
	if (ok) ok = venture_database_commit(self->database, error); else venture_database_rollback(self->database);
	return ok ? g_steal_pointer(&row) : NULL;
}
VentureEntity *venture_ocr_service_batch(VentureOcrService *self, gint64 organization,
	gint64 after_id, guint limit, const gchar *language, gboolean force, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CAPTURE_ITEM);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) batch = NULL;
	g_autoptr(JsonNode) selection = json_node_new(JSON_NODE_ARRAY);
	g_autofree gchar *lang = NULL, *encoded = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_OCR_SERVICE(self) && TRUE, NULL);
	if (!ready(self, TRUE, error)) return NULL;
	if (organization <= 0 || limit == 0 || limit > VENTURE_OCR_MAX_BATCH || after_id < 0) {
		refuse(error, VENTURE_ERROR_VALIDATION, "OCR batch requires an organization, nonnegative cursor and limit from 1 to 25"); return NULL;
	}
	lang = language_for(self, language, error); if (lang == NULL) return NULL;
	venture_query_set_organization(query, organization); venture_query_set_limit(query, limit);
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "inbox", NULL);
	venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, after_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error); if (rows == NULL) return NULL;
	json_node_take_array(selection, json_array_new());
	for (i = 0; i < rows->len; i++) json_array_add_int_element(json_node_get_array(selection), venture_entity_get_id(g_ptr_array_index(rows, i)));
	encoded = venture_json_to_string(selection, FALSE);
	batch = VENTURE_ENTITY(venture_ocr_batch_new());
	g_object_set(batch, "organization-id", organization, "title", "Capture inbox OCR", "state", rows->len > 0 ? "pending" : "succeeded",
		"language", lang, "capture-ids", encoded, "total", (gint64)rows->len, "force", force, NULL);
	if (!save(self, batch, actor, error)) return NULL;
	return g_steal_pointer(&batch);
}
VentureEntity *venture_ocr_service_batch_step(VentureOcrService *self, VentureEntity *batch,
	gboolean cancel, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL, capture = NULL, document = NULL, job = NULL, updated = NULL;
	g_autoptr(JsonNode) selection = NULL;
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *encoded = NULL, *language = NULL, *state = NULL, *job_state = NULL;
	gint64 cursor, total, failed, job_id, document_id = 0;
	gboolean force;
	g_return_val_if_fail(VENTURE_IS_OCR_SERVICE(self) && VENTURE_IS_OCR_BATCH(batch), NULL);
	if (!ready(self, !cancel, error)) return NULL;
	row = live_record(self, batch, error); if (row == NULL) return NULL;
	g_object_get(row, "capture-ids", &encoded, "language", &language, "state", &state, "cursor", &cursor,
		"total", &total, "failed", &failed, "current-job-id", &job_id, "force", &force, NULL);
	if (g_strcmp0(state, "succeeded") == 0) return g_steal_pointer(&row);
	if (job_id > 0) job = venture_database_get(self->database, VENTURE_TYPE_OCR_JOB, job_id, &failure);
	if (cancel) {
		if (job != NULL) {
			g_object_get(job, "state", &job_state, NULL);
			if (g_strcmp0(job_state, "succeeded") != 0) {
				updated = venture_ocr_service_control(self, job, "cancel", NULL, actor, error);
				if (updated == NULL) return NULL;
			}
		}
		g_object_set(row, "state", "cancelled", NULL);
		if (!save(self, row, actor, error)) return NULL;
		return g_steal_pointer(&row);
	}
	selection = venture_json_parse(encoded, error); if (selection == NULL) return NULL;
	if (cursor < 0 || cursor >= total || total > VENTURE_OCR_MAX_BATCH ||
		!JSON_NODE_HOLDS_ARRAY(selection) || json_array_get_length(json_node_get_array(selection)) != (guint)total) {
		refuse(error, VENTURE_ERROR_VALIDATION, "OCR batch cursor is invalid"); return NULL;
	}
	if (job == NULL && failure == NULL) {
		gint64 capture_id = json_array_get_int_element(json_node_get_array(selection), (guint)cursor);
		capture = venture_database_get(self->database, VENTURE_TYPE_CAPTURE_ITEM, capture_id, &failure);
		if (capture != NULL && !venture_entity_is_deleted(capture) && venture_entity_get_organization_id(capture) == venture_entity_get_organization_id(row)) {
			g_object_get(capture, "document-id", &document_id, NULL);
			document = venture_database_get(self->database, VENTURE_TYPE_DOCUMENT, document_id, &failure);
			if (document != NULL && venture_entity_get_organization_id(document) == venture_entity_get_organization_id(row))
				job = venture_ocr_service_queue(self, document, language, force, actor, &failure);
		}
		if (job == NULL && failure == NULL) refuse(&failure, VENTURE_ERROR_NOT_FOUND, "Capture document is unavailable in this organization");
		if (job != NULL) {
			g_object_set(row, "current-job-id", venture_entity_get_id(job), "state", "running", NULL);
			if (!save(self, row, actor, error)) return NULL;
		}
	}
	if (job != NULL && failure == NULL) {
		g_object_get(job, "state", &job_state, NULL);
		if (g_strcmp0(job_state, "cancelled") == 0) {
			updated = venture_ocr_service_control(self, job, "retry", NULL, actor, &failure);
			g_set_object(&job, updated); g_clear_object(&updated);
		}
		if (job != NULL && g_strcmp0(job_state, "failed") != 0) {
			updated = venture_ocr_service_step(self, job, NULL, actor, &failure);
			g_set_object(&job, updated); g_clear_object(&updated);
		}
		g_clear_pointer(&job_state, g_free);
		if (job != NULL) g_object_get(job, "state", &job_state, NULL);
	}
	if (failure != NULL || g_strcmp0(job_state, "failed") == 0) {
		g_autofree gchar *message = NULL;
		if (job != NULL) g_object_get(job, "error", &message, NULL);
		g_object_set(row, "failed", failed + 1, "error", failure != NULL ? failure->message : message, NULL);
		cursor++;
	} else if (g_strcmp0(job_state, "succeeded") == 0) cursor++;
	g_object_set(row, "cursor", cursor, "state", cursor == total ? "succeeded" : "running", NULL);
	if (failure != NULL || g_strcmp0(job_state, "failed") == 0 || g_strcmp0(job_state, "succeeded") == 0)
		g_object_set(row, "current-job-id", (gint64)0, NULL);
	if (!save(self, row, actor, error)) return NULL;
	return g_steal_pointer(&row);
}
