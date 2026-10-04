/*
 * venture-markdown.c - The markdown a comment is written in
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two passes, the way markdown is usually read: blocks first, line by line
 * (fences, headings, rules, quotes, lists, tables, paragraphs), then the
 * inline syntax inside each block. Nested blocks -- a quote, a list item --
 * are their own line arrays, rendered by the same block pass one level
 * deeper.
 *
 * The security property is that nothing the author typed is ever copied to
 * the output except through md_escape_char(): every tag written here is a
 * literal in this file, every attribute value is escaped, and a link's
 * target is checked before it is written at all. The parser can be wrong
 * about what somebody meant -- emphasis that does not close the way they
 * hoped -- but being wrong only ever produces escaped text.
 */

#include "venture.h"

#include <string.h>

/* Above this a body is refused at the save; the renderer still bounds its
 * own work rather than trusting the caller to have checked. */
#define MD_MAX_INPUT (256 * 1024)

/* The delimiters whose closers are searched for, as indices into the
 * per-call cache of searches already known to fail. */
enum
{
	MD_DELIM_STRONG_STAR = 0,
	MD_DELIM_STRONG_UNDER,
	MD_DELIM_EM_STAR,
	MD_DELIM_EM_UNDER,
	MD_DELIM_STRIKE,
	MD_DELIM_COUNT
};

typedef struct
{
	const VentureMarkdownOptions *options;
} MdContext;

/* --- Output ---------------------------------------------------------------- */

/*
 * The one place source text reaches the output. Single quotes are escaped
 * as well as double, because the output may one day sit in an attribute
 * somebody quoted with them.
 */
static void
md_escape_char(
	GString	*out,
	gchar	 c
){
	switch (c)
	{
	case '&':
		g_string_append(out, "&amp;");
		break;
	case '<':
		g_string_append(out, "&lt;");
		break;
	case '>':
		g_string_append(out, "&gt;");
		break;
	case '"':
		g_string_append(out, "&quot;");
		break;
	case '\'':
		g_string_append(out, "&#39;");
		break;
	default:
		g_string_append_c(out, c);
		break;
	}
}

static void
md_escape_len(
	GString		*out,
	const gchar	*text,
	gsize		 len
){
	gsize i;

	for (i = 0; i < len; i++)
		md_escape_char(out, text[i]);
}

/* --- Small readers --------------------------------------------------------- */

static gboolean
md_is_blank(const gchar *line)
{
	const gchar *p;

	for (p = line; '\0' != *p; p++)
	{
		if ((' ' != *p) && ('\t' != *p))
			return FALSE;
	}

	return TRUE;
}

/*
 * The indentation of a line in columns, a tab counting four, and where its
 * text starts.
 */
static guint
md_indent(
	const gchar	*line,
	const gchar	**text
){
	guint columns = 0;
	const gchar *p;

	for (p = line; (' ' == *p) || ('\t' == *p); p++)
		columns += ('\t' == *p) ? 4 : 1;

	if (NULL != text)
		*text = p;

	return columns;
}

/*
 * The line with up to @columns of its indentation removed. A tab that
 * straddles the cut is taken whole: a list item's content is never
 * half-indented in practice.
 */
static gchar *
md_strip_indent(
	const gchar	*line,
	guint		 columns
){
	guint taken = 0;
	const gchar *p;

	for (p = line; (taken < columns) && ((' ' == *p) || ('\t' == *p)); p++)
		taken += ('\t' == *p) ? 4 : 1;

	return g_strdup(p);
}

/* How many of @c start at @s[i]. */
static gsize
md_run(
	const gchar	*s,
	gsize		 i,
	gsize		 len,
	gchar		 c
){
	gsize n = 0;

	while ((i + n < len) && (c == s[i + n]))
		n++;

	return n;
}

/* --- Block recognisers ----------------------------------------------------- */

/*
 * An opening code fence: three or more backticks or tildes, at most three
 * spaces in. The info string's first word is the language, kept only when
 * it looks like one -- it becomes part of a class name.
 */
static gboolean
md_fence_open(
	const gchar	 *line,
	gchar		 *fence_char,
	gsize		 *fence_len,
	gchar		**language
){
	const gchar *text;
	const gchar *info;
	gsize run;
	gsize n;

	if (md_indent(line, &text) > 3)
		return FALSE;

	if (('`' != text[0]) && ('~' != text[0]))
		return FALSE;

	run = md_run(text, 0, strlen(text), text[0]);

	if (run < 3)
		return FALSE;

	info = text + run;

	/* A backtick fence's info string may not hold a backtick, or a line
	 * of inline code would open a block. */
	if (('`' == text[0]) && (NULL != strchr(info, '`')))
		return FALSE;

	while ((' ' == *info) || ('\t' == *info))
		info++;

	n = 0;

	while ((n < 24) && ('\0' != info[n]) &&
	       (g_ascii_isalnum(info[n]) || (NULL != strchr("_+.#-", info[n]))))
		n++;

	*fence_char = text[0];
	*fence_len = run;
	*language = ((n > 0) && ((' ' == info[n]) || ('\t' == info[n]) ||
	                         ('\0' == info[n])))
		? g_strndup(info, n) : NULL;

	return TRUE;
}

static gboolean
md_fence_close(
	const gchar	*line,
	gchar		 fence_char,
	gsize		 fence_len
){
	const gchar *text;
	gsize run;

	if (md_indent(line, &text) > 3)
		return FALSE;

	run = md_run(text, 0, strlen(text), fence_char);

	return (run >= fence_len) && md_is_blank(text + run);
}

/* "# Title" to "######  Title ##", with the text between. */
static gboolean
md_heading(
	const gchar	 *line,
	guint		 *level,
	const gchar	**body,
	gsize		 *body_len
){
	const gchar *text;
	const gchar *end;
	gsize run;

	if (md_indent(line, &text) > 3)
		return FALSE;

	run = md_run(text, 0, strlen(text), '#');

	if ((run < 1) || (run > 6))
		return FALSE;

	if (('\0' != text[run]) && (' ' != text[run]) && ('\t' != text[run]))
		return FALSE;

	text += run;

	while ((' ' == *text) || ('\t' == *text))
		text++;

	end = text + strlen(text);

	while ((end > text) && ((' ' == end[-1]) || ('\t' == end[-1])))
		end--;

	/* A closing run of #s preceded by a space is decoration. */
	{
		const gchar *hashes = end;

		while ((hashes > text) && ('#' == hashes[-1]))
			hashes--;

		if ((hashes < end) &&
		    ((hashes == text) || (' ' == hashes[-1]) || ('\t' == hashes[-1])))
		{
			end = hashes;

			while ((end > text) && ((' ' == end[-1]) || ('\t' == end[-1])))
				end--;
		}
	}

	*level = (guint)run;
	*body = text;
	*body_len = (gsize)(end - text);

	return TRUE;
}

/* Three or more of one of - * _, with only spaces between. */
static gboolean
md_rule(const gchar *line)
{
	const gchar *text;
	const gchar *p;
	gchar mark;
	guint count = 0;

	if (md_indent(line, &text) > 3)
		return FALSE;

	mark = text[0];

	if (('-' != mark) && ('*' != mark) && ('_' != mark))
		return FALSE;

	for (p = text; '\0' != *p; p++)
	{
		if (mark == *p)
			count++;
		else if ((' ' != *p) && ('\t' != *p))
			return FALSE;
	}

	return count >= 3;
}

static gboolean
md_quote(const gchar *line)
{
	const gchar *text;

	return (md_indent(line, &text) <= 3) && ('>' == text[0]);
}

/*
 * A list item: "- ", "* ", "+ " or "12. " / "12) ", with what follows.
 * The content column is where a continuation line must be indented to
 * belong to the item.
 */
typedef struct
{
	guint		 indent;
	gboolean	 ordered;
	gint64		 number;
	guint		 content_column;
	const gchar	*content;
} MdItem;

static gboolean
md_list_item(
	const gchar	*line,
	MdItem		*item
){
	const gchar *text;
	const gchar *after;
	guint indent;
	guint marker;
	guint spaces;

	indent = md_indent(line, &text);

	if (('-' == text[0]) || ('*' == text[0]) || ('+' == text[0]))
	{
		item->ordered = FALSE;
		item->number = 0;
		marker = 1;
	}
	else if (g_ascii_isdigit(text[0]))
	{
		guint digits = 0;

		while ((digits < 9) && g_ascii_isdigit(text[digits]))
			digits++;

		if (('.' != text[digits]) && (')' != text[digits]))
			return FALSE;

		item->ordered = TRUE;
		item->number = g_ascii_strtoll(text, NULL, 10);
		marker = digits + 1;
	}
	else
	{
		return FALSE;
	}

	after = text + marker;

	/* "-x" is a word, not an item; an item with nothing after it is
	 * still an item. */
	if (('\0' != *after) && (' ' != *after) && ('\t' != *after))
		return FALSE;

	spaces = 0;

	while (((' ' == after[spaces]) || ('\t' == after[spaces])) && (spaces < 5))
		spaces++;

	/* Five spaces in is code under the item, not more indentation; and
	 * an empty item's content starts one past the marker. */
	if ((spaces > 4) || ('\0' == after[spaces]))
		spaces = 1;

	item->indent = indent;
	item->content_column = indent + marker + spaces;
	item->content = after;

	while ((' ' == *item->content) || ('\t' == *item->content))
		item->content++;

	return TRUE;
}

/*
 * The cells of a table row, split on pipes that are not escaped; a
 * leading and a trailing pipe frame the row and are not cells.
 */
static GPtrArray *
md_table_cells(const gchar *line)
{
	GPtrArray *cells;
	g_autoptr(GString) cell = NULL;
	const gchar *p;
	const gchar *end;

	cells = g_ptr_array_new_with_free_func(g_free);
	p = line;

	while ((' ' == *p) || ('\t' == *p))
		p++;

	if ('|' == *p)
		p++;

	end = p + strlen(p);

	while ((end > p) && ((' ' == end[-1]) || ('\t' == end[-1])))
		end--;

	if ((end > p) && ('|' == end[-1]) && ((end - 1 == p) || ('\\' != end[-2])))
		end--;

	cell = g_string_new(NULL);

	for (; p < end; p++)
	{
		if (('\\' == *p) && (p + 1 < end) && ('|' == p[1]))
		{
			g_string_append_c(cell, '|');
			p++;
			continue;
		}

		if ('|' == *p)
		{
			g_ptr_array_add(cells, g_strdup(g_strstrip(cell->str)));
			g_string_truncate(cell, 0);
			continue;
		}

		g_string_append_c(cell, *p);
	}

	g_ptr_array_add(cells, g_strdup(g_strstrip(cell->str)));

	return cells;
}

/*
 * The line under a table's header: dashes, optionally with colons for
 * alignment, one per column. 'l', 'c', 'r' or '\0' per column.
 */
static gboolean
md_table_delimiter(
	const gchar	 *line,
	guint		  columns,
	gchar		**aligns
){
	g_autoptr(GPtrArray) cells = NULL;
	guint i;

	if (NULL == strchr(line, '-'))
		return FALSE;

	cells = md_table_cells(line);

	if (cells->len != columns)
		return FALSE;

	*aligns = g_new0(gchar, columns + 1);

	for (i = 0; i < cells->len; i++)
	{
		const gchar *cell = g_ptr_array_index(cells, i);
		gsize length = strlen(cell);
		gboolean left;
		gboolean right;
		gsize j;

		if (0 == length)
		{
			g_clear_pointer(aligns, g_free);
			return FALSE;
		}

		left = (':' == cell[0]);
		right = (length > 1) && (':' == cell[length - 1]);

		for (j = left ? 1 : 0; j < (right ? length - 1 : length); j++)
		{
			if ('-' != cell[j])
			{
				g_clear_pointer(aligns, g_free);
				return FALSE;
			}
		}

		(*aligns)[i] = (left && right) ? 'c' : right ? 'r' : left ? 'l' : 'n';
	}

	return TRUE;
}

/* A header row followed by its delimiter: the start of a table. */
static gboolean
md_table_start(
	GPtrArray	 *lines,
	guint		  i,
	guint		 *columns,
	gchar		**aligns
){
	g_autoptr(GPtrArray) header = NULL;
	const gchar *line;

	line = g_ptr_array_index(lines, i);

	if ((i + 1 >= lines->len) || (NULL == strchr(line, '|')))
		return FALSE;

	header = md_table_cells(line);

	/* A bound on columns: a "table" a thousand pipes wide is a line of
	 * pipes, not a table anybody meant. */
	if ((header->len < 1) || (header->len > 24))
		return FALSE;

	if (!md_table_delimiter(g_ptr_array_index(lines, i + 1), header->len,
	                        aligns))
		return FALSE;

	*columns = header->len;

	return TRUE;
}

/* Whether a line begins a block that ends the paragraph before it. */
static gboolean
md_interrupts(
	GPtrArray	*lines,
	guint		 i
){
	const gchar *line;
	gchar fence_char;
	gsize fence_len;
	g_autofree gchar *language = NULL;
	g_autofree gchar *aligns = NULL;
	const gchar *body;
	gsize body_len;
	guint level;
	guint columns;
	MdItem item;

	line = g_ptr_array_index(lines, i);

	return md_is_blank(line) ||
	       md_fence_open(line, &fence_char, &fence_len, &language) ||
	       md_heading(line, &level, &body, &body_len) ||
	       md_rule(line) ||
	       md_quote(line) ||
	       md_list_item(line, &item) ||
	       md_table_start(lines, i, &columns, &aligns);
}

/* --- Inline ---------------------------------------------------------------- */

static void
md_inline(
	MdContext	*ctx,
	GString		*out,
	const gchar	*s,
	gsize		 len,
	guint		 depth,
	gboolean	 in_link
);

/*
 * The end of a code span opened by @run backticks: the next run of exactly
 * that many.
 */
static gboolean
md_backtick_close(
	const gchar	*s,
	gsize		 from,
	gsize		 len,
	gsize		 run,
	gsize		*at
){
	gsize j = from;

	while (j < len)
	{
		if ('`' == s[j])
		{
			gsize here = md_run(s, j, len, '`');

			if (here == run)
			{
				*at = j;
				return TRUE;
			}

			j += here;
			continue;
		}

		j++;
	}

	return FALSE;
}

/*
 * The closing delimiter for emphasis opened just before @from: not
 * preceded by a space, not inside a code span, not escaped, and for a
 * single mark not half of a double. An underscore must also end a word, so
 * snake_case_names stay names.
 */
static gboolean
md_find_closer(
	const gchar	*s,
	gsize		 from,
	gsize		 len,
	const gchar	*delim,
	gsize		 k,
	gsize		*at
){
	gsize j = from;

	while (j + k <= len)
	{
		gchar c = s[j];

		if (('\\' == c) && (j + 1 < len))
		{
			j += 2;
			continue;
		}

		if ('`' == c)
		{
			gsize run = md_run(s, j, len, '`');
			gsize close;

			if (md_backtick_close(s, j + run, len, run, &close))
				j = close + run;
			else
				j += run;

			continue;
		}

		if (0 == memcmp(s + j, delim, k))
		{
			gsize run = md_run(s, j, len, delim[0]);
			gboolean ok;

			ok = (j > from) && !g_ascii_isspace(s[j - 1]);

			/* A single mark never closes on part of a longer run. */
			if (ok && (1 == k) && (1 != run))
				ok = FALSE;

			if (ok && ('_' == delim[0]) && (j + k < len) &&
			    g_ascii_isalnum(s[j + k]))
				ok = FALSE;

			if (ok)
			{
				/* "***" closes a strong run with its last two marks,
				 * leaving the first to close an emphasis inside it:
				 * "**bold *and it***". */
				*at = ((2 == k) && (run > 2)) ? j + run - 2 : j;
				return TRUE;
			}

			j += (1 == k) ? run : 1;
			continue;
		}

		j++;
	}

	return FALSE;
}

/*
 * Looks for a closer, remembering a failure: a search from a later point
 * covers less of the text, so it cannot succeed where an earlier one
 * failed. Without the memory, "**" repeated ten thousand times is a
 * quadratic number of comparisons.
 */
static gboolean
md_closer_cached(
	gsize		*fail,
	guint		 kind,
	const gchar	*s,
	gsize		 from,
	gsize		 len,
	const gchar	*delim,
	gsize		 k,
	gsize		*at
){
	if (from >= fail[kind])
		return FALSE;

	if (md_find_closer(s, from, len, delim, k, at))
		return TRUE;

	fail[kind] = from;

	return FALSE;
}

/*
 * Where a link may point: an http(s) address, a path on this site, or an
 * anchor on this page. Nothing else -- javascript:, data:, a
 * protocol-relative "//host" -- is ever written as an href.
 */
static gboolean
md_url_is_allowed(const gchar *url)
{
	const gchar *p;

	if ((NULL == url) || ('\0' == *url))
		return FALSE;

	for (p = url; '\0' != *p; p++)
	{
		if (((guchar)*p <= 0x20) || (0x7f == (guchar)*p) || ('\\' == *p) ||
		    ('<' == *p) || ('>' == *p) || ('"' == *p) || ('`' == *p))
			return FALSE;
	}

	if (0 == g_ascii_strncasecmp(url, "https://", 8))
		return '\0' != url[8];

	if (0 == g_ascii_strncasecmp(url, "http://", 7))
		return '\0' != url[7];

	if ('/' == url[0])
		return '/' != url[1];

	if ('#' == url[0])
	{
		for (p = url + 1; '\0' != *p; p++)
		{
			if (!g_ascii_isalnum(*p) && ('-' != *p) && ('_' != *p))
				return FALSE;
		}

		return '\0' != url[1];
	}

	return FALSE;
}

/*
 * "[text](target)" or "[text](target "title")" at @s[i]. The bracket may
 * nest; the target may hold one level of balanced parentheses, which is
 * how Wikipedia addresses are written.
 */
static gboolean
md_parse_link(
	const gchar	*s,
	gsize		 i,
	gsize		 len,
	gsize		*text_end,
	gsize		*url_start,
	gsize		*url_end,
	gsize		*after
){
	gsize j;
	guint depth = 0;
	guint parens = 0;

	for (j = i + 1; j < len; j++)
	{
		if (('\\' == s[j]) && (j + 1 < len))
		{
			j++;
			continue;
		}

		if ('[' == s[j])
			depth++;
		else if (']' == s[j])
		{
			if (0 == depth)
				break;
			depth--;
		}
	}

	if ((j >= len) || (j + 1 >= len) || ('(' != s[j + 1]))
		return FALSE;

	*text_end = j;
	j += 2;

	while ((j < len) && (' ' == s[j]))
		j++;

	*url_start = j;

	while ((j < len) && !g_ascii_isspace(s[j]))
	{
		if ('(' == s[j])
			parens++;
		else if (')' == s[j])
		{
			if (0 == parens)
				break;
			parens--;
		}

		j++;
	}

	*url_end = j;

	if ((*url_end == *url_start) || (*url_end - *url_start > 2048))
		return FALSE;

	while ((j < len) && (' ' == s[j]))
		j++;

	/* An optional title, which is read and dropped. */
	if ((j < len) && (('"' == s[j]) || ('\'' == s[j])))
	{
		gchar quote = s[j];

		j++;

		while ((j < len) && (quote != s[j]))
			j++;

		if (j >= len)
			return FALSE;

		j++;

		while ((j < len) && (' ' == s[j]))
			j++;
	}

	if ((j >= len) || (')' != s[j]))
		return FALSE;

	*after = j + 1;

	return TRUE;
}

static void
md_open_link(
	GString		*out,
	const gchar	*url,
	const gchar	*klass
){
	g_string_append(out, "<a href=\"");
	md_escape_len(out, url, strlen(url));
	g_string_append(out, "\"");

	if (NULL != klass)
		g_string_append_printf(out, " class=\"%s\"", klass);

	/* Somewhere else on the web is told nothing about this page, and
	 * gets no say in ranking from it. */
	if ('/' != url[0] && '#' != url[0])
		g_string_append(out, " rel=\"noopener noreferrer nofollow\"");

	g_string_append(out, ">");
}

/* An inline run between two delimiters, wrapped in @tag. */
static void
md_wrap(
	MdContext	*ctx,
	GString		*out,
	const gchar	*tag,
	const gchar	*s,
	gsize		 len,
	guint		 depth,
	gboolean	 in_link
){
	g_string_append_printf(out, "<%s>", tag);
	md_inline(ctx, out, s, len, depth + 1, in_link);
	g_string_append_printf(out, "</%s>", tag);
}

static void
md_inline(
	MdContext	*ctx,
	GString		*out,
	const gchar	*s,
	gsize		 len,
	guint		 depth,
	gboolean	 in_link
){
	gsize fail[MD_DELIM_COUNT];
	gsize i;
	guint k;

	if (depth > VENTURE_MARKDOWN_MAX_DEPTH)
	{
		md_escape_len(out, s, len);
		return;
	}

	for (k = 0; k < MD_DELIM_COUNT; k++)
		fail[k] = G_MAXSIZE;

	i = 0;

	while (i < len)
	{
		gchar c = s[i];
		gchar previous = (i > 0) ? s[i - 1] : ' ';

		/* A backslash makes the next punctuation mark itself. */
		if (('\\' == c) && (i + 1 < len) && g_ascii_ispunct(s[i + 1]))
		{
			md_escape_char(out, s[i + 1]);
			i += 2;
			continue;
		}

		if ('\n' == c)
		{
			/* A comment box is typed in lines; a line break is one. */
			g_string_append(out, "<br>\n");
			i++;
			continue;
		}

		if ('`' == c)
		{
			gsize run = md_run(s, i, len, '`');
			gsize close;

			if (md_backtick_close(s, i + run, len, run, &close))
			{
				const gchar *code = s + i + run;
				gsize code_len = close - (i + run);

				if ((code_len >= 2) && (' ' == code[0]) &&
				    (' ' == code[code_len - 1]))
				{
					code++;
					code_len -= 2;
				}

				g_string_append(out, "<code>");
				md_escape_len(out, code, code_len);
				g_string_append(out, "</code>");
				i = close + run;
				continue;
			}

			md_escape_len(out, s + i, run);
			i += run;
			continue;
		}

		if (('[' == c) && !in_link)
		{
			gsize text_end;
			gsize url_start;
			gsize url_end;
			gsize after;

			if (md_parse_link(s, i, len, &text_end, &url_start, &url_end,
			                  &after))
			{
				g_autofree gchar *url = NULL;

				url = g_strndup(s + url_start, url_end - url_start);

				if (md_url_is_allowed(url))
				{
					md_open_link(out, url, NULL);
					md_inline(ctx, out, s + i + 1, text_end - (i + 1),
					          depth + 1, TRUE);
					g_string_append(out, "</a>");
					i = after;
					continue;
				}
			}

			md_escape_char(out, c);
			i++;
			continue;
		}

		if (('*' == c) || ('_' == c))
		{
			gsize run = md_run(s, i, len, c);
			gboolean opens;
			gsize at;

			/* Opens when followed by text; an underscore must also
			 * start a word, or snake_case would turn italic. */
			opens = (i + run < len) && !g_ascii_isspace(s[i + run]) &&
			        (('*' == c) || !g_ascii_isalnum(previous));

			if (opens && (run >= 2) &&
			    md_closer_cached(fail,
			                     ('*' == c) ? MD_DELIM_STRONG_STAR
			                                : MD_DELIM_STRONG_UNDER,
			                     s, i + 2, len, ('*' == c) ? "**" : "__", 2,
			                     &at))
			{
				md_wrap(ctx, out, "strong", s + i + 2, at - (i + 2), depth,
				        in_link);
				i = at + 2;
				continue;
			}

			if (opens && (1 == run) &&
			    md_closer_cached(fail,
			                     ('*' == c) ? MD_DELIM_EM_STAR
			                                : MD_DELIM_EM_UNDER,
			                     s, i + 1, len, ('*' == c) ? "*" : "_", 1,
			                     &at))
			{
				md_wrap(ctx, out, "em", s + i + 1, at - (i + 1), depth,
				        in_link);
				i = at + 1;
				continue;
			}

			md_escape_len(out, s + i, run);
			i += run;
			continue;
		}

		if ('~' == c)
		{
			gsize run = md_run(s, i, len, '~');
			gsize at;

			if ((2 == run) && (i + 2 < len) && !g_ascii_isspace(s[i + 2]) &&
			    md_closer_cached(fail, MD_DELIM_STRIKE, s, i + 2, len, "~~", 2,
			                     &at))
			{
				md_wrap(ctx, out, "del", s + i + 2, at - (i + 2), depth,
				        in_link);
				i = at + 2;
				continue;
			}

			md_escape_len(out, s + i, run);
			i += run;
			continue;
		}

		/* @name: the same grammar the inbox reads mentions with -- an
		 * @ inside a word, an email address, names nobody. */
		if (('@' == c) && !in_link && (NULL != ctx->options) &&
		    (NULL != ctx->options->mention) &&
		    !g_ascii_isalnum(previous) && ('.' != previous))
		{
			gsize start = i + 1;
			gsize end = start;

			while ((end < len) &&
			       (g_ascii_isalnum(s[end]) || ('_' == s[end]) ||
			        ('-' == s[end]) || ('.' == s[end])))
				end++;

			while ((end > start) && ('.' == s[end - 1]))
				end--;

			if ((end > start) && (end - start <= 64))
			{
				g_autofree gchar *name = NULL;
				g_autofree gchar *title = NULL;

				name = g_strndup(s + start, end - start);
				title = ctx->options->mention(name, ctx->options->user_data);

				if (NULL != title)
				{
					g_string_append(out, "<span class=\"mention\" title=\"");
					md_escape_len(out, title, strlen(title));
					g_string_append(out, "\">@");
					md_escape_len(out, name, strlen(name));
					g_string_append(out, "</span>");
					i = end;
					continue;
				}
			}
		}

		/* #type/id: a record, linked by its name when the reader may
		 * open it, and left as typed when not. */
		if (('#' == c) && !in_link && (NULL != ctx->options) &&
		    (NULL != ctx->options->reference) &&
		    !g_ascii_isalnum(previous) && ('/' != previous) &&
		    (i + 1 < len) && g_ascii_islower(s[i + 1]))
		{
			gsize start = i + 1;
			gsize slash = start;
			gsize end;

			while ((slash < len) && (slash - start < 64) &&
			       (g_ascii_islower(s[slash]) || g_ascii_isdigit(s[slash]) ||
			        ('_' == s[slash])))
				slash++;

			end = slash + 1;

			while ((end < len) && (end - slash <= 18) &&
			       g_ascii_isdigit(s[end]))
				end++;

			if ((slash < len) && ('/' == s[slash]) && (end > slash + 1) &&
			    ((end == len) || !g_ascii_isalnum(s[end])))
			{
				g_autofree gchar *type_name = NULL;
				g_autofree gchar *label = NULL;
				g_autofree gchar *href = NULL;
				gint64 id;

				type_name = g_strndup(s + start, slash - start);
				id = g_ascii_strtoll(s + slash + 1, NULL, 10);
				label = (id > 0)
					? ctx->options->reference(type_name, id,
					                          ctx->options->user_data)
					: NULL;

				if (NULL != label)
				{
					href = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT,
					                       type_name, id);
					md_open_link(out, href, "record-ref");
					md_escape_len(out, label, strlen(label));
					g_string_append(out, "</a>");
					i = end;
					continue;
				}
			}
		}

		/* A bare web address is a link, less the sentence's own
		 * punctuation after it. */
		if ((('h' == c) || ('H' == c)) && !in_link &&
		    !g_ascii_isalnum(previous) && ('/' != previous) &&
		    ((0 == g_ascii_strncasecmp(s + i, "https://",
		                               MIN((gsize)8, len - i))) ||
		     (0 == g_ascii_strncasecmp(s + i, "http://",
		                               MIN((gsize)7, len - i)))) &&
		    (len - i > 8))
		{
			gsize scheme = (0 == g_ascii_strncasecmp(s + i, "https://", 8))
				? 8 : 7;
			gsize end = i;
			gint open_parens = 0;
			gint close_parens = 0;
			gsize j;

			while ((end < len) && ((guchar)s[end] > 0x20) &&
			       ('<' != s[end]) && ('>' != s[end]) && ('"' != s[end]) &&
			       ('`' != s[end]) && ('\\' != s[end]))
				end++;

			while ((end > i) && (NULL != strchr(".,;:!?*_~'", s[end - 1])))
				end--;

			for (j = i; j < end; j++)
			{
				if ('(' == s[j])
					open_parens++;
				else if (')' == s[j])
					close_parens++;
			}

			while ((end > i) && (')' == s[end - 1]) &&
			       (close_parens > open_parens))
			{
				end--;
				close_parens--;
			}

			if (end - i > scheme)
			{
				g_autofree gchar *url = NULL;

				url = g_strndup(s + i, end - i);

				if (md_url_is_allowed(url))
				{
					md_open_link(out, url, NULL);
					md_escape_len(out, url, strlen(url));
					g_string_append(out, "</a>");
					i = end;
					continue;
				}
			}
		}

		md_escape_char(out, c);
		i++;
	}
}

/* --- Blocks ---------------------------------------------------------------- */

static void
md_blocks(
	MdContext	*ctx,
	GString		*out,
	GPtrArray	*lines,
	guint		 depth,
	gboolean	 tight
);

/*
 * The heading levels a comment may use. A comment sits inside a page that
 * has its own h1 and h2; "# Summary" in a comment is a section of the
 * comment, so it starts at h3.
 */
static guint
md_heading_tag(guint level)
{
	return MIN(level + 2, (guint)6);
}

static guint
md_paragraph(
	MdContext	*ctx,
	GString		*out,
	GPtrArray	*lines,
	guint		 i,
	guint		 depth,
	gboolean	 tight
){
	g_autoptr(GString) text = NULL;
	guint start = i;

	text = g_string_new(NULL);

	while ((i < lines->len) && ((i == start) || !md_interrupts(lines, i)))
	{
		g_autofree gchar *line = NULL;

		line = g_strstrip(g_strdup(g_ptr_array_index(lines, i)));

		if (i > start)
			g_string_append_c(text, '\n');

		g_string_append(text, line);
		i++;
	}

	if (!tight)
		g_string_append(out, "<p>");

	md_inline(ctx, out, text->str, text->len, depth, FALSE);

	if (!tight)
		g_string_append(out, "</p>");

	return i;
}

static guint
md_fence(
	GString		*out,
	GPtrArray	*lines,
	guint		 i
){
	g_autofree gchar *language = NULL;
	gchar fence_char;
	gsize fence_len;
	guint indent;
	gboolean first = TRUE;

	md_fence_open(g_ptr_array_index(lines, i), &fence_char, &fence_len,
	              &language);
	indent = md_indent(g_ptr_array_index(lines, i), NULL);
	i++;

	g_string_append(out, "<pre><code");

	if (NULL != language)
	{
		g_string_append(out, " class=\"language-");
		md_escape_len(out, language, strlen(language));
		g_string_append(out, "\"");
	}

	g_string_append(out, ">");

	/* Everything up to the closing fence, byte for byte: no list, no
	 * emphasis and no link rule reaches inside, and an unterminated
	 * fence ends with the comment. */
	while ((i < lines->len) &&
	       !md_fence_close(g_ptr_array_index(lines, i), fence_char, fence_len))
	{
		g_autofree gchar *line = NULL;

		line = md_strip_indent(g_ptr_array_index(lines, i), indent);

		if (!first)
			g_string_append_c(out, '\n');

		md_escape_len(out, line, strlen(line));
		first = FALSE;
		i++;
	}

	g_string_append(out, "</code></pre>");

	return (i < lines->len) ? i + 1 : i;
}

static guint
md_blockquote(
	MdContext	*ctx,
	GString		*out,
	GPtrArray	*lines,
	guint		 i,
	guint		 depth
){
	g_autoptr(GPtrArray) inner = NULL;

	inner = g_ptr_array_new_with_free_func(g_free);

	while ((i < lines->len) && md_quote(g_ptr_array_index(lines, i)))
	{
		const gchar *text;

		md_indent(g_ptr_array_index(lines, i), &text);
		text++;

		if (' ' == *text)
			text++;

		g_ptr_array_add(inner, g_strdup(text));
		i++;
	}

	g_string_append(out, "<blockquote>");
	md_blocks(ctx, out, inner, depth + 1, FALSE);
	g_string_append(out, "</blockquote>");

	return i;
}

static guint
md_table(
	MdContext	*ctx,
	GString		*out,
	GPtrArray	*lines,
	guint		 i,
	guint		 columns,
	const gchar	*aligns,
	guint		 depth
){
	guint row;

	g_string_append(out, "<div class=\"md-table\"><table>");

	for (row = 0; i < lines->len; row++)
	{
		g_autoptr(GPtrArray) cells = NULL;
		const gchar *line;
		guint c;

		/* The delimiter row is what made this a table; it is not a
		 * row of it. */
		if (1 == row)
		{
			i++;
			continue;
		}

		line = g_ptr_array_index(lines, i);

		if ((row > 1) && (md_is_blank(line) || (NULL == strchr(line, '|'))))
			break;

		cells = md_table_cells(line);
		g_string_append(out, (0 == row) ? "<thead><tr>" : "<tr>");

		for (c = 0; c < columns; c++)
		{
			const gchar *tag = (0 == row) ? "th" : "td";
			const gchar *cell = (c < cells->len)
				? (const gchar *)g_ptr_array_index(cells, c) : "";

			g_string_append_printf(out, "<%s", tag);

			if ('l' == aligns[c])
				g_string_append(out, " class=\"md-left\"");
			else if ('c' == aligns[c])
				g_string_append(out, " class=\"md-center\"");
			else if ('r' == aligns[c])
				g_string_append(out, " class=\"md-right\"");

			g_string_append(out, ">");
			md_inline(ctx, out, cell, strlen(cell), depth, FALSE);
			g_string_append_printf(out, "</%s>", tag);
		}

		g_string_append(out, (0 == row) ? "</tr></thead><tbody>" : "</tr>");
		i++;
	}

	g_string_append(out, "</tbody></table></div>");

	return i;
}

/* One item of a list: its lines, de-indented, and whether it is a task. */
typedef struct
{
	GPtrArray	*lines;
	gint		 task;
} MdListEntry;

static void
md_list_entry_free(gpointer data)
{
	MdListEntry *entry = data;

	g_ptr_array_unref(entry->lines);
	g_free(entry);
}

/*
 * A list, and every list nested in its items. Items are collected first
 * and rendered after, because whether the list is tight -- no blank line
 * anywhere in it, so its items are not paragraphs -- is only known at the
 * end.
 */
static guint
md_list(
	MdContext	*ctx,
	GString		*out,
	GPtrArray	*lines,
	guint		 i,
	guint		 depth
){
	g_autoptr(GPtrArray) entries = NULL;
	MdItem first;
	gboolean tight = TRUE;
	gboolean ended = FALSE;
	guint e;

	md_list_item(g_ptr_array_index(lines, i), &first);
	entries = g_ptr_array_new_with_free_func(md_list_entry_free);

	while (!ended && (i < lines->len))
	{
		MdListEntry *entry;
		MdItem item;
		guint j;

		if (!md_list_item(g_ptr_array_index(lines, i), &item) ||
		    (item.ordered != first.ordered) ||
		    (item.indent > first.indent + 3) || md_rule(g_ptr_array_index(lines, i)))
			break;

		entry = g_new0(MdListEntry, 1);
		entry->lines = g_ptr_array_new_with_free_func(g_free);

		/* "[ ] " and "[x] " make a checklist item. */
		if ((0 == strncmp(item.content, "[ ] ", 4)) ||
		    (0 == g_strcmp0(item.content, "[ ]")))
			entry->task = 1;
		else if ((0 == g_ascii_strncasecmp(item.content, "[x] ", 4)) ||
		         (0 == g_ascii_strcasecmp(item.content, "[x]")))
			entry->task = 2;

		g_ptr_array_add(entry->lines,
			g_strdup((0 != entry->task)
				? item.content + MIN(strlen(item.content), (gsize)4)
				: item.content));
		g_ptr_array_add(entries, entry);

		for (j = i + 1; j < lines->len; j++)
		{
			const gchar *line = g_ptr_array_index(lines, j);
			MdItem next;
			guint indent;

			if (md_is_blank(line))
			{
				guint k = j;

				while ((k < lines->len) &&
				       md_is_blank(g_ptr_array_index(lines, k)))
					k++;

				if (k >= lines->len)
				{
					ended = TRUE;
					break;
				}

				if (md_indent(g_ptr_array_index(lines, k), NULL) >=
				    item.content_column)
				{
					/* A blank line inside an item: the item
					 * holds paragraphs, so the list is loose. */
					for (; j < k; j++)
						g_ptr_array_add(entry->lines, g_strdup(""));
					tight = FALSE;
					j = k - 1;
					continue;
				}

				if (md_list_item(g_ptr_array_index(lines, k), &next) &&
				    (next.ordered == first.ordered) &&
				    (next.indent <= first.indent + 3))
				{
					tight = FALSE;
					j = k;
				}
				else
				{
					ended = TRUE;
				}

				break;
			}

			indent = md_indent(line, NULL);

			if (indent >= item.content_column)
			{
				g_ptr_array_add(entry->lines,
				                md_strip_indent(line, item.content_column));
				continue;
			}

			if (md_list_item(line, &next))
			{
				/* An item indented past this list's own marks but
				 * short of the content column: a sublist typed
				 * with less indentation than strictly needed. */
				if (next.indent > first.indent + 1)
				{
					g_ptr_array_add(entry->lines,
					                md_strip_indent(line, indent));
					continue;
				}

				break;
			}

			if (!md_interrupts(lines, j))
			{
				/* A lazy continuation of the item's text. */
				g_ptr_array_add(entry->lines, g_strstrip(g_strdup(line)));
				continue;
			}

			ended = TRUE;
			break;
		}

		i = j;
	}

	if (first.ordered)
	{
		if (1 != first.number)
			g_string_append_printf(out, "<ol start=\"%" G_GINT64_FORMAT "\">",
			                       first.number);
		else
			g_string_append(out, "<ol>");
	}
	else
	{
		g_string_append(out, "<ul>");
	}

	for (e = 0; e < entries->len; e++)
	{
		MdListEntry *entry = g_ptr_array_index(entries, e);

		if (0 != entry->task)
			g_string_append_printf(out,
				"<li class=\"task\"><input type=\"checkbox\" disabled%s "
				"aria-label=\"%s\"> ",
				(2 == entry->task) ? " checked" : "",
				(2 == entry->task) ? "Done" : "Not done");
		else
			g_string_append(out, "<li>");

		md_blocks(ctx, out, entry->lines, depth + 1, tight);
		g_string_append(out, "</li>");
	}

	g_string_append(out, first.ordered ? "</ol>" : "</ul>");

	return i;
}

static void
md_blocks(
	MdContext	*ctx,
	GString		*out,
	GPtrArray	*lines,
	guint		 depth,
	gboolean	 tight
){
	guint i = 0;

	while (i < lines->len)
	{
		const gchar *line = g_ptr_array_index(lines, i);
		g_autofree gchar *language = NULL;
		g_autofree gchar *aligns = NULL;
		const gchar *body;
		gchar fence_char;
		gsize fence_len;
		gsize body_len;
		guint level;
		guint columns;
		MdItem item;

		if (md_is_blank(line))
		{
			i++;
			continue;
		}

		/* Past the bound, what is left is text. */
		if (depth > VENTURE_MARKDOWN_MAX_DEPTH)
		{
			g_string_append(out, "<p>");

			for (; i < lines->len; i++)
			{
				md_escape_len(out, g_ptr_array_index(lines, i),
				              strlen(g_ptr_array_index(lines, i)));
				g_string_append(out, "<br>\n");
			}

			g_string_append(out, "</p>");
			return;
		}

		if (md_fence_open(line, &fence_char, &fence_len, &language))
			i = md_fence(out, lines, i);
		else if (md_heading(line, &level, &body, &body_len))
		{
			g_string_append_printf(out, "<h%u>", md_heading_tag(level));
			md_inline(ctx, out, body, body_len, depth, FALSE);
			g_string_append_printf(out, "</h%u>", md_heading_tag(level));
			i++;
		}
		else if (md_rule(line))
		{
			g_string_append(out, "<hr>");
			i++;
		}
		else if (md_quote(line))
			i = md_blockquote(ctx, out, lines, i, depth);
		else if (md_list_item(line, &item))
			i = md_list(ctx, out, lines, i, depth);
		else if (md_table_start(lines, i, &columns, &aligns))
			i = md_table(ctx, out, lines, i, columns, aligns, depth);
		else
			i = md_paragraph(ctx, out, lines, i, depth, tight);
	}
}

/* --- Entry points ---------------------------------------------------------- */

/*
 * The source as lines: valid UTF-8, carriage returns gone, control
 * characters other than a tab dropped -- a stray NUL-adjacent byte or an
 * escape sequence has no business in a page.
 */
static GPtrArray *
md_lines(const gchar *text)
{
	g_autofree gchar *valid = NULL;
	g_autoptr(GString) clean = NULL;
	g_auto(GStrv) split = NULL;
	GPtrArray *lines;
	const gchar *p;
	gsize i;

	valid = g_utf8_make_valid(text, (gssize)MIN(strlen(text), (gsize)MD_MAX_INPUT));
	clean = g_string_sized_new(strlen(valid));

	for (p = valid; '\0' != *p; p++)
	{
		guchar c = (guchar)*p;

		if ('\r' == c)
		{
			if ('\n' != p[1])
				g_string_append_c(clean, '\n');
			continue;
		}

		if ((c < 0x20) && ('\n' != c) && ('\t' != c))
			continue;

		if (0x7f == c)
			continue;

		g_string_append_c(clean, (gchar)c);
	}

	split = g_strsplit(clean->str, "\n", -1);
	lines = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; NULL != split[i]; i++)
		g_ptr_array_add(lines, g_strdup(split[i]));

	return lines;
}

gchar *
venture_markdown_to_html(
	const gchar			*text,
	const VentureMarkdownOptions	*options
){
	g_autoptr(GPtrArray) lines = NULL;
	GString *out;
	MdContext ctx;

	out = g_string_new(NULL);

	if ((NULL == text) || ('\0' == *text))
		return g_string_free(out, FALSE);

	ctx.options = options;
	lines = md_lines(text);
	md_blocks(&ctx, out, lines, 0, FALSE);

	return g_string_free(out, FALSE);
}

/* Inline tags join words; every other tag ends one. */
static gboolean
md_tag_is_inline(const gchar *name)
{
	static const gchar *const inline_tags[] = {
		"strong", "em", "del", "code", "a", "span", NULL
	};
	gsize i;

	for (i = 0; NULL != inline_tags[i]; i++)
	{
		if (0 == g_ascii_strcasecmp(name, inline_tags[i]))
			return TRUE;
	}

	return FALSE;
}

gchar *
venture_markdown_to_text(
	const gchar	*text,
	gsize		 max_chars
){
	g_autofree gchar *html = NULL;
	g_autoptr(GString) plain = NULL;
	g_autofree gchar *collapsed = NULL;
	const gchar *p;

	html = venture_markdown_to_html(text, NULL);
	plain = g_string_new(NULL);

	/* The renderer's own output is the input here, so its tags and its
	 * five entities are all there is to undo. */
	for (p = html; '\0' != *p; p++)
	{
		if ('<' == *p)
		{
			gchar name[16];
			gsize n = 0;
			const gchar *q = p + 1;

			if ('/' == *q)
				q++;

			while (g_ascii_isalnum(*q) && (n + 1 < sizeof(name)))
				name[n++] = *q++;

			name[n] = '\0';

			if (!md_tag_is_inline(name))
				g_string_append_c(plain, ' ');

			while (('\0' != *p) && ('>' != *p))
				p++;

			if ('\0' == *p)
				break;

			continue;
		}

		if ('&' == *p)
		{
			static const struct { const gchar *entity; gchar c; } entities[] = {
				{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
				{ "&quot;", '"' }, { "&#39;", '\'' }
			};
			gsize e;
			gboolean decoded = FALSE;

			for (e = 0; e < G_N_ELEMENTS(entities); e++)
			{
				gsize length = strlen(entities[e].entity);

				if (0 == strncmp(p, entities[e].entity, length))
				{
					g_string_append_c(plain, entities[e].c);
					p += length - 1;
					decoded = TRUE;
					break;
				}
			}

			if (decoded)
				continue;
		}

		g_string_append_c(plain, *p);
	}

	/* One line, single-spaced. */
	{
		GString *flat = g_string_new(NULL);
		gboolean space = FALSE;

		for (p = plain->str; '\0' != *p; p++)
		{
			if (g_ascii_isspace(*p))
			{
				space = TRUE;
				continue;
			}

			if (space && (flat->len > 0))
				g_string_append_c(flat, ' ');

			space = FALSE;
			g_string_append_c(flat, *p);
		}

		collapsed = g_string_free(flat, FALSE);
	}

	if (0 == max_chars)
		return g_steal_pointer(&collapsed);

	return venture_truncate(collapsed, max_chars);
}
