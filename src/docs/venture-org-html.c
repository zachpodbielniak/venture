/*
 * venture-org-html.c - A small org-mode to HTML renderer
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * Line-oriented: each line is classified once, and the classification
 * decides what open construct it closes. There is no lookahead beyond
 * "was the previous line blank", which is all Org itself needs for the
 * subset the documentation uses.
 */

#include "venture-org-html.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

/**
 * venture_org_html_escape_append:
 * @out: the buffer to append to
 * @text: text to escape
 *
 * Appends @text to @out with the five characters HTML treats as
 * structure replaced by their entities.
 */
void
venture_org_html_escape_append(
	GString		*out,
	const gchar	*text
){
	const gchar *p;

	g_return_if_fail(NULL != out);

	if (NULL == text)
		return;

	for (p = text; '\0' != *p; p++)
	{
		switch (*p)
		{
		case '&':  g_string_append(out, "&amp;");  break;
		case '<':  g_string_append(out, "&lt;");   break;
		case '>':  g_string_append(out, "&gt;");   break;
		case '"':  g_string_append(out, "&quot;"); break;
		case '\'': g_string_append(out, "&#39;");  break;
		default:   g_string_append_c(out, *p);     break;
		}
	}
}

/**
 * venture_org_heading_slug:
 * @heading: a heading's text, org markup and all
 *
 * The anchor a heading is given: lower-case ASCII letters and digits,
 * every other run of characters collapsed to one hyphen, so
 * "Linking any record to any other" becomes
 * "linking-any-record-to-any-other". Org markup characters count as
 * separators, which is what makes the slug of "=venturectl= docs" and of
 * "venturectl docs" the same.
 *
 * Returns: (transfer full): the slug
 */
gchar *
venture_org_heading_slug(const gchar *heading)
{
	GString *slug = g_string_new(NULL);
	const gchar *p;
	gboolean pending_dash = FALSE;

	if (NULL == heading)
		return g_string_free(slug, FALSE);

	for (p = heading; '\0' != *p; p++)
	{
		if (g_ascii_isalnum(*p))
		{
			if (pending_dash && slug->len > 0)
				g_string_append_c(slug, '-');

			pending_dash = FALSE;
			g_string_append_c(slug, g_ascii_tolower(*p));
		}
		else
			pending_dash = TRUE;
	}

	return g_string_free(slug, FALSE);
}

/**
 * venture_org_get_keyword:
 * @org: an org document
 * @keyword: a keyword name such as "title", case-insensitively
 *
 * The value of the first `#+keyword:` line in @org, trimmed.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL if absent
 */
gchar *
venture_org_get_keyword(
	const gchar	*org,
	const gchar	*keyword
){
	const gchar *line;
	gsize keyword_length;

	g_return_val_if_fail(NULL != keyword, NULL);

	if (NULL == org)
		return NULL;

	keyword_length = strlen(keyword);
	line = org;

	while ('\0' != *line)
	{
		const gchar *end = strchr(line, '\n');
		gsize length = (NULL != end) ? (gsize)(end - line) : strlen(line);

		if (length > keyword_length + 3 &&
		    0 == strncmp(line, "#+", 2) &&
		    0 == g_ascii_strncasecmp(line + 2, keyword, keyword_length) &&
		    ':' == line[2 + keyword_length])
		{
			g_autofree gchar *value = g_strndup(line + 3 + keyword_length,
			                                    length - 3 - keyword_length);

			return g_strdup(g_strstrip(value));
		}

		if (NULL == end)
			break;

		line = end + 1;
	}

	return NULL;
}

/**
 * venture_org_html_to_text:
 * @html: rendered HTML
 *
 * The words in @html: tags removed, the common entities decoded and
 * whitespace collapsed to single spaces. This is what the search index
 * carries for a page.
 *
 * Returns: (transfer full): the text
 */
gchar *
venture_org_html_to_text(const gchar *html)
{
	GString *text = g_string_new(NULL);
	const gchar *p;
	gboolean in_tag = FALSE;
	gboolean space_pending = FALSE;

	if (NULL == html)
		return g_string_free(text, FALSE);

	for (p = html; '\0' != *p; p++)
	{
		if (in_tag)
		{
			if ('>' == *p)
			{
				in_tag = FALSE;
				space_pending = TRUE;
			}

			continue;
		}

		if ('<' == *p)
		{
			in_tag = TRUE;
			continue;
		}

		if (g_ascii_isspace(*p))
		{
			space_pending = TRUE;
			continue;
		}

		if (space_pending && text->len > 0)
			g_string_append_c(text, ' ');

		space_pending = FALSE;

		if ('&' == *p)
		{
			static const struct { const gchar *entity; gchar plain; } entities[] = {
				{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
				{ "&quot;", '"' }, { "&#39;", '\'' }, { "&nbsp;", ' ' }
			};
			gsize i;

			for (i = 0; i < G_N_ELEMENTS(entities); i++)
			{
				gsize length = strlen(entities[i].entity);

				if (0 == strncmp(p, entities[i].entity, length))
				{
					g_string_append_c(text, entities[i].plain);
					p += length - 1;
					break;
				}
			}

			if (i < G_N_ELEMENTS(entities))
				continue;
		}

		g_string_append_c(text, *p);
	}

	return g_string_free(text, FALSE);
}

/* ------------------------------------------------------------------------
 * Inline markup
 * ---------------------------------------------------------------------- */

/* Org's rule: emphasis may start after one of these, or at a line start. */
static gboolean
is_pre_char(gchar c)
{
	return '\0' == c || g_ascii_isspace(c) || NULL != strchr("-(\"'{", c);
}

/* ... and must end before one of these, or at the line end. */
static gboolean
is_post_char(gchar c)
{
	return '\0' == c || g_ascii_isspace(c) || NULL != strchr("-.,:!?;'\")}[]", c);
}

/*
 * Where an inline span opened by @marker at @start closes, or NULL.
 * @start points at the opening marker. The span may not begin or end
 * with whitespace, may not be empty, and does not cross a line.
 */
static const gchar *
find_span_end(
	const gchar	*start,
	gchar		 marker
){
	const gchar *p;

	if (g_ascii_isspace(start[1]) || '\0' == start[1] || marker == start[1])
		return NULL;

	for (p = start + 1; '\0' != *p && '\n' != *p; p++)
	{
		if (marker == *p && !g_ascii_isspace(p[-1]) && is_post_char(p[1]))
			return p;
	}

	return NULL;
}

static void
append_inline(
	GString		*out,
	const gchar	*text,
	gsize		 length
);

static void
append_link(
	GString		*out,
	const gchar	*target,
	gsize		 target_length,
	const gchar	*description,
	gsize		 description_length
){
	g_autofree gchar *href = g_strndup(target, target_length);
	const gchar *shown = href;

	if (g_str_has_prefix(href, "file:"))
		shown = href + strlen("file:");

	g_string_append(out, "<a href=\"");
	venture_org_html_escape_append(out, shown);
	g_string_append(out, "\">");

	if (NULL != description)
		append_inline(out, description, description_length);
	else
		venture_org_html_escape_append(out, shown);

	g_string_append(out, "</a>");
}

/*
 * Renders @length bytes of @text with org's inline markup: links,
 * =verbatim=, ~code~, *bold* and /italic/. Everything else is escaped.
 */
static void
append_inline(
	GString		*out,
	const gchar	*text,
	gsize		 length
){
	const gchar *end = text + length;
	const gchar *p = text;

	while (p < end)
	{
		gchar c = *p;
		gchar previous = (p > text) ? p[-1] : '\0';

		if ('[' == c && p + 1 < end && '[' == p[1])
		{
			const gchar *close = g_strstr_len(p + 2, end - (p + 2), "]]");

			if (NULL != close)
			{
				const gchar *split = g_strstr_len(p + 2, close - (p + 2), "][");

				if (NULL != split)
					append_link(out, p + 2, split - (p + 2),
					            split + 2, close - (split + 2));
				else
					append_link(out, p + 2, close - (p + 2), NULL, 0);

				p = close + 2;
				continue;
			}
		}

		if (('=' == c || '~' == c) && is_pre_char(previous))
		{
			const gchar *close = find_span_end(p, c);

			if (NULL != close && close < end)
			{
				g_autofree gchar *code = g_strndup(p + 1, close - (p + 1));

				g_string_append(out, "<code>");
				venture_org_html_escape_append(out, code);
				g_string_append(out, "</code>");
				p = close + 1;
				continue;
			}
		}

		if (('*' == c || '/' == c) && is_pre_char(previous))
		{
			const gchar *close = find_span_end(p, c);

			if (NULL != close && close < end)
			{
				const gchar *tag = ('*' == c) ? "b" : "i";

				g_string_append_printf(out, "<%s>", tag);
				append_inline(out, p + 1, close - (p + 1));
				g_string_append_printf(out, "</%s>", tag);
				p = close + 1;
				continue;
			}
		}

		switch (c)
		{
		case '&':  g_string_append(out, "&amp;");  break;
		case '<':  g_string_append(out, "&lt;");   break;
		case '>':  g_string_append(out, "&gt;");   break;
		default:   g_string_append_c(out, c);      break;
		}

		p++;
	}
}

/* ------------------------------------------------------------------------
 * Blocks
 * ---------------------------------------------------------------------- */

typedef enum
{
	BLOCK_NONE,
	BLOCK_SRC,
	BLOCK_EXAMPLE,
	BLOCK_QUOTE
} BlockKind;

typedef struct
{
	gint		indent;
	gboolean	ordered;
} ListLevel;

typedef struct
{
	GString		*out;
	GString		*paragraph;	/* lines of the paragraph being gathered */
	GArray		*lists;		/* of ListLevel, innermost last */
	GPtrArray	*table;		/* of gchar * rows, NULL for a rule */
	BlockKind	 block;
	GString		*block_text;	/* verbatim lines of a src/example block */
	gchar		*block_language;
	gboolean	 blank_seen;	/* a blank line since the last content */
} Renderer;

static void
flush_paragraph(Renderer *r)
{
	if (0 == r->paragraph->len)
		return;

	g_string_append(r->out, "<p>");
	append_inline(r->out, r->paragraph->str, r->paragraph->len);
	g_string_append(r->out, "</p>\n");
	g_string_truncate(r->paragraph, 0);
}

static void
close_lists(
	Renderer	*r,
	gint		 to_indent
){
	flush_paragraph(r);

	while (r->lists->len > 0)
	{
		ListLevel *top = &g_array_index(r->lists, ListLevel, r->lists->len - 1);

		if (top->indent < to_indent)
			break;

		g_string_append(r->out, top->ordered ? "</li>\n</ol>\n" : "</li>\n</ul>\n");
		g_array_set_size(r->lists, r->lists->len - 1);
	}
}

/*
 * Splits a table row into its cells, trimmed. A row is "| a | b |";
 * the outer pipes are optional in org and dropped here.
 */
static gchar **
split_row(const gchar *line)
{
	g_autofree gchar *inner = g_strdup(line);
	gchar **cells;
	gchar *p = g_strstrip(inner);
	gsize length;
	guint i;

	if ('|' == *p)
		p++;

	length = strlen(p);

	if (length > 0 && '|' == p[length - 1])
		p[length - 1] = '\0';

	cells = g_strsplit(p, "|", -1);

	for (i = 0; NULL != cells[i]; i++)
		g_strstrip(cells[i]);

	return cells;
}

static void
flush_table(Renderer *r)
{
	guint first_rule = r->table->len;
	guint i;
	gboolean in_head;

	if (0 == r->table->len)
		return;

	for (i = 0; i < r->table->len; i++)
	{
		if (NULL == g_ptr_array_index(r->table, i))
		{
			first_rule = i;
			break;
		}
	}

	/* Rows above the first rule are the header, as in Org. */
	in_head = (first_rule > 0 && first_rule < r->table->len);

	g_string_append(r->out, "<table>\n");

	if (in_head)
		g_string_append(r->out, "<thead>\n");
	else
		g_string_append(r->out, "<tbody>\n");

	for (i = 0; i < r->table->len; i++)
	{
		const gchar *row = g_ptr_array_index(r->table, i);
		g_auto(GStrv) cells = NULL;
		guint c;

		if (NULL == row)
		{
			if (in_head)
			{
				g_string_append(r->out, "</thead>\n<tbody>\n");
				in_head = FALSE;
			}

			continue;
		}

		cells = split_row(row);
		g_string_append(r->out, "<tr>");

		for (c = 0; NULL != cells[c]; c++)
		{
			g_string_append(r->out, in_head ? "<th scope=\"col\">" : "<td>");
			append_inline(r->out, cells[c], strlen(cells[c]));
			g_string_append(r->out, in_head ? "</th>" : "</td>");
		}

		g_string_append(r->out, "</tr>\n");
	}

	if (in_head)
		g_string_append(r->out, "</thead>\n");
	else
		g_string_append(r->out, "</tbody>\n");

	g_string_append(r->out, "</table>\n");
	g_ptr_array_set_size(r->table, 0);
}

/* Everything open closes: paragraph, lists, table. Blocks close themselves. */
static void
close_all(Renderer *r)
{
	flush_paragraph(r);
	close_lists(r, -1);
	flush_table(r);
}

/*
 * A src or example block, with the common indentation of its lines
 * removed the way Org does, so an indented block reads as written.
 */
static void
flush_block(Renderer *r)
{
	g_auto(GStrv) lines = g_strsplit(r->block_text->str, "\n", -1);
	gsize common = G_MAXSIZE;
	guint i;

	for (i = 0; NULL != lines[i]; i++)
	{
		gsize indent = strspn(lines[i], " \t");

		if ('\0' == lines[i][indent])
			continue;

		if (indent < common)
			common = indent;
	}

	if (G_MAXSIZE == common)
		common = 0;

	if (BLOCK_SRC == r->block)
	{
		g_string_append(r->out, "<div class=\"org-src-container\">\n<pre class=\"src src-");
		venture_org_html_escape_append(r->out,
			(NULL != r->block_language) ? r->block_language : "text");
		g_string_append(r->out, "\">");
	}
	else
		g_string_append(r->out, "<pre class=\"example\">");

	for (i = 0; NULL != lines[i]; i++)
	{
		/* g_strsplit leaves one empty trailing element for the final
		 * newline; it is not a line of the block. */
		if (NULL == lines[i + 1] && '\0' == lines[i][0])
			break;

		venture_org_html_escape_append(r->out,
			lines[i] + MIN(common, strlen(lines[i])));
		g_string_append_c(r->out, '\n');
	}

	g_string_append(r->out, "</pre>\n");

	if (BLOCK_SRC == r->block)
		g_string_append(r->out, "</div>\n");

	g_string_truncate(r->block_text, 0);
	g_clear_pointer(&r->block_language, g_free);
}

/*
 * Whether @line opens a list item; if so, where its text starts and
 * whether it is numbered.
 */
static gboolean
parse_list_item(
	const gchar	 *line,
	gint		 *indent,
	gboolean	 *ordered,
	const gchar	**text
){
	const gchar *p = line + strspn(line, " ");

	*indent = (gint)(p - line);

	if (('-' == *p || '+' == *p) && ' ' == p[1])
	{
		*ordered = FALSE;
		*text = p + 2;
		return TRUE;
	}

	if (g_ascii_isdigit(*p))
	{
		const gchar *q = p;

		while (g_ascii_isdigit(*q))
			q++;

		if (('.' == *q || ')' == *q) && ' ' == q[1])
		{
			*ordered = TRUE;
			*text = q + 2;
			return TRUE;
		}
	}

	return FALSE;
}

static void
open_list_item(
	Renderer	*r,
	gint		 indent,
	gboolean	 ordered,
	const gchar	*text
){
	ListLevel *top;

	flush_paragraph(r);
	flush_table(r);

	/* Deeper than the innermost list: nest. Shallower: unwind. At the
	 * same depth the item joins the open list whatever its bullet, as
	 * Org reads it: only two blank lines end a list. */
	close_lists(r, indent + 1);
	top = (r->lists->len > 0)
		? &g_array_index(r->lists, ListLevel, r->lists->len - 1)
		: NULL;

	if (NULL == top || top->indent < indent)
	{
		ListLevel level;

		level.indent = indent;
		level.ordered = ordered;
		g_array_append_val(r->lists, level);
		g_string_append(r->out, ordered ? "<ol>\n<li>" : "<ul>\n<li>");
	}
	else
		g_string_append(r->out, "</li>\n<li>");

	g_string_append(r->paragraph, text);
}

static void
render_line(
	Renderer	*r,
	const gchar	*line
){
	const gchar *trimmed = line + strspn(line, " \t");

	/* Inside a literal block nothing is interpreted but its end. */
	if (BLOCK_SRC == r->block || BLOCK_EXAMPLE == r->block)
	{
		if (0 == g_ascii_strncasecmp(trimmed, "#+end_", 6))
		{
			flush_block(r);
			r->block = BLOCK_NONE;
		}
		else
		{
			g_string_append(r->block_text, line);
			g_string_append_c(r->block_text, '\n');
		}

		return;
	}

	if (BLOCK_QUOTE == r->block && 0 == g_ascii_strncasecmp(trimmed, "#+end_quote", 11))
	{
		close_all(r);
		g_string_append(r->out, "</blockquote>\n");
		r->block = BLOCK_NONE;
		return;
	}

	if ('\0' == *trimmed)
	{
		flush_paragraph(r);
		flush_table(r);

		/* Two blank lines end every open list, as in Org; one only
		 * separates paragraphs within an item. */
		if (r->blank_seen)
			close_lists(r, -1);

		r->blank_seen = TRUE;
		return;
	}

	if ('#' == trimmed[0] && '+' == trimmed[1])
	{
		if (0 == g_ascii_strncasecmp(trimmed, "#+begin_src", 11))
		{
			const gchar *language = trimmed + 11;

			close_all(r);
			language += strspn(language, " \t");
			r->block = BLOCK_SRC;
			r->block_language = g_strndup(language, strcspn(language, " \t"));

			if ('\0' == r->block_language[0])
				g_clear_pointer(&r->block_language, g_free);
		}
		else if (0 == g_ascii_strncasecmp(trimmed, "#+begin_example", 15))
		{
			close_all(r);
			r->block = BLOCK_EXAMPLE;
		}
		else if (0 == g_ascii_strncasecmp(trimmed, "#+begin_quote", 13))
		{
			close_all(r);
			g_string_append(r->out, "<blockquote>\n");
			r->block = BLOCK_QUOTE;
		}

		/* Every other keyword -- title, description, name -- is
		 * metadata, read elsewhere or not at all. */
		return;
	}

	/* A comment line. */
	if ('#' == trimmed[0] && (' ' == trimmed[1] || '\0' == trimmed[1]))
		return;

	if ('*' == line[0])
	{
		const gchar *stars = line;
		gint level = 0;
		g_autofree gchar *slug = NULL;
		const gchar *text;

		while ('*' == *stars)
		{
			level++;
			stars++;
		}

		if (' ' == *stars)
		{
			text = stars + 1;
			close_all(r);
			slug = venture_org_heading_slug(text);
			/* The document title is the h1; the first org level is h2. */
			level = MIN(level + 1, 6);
			g_string_append_printf(r->out, "<h%d id=\"", level);
			venture_org_html_escape_append(r->out, slug);
			g_string_append(r->out, "\">");
			append_inline(r->out, text, strlen(text));
			g_string_append_printf(r->out, "</h%d>\n", level);
			r->blank_seen = FALSE;
			return;
		}
	}

	if (0 == strncmp(trimmed, "-----", 5) && '\0' == trimmed[strspn(trimmed, "-")])
	{
		close_all(r);
		g_string_append(r->out, "<hr>\n");
		return;
	}

	if ('|' == trimmed[0])
	{
		flush_paragraph(r);
		close_lists(r, -1);

		if ('-' == trimmed[1] || '+' == trimmed[1])
			g_ptr_array_add(r->table, NULL);
		else
			g_ptr_array_add(r->table, g_strdup(trimmed));

		r->blank_seen = FALSE;
		return;
	}

	{
		gint indent;
		gboolean ordered;
		const gchar *text;

		if (parse_list_item(line, &indent, &ordered, &text))
		{
			open_list_item(r, indent, ordered, text);
			r->blank_seen = FALSE;
			return;
		}
	}

	/*
	 * Plain text. Inside a list, an indented line continues the item;
	 * an unindented one after a blank line ends the list. Otherwise it
	 * joins the paragraph being gathered.
	 */
	flush_table(r);

	if (r->lists->len > 0)
	{
		ListLevel *top = &g_array_index(r->lists, ListLevel, r->lists->len - 1);
		gint indent = (gint)(trimmed - line);

		if (indent > top->indent)
		{
			if (r->blank_seen)
			{
				flush_paragraph(r);
				g_string_append(r->out, "<p>");
				append_inline(r->out, trimmed, strlen(trimmed));
				g_string_append(r->out, "</p>\n");
			}
			else
			{
				g_string_append_c(r->paragraph, '\n');
				g_string_append(r->paragraph, trimmed);
			}

			r->blank_seen = FALSE;
			return;
		}

		close_lists(r, -1);
	}

	if (r->paragraph->len > 0)
		g_string_append_c(r->paragraph, '\n');

	g_string_append(r->paragraph, trimmed);
	r->blank_seen = FALSE;
}

/**
 * venture_org_to_html:
 * @org: an org document
 *
 * Renders the body of @org -- everything but its keywords -- to HTML.
 * The document title (`#+title:`) is not part of the body; the page
 * that wraps this decides where it goes.
 *
 * Returns: (transfer full): the HTML body
 */
gchar *
venture_org_to_html(const gchar *org)
{
	Renderer r;
	g_auto(GStrv) lines = NULL;
	guint i;

	g_return_val_if_fail(NULL != org, NULL);

	r.out = g_string_new(NULL);
	r.paragraph = g_string_new(NULL);
	r.lists = g_array_new(FALSE, FALSE, sizeof(ListLevel));
	r.table = g_ptr_array_new_with_free_func(g_free);
	r.block = BLOCK_NONE;
	r.block_text = g_string_new(NULL);
	r.block_language = NULL;
	r.blank_seen = FALSE;

	lines = g_strsplit(org, "\n", -1);

	for (i = 0; NULL != lines[i]; i++)
	{
		g_autofree gchar *line = g_strdup(lines[i]);
		gsize length = strlen(line);

		if (length > 0 && '\r' == line[length - 1])
			line[length - 1] = '\0';

		render_line(&r, line);
	}

	/* An unterminated block still renders what it had. */
	if (BLOCK_SRC == r.block || BLOCK_EXAMPLE == r.block)
		flush_block(&r);

	close_all(&r);

	if (BLOCK_QUOTE == r.block)
		g_string_append(r.out, "</blockquote>\n");

	g_string_free(r.paragraph, TRUE);
	g_array_unref(r.lists);
	g_ptr_array_unref(r.table);
	g_string_free(r.block_text, TRUE);
	g_free(r.block_language);

	return g_string_free(r.out, FALSE);
}
