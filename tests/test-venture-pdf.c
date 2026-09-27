/*
 * test-venture-pdf.c - Documents are real PDF files.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Invoices and receipts are drawn by VenturePdfWriter and mailed to
 * customers. If this regresses, a customer opens an attachment their
 * reader calls damaged -- a broken cross-reference table, an unescaped
 * bracket in a company name, a character the fonts cannot hold.
 */

#include <venture.h>

#include <string.h>

static gchar *
pdf_text(GBytes *bytes)
{
	gsize size;
	const gchar *data = g_bytes_get_data(bytes, &size);

	return g_strndup(data, size);
}

/* The file is a PDF, every object the xref table names starts where it
 * says, and startxref points at the table. */
static void
test_structure(void)
{
	g_autoptr(VenturePdfWriter) pdf = venture_pdf_writer_new(595, 842);
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *text = NULL;
	const gchar *xref;
	const gchar *start;
	gchar **entries;
	guint i;

	venture_pdf_writer_set_title(pdf, "Invoice INV-1");
	venture_pdf_writer_text(pdf, 40, 60, 20, TRUE, VENTURE_PDF_ALIGN_LEFT, "Invoice");
	venture_pdf_writer_new_page(pdf);
	venture_pdf_writer_text(pdf, 40, 60, 10, FALSE, VENTURE_PDF_ALIGN_LEFT, "Page two");
	bytes = venture_pdf_writer_finish(pdf);
	text = pdf_text(bytes);

	g_assert_true(g_str_has_prefix(text, "%PDF-1.4\n"));
	g_assert_true(g_str_has_suffix(text, "%%EOF\n"));
	g_assert_nonnull(strstr(text, "/Count 2"));
	g_assert_nonnull(strstr(text, "/BaseFont /Helvetica-Bold"));
	/* "Invoice INV-1" as UTF-16BE with its byte-order mark. */
	g_assert_nonnull(strstr(text, "/Title <FEFF0049006E0076006F00690063006500200049004E0056002D0031>"));

	/* startxref names the offset of "xref". */
	start = g_strrstr(text, "startxref\n");
	g_assert_nonnull(start);
	xref = text + g_ascii_strtoull(start + strlen("startxref\n"), NULL, 10);
	g_assert_true(g_str_has_prefix(xref, "xref\n"));

	/* Each in-use entry points at "N 0 obj". */
	entries = g_strsplit(xref, "\n", -1);
	for (i = 3; NULL != entries[i] && g_str_has_suffix(entries[i], " n "); i++)
	{
		g_autofree gchar *expected = g_strdup_printf("%u 0 obj", i - 2);
		gsize offset = g_ascii_strtoull(entries[i], NULL, 10);

		g_assert_true(g_str_has_prefix(text + offset, expected));
	}
	g_assert_cmpuint(i - 3, ==, 9);
	g_strfreev(entries);
}

/* Brackets and backslashes in a name are escaped; a character the
 * standard fonts cannot draw becomes "?", never a broken string. */
static void
test_text_is_safe(void)
{
	g_autoptr(VenturePdfWriter) pdf = venture_pdf_writer_new(612, 792);
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *text = NULL;

	venture_pdf_writer_text(pdf, 40, 60, 10, FALSE, VENTURE_PDF_ALIGN_LEFT,
	                        "Smith (Holdings) \\ Co \xe6\x97\xa5");
	bytes = venture_pdf_writer_finish(pdf);
	text = pdf_text(bytes);

	g_assert_nonnull(strstr(text, "(Smith \\(Holdings\\) \\\\ Co ?) Tj"));
}

/* Right-aligned amounts end where they are told to, which is what makes a
 * column of money line up. Digits are one width in Helvetica. */
static void
test_widths(void)
{
	g_autoptr(VenturePdfWriter) pdf = venture_pdf_writer_new(595, 842);

	g_assert_cmpfloat_with_epsilon(
		venture_pdf_writer_text_width(pdf, 10, FALSE, "1234"), 22.24, 0.001);
	g_assert_cmpfloat(venture_pdf_writer_text_width(pdf, 10, TRUE, "Total"), >,
	                  venture_pdf_writer_text_width(pdf, 10, FALSE, "Total"));
}

/* Long text wraps inside its width and says where it stopped. */
static void
test_wrap(void)
{
	g_autoptr(VenturePdfWriter) pdf = venture_pdf_writer_new(595, 842);
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *text = NULL;
	gdouble after;

	after = venture_pdf_writer_wrap(pdf, 40, 100, 100, 10, FALSE,
		"Payment is due within thirty days of the invoice date.\nThank you.");
	g_assert_cmpfloat(after, >, 100 + 3 * 10);
	bytes = venture_pdf_writer_finish(pdf);
	text = pdf_text(bytes);
	g_assert_nonnull(strstr(text, "(Thank you.) Tj"));
}

/*
 * A word longer than the width -- an invoice number, a URL -- is broken
 * across lines rather than run past the edge; right-aligned wrapping ends
 * every line at its edge; and a title shrinks to fit before it wraps. If
 * this regresses, a long invoice number is drawn over the letterhead.
 */
static void
test_long_words(void)
{
	g_autoptr(VenturePdfWriter) pdf = venture_pdf_writer_new(595, 842);
	const gchar *word = "INV-2026-ACME-CORPORATION-INTERNATIONAL-HOLDINGS-0000000041";
	gdouble after, size;

	after = venture_pdf_writer_wrap(pdf, 40, 100, 60, 10, FALSE, word);
	g_assert_cmpfloat(after, >=, 100 + 3 * 10);

	after = venture_pdf_writer_wrap_aligned(pdf, 555, 300, 120, 10, TRUE, VENTURE_PDF_ALIGN_RIGHT, word);
	g_assert_cmpfloat(after, >, 300 + 10);

	size = venture_pdf_writer_fit_size(pdf, 20, 12, TRUE, 200, "Invoice 41");
	g_assert_cmpfloat(size, ==, 20);
	size = venture_pdf_writer_fit_size(pdf, 20, 12, TRUE, 200, word);
	g_assert_cmpfloat(size, ==, 12);
	size = venture_pdf_writer_fit_size(pdf, 20, 8, TRUE, 300, "Invoice INV-2026-ACME-0041-EXTRA");
	g_assert_cmpfloat(size, <, 20);
	g_assert_cmpfloat(venture_pdf_writer_text_width(pdf, size, TRUE, "Invoice INV-2026-ACME-0041-EXTRA"), <=, 300);
}

/* The hex text string after "/Title <", decoded back to UTF-8. */
static gchar *
title_of(const gchar *text)
{
	const gchar *p = strstr(text, "/Title <");
	g_autoptr(GArray) units = g_array_new(FALSE, FALSE, sizeof(gunichar2));

	g_assert_nonnull(p);
	p += strlen("/Title <");
	g_assert_true(g_str_has_prefix(p, "FEFF"));
	for (p += 4; *p != '>'; p += 4)
	{
		g_autofree gchar *unit = g_strndup(p, 4);
		gunichar2 value = (gunichar2)g_ascii_strtoull(unit, NULL, 16);

		g_assert_true(g_ascii_isxdigit(p[0]) && g_ascii_isxdigit(p[3]));
		g_array_append_val(units, value);
	}
	return g_utf16_to_utf8((const gunichar2 *)units->data, (glong)units->len, NULL, NULL, NULL);
}

/*
 * The document title is a PDF text string, which is UTF-16BE (or
 * PDFDocEncoding), never the fonts' WinAnsi. If this regresses, a reader's
 * title bar shows "MÃ¼ller" for an invoice to Müller, and a character past
 * Latin-1 is lost altogether.
 */
static void
test_title_is_unicode(void)
{
	g_autoptr(VenturePdfWriter) pdf = venture_pdf_writer_new(595, 842);
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *title = NULL;
	const gchar *wanted = "Invoice M\xc3\xbcller \xe6\x97\xa5 \xf0\x9f\x93\x84 (1)";

	venture_pdf_writer_set_title(pdf, wanted);
	bytes = venture_pdf_writer_finish(pdf);
	text = pdf_text(bytes);
	title = title_of(text);
	g_assert_cmpstr(title, ==, wanted);
}

/*
 * Text that is not valid UTF-8 -- here a three-byte sequence cut off
 * after its lead byte -- is drawn as far as it goes with "?" for the bad
 * byte. If this regresses, wrapping steps g_utf8_next_char() past the
 * terminator and reads whatever memory follows (AddressSanitizer reports
 * it; without it the line may pick up garbage).
 */
static void
test_invalid_utf8(void)
{
	g_autoptr(VenturePdfWriter) pdf = venture_pdf_writer_new(595, 842);
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *text = NULL;
	/* Allocated to exactly its length, so an over-read leaves the block. */
	g_autofree gchar *bad = g_strdup("Acme\xe0");
	g_autofree gchar *title_bad = g_strdup("Invoice \xe0");
	gdouble after;

	after = venture_pdf_writer_wrap(pdf, 40, 100, 10, 10, FALSE, bad);
	g_assert_cmpfloat(after, >, 100);
	venture_pdf_writer_text(pdf, 40, 300, 10, FALSE, VENTURE_PDF_ALIGN_LEFT, bad);
	venture_pdf_writer_set_title(pdf, title_bad);
	bytes = venture_pdf_writer_finish(pdf);
	text = pdf_text(bytes);
	g_assert_nonnull(strstr(text, "(Acme?) Tj"));
	g_assert_nonnull(strstr(text, "(?) Tj"));
	{
		g_autofree gchar *title = title_of(text);
		g_assert_cmpstr(title, ==, "Invoice \xef\xbf\xbd");
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/pdf/structure", test_structure);
	g_test_add_func("/pdf/text-is-safe", test_text_is_safe);
	g_test_add_func("/pdf/widths", test_widths);
	g_test_add_func("/pdf/wrap", test_wrap);
	g_test_add_func("/pdf/long-words", test_long_words);
	g_test_add_func("/pdf/title-is-unicode", test_title_is_unicode);
	g_test_add_func("/pdf/invalid-utf8", test_invalid_utf8);

	return g_test_run();
}
