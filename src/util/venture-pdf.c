/*
 * venture-pdf.c - A small PDF writer for business documents
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/*
 * Advance widths of the standard Helvetica and Helvetica-Bold faces for
 * printable ASCII, in thousandths of an em, from Adobe's core font
 * metrics. Everything outside ASCII is measured as a digit, which is
 * close enough for the few accented letters a name carries and exact for
 * every number on a document -- the thing that has to line up.
 */
static const guint16 venture_pdf_helvetica[95] = {
	278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
	556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
	1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
	667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
	333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
	556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584
};

static const guint16 venture_pdf_helvetica_bold[95] = {
	278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
	556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
	975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
	667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
	333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
	611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584
};

struct _VenturePdfWriter
{
	GObject parent_instance;

	gdouble width;
	gdouble height;
	gchar *title;

	/* One content stream per page, drawn into in order. */
	GPtrArray *pages;
	GString *current;
	gboolean finished;
	gdouble flow_top;
	gdouble flow_bottom;
};

G_DEFINE_FINAL_TYPE(VenturePdfWriter, venture_pdf_writer, G_TYPE_OBJECT)

static void
venture_pdf_writer_finalize(GObject *object)
{
	VenturePdfWriter *self = VENTURE_PDF_WRITER(object);

	g_clear_pointer(&self->pages, g_ptr_array_unref);
	g_clear_pointer(&self->title, g_free);

	G_OBJECT_CLASS(venture_pdf_writer_parent_class)->finalize(object);
}

static void
venture_pdf_writer_class_init(VenturePdfWriterClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_pdf_writer_finalize;
}

static void
venture_pdf_string_free(gpointer data)
{
	g_string_free(data, TRUE);
}

static void
venture_pdf_writer_init(VenturePdfWriter *self)
{
	self->pages = g_ptr_array_new_with_free_func(venture_pdf_string_free);
}

VenturePdfWriter *
venture_pdf_writer_new(
	gdouble	width,
	gdouble	height
){
	VenturePdfWriter *self;

	g_return_val_if_fail(width > 0 && height > 0, NULL);

	self = g_object_new(VENTURE_TYPE_PDF_WRITER, NULL);
	self->width = width;
	self->height = height;
	venture_pdf_writer_new_page(self);

	return self;
}

void
venture_pdf_writer_new_page(VenturePdfWriter *self)
{
	g_return_if_fail(VENTURE_IS_PDF_WRITER(self));
	g_return_if_fail(!self->finished);

	self->current = g_string_new(NULL);
	g_ptr_array_add(self->pages, self->current);
}

/*
 * UTF-8 to the WinAnsi bytes the standard fonts are drawn in. Anything the
 * encoding cannot hold becomes "?", so a name in another script degrades to
 * question marks rather than to an unreadable file.
 */
static gchar *
venture_pdf_winansi(const gchar *text)
{
	g_autoptr(GString) out = g_string_new(NULL);
	g_autofree gchar *valid = NULL;
	const gchar *p;

	if (NULL == text)
		return g_strdup("");

	/* Caller text is a stored field, not a promise of UTF-8: an invalid
	 * byte becomes U+FFFD, drawn as "?", rather than ending the line. */
	valid = g_utf8_make_valid(text, -1);
	for (p = valid; '\0' != *p; p = g_utf8_next_char(p))
	{
		gunichar c = g_utf8_get_char_validated(p, -1);

		if (c == (gunichar)-1 || c == (gunichar)-2)
			break;

		if ((c >= 32 && c < 127) || (c >= 160 && c <= 255))
			g_string_append_c(out, (gchar)c);
		else if (0x2014 == c)
			g_string_append_c(out, (gchar)0x97);
		else if (0x2013 == c)
			g_string_append_c(out, (gchar)0x96);
		else if (0x2018 == c || 0x2019 == c)
			g_string_append_c(out, '\'');
		else if (0x201C == c || 0x201D == c)
			g_string_append_c(out, '"');
		else if (0x2026 == c)
			g_string_append_c(out, (gchar)0x85);
		else if (0x20AC == c)
			g_string_append_c(out, (gchar)0x80);
		else if ('\t' == c)
			g_string_append_c(out, ' ');
		else
			g_string_append_c(out, '?');
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

static gdouble
venture_pdf_width_of(
	const gchar	*encoded,
	gdouble		 size,
	gboolean	 bold
){
	const guint16 *table = bold ? venture_pdf_helvetica_bold : venture_pdf_helvetica;
	gdouble units = 0;
	const guchar *p;

	for (p = (const guchar *)encoded; '\0' != *p; p++)
		units += (*p >= 32 && *p < 127) ? table[*p - 32] : 556;

	return units * size / 1000.0;
}

gdouble
venture_pdf_writer_text_width(
	VenturePdfWriter	*self,
	gdouble			 size,
	gboolean		 bold,
	const gchar		*text
){
	g_autofree gchar *encoded = NULL;

	g_return_val_if_fail(VENTURE_IS_PDF_WRITER(self), 0);

	encoded = venture_pdf_winansi(text);

	return venture_pdf_width_of(encoded, size, bold);
}

/* A number written the way PDF wants it: no locale, no exponent. */
static void
venture_pdf_number(GString *out, gdouble value)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	g_ascii_formatd(buffer, sizeof(buffer), "%.2f", value);
	g_string_append(out, buffer);
}

/* A PDF literal string's body, with its three special bytes escaped. */
static void
venture_pdf_literal(GString *out, const gchar *encoded)
{
	const guchar *p;

	for (p = (const guchar *)encoded; '\0' != *p; p++)
	{
		if ('(' == *p || ')' == *p || '\\' == *p)
			g_string_append_c(out, '\\');
		g_string_append_c(out, (gchar)*p);
	}
}

void
venture_pdf_writer_text(
	VenturePdfWriter	*self,
	gdouble			 x,
	gdouble			 y,
	gdouble			 size,
	gboolean		 bold,
	VenturePdfAlign		 align,
	const gchar		*text
){
	g_autofree gchar *encoded = NULL;

	g_return_if_fail(VENTURE_IS_PDF_WRITER(self));
	g_return_if_fail(!self->finished);

	encoded = venture_pdf_winansi(text);

	if ('\0' == encoded[0])
		return;

	if (VENTURE_PDF_ALIGN_RIGHT == align)
		x -= venture_pdf_width_of(encoded, size, bold);

	g_string_append_printf(self->current, "BT /%s ", bold ? "F2" : "F1");
	venture_pdf_number(self->current, size);
	g_string_append(self->current, " Tf ");
	venture_pdf_number(self->current, x);
	g_string_append_c(self->current, ' ');
	venture_pdf_number(self->current, self->height - y);
	g_string_append(self->current, " Td (");
	venture_pdf_literal(self->current, encoded);
	g_string_append(self->current, ") Tj ET\n");
}

/*
 * Emits @line and moves down, or -- when @line alone is wider than @width
 * -- breaks it between characters into pieces that fit. Only a word with
 * no space in it ever reaches the second case.
 */
static gdouble
venture_pdf_emit_line(VenturePdfWriter *self, gdouble x, gdouble y, gdouble width, gdouble size,
	gboolean bold, VenturePdfAlign align, const gchar *line)
{
	gdouble leading = size * 1.35;
	const gchar *start = line;

	while (*start != '\0')
	{
		const gchar *end = start, *next;
		g_autofree gchar *piece = NULL;

		/* Take characters while they fit; always at least one. */
		for (next = g_utf8_next_char(end); ; next = g_utf8_next_char(next))
		{
			g_autofree gchar *candidate = g_strndup(start, next - start);

			if (end != start && venture_pdf_writer_text_width(self, size, bold, candidate) > width)
				break;
			end = next;
			if (*next == '\0')
				break;
		}
		if (self->flow_bottom > 0 && y > self->flow_bottom)
		{
			venture_pdf_writer_new_page(self);
			y = self->flow_top;
		}
		piece = g_strndup(start, end - start);
		venture_pdf_writer_text(self, x, y, size, bold, align, piece);
		y += leading;
		start = end;
	}
	return y;
}

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
){
	g_auto(GStrv) paragraphs = NULL;
	g_autofree gchar *valid = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_PDF_WRITER(self), y);

	if (NULL == text)
		return y;

	/*
	 * Made valid once, here, because venture_pdf_emit_line() steps through
	 * the text with g_utf8_next_char(): a truncated sequence before the
	 * terminator -- a lone 0xE0 -- would step it past the NUL and on
	 * through whatever memory follows.
	 */
	valid = g_utf8_make_valid(text, -1);
	paragraphs = g_strsplit(valid, "\n", -1);

	for (i = 0; NULL != paragraphs[i]; i++)
	{
		g_auto(GStrv) words = g_strsplit(paragraphs[i], " ", -1);
		g_autoptr(GString) line = g_string_new(NULL);
		guint w;

		for (w = 0; NULL != words[w]; w++)
		{
			g_autofree gchar *candidate = NULL;

			if ('\0' == words[w][0])
				continue;

			candidate = (0 == line->len) ? g_strdup(words[w])
				: g_strconcat(line->str, " ", words[w], NULL);

			if ((line->len > 0) &&
			    (venture_pdf_writer_text_width(self, size, bold, candidate) > width))
			{
				y = venture_pdf_emit_line(self, x, y, width, size, bold, align, line->str);
				g_string_assign(line, words[w]);
			}
			else
				g_string_assign(line, candidate);
		}

		if (line->len > 0)
			y = venture_pdf_emit_line(self, x, y, width, size, bold, align, line->str);
		else
			y += size * 1.35;
	}

	return y;
}

gdouble
venture_pdf_writer_wrap(
	VenturePdfWriter	*self,
	gdouble			 x,
	gdouble			 y,
	gdouble			 width,
	gdouble			 size,
	gboolean		 bold,
	const gchar		*text
){
	return venture_pdf_writer_wrap_aligned(self, x, y, width, size, bold, VENTURE_PDF_ALIGN_LEFT, text);
}

gdouble
venture_pdf_writer_wrap_pages(
	VenturePdfWriter *self, gdouble x, gdouble y, gdouble width,
	gdouble size, gboolean bold, const gchar *text, gdouble top, gdouble bottom
){
	gdouble end;

	g_return_val_if_fail(VENTURE_IS_PDF_WRITER(self), y);
	g_return_val_if_fail(top > 0 && bottom > top, y);
	self->flow_top = top;
	self->flow_bottom = bottom;
	end = venture_pdf_writer_wrap(self, x, y, width, size, bold, text);
	self->flow_top = 0;
	self->flow_bottom = 0;
	return end;
}

gdouble
venture_pdf_writer_fit_size(
	VenturePdfWriter	*self,
	gdouble			 size,
	gdouble			 min_size,
	gboolean		 bold,
	gdouble			 width,
	const gchar		*text
){
	g_return_val_if_fail(VENTURE_IS_PDF_WRITER(self), min_size);

	for (; size > min_size; size -= 0.5)
		if (venture_pdf_writer_text_width(self, size, bold, text != NULL ? text : "") <= width)
			return size;
	return min_size;
}

void
venture_pdf_writer_rule(
	VenturePdfWriter	*self,
	gdouble			 x1,
	gdouble			 y1,
	gdouble			 x2,
	gdouble			 y2,
	gdouble			 thickness
){
	g_return_if_fail(VENTURE_IS_PDF_WRITER(self));
	g_return_if_fail(!self->finished);

	venture_pdf_number(self->current, thickness);
	g_string_append(self->current, " w ");
	venture_pdf_number(self->current, x1);
	g_string_append_c(self->current, ' ');
	venture_pdf_number(self->current, self->height - y1);
	g_string_append(self->current, " m ");
	venture_pdf_number(self->current, x2);
	g_string_append_c(self->current, ' ');
	venture_pdf_number(self->current, self->height - y2);
	g_string_append(self->current, " l S\n");
}

void
venture_pdf_writer_fill(
	VenturePdfWriter	*self,
	gdouble			 x,
	gdouble			 y,
	gdouble			 width,
	gdouble			 height,
	gdouble			 grey
){
	g_return_if_fail(VENTURE_IS_PDF_WRITER(self));
	g_return_if_fail(!self->finished);

	/* Saved and restored, so the fill's grey does not become the text's. */
	g_string_append(self->current, "q ");
	venture_pdf_number(self->current, grey);
	g_string_append(self->current, " g ");
	venture_pdf_number(self->current, x);
	g_string_append_c(self->current, ' ');
	venture_pdf_number(self->current, self->height - y - height);
	g_string_append_c(self->current, ' ');
	venture_pdf_number(self->current, width);
	g_string_append_c(self->current, ' ');
	venture_pdf_number(self->current, height);
	g_string_append(self->current, " re f Q\n");
}

void
venture_pdf_writer_set_grey(
	VenturePdfWriter	*self,
	gdouble			 grey
){
	g_return_if_fail(VENTURE_IS_PDF_WRITER(self));
	g_return_if_fail(!self->finished);

	venture_pdf_number(self->current, grey);
	g_string_append(self->current, " g ");
	venture_pdf_number(self->current, grey);
	g_string_append(self->current, " G\n");
}

void
venture_pdf_writer_set_title(
	VenturePdfWriter	*self,
	const gchar		*title
){
	g_return_if_fail(VENTURE_IS_PDF_WRITER(self));

	g_free(self->title);
	self->title = (NULL != title) ? g_utf8_make_valid(title, -1) : NULL;
}

/*
 * A PDF text string outside a content stream is PDFDocEncoding or
 * UTF-16BE, not the fonts' WinAnsi: WinAnsi bytes in /Title show a reader's
 * title bar the wrong letters for anything past ASCII. UTF-16BE with its
 * byte-order mark, as hex, holds every character and needs no escaping.
 */
static void
venture_pdf_text_string(GString *out, const gchar *text)
{
	g_autofree gunichar2 *units = NULL;
	glong n_units = 0;
	glong i;

	g_string_append(out, "<FEFF");
	if (NULL != text)
		units = g_utf8_to_utf16(text, -1, NULL, &n_units, NULL);
	for (i = 0; NULL != units && i < n_units; i++)
		g_string_append_printf(out, "%04X", (guint)units[i]);
	g_string_append_c(out, '>');
}

/* Starts object @number and records where it begins, for the xref table. */
static void
venture_pdf_object(
	GString	*out,
	GArray	*offsets,
	guint	 number
){
	g_array_index(offsets, gsize, number) = out->len;
	g_string_append_printf(out, "%u 0 obj\n", number);
}

GBytes *
venture_pdf_writer_finish(VenturePdfWriter *self)
{
	g_autoptr(GString) out = NULL;
	g_autoptr(GArray) offsets = NULL;
	guint n_pages;
	guint objects;
	guint i;
	gsize xref;

	g_return_val_if_fail(VENTURE_IS_PDF_WRITER(self), NULL);
	g_return_val_if_fail(!self->finished, NULL);

	self->finished = TRUE;
	n_pages = self->pages->len;

	/*
	 * Objects: 1 catalog, 2 page tree, 3 and 4 the two fonts, 5 the info
	 * dictionary, then a page and its content stream for each page.
	 */
	objects = 5 + 2 * n_pages;
	offsets = g_array_sized_new(FALSE, TRUE, sizeof(gsize), objects + 1);
	g_array_set_size(offsets, objects + 1);

	/* The binary comment marks the file as binary to transfer tools. */
	out = g_string_new("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");

	venture_pdf_object(out, offsets, 1);
	g_string_append(out, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

	venture_pdf_object(out, offsets, 2);
	g_string_append(out, "<< /Type /Pages /Kids [");
	for (i = 0; i < n_pages; i++)
		g_string_append_printf(out, " %u 0 R", 6 + 2 * i);
	g_string_append_printf(out, " ] /Count %u >>\nendobj\n", n_pages);

	venture_pdf_object(out, offsets, 3);
	g_string_append(out, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
	                     "/Encoding /WinAnsiEncoding >>\nendobj\n");
	venture_pdf_object(out, offsets, 4);
	g_string_append(out, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
	                     "/Encoding /WinAnsiEncoding >>\nendobj\n");

	venture_pdf_object(out, offsets, 5);
	g_string_append(out, "<< /Producer (VENTURE) /Title ");
	venture_pdf_text_string(out, self->title);
	g_string_append(out, " >>\nendobj\n");

	for (i = 0; i < n_pages; i++)
	{
		GString *content = g_ptr_array_index(self->pages, i);
		guint page = 6 + 2 * i;

		venture_pdf_object(out, offsets, page);
		g_string_append(out, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ");
		venture_pdf_number(out, self->width);
		g_string_append_c(out, ' ');
		venture_pdf_number(out, self->height);
		g_string_append_printf(out, "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> "
		                       "/Contents %u 0 R >>\nendobj\n", page + 1);

		venture_pdf_object(out, offsets, page + 1);
		g_string_append_printf(out, "<< /Length %" G_GSIZE_FORMAT " >>\nstream\n",
		                       content->len);
		g_string_append_len(out, content->str, (gssize)content->len);
		g_string_append(out, "\nendstream\nendobj\n");
	}

	xref = out->len;
	g_string_append_printf(out, "xref\n0 %u\n0000000000 65535 f \n", objects + 1);
	for (i = 1; i <= objects; i++)
		g_string_append_printf(out, "%010" G_GSIZE_FORMAT " 00000 n \n",
		                       g_array_index(offsets, gsize, i));
	g_string_append_printf(out, "trailer\n<< /Size %u /Root 1 0 R /Info 5 0 R >>\n"
	                       "startxref\n%" G_GSIZE_FORMAT "\n%%%%EOF\n", objects + 1, xref);

	return g_string_free_to_bytes(g_steal_pointer(&out));
}
