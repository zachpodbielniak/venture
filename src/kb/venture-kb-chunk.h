/*
 * venture-kb-chunk.h - Splitting a document into passages worth embedding
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A document is longer than an embedding model's context and, more to the
 * point, longer than the answer to a question. One vector per document finds
 * the right handbook and cannot say which paragraph; one per passage finds
 * the paragraph. So documents are split, and the split is where retrieval
 * quality is mostly decided.
 *
 * The split follows headings first, because a heading is the author's own
 * statement about where one idea ends. Only a section too long to embed is
 * broken further, and then at a paragraph boundary rather than mid-sentence.
 * Consecutive passages overlap, so the one sentence that answers the question
 * is not the one that fell across a boundary.
 */

#ifndef VENTURE_KB_CHUNK_H
#define VENTURE_KB_CHUNK_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureKbPassage:
 * @text: the passage, as it will be embedded
 * @heading: (nullable): the heading it sits under
 * @ordinal: its position in the document, from 0
 *
 * One piece of a document.
 *
 * @heading is carried separately rather than left in @text so a result can
 * say where in the document it came from without re-parsing the body -- and
 * so it can be prepended to the embedded text, which measurably helps a
 * passage that says "it must be set before the first run" without saying
 * what "it" is.
 */
typedef struct
{
	gchar	*text;
	gchar	*heading;
	gsize	 ordinal;
} VentureKbPassage;

/**
 * venture_kb_passage_free:
 * @self: (nullable): a passage
 *
 * Frees a passage.
 */
void
venture_kb_passage_free(VentureKbPassage *self);

/**
 * venture_kb_chunk_text:
 * @text: the document
 * @format: what it was written in, which decides how headings are found
 * @target_chars: the size to aim for, in characters
 * @overlap_chars: how much each passage repeats from the one before
 *
 * Splits a document into passages.
 *
 * Sizes are in characters rather than tokens because the tokeniser belongs
 * to the model and we do not have it. They are targets, not limits: a
 * section shorter than @target_chars is left whole rather than padded, and a
 * paragraph longer than it is not broken mid-word.
 *
 * Splitting is UTF-8 aware throughout. A byte-wise split would cut a
 * multi-byte character in half, and the resulting passage is not merely
 * wrong but invalid UTF-8, which fails at the JSON encoder much later.
 *
 * An empty or whitespace-only document produces an empty array rather than
 * one empty passage: embedding nothing costs a request and returns a zero
 * vector that matches everything equally badly.
 *
 * Returns: (transfer full) (element-type VentureKbPassage): the passages,
 *   in document order
 */
GPtrArray *
venture_kb_chunk_text(
	const gchar		*text,
	VentureKbFormat		 format,
	gsize			 target_chars,
	gsize			 overlap_chars
);

/**
 * venture_kb_chunk_embed_text:
 * @passage: a passage
 *
 * The text to embed for @passage, which is its heading and its body rather
 * than its body alone.
 *
 * A passage lifted out of the middle of a document loses the context its
 * heading gave it -- "set it before the first run" is unfindable without
 * "API tokens" above it. Prepending the heading costs a few tokens and
 * restores the subject.
 *
 * Returns: (transfer full): the text to embed
 */
gchar *
venture_kb_chunk_embed_text(const VentureKbPassage *passage);

/**
 * venture_kb_format_from_path:
 * @path: a filename or path
 *
 * Guesses a format from an extension.
 *
 * Returns %VENTURE_KB_FORMAT_OTHER for anything unrecognised rather than
 * assuming text: a binary read as text produces a body of mojibake that
 * embeds to a vector, and a vector nobody can tell is meaningless.
 *
 * Returns: the format
 */
VentureKbFormat
venture_kb_format_from_path(const gchar *path);

/**
 * venture_kb_format_extension:
 * @format: a format
 *
 * The file extension an article of this format is exported as.
 *
 * Returns: (transfer none): the extension, including the dot
 */
const gchar *
venture_kb_format_extension(VentureKbFormat format);

/**
 * venture_kb_slugify:
 * @text: any text
 *
 * Reduces text to a lowercase, hyphenated slug.
 *
 * Used for both knowledge-base slugs -- what somebody types after '#' -- and
 * article slugs. Non-ASCII is dropped rather than transliterated: a slug is
 * something a person types at a keyboard, and a transliteration nobody
 * expects is worse than a shorter slug.
 *
 * Returns: (transfer full) (nullable): the slug, or %NULL when nothing
 *   usable remained
 */
gchar *
venture_kb_slugify(const gchar *text);

G_END_DECLS

#endif /* VENTURE_KB_CHUNK_H */
