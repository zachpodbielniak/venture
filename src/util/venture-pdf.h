/*
 * venture-pdf.h - A small PDF writer for business documents
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Invoices and receipts are text, rules and a table: nothing that needs a
 * layout engine. #VenturePdfWriter draws exactly that onto pages with the
 * two standard Helvetica faces every PDF reader carries, so a document is
 * produced with no library, no font file and no subprocess, and is the
 * same bytes every time it is drawn from the same data.
 *
 * Coordinates are points from the top-left corner of the page, which is
 * how a document is laid out; the writer flips them into PDF's bottom-left
 * space. Text is UTF-8 in and WinAnsi out: a character outside it becomes
 * "?" rather than a broken file.
 */

#ifndef VENTURE_PDF_H
#define VENTURE_PDF_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_PDF_WRITER (venture_pdf_writer_get_type())

G_DECLARE_FINAL_TYPE(VenturePdfWriter, venture_pdf_writer, VENTURE, PDF_WRITER, GObject)

/**
 * VenturePdfAlign:
 * @VENTURE_PDF_ALIGN_LEFT: text starts at x
 * @VENTURE_PDF_ALIGN_RIGHT: text ends at x
 *
 * Where a line of text sits relative to its x coordinate.
 */
typedef enum
{
	VENTURE_PDF_ALIGN_LEFT = 0,
	VENTURE_PDF_ALIGN_RIGHT
} VenturePdfAlign;

/**
 * venture_pdf_writer_new:
 * @width: page width in points (595 for A4, 612 for Letter)
 * @height: page height in points (842 for A4, 792 for Letter)
 *
 * Returns: (transfer full): a writer with one empty page
 */
VenturePdfWriter *
venture_pdf_writer_new(
	gdouble	width,
	gdouble	height
);

/**
 * venture_pdf_writer_new_page:
 * @self: a #VenturePdfWriter
 *
 * Starts another page; later drawing goes onto it.
 */
void
venture_pdf_writer_new_page(VenturePdfWriter *self);

/**
 * venture_pdf_writer_text:
 * @self: a #VenturePdfWriter
 * @x: points from the left edge
 * @y: points from the top edge to the text's baseline
 * @size: font size in points
 * @bold: whether to use the bold face
 * @align: which end of the text @x names
 * @text: (nullable): UTF-8 text on one line
 *
 * Draws one line of text in the current grey.
 */
void
venture_pdf_writer_text(
	VenturePdfWriter	*self,
	gdouble			 x,
	gdouble			 y,
	gdouble			 size,
	gboolean		 bold,
	VenturePdfAlign		 align,
	const gchar		*text
);

/**
 * venture_pdf_writer_text_width:
 * @self: a #VenturePdfWriter
 * @size: font size in points
 * @bold: whether the bold face is meant
 * @text: (nullable): UTF-8 text
 *
 * Returns: how wide @text is drawn, in points
 */
gdouble
venture_pdf_writer_text_width(
	VenturePdfWriter	*self,
	gdouble			 size,
	gboolean		 bold,
	const gchar		*text
);

/**
 * venture_pdf_writer_wrap:
 * @self: a #VenturePdfWriter
 * @x: points from the left edge
 * @y: baseline of the first line, points from the top
 * @width: widest a line may be
 * @size: font size in points
 * @bold: whether to use the bold face
 * @text: (nullable): UTF-8 text, which may contain line breaks
 *
 * Draws @text left-aligned, as venture_pdf_writer_wrap_aligned() does.
 *
 * Returns: the baseline below the last line drawn
 */
gdouble
venture_pdf_writer_wrap(
	VenturePdfWriter	*self,
	gdouble			 x,
	gdouble			 y,
	gdouble			 width,
	gdouble			 size,
	gboolean		 bold,
	const gchar		*text
);

/**
 * venture_pdf_writer_wrap_aligned:
 * @self: a #VenturePdfWriter
 * @x: the left edge, or with %VENTURE_PDF_ALIGN_RIGHT the right edge
 * @y: baseline of the first line, points from the top
 * @width: widest a line may be
 * @size: font size in points
 * @bold: whether to use the bold face
 * @align: which edge every line keeps to
 * @text: (nullable): UTF-8 text, which may contain line breaks
 *
 * Draws @text broken into lines at spaces so none is wider than @width. A
 * single word wider than @width -- a long invoice number, a web address --
 * is broken between characters rather than run past the edge.
 *
 * Returns: the baseline below the last line drawn
 */
gdouble
venture_pdf_writer_wrap_aligned(
	VenturePdfWriter	*self,
	gdouble			 x,
	gdouble			 y,
	gdouble			 width,
	gdouble			 size,
	gboolean		 bold,
	VenturePdfAlign		 align,
	const gchar		*text
);

/**
 * venture_pdf_writer_fit_size:
 * @self: a #VenturePdfWriter
 * @size: the size wanted
 * @min_size: the smallest size worth shrinking to
 * @bold: whether to use the bold face
 * @width: the space available
 * @text: UTF-8 text
 *
 * The largest size, in half-point steps from @size down to @min_size, at
 * which @text fits on one line of @width. Text that does not fit even at
 * @min_size gets @min_size, and should then be wrapped.
 *
 * Returns: a font size in points
 */
gdouble
venture_pdf_writer_fit_size(
	VenturePdfWriter	*self,
	gdouble			 size,
	gdouble			 min_size,
	gboolean		 bold,
	gdouble			 width,
	const gchar		*text
);

/**
 * venture_pdf_writer_rule:
 * @self: a #VenturePdfWriter
 * @x1: start, from the left
 * @y1: start, from the top
 * @x2: end, from the left
 * @y2: end, from the top
 * @thickness: line width in points
 *
 * Draws a straight line in the current grey.
 */
void
venture_pdf_writer_rule(
	VenturePdfWriter	*self,
	gdouble			 x1,
	gdouble			 y1,
	gdouble			 x2,
	gdouble			 y2,
	gdouble			 thickness
);

/**
 * venture_pdf_writer_fill:
 * @self: a #VenturePdfWriter
 * @x: left edge
 * @y: top edge
 * @width: width
 * @height: height
 * @grey: 0 is black, 1 is white
 *
 * Fills a rectangle -- a table's header band, a total's box.
 */
void
venture_pdf_writer_fill(
	VenturePdfWriter	*self,
	gdouble			 x,
	gdouble			 y,
	gdouble			 width,
	gdouble			 height,
	gdouble			 grey
);

/**
 * venture_pdf_writer_set_grey:
 * @self: a #VenturePdfWriter
 * @grey: 0 is black, 1 is white
 *
 * Sets the colour later text and rules are drawn in.
 */
void
venture_pdf_writer_set_grey(
	VenturePdfWriter	*self,
	gdouble			 grey
);

/**
 * venture_pdf_writer_set_title:
 * @self: a #VenturePdfWriter
 * @title: (nullable): the document's title, as a reader shows it
 */
void
venture_pdf_writer_set_title(
	VenturePdfWriter	*self,
	const gchar		*title
);

/**
 * venture_pdf_writer_finish:
 * @self: a #VenturePdfWriter
 *
 * Writes the whole document. The writer can be drawn on no further.
 *
 * Returns: (transfer full): the PDF file's bytes
 */
GBytes *
venture_pdf_writer_finish(VenturePdfWriter *self);

G_END_DECLS

#endif /* VENTURE_PDF_H */
