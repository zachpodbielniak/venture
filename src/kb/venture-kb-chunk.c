/*
 * venture-kb-chunk.c - Splitting a document into passages worth embedding
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

void
venture_kb_passage_free(VentureKbPassage *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->text, g_free);
	g_clear_pointer(&self->heading, g_free);
	g_free(self);
}

/*
 * Whether a line opens a section, and what its title is.
 *
 * Org and Markdown are the two formats worth parsing: both mark headings at
 * the start of a line with a repeated character, which is cheap to detect
 * and impossible to confuse with body text that happens to contain one.
 * Everything else is treated as having no headings at all rather than
 * guessed at -- a wrong heading is worse than none, because it is attached
 * to every passage beneath it.
 */
static gchar *
venture_kb_heading_of_line(
	const gchar	*line,
	VentureKbFormat	 format
){
	const gchar *p = line;
	gchar marker;
	guint depth = 0;

	if (VENTURE_KB_FORMAT_ORG == format)
		marker = '*';
	else if (VENTURE_KB_FORMAT_MARKDOWN == format)
		marker = '#';
	else
		return NULL;

	while (marker == *p)
	{
		depth++;
		p++;
	}

	/*
	 * A run of markers is only a heading when a space follows it. Without
	 * that check, Markdown's "#hashtag" and Org's "**bold**" both become
	 * headings, and in Org the bold case is common in body text.
	 */
	if ((0 == depth) || (' ' != *p))
		return NULL;

	while (' ' == *p)
		p++;

	if ('\0' == *p)
		return NULL;

	return g_strdup(p);
}

/*
 * Appends a passage, trimming it and skipping it when nothing is left.
 *
 * Whitespace-only passages are the common case at section boundaries, and
 * each one costs an embedding request and returns a vector that matches
 * every query equally badly.
 */
static void
venture_kb_chunk_append(
	GPtrArray	*out,
	const gchar	*text,
	const gchar	*heading
){
	g_autofree gchar *trimmed = NULL;
	VentureKbPassage *passage;

	if (NULL == text)
		return;

	trimmed = g_strdup(text);
	g_strstrip(trimmed);

	if (venture_string_is_empty(trimmed))
		return;

	passage = g_new0(VentureKbPassage, 1);
	passage->text = g_steal_pointer(&trimmed);
	passage->heading = g_strdup(heading);
	passage->ordinal = out->len;

	g_ptr_array_add(out, passage);
}

/*
 * Steps back from @from to a boundary that is safe to cut at.
 *
 * Prefers a blank line, then a sentence end, then a space; a cut mid-word is
 * the last resort rather than the default. Returns a pointer into @text that
 * always sits on a UTF-8 character boundary -- g_utf8_find_prev_char() is
 * what guarantees that, and a byte-wise scan is what would not.
 */
static const gchar *
venture_kb_break_before(
	const gchar	*text,
	const gchar	*from
){
	const gchar *best_paragraph = NULL;
	const gchar *best_sentence = NULL;
	const gchar *best_space = NULL;
	const gchar *p;

	for (p = text; (NULL != p) && (p < from); p = g_utf8_next_char(p))
	{
		gunichar c = g_utf8_get_char(p);

		if ('\n' == c)
		{
			const gchar *next = g_utf8_next_char(p);

			if ((NULL != next) && (next < from) &&
			    ('\n' == g_utf8_get_char(next)))
				best_paragraph = g_utf8_next_char(next);
		}
		else if (('.' == c) || ('!' == c) || ('?' == c))
		{
			const gchar *next = g_utf8_next_char(p);

			if ((NULL != next) && (next < from) &&
			    g_unichar_isspace(g_utf8_get_char(next)))
				best_sentence = g_utf8_next_char(next);
		}
		else if (g_unichar_isspace(c))
		{
			best_space = g_utf8_next_char(p);
		}
	}

	if (NULL != best_paragraph)
		return best_paragraph;

	if (NULL != best_sentence)
		return best_sentence;

	if (NULL != best_space)
		return best_space;

	return from;
}

/*
 * Splits one section into passages of roughly @target characters.
 *
 * A section shorter than the target is emitted whole, which is the common
 * case and the one worth keeping cheap.
 */
static void
venture_kb_chunk_section(
	GPtrArray	*out,
	const gchar	*text,
	const gchar	*heading,
	gsize		 target,
	gsize		 overlap
){
	const gchar *cursor = text;

	if (venture_string_is_empty(text))
		return;

	while ('\0' != *cursor)
	{
		const gchar *limit;
		const gchar *cut;
		g_autofree gchar *piece = NULL;
		glong remaining;

		remaining = g_utf8_strlen(cursor, -1);

		if ((gsize)remaining <= target)
		{
			venture_kb_chunk_append(out, cursor, heading);
			return;
		}

		limit = g_utf8_offset_to_pointer(cursor, (glong)target);
		cut = venture_kb_break_before(cursor, limit);

		/*
		 * A section with no break at all in the first @target
		 * characters -- a base64 blob, a minified file -- is cut at
		 * the limit. Refusing to cut would emit the whole thing as one
		 * passage and blow past the model's context.
		 */
		if (cut <= cursor)
			cut = limit;

		piece = g_strndup(cursor, (gsize)(cut - cursor));
		venture_kb_chunk_append(out, piece, heading);

		/*
		 * Step back by the overlap so a sentence spanning the cut
		 * appears whole in the next passage. Bounded below by one
		 * character of progress: an overlap larger than the piece just
		 * emitted would move the cursor backwards and never terminate.
		 */
		{
			glong consumed = g_utf8_strlen(cursor, cut - cursor);
			glong step = consumed - (glong)overlap;

			if (step < 1)
				step = consumed;

			if (step < 1)
				step = 1;

			cursor = g_utf8_offset_to_pointer(cursor, step);
		}
	}
}

GPtrArray *
venture_kb_chunk_text(
	const gchar		*text,
	VentureKbFormat		 format,
	gsize			 target_chars,
	gsize			 overlap_chars
){
	g_autoptr(GPtrArray) out = NULL;
	g_autoptr(GString) section = NULL;
	g_autofree gchar *heading = NULL;
	g_auto(GStrv) lines = NULL;
	gsize i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_kb_passage_free);

	if (venture_string_is_empty(text))
		return g_steal_pointer(&out);

	/* Guard rails rather than validation: a zero target would loop
	 * forever, and an overlap at or above the target never advances. */
	if (target_chars < 64)
		target_chars = 64;

	if (overlap_chars >= target_chars)
		overlap_chars = target_chars / 4;

	lines = g_strsplit(text, "\n", -1);
	section = g_string_new(NULL);

	for (i = 0; NULL != lines[i]; i++)
	{
		g_autofree gchar *found = NULL;

		found = venture_kb_heading_of_line(lines[i], format);

		if (NULL != found)
		{
			/* The section that just ended, under the heading it
			 * was written beneath. */
			venture_kb_chunk_section(out, section->str, heading,
			                         target_chars, overlap_chars);
			g_string_truncate(section, 0);

			g_clear_pointer(&heading, g_free);
			heading = g_steal_pointer(&found);
			continue;
		}

		g_string_append(section, lines[i]);
		g_string_append_c(section, '\n');
	}

	venture_kb_chunk_section(out, section->str, heading, target_chars,
	                         overlap_chars);

	/* Ordinals are assigned on append, and a skipped empty passage would
	 * otherwise leave a gap. Renumbered once, at the end. */
	for (i = 0; i < out->len; i++)
	{
		VentureKbPassage *passage = g_ptr_array_index(out, i);

		passage->ordinal = i;
	}

	return g_steal_pointer(&out);
}

gchar *
venture_kb_chunk_embed_text(const VentureKbPassage *passage)
{
	g_return_val_if_fail(NULL != passage, NULL);

	if (venture_string_is_empty(passage->heading))
		return g_strdup(passage->text);

	return g_strdup_printf("%s\n\n%s", passage->heading, passage->text);
}

VentureKbFormat
venture_kb_format_from_path(const gchar *path)
{
	g_autofree gchar *lower = NULL;

	if (venture_string_is_empty(path))
		return VENTURE_KB_FORMAT_OTHER;

	lower = g_ascii_strdown(path, -1);

	if (g_str_has_suffix(lower, ".org"))
		return VENTURE_KB_FORMAT_ORG;

	if (g_str_has_suffix(lower, ".md") ||
	    g_str_has_suffix(lower, ".markdown") ||
	    g_str_has_suffix(lower, ".mdown"))
		return VENTURE_KB_FORMAT_MARKDOWN;

	if (g_str_has_suffix(lower, ".txt") ||
	    g_str_has_suffix(lower, ".text") ||
	    g_str_has_suffix(lower, ".rst") ||
	    g_str_has_suffix(lower, ".adoc"))
		return VENTURE_KB_FORMAT_TEXT;

	if (g_str_has_suffix(lower, ".html") || g_str_has_suffix(lower, ".htm"))
		return VENTURE_KB_FORMAT_HTML;

	if (g_str_has_suffix(lower, ".pdf"))
		return VENTURE_KB_FORMAT_PDF;

	if (g_str_has_suffix(lower, ".docx"))
		return VENTURE_KB_FORMAT_DOCX;

	return VENTURE_KB_FORMAT_OTHER;
}

const gchar *
venture_kb_format_extension(VentureKbFormat format)
{
	switch (format)
	{
	case VENTURE_KB_FORMAT_ORG:
		return ".org";
	case VENTURE_KB_FORMAT_MARKDOWN:
		return ".md";
	case VENTURE_KB_FORMAT_HTML:
		return ".html";
	/*
	 * A PDF or a .docx exports as .txt, not as itself. What the article
	 * holds is the text that was extracted from the file; writing that
	 * out under a .pdf name would produce a file nothing can open.
	 */
	case VENTURE_KB_FORMAT_PDF:
	case VENTURE_KB_FORMAT_DOCX:
	case VENTURE_KB_FORMAT_TEXT:
	case VENTURE_KB_FORMAT_OTHER:
	default:
		return ".txt";
	}
}

gchar *
venture_kb_slugify(const gchar *text)
{
	g_autoptr(GString) out = NULL;
	const gchar *p;
	gboolean pending_hyphen = FALSE;

	if (venture_string_is_empty(text))
		return NULL;

	out = g_string_new(NULL);

	for (p = text; (NULL != p) && ('\0' != *p); p = g_utf8_next_char(p))
	{
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_isalnum(c) && (c < 128))
		{
			/*
			 * A hyphen is only emitted once something follows it,
			 * so a slug never starts or ends with one and never
			 * doubles them.
			 */
			if (pending_hyphen && (out->len > 0))
				g_string_append_c(out, '-');

			pending_hyphen = FALSE;
			g_string_append_c(out, (gchar)g_unichar_tolower(c));
		}
		else
		{
			pending_hyphen = TRUE;
		}
	}

	if (0 == out->len)
		return NULL;

	return g_strdup(out->str);
}
