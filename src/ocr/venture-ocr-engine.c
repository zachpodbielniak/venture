/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
G_DEFINE_INTERFACE(VentureOcrEngine, venture_ocr_engine, G_TYPE_OBJECT)
static void venture_ocr_engine_default_init(VentureOcrEngineInterface *iface) { (void)iface; }
guint venture_ocr_engine_inspect(VentureOcrEngine *self, GBytes *bytes, const gchar *mime, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_OCR_ENGINE(self), 0);
	g_return_val_if_fail(bytes != NULL, 0);
	if (g_bytes_get_size(bytes) == 0 || g_bytes_get_size(bytes) > VENTURE_OCR_MAX_BYTES ||
		VENTURE_OCR_ENGINE_GET_IFACE(self)->inspect == NULL) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "OCR source is empty, too large or unsupported"); return 0;
	}
	{
		guint pages = VENTURE_OCR_ENGINE_GET_IFACE(self)->inspect(self, bytes, mime, error);
		if (pages > VENTURE_OCR_MAX_PAGES || (pages == 0 && (error == NULL || *error == NULL))) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "OCR provider returned an invalid page count"); return 0;
		}
		return pages;
	}
}
gchar *venture_ocr_engine_extract(VentureOcrEngine *self, GBytes *bytes, const gchar *mime,
	guint page, const gchar *language, GCancellable *cancellable, GError **error)
{
	guint pages = venture_ocr_engine_inspect(self, bytes, mime, error);
	g_autofree gchar *text = NULL;
	if (pages == 0) return NULL;
	if (page >= pages || language == NULL || !g_regex_match_simple("^[A-Za-z0-9_]+(\\+[A-Za-z0-9_]+)*$", language, 0, 0) ||
		VENTURE_OCR_ENGINE_GET_IFACE(self)->extract == NULL) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "OCR page or language is invalid"); return NULL;
	}
	if (cancellable != NULL && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
	text = VENTURE_OCR_ENGINE_GET_IFACE(self)->extract(self, bytes, mime, page, language, cancellable, error);
	if (text != NULL && (strlen(text) > VENTURE_OCR_MAX_TEXT || !g_utf8_validate(text, -1, NULL))) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "OCR output exceeds the text bound or is not UTF-8"); return NULL;
	}
	return g_steal_pointer(&text);
}
const gchar *venture_ocr_engine_describe(VentureOcrEngine *self)
{
	g_return_val_if_fail(VENTURE_IS_OCR_ENGINE(self), "unavailable");
	return VENTURE_OCR_ENGINE_GET_IFACE(self)->describe != NULL ? VENTURE_OCR_ENGINE_GET_IFACE(self)->describe(self) : "unspecified";
}
