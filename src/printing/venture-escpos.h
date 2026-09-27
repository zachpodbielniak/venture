/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ESCPOS_H
#define VENTURE_ESCPOS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS

#define VENTURE_ESCPOS_DLE 0x10
#define VENTURE_ESCPOS_EOT 0x04
#define VENTURE_ESCPOS_ESC 0x1b
#define VENTURE_ESCPOS_GS 0x1d
#define VENTURE_ESCPOS_STATUS_PRINTER 1
#define VENTURE_ESCPOS_STATUS_PAPER 4
#define VENTURE_ESCPOS_STATUS_OFFLINE 0x08
#define VENTURE_ESCPOS_STATUS_PAPER_OUT 0x60
#define VENTURE_ESCPOS_STATUS_PAPER_NEAR_END 0x0c
#define VENTURE_ESCPOS_DEFAULT_FEED 4
#define VENTURE_ESCPOS_DEFAULT_TABS 4
#define VENTURE_ESCPOS_DEFAULT_QR_SIZE 6

/* Measured on the TM-T20IV-SP's full 576-dot print area: font A
 * (12 dots) is 48 columns and B (9 dots) is 64. The quoted 42/56
 * assumes 512 dots; do not correct these back without a printed ruler. */
#define VENTURE_ESCPOS_FONT_A_WIDTH 48
#define VENTURE_ESCPOS_FONT_B_WIDTH 64

/* Borrowed UTF-8 strings. Timestamp is explicit for deterministic rendering.
 * Raw data bypasses the text pipeline; never expose it to an HTTP caller. */
typedef struct
{
	const gchar				*title;
	const gchar				*timestamp;
	const gchar				*body;
	const gchar				*font;
	const gchar				*codepage;
	const gchar				*alignment;
	const gchar * const		*qr;
	const gchar * const		*barcodes;
	const gchar				*barcode_type;
	GBytes					*raw;
	guint					width;
	guint					tabs;
	guint					qr_size;
	guint					feed;
	gboolean				cut;
	gboolean				cut_only;
	gboolean				bold;
	gboolean				double_height;
	gboolean				wrap;
	gint					reflow; /* 0: per line, 1: heuristic, 2: force */
} VentureEscposDocument;
/**
 * venture_escpos_document_init:
 * @document: document to initialize
 * @printer: (nullable): configured defaults, or Bash defaults when NULL
 */
void
venture_escpos_document_init(
	VentureEscposDocument	*document,
	const VenturePrinter	*printer
);
/**
 * venture_escpos_render:
 * @document: text and layout
 * @error: (out) (optional): return location for an error
 * Returns: (transfer full) (nullable): ESC/POS bytes, suitable for a dry run
 */
GBytes *
venture_escpos_render(
	VentureEscposDocument	*document,
	GError					**error
);
/**
 * venture_printer_exchange:
 * @printer: trusted configuration, never request-derived host/port
 * @bytes: (transfer none) (nullable): bytes to send; NULL queries real-time status
 * @error: (out) (optional): return location for an error
 *
 * Uses asynchronous GIO under a nested main loop on the calling context.
 * No database transaction may be held across this reentrant call. A single
 * cancellable deadline bounds DNS, connection, writes and status reads.
 * Returns: (transfer full) (nullable): empty bytes on send, two status bytes
 *   on query, or NULL on failure (delivery may already be partial)
 */
GBytes *
venture_printer_exchange(
	const VenturePrinter	*printer,
	GBytes					*bytes,
	GError					**error
);
G_END_DECLS
#endif
