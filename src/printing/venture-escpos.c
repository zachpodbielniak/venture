/* Port of receipt-print, Copyright (C) 2026 Zach Podbielniak.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

#define ESCPOS_BYTE_MAX 255
#define ESCPOS_QR_STORE_MAX 7092
#define ESCPOS_BARCODE_HEIGHT 162
#define ESCPOS_BARCODE_WIDTH 3
#define ESCPOS_BARCODE_FIRST_SYSTEM 65
#define ESCPOS_RESET "\033@"
#define ESCPOS_CUT "\035VB"
#define ESCPOS_QR_MODEL_2 "\035(k\004\0001A2\000"
#define ESCPOS_QR_SIZE "\035(k\003\0001C"
#define ESCPOS_QR_CORRECTION_M "\035(k\003\0001E1"
#define ESCPOS_QR_STORE "\035(k"
#define ESCPOS_QR_STORE_DATA "1P0"
#define ESCPOS_QR_PRINT "\035(k\003\0001Q0"
#define ESCPOS_PAGE_CP437 0
#define ESCPOS_PAGE_CP1252 16
#define ESCPOS_PAGE_CP858 19


/**
 * venture_escpos_document_init:
 * @document: document to initialize
 * @printer: (nullable): configured defaults, or Bash defaults when NULL
 */
void
venture_escpos_document_init(
	VentureEscposDocument	*document,
	const VenturePrinter	*printer
){
	memset(document, 0, sizeof *document);
	document->font = printer ? printer->font : "b";
	document->codepage = printer ? printer->codepage : "cp1252";
	document->alignment = printer ? printer->alignment : "left";
	document->width = printer ? printer->width : 0;
	document->feed = printer ? printer->feed : VENTURE_ESCPOS_DEFAULT_FEED;
	document->cut = printer ? printer->cut : TRUE;
	document->tabs = VENTURE_ESCPOS_DEFAULT_TABS;
	document->qr_size = VENTURE_ESCPOS_DEFAULT_QR_SIZE;
	document->barcode_type = "code128";
	document->wrap = TRUE;
	document->reflow = 1;
}

/* Resolve closed vocabularies before emitting bytes; unknown options must not select a different
 * printer mode. */
static gint
choice(
	const gchar			*value,
	const gchar *const	*choices
){
	guint	i;

	for (i = 0; choices[i]; i++)
	{
		if (g_strcmp0(value, choices[i]) == 0)
		{
			return (gint) i;
		}
	}
	return -1;
}

/* Keep three-byte commands together so a parameter cannot drift into printable text. */
static void
command(
	GString	*out,
	guint8	prefix,
	guint8	opcode,
	guint8	value
){
	g_string_append_c(out, prefix);
	g_string_append_c(out, opcode);
	g_string_append_c(out, value);
}

/* Strip terminal controls before conversion so only intentional printer commands reach paper. */
static gchar *
encode(
	const gchar	*input,
	const gchar	*charset,
	guint		tabs,
	gboolean	body
){
	g_autoptr(GString)	staged = g_string_new(NULL);
	g_autoptr(GString)	clean = g_string_new(NULL);
	g_autofree gchar	*stripped = NULL;
	g_autofree gchar	*encoded = NULL;
	g_autofree gchar	*source = NULL;
	gsize				i;
	gsize				column = 0;
	gsize				size = 0;
	const gchar			*text = input ? input : "";

	if (body)
	{
		g_autoptr(GRegex)	ansi = NULL;
		gchar				*read;
		gchar				*write;

		source = g_strdup(text);
		write = source;

		for (read = source; *read; read++)
		{
			if (*read != '\r')
			{
				*write++ = *read;
			}
		}
		*write = '\0';
		text = source;
		/* Byte ranges are load-bearing. Locale collation used to leave
		 * visible "[33m" litter after only the ESC was sanitized away. */
		ansi = g_regex_new("\033\\][^\007\033\n]*(\007|\033\\\\)|\033\\[[0-9;?]*[ "
						   "-/]*[@-~]|\033[@-Z\\\\-_]|\033[()][A-Za-z0-9]",
						   G_REGEX_RAW, 0, NULL);
		stripped = g_regex_replace_literal(ansi, text, -1, 0, "", 0, NULL);
		text = stripped;
	}
	for (i = 0; text[i]; i++)
	{
		guchar c = (guchar) text[i];

		if (body && c == '\r')
		{
			continue;
		}
		if (body && c == '\t')
		{
			guint	n = tabs - column % tabs;

			while (n--)
			{
				g_string_append_c(staged, ' ');
				column++;
			}
			continue;
		}
		g_string_append_c(staged, c);
		/* expand counts UTF-8 characters before conversion, not bytes. */
		if (c == '\n')
		{
			column = 0;
		}
		else if (c == '\b' && column)
		{
			column--;
		}
		else if ((c & 0xc0) != 0x80)
		{
			column++;
		}
	}
	{
		g_autofree gchar	*target = g_strconcat(charset, "//TRANSLIT", NULL);

		encoded = g_convert_with_fallback(staged->str, staged->len, target, "UTF-8", "?", NULL,
										  &size, NULL);
	}
	/* Binary/truncated UTF-8 must never abort a print. The script's last
	 * resort keeps only ASCII, tabs and LF from the staged input. */
	if (!encoded)
	{
		for (i = 0; i < staged->len; i++)
		{
			if (staged->str[i] == '\n' || (staged->str[i] >= 32 && staged->str[i] <= 126))
			{
				g_string_append_c(clean, staged->str[i]);
			}
		}
	}
	else
	{
		/* CP1252 0x80..0x9f are printable, including curly quotes. */
		for (i = 0; i < size; i++)
		{
			if ((guchar) encoded[i] >= 32 && (guchar) encoded[i] != 127)
			{
				g_string_append_c(clean, encoded[i]);
			}
			else if (encoded[i] == '\n')
			{
				g_string_append_c(clean, '\n');
			}
		}
	}
	return g_string_free(g_steal_pointer(&clean), FALSE);
}

/* Emit a complete physical line, including indentation retained by the prose/code heuristic. */
static void
line_out(
	GString		*out,
	const gchar	*indent,
	const gchar	*text
){
	g_string_append(out, indent);
	g_string_append(out, text);
	g_string_append_c(out, '\n');
}

/* Wrap encoded single-byte text so widths match the printer rather than UTF-8 byte counts. */
static void
wrap_para(
	GString		*out,
	const gchar	*text,
	const gchar	*first,
	const gchar	*next,
	guint		width
){
	g_auto(GStrv)		words = g_strsplit_set(text, " \t", -1);
	g_autoptr(GString)	line = g_string_new(NULL);
	const gchar			*indent;
	guint				k;

	if (strlen(first) >= width - 8)
	{
		first = "";
	}
	if (strlen(next) >= width - 8)
	{
		next = "";
	}
	indent = first;

	for (k = 0; words[k]; k++)
	{
		const gchar	*word = words[k];
		gsize		length = strlen(word);

		if (!length)
		{
			continue;
		}
		if (strlen(indent) + line->len + (line->len ? 1 : 0) + length <= width)
		{
			if (line->len)
			{
				g_string_append_c(line, ' ');
			}
			g_string_append(line, word);
			continue;
		}
		if (line->len)
		{
			line_out(out, indent, line->str);
			g_string_truncate(line, 0);
			indent = next;
		}
		if (strlen(indent) + length > width)
		{
			/* Hard split an unbreakable token, emitting even its last chunk. */
			while (length)
			{
				gsize	room = MIN(length, width - strlen(indent));

				g_string_append(out, indent);
				g_string_append_len(out, word, room);
				g_string_append_c(out, '\n');
				word += room;
				length -= room;
			}
			indent = next;
		}
		else
		{
			g_string_append(line, word);
		}
	}
	if (line->len)
	{
		line_out(out, indent, line->str);
	}
	else if (!*text)
	{
		g_string_append_c(out, '\n');
	}
}

/* Preserve logical code lines while still fitting overlong lines to the paper. */
static void
verbatim(
	GString		*out,
	const gchar	*indent,
	const gchar	*rest,
	guint		width
){
	if (strlen(indent) + strlen(rest) <= width)
	{
		line_out(out, indent, rest);
	}
	else
	{
		wrap_para(out, rest, indent, indent, width);
	}
}

/* Match the Bash paragraph heuristic, preserving lists and fenced text that reflow would corrupt.
 */
static void
format_text(
	GString		*out,
	gchar		**lines,
	guint		width,
	gboolean	reflow,
	gsize		longest
){
	g_autoptr(GString)	paragraph = g_string_new(NULL);
	g_autofree gchar	*first = NULL;
	g_autofree gchar	*next = NULL;

	g_autoptr(GRegex)	bullet =
		g_regex_new("^([-*+]|[0-9]+[.)]|[a-zA-Z][.)])[ \\t]+", G_REGEX_RAW, 0, NULL);
	gsize		previous = 0;
	gsize		fill_min = MAX(40, longest * 3 / 4);
	gboolean	fence = FALSE;
	guint		i;

	for (i = 0; lines[i]; i++)
	{
		const gchar				*line = lines[i];
		const gchar				*rest = line;
		g_autofree gchar		*indent = NULL;
		g_autofree gchar		*lower = NULL;
		g_autoptr(GMatchInfo)	match = NULL;
		gsize					plen = previous;
		gboolean				fenced_marker;
		gboolean				org_marker;
		gboolean				stand_alone;
		gboolean				list;
		gint					marker_end = 0;

		previous = strlen(line);

		while (*rest == ' ' || *rest == '\t')
		{
			rest++;
		}
		indent = g_strndup(line, rest - line);

		if (!reflow)
		{
			verbatim(out, indent, rest, width);
			continue;
		}
		lower = g_ascii_strdown(rest, -1);
		fenced_marker = g_str_has_prefix(rest, "```") || g_str_has_prefix(rest, "~~~");
		org_marker = g_str_has_prefix(lower, "#+begin_") || g_str_has_prefix(lower, "#+end_");
		stand_alone = *rest == '|' || *rest == ':' || g_str_has_prefix(rest, "#+");

		if (*rest == '#' || (!*indent && *rest == '*'))
		{
			const gchar	*p = rest;

			while (*p == *rest)
			{
				p++;
			}
			stand_alone |= *p == ' ' || *p == '\t';
		}
		list = g_regex_match(bullet, rest, 0, &match);

		if (list)
		{
			g_match_info_fetch_pos(match, 0, NULL, &marker_end);
		}
		if (fenced_marker || org_marker || fence || !*rest || stand_alone)
		{
			if (paragraph->len)
			{
				wrap_para(out, paragraph->str, first, next, width);
			}
			g_string_truncate(paragraph, 0);
		}
		if (fenced_marker || org_marker || fence)
		{
			line_out(out, "", line);

			if (fenced_marker)
			{
				fence = !fence;
			}
			if (org_marker)
			{
				fence = g_str_has_prefix(lower, "#+begin_");
			}
			continue;
		}
		if (!*rest)
		{
			g_string_append_c(out, '\n');
			continue;
		}
		if (stand_alone || (strlen(indent) >= 4 && !paragraph->len))
		{
			verbatim(out, indent, rest, width);
			continue;
		}
		if (list || (paragraph->len && plen < fill_min))
		{
			if (paragraph->len)
			{
				wrap_para(out, paragraph->str, first, next, width);
			}
			g_string_truncate(paragraph, 0);
		}
		if (paragraph->len)
		{
			g_string_append_c(paragraph, ' ');
			g_string_append(paragraph, rest);
		}
		else
		{
			g_free(first);
			g_free(next);
			first = g_strdup(indent);
			next = list ? g_strnfill(strlen(indent) + marker_end, ' ') : g_strdup(indent);
			g_string_append(paragraph, rest);
		}
	}
	if (paragraph->len)
	{
		wrap_para(out, paragraph->str, first, next, width);
	}
}

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
){
	static const gchar *const fonts[] = {"a", "b", "auto", NULL};
	static const gchar *const pages[] = {"cp437", "cp1252", "cp858", NULL};
	static const guint8	page_ids[] = {ESCPOS_PAGE_CP437, ESCPOS_PAGE_CP1252, ESCPOS_PAGE_CP858};

	static const gchar *const aligns[] = {"left", "center", "right", NULL};
	static const gchar *const barcodes[] = {"upca", "upce",	   "ean13",	 "ean8",	"code39",
											"itf",	"codabar", "code93", "code128", NULL};
	g_autoptr(GString)	out = g_string_new(NULL);
	g_autofree gchar	*body = NULL;
	g_auto(GStrv)		lines = NULL;
	gint				font = choice(document->font, fonts);
	gint				page = choice(document->codepage, pages);
	gint				align = choice(document->alignment, aligns);
	gint				barcode = choice(document->barcode_type, barcodes);
	guint				i;
	guint				width;
	guint				total = 0;
	guint				code = 0;
	gsize				longest = 0;
	gsize				length;
	gboolean			reflow;

	if (font < 0 || page < 0 || align < 0 || barcode < 0 || document->feed > ESCPOS_BYTE_MAX ||
	   document->reflow < 0 || document->reflow > 2 || document->tabs < 1 || document->tabs > 16 ||
	   document->qr_size < 1 || document->qr_size > 16 ||
	   (document->width && (document->width < 8 || document->width > ESCPOS_BYTE_MAX)))
	{
		goto invalid;
	}
	if (document->raw)
	{
		return g_bytes_ref(document->raw);
	}
	body =
		encode(document->cut_only ? "" : document->body, document->codepage, document->tabs, TRUE);
	length = strlen(body);
	lines = g_strsplit(body, "\n", -1);
	/* awk has no extra empty record for a terminating newline. */
	if (!length || body[length - 1] == '\n')
	{
		guint	n = g_strv_length(lines);

		if (n)
		{
			g_clear_pointer(&lines[n - 1], g_free);
		}
	}
	for (i = 0; lines[i]; i++)
	{
		gsize	n = strlen(lines[i]);

		longest = MAX(longest, n);

		while (n && (lines[i][n - 1] == ' ' || lines[i][n - 1] == '\t'))
		{
			n--;
		}
		if (!n)
		{
			continue;
		}
		total++;

		if (strchr(";{}()[]", lines[i][n - 1]))
		{
			code++;
		}
	}
	/* Reflow is catastrophic for source code: it joins logical lines. */
	reflow = document->reflow == 2 ||
			 (document->reflow == 1 && !(total >= 3 && code * 100 / total >= 25));

	if (font == 2)
	{
		font = longest <= VENTURE_ESCPOS_FONT_A_WIDTH ? 0 : 1;
	}
	/* Measured on the TM-T20IV-SP's full 576-dot print area: font A
	 * (12 dots) is 48 columns and B (9 dots) is 64. The quoted 42/56
	 * assumes 512 dots; do not correct these back without a printed ruler. */
	width = document->width ? document->width
							: (font ? VENTURE_ESCPOS_FONT_B_WIDTH : VENTURE_ESCPOS_FONT_A_WIDTH);
	g_string_append(out, ESCPOS_RESET);
	command(out, VENTURE_ESCPOS_ESC, 't', page_ids[page]);
	command(out, VENTURE_ESCPOS_ESC, 'M', font);
	command(out, VENTURE_ESCPOS_ESC, 'a', align);
	command(out, VENTURE_ESCPOS_ESC, 'E', document->bold);
	command(out, VENTURE_ESCPOS_GS, '!', document->double_height ? 1 : 0);

	if (document->title && *document->title)
	{
		g_autofree gchar	*title =
			encode(document->title, document->codepage, document->tabs, FALSE);
		g_autofree gchar	*rule = g_strnfill(width, '-');
		gsize				n = strlen(title);

		while (n && title[n - 1] == '\n')
		{
			title[--n] = '\0';
		}
		command(out, VENTURE_ESCPOS_ESC, 'a', 1);
		command(out, VENTURE_ESCPOS_ESC, 'E', 1);
		line_out(out, "", title);
		command(out, VENTURE_ESCPOS_ESC, 'E', 0);

		if (document->timestamp)
		{
			line_out(out, "", document->timestamp);
		}
		line_out(out, "", rule);
		command(out, VENTURE_ESCPOS_ESC, 'a', align);
	}
	else if (document->timestamp)
	{
		command(out, VENTURE_ESCPOS_ESC, 'a', 1);
		line_out(out, "", document->timestamp);
		command(out, VENTURE_ESCPOS_ESC, 'a', align);
	}
	if (document->wrap)
	{
		format_text(out, lines, width, reflow, longest);
	}
	else if (length)
	{
		g_string_append(out, body);

		if (body[length - 1] != '\n')
		{
			g_string_append_c(out, '\n');
		}
	}
	/* Native symbols carry byte counts, not converted text widths. */
	for (i = 0; document->qr && document->qr[i]; i++)
	{
		const gchar	*data = document->qr[i];
		gsize		n = strlen(data) + 3;

		if (n > ESCPOS_QR_STORE_MAX)
		{
			goto invalid;
		}
		g_string_append_c(out, '\n');
		command(out, VENTURE_ESCPOS_ESC, 'a', 1);
		g_string_append_len(out, ESCPOS_QR_MODEL_2, sizeof ESCPOS_QR_MODEL_2 - 1);
		g_string_append_len(out, ESCPOS_QR_SIZE, sizeof ESCPOS_QR_SIZE - 1);
		g_string_append_c(out, document->qr_size);
		g_string_append_len(out, ESCPOS_QR_CORRECTION_M, sizeof ESCPOS_QR_CORRECTION_M - 1);
		g_string_append(out, ESCPOS_QR_STORE);
		g_string_append_c(out, n % 256);
		g_string_append_c(out, n / 256);
		g_string_append(out, ESCPOS_QR_STORE_DATA);
		g_string_append(out, data);
		g_string_append_len(out, ESCPOS_QR_PRINT, sizeof ESCPOS_QR_PRINT - 1);
		command(out, VENTURE_ESCPOS_ESC, 'a', 0);
		g_string_append_c(out, '\n');
	}
	for (i = 0; document->barcodes && document->barcodes[i]; i++)
	{
		g_autofree gchar	*payload = barcode == 8 ? g_strconcat("{B", document->barcodes[i], NULL)
												 : g_strdup(document->barcodes[i]);
		gsize	n = strlen(payload);

		if (n < 1 || n > ESCPOS_BYTE_MAX)
		{
			goto invalid;
		}
		g_string_append_c(out, '\n');
		command(out, VENTURE_ESCPOS_GS, 'h', ESCPOS_BARCODE_HEIGHT);
		command(out, VENTURE_ESCPOS_GS, 'w', ESCPOS_BARCODE_WIDTH);
		command(out, VENTURE_ESCPOS_GS, 'H', 2);
		command(out, VENTURE_ESCPOS_GS, 'f', 0);
		command(out, VENTURE_ESCPOS_ESC, 'a', 1);
		command(out, VENTURE_ESCPOS_GS, 'k', barcode + ESCPOS_BARCODE_FIRST_SYSTEM);
		g_string_append_c(out, n);
		g_string_append(out, payload);
		g_string_append_c(out, '\n');
		command(out, VENTURE_ESCPOS_ESC, 'a', 0);
	}
	/* Reset modes before feeding so the next receipt starts predictably. */
	command(out, VENTURE_ESCPOS_ESC, 'E', 0);
	command(out, VENTURE_ESCPOS_GS, '!', 0);
	command(out, VENTURE_ESCPOS_ESC, 'a', 0);
	command(out, VENTURE_ESCPOS_ESC, 'd', document->feed);

	if (document->cut || document->cut_only)
	{
		g_string_append(out, ESCPOS_CUT);
		g_string_append_c(out, 0);
	}
	return g_string_free_to_bytes(g_steal_pointer(&out));
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
						"Invalid ESC/POS layout or payload length");
	return NULL;
}
