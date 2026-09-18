/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_QR_SVG_H
#define VENTURE_QR_SVG_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/**
 * venture_qr_svg_render:
 * @text: what the code should say, at most 213 bytes
 * @module_size: pixels per module for the width and height attributes; 0
 *   means 6
 * @error: (out) (optional): refusal when @text is too long or empty
 *
 * Encodes @text as a QR symbol (ISO/IEC 18004, byte mode, error-correction
 * level M, versions 1 to 10, the best of the eight masks) and renders it as
 * a self-contained SVG with a four-module quiet zone. Nothing leaves the
 * process: an enrolment secret must never be sent to a chart service to be
 * drawn.
 *
 * Returns: (transfer full) (nullable): the SVG document, or %NULL on refusal
 */
gchar *venture_qr_svg_render(const gchar *text, guint module_size, GError **error);

/**
 * venture_qr_matrix:
 * @text: what the code should say, at most 213 bytes
 * @out_size: (out): modules per side
 * @error: (out) (optional): refusal when @text is too long or empty
 *
 * The symbol as a square of bytes, 1 for dark, row-major, for tests and
 * for any renderer other than SVG.
 *
 * Returns: (transfer full) (array) (nullable): @out_size squared modules
 */
guint8 *venture_qr_matrix(const gchar *text, guint *out_size, GError **error);

G_END_DECLS
#endif
