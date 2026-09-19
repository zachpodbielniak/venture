/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

static const gchar base32_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

gchar *
venture_base32_encode(const guchar *bytes, gsize length)
{
	GString *out = g_string_sized_new((length * 8 + 4) / 5 + 1);
	guint buffer = 0;
	gint bits = 0;
	gsize i;
	g_return_val_if_fail(NULL != bytes || 0 == length, NULL);
	for (i = 0; i < length; i++)
	{
		buffer = (buffer << 8) | bytes[i];
		bits += 8;
		while (bits >= 5)
		{
			bits -= 5;
			g_string_append_c(out, base32_alphabet[(buffer >> bits) & 0x1f]);
		}
	}
	if (bits > 0)
		g_string_append_c(out, base32_alphabet[(buffer << (5 - bits)) & 0x1f]);
	return g_string_free(out, FALSE);
}

guchar *
venture_base32_decode(const gchar *text, gsize *out_length)
{
	GByteArray *out;
	guint buffer = 0;
	gint bits = 0;
	const gchar *p;
	g_return_val_if_fail(NULL != text, NULL);
	g_return_val_if_fail(NULL != out_length, NULL);
	out = g_byte_array_new();
	for (p = text; '\0' != *p; p++)
	{
		gchar c = g_ascii_toupper(*p);
		const gchar *at;
		gint value;
		if (' ' == c || '=' == c || '-' == c)
			continue;
		at = strchr(base32_alphabet, c);
		if (NULL == at || '\0' == c)
		{
			g_byte_array_unref(out);
			return NULL;
		}
		value = (gint)(at - base32_alphabet);
		buffer = (buffer << 5) | (guint)value;
		bits += 5;
		if (bits >= 8)
		{
			guchar byte;
			bits -= 8;
			byte = (guchar)((buffer >> bits) & 0xff);
			g_byte_array_append(out, &byte, 1);
		}
	}
	*out_length = out->len;
	return g_byte_array_free(out, FALSE);
}

gchar *
venture_totp_code(const guchar *secret, gsize secret_length, guint64 counter, guint digits)
{
	g_autoptr(GHmac) hmac = NULL;
	guchar message[8];
	guchar digest[20];
	gsize digest_length = sizeof(digest);
	guint offset;
	guint32 binary;
	guint32 modulus = 1;
	guint i;
	g_return_val_if_fail(NULL != secret, NULL);
	g_return_val_if_fail(digits >= 6 && digits <= 8, NULL);
	/* The counter travels big-endian, RFC 4226 section 5.3. */
	for (i = 0; i < 8; i++)
		message[i] = (guchar)((counter >> (8 * (7 - i))) & 0xff);
	hmac = g_hmac_new(G_CHECKSUM_SHA1, secret, (gssize)secret_length);
	g_hmac_update(hmac, message, sizeof(message));
	g_hmac_get_digest(hmac, digest, &digest_length);
	/* Dynamic truncation: the low nibble of the last byte picks four
	 * bytes, whose top bit is masked off. */
	offset = digest[19] & 0x0f;
	binary = ((guint32)(digest[offset] & 0x7f) << 24) |
	         ((guint32)digest[offset + 1] << 16) |
	         ((guint32)digest[offset + 2] << 8) |
	         (guint32)digest[offset + 3];
	for (i = 0; i < digits; i++)
		modulus *= 10;
	return g_strdup_printf("%0*u", (gint)digits, binary % modulus);
}

guint64
venture_totp_counter(GDateTime *when)
{
	gint64 unix_seconds;
	g_return_val_if_fail(NULL != when, 0);
	unix_seconds = g_date_time_to_unix(when);
	if (unix_seconds < 0)
		return 0;
	return (guint64)unix_seconds / VENTURE_TOTP_PERIOD;
}

gboolean
venture_totp_verify(const guchar *secret, gsize secret_length, const gchar *code,
	GDateTime *now, guint drift_steps, guint64 *out_counter)
{
	g_autofree gchar *typed = NULL;
	guint64 centre;
	guint64 first;
	guint64 step;
	gboolean matched = FALSE;
	const gchar *p;
	gchar *q;
	g_return_val_if_fail(NULL != secret, FALSE);
	g_return_val_if_fail(NULL != now, FALSE);
	if (NULL != out_counter)
		*out_counter = 0;
	if (NULL == code)
		return FALSE;
	/* People type "123 456" and apps display it that way. */
	typed = g_strdup(code);
	for (p = code, q = typed; '\0' != *p; p++)
		if (' ' != *p)
			*q++ = *p;
	*q = '\0';
	if (strlen(typed) != VENTURE_TOTP_DIGITS)
		return FALSE;
	for (p = typed; '\0' != *p; p++)
		if (!g_ascii_isdigit(*p))
			return FALSE;
	centre = venture_totp_counter(now);
	first = (centre > drift_steps) ? (centre - drift_steps) : 0;
	/* Every step in the window is compared, even after a match, so the
	 * time taken does not say which step matched. */
	for (step = first; step <= centre + drift_steps; step++)
	{
		g_autofree gchar *expected = venture_totp_code(secret, secret_length, step, VENTURE_TOTP_DIGITS);
		if (venture_constant_time_equal(expected, typed) && !matched)
		{
			matched = TRUE;
			if (NULL != out_counter)
				*out_counter = step;
		}
	}
	return matched;
}

gchar *
venture_totp_uri(const gchar *issuer, const gchar *account, const gchar *base32_secret)
{
	g_autofree gchar *issuer_escaped = NULL;
	g_autofree gchar *account_escaped = NULL;
	g_return_val_if_fail(NULL != base32_secret, NULL);
	if (venture_string_is_empty(issuer))
		issuer = "VENTURE";
	if (venture_string_is_empty(account))
		account = "account";
	issuer_escaped = g_uri_escape_string(issuer, NULL, FALSE);
	account_escaped = g_uri_escape_string(account, NULL, FALSE);
	return g_strdup_printf("otpauth://totp/%s:%s?secret=%s&issuer=%s&algorithm=SHA1&digits=%u&period=%u",
		issuer_escaped, account_escaped, base32_secret, issuer_escaped,
		(guint)VENTURE_TOTP_DIGITS, (guint)VENTURE_TOTP_PERIOD);
}
