/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "sequences/venture-sequence-tracking-private.h"
#include <sys/random.h>
#include <errno.h>
#include <string.h>

/* 256 bits from the operating system, hex encoded: 64 characters. */
gchar *
venture_sequence_tracking_token(GError **error)
{
	guchar bytes[32];
	gchar *result;
	gsize used = 0;
	guint i;
	while (used < sizeof(bytes))
	{
		ssize_t n = getrandom(bytes + used, sizeof(bytes) - used, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
				"VentureSequenceService: cannot obtain secure random bytes");
			return NULL;
		}
		used += (gsize)n;
	}
	result = g_malloc(2 * sizeof(bytes) + 1);
	for (i = 0; i < sizeof(bytes); i++)
		g_snprintf(result + 2 * i, 3, "%02x", bytes[i]);
	return result;
}

static gboolean
url_start(const gchar *cursor)
{
	return g_str_has_prefix(cursor, "http://") || g_str_has_prefix(cursor, "https://");
}

/* A link ends at whitespace or a delimiter; trailing prose punctuation
 * stays outside the link so "see https://x.example/a." keeps its period. */
static gsize
url_length(const gchar *cursor)
{
	gsize length = 0;
	while (cursor[length] != '\0' && !g_ascii_isspace(cursor[length]) &&
		strchr("\"'<>", cursor[length]) == NULL)
		length++;
	while (length > 0 && strchr(".,;)", cursor[length - 1]) != NULL)
		length--;
	return length;
}

/*
 * Rewrites every absolute link in @body to the click endpoint, appends the
 * open pixel and reports the original destinations in order, starting at
 * position one. @base_url may be empty, in which case the paths are relative.
 */
gchar *
venture_sequence_tracking_wrap(const gchar *body, const gchar *base_url,
	const gchar *token, GPtrArray *urls)
{
	g_autoptr(GString) output = g_string_new(NULL);
	g_autofree gchar *base = g_strdup(base_url != NULL ? base_url : "");
	const gchar *cursor = body != NULL ? body : "";
	guint position = 0;
	if (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';
	while (*cursor != '\0')
	{
		gsize length;
		if (!url_start(cursor))
		{
			g_string_append_c(output, *cursor);
			cursor++;
			continue;
		}
		length = url_length(cursor);
		position++;
		g_ptr_array_add(urls, g_strndup(cursor, length));
		g_string_append_printf(output, "%s/t/c/%s/%u", base, token, position);
		cursor += length;
	}
	g_string_append_printf(output, "\n<img src=\"%s/t/o/%s.gif\" width=\"1\" height=\"1\" alt=\"\">", base, token);
	return g_string_free(g_steal_pointer(&output), FALSE);
}

/* One transparent pixel; the smallest valid GIF89a. */
const guint8 *
venture_sequence_tracking_pixel(gsize *length)
{
	static const guint8 pixel[] = {
		0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x21, 0xf9, 0x04, 0x01, 0x00,
		0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
		0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b
	};
	*length = sizeof(pixel);
	return pixel;
}
