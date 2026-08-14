/*
 * venture-string-util.c - String helpers used across VENTURE
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

gboolean
venture_string_is_empty(const gchar *text)
{
	const gchar *cursor;

	if (NULL == text)
		return TRUE;

	for (cursor = text; '\0' != *cursor; cursor++)
	{
		if (!g_ascii_isspace(*cursor))
			return FALSE;
	}

	return TRUE;
}

gchar *
venture_slugify(const gchar *text)
{
	g_autofree gchar *normalised = NULL;
	g_autofree gchar *ascii = NULL;
	g_autoptr(GString) slug = NULL;
	gboolean pending_separator;
	const gchar *cursor;

	if (venture_string_is_empty(text))
		return g_strdup("untitled");

	/* Decompose so that accents become separate combining marks, then let
	 * the ASCII transliteration drop them. "Café" becomes "cafe" rather
	 * than "caf". */
	normalised = g_utf8_normalize(text, -1, G_NORMALIZE_ALL);
	ascii = g_str_to_ascii((NULL != normalised) ? normalised : text, NULL);

	slug = g_string_new(NULL);
	pending_separator = FALSE;

	for (cursor = ascii; '\0' != *cursor; cursor++)
	{
		if (g_ascii_isalnum(*cursor))
		{
			/* Separators are emitted lazily so a run of punctuation
			 * collapses to one hyphen and none is left trailing. */
			if (pending_separator && (slug->len > 0))
				g_string_append_c(slug, '-');

			pending_separator = FALSE;
			g_string_append_c(slug, g_ascii_tolower(*cursor));
			continue;
		}

		pending_separator = TRUE;
	}

	if (0 == slug->len)
		return g_strdup("untitled");

	return g_string_free(g_steal_pointer(&slug), FALSE);
}

void
venture_html_escape_append(
	GString		*string,
	const gchar	*text
){
	const gchar *cursor;

	g_return_if_fail(NULL != string);

	if (NULL == text)
		return;

	for (cursor = text; '\0' != *cursor; cursor++)
	{
		switch (*cursor)
		{
		case '&':
			g_string_append(string, "&amp;");
			break;

		case '<':
			g_string_append(string, "&lt;");
			break;

		case '>':
			g_string_append(string, "&gt;");
			break;

		case '"':
			g_string_append(string, "&quot;");
			break;

		/* Single quotes matter because attributes are sometimes
		 * single-quoted -- hx-vals carries JSON full of double quotes,
		 * so its attribute has to be. */
		case '\'':
			g_string_append(string, "&#39;");
			break;

		default:
			g_string_append_c(string, *cursor);
			break;
		}
	}
}

gchar *
venture_html_escape(const gchar *text)
{
	g_autoptr(GString) escaped = NULL;

	escaped = g_string_new(NULL);
	venture_html_escape_append(escaped, text);

	return g_string_free(g_steal_pointer(&escaped), FALSE);
}

gchar *
venture_attribute_escape(const gchar *text)
{
	g_autoptr(GString) escaped = NULL;
	const gchar *cursor;

	escaped = g_string_new(NULL);

	if (NULL == text)
		return g_string_free(g_steal_pointer(&escaped), FALSE);

	for (cursor = text; '\0' != *cursor; cursor++)
	{
		if ('&' == *cursor)
			g_string_append(escaped, "&amp;");
		else if ('\'' == *cursor)
			g_string_append(escaped, "&#39;");
		else if ('<' == *cursor)
			g_string_append(escaped, "&lt;");
		else if ('>' == *cursor)
			g_string_append(escaped, "&gt;");
		else
			g_string_append_c(escaped, *cursor);
	}

	return g_string_free(g_steal_pointer(&escaped), FALSE);
}

gchar *
venture_csv_escape(const gchar *text)
{
	g_autoptr(GString) escaped = NULL;
	gboolean needs_quoting;
	gboolean formula_risk;
	const gchar *cursor;

	if (NULL == text)
		return g_strdup("");

	/* A leading =, +, - or @ makes a spreadsheet treat the cell as a
	 * formula. A contact note starting with "=cmd|..." is a live command
	 * injection the moment someone opens the export. Prefixing a single
	 * quote defuses it while still displaying the original text. */
	formula_risk = (('=' == text[0]) || ('+' == text[0]) ||
	                ('-' == text[0]) || ('@' == text[0]) ||
	                ('\t' == text[0]) || ('\r' == text[0]));

	needs_quoting = formula_risk || g_ascii_isspace(text[0]);

	for (cursor = text; !needs_quoting && ('\0' != *cursor); cursor++)
	{
		if ((',' == *cursor) || ('"' == *cursor) ||
		    ('\n' == *cursor) || ('\r' == *cursor))
			needs_quoting = TRUE;
	}

	if (!needs_quoting)
		return g_strdup(text);

	escaped = g_string_new("\"");

	if (formula_risk)
		g_string_append_c(escaped, '\'');

	for (cursor = text; '\0' != *cursor; cursor++)
	{
		if ('"' == *cursor)
			g_string_append(escaped, "\"\"");
		else
			g_string_append_c(escaped, *cursor);
	}

	g_string_append_c(escaped, '"');

	return g_string_free(g_steal_pointer(&escaped), FALSE);
}

gchar *
venture_truncate(
	const gchar	*text,
	gsize		 max_chars
){
	glong length;
	g_autofree gchar *kept = NULL;

	if (NULL == text)
		return NULL;

	length = g_utf8_strlen(text, -1);

	if (length <= (glong)max_chars)
		return g_strdup(text);

	/* Substring rather than byte slice, so a multi-byte character is
	 * never cut in half and the result stays valid UTF-8. */
	kept = g_utf8_substring(text, 0, (glong)max_chars);

	return g_strdup_printf("%s\xe2\x80\xa6", kept);
}

gchar *
venture_generate_token(gsize n_bytes)
{
	g_autofree guchar *bytes = NULL;
	g_autoptr(GString) hex = NULL;
	gsize i;

	g_return_val_if_fail(n_bytes > 0, NULL);

	bytes = g_new0(guchar, n_bytes);

	/* GRand is a Mersenne Twister and is not suitable for anything
	 * security relevant. g_random is likewise not cryptographic. Reading
	 * the system CSPRNG is the only acceptable source for a token that
	 * authenticates API access. */
	{
		g_autoptr(GError) local_error = NULL;
		g_autoptr(GFile) urandom = NULL;
		g_autoptr(GFileInputStream) stream = NULL;
		gsize read_bytes = 0;

		urandom = g_file_new_for_path("/dev/urandom");
		stream = g_file_read(urandom, NULL, &local_error);

		if (NULL == stream)
		{
			g_error("Cannot open /dev/urandom to generate a token: %s",
			        local_error->message);
		}

		if (!g_input_stream_read_all(G_INPUT_STREAM(stream), bytes, n_bytes,
		                             &read_bytes, NULL, &local_error) ||
		    (read_bytes != n_bytes))
		{
			g_error("Cannot read %" G_GSIZE_FORMAT " random bytes: %s",
			        n_bytes,
			        (NULL != local_error) ? local_error->message : "short read");
		}
	}

	hex = g_string_sized_new(n_bytes * 2);

	for (i = 0; i < n_bytes; i++)
		g_string_append_printf(hex, "%02x", bytes[i]);

	return g_string_free(g_steal_pointer(&hex), FALSE);
}

/*
 * PBKDF2-HMAC-SHA256. GLib has no PBKDF2, so this is the standard
 * construction over GHmac: for a single output block, the result is the XOR
 * of `iterations` successive HMACs, each taken over the previous one.
 *
 * One block of SHA-256 gives 32 bytes of derived key, which is the full
 * strength of the underlying hash and plenty for a password verifier.
 */
static void
venture_pbkdf2_sha256(
	const gchar	*password,
	const guchar	*salt,
	gsize		 salt_len,
	guint		 iterations,
	guchar		*out_key,
	gsize		 key_len
){
	g_autofree guchar *block = NULL;
	g_autofree guchar *previous = NULL;
	g_autofree guchar *salt_and_index = NULL;
	gsize digest_len;
	guint iteration;
	gsize i;

	digest_len = g_checksum_type_get_length(G_CHECKSUM_SHA256);
	block = g_new0(guchar, digest_len);
	previous = g_new0(guchar, digest_len);

	/* The first HMAC is over salt || INT32BE(1); with a single output
	 * block the block index is always 1. */
	salt_and_index = g_new0(guchar, salt_len + 4);
	memcpy(salt_and_index, salt, salt_len);
	salt_and_index[salt_len + 0] = 0;
	salt_and_index[salt_len + 1] = 0;
	salt_and_index[salt_len + 2] = 0;
	salt_and_index[salt_len + 3] = 1;

	{
		g_autoptr(GHmac) hmac = NULL;
		gsize length;

		length = digest_len;
		hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)password,
		                  strlen(password));
		g_hmac_update(hmac, salt_and_index, (gssize)(salt_len + 4));
		g_hmac_get_digest(hmac, previous, &length);
	}

	memcpy(block, previous, digest_len);

	for (iteration = 1; iteration < iterations; iteration++)
	{
		g_autoptr(GHmac) hmac = NULL;
		gsize length;

		length = digest_len;
		hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)password,
		                  strlen(password));
		g_hmac_update(hmac, previous, (gssize)digest_len);
		g_hmac_get_digest(hmac, previous, &length);

		for (i = 0; i < digest_len; i++)
			block[i] ^= previous[i];
	}

	memcpy(out_key, block, MIN(key_len, digest_len));
}

static gchar *
venture_bytes_to_hex(
	const guchar	*bytes,
	gsize		 length
){
	g_autoptr(GString) hex = NULL;
	gsize i;

	hex = g_string_sized_new(length * 2);

	for (i = 0; i < length; i++)
		g_string_append_printf(hex, "%02x", bytes[i]);

	return g_string_free(g_steal_pointer(&hex), FALSE);
}

gchar *
venture_hash_password(
	const gchar	 *password,
	guint		  iterations,
	GError		**error
){
	g_autofree gchar *salt_hex = NULL;
	g_autofree gchar *hash_hex = NULL;
	guchar salt[16];
	guchar key[32];
	gsize i;

	g_return_val_if_fail(NULL != password, NULL);

	if ('\0' == password[0])
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "A password may not be empty");
		return NULL;
	}

	/* A low iteration count is a silent weakness, so clamp rather than
	 * trust a configuration that may have been copied from somewhere
	 * out of date. */
	if (iterations < 100000)
		iterations = 100000;

	salt_hex = venture_generate_token(sizeof(salt));

	for (i = 0; i < sizeof(salt); i++)
	{
		gint high;
		gint low;

		high = g_ascii_xdigit_value(salt_hex[i * 2]);
		low = g_ascii_xdigit_value(salt_hex[(i * 2) + 1]);
		salt[i] = (guchar)((high << 4) | low);
	}

	venture_pbkdf2_sha256(password, salt, sizeof(salt), iterations,
	                      key, sizeof(key));

	hash_hex = venture_bytes_to_hex(key, sizeof(key));

	/* The iteration count travels with the hash, so raising it later
	 * applies to new passwords without invalidating existing ones. */
	return g_strdup_printf("pbkdf2-sha256$%u$%s$%s",
	                       iterations, salt_hex, hash_hex);
}

gboolean
venture_verify_password(
	const gchar	*password,
	const gchar	*encoded
){
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *candidate = NULL;
	guchar salt[16];
	guchar key[32];
	guint iterations;
	gsize salt_len;
	gsize i;

	if ((NULL == password) || (NULL == encoded))
		return FALSE;

	parts = g_strsplit(encoded, "$", 4);

	if ((NULL == parts[0]) || (NULL == parts[1]) ||
	    (NULL == parts[2]) || (NULL == parts[3]))
		return FALSE;

	if (0 != g_strcmp0(parts[0], "pbkdf2-sha256"))
		return FALSE;

	iterations = (guint)g_ascii_strtoull(parts[1], NULL, 10);

	if (0 == iterations)
		return FALSE;

	salt_len = strlen(parts[2]) / 2;

	if (salt_len > sizeof(salt))
		salt_len = sizeof(salt);

	for (i = 0; i < salt_len; i++)
	{
		gint high;
		gint low;

		high = g_ascii_xdigit_value(parts[2][i * 2]);
		low = g_ascii_xdigit_value(parts[2][(i * 2) + 1]);

		if ((high < 0) || (low < 0))
			return FALSE;

		salt[i] = (guchar)((high << 4) | low);
	}

	venture_pbkdf2_sha256(password, salt, salt_len, iterations,
	                      key, sizeof(key));

	candidate = venture_bytes_to_hex(key, sizeof(key));

	return venture_constant_time_equal(candidate, parts[3]);
}

gboolean
venture_constant_time_equal(
	const gchar	*a,
	const gchar	*b
){
	gsize length_a;
	gsize length_b;
	guchar difference;
	gsize i;

	if ((NULL == a) || (NULL == b))
		return (a == b);

	length_a = strlen(a);
	length_b = strlen(b);

	/* A length mismatch is reported without comparing contents, since
	 * length is not the secret; what must not leak is the position of the
	 * first differing byte, which the loop below never reveals because it
	 * always runs to completion. */
	if (length_a != length_b)
		return FALSE;

	difference = 0;

	for (i = 0; i < length_a; i++)
		difference |= (guchar)(a[i] ^ b[i]);

	return (0 == difference);
}

gchar *
venture_pluralise(const gchar *word)
{
	gsize length;

	g_return_val_if_fail(NULL != word, NULL);

	length = strlen(word);

	if (0 == length)
		return g_strdup(word);

	/* "category" to "categories", but "day" to "days": the -y rule only
	 * applies after a consonant. */
	if (('y' == word[length - 1]) && (length > 1) &&
	    (NULL == strchr("aeiou", word[length - 2])))
	{
		g_autofree gchar *stem = NULL;

		stem = g_strndup(word, length - 1);

		return g_strdup_printf("%sies", stem);
	}

	/* Sibilant endings take -es: "tax" to "taxes", "batch" to "batches". */
	if (('s' == word[length - 1]) || ('x' == word[length - 1]) ||
	    ('z' == word[length - 1]) ||
	    ((length > 1) && ('c' == word[length - 2]) && ('h' == word[length - 1])) ||
	    ((length > 1) && ('s' == word[length - 2]) && ('h' == word[length - 1])))
	{
		return g_strdup_printf("%ses", word);
	}

	return g_strdup_printf("%ss", word);
}

gchar *
venture_string_redact_uri(const gchar *uri)
{
	const gchar *scheme_end;
	const gchar *authority;
	const gchar *at;
	const gchar *colon;

	if (NULL == uri)
		return NULL;

	scheme_end = strstr(uri, "://");

	if (NULL == scheme_end)
		return g_strdup(uri);

	authority = scheme_end + 3;
	at = strchr(authority, '@');

	/* No userinfo, so nothing to hide. A SQLite path takes this branch. */
	if (NULL == at)
		return g_strdup(uri);

	colon = memchr(authority, ':', (gsize)(at - authority));

	/* A user with no password. */
	if (NULL == colon)
		return g_strdup(uri);

	return g_strdup_printf("%.*s:%s%s", (int)(colon - uri), uri,
	                       "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2", at);
}

gboolean
venture_string_list_contains_ci(
	const gchar * const	*haystack,
	const gchar		*needle
){
	gsize i;

	if ((NULL == haystack) || (NULL == needle))
		return FALSE;

	for (i = 0; NULL != haystack[i]; i++)
	{
		if (0 == g_ascii_strcasecmp(haystack[i], needle))
			return TRUE;
	}

	return FALSE;
}
