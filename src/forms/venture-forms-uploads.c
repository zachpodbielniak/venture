/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-forms-private.h"
#include <glib/gstdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#define UPLOAD_MAX_FILE (20 * 1024 * 1024)
#define UPLOAD_MAX_BODY (32 * 1024 * 1024)
#define UPLOAD_MAX_ROWS 10000
#define UPLOAD_WRITE "venture-forms-upload-write"
#define UPLOAD_DOWNLOAD "venture-forms-upload-download"

typedef struct {
	VentureFormsUploadScanner scanner;
	gpointer data;
	GDestroyNotify destroy;
} UploadScanner;

static void upload_scanner_free(gpointer data)
{
	UploadScanner *scanner = data;
	if (scanner->destroy != NULL) scanner->destroy(scanner->data);
	g_free(scanner);
}

void venture_forms_set_upload_scanner(VentureDatabase *database, VentureFormsUploadScanner callback,
	gpointer data, GDestroyNotify destroy)
{
	UploadScanner *scanner = g_new0(UploadScanner, 1);
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	scanner->scanner = callback; scanner->data = data; scanner->destroy = destroy;
	g_object_set_data_full(G_OBJECT(database), "venture-forms-upload-scanner", scanner, upload_scanner_free);
}

static gboolean upload_fail(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "Upload", "%s", message); return FALSE;
}

/* A private context drains only scanner I/O, never application callbacks.
 * Output is discarded so a noisy scanner cannot fill memory or leak bytes. */
typedef struct {
	GSubprocess *process;
	GCancellable *io;
	GError *error;
	gboolean done;
	gboolean expired;
	pid_t group;
} UploadScanRun;

static gboolean upload_scan_timeout(gpointer data)
{
	UploadScanRun *run = data;
	run->expired = TRUE;
	if (run->group > 0) kill(-run->group, SIGKILL);
	g_subprocess_force_exit(run->process);
	g_cancellable_cancel(run->io);
	return G_SOURCE_REMOVE;
}

static void upload_scan_done(GObject *source, GAsyncResult *result, gpointer data)
{
	UploadScanRun *run = data;
	g_subprocess_communicate_finish(G_SUBPROCESS(source), result, NULL, NULL, &run->error);
	run->done = TRUE;
}

/* Only async-signal-safe operations are permitted after fork. A separate
 * session lets a deadline terminate scanner children holding inherited pipes. */
static void upload_scan_child(gpointer data)
{
	(void)data;
	if (setsid() < 0) _exit(127);
}

static gboolean upload_scan_process(const gchar *executable, const gchar *mime,
	GBytes *bytes, gint64 deadline, GError **error)
{
	const gchar *argv[] = { executable, mime, NULL };
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GCancellable) io = g_cancellable_new();
	g_autoptr(GSource) timer = g_timeout_source_new(0);
	UploadScanRun run;
	gboolean accepted;
	if (g_get_monotonic_time() >= deadline)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Upload scanner deadline exhausted");
		return FALSE;
	}
	g_subprocess_launcher_set_child_setup(launcher, upload_scan_child, NULL, NULL);
	process = g_subprocess_launcher_spawnv(launcher, argv, error);
	if (process == NULL) return FALSE;
	run.process = process; run.io = io; run.error = NULL; run.done = FALSE; run.expired = FALSE;
	run.group = g_subprocess_get_identifier(process) != NULL ? (pid_t)g_ascii_strtoll(g_subprocess_get_identifier(process), NULL, 10) : 0;
	g_main_context_push_thread_default(context);
	g_source_set_ready_time(timer, deadline);
	g_source_set_callback(timer, upload_scan_timeout, &run, NULL);
	g_source_attach(timer, context);
	g_subprocess_communicate_async(process, bytes, io, upload_scan_done, &run);
	while (!run.done) g_main_context_iteration(context, TRUE);
	g_source_destroy(timer);
	g_main_context_pop_thread_default(context);
	/* Cancellation of communicate cancels its wait too. Reap the killed
	 * child before releasing the buffers or accepting another upload. */
	if (run.expired)
	{
		g_clear_error(&run.error);
		g_subprocess_wait(process, NULL, NULL);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Upload scanner deadline exhausted");
		return FALSE;
	}
	if (run.error != NULL)
	{
		g_subprocess_force_exit(process); g_subprocess_wait(process, NULL, NULL);
		g_propagate_error(error, run.error); return FALSE;
	}
	accepted = g_subprocess_get_successful(process);
	if (accepted) return TRUE;
	if (g_subprocess_get_if_exited(process) && g_subprocess_get_exit_status(process) == 1)
		return upload_fail(error, "the file scanner refused this attachment");
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Upload scanner failed");
	return FALSE;
}

/* OS failures are operational, not evidence that a visitor chose a bad file.
 * Only the operation and strerror are safe to expose to the boundary logger. */
static gboolean upload_storage_fail(GError **error, const gchar *operation, gint code)
{
	g_set_error(error, G_IO_ERROR, g_io_error_from_errno(code),
		"Attachment storage %s failed: %s", operation, g_strerror(code));
	return FALSE;
}

static gchar *upload_root(VentureDatabase *database, GError **error)
{
	GWeakRef *reference = g_object_get_data(G_OBJECT(database), "venture-forms-payment-context");
	g_autoptr(VentureContext) context = reference != NULL ? g_weak_ref_get(reference) : NULL;
	if (context == NULL) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Attachment storage is unavailable"); return NULL; }
	return g_build_filename(venture_config_get_state_dir(venture_context_get_config(context)), "attachments", NULL);
}

static gint64 upload_max_bytes(const VentureFormsField *field)
{
	return field->file_max_bytes > 0 ? MIN(field->file_max_bytes, UPLOAD_MAX_FILE) : 5 * 1024 * 1024;
}
static guint upload_max_count(const VentureFormsField *field)
{
	return field->file_max_count > 0 ? MIN(field->file_max_count, 10) : 1;
}
static gboolean upload_type_allowed(const gchar *types, const gchar *mime)
{
	g_auto(GStrv) choices = g_strsplit(venture_string_is_empty(types) ? "application/pdf,image/png,image/jpeg,text/plain" : types, ",", -1);
	guint i;
	for (i = 0; choices[i] != NULL; i++) if (g_strcmp0(g_strstrip(choices[i]), mime) == 0) return TRUE;
	return FALSE;
}

gboolean venture_forms_upload_field_validate(VentureEntity *entity, GError **error)
{
	gint64 bytes = venture_forms_get_int(entity, "file-max-bytes"), count = venture_forms_get_int(entity, "file-max-count");
	g_autofree gchar *types = venture_forms_get_string(entity, "file-types"), *fallback = venture_forms_get_string(entity, "default-value"), *mapping = venture_forms_get_string(entity, "maps-to"), *contact = venture_forms_get_string(entity, "contact-field");
	g_auto(GStrv) choices = NULL;
	VentureFormFieldKind kind;
	guint i;
	g_object_get(entity, "kind", &kind, NULL);
	if (bytes < 0 || bytes > UPLOAD_MAX_FILE || count < 0 || count > 10) return upload_fail(error, "file size must be 0..20 MiB and count 0..10");
	if (!venture_string_is_empty(types))
	{
		if (strlen(types) > 256) return upload_fail(error, "allowed types are too long");
		choices = g_strsplit(types, ",", -1);
		for (i = 0; choices[i] != NULL; i++)
			if (!upload_type_allowed(NULL, g_strstrip(choices[i]))) return upload_fail(error, "allowed types must be PDF, PNG, JPEG or plain text MIME names");
	}
	if (kind == VENTURE_FORM_FIELD_FILE && (!venture_string_is_empty(fallback) || !venture_string_is_empty(mapping) ||
	    !venture_string_is_empty(contact) || venture_forms_get_bool(entity, "allow-prefill")))
		return upload_fail(error, "file questions cannot have defaults, URL/contact prefill or lead mappings");
	return TRUE;
}

gboolean venture_forms_upload_definition(GPtrArray *fields, GError **error)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->kind == VENTURE_FORM_FIELD_FILE && venture_entity_registry_lookup(venture_entity_registry_get_default(), "document") == G_TYPE_INVALID)
			return upload_fail(error, "file questions require the documents module");
	}
	return TRUE;
}

gboolean venture_forms_upload_check_write(VentureEntity *entity, GError **error)
{
	if (VENTURE_IS_FORM_UPLOAD(entity) && g_object_get_data(G_OBJECT(entity), UPLOAD_WRITE) == NULL)
		return upload_fail(error, "private uploads belong to the form service; use response erasure or retention");
	return TRUE;
}

static gboolean upload_save(VentureDatabase *database, VentureEntity *upload, GError **error)
{
	g_object_set_data(G_OBJECT(upload), UPLOAD_WRITE, GINT_TO_POINTER(1));
	g_object_set(upload, "generation", venture_forms_get_int(upload, "generation") + 1, NULL);
	return venture_database_save(database, upload, NULL, error);
}

static const VentureFormsField *upload_field(GPtrArray *fields, const gchar *key)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->kind != VENTURE_FORM_FIELD_FILE) continue;
		if (g_strcmp0(field->key, key) == 0) return field;
		if (field->group_key != NULL)
		{
			g_autofree gchar *prefix = g_strconcat(field->group_key, "[", NULL);
			const gchar *cursor;
			if (!g_str_has_prefix(key, prefix)) continue;
			cursor = key + strlen(prefix);
			if (!g_ascii_isdigit(*cursor)) continue;
			while (g_ascii_isdigit(*cursor)) cursor++;
			if (g_str_has_prefix(cursor, "]["))
			{
				g_autofree gchar *suffix = g_strconcat("][", field->key, "]", NULL);
				if (g_str_equal(cursor, suffix)) return field;
			}
		}
	}
	return NULL;
}

static VentureEntity *upload_find(VentureDatabase *database, VentureEntity *form,
	const gchar *key, const gchar *token, GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(VentureEntity) upload = NULL;
	g_autoptr(GDateTime) expiry = NULL;
	g_autofree gchar *hash = NULL, *stored_key = NULL;
	guint i;
	if (token == NULL || strlen(token) != 64) goto missing;
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(token[i])) goto missing;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "token-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
	upload = venture_database_find_one(database, query, NULL);
	if (upload == NULL || venture_forms_get_int(upload, "document-id") > 0 || venture_forms_get_int(upload, "response-id") > 0) goto missing;
	stored_key = venture_forms_get_string(upload, "field-key"); g_object_get(upload, "expires-at", &expiry, NULL);
	if (g_strcmp0(key, stored_key) != 0 || expiry == NULL || g_date_time_compare(now, expiry) >= 0) goto missing;
	return g_steal_pointer(&upload);
missing:
	upload_fail(error, "a file is unavailable; choose it again"); return NULL;
}

/* Negative controls are generated by the native removal checkboxes. They
 * remove only a bearer the visitor already has; cleanup follows expiry. */
void venture_forms_upload_normalize(GPtrArray *fields, GHashTable *answers)
{
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, answers);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		GPtrArray *values = value;
		g_autoptr(GHashTable) removed = g_hash_table_new(g_str_hash, g_str_equal);
		guint i;
		if (upload_field(fields, key) == NULL) continue;
		for (i = 0; i < values->len; i++)
		{
			const gchar *token = g_ptr_array_index(values, i);
			if (*token == '!') g_hash_table_add(removed, (gpointer)(token + 1));
		}
		/* Copy keys: removing array elements otherwise invalidates the set. */
		{
			GPtrArray *kept = g_ptr_array_new_with_free_func(g_free);
			for (i = 0; i < values->len; i++)
			{
				const gchar *token = g_ptr_array_index(values, i);
				if (*token != '\0' && *token != '!' && !g_hash_table_contains(removed, token)) g_ptr_array_add(kept, g_strdup(token));
			}
			g_hash_table_iter_replace(&iter, kept);
		}
	}
}

void venture_forms_upload_check(VentureDatabase *database, VentureEntity *form, GDateTime *now,
	const VentureFormsField *field, GPtrArray *values, JsonObject *answers, JsonObject *errors, GString *summary)
{
	g_autoptr(JsonArray) accepted = json_array_new();
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	guint i;
	if (values != NULL && values->len > upload_max_count(field)) goto refused;
	for (i = 0; values != NULL && i < values->len; i++)
	{
		const gchar *token = g_ptr_array_index(values, i);
		g_autoptr(VentureEntity) upload = upload_find(database, form, field->base_key != NULL ? field->base_key : field->key, token, now, NULL);
		g_autofree gchar *mime = upload != NULL ? venture_forms_get_string(upload, "mime-type") : NULL;
		if (upload == NULL || g_hash_table_contains(seen, token) || venture_forms_get_int(upload, "size-bytes") > upload_max_bytes(field) || !upload_type_allowed(field->file_types, mime)) goto refused;
		g_hash_table_add(seen, (gpointer)token); json_array_add_string_element(accepted, token);
	}
	if (field->required && json_array_get_length(accepted) == 0) goto refused;
	if (json_array_get_length(accepted) > 0)
	{
		g_string_append_printf(summary, "%s: %u file(s)\n", field->label, json_array_get_length(accepted));
		json_object_set_array_member(answers, field->key, g_steal_pointer(&accepted));
	}
	return;
refused:
	json_object_set_string_member(errors, field->key, "Choose available files within this question's count, size and type limits.");
}

static gchar *upload_sniff(GBytes *bytes, GError **error)
{
	gsize length;
	const guchar *data = g_bytes_get_data(bytes, &length);
	g_autofree gchar *type = NULL, *mime = NULL, *lower = NULL;
	gboolean uncertain = FALSE;
	if (length == 0) goto refused;
	if ((length >= 2 && (!memcmp(data, "MZ", 2) || !memcmp(data, "#!", 2))) || (length >= 4 && !memcmp(data, "\177ELF", 4))) goto refused;
	lower = g_ascii_strdown((const gchar *)data, (gssize)MIN(length, 65536));
	if (g_strstr_len(lower, (gssize)MIN(length, 65536), "<html") || g_strstr_len(lower, (gssize)MIN(length, 65536), "<svg") || g_strstr_len(lower, (gssize)MIN(length, 65536), "<!doctype html")) goto refused;
	type = g_content_type_guess(NULL, data, MIN(length, 65536), &uncertain); mime = g_content_type_get_mime_type(type);
	if (!upload_type_allowed(NULL, mime)) goto refused;
	if (g_str_equal(mime, "text/plain") && (!g_utf8_validate((const gchar *)data, (gssize)length, NULL) || memchr(data, 0, length) != NULL || uncertain)) goto refused;
	return g_steal_pointer(&mime);
refused:
	upload_fail(error, "file content is not an allowed PDF, PNG, JPEG or plain text file"); return NULL;
}

static gboolean upload_unlink(VentureDatabase *database, VentureEntity *upload, GError **error)
{
	g_autofree gchar *root = upload_root(database, error), *path = venture_forms_get_string(upload, "path"), *prefix = NULL;
	const gchar *name;
	gint directory, result, saved_errno;
	if (root == NULL) return FALSE;
	prefix = g_strconcat(root, G_DIR_SEPARATOR_S, NULL);
	if (path == NULL || !g_str_has_prefix(path, prefix)) return upload_fail(error, "file storage identity is invalid");
	name = path + strlen(prefix);
	if (!g_str_has_prefix(name, "form-upload-") || strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return upload_fail(error, "file storage identity is invalid");
	directory = g_open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
	if (directory < 0) return errno == ENOENT ? TRUE : upload_fail(error, "attachment storage cannot be opened for cleanup");
	result = unlinkat(directory, name, 0); saved_errno = errno; close(directory);
	return result == 0 || saved_errno == ENOENT ? TRUE : upload_fail(error, "attachment cleanup failed; retry retention or erasure");
}

static gboolean upload_quota(VentureDatabase *database, VentureEntity *form, const gchar *client,
	gsize incoming, GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) hour = g_date_time_add_hours(now, -1);
	gint64 total = 0, recent = 0, maximum = venture_forms_get_int(form, "upload-quota-bytes"), per_client = venture_forms_get_int(form, "upload-client-hourly-bytes");
	guint i;
	if (maximum <= 0) maximum = 100 * 1024 * 1024;
	if (per_client <= 0) per_client = 20 * 1024 * 1024;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_set_limit(query, UPLOAD_MAX_ROWS + 1); rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	if (rows->len >= UPLOAD_MAX_ROWS) return upload_fail(error, "upload storage is full; contact the form owner");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *digest = venture_forms_get_string(row, "client-hash");
		g_autoptr(GDateTime) created = NULL;
		gint64 size = venture_forms_get_int(row, "size-bytes");
		g_object_get(row, "created-at", &created, NULL);
		if (size < 0 || size > UPLOAD_MAX_FILE) return upload_fail(error, "upload storage metadata is invalid");
		total += size;
		if (g_strcmp0(client, digest) == 0 && created != NULL && g_date_time_compare(created, hour) >= 0) recent += size;
	}
	if (incoming > (guint64)MAX((gint64)0, maximum - total) || incoming > (guint64)MAX((gint64)0, per_client - recent))
		return upload_fail(error, "the form or client upload quota has been reached");
	return TRUE;
}

void venture_forms_upload_part_free(gpointer data)
{
	VentureFormsUploadPart *part = data;
	g_free(part->key); g_free(part->filename); g_bytes_unref(part->bytes); g_free(part);
}

static gboolean upload_scan_parts(VentureDatabase *database, GPtrArray *fields,
	GPtrArray *parts, GError **error)
{
	GWeakRef *reference = g_object_get_data(G_OBJECT(database), "venture-forms-payment-context");
	g_autoptr(VentureContext) context = reference != NULL ? g_weak_ref_get(reference) : NULL;
	g_autofree gchar *executable = NULL;
	UploadScanner *scanner = g_object_get_data(G_OBJECT(database), "venture-forms-upload-scanner");
	gint64 milliseconds = 10000, deadline;
	guint i;
	if (context != NULL) g_object_get(venture_context_get_config(context),
		"forms-scanner-executable", &executable, "forms-scanner-timeout-ms", &milliseconds, NULL);
	if (!venture_string_is_empty(executable) &&
	    (!g_path_is_absolute(executable) || milliseconds < 1 || milliseconds > 30000))
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid upload scanner configuration");
		return FALSE;
	}
	deadline = g_get_monotonic_time() + CLAMP(milliseconds, 1, 30000) * 1000;
	for (i = 0; i < parts->len; i++)
	{
		VentureFormsUploadPart *part = g_ptr_array_index(parts, i);
		const VentureFormsField *field = upload_field(fields, part->key);
		g_autofree gchar *mime = NULL;
		gsize length = g_bytes_get_size(part->bytes);
		if (field == NULL || length == 0 || length > (gsize)upload_max_bytes(field))
			return upload_fail(error, "unknown file question or exceeded file size/count");
		mime = upload_sniff(part->bytes, error);
		if (mime == NULL) return FALSE;
		if (!upload_type_allowed(field->file_types, mime))
			return upload_fail(error, "this file's content type is not allowed for the question");
		if (!venture_string_is_empty(executable) && !upload_scan_process(executable, mime, part->bytes, deadline, error)) return FALSE;
		if (scanner != NULL && scanner->scanner != NULL && !scanner->scanner(part->bytes, mime, scanner->data, error))
		{
			if (error != NULL && *error == NULL) upload_fail(error, "the file scanner refused this attachment");
			return FALSE;
		}
	}
	return TRUE;
}

gboolean venture_forms_receive_uploads(VentureDatabase *database, VentureEntity *form, VentureEntity *version,
	GPtrArray *fields, GHashTable *answers, GPtrArray *parts, const gchar *client, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) created = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *root = NULL, *key = venture_forms_get_string(form, "ticket-key"), *digest = NULL;
	g_autoptr(GDateTime) expires = g_date_time_add_hours(now, 1);
	gint directory = -1;
	guint i;
	gboolean committed = FALSE;
	if (parts->len == 0) return TRUE;
	if (parts->len > 100) return upload_fail(error, "too many files in one request");
	if (!venture_forms_upload_definition(fields, error)) return FALSE;
	root = upload_root(database, error); if (root == NULL) return FALSE;
	if (!upload_scan_parts(database, fields, parts, error)) return FALSE;
	digest = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), client != NULL ? client : "unknown", -1);
	if (!venture_database_begin(database, error)) return FALSE;
	if (g_mkdir_with_parents(root, 0700) != 0 || (directory = g_open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0)) < 0)
	{ upload_storage_fail(error, "open directory", errno); goto fail; }
	for (i = 0; i < parts->len; i++)
	{
		VentureFormsUploadPart *part = g_ptr_array_index(parts, i);
		const VentureFormsField *field = upload_field(fields, part->key);
		GPtrArray *previous = g_hash_table_lookup(answers, part->key);
		g_autofree gchar *mime = NULL, *token = NULL, *hash = NULL, *uuid = NULL, *filename = NULL, *path = NULL;
		g_autoptr(VentureEntity) upload = NULL;
		gsize length, offset = 0;
		const guint8 *bytes = g_bytes_get_data(part->bytes, &length);
		gint fd, write_error = 0;
		if (field == NULL || length == 0 || length > (gsize)upload_max_bytes(field) || (previous != NULL && previous->len >= upload_max_count(field)))
		{ upload_fail(error, "unknown file question or exceeded file size/count"); goto fail; }
		if (venture_string_is_empty(part->filename) || strlen(part->filename) > 512 || !g_utf8_validate(part->filename, -1, NULL))
		{ upload_fail(error, "filename must be nonempty UTF-8 of at most 512 bytes"); goto fail; }
		mime = upload_sniff(part->bytes, error); if (mime == NULL) goto fail;
		if (!upload_type_allowed(field->file_types, mime)) { upload_fail(error, "this file's content type is not allowed for the question"); goto fail; }
		if (!upload_quota(database, form, digest, length, now, error)) goto fail;
		token = venture_generate_token(32); hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
		uuid = g_uuid_string_random(); filename = g_strconcat("form-upload-", uuid, NULL); path = g_build_filename(root, filename, NULL);
		fd = openat(directory, filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0) { upload_storage_fail(error, "create", errno); goto fail; }
		while (offset < length)
		{
			ssize_t wrote = write(fd, bytes + offset, length - offset);
			if (wrote < 0 && errno == EINTR) continue;
			if (wrote <= 0) { write_error = wrote < 0 ? errno : EIO; break; }
			offset += (gsize)wrote;
		}
		if (close(fd) != 0 && write_error == 0) write_error = errno;
		if (write_error != 0)
		{
			unlinkat(directory, filename, 0);
			upload_storage_fail(error, "write", write_error); goto fail;
		}
		upload = g_object_new(VENTURE_TYPE_FORM_UPLOAD, "organization-id", venture_entity_get_organization_id(form), "name", "Private form upload",
			"form-id", venture_entity_get_id(form), "version-id", venture_entity_get_id(version), "field-key", field->key,
			"filename", part->filename, "path", path, "token-hash", hash, "client-hash", digest, "size-bytes", (gint64)length,
			"mime-type", mime, "sensitive", field->sensitive, "expires-at", expires, NULL);
		g_ptr_array_add(created, g_object_ref(upload));
		if (!upload_save(database, upload, error)) goto fail;
		venture_forms_answers_add(answers, part->key, token);
	}
	committed = venture_database_commit(database, error);
	if (!committed) goto cleanup;
	close(directory); return TRUE;
fail:
	venture_database_rollback(database);
cleanup:
	for (i = 0; i < created->len; i++) upload_unlink(database, g_ptr_array_index(created, i), NULL);
	if (directory >= 0) close(directory);
	return FALSE;
}

gsize venture_forms_upload_body_limit(VentureDatabase *database, const gchar *token)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureEntity) form = venture_forms_find_live(database, token, now, NULL), version = NULL;
	g_autoptr(VentureQuery) query = NULL;
	if (form == NULL) return VENTURE_FORMS_MAX_BODY;
	/* Old fill-time tickets still name frozen versions. A later publication
	 * removing its file question must not revoke an in-flight body's bound. */
	query = venture_query_new(VENTURE_TYPE_FORM_VERSION);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "definition", VENTURE_FILTER_OP_LIKE, "%\"kind\":\"file\"%", NULL);
	venture_query_set_limit(query, 1); version = venture_database_find_one(database, query, NULL);
	return version != NULL ? UPLOAD_MAX_BODY : VENTURE_FORMS_MAX_BODY;
}

static gboolean upload_claim_answer(VentureDatabase *database, VentureEntity *form, const VentureFormsField *field,
	JsonObject *answers, const gchar *key, GDateTime *now, GHashTable *seen, GPtrArray *claimed, GError **error)
{
	JsonNode *node = json_object_get_member(answers, key);
	g_autoptr(JsonArray) metadata = json_array_new();
	g_autofree gchar *root = NULL;
	guint i;
	if (node == NULL) return TRUE;
	if (!JSON_NODE_HOLDS_ARRAY(node) || json_array_get_length(json_node_get_array(node)) > upload_max_count(field))
		return upload_fail(error, "invalid retained attachment list");
	root = upload_root(database, error); if (root == NULL) return FALSE;
	for (i = 0; i < json_array_get_length(json_node_get_array(node)); i++)
	{
		JsonNode *item = json_array_get_element(json_node_get_array(node), i);
		g_autoptr(VentureEntity) upload = NULL, document = NULL;
		g_autofree gchar *filename = NULL, *path = NULL, *mime = NULL;
		const gchar *token;
		JsonObject *entry;
		if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING) return upload_fail(error, "invalid attachment capability");
		token = json_node_get_string(item);
		if (g_hash_table_contains(seen, token)) return upload_fail(error, "a file may appear only once in a response");
		upload = upload_find(database, form, key, token, now, error); if (upload == NULL) return FALSE;
		filename = venture_forms_get_string(upload, "filename"); path = venture_forms_get_string(upload, "path"); mime = venture_forms_get_string(upload, "mime-type");
		if (venture_forms_get_int(upload, "size-bytes") > upload_max_bytes(field) || !upload_type_allowed(field->file_types, mime)) return upload_fail(error, "retained file no longer meets this question's limits");
		document = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", venture_entity_get_organization_id(form), "title", "Form attachment", "kind", "form_attachment",
			"form-upload-id", venture_entity_get_id(upload), "path", path, "mime-type", mime, "size-bytes", venture_forms_get_int(upload, "size-bytes"), NULL);
		if (!venture_document_service_save_attachment(venture_document_service_get(database), document, root, NULL, error)) return FALSE;
		/* A resumed draft can move to a newly private question. Never let
		 * an older upload row weaken the accepted version's download gate. */
		g_object_set(upload, "document-id", venture_entity_get_id(document),
			"sensitive", field->sensitive || venture_forms_get_bool(upload, "sensitive"), NULL);
		if (!upload_save(database, upload, error)) return FALSE;
		g_ptr_array_add(claimed, g_object_ref(upload)); g_hash_table_add(seen, g_strdup(token));
		entry = json_object_new(); json_object_set_int_member(entry, "upload_id", venture_entity_get_id(upload));
		json_object_set_int_member(entry, "document_id", venture_entity_get_id(document));
		json_object_set_string_member(entry, "name", filename); json_object_set_string_member(entry, "type", mime);
		json_object_set_int_member(entry, "size", venture_forms_get_int(upload, "size-bytes")); json_array_add_object_element(metadata, entry);
	}
	json_object_set_array_member(answers, key, g_steal_pointer(&metadata)); return TRUE;
}

gboolean venture_forms_upload_claim(VentureDatabase *database, VentureEntity *form, VentureEntity *response,
	GPtrArray *fields, GDateTime *now, GPtrArray **claimed, GError **error)
{
	const gchar *columns[] = { "answers", "sensitive-answers" };
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint c, i;
	*claimed = g_ptr_array_new_with_free_func(g_object_unref);
	for (c = 0; c < G_N_ELEMENTS(columns); c++)
	{
		g_autofree gchar *text = venture_forms_get_string(response, columns[c]), *output = NULL;
		g_autoptr(JsonNode) node = text != NULL ? json_from_string(text, NULL) : NULL;
		g_autoptr(GHashTable) visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		gboolean changed = FALSE;
		if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) continue;
		for (i = 0; i < fields->len; i++)
		{
			const VentureFormsField *field = g_ptr_array_index(fields, i);
			const gchar *key = field->base_key != NULL ? field->base_key : field->key;
			g_autofree gchar *identity = g_strdup_printf("%s:%s", field->group_key != NULL ? field->group_key : "", key);
			if (field->kind != VENTURE_FORM_FIELD_FILE || field->group_boundary != VENTURE_FORMS_GROUP_NONE || g_hash_table_contains(visited, identity)) continue;
			g_hash_table_add(visited, g_steal_pointer(&identity));
			if (field->group_key != NULL)
			{
				JsonNode *rows = json_object_get_member(json_node_get_object(node), field->group_key);
				guint j;
				if (rows == NULL || !JSON_NODE_HOLDS_ARRAY(rows)) continue;
				for (j = 0; j < json_array_get_length(json_node_get_array(rows)); j++)
				{
					JsonNode *row = json_array_get_element(json_node_get_array(rows), j);
					if (!JSON_NODE_HOLDS_OBJECT(row)) return upload_fail(error, "invalid retained repeated attachment");
					if (!upload_claim_answer(database, form, field, json_node_get_object(row), key, now, seen, *claimed, error)) return FALSE;
				}
			}
			else if (!upload_claim_answer(database, form, field, json_node_get_object(node), key, now, seen, *claimed, error)) return FALSE;
			changed = TRUE;
		}
		if (changed) { output = json_to_string(node, FALSE); g_object_set(response, columns[c], output, NULL); }
	}
	return TRUE;
}

gboolean venture_forms_upload_bind_response(VentureDatabase *database, VentureEntity *response, GPtrArray *claimed, GError **error)
{
	guint i;
	for (i = 0; claimed != NULL && i < claimed->len; i++)
	{
		VentureEntity *upload = g_ptr_array_index(claimed, i);
		g_object_set(upload, "response-id", venture_entity_get_id(response), "token-hash", NULL,
			"source-type", NULL, "source-id", (gint64)0, "expires-at", NULL, NULL);
		if (!upload_save(database, upload, error)) return FALSE;
	}
	return TRUE;
}

static gboolean upload_bind_node(VentureDatabase *database, VentureEntity *form, VentureEntity *source,
	JsonNode *node, GDateTime *expires, guint depth, GError **error)
{
	if (depth > 32) return upload_fail(error, "retained upload nesting is too deep");
	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObjectIter iter;
		JsonNode *child;
		json_object_iter_init(&iter, json_node_get_object(node));
		while (json_object_iter_next(&iter, NULL, &child)) if (!upload_bind_node(database, form, source, child, expires, depth + 1, error)) return FALSE;
	}
	else if (JSON_NODE_HOLDS_ARRAY(node))
	{
		guint i;
		for (i = 0; i < json_array_get_length(json_node_get_array(node)); i++)
			if (!upload_bind_node(database, form, source, json_array_get_element(json_node_get_array(node), i), expires, depth + 1, error)) return FALSE;
	}
	else if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
	{
		const gchar *token = json_node_get_string(node);
		g_autofree gchar *hash = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) upload = NULL;
		if (strlen(token) != 64) return TRUE;
		hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
		query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
		venture_query_set_organization(query, venture_entity_get_organization_id(form));
		venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
		venture_query_add_filter_string(query, "token-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
		upload = venture_database_find_one(database, query, NULL);
		if (upload == NULL || venture_forms_get_int(upload, "document-id") > 0 || venture_forms_get_int(upload, "response-id") > 0) return TRUE;
		g_object_set(upload, "source-type", venture_entity_get_entity_name(source), "source-id", venture_entity_get_id(source), "expires-at", expires, NULL);
		if (!upload_save(database, upload, error)) return FALSE;
	}
	return TRUE;
}

gboolean venture_forms_upload_bind_source(VentureDatabase *database, VentureEntity *form, VentureEntity *source,
	JsonObject *values, GDateTime *expires, GError **error)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, values);
	return upload_bind_node(database, form, source, node, expires, 0, error);
}

static gboolean upload_purge(VentureDatabase *database, VentureEntity *upload, GError **error)
{
	g_autoptr(VentureEntity) document = NULL;
	gint64 id = venture_forms_get_int(upload, "document-id");
	/* A failed unlink keeps its durable row and byte quota for a later
	 * retry. Never report successful erasure while bytes remain on disk. */
	if (!upload_unlink(database, upload, error)) return FALSE;
	if (id > 0)
	{
		document = venture_database_get(database, VENTURE_TYPE_DOCUMENT, id, NULL);
		if (document != NULL && !venture_database_purge(database, document, NULL, error)) return FALSE;
	}
	g_object_set_data(G_OBJECT(upload), UPLOAD_WRITE, GINT_TO_POINTER(1));
	return venture_database_purge(database, upload, NULL, error);
}

gboolean venture_forms_upload_purge_source(VentureDatabase *database, VentureEntity *source, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, venture_entity_get_organization_id(source));
	venture_query_set_include_deleted(query, TRUE); venture_query_set_limit(query, UPLOAD_MAX_ROWS + 1);
	if (VENTURE_IS_FORM_SUBMISSION(source)) venture_query_add_filter_int(query, "response-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(source), NULL);
	else
	{
		venture_query_add_filter_string(query, "source-type", VENTURE_FILTER_OP_EQ, venture_entity_get_entity_name(source), NULL);
		venture_query_add_filter_int(query, "source-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(source), NULL);
		venture_query_add_filter_int(query, "response-id", VENTURE_FILTER_OP_EQ, 0, NULL);
	}
	rows = venture_database_find(database, query, error); if (rows == NULL) return FALSE;
	if (rows->len > UPLOAD_MAX_ROWS) return upload_fail(error, "too many attachments for one cleanup operation");
	for (i = 0; i < rows->len; i++) if (!upload_purge(database, g_ptr_array_index(rows, i), error)) return FALSE;
	return TRUE;
}

gint64 venture_forms_upload_sweep(VentureDatabase *database, gint64 organization, guint limit, GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *cutoff = venture_time_to_string(now);
	guint i;
	venture_query_set_organization(query, organization); venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "response-id", VENTURE_FILTER_OP_EQ, 0, NULL);
	venture_query_add_filter_int(query, "document-id", VENTURE_FILTER_OP_EQ, 0, NULL);
	venture_query_add_filter_string(query, "expires-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL); venture_query_set_limit(query, CLAMP(limit, 1, 1000));
	rows = venture_database_find(database, query, error); if (rows == NULL) return -1;
	for (i = 0; i < rows->len; i++) if (!upload_purge(database, g_ptr_array_index(rows, i), error)) return -1;
	return rows->len;
}

GBytes *venture_forms_upload_read(VentureDatabase *database, VentureEntity *upload, GError **error)
{
	g_autoptr(VentureEntity) document = NULL, response = NULL;
	g_autofree gchar *root = upload_root(database, error);
	GBytes *bytes;
	if (root == NULL) return NULL;
	response = venture_database_get(database, VENTURE_TYPE_FORM_SUBMISSION, venture_forms_get_int(upload, "response-id"), NULL);
	if (response == NULL || venture_entity_get_organization_id(response) != venture_entity_get_organization_id(upload)) goto missing;
	document = venture_database_get(database, VENTURE_TYPE_DOCUMENT, venture_forms_get_int(upload, "document-id"), NULL);
	if (document == NULL || venture_forms_get_int(document, "form-upload-id") != venture_entity_get_id(upload)) goto missing;
	/* The route has checked the actual response and its sensitive-owner gate.
	 * This transient marker cannot be supplied through any record decoder. */
	g_object_set_data(G_OBJECT(document), UPLOAD_DOWNLOAD, GINT_TO_POINTER(1));
	bytes = venture_document_service_read_attachment(venture_document_service_get(database), document, root, UPLOAD_MAX_FILE, error);
	g_object_set_data(G_OBJECT(document), UPLOAD_DOWNLOAD, NULL); return bytes;
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Attachment not found"); return NULL;
}

/* Access exports describe the file without exporting a still-live claim that
 * could be replayed in another submission. Only this form's claims match. */
static void upload_export_node(JsonNode *node, GHashTable *claims, guint depth)
{
	guint i;
	if (node == NULL || depth > 16) return;
	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		g_autoptr(GList) members = json_object_get_members(json_node_get_object(node));
		GList *item;
		for (item = members; item != NULL; item = item->next)
			upload_export_node(json_object_get_member(json_node_get_object(node), item->data), claims, depth + 1);
	}
	else if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		for (i = 0; i < json_array_get_length(array); i++) upload_export_node(json_array_get_element(array, i), claims, depth + 1);
	}
	else if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
	{
		const gchar *value = json_node_get_string(node);
		g_autofree gchar *hash = NULL;
		JsonObject *metadata;
		if (value == NULL || strlen(value) != 64) return;
		hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, value, -1);
		metadata = g_hash_table_lookup(claims, hash);
		if (metadata != NULL) { json_node_init_object(node, metadata); }
	}
}

gboolean venture_forms_upload_export(VentureDatabase *database, gint64 form_id, JsonNode *answers, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) claims = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
	guint i;
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
	venture_query_set_limit(query, 0); venture_query_set_include_deleted(query, TRUE);
	rows = venture_database_find(database, query, error); if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *upload = g_ptr_array_index(rows, i);
		g_autofree gchar *hash = venture_forms_get_string(upload, "token-hash"), *name = venture_forms_get_string(upload, "filename"), *mime = venture_forms_get_string(upload, "mime-type");
		JsonObject *metadata;
		if (venture_string_is_empty(hash)) continue;
		metadata = json_object_new();
		json_object_set_int_member(metadata, "upload_id", venture_entity_get_id(upload));
		json_object_set_string_member(metadata, "name", name != NULL ? name : "");
		json_object_set_string_member(metadata, "type", mime != NULL ? mime : "");
		json_object_set_int_member(metadata, "size", venture_forms_get_int(upload, "size-bytes"));
		g_hash_table_insert(claims, g_steal_pointer(&hash), metadata);
	}
	upload_export_node(answers, claims, 0); return TRUE;
}
