/*
 * venture-string-util.h - String helpers used across VENTURE
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The escaping functions here are load bearing. Record names, product
 * titles, contact notes and AI-generated text all end up rendered into HTML
 * and written into CSV, and every one of those strings is attacker-influenced
 * in the sense that matters: it came from outside and nobody checked it.
 * Escaping is therefore never optional and never done by hand elsewhere in
 * the tree.
 */

#ifndef VENTURE_STRING_UTIL_H
#define VENTURE_STRING_UTIL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * venture_string_is_empty:
 * @text: (nullable): the string to test
 *
 * Returns: %TRUE if @text is %NULL or contains only whitespace
 */
gboolean
venture_string_is_empty(const gchar *text);

/**
 * venture_slugify:
 * @text: (nullable): the text to convert
 *
 * Produces a URL- and filename-safe slug: lowercase ASCII, words joined by
 * hyphens, accents folded, everything else dropped. An empty or unusable
 * input yields "untitled" rather than an empty string, because a slug is
 * used as an identifier and an empty identifier breaks callers.
 *
 * Returns: (transfer full): the slug
 */
gchar *
venture_slugify(const gchar *text);

/**
 * venture_html_escape:
 * @text: (nullable): the text to escape
 *
 * Escapes text for insertion into HTML element content or a double-quoted
 * attribute value. Escapes `&`, `<`, `>`, `"` and `'`.
 *
 * Every piece of dynamic text rendered by the web layer goes through this.
 *
 * Returns: (transfer full): the escaped text; never %NULL
 */
gchar *
venture_html_escape(const gchar *text);

/**
 * venture_html_escape_append:
 * @string: the #GString to append to
 * @text: (nullable): the text to escape and append
 *
 * As venture_html_escape(), but appends directly, which is what the page
 * builders want and avoids an allocation per field.
 */
void
venture_html_escape_append(
	GString		*string,
	const gchar	*text
);

/**
 * venture_attribute_escape:
 * @text: (nullable): the text to escape
 *
 * Escapes text for use inside a single-quoted HTML attribute, which is what
 * hx-vals and hx-headers need since their contents are JSON containing
 * double quotes.
 *
 * Returns: (transfer full): the escaped text; never %NULL
 */
gchar *
venture_attribute_escape(const gchar *text);

/**
 * venture_csv_escape:
 * @text: (nullable): the field to escape
 *
 * Quotes and escapes a CSV field per RFC 4180 when it contains a comma, a
 * quote, a newline or leading whitespace.
 *
 * A field beginning with `=`, `+`, `-` or `@` is additionally prefixed with
 * a single quote. Spreadsheet software treats such a field as a formula, and
 * a contact note beginning with "=" would otherwise execute when the export
 * is opened. This is a real attack, not a hypothetical one.
 *
 * Returns: (transfer full): the escaped field; never %NULL
 */
gchar *
venture_csv_escape(const gchar *text);

/**
 * venture_truncate:
 * @text: (nullable): the text to shorten
 * @max_chars: the maximum number of characters to keep
 *
 * Truncates on a character boundary, appending an ellipsis when anything was
 * removed. Counts characters rather than bytes, so it never splits a UTF-8
 * sequence.
 *
 * Returns: (transfer full) (nullable): the shortened text
 */
gchar *
venture_truncate(
	const gchar	*text,
	gsize		 max_chars
);

/**
 * venture_generate_token:
 * @n_bytes: how many random bytes to draw; 32 is the usual choice
 *
 * Generates a random token as lowercase hex, drawn from the system's
 * cryptographic random source. Used for API tokens, session identifiers and
 * confirmation identifiers.
 *
 * Returns: (transfer full): the token
 */
gchar *
venture_generate_token(gsize n_bytes);

/**
 * venture_hash_password:
 * @password: the password to hash
 * @iterations: the PBKDF2 iteration count
 * @error: (out) (optional): return location for a #GError
 *
 * Hashes a password with PBKDF2-HMAC-SHA256 and a fresh random salt. The
 * result is a self-describing string of the form
 * `pbkdf2-sha256$<iterations>$<salt-hex>$<hash-hex>`, so raising the
 * iteration count later does not invalidate existing hashes.
 *
 * Returns: (transfer full) (nullable): the encoded hash, or %NULL on error
 */
gchar *
venture_hash_password(
	const gchar	 *password,
	guint		  iterations,
	GError		**error
);

/**
 * venture_verify_password:
 * @password: the password to check
 * @encoded: the stored hash produced by venture_hash_password()
 *
 * Verifies a password against a stored hash, comparing in constant time so
 * the comparison itself does not leak how much of the hash matched.
 *
 * Returns: %TRUE if @password is correct
 */
gboolean
venture_verify_password(
	const gchar	*password,
	const gchar	*encoded
);

/**
 * venture_constant_time_equal:
 * @a: (nullable): the first string
 * @b: (nullable): the second string
 *
 * Compares two strings in time that does not depend on where they first
 * differ. Used for token and hash comparison.
 *
 * Returns: %TRUE if the strings are equal
 */
gboolean
venture_constant_time_equal(
	const gchar	*a,
	const gchar	*b
);

/**
 * venture_pluralise:
 * @word: the singular noun
 *
 * Produces the plural form used for table names and UI headings. Handles the
 * regular cases plus the -y, -s, -x, -ch and -sh endings that occur in this
 * schema ("category" to "categories", "expense" to "expenses").
 *
 * Returns: (transfer full): the plural
 */
gchar *
venture_pluralise(const gchar *word);

/**
 * venture_string_redact_uri:
 * @uri: (nullable): a connection or service URI
 *
 * Replaces the password in @uri's userinfo with a mask, leaving everything
 * else intact.
 *
 * Anywhere a URI reaches a human -- a log line, an error message, a settings
 * page, a pasted bug report -- it must not carry a credential. A connection
 * failure is exactly when the URI gets printed and exactly when someone
 * copies it to ask for help.
 *
 * Returns: (transfer full) (nullable): the redacted URI, or %NULL if @uri
 *   was %NULL
 */
gchar *
venture_string_redact_uri(const gchar *uri);

/**
 * venture_string_list_contains_ci:
 * @haystack: (array zero-terminated=1) (nullable): the strings to search
 * @needle: the string to find
 *
 * Returns: %TRUE if @needle appears in @haystack, compared case insensitively
 */
gboolean
venture_string_list_contains_ci(
	const gchar * const	*haystack,
	const gchar		*needle
);

G_END_DECLS

#endif /* VENTURE_STRING_UTIL_H */
