/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_OCR_ENGINE_H
#define VENTURE_OCR_ENGINE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_OCR_MAX_BYTES (20 * 1024 * 1024)
#define VENTURE_OCR_MAX_TEXT (1024 * 1024)
#define VENTURE_OCR_MAX_PAGES 50
#define VENTURE_OCR_MAX_DIMENSION 4096
#define VENTURE_OCR_MAX_BATCH 25
#define VENTURE_TYPE_OCR_ENGINE (venture_ocr_engine_get_type())
G_DECLARE_INTERFACE(VentureOcrEngine, venture_ocr_engine, VENTURE, OCR_ENGINE, GObject)
/**
 * VentureOcrEngineInterface:
 * @parent_iface: parent interface
 * @inspect: validate source bytes and return page count
 * @extract: extract exactly one zero-based page; no repository access
 * @describe: borrowed stable engine/version description
 *
 * Implementations run synchronously on the caller, never write records,
 * honor cancellation and all public bounds, and own their temporary files.
 */
struct _VentureOcrEngineInterface {
	GTypeInterface parent_iface;
	guint (*inspect)(VentureOcrEngine *, GBytes *, const gchar *, GError **);
	gchar *(*extract)(VentureOcrEngine *, GBytes *, const gchar *, guint, const gchar *, GCancellable *, GError **);
	const gchar *(*describe)(VentureOcrEngine *);
	/* Reserved for compatible provider interface extensions. */
	gpointer padding[8];
};
/**
 * venture_ocr_engine_inspect:
 * @self: engine
 * @bytes: bounded original bytes
 * @mime: declared supported MIME type
 * @error: (out) (optional): invalid source
 * Returns: number of pages, or zero on failure
 */
guint venture_ocr_engine_inspect(VentureOcrEngine *self, GBytes *bytes, const gchar *mime, GError **error);
/**
 * venture_ocr_engine_extract:
 * @self: engine
 * @bytes: original bytes
 * @mime: supported MIME type
 * @page: zero-based page index
 * @language: installed language names joined with +
 * @cancellable: (nullable): active operation cancellation
 * @error: (out) (optional): bounded extraction failure
 * Returns: (transfer full) (nullable): UTF-8 text for this page
 */
gchar *venture_ocr_engine_extract(VentureOcrEngine *self, GBytes *bytes, const gchar *mime,
	guint page, const gchar *language, GCancellable *cancellable, GError **error);
/**
 * venture_ocr_engine_describe:
 * @self: engine
 * Returns: (transfer none): stable engine/version provenance
 */
const gchar *venture_ocr_engine_describe(VentureOcrEngine *self);
/**
 * venture_ocr_local_check:
 * @executable: operator-configured executable name or absolute path
 * @error: (out) (optional): missing binary or build dependency
 * Returns: whether the local dependency chain is available
 */
gboolean venture_ocr_local_check(const gchar *executable, GError **error);
/**
 * venture_ocr_local_new:
 * @executable: operator-configured Tesseract executable
 * @timeout_ms: per-process deadline, from 1 to 30000 milliseconds
 * @error: (out) (optional): configuration or version probe failure
 * Returns: (transfer full) (nullable): a local Tesseract/Poppler engine
 */
VentureOcrEngine *venture_ocr_local_new(const gchar *executable, guint timeout_ms, GError **error);
G_END_DECLS
#endif
