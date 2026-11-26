/*
 * test-avatars.c - Profile pictures
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A picture is the one file a person uploads that every other person in
 * their organizations is then shown, so the rules pinned here are the
 * ones whose failure is somebody else's problem: nothing but PNG, JPEG
 * and WebP is ever handed to a decoder; a small file claiming an
 * enormous image is refused from its header; what is kept is a PNG of
 * pixels and nothing else (no EXIF, no location); and a picture is served
 * to exactly the readers who may already see the name beside it.
 *
 * The tests that decode need GdkPixbuf that can actually decode. On
 * Fedora 44 that is glycin, whose sandbox a container without user
 * namespaces cannot start; there they skip, saying so, rather than fail
 * for a reason that is not this code's. Everything before the decoder,
 * the encoder, the store's refusals and every access rule run anywhere,
 * in builds with and without GdkPixbuf.
 */

#include <venture.h>

#include <string.h>
#include <libsoup/soup.h>

#ifdef VENTURE_HAVE_GDK_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include "venture-test-util.h"

/* --- Fixture pictures ------------------------------------------------------ */

/*
 * A 6 by 4 JPEG and the same as a lossless WebP, written once by
 * GdkPixbuf/glycin and kept as bytes: neither format can be written here
 * without the codec the test may not have.
 */
static const guint8 fixture_jpeg[] = {
	0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
	0x02, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xc0, 0x00, 0x11,
	0x08, 0x00, 0x04, 0x00, 0x06, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01,
	0x03, 0x11, 0x01, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x10, 0x0b, 0x0c, 0x0e,
	0x0c, 0x0a, 0x10, 0x0e, 0x0d, 0x0e, 0x12, 0x11, 0x10, 0x13, 0x18, 0x28,
	0x1a, 0x18, 0x16, 0x16, 0x18, 0x31, 0x23, 0x25, 0x1d, 0x28, 0x3a, 0x33,
	0x3d, 0x3c, 0x39, 0x33, 0x38, 0x37, 0x40, 0x48, 0x5c, 0x4e, 0x40, 0x44,
	0x57, 0x45, 0x37, 0x38, 0x50, 0x6d, 0x51, 0x57, 0x5f, 0x62, 0x67, 0x68,
	0x67, 0x3e, 0x4d, 0x71, 0x79, 0x70, 0x64, 0x78, 0x5c, 0x65, 0x67, 0x63,
	0xff, 0xdb, 0x00, 0x43, 0x01, 0x11, 0x12, 0x12, 0x18, 0x15, 0x18, 0x2f,
	0x1a, 0x1a, 0x2f, 0x63, 0x42, 0x38, 0x42, 0x63, 0x63, 0x63, 0x63, 0x63,
	0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63,
	0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63,
	0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63,
	0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0xff, 0xc4, 0x00,
	0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
	0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00,
	0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
	0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
	0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
	0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
	0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
	0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
	0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
	0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
	0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86,
	0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3,
	0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
	0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
	0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00,
	0x1f, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
	0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
	0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
	0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
	0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
	0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
	0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
	0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
	0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84,
	0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
	0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
	0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
	0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
	0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xda, 0x00,
	0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0xa1,
	0x5e, 0xe1, 0xe7, 0x9f, 0xff, 0xd9
};

static const guint8 fixture_webp[] = {
	0x52, 0x49, 0x46, 0x46, 0x98, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
	0x56, 0x50, 0x38, 0x4c, 0x8b, 0x00, 0x00, 0x00, 0x2f, 0x05, 0xc0, 0x00,
	0x00, 0xcd, 0x55, 0x20, 0x22, 0x02, 0x1e, 0x88, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x78, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe7, 0x1f, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x81, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x9c, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
	0xe4, 0x87, 0x02, 0x00
};

/* The colours of the three bands of the wide test picture. */
static const guint8 band_left[3] = { 0xff, 0x00, 0x00 };
static const guint8 band_middle[3] = { 0x00, 0xff, 0x00 };
static const guint8 band_right[3] = { 0x00, 0x00, 0xff };

/*
 * A 60 by 30 PNG in three vertical bands -- red, green, blue, 20 pixels
 * each -- written by the store's own encoder, which needs no codec. Its
 * middle square is all green, so a centre crop is visible from outside.
 */
static GBytes *
wide_png(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree guint8 *pixels = NULL;
	GBytes *png;
	gint x;
	gint y;

	pixels = g_malloc(60 * 30 * 3);

	for (y = 0; y < 30; y++)
	{
		for (x = 0; x < 60; x++)
		{
			const guint8 *band;

			band = (x < 20) ? band_left : (x < 40) ? band_middle : band_right;
			memcpy(pixels + (y * 60 + x) * 3, band, 3);
		}
	}

	png = venture_avatar_png_encode(pixels, 60, 30, 60 * 3, FALSE, &error);
	g_assert_no_error(error);

	return png;
}

/* A file that is only a PNG header claiming @width by @height. */
static GBytes *
claimed_png(
	guint32	width,
	guint32	height
){
	guint8 bytes[33];

	memset(bytes, 0, sizeof(bytes));
	memcpy(bytes, "\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR", 16);
	bytes[16] = (guint8)(width >> 24);
	bytes[17] = (guint8)(width >> 16);
	bytes[18] = (guint8)(width >> 8);
	bytes[19] = (guint8)width;
	bytes[20] = (guint8)(height >> 24);
	bytes[21] = (guint8)(height >> 16);
	bytes[22] = (guint8)(height >> 8);
	bytes[23] = (guint8)height;
	bytes[24] = 8;
	bytes[25] = 6;

	return g_bytes_new(bytes, sizeof(bytes));
}

/* --- Reading PNG back ------------------------------------------------------ */

static guint32
be32_at(const guint8 *p)
{
	return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
	       ((guint32)p[2] << 8) | (guint32)p[3];
}

/*
 * The chunk types of a PNG, in order, joined by spaces ("IHDR IDAT IEND"),
 * and its IHDR size. Asserts the signature and that every chunk fits.
 */
static gchar *
png_chunks(
	GBytes	*png,
	guint32	*width,
	guint32	*height
){
	g_autoptr(GString) types = NULL;
	const guint8 *p;
	gsize size;
	gsize at;

	p = g_bytes_get_data(png, &size);
	g_assert_cmpuint(size, >, 8);
	g_assert_cmpmem(p, 8, "\x89PNG\r\n\x1a\n", 8);

	types = g_string_new(NULL);
	at = 8;

	while (at + 12 <= size)
	{
		guint32 length = be32_at(p + at);

		g_assert_cmpuint(at + 12 + length, <=, size);

		if (types->len > 0)
			g_string_append_c(types, ' ');

		g_string_append_len(types, (const gchar *)p + at + 4, 4);

		if ((0 == memcmp(p + at + 4, "IHDR", 4)) && (NULL != width))
		{
			*width = be32_at(p + at + 8);
			*height = be32_at(p + at + 12);
		}

		at += 12 + length;
	}

	g_assert_cmpuint(at, ==, size);

	return g_string_free(g_steal_pointer(&types), FALSE);
}

/* --- Whether this machine can decode ------------------------------------- */

/*
 * TRUE when GdkPixbuf here can decode a PNG at all. FALSE in a build
 * without it, and where its decoder cannot run (glycin's sandbox in a
 * container); *why says which, for the skip message.
 */
static gboolean
can_decode(const gchar **why)
{
#ifdef VENTURE_HAVE_GDK_PIXBUF
	static gint known = -1;
	static gchar *reason = NULL;

	if (known < 0)
	{
		g_autoptr(GdkPixbufLoader) loader = NULL;
		g_autoptr(GBytes) png = NULL;
		g_autoptr(GError) error = NULL;
		gboolean ok;

		png = wide_png();
		loader = gdk_pixbuf_loader_new_with_mime_type("image/png", &error);
		ok = (NULL != loader) && gdk_pixbuf_loader_write_bytes(loader, png, &error);

		if (NULL != loader)
			ok = gdk_pixbuf_loader_close(loader, ok ? &error : NULL) && ok;

		known = ok ? 1 : 0;

		if (!ok)
			reason = g_strdup_printf("GdkPixbuf cannot decode here: %s",
			                         (NULL != error) ? error->message : "no reason");
	}

	*why = reason;

	return 1 == known;
#else
	*why = "built without GdkPixbuf";

	return FALSE;
#endif
}

/* can_decode() without the reason. */
#ifdef VENTURE_HAVE_GDK_PIXBUF
static gboolean
can_decode_quietly(void)
{
	const gchar *why = NULL;

	return can_decode(&why);
}
#endif

/* Skips the current test unless pictures can be decoded; TRUE to go on. */
static gboolean
need_decoder(void)
{
	const gchar *why = NULL;

	if (can_decode(&why))
		return TRUE;

	g_test_skip(why);

	return FALSE;
}

/* --- Before any decoder ---------------------------------------------------- */

/*
 * The bytes, never the name: PNG, JPEG and WebP by their magic numbers;
 * GIF, SVG, BMP, text, nothing and a cut-short PNG refused. If this
 * regresses an SVG -- which carries script -- reaches a decoder, or a GIF
 * becomes somebody's face.
 */
static void
test_sniff(void)
{
	static const gchar gif[] = "GIF89a\x10\x00\x10\x00\x80\x00\x00";
	static const gchar svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"></svg>";
	static const gchar bmp[] = "BM\x36\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00\x28\x00\x00\x00"
	                           "\x01\x00\x00\x00\x01\x00\x00\x00\x01\x00\x18\x00";
	static const gchar text[] = "Hello, this is a letter, not a picture of anybody.";
	g_autoptr(GBytes) png = NULL;
	g_autoptr(GBytes) truncated = NULL;
	g_autoptr(GBytes) jpeg = NULL;
	g_autoptr(GBytes) webp = NULL;
	g_autoptr(GBytes) empty = NULL;
	g_autoptr(GError) error = NULL;
	gint width;
	gint height;

	png = wide_png();
	jpeg = g_bytes_new_static(fixture_jpeg, sizeof(fixture_jpeg));
	webp = g_bytes_new_static(fixture_webp, sizeof(fixture_webp));

	width = height = 0;
	g_assert_cmpstr(venture_avatar_sniff(png, &width, &height), ==, "image/png");
	g_assert_cmpint(width, ==, 60);
	g_assert_cmpint(height, ==, 30);
	g_assert_cmpstr(venture_avatar_sniff(jpeg, &width, &height), ==, "image/jpeg");
	g_assert_cmpint(width, ==, 6);
	g_assert_cmpint(height, ==, 4);
	g_assert_cmpstr(venture_avatar_sniff(webp, &width, &height), ==, "image/webp");
	g_assert_cmpint(width, ==, 6);
	g_assert_cmpint(height, ==, 4);

	{
		g_autoptr(GBytes) b1 = g_bytes_new_static(gif, sizeof(gif) - 1);
		g_autoptr(GBytes) b2 = g_bytes_new_static(svg, sizeof(svg) - 1);
		g_autoptr(GBytes) b3 = g_bytes_new_static(bmp, sizeof(bmp) - 1);
		g_autoptr(GBytes) b4 = g_bytes_new_static(text, sizeof(text) - 1);

		g_assert_null(venture_avatar_sniff(b1, NULL, NULL));
		g_assert_null(venture_avatar_sniff(b2, NULL, NULL));
		g_assert_null(venture_avatar_sniff(b3, NULL, NULL));
		g_assert_null(venture_avatar_sniff(b4, NULL, NULL));

		/* Refused before a decoder, with the reason, as a validation. */
		g_assert_null(venture_avatar_check_upload(b2, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "not a PNG, JPEG or WebP"));
		g_clear_error(&error);
	}

	empty = g_bytes_new(NULL, 0);
	g_assert_null(venture_avatar_sniff(empty, NULL, NULL));
	g_assert_null(venture_avatar_sniff(NULL, NULL, NULL));
	g_assert_null(venture_avatar_check_upload(empty, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "no file arrived"));
	g_clear_error(&error);

	/* A PNG cut off inside its header is not a PNG. */
	truncated = g_bytes_new_from_bytes(png, 0, 20);
	g_assert_null(venture_avatar_sniff(truncated, NULL, NULL));
	g_assert_null(venture_avatar_check_upload(truncated, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_assert_cmpstr(venture_avatar_check_upload(png, &error), ==, "image/png");
	g_assert_no_error(error);
}

/*
 * The size cap is a count of bytes, checked first: one byte over is
 * refused however good a picture the rest would be.
 */
static void
test_size_cap(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) over = NULL;
	g_autoptr(GBytes) header = NULL;
	guint8 *big;

	header = claimed_png(10, 10);
	big = g_malloc0(VENTURE_AVATAR_MAX_BYTES + 1);
	memcpy(big, g_bytes_get_data(header, NULL), g_bytes_get_size(header));
	over = g_bytes_new_take(big, VENTURE_AVATAR_MAX_BYTES + 1);

	g_assert_null(venture_avatar_check_upload(over, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "at most 5 MB"));
	g_clear_error(&error);

	/* The same header under the cap passes everything a header can say. */
	g_assert_cmpstr(venture_avatar_check_upload(header, &error), ==, "image/png");
	g_assert_no_error(error);
}

/*
 * A decompression bomb: a few bytes that claim 100000 by 100000 pixels,
 * forty gigabytes once decoded. Refused from the header, by the upload
 * check and by the whole render, before any decoder is made -- the test
 * would exhaust memory, not fail, if it reached one.
 */
static void
test_pixel_cap(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bomb = NULL;
	g_autoptr(GBytes) edge = NULL;
	g_autoptr(GPtrArray) renditions = NULL;

	bomb = claimed_png(100000, 100000);
	g_assert_null(venture_avatar_check_upload(bomb, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "100000 by 100000"));
	g_clear_error(&error);

	renditions = venture_avatar_render(bomb, &error);
	g_assert_null(renditions);

	if (venture_avatar_is_available())
	{
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "at most 4096 on a side"));
	}
	else
	{
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED);
	}

	g_clear_error(&error);

	/* One side over is enough. */
	edge = claimed_png(VENTURE_AVATAR_MAX_SIDE + 1, 1);
	g_assert_null(venture_avatar_check_upload(edge, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/*
 * The encoder writes IHDR, IDAT and IEND and nothing else, with correct
 * CRCs (GdkPixbuf reads it back, where it can decode), the same bytes for
 * the same pixels. A text or EXIF chunk here would be metadata kept.
 */
static void
test_png_encode(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) once = NULL;
	g_autoptr(GBytes) again = NULL;
	g_autofree gchar *chunks = NULL;
	guint8 pixels[2 * 3 * 4];
	guint32 width = 0;
	guint32 height = 0;
	guint i;

	for (i = 0; i < sizeof(pixels); i++)
		pixels[i] = (guint8)(i * 11);

	once = venture_avatar_png_encode(pixels, 3, 2, 12, TRUE, &error);
	g_assert_no_error(error);
	again = venture_avatar_png_encode(pixels, 3, 2, 12, TRUE, &error);
	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(once, again));

	chunks = png_chunks(once, &width, &height);
	g_assert_cmpstr(chunks, ==, "IHDR IDAT IEND");
	g_assert_cmpuint(width, ==, 3);
	g_assert_cmpuint(height, ==, 2);

	g_assert_null(venture_avatar_png_encode(pixels, 0, 2, 12, TRUE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

#ifdef VENTURE_HAVE_GDK_PIXBUF
	/* Read back where a decoder can run; the chunk checks above already
	 * hold everywhere, so this is a note, not a skip. */
	{
		const gchar *why = NULL;

		if (!can_decode(&why))
			g_test_message("PNG read-back not checked: %s", why);
	}

	if (can_decode_quietly())
	{
		g_autoptr(GdkPixbufLoader) loader = NULL;
		GdkPixbuf *read;
		const guint8 *back;

		loader = gdk_pixbuf_loader_new_with_mime_type("image/png", &error);
		g_assert_no_error(error);
		g_assert_true(gdk_pixbuf_loader_write_bytes(loader, once, &error));
		g_assert_true(gdk_pixbuf_loader_close(loader, &error));
		g_assert_no_error(error);
		read = gdk_pixbuf_loader_get_pixbuf(loader);
		g_assert_cmpint(gdk_pixbuf_get_width(read), ==, 3);
		g_assert_cmpint(gdk_pixbuf_get_height(read), ==, 2);
		back = gdk_pixbuf_read_pixels(read);
		g_assert_cmpmem(back, 12, pixels, 12);
	}
#endif
}

/* "Ruth Ellery" is RE; the one rule every badge uses. */
static void
test_initials(void)
{
	g_autofree gchar *a = venture_avatar_initials("Ruth Ellery");
	g_autofree gchar *b = venture_avatar_initials("zach");
	g_autofree gchar *c = venture_avatar_initials("  -- ");
	g_autofree gchar *d = venture_avatar_initials(NULL);
	g_autofree gchar *e = venture_avatar_initials("\xc3\xa9mile zola dupont");

	g_assert_cmpstr(a, ==, "RE");
	g_assert_cmpstr(b, ==, "Z");
	g_assert_cmpstr(c, ==, "?");
	g_assert_cmpstr(d, ==, "?");
	g_assert_cmpstr(e, ==, "\xc3\x89Z");
}

/*
 * The badge: an <img> with alt, size, lazy loading and the 2x picture
 * when there is an address; the initials, escaped and hidden from a
 * screen reader, when there is not.
 */
static void
test_badge_markup(void)
{
	g_autoptr(GString) html = NULL;
	g_autofree gchar *url = NULL;

	url = venture_avatar_url(7, "0123456789abcdef0123", VENTURE_AVATAR_SIZE_SMALL);
	g_assert_cmpstr(url, ==, "/avatar/7/32?v=0123456789abcdef0123");

	html = g_string_new(NULL);
	venture_avatar_html_append(html, url, "Ruth <Ellery>", VENTURE_AVATAR_SIZE_SMALL);
	g_assert_nonnull(strstr(html->str, "<img class=\"avatar avatar-32\" src=\"/avatar/7/32?v=0123456789abcdef0123\""));
	g_assert_nonnull(strstr(html->str, "srcset=\"/avatar/7/64?v=0123456789abcdef0123 2x\""));
	g_assert_nonnull(strstr(html->str, "alt=\"Ruth &lt;Ellery&gt;\""));
	g_assert_nonnull(strstr(html->str, "width=\"32\" height=\"32\" loading=\"lazy\""));

	g_string_truncate(html, 0);
	venture_avatar_html_append(html, NULL, "Ruth <Ellery>", VENTURE_AVATAR_SIZE_SMALL);
	g_assert_cmpstr(html->str, ==,
		"<span class=\"avatar avatar-32 avatar-initials\" aria-hidden=\"true\">RE</span>");
}

/* --- The decoder ----------------------------------------------------------- */

/* The pixel at the middle of a rendition, through GdkPixbuf. */
#ifdef VENTURE_HAVE_GDK_PIXBUF
static void
centre_pixel(
	GBytes	*png,
	guint8	*rgb
){
	g_autoptr(GdkPixbufLoader) loader = NULL;
	g_autoptr(GError) error = NULL;
	GdkPixbuf *read;
	const guint8 *pixels;
	gint w;
	gint h;

	loader = gdk_pixbuf_loader_new_with_mime_type("image/png", &error);
	g_assert_no_error(error);
	g_assert_true(gdk_pixbuf_loader_write_bytes(loader, png, &error));
	g_assert_true(gdk_pixbuf_loader_close(loader, &error));
	read = gdk_pixbuf_loader_get_pixbuf(loader);
	w = gdk_pixbuf_get_width(read);
	h = gdk_pixbuf_get_height(read);
	pixels = gdk_pixbuf_read_pixels(read);
	memcpy(rgb, pixels + (h / 2) * gdk_pixbuf_get_rowstride(read) +
	       (w / 2) * gdk_pixbuf_get_n_channels(read), 3);
}
#endif

/*
 * A wide picture becomes four square PNGs, 32, 64, 96 and 192 pixels,
 * cropped from its middle (all green), each nothing but IHDR, IDAT and
 * IEND. A JPEG and a WebP render too. If this regresses a person's face
 * is squashed, or their phone's location rides along in the file.
 */
static void
test_render(void)
{
#ifdef VENTURE_HAVE_GDK_PIXBUF
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) wide = NULL;
	g_autoptr(GPtrArray) renditions = NULL;
	const guint *sizes;
	guint n_sizes;
	guint i;

	if (!need_decoder())
		return;

	wide = wide_png();
	renditions = venture_avatar_render(wide, &error);
	g_assert_no_error(error);
	sizes = venture_avatar_sizes(&n_sizes);
	g_assert_cmpuint(renditions->len, ==, n_sizes);
	g_assert_cmpuint(n_sizes, ==, 4);
	g_assert_cmpuint(sizes[0], ==, 32);
	g_assert_cmpuint(sizes[3], ==, 192);

	for (i = 0; i < renditions->len; i++)
	{
		GBytes *png = g_ptr_array_index(renditions, i);
		g_autofree gchar *chunks = NULL;
		guint32 width = 0;
		guint32 height = 0;
		guint8 rgb[3];

		chunks = png_chunks(png, &width, &height);
		g_assert_cmpstr(chunks, ==, "IHDR IDAT IEND");
		g_assert_null(strstr(chunks, "tEXt"));
		g_assert_null(strstr(chunks, "eXIf"));
		g_assert_cmpuint(width, ==, sizes[i]);
		g_assert_cmpuint(height, ==, sizes[i]);

		centre_pixel(png, rgb);
		g_assert_cmpmem(rgb, 3, band_middle, 3);
	}

	g_clear_pointer(&renditions, g_ptr_array_unref);

	{
		g_autoptr(GBytes) jpeg = g_bytes_new_static(fixture_jpeg, sizeof(fixture_jpeg));

		renditions = venture_avatar_render(jpeg, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(renditions->len, ==, 4);
		g_clear_pointer(&renditions, g_ptr_array_unref);
	}

	/* WebP decodes when this server has a WebP decoder, and is refused
	 * saying so when it has not; never a crash. */
	{
		g_autoptr(GBytes) webp = g_bytes_new_static(fixture_webp, sizeof(fixture_webp));

		renditions = venture_avatar_render(webp, &error);

		if (NULL == renditions)
		{
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
			g_assert_nonnull(strstr(error->message, "WebP"));
			g_clear_error(&error);
		}
		else
		{
			g_assert_cmpuint(renditions->len, ==, 4);
		}
	}

	/* Garbage after a good header is refused by the decoder, cleanly. */
	{
		g_autoptr(GBytes) broken = NULL;
		guint8 bytes[200];

		memset(bytes, 0x5a, sizeof(bytes));
		memcpy(bytes, g_bytes_get_data(wide, NULL), 33);
		broken = g_bytes_new(bytes, sizeof(bytes));
		g_assert_null(venture_avatar_render(broken, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "could not be decoded"));
	}
#else
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) wide = wide_png();

	/* Without GdkPixbuf the feature says it is absent, and that is all. */
	g_assert_false(venture_avatar_is_available());
	g_assert_null(venture_avatar_render(wide, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED);
	g_assert_nonnull(strstr(error->message, "not available in this build"));
#endif
}

/* --- The server ------------------------------------------------------------ */

typedef struct
{
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	VentureWebServer *server;
	gchar *state_dir;
	gint64 org;
	gint64 other_org;
	gint64 owner_id;
	gint64 editor_id;
	gint64 viewer_id;
	gint64 outsider_id;
	gchar *owner_token;
	gchar *editor_token;
	gchar *viewer_token;
	gchar *readonly_token;
	gchar *outsider_token;
	VentureEntity *company;
} Fixture;

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->database, record, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(record), error->message);
}

static gint64
make_user(
	Fixture		*f,
	const gchar	*username,
	const gchar	*display,
	VentureUserRole	 role
){
	g_autoptr(VentureEntity) user = NULL;

	user = g_object_new(VENTURE_TYPE_USER, "username", username,
	                    "display-name", display, "role", role, "active", TRUE,
	                    NULL);
	save(f, user);

	return venture_entity_get_id(user);
}

static void
make_member(
	Fixture			*f,
	gint64			 user_id,
	gint64			 org,
	VentureOrganizationRole	 role
){
	g_autoptr(VentureEntity) member = NULL;

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
	                      "user-id", user_id, "organization-id", org,
	                      "role", role, "active", TRUE, NULL);
	save(f, member);
}

static gchar *
make_token(
	Fixture		*f,
	gint64		 user_id,
	VentureUserRole	 role
){
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", "picture test", "user-id", user_id,
	             "role", role, NULL);
	secret = venture_api_token_generate(token);
	save(f, VENTURE_ENTITY(token));

	return secret;
}

/*
 * A server with sign-in on, two organizations, the install's owner, an
 * editor and a viewer in the first, somebody in the second only, and
 * tokens for each -- the editor's viewer-role token is read-only.
 */
static void
fixture_set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) other = NULL;

	(void)data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "test-secret-value", TRUE);
	f->state_dir = g_dir_make_tmp("venture-avatars-XXXXXX", &error);
	g_assert_no_error(error);

	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-require-auth", TRUE,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             NULL);

	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->org = venture_context_get_default_organization_id(f->context);

	other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other Ltd",
	                     "slug", "other", NULL);
	save(f, other);
	f->other_org = venture_entity_get_id(other);

	f->owner_id = make_user(f, "olive", "Olive Owner", VENTURE_USER_ROLE_OWNER);
	f->editor_id = make_user(f, "erin", "Erin Edits", VENTURE_USER_ROLE_EDITOR);
	f->viewer_id = make_user(f, "vic", "Vic Views", VENTURE_USER_ROLE_VIEWER);
	f->outsider_id = make_user(f, "otto", "Otto Outside", VENTURE_USER_ROLE_EDITOR);

	make_member(f, f->owner_id, f->org, VENTURE_ORGANIZATION_ROLE_OWNER);
	make_member(f, f->editor_id, f->org, VENTURE_ORGANIZATION_ROLE_EDITOR);
	make_member(f, f->viewer_id, f->org, VENTURE_ORGANIZATION_ROLE_VIEWER);
	make_member(f, f->outsider_id, f->other_org, VENTURE_ORGANIZATION_ROLE_EDITOR);

	f->owner_token = make_token(f, f->owner_id, VENTURE_USER_ROLE_OWNER);
	f->editor_token = make_token(f, f->editor_id, VENTURE_USER_ROLE_EDITOR);
	f->viewer_token = make_token(f, f->viewer_id, VENTURE_USER_ROLE_VIEWER);
	f->readonly_token = make_token(f, f->editor_id, VENTURE_USER_ROLE_VIEWER);
	f->outsider_token = make_token(f, f->outsider_id, VENTURE_USER_ROLE_EDITOR);

	/* The viewer's own company: a viewer reads only what they own, and the
	 * thread on it is where they see a colleague's picture. */
	f->company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Bellhaven Books",
	                          "owner-user-id", f->viewer_id, NULL);
	venture_entity_set_organization_id(f->company, f->org);
	save(f, f->company);

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(Fixture *f, gconstpointer data)
{
	(void)data;

	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->company);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	g_free(f->owner_token);
	g_free(f->editor_token);
	g_free(f->viewer_token);
	g_free(f->readonly_token);
	g_free(f->outsider_token);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                 result, &reply->error);
	reply->done = TRUE;
}

/* What came back from one request. */
typedef struct
{
	guint status;
	GBytes *body;
	gchar *content_type;
	gchar *cache_control;
	gchar *etag;
	gchar *nosniff;
	gchar *location;
} Answer;

static void
answer_clear(Answer *answer)
{
	g_clear_pointer(&answer->body, g_bytes_unref);
	g_clear_pointer(&answer->content_type, g_free);
	g_clear_pointer(&answer->cache_control, g_free);
	g_clear_pointer(&answer->etag, g_free);
	g_clear_pointer(&answer->nosniff, g_free);
	g_clear_pointer(&answer->location, g_free);
}

/*
 * One request on this thread's main context, where the server runs.
 * Redirects are not followed. @body may be NULL.
 */
static void
request(
	Fixture		*f,
	const gchar	*method,
	const gchar	*path,
	const gchar	*token,
	const gchar	*content_type,
	GBytes		*body,
	const gchar	*if_none_match,
	Answer		*answer
){
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SoupMessageHeaders *headers;
	SoupMessageHeaders *response;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	memset(answer, 0, sizeof(*answer));
	session = soup_session_new_with_options("timeout", 30, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	if (NULL != token)
	{
		g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);

		soup_message_headers_replace(headers, "Authorization", bearer);
	}

	if (NULL != if_none_match)
		soup_message_headers_replace(headers, "If-None-Match", if_none_match);

	if (NULL != body)
		soup_message_set_request_body_from_bytes(message, content_type, body);

	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	response = soup_message_get_response_headers(message);
	answer->status = soup_message_get_status(message);
	answer->body = reply.bytes;
	answer->content_type = g_strdup(soup_message_headers_get_one(response, "Content-Type"));
	answer->cache_control = g_strdup(soup_message_headers_get_one(response, "Cache-Control"));
	answer->etag = g_strdup(soup_message_headers_get_one(response, "ETag"));
	answer->nosniff = g_strdup(soup_message_headers_get_one(response, "X-Content-Type-Options"));
	answer->location = g_strdup(soup_message_headers_get_one(response, "Location"));
}

/* The body of an answer as JSON. */
static JsonNode *
answer_json(Answer *answer)
{
	g_autofree gchar *text = NULL;

	text = g_strndup(g_bytes_get_data(answer->body, NULL),
	                 g_bytes_get_size(answer->body));

	return venture_json_parse(text, NULL);
}

static gboolean
answer_mentions(
	Answer		*answer,
	const gchar	*needle
){
	g_autofree gchar *text = NULL;

	text = g_strndup(g_bytes_get_data(answer->body, NULL),
	                 g_bytes_get_size(answer->body));

	return NULL != strstr(text, needle);
}

/* One raw HTTP exchange from a thread, while this thread runs the server. */
typedef struct
{
	guint16	 port;
	gchar	*request;
	gchar	*response;
	gint	 done;
} RawExchange;

static gpointer
raw_exchange_thread(gpointer data)
{
	RawExchange *x = data;
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GString) response = NULL;
	gchar buffer[1024];
	gssize count;

	client = g_socket_client_new();
	g_socket_client_set_timeout(client, 15);
	response = g_string_new(NULL);
	connection = g_socket_client_connect_to_host(client, "127.0.0.1", x->port, NULL, NULL);

	if (NULL != connection)
	{
		g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),
		                          x->request, strlen(x->request), NULL, NULL, NULL);

		while ((count = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(connection)),
		                                    buffer, sizeof(buffer), NULL, NULL)) > 0)
			g_string_append_len(response, buffer, count);

		g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
	}

	x->response = g_string_free(g_steal_pointer(&response), FALSE);
	g_atomic_int_set(&x->done, 1);

	return NULL;
}

static gchar *
raw_exchange(
	Fixture		*f,
	const gchar	*request_text
){
	RawExchange x;
	GThread *thread;
	gint64 deadline;

	x.port = (guint16)venture_web_server_get_port(f->server);
	x.request = (gchar *)request_text;
	x.response = NULL;
	x.done = 0;
	deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
	thread = g_thread_new("avatar-raw-client", raw_exchange_thread, &x);

	while (!g_atomic_int_get(&x.done) && (g_get_monotonic_time() < deadline))
	{
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}

	g_assert_true(g_atomic_int_get(&x.done));
	g_thread_join(thread);

	return x.response;
}

/* A multipart body with @data as its "file" field. */
static GBytes *
multipart(
	GBytes	 *data,
	gchar	**content_type
){
	g_autoptr(SoupMultipart) parts = NULL;
	g_autoptr(SoupMessageHeaders) headers = NULL;
	GBytes *body;

	parts = soup_multipart_new("multipart/form-data");
	soup_multipart_append_form_file(parts, "file", "face.gif", "image/gif", data);
	headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);
	soup_multipart_to_message(parts, headers, &body);
	*content_type = g_strdup(soup_message_headers_get_one(headers, "Content-Type"));

	return body;
}

/*
 * Who may change whose picture, before any decoder: the person themself,
 * a global owner for anybody, nobody else; a read-only token never; no
 * credential, 401. The refusal for a GIF is the proof the authority check
 * was passed -- the request got as far as reading the file.
 */
static void
test_permissions(Fixture *f, gconstpointer data)
{
	static const gchar gif[] = "GIF89a\x10\x00\x10\x00\x80\x00\x00";
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *viewer_path = NULL;
	guint refused_kind;
	Answer answer;

	(void)data;

	body = g_bytes_new_static(gif, sizeof(gif) - 1);
	viewer_path = g_strdup_printf("/api/v1/users/%" G_GINT64_FORMAT "/avatar", f->viewer_id);
	refused_kind = venture_avatar_is_available() ? 422 : 501;

	request(f, "PUT", "/api/v1/account/avatar", NULL, "image/png", body, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 401);
	answer_clear(&answer);

	/* Yourself: through to the file, which is not a picture. */
	request(f, "PUT", "/api/v1/account/avatar", f->editor_token, "image/png", body, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, refused_kind);
	if (venture_avatar_is_available())
		g_assert_true(answer_mentions(&answer, "not a PNG, JPEG or WebP"));
	else
		g_assert_true(answer_mentions(&answer, "not available in this build"));
	answer_clear(&answer);

	/* Somebody else, as an editor: refused before the file is read. */
	request(f, "PUT", viewer_path, f->editor_token, "image/png", body, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 403);
	answer_clear(&answer);

	request(f, "DELETE", viewer_path, f->editor_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 403);
	answer_clear(&answer);

	/* Somebody else, as the owner: through to the file. */
	request(f, "PUT", viewer_path, f->owner_token, "image/png", body, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, refused_kind);
	answer_clear(&answer);

	/* A read-only token is read-only, even about its owner's own face;
	 * the viewer's token is one too. */
	request(f, "PUT", "/api/v1/account/avatar", f->readonly_token, "image/png", body, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 403);
	answer_clear(&answer);

	request(f, "DELETE", "/api/v1/account/avatar", f->viewer_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 403);
	answer_clear(&answer);

	/* Removing a picture nobody set succeeds and says there is none. */
	request(f, "DELETE", "/api/v1/account/avatar", f->editor_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	{
		g_autoptr(JsonNode) state = answer_json(&answer);

		g_assert_true(json_object_get_null_member(json_node_get_object(state), "avatar_url"));
	}
	answer_clear(&answer);

	/* An account that does not exist, to the owner. */
	request(f, "DELETE", "/api/v1/users/99999/avatar", f->owner_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 404);
	answer_clear(&answer);

	/* Sent as a browser form sends it: the same refusal. */
	{
		g_autoptr(GBytes) form = NULL;
		g_autofree gchar *type = NULL;

		form = multipart(body, &type);
		request(f, "PUT", "/api/v1/account/avatar", f->editor_token, type, form, NULL, &answer);
		g_assert_cmpuint(answer.status, ==, refused_kind);
		answer_clear(&answer);
	}

	/* Far past the cap: refused at the headers, before a byte of the body
	 * is read -- the declared length alone is enough. */
	{
		g_autofree gchar *raw = NULL;
		g_autofree gchar *reply = NULL;

		raw = g_strdup_printf("PUT /api/v1/account/avatar HTTP/1.1\r\nHost: localhost\r\n"
		                      "Authorization: Bearer %s\r\nContent-Type: image/png\r\n"
		                      "Content-Length: %d\r\nConnection: close\r\n\r\n",
		                      f->editor_token, VENTURE_AVATAR_MAX_BYTES + 512 * 1024);
		reply = raw_exchange(f, raw);
		g_assert_nonnull(strstr(reply, " 413 "));
	}
}

/*
 * Only the store writes "avatar-version": a REST client naming one would
 * point the account at files that do not exist.
 */
static void
test_version_not_writable(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *path = NULL;
	Answer answer;

	(void)data;

	user = venture_database_get(f->database, VENTURE_TYPE_USER, f->viewer_id, NULL);
	g_object_set(user, "avatar-version", "0123456789abcdef0123", NULL);
	g_assert_false(venture_database_save(f->database, user, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "uploading a picture"));
	g_clear_error(&error);

	/* Any other change to the account still saves. */
	g_clear_object(&user);
	user = venture_database_get(f->database, VENTURE_TYPE_USER, f->viewer_id, NULL);
	g_object_set(user, "timezone", "Europe/Berlin", NULL);
	g_assert_true(venture_database_save(f->database, user, NULL, &error));
	g_assert_no_error(error);

	path = g_strdup_printf("/api/v1/user/%" G_GINT64_FORMAT, f->viewer_id);
	body = g_bytes_new_static("{\"avatar_version\":\"0123456789abcdef0123\"}", 41);
	request(f, "PATCH", path, f->owner_token, "application/json", body, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 422);
	answer_clear(&answer);
}

/*
 * The whole round: the editor sets a picture through the API; it is a
 * 32 pixel PNG at its address, cached for a year with the version and
 * revalidated without; their colleague sees it, somebody from another
 * organization gets the same 404 as for nobody at all, and an unknown
 * size is not found. Removing it puts the initials back, on the comment
 * too. If this regresses, a face leaks across tenants, or a removed
 * picture lingers in somebody's thread.
 */
static void
test_round_trip(Fixture *f, gconstpointer data)
{
	g_autoptr(GBytes) wide = NULL;
	g_autoptr(JsonNode) state = NULL;
	g_autoptr(JsonNode) thread = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *stale = NULL;
	g_autofree gchar *wrong_size = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *record = NULL;
	g_autofree gchar *chunks = NULL;
	VentureAuthPrincipal editor;
	VentureEntity *comment;
	JsonObject *object;
	guint32 width = 0;
	guint32 height = 0;
	Answer answer;

	(void)data;

	if (!need_decoder())
		return;

	wide = wide_png();
	request(f, "PUT", "/api/v1/account/avatar", f->editor_token, "image/png", wide, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	state = answer_json(&answer);
	answer_clear(&answer);
	object = json_node_get_object(state);
	g_assert_cmpint(json_object_get_int_member(object, "user_id"), ==, f->editor_id);
	version = g_strdup(json_object_get_string_member(object, "avatar_version"));
	g_assert_cmpuint(strlen(version), ==, 20);
	url = g_strdup(json_object_get_string_member(object, "avatar_url"));
	g_assert_true(g_str_has_suffix(url, version));
	g_assert_true(json_object_has_member(json_object_get_object_member(object, "avatar_urls"), "192"));

	/* Only the renditions are kept: four PNGs, no upload. */
	dir = g_strdup_printf("%s/avatars/%" G_GINT64_FORMAT, f->state_dir, f->editor_id);
	{
		g_autoptr(GDir) listing = g_dir_open(dir, 0, &error);
		guint files = 0;

		g_assert_no_error(error);

		while (NULL != g_dir_read_name(listing))
			files++;

		g_assert_cmpuint(files, ==, 4);
	}

	/* The editor themself, and a colleague. */
	request(f, "GET", url, f->editor_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	g_assert_cmpstr(answer.content_type, ==, "image/png");
	g_assert_cmpstr(answer.nosniff, ==, "nosniff");
	g_assert_nonnull(strstr(answer.cache_control, "immutable"));
	g_assert_nonnull(strstr(answer.cache_control, "private"));
	g_assert_nonnull(answer.etag);
	chunks = png_chunks(answer.body, &width, &height);
	g_assert_cmpstr(chunks, ==, "IHDR IDAT IEND");
	g_assert_cmpuint(width, ==, 32);
	g_assert_cmpuint(height, ==, 32);

	{
		g_autofree gchar *etag = g_strdup(answer.etag);

		answer_clear(&answer);
		request(f, "GET", url, f->viewer_token, NULL, NULL, etag, &answer);
		g_assert_cmpuint(answer.status, ==, 304);
		answer_clear(&answer);
	}

	request(f, "GET", url, f->viewer_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	answer_clear(&answer);

	/* Another tenant, no credential, a size never kept, nobody. */
	request(f, "GET", url, f->outsider_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 404);
	answer_clear(&answer);

	request(f, "GET", url, NULL, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 401);
	answer_clear(&answer);

	wrong_size = g_strdup_printf("/avatar/%" G_GINT64_FORMAT "/50?v=%s", f->editor_id, version);
	request(f, "GET", wrong_size, f->editor_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 404);
	answer_clear(&answer);

	request(f, "GET", "/avatar/99999/32", f->owner_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 404);
	answer_clear(&answer);

	/* An address without the current version still answers, but is not
	 * cached as if it could never change. */
	stale = g_strdup_printf("/avatar/%" G_GINT64_FORMAT "/192?v=old", f->editor_id);
	request(f, "GET", stale, f->viewer_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	g_assert_nonnull(strstr(answer.cache_control, "no-cache"));
	answer_clear(&answer);

	/* The editor comments: the thread carries the picture for a colleague
	 * and not for the outsider, and the page draws it. */
	editor.user_id = f->editor_id;
	editor.token_id = 0;
	editor.role = VENTURE_USER_ROLE_EDITOR;
	editor.name = (gchar *)"erin";
	editor.authenticated = TRUE;
	editor.provider_token = FALSE;
	comment = venture_comment_create(f->context, &editor, "company",
		venture_entity_get_id(f->company), 0, "A picture now.", &error);
	g_assert_no_error(error);
	g_clear_object(&comment);

	{
		VentureAuthPrincipal viewer = editor;
		JsonObject *first;

		viewer.user_id = f->viewer_id;
		viewer.role = VENTURE_USER_ROLE_VIEWER;
		viewer.name = (gchar *)"vic";
		thread = venture_comment_thread(f->context, &viewer, "company",
			venture_entity_get_id(f->company), &error);
		g_assert_no_error(error);
		first = json_array_get_object_element(json_node_get_array(thread), 0);
		g_assert_cmpstr(json_object_get_string_member(first, "avatar_url"), ==, url);
		g_assert_cmpstr(json_object_get_string_member(first, "author_initials"), ==, "EE");
	}

	record = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, venture_entity_get_id(f->company));
	request(f, "GET", record, f->viewer_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	{
		g_autofree gchar *img = g_strdup_printf("<img class=\"avatar avatar-32\" src=\"/avatar/%"
			G_GINT64_FORMAT "/32?v=%s\"", f->editor_id, version);

		g_assert_true(answer_mentions(&answer, img));
	}
	answer_clear(&answer);

	/* Removed: the address is gone, the files are gone, the thread and
	 * the page draw the initials. */
	request(f, "DELETE", "/api/v1/account/avatar", f->editor_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	answer_clear(&answer);

	request(f, "GET", url, f->editor_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 404);
	answer_clear(&answer);
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));

	request(f, "GET", record, f->viewer_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	g_assert_false(answer_mentions(&answer, "<img class=\"avatar"));
	g_assert_true(answer_mentions(&answer,
		"<span class=\"avatar avatar-32 avatar-initials\" aria-hidden=\"true\">EE</span>"));
	answer_clear(&answer);
}

/*
 * The owner sets somebody else's picture from the users API; a second
 * picture replaces the first, whose files go, so a replaced face is not
 * left on disk.
 */
static void
test_replace(Fixture *f, gconstpointer data)
{
	g_autoptr(GBytes) wide = NULL;
	g_autoptr(GBytes) jpeg = NULL;
	g_autoptr(JsonNode) first = NULL;
	g_autoptr(JsonNode) second = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *old_file = NULL;
	const gchar *old_version;
	const gchar *new_version;
	Answer answer;

	(void)data;

	if (!need_decoder())
		return;

	path = g_strdup_printf("/api/v1/users/%" G_GINT64_FORMAT "/avatar", f->viewer_id);
	wide = wide_png();
	request(f, "PUT", path, f->owner_token, "application/octet-stream", wide, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	first = answer_json(&answer);
	answer_clear(&answer);
	old_version = json_object_get_string_member(json_node_get_object(first), "avatar_version");
	old_file = g_strdup_printf("%s/avatars/%" G_GINT64_FORMAT "/%s-96.png",
	                           f->state_dir, f->viewer_id, old_version);
	g_assert_true(g_file_test(old_file, G_FILE_TEST_IS_REGULAR));

	jpeg = g_bytes_new_static(fixture_jpeg, sizeof(fixture_jpeg));
	request(f, "PUT", path, f->owner_token, "image/jpeg", jpeg, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	second = answer_json(&answer);
	answer_clear(&answer);
	new_version = json_object_get_string_member(json_node_get_object(second), "avatar_version");
	g_assert_cmpstr(new_version, !=, old_version);
	g_assert_false(g_file_test(old_file, G_FILE_TEST_EXISTS));
}

/*
 * The account page offers the picture forms, and the menu shows the
 * person's badge; a page form's refusal comes back as a notice.
 */
static void
test_account_page(Fixture *f, gconstpointer data)
{
	static const gchar svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
	g_autoptr(GBytes) form = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *type = NULL;
	Answer answer;

	(void)data;

	request(f, "GET", "/account", f->editor_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	g_assert_true(answer_mentions(&answer, "<h2>Picture</h2>"));
	g_assert_true(answer_mentions(&answer, "<a class=\"sidebar-user-avatar\" href=\"/account\""));
	g_assert_true(answer_mentions(&answer, "avatar-96 avatar-initials\" aria-hidden=\"true\">EE</span>"));

	if (venture_avatar_is_available())
		g_assert_true(answer_mentions(&answer, "action=\"/account/avatar\""));
	else
		g_assert_true(answer_mentions(&answer, "built without image support"));

	answer_clear(&answer);

	body = g_bytes_new_static(svg, sizeof(svg) - 1);
	form = multipart(body, &type);
	request(f, "POST", "/account/avatar", f->editor_token, type, form, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 302);
	g_assert_cmpstr(answer.location, ==, venture_avatar_is_available()
		? "/account?notice=avatar-not-image" : "/account?notice=avatar-unavailable");
	answer_clear(&answer);

	/* The users page is the owner's, with a picture column. */
	request(f, "GET", "/users", f->owner_token, NULL, NULL, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 200);
	g_assert_true(answer_mentions(&answer, "<th>Picture</th>"));
	answer_clear(&answer);

	request(f, "POST", "/users/1/avatar", f->editor_token, type, form, NULL, &answer);
	g_assert_cmpuint(answer.status, ==, 403);
	answer_clear(&answer);
}

#define AVATAR_TEST(path, func) \
	g_test_add("/avatars/" path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/avatars/sniff", test_sniff);
	g_test_add_func("/avatars/size-cap", test_size_cap);
	g_test_add_func("/avatars/pixel-cap", test_pixel_cap);
	g_test_add_func("/avatars/png-encode", test_png_encode);
	g_test_add_func("/avatars/initials", test_initials);
	g_test_add_func("/avatars/badge-markup", test_badge_markup);
	g_test_add_func("/avatars/render", test_render);
	AVATAR_TEST("permissions", test_permissions);
	AVATAR_TEST("version-not-writable", test_version_not_writable);
	AVATAR_TEST("round-trip", test_round_trip);
	AVATAR_TEST("replace", test_replace);
	AVATAR_TEST("account-page", test_account_page);

	return g_test_run();
}
