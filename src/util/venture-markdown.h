/*
 * venture-markdown.h - The markdown a comment is written in
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A small, strict renderer for what people actually type into a comment
 * box: paragraphs, emphasis, code, fenced blocks, lists that nest, quotes,
 * headings, rules, simple tables, links to web addresses and this site's
 * own pages, @mentions and #type/id references to records.
 *
 * No HTML the author typed ever reaches the page. Every character of the
 * source leaves through the HTML escaper, and the only markup in the output
 * is the fixed set of tags this file writes itself; a link is written only
 * when its target is an http(s) address or a path on this site, so a
 * javascript: URL is text. That property is what the tests in
 * tests/test-markdown.c pin, case by case.
 */

#ifndef VENTURE_MARKDOWN_H
#define VENTURE_MARKDOWN_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_MARKDOWN_MAX_DEPTH:
 *
 * How deeply quotes and lists may nest, and emphasis inside emphasis,
 * before the rest is rendered as plain text. A bound, because the input is
 * a stranger's and recursion is a stack.
 */
#define VENTURE_MARKDOWN_MAX_DEPTH (6)

/**
 * VentureMarkdownMentionFunc:
 * @username: the name written after the @
 * @user_data: (closure): the data given in #VentureMarkdownOptions
 *
 * Decides whether "@name" is a mention. Returning %NULL leaves it as the
 * text it was -- which is what a name nobody has, or somebody who may not
 * read the record, must look like.
 *
 * Returns: (transfer full) (nullable): what to call the person in the
 *   chip's title, or %NULL when it is not a mention
 */
typedef gchar *(*VentureMarkdownMentionFunc) (
	const gchar	*username,
	gpointer	 user_data
);

/**
 * VentureMarkdownReferenceFunc:
 * @type_name: the record type written after the #
 * @id: the record's id
 * @user_data: (closure): the data given in #VentureMarkdownOptions
 *
 * Decides whether "#invoice/12" names a record the reader may open.
 *
 * Returns: (transfer full) (nullable): the record's name for the link, or
 *   %NULL to leave the text as it was
 */
typedef gchar *(*VentureMarkdownReferenceFunc) (
	const gchar	*type_name,
	gint64		 id,
	gpointer	 user_data
);

/**
 * VentureMarkdownOptions:
 * @mention: (nullable): resolves @names; %NULL renders none as mentions
 * @reference: (nullable): resolves #type/id; %NULL renders none as links
 * @user_data: passed to both
 *
 * What the renderer may ask the caller about. Both are optional, so the
 * same function renders an excerpt (neither) and a page (both).
 */
typedef struct
{
	VentureMarkdownMentionFunc	 mention;
	VentureMarkdownReferenceFunc	 reference;
	gpointer			 user_data;
} VentureMarkdownOptions;

/**
 * venture_markdown_to_html:
 * @text: (nullable): the markdown
 * @options: (nullable): how to resolve mentions and references
 *
 * Renders markdown as HTML that is safe to put inside an element of a
 * page. Invalid UTF-8 is repaired first, and control characters other than
 * a newline or a tab are dropped.
 *
 * Returns: (transfer full): the HTML; empty for empty input
 */
gchar *
venture_markdown_to_html(
	const gchar			*text,
	const VentureMarkdownOptions	*options
);

/**
 * venture_markdown_to_text:
 * @text: (nullable): the markdown
 * @max_chars: at most this many characters, an ellipsis included; 0 for
 *   no limit
 *
 * The words of a piece of markdown with its syntax taken away, on one
 * line: what an inbox line, an activity entry or a webhook excerpt shows.
 *
 * Returns: (transfer full): the text
 */
gchar *
venture_markdown_to_text(
	const gchar	*text,
	gsize		 max_chars
);

G_END_DECLS

#endif /* VENTURE_MARKDOWN_H */
