/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <glib/gstdio.h>
#ifdef VENTURE_HAVE_OCR_RASTER
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif
#ifdef VENTURE_HAVE_POPPLER
#include <poppler.h>
#include <cairo.h>
#endif

typedef struct { GObject parent; gchar *executable, *description; guint timeout_ms; } VentureOcrLocal;
typedef struct { GObjectClass parent; } VentureOcrLocalClass;
static void local_interface_init(VentureOcrEngineInterface *iface);
GType venture_ocr_local_get_type(void);
G_DEFINE_TYPE_WITH_CODE(VentureOcrLocal, venture_ocr_local, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_OCR_ENGINE, local_interface_init))
static void local_finalize(GObject *object)
{
	VentureOcrLocal *self = (VentureOcrLocal *)object;
	g_free(self->executable); g_free(self->description);
	G_OBJECT_CLASS(venture_ocr_local_parent_class)->finalize(object);
}
static void venture_ocr_local_class_init(VentureOcrLocalClass *klass) { G_OBJECT_CLASS(klass)->finalize = local_finalize; }
static void venture_ocr_local_init(VentureOcrLocal *self) { self->timeout_ms = 30000; }

/* A private context keeps this synchronous adapter from dispatching unrelated
 * application/database callbacks while a page is half-written. All three
 * callbacks are drained, including after timeout, before stack state dies. */
typedef struct { GSubprocess *process; GMainContext *context; GCancellable *io; GString *out;
	guint pending; gboolean stopped; GError *error; } ProcessRun;
typedef struct { ProcessRun *run; GInputStream *stream; gboolean stdout_stream; gchar buffer[8192]; } ReadPipe;
static void stop_process(ProcessRun *run)
{
	if (!run->stopped) { run->stopped = TRUE; g_subprocess_force_exit(run->process); g_cancellable_cancel(run->io); }
}
static gboolean process_deadline(gpointer data)
{
	ProcessRun *run = data;
	if (run->error == NULL) g_set_error_literal(&run->error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "OCR process exceeded its deadline");
	stop_process(run); return G_SOURCE_REMOVE;
}
static void process_cancelled(GCancellable *cancellable, gpointer data)
{
	ProcessRun *run = data;
	(void)cancellable;
	/* Cancellation may arrive from another thread; force_exit and
	 * cancellable are thread-safe, and errors are decided on our context. */
	g_subprocess_force_exit(run->process); g_cancellable_cancel(run->io);
}
static void read_pipe_done(GObject *source, GAsyncResult *result, gpointer data)
{
	ReadPipe *pipe = data;
	ProcessRun *run = pipe->run;
	g_autoptr(GError) error = NULL;
	gssize size = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
	if (size > 0) {
		if (pipe->stdout_stream) {
			if (run->out->len + (gsize)size > VENTURE_OCR_MAX_TEXT) {
				if (run->error == NULL) g_set_error_literal(&run->error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "OCR output exceeds the text limit");
				stop_process(run);
			} else g_string_append_len(run->out, pipe->buffer, size);
		}
		g_input_stream_read_async(pipe->stream, pipe->buffer, sizeof pipe->buffer, G_PRIORITY_DEFAULT,
			run->io, read_pipe_done, pipe);
		return;
	}
	if (error != NULL && run->error == NULL) run->error = g_steal_pointer(&error);
	run->pending--;
}
static void process_waited(GObject *source, GAsyncResult *result, gpointer data)
{
	ProcessRun *run = data;
	g_autoptr(GError) error = NULL;
	if (!g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error) && run->error == NULL)
		run->error = g_steal_pointer(&error);
	run->pending--;
}
static gchar *run_process(const gchar *const *argv, guint timeout_ms, GCancellable *cancellable, GError **error)
{
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GCancellable) io = g_cancellable_new();
	g_autoptr(GSource) timer = g_timeout_source_new(timeout_ms);
	g_autoptr(GString) output = g_string_new(NULL);
	ProcessRun run;
	ReadPipe pipes[2];
	gulong cancelled = 0;
	guint i;
	process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
	if (process == NULL) return NULL;
	run.process = process; run.context = context; run.io = io; run.out = output; run.pending = 3; run.stopped = FALSE; run.error = NULL;
	g_main_context_push_thread_default(context);
	for (i = 0; i < 2; i++) {
		pipes[i].run = &run; pipes[i].stdout_stream = i == 0;
		pipes[i].stream = i == 0 ? g_subprocess_get_stdout_pipe(process) : g_subprocess_get_stderr_pipe(process);
		g_input_stream_read_async(pipes[i].stream, pipes[i].buffer, sizeof pipes[i].buffer,
			G_PRIORITY_DEFAULT, io, read_pipe_done, &pipes[i]);
	}
	g_subprocess_wait_async(process, NULL, process_waited, &run);
	g_source_set_callback(timer, process_deadline, &run, NULL); g_source_attach(timer, context);
	if (cancellable != NULL) cancelled = g_cancellable_connect(cancellable, G_CALLBACK(process_cancelled), &run, NULL);
	while (run.pending > 0) g_main_context_iteration(context, TRUE);
	if (cancelled != 0) g_cancellable_disconnect(cancellable, cancelled);
	g_source_destroy(timer);
	g_main_context_pop_thread_default(context);
	if (run.error != NULL) { g_propagate_error(error, run.error); return NULL; }
	if (cancellable != NULL && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
	if (!g_subprocess_get_successful(process)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
			"Tesseract failed; check the selected language data and attachment format"); return NULL;
	}
	if (memchr(output->str, '\0', output->len) != NULL || !g_utf8_validate(output->str, output->len, NULL)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "OCR output is not UTF-8 text"); return NULL;
	}
	return g_string_free(g_steal_pointer(&output), FALSE);
}

gboolean venture_ocr_local_check(const gchar *executable, GError **error)
{
#if defined(VENTURE_HAVE_POPPLER) && defined(VENTURE_HAVE_OCR_RASTER)
	g_autofree gchar *program = g_find_program_in_path(executable != NULL ? executable : "tesseract");
	if (program != NULL) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "OCR enabled but Tesseract is not installed or executable");
#else
	(void)executable;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "OCR requires a server built with Poppler and GdkPixbuf");
#endif
	return FALSE;
}
#ifdef VENTURE_HAVE_OCR_RASTER
static void image_size(GdkPixbufLoader *loader, gint width, gint height, gpointer data)
{
	gboolean *valid = data;
	*valid = width > 0 && height > 0 && width <= VENTURE_OCR_MAX_DIMENSION && height <= VENTURE_OCR_MAX_DIMENSION;
	if (!*valid) gdk_pixbuf_loader_set_size(loader, 1, 1);
}
static GdkPixbuf *load_image(GBytes *bytes, GError **error)
{
	g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
	gboolean valid = FALSE;
	gsize size;
	gconstpointer data = g_bytes_get_data(bytes, &size);
	g_signal_connect(loader, "size-prepared", G_CALLBACK(image_size), &valid);
	if (!gdk_pixbuf_loader_write(loader, data, size, error)) { gdk_pixbuf_loader_close(loader, NULL); return NULL; }
	if (!gdk_pixbuf_loader_close(loader, error)) return NULL;
	if (!valid || gdk_pixbuf_loader_get_pixbuf(loader) == NULL) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Image dimensions exceed the OCR raster bound"); return NULL;
	}
	return g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
}
#endif
static guint local_inspect(VentureOcrEngine *engine, GBytes *bytes, const gchar *mime, GError **error)
{
	(void)engine;
	if (g_strcmp0(mime, "application/pdf") == 0) {
#ifdef VENTURE_HAVE_POPPLER
		g_autoptr(PopplerDocument) pdf = poppler_document_new_from_bytes(bytes, NULL, error);
		gint pages;
		if (pdf == NULL) return 0;
		pages = poppler_document_get_n_pages(pdf);
		if (pages > 0 && pages <= VENTURE_OCR_MAX_PAGES) return (guint)pages;
#endif
	} else if (g_strcmp0(mime, "image/png") == 0 || g_strcmp0(mime, "image/jpeg") == 0 || g_strcmp0(mime, "image/webp") == 0) {
#ifdef VENTURE_HAVE_OCR_RASTER
		g_autoptr(GdkPixbuf) image = load_image(bytes, error);
		if (image != NULL) return 1;
		return 0;
#endif
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "OCR supports bounded PNG, JPEG, WebP and PDF documents (at most 50 pages)");
	return 0;
}
static gchar *local_extract(VentureOcrEngine *engine, GBytes *bytes, const gchar *mime,
	guint page, const gchar *language, GCancellable *cancellable, GError **error)
{
	VentureOcrLocal *self = (VentureOcrLocal *)engine;
	g_autofree gchar *directory = NULL, *path = NULL, *text = NULL;
	gboolean written = FALSE;
	const gchar *argv[7];
	directory = g_dir_make_tmp("venture-ocr-XXXXXX", error);
	if (directory == NULL) return NULL;
	path = g_build_filename(directory, "page.png", NULL);
	if (g_strcmp0(mime, "application/pdf") == 0) {
#ifdef VENTURE_HAVE_POPPLER
		g_autoptr(PopplerDocument) pdf = poppler_document_new_from_bytes(bytes, NULL, error);
		g_autoptr(PopplerPage) source = NULL;
		gdouble width, height, scale;
		cairo_surface_t *surface;
		cairo_t *cr;
		cairo_status_t status;
		if (pdf == NULL) goto done;
		source = poppler_document_get_page(pdf, (gint)page);
		if (source == NULL) goto done;
		text = poppler_page_get_text(source);
		if (text != NULL && *g_strstrip(text) != '\0') goto done;
		g_clear_pointer(&text, g_free);
		poppler_page_get_size(source, &width, &height);
		if (!(width > 0 && height > 0 && width < 100000 && height < 100000)) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "PDF page geometry is invalid"); goto done;
		}
		scale = MIN(150.0 / 72.0, MIN(VENTURE_OCR_MAX_DIMENSION / width, VENTURE_OCR_MAX_DIMENSION / height));
		surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, MAX(1, (gint)(width * scale)), MAX(1, (gint)(height * scale)));
		cr = cairo_create(surface); cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
		cairo_scale(cr, scale, scale); poppler_page_render(source, cr); cairo_destroy(cr);
		status = cairo_surface_write_to_png(surface, path); cairo_surface_destroy(surface);
		if (status != CAIRO_STATUS_SUCCESS) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Could not rasterize PDF page"); goto done; }
		written = TRUE;
#endif
	} else {
#ifdef VENTURE_HAVE_OCR_RASTER
		g_autoptr(GdkPixbuf) image = load_image(bytes, error);
		if (image == NULL) goto done;
		written = gdk_pixbuf_save(image, path, "png", error, NULL);
#endif
	}
	if (!written) goto done;
	argv[0] = self->executable; argv[1] = path; argv[2] = "stdout"; argv[3] = "-l"; argv[4] = language; argv[5] = NULL;
	text = run_process(argv, self->timeout_ms, cancellable, error);
done:
	if (path != NULL) g_unlink(path);
	/* The engine owns exactly one artifact in this private directory. */
	g_rmdir(directory);
	return g_steal_pointer(&text);
}
static const gchar *local_describe(VentureOcrEngine *engine) { return ((VentureOcrLocal *)engine)->description; }
static void local_interface_init(VentureOcrEngineInterface *iface)
{
	iface->inspect = local_inspect; iface->extract = local_extract; iface->describe = local_describe;
}
VentureOcrEngine *venture_ocr_local_new(const gchar *executable, guint timeout_ms, GError **error)
{
	VentureOcrLocal *self;
	g_autofree gchar *version = NULL;
	const gchar *argv[3];
	if (!venture_ocr_local_check(executable, error)) return NULL;
	if (timeout_ms == 0 || timeout_ms > 30000) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "OCR process deadline must be from 1 to 30000 milliseconds"); return NULL;
	}
	argv[0] = executable != NULL ? executable : "tesseract"; argv[1] = "--version"; argv[2] = NULL;
	version = run_process(argv, MIN(timeout_ms, 2000), NULL, error);
	if (version == NULL) return NULL;
	g_strdelimit(version, "\r\n", ' ');
	if (strlen(version) > 256) version[256] = '\0';
	self = g_object_new(venture_ocr_local_get_type(), NULL);
	self->executable = g_find_program_in_path(argv[0]); self->timeout_ms = timeout_ms;
	self->description = g_strdup(version);
	return VENTURE_OCR_ENGINE(self);
}
