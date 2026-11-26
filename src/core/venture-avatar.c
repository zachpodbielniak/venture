/*
 * venture-avatar.c - Profile pictures for user accounts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Why a store of its own, not a document or a hosted image. Both of those
 * keep a file as an *organization's* record, and a user is not one
 * organization's: the same person works in several, and there is no right
 * one to file their face under. A document is also something the
 * Documents pages, search, OCR and the generic attachment route all read
 * by the document's own rules, so a picture filed as one would be served
 * by a second rule beside /avatar's; deleting a document keeps its bytes,
 * and removing a picture must not; and four renditions would be four
 * documents per person. A hosted image is public by design (a mail app
 * fetches it with no credential); a picture is shown only to people who
 * may see the name beside it. What is shared is the shape of the store:
 * files under the state directory, a 0700 directory and 0600 files,
 * written whole, read back through openat() with O_NOFOLLOW at every
 * step, and the magic-number sniffer the hosted image store already has.
 *
 * Why this file writes PNG itself. GdkPixbuf on Fedora 44 hands every
 * load *and save* to glycin, which runs the codec in a bwrap sandbox; a
 * container without user namespaces cannot start it. Decoding needs a
 * decoder, so that stays GdkPixbuf's (and a container that cannot run
 * glycin refuses uploads with glycin's own message); encoding a PNG of
 * known pixels needs only zlib, which GLib already carries. It also makes
 * "no metadata" true by construction: there is nowhere for it to go.
 */

#include "venture.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>

#ifdef VENTURE_HAVE_GDK_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

/* Set on the database, naming the user row the store is writing, for
 * exactly as long as that one save runs: the validator's way of telling
 * the store from every other writer. */
#define VENTURE_AVATAR_FILING_KEY "venture-avatar-filing"

/* Hex digits of the SHA-256 kept as a version: 80 bits, plenty to tell
 * one person's pictures apart, short enough for an address. */
#define VENTURE_AVATAR_VERSION_LENGTH (20)

/* The PNG encoder's own bound on a side; nothing it is handed comes close
 * (the largest rendition is 192), it only keeps the arithmetic small. */
#define VENTURE_AVATAR_PNG_MAX_SIDE (16384)

static const guint venture_avatar_size_table[] = { 32, 64, 96, 192 };

const guint *
venture_avatar_sizes(guint *n_sizes)
{
	if (NULL != n_sizes)
		*n_sizes = G_N_ELEMENTS(venture_avatar_size_table);

	return venture_avatar_size_table;
}

gboolean
venture_avatar_size_is_valid(guint size)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(venture_avatar_size_table); i++)
	{
		if (venture_avatar_size_table[i] == size)
			return TRUE;
	}

	return FALSE;
}

gboolean
venture_avatar_is_available(void)
{
#ifdef VENTURE_HAVE_GDK_PIXBUF
	return TRUE;
#else
	return FALSE;
#endif
}

/* --- Before any decoder ---------------------------------------------------- */

static guint32
venture_avatar_le32(const guint8 *p)
{
	return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) |
	       ((guint32)p[3] << 24);
}

static guint32
venture_avatar_le24(const guint8 *p)
{
	return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16);
}

static guint32
venture_avatar_le16(const guint8 *p)
{
	return (guint32)p[0] | ((guint32)p[1] << 8);
}

/*
 * The size of the one frame a WebP file holds, read from the bitstream
 * itself rather than from the VP8X canvas in front of it.
 *
 * Why. The extended format's VP8X chunk states a canvas size, and that is
 * all the shared sniffer reads; the decoder believes the VP8 or VP8L
 * bitstream instead, and only compares the two after it has decoded the
 * whole frame. So a 64 by 64 canvas around a 16383 by 16383 lossy frame
 * passed the pixel cap and cost seconds of decoding before the decoder
 * refused it. Here every chunk is walked (RIFF: a fourcc, a little-endian
 * length, the payload padded to an even length), and the file is refused
 * unless it holds exactly one still frame whose bitstream size is the size
 * the canvas claims. Anything malformed is refused: fail closed.
 *
 * Offsets, from the WebP container and VP8/VP8L bitstream specifications,
 * relative to a chunk's payload:
 *   VP8X  0: flags (0x02 animation), 1-3 reserved, 4-6 canvas width - 1,
 *         7-9 canvas height - 1 (24-bit little-endian each)
 *   "VP8 " 0-2: frame tag (bit 0 clear on a key frame), 3-5: start code
 *         9d 01 2a, 6-7: width, 8-9: height (14 bits each, 2 scale bits)
 *   VP8L  0: signature 0x2f, 1-4: 14 bits width - 1, 14 bits height - 1,
 *         1 bit alpha, 3 bits version (must be 0)
 */
static gboolean
venture_avatar_webp_frame_size(
	const guint8	 *p,
	gsize		  size,
	gint		 *width,
	gint		 *height,
	GError		**error
){
	gsize end;
	gsize offset;
	guint32 riff_size;
	gboolean extended;
	gboolean found;
	gint canvas_width;
	gint canvas_height;
	gint frame_width;
	gint frame_height;

	if ((size < 12) || (0 != memcmp(p, "RIFF", 4)) || (0 != memcmp(p + 8, "WEBP", 4)))
	{
		venture_set_error_validation(error, "Picture", "it is not a WebP file");
		return FALSE;
	}

	/* The RIFF length covers everything after its own eight bytes. Bytes
	 * past it are not part of the file (the decoder ignores them too);
	 * a length past the end means the file was cut short. */
	riff_size = venture_avatar_le32(p + 4);

	if ((riff_size < 4) || ((gsize)riff_size > size - 8))
	{
		venture_set_error_validation(error, "Picture",
			"it is a WebP file cut short: its header says %u bytes follow, "
			"and %" G_GSIZE_FORMAT " do", riff_size, size - 8);
		return FALSE;
	}

	end = 8 + (gsize)riff_size;
	offset = 12;
	extended = FALSE;
	found = FALSE;
	canvas_width = 0;
	canvas_height = 0;
	frame_width = 0;
	frame_height = 0;

	while (offset < end)
	{
		const guint8 *chunk;
		const guint8 *payload;
		gsize length;
		gsize padded;

		/* Every comparison is against what is left, never a sum that
		 * could wrap: a hostile length is up to 4 GB. */
		if (end - offset < 8)
		{
			venture_set_error_validation(error, "Picture",
				"it is a WebP file with a chunk header cut short");
			return FALSE;
		}

		chunk = p + offset;
		payload = chunk + 8;
		length = (gsize)venture_avatar_le32(chunk + 4);

		if (length > end - offset - 8)
		{
			venture_set_error_validation(error, "Picture",
				"it is a WebP file with a chunk that runs past its end");
			return FALSE;
		}

		/* Padded to an even length; a writer that left the pad byte off
		 * the very last chunk is let off, as the decoder lets it off. */
		padded = length + (length & 1);

		if (padded > end - offset - 8)
			padded = length;

		/* The first chunk says which layout this is: VP8X for the
		 * extended format, or the bitstream itself for the simple one. */
		if ((12 == offset) &&
		    (0 != memcmp(chunk, "VP8X", 4)) &&
		    (0 != memcmp(chunk, "VP8 ", 4)) &&
		    (0 != memcmp(chunk, "VP8L", 4)))
		{
			venture_set_error_validation(error, "Picture",
				"it is a WebP file that does not start with a picture");
			return FALSE;
		}

		if (0 == memcmp(chunk, "VP8X", 4))
		{
			if ((12 != offset) || (length < 10))
			{
				venture_set_error_validation(error, "Picture",
					"it is a WebP file with a misplaced or short VP8X header");
				return FALSE;
			}

			if (0 != (payload[0] & 0x02))
			{
				venture_set_error_validation(error, "Picture",
					"it is an animated WebP; a picture of a person is one still "
					"frame");
				return FALSE;
			}

			extended = TRUE;
			canvas_width = (gint)venture_avatar_le24(payload + 4) + 1;
			canvas_height = (gint)venture_avatar_le24(payload + 7) + 1;
		}
		else if ((0 == memcmp(chunk, "ANIM", 4)) || (0 == memcmp(chunk, "ANMF", 4)))
		{
			venture_set_error_validation(error, "Picture",
				"it is an animated WebP; a picture of a person is one still "
				"frame");
			return FALSE;
		}
		else if ((0 == memcmp(chunk, "VP8 ", 4)) || (0 == memcmp(chunk, "VP8L", 4)))
		{
			gboolean lossless;

			lossless = (0 == memcmp(chunk, "VP8L", 4));

			if (found)
			{
				venture_set_error_validation(error, "Picture",
					"it is a WebP file with more than one frame; a picture of a "
					"person is one still frame");
				return FALSE;
			}

			if (lossless)
			{
				guint32 bits;

				if ((length < 5) || (0x2f != payload[0]))
				{
					venture_set_error_validation(error, "Picture",
						"it is a WebP file whose lossless frame header is "
						"damaged");
					return FALSE;
				}

				bits = venture_avatar_le32(payload + 1);

				if (0 != (bits >> 29))
				{
					venture_set_error_validation(error, "Picture",
						"it is a WebP file in an unknown lossless version");
					return FALSE;
				}

				frame_width = (gint)(bits & 0x3fff) + 1;
				frame_height = (gint)((bits >> 14) & 0x3fff) + 1;
			}
			else
			{
				/* Only a key frame carries the start code and the size;
				 * the first frame of a still picture is always one. */
				if ((length < 10) || (0 != (payload[0] & 0x01)) ||
				    (0x9d != payload[3]) || (0x01 != payload[4]) ||
				    (0x2a != payload[5]))
				{
					venture_set_error_validation(error, "Picture",
						"it is a WebP file whose lossy frame header is "
						"damaged");
					return FALSE;
				}

				frame_width = (gint)(venture_avatar_le16(payload + 6) & 0x3fff);
				frame_height = (gint)(venture_avatar_le16(payload + 8) & 0x3fff);
			}

			found = TRUE;
		}

		offset += 8 + padded;
	}

	if (!found || (frame_width <= 0) || (frame_height <= 0))
	{
		venture_set_error_validation(error, "Picture",
			"it is a WebP file with no picture in it");
		return FALSE;
	}

	if (extended &&
	    ((frame_width != canvas_width) || (frame_height != canvas_height)))
	{
		venture_set_error_validation(error, "Picture",
			"it is a WebP file whose header says %d by %d pixels and whose "
			"picture is %d by %d; the two must agree",
			canvas_width, canvas_height, frame_width, frame_height);
		return FALSE;
	}

	*width = frame_width;
	*height = frame_height;

	return TRUE;
}

/*
 * venture_avatar_sniff() with the reason a WebP file was refused, as a
 * %VENTURE_ERROR_VALIDATION in @error. Bytes that are not a picture at all
 * leave @error unset; the caller names that refusal.
 */
static const gchar *
venture_avatar_sniff_checked(
	GBytes	 *data,
	gint	 *width,
	gint	 *height,
	GError	**error
){
	const gchar *type;
	gint w;
	gint h;

	w = 0;
	h = 0;

	/* The hosted image sniffer already reads PNG, JPEG, WebP and GIF
	 * headers, dimensions included; a picture of a person is never a GIF
	 * (an animation, and a format with a history of decoder bugs).
	 *
	 * For PNG and JPEG the size it reads is the one the decoder uses: a
	 * PNG's IHDR must be its first chunk (the sniffer insists on offset
	 * 12) and a decoder refuses a second; a JPEG's size is its first
	 * frame header, which is the decoder's too, and a second one makes
	 * it fail before any scan is decoded. WebP is the format where the
	 * header read there (the VP8X canvas) is not what the decoder
	 * believes, so it is walked again here, to the bitstream. */
	type = venture_hosted_image_sniff(data, &w, &h, NULL);

	if ((NULL == type) || (0 == g_strcmp0(type, "image/gif")))
		return NULL;

	if (0 == g_strcmp0(type, "image/webp"))
	{
		gsize size;
		const guint8 *p;

		p = g_bytes_get_data(data, &size);

		if (!venture_avatar_webp_frame_size(p, size, &w, &h, error))
			return NULL;
	}

	if (NULL != width)
		*width = w;

	if (NULL != height)
		*height = h;

	return type;
}

const gchar *
venture_avatar_sniff(
	GBytes	*data,
	gint	*width,
	gint	*height
){
	return venture_avatar_sniff_checked(data, width, height, NULL);
}

const gchar *
venture_avatar_check_upload(
	GBytes	 *data,
	GError	**error
){
	g_autoptr(GError) local = NULL;
	const gchar *type;
	gsize size;
	gint width;
	gint height;

	size = (NULL != data) ? g_bytes_get_size(data) : 0;

	if (0 == size)
	{
		venture_set_error_validation(error, "Picture", "no file arrived");
		return NULL;
	}

	if (size > VENTURE_AVATAR_MAX_BYTES)
	{
		venture_set_error_validation(error, "Picture",
			"it is %" G_GSIZE_FORMAT " bytes; a picture may be at most %d MB",
			size, VENTURE_AVATAR_MAX_BYTES / (1024 * 1024));
		return NULL;
	}

	width = 0;
	height = 0;
	type = venture_avatar_sniff_checked(data, &width, &height, &local);

	/* A WebP file refused for what is inside it says why. */
	if ((NULL == type) && (NULL != local))
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}

	if (NULL == type)
	{
		venture_set_error_validation(error, "Picture",
			"it is not a PNG, JPEG or WebP picture, or its header is cut "
			"short; the file itself is read, not its name or declared type, "
			"and SVG, GIF, BMP, TIFF and icon files are not accepted");
		return NULL;
	}

	if ((width > VENTURE_AVATAR_MAX_SIDE) || (height > VENTURE_AVATAR_MAX_SIDE))
	{
		venture_set_error_validation(error, "Picture",
			"it claims to be %d by %d pixels; at most %d on a side",
			width, height, VENTURE_AVATAR_MAX_SIDE);
		return NULL;
	}

	return type;
}

/* --- Writing PNG ----------------------------------------------------------- */

/* The PNG (ISO 3309) CRC-32, table-driven; built once. */
static guint32 venture_avatar_crc_table[256];

static void
venture_avatar_crc_init(void)
{
	static gsize ready = 0;

	if (g_once_init_enter(&ready))
	{
		guint32 n;

		for (n = 0; n < 256; n++)
		{
			guint32 c;
			guint k;

			c = n;

			for (k = 0; k < 8; k++)
				c = (c & 1) ? (0xedb88320U ^ (c >> 1)) : (c >> 1);

			venture_avatar_crc_table[n] = c;
		}

		g_once_init_leave(&ready, 1);
	}
}

static guint32
venture_avatar_crc_update(
	guint32		 crc,
	const guint8	*data,
	gsize		 length
){
	gsize i;

	for (i = 0; i < length; i++)
		crc = venture_avatar_crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);

	return crc;
}

static void
venture_avatar_put_be32(
	GByteArray	*out,
	guint32		 value
){
	guint8 bytes[4];

	bytes[0] = (guint8)(value >> 24);
	bytes[1] = (guint8)(value >> 16);
	bytes[2] = (guint8)(value >> 8);
	bytes[3] = (guint8)value;
	g_byte_array_append(out, bytes, 4);
}

/* One chunk: length, type, data, and the CRC over type and data. */
static void
venture_avatar_put_chunk(
	GByteArray	*out,
	const gchar	*type,
	const guint8	*data,
	gsize		 length
){
	guint32 crc;

	venture_avatar_put_be32(out, (guint32)length);
	g_byte_array_append(out, (const guint8 *)type, 4);

	if (length > 0)
		g_byte_array_append(out, data, (guint)length);

	crc = venture_avatar_crc_update(0xffffffffU, (const guint8 *)type, 4);
	crc = venture_avatar_crc_update(crc, data, length);
	venture_avatar_put_be32(out, crc ^ 0xffffffffU);
}

/* @data as one zlib stream, as an IDAT wants it. */
static GBytes *
venture_avatar_deflate(
	const guint8	 *data,
	gsize		  length,
	GError		**error
){
	g_autoptr(GZlibCompressor) compressor = NULL;
	g_autoptr(GByteArray) out = NULL;
	GConverterResult result;
	guint8 buffer[16384];
	gsize consumed;

	compressor = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 9);
	out = g_byte_array_new();
	consumed = 0;

	do
	{
		gsize bytes_read;
		gsize bytes_written;

		bytes_read = 0;
		bytes_written = 0;
		result = g_converter_convert(G_CONVERTER(compressor),
			data + consumed, length - consumed, buffer, sizeof(buffer),
			G_CONVERTER_INPUT_AT_END, &bytes_read, &bytes_written, error);

		if (G_CONVERTER_ERROR == result)
		{
			g_prefix_error(error, "Cannot compress the picture: ");
			return NULL;
		}

		consumed += bytes_read;
		g_byte_array_append(out, buffer, (guint)bytes_written);
	}
	while (G_CONVERTER_FINISHED != result);

	return g_byte_array_free_to_bytes(g_steal_pointer(&out));
}

GBytes *
venture_avatar_png_encode(
	const guint8	 *pixels,
	gint		  width,
	gint		  height,
	gint		  rowstride,
	gboolean	  has_alpha,
	GError		**error
){
	g_autoptr(GByteArray) raw = NULL;
	g_autoptr(GByteArray) png = NULL;
	g_autoptr(GBytes) compressed = NULL;
	guint8 header[13];
	gsize row_bytes;
	gint channels;
	gint y;

	g_return_val_if_fail(NULL != pixels, NULL);

	channels = has_alpha ? 4 : 3;

	if ((width < 1) || (height < 1) ||
	    (width > VENTURE_AVATAR_PNG_MAX_SIDE) ||
	    (height > VENTURE_AVATAR_PNG_MAX_SIDE) ||
	    (rowstride < width * channels))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Cannot write a %d by %d PNG with %d bytes a row",
		            width, height, rowstride);
		return NULL;
	}

	venture_avatar_crc_init();

	/* The image data: each row led by filter type 0 (none). Small
	 * pictures gain little from the adaptive filters, and "none" keeps
	 * the encoder trivially correct. */
	row_bytes = (gsize)width * (gsize)channels;
	raw = g_byte_array_sized_new((guint)((row_bytes + 1) * (gsize)height));

	for (y = 0; y < height; y++)
	{
		static const guint8 filter_none = 0;

		g_byte_array_append(raw, &filter_none, 1);
		g_byte_array_append(raw, pixels + ((gsize)y * (gsize)rowstride),
		                    (guint)row_bytes);
	}

	compressed = venture_avatar_deflate(raw->data, raw->len, error);

	if (NULL == compressed)
		return NULL;

	/* IHDR: size, 8 bits a sample, truecolour with or without alpha,
	 * deflate, adaptive filtering, no interlace. */
	header[0] = (guint8)((guint32)width >> 24);
	header[1] = (guint8)((guint32)width >> 16);
	header[2] = (guint8)((guint32)width >> 8);
	header[3] = (guint8)width;
	header[4] = (guint8)((guint32)height >> 24);
	header[5] = (guint8)((guint32)height >> 16);
	header[6] = (guint8)((guint32)height >> 8);
	header[7] = (guint8)height;
	header[8] = 8;
	header[9] = has_alpha ? 6 : 2;
	header[10] = 0;
	header[11] = 0;
	header[12] = 0;

	png = g_byte_array_new();
	g_byte_array_append(png, (const guint8 *)"\x89PNG\r\n\x1a\n", 8);
	venture_avatar_put_chunk(png, "IHDR", header, sizeof(header));
	venture_avatar_put_chunk(png, "IDAT",
		g_bytes_get_data(compressed, NULL), g_bytes_get_size(compressed));
	venture_avatar_put_chunk(png, "IEND", NULL, 0);

	return g_byte_array_free_to_bytes(g_steal_pointer(&png));
}

/* --- Decoding and the renditions ------------------------------------------ */

#ifdef VENTURE_HAVE_GDK_PIXBUF

typedef struct
{
	gboolean	refused;
	gint		width;
	gint		height;
} VentureAvatarSizeCheck;

/*
 * The decoder's own word on the size, which the header check already
 * bounded. With a classic in-process loader, a size of zero here tells the
 * module to stop before it allocates (the convention
 * gdk_pixbuf_get_file_info() relies on). GdkPixbuf on glycin decodes in a
 * memory-limited sandbox first and says the size after, so there it is a
 * second opinion, not the fence: the header check is.
 */
static void
venture_avatar_size_prepared(
	GdkPixbufLoader	*loader,
	gint		 width,
	gint		 height,
	gpointer	 data
){
	VentureAvatarSizeCheck *check;

	check = data;
	check->width = width;
	check->height = height;

	if ((width <= 0) || (height <= 0) ||
	    (width > VENTURE_AVATAR_MAX_SIDE) || (height > VENTURE_AVATAR_MAX_SIDE))
	{
		check->refused = TRUE;
		gdk_pixbuf_loader_set_size(loader, 0, 0);
	}
}

/* The upload as pixels, through a loader for the sniffed type only. */
static GdkPixbuf *
venture_avatar_decode(
	GBytes		 *data,
	const gchar	 *type,
	GError		**error
){
	g_autoptr(GdkPixbufLoader) loader = NULL;
	g_autoptr(GError) local = NULL;
	VentureAvatarSizeCheck check;
	GdkPixbuf *pixbuf;
	gboolean written;
	gboolean closed;

	loader = gdk_pixbuf_loader_new_with_mime_type(type, &local);

	if (NULL == loader)
	{
		if (0 == g_strcmp0(type, "image/webp"))
			venture_set_error_validation(error, "Picture",
				"it is a WebP picture, and this server has no WebP decoder "
				"(%s); upload a PNG or JPEG instead",
				(NULL != local) ? local->message : "no loader");
		else
			venture_set_error_validation(error, "Picture",
				"this server has no decoder for %s pictures (%s)", type,
				(NULL != local) ? local->message : "no loader");

		return NULL;
	}

	check.refused = FALSE;
	check.width = 0;
	check.height = 0;
	g_signal_connect(loader, "size-prepared",
	                 G_CALLBACK(venture_avatar_size_prepared), &check);

	written = gdk_pixbuf_loader_write_bytes(loader, data, &local);

	/* Closed whatever happened: a loader dropped while open complains,
	 * and the module must let go of what it holds. Its error is wanted
	 * only when the write itself went through. */
	closed = gdk_pixbuf_loader_close(loader, written ? &local : NULL);

	if (check.refused)
	{
		venture_set_error_validation(error, "Picture",
			"it is %d by %d pixels; at most %d on a side",
			check.width, check.height, VENTURE_AVATAR_MAX_SIDE);
		return NULL;
	}

	if (!written || !closed)
	{
		venture_set_error_validation(error, "Picture",
			"it could not be decoded as %s: %s", type,
			(NULL != local) ? local->message : "the decoder gave no reason");
		return NULL;
	}

	pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);

	if ((NULL == pixbuf) ||
	    (gdk_pixbuf_get_width(pixbuf) > VENTURE_AVATAR_MAX_SIDE) ||
	    (gdk_pixbuf_get_height(pixbuf) > VENTURE_AVATAR_MAX_SIDE))
	{
		venture_set_error_validation(error, "Picture",
			"it could not be decoded as %s: the decoder produced no image "
			"within %d pixels a side", type, VENTURE_AVATAR_MAX_SIDE);
		return NULL;
	}

	/* A phone's JPEG is often stored sideways with an EXIF note saying
	 * so; the note is about to be dropped, so honour it first. */
	return gdk_pixbuf_apply_embedded_orientation(pixbuf);
}

/* One decoded square, scaled and written as PNG. */
static GBytes *
venture_avatar_rendition(
	GdkPixbuf	 *square,
	guint		  size,
	GError		**error
){
	g_autoptr(GdkPixbuf) scaled = NULL;

	scaled = gdk_pixbuf_scale_simple(square, (gint)size, (gint)size,
	                                 GDK_INTERP_BILINEAR);

	if ((NULL == scaled) ||
	    (8 != gdk_pixbuf_get_bits_per_sample(scaled)) ||
	    (GDK_COLORSPACE_RGB != gdk_pixbuf_get_colorspace(scaled)) ||
	    (gdk_pixbuf_get_n_channels(scaled) !=
	     (gdk_pixbuf_get_has_alpha(scaled) ? 4 : 3)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot scale the picture to %u pixels", size);
		return NULL;
	}

	return venture_avatar_png_encode(gdk_pixbuf_read_pixels(scaled),
		gdk_pixbuf_get_width(scaled), gdk_pixbuf_get_height(scaled),
		gdk_pixbuf_get_rowstride(scaled), gdk_pixbuf_get_has_alpha(scaled),
		error);
}

/*
 * The decode, the crop and every rendition, for bytes that passed
 * venture_avatar_check_upload() as @type, by a caller holding the slot.
 * Touches nothing shared, so it runs on any thread.
 */
static GPtrArray *
venture_avatar_render_unlocked(
	GBytes		 *data,
	const gchar	 *type,
	GError		**error
){
	g_autoptr(GdkPixbuf) decoded = NULL;
	g_autoptr(GdkPixbuf) square = NULL;
	g_autoptr(GPtrArray) renditions = NULL;
	gint width;
	gint height;
	gint side;
	guint i;

	decoded = venture_avatar_decode(data, type, error);

	if (NULL == decoded)
		return NULL;

	/* The middle square: a portrait loses its top and bottom equally, a
	 * landscape its sides. */
	width = gdk_pixbuf_get_width(decoded);
	height = gdk_pixbuf_get_height(decoded);
	side = MIN(width, height);
	square = gdk_pixbuf_new_subpixbuf(decoded, (width - side) / 2,
	                                  (height - side) / 2, side, side);

	renditions = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);

	for (i = 0; i < G_N_ELEMENTS(venture_avatar_size_table); i++)
	{
		GBytes *png;

		png = venture_avatar_rendition(square, venture_avatar_size_table[i],
		                               error);

		if (NULL == png)
			return NULL;

		g_ptr_array_add(renditions, png);
	}

	return g_steal_pointer(&renditions);
}

#endif /* VENTURE_HAVE_GDK_PIXBUF */

/* --- The decode slot ------------------------------------------------------- */

/*
 * One decode at a time, process-wide. An honest 4096-pixel PNG costs a
 * fifth of a second and a hundred megabytes to decode and scale; a queue
 * of them is a queue of that, so a second upload is told to come back
 * rather than made to wait. 0 free, 1 held; changed only atomically,
 * because the worker gives it back.
 */
static gint venture_avatar_slot_held = 0;

gboolean
venture_avatar_render_slot_acquire(void)
{
	return g_atomic_int_compare_and_exchange(&venture_avatar_slot_held, 0, 1);
}

void
venture_avatar_render_slot_release(void)
{
	g_atomic_int_set(&venture_avatar_slot_held, 0);
}

/* The refusal for a second render while one is running. */
static void
venture_avatar_set_error_busy(GError **error)
{
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
		"Another picture is being processed; try again in a few seconds");
}

#ifndef VENTURE_HAVE_GDK_PIXBUF
static void
venture_avatar_set_error_unsupported(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		"Profile pictures are not available in this build: the server was "
		"built without GdkPixbuf (gdk-pixbuf2-devel), so everybody is drawn "
		"by their initials");
}
#endif

GPtrArray *
venture_avatar_render(
	GBytes	 *data,
	GError	**error
){
#ifdef VENTURE_HAVE_GDK_PIXBUF
	GPtrArray *renditions;
	const gchar *type;

	/* Every check that needs no decoder runs before one is made, and
	 * before the slot: a bad file is told so even while it is busy. */
	type = venture_avatar_check_upload(data, error);

	if (NULL == type)
		return NULL;

	if (!venture_avatar_render_slot_acquire())
	{
		venture_avatar_set_error_busy(error);
		return NULL;
	}

	renditions = venture_avatar_render_unlocked(data, type, error);
	venture_avatar_render_slot_release();

	return renditions;
#else
	(void)data;
	venture_avatar_set_error_unsupported(error);
	return NULL;
#endif
}

#ifdef VENTURE_HAVE_GDK_PIXBUF

/* What a render worker is handed: the bytes and their sniffed type. */
typedef struct
{
	GBytes		*data;
	const gchar	*type;		/* a static string from the sniffer */
} VentureAvatarRenderJob;

static void
venture_avatar_render_job_free(gpointer data)
{
	VentureAvatarRenderJob *job;

	job = data;
	g_clear_pointer(&job->data, g_bytes_unref);
	g_free(job);
}

/*
 * On a GTask worker, holding the slot venture_avatar_render_async() took
 * for it, and giving it back the moment the pixels are done with.
 */
static void
venture_avatar_render_thread(
	GTask		*task,
	gpointer	 source,
	gpointer	 task_data,
	GCancellable	*cancellable
){
	VentureAvatarRenderJob *job;
	GPtrArray *renditions;
	GError *error;

	(void)source;
	(void)cancellable;

	job = task_data;
	error = NULL;
	renditions = venture_avatar_render_unlocked(job->data, job->type, &error);
	venture_avatar_render_slot_release();

	if (NULL == renditions)
		g_task_return_error(task, error);
	else
		g_task_return_pointer(task, renditions,
		                      (GDestroyNotify)g_ptr_array_unref);
}

#endif /* VENTURE_HAVE_GDK_PIXBUF */

void
venture_avatar_render_async(
	GBytes			*data,
	GCancellable		*cancellable,
	GAsyncReadyCallback	 callback,
	gpointer		 user_data
){
	g_autoptr(GTask) task = NULL;
	GError *error;

	task = g_task_new(NULL, cancellable, callback, user_data);
	g_task_set_source_tag(task, venture_avatar_render_async);
	g_task_set_static_name(task, "venture-avatar-render");
	error = NULL;

#ifdef VENTURE_HAVE_GDK_PIXBUF
	{
		VentureAvatarRenderJob *job;
		const gchar *type;

		/* The cheap refusals here, on the caller's thread: they need no
		 * worker and no slot. */
		type = venture_avatar_check_upload(data, &error);

		if (NULL == type)
		{
			g_task_return_error(task, error);
			return;
		}

		/* Taken here, not on the worker, so the very next request on
		 * this thread already finds it held. */
		if (!venture_avatar_render_slot_acquire())
		{
			venture_avatar_set_error_busy(&error);
			g_task_return_error(task, error);
			return;
		}

		job = g_new0(VentureAvatarRenderJob, 1);
		job->data = g_bytes_ref(data);
		job->type = type;
		g_task_set_task_data(task, job, venture_avatar_render_job_free);
		g_task_run_in_thread(task, venture_avatar_render_thread);
	}
#else
	(void)data;
	venture_avatar_set_error_unsupported(&error);
	g_task_return_error(task, error);
#endif
}

GPtrArray *
venture_avatar_render_finish(
	GAsyncResult	 *result,
	GError		**error
){
	g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
	g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) ==
	                     venture_avatar_render_async, NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* --- Names and badges ------------------------------------------------------ */

gchar *
venture_avatar_initials(const gchar *name)
{
	g_autoptr(GString) out = NULL;
	const gchar *p;
	gboolean word_start;

	out = g_string_new(NULL);
	word_start = TRUE;

	/* The first letter or digit of each of the first two words. */
	for (p = name; (NULL != p) && ('\0' != *p) && (g_utf8_strlen(out->str, -1) < 2);
	     p = g_utf8_next_char(p))
	{
		gunichar c = g_utf8_get_char(p);

		if (!g_unichar_isalnum(c))
		{
			word_start = TRUE;
			continue;
		}

		if (word_start)
			g_string_append_unichar(out, g_unichar_toupper(c));

		word_start = FALSE;
	}

	if (0 == out->len)
		g_string_append_c(out, '?');

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gchar *
venture_avatar_url(
	gint64		 user_id,
	const gchar	*version,
	guint		 size
){
	return g_strdup_printf("/avatar/%" G_GINT64_FORMAT "/%u?v=%s", user_id,
	                       size, (NULL != version) ? version : "");
}

gchar *
venture_avatar_url_for(
	VentureActorNames	*viewer,
	gint64			 user_id,
	guint			 size
){
	const gchar *version;

	g_return_val_if_fail(NULL != viewer, NULL);

	if ((user_id <= 0) || !venture_avatar_size_is_valid(size))
		return NULL;

	version = venture_actor_names_avatar_version(viewer, user_id);

	if (NULL == version)
		return NULL;

	return venture_avatar_url(user_id, version, size);
}

/*
 * The same picture at twice the size, for srcset, when @url is one of
 * ours ("/avatar/ID/SIZE?v=VERSION") and twice SIZE is kept.
 */
static gchar *
venture_avatar_double_url(const gchar *url)
{
	const gchar *rest;
	const gchar *slash;
	const gchar *query;
	g_autofree gchar *size_text = NULL;
	guint64 size;

	if ((NULL == url) || !g_str_has_prefix(url, "/avatar/"))
		return NULL;

	rest = url + strlen("/avatar/");
	slash = strchr(rest, '/');

	if (NULL == slash)
		return NULL;

	query = strchr(slash + 1, '?');

	if (NULL == query)
		return NULL;

	size_text = g_strndup(slash + 1, (gsize)(query - (slash + 1)));

	if (!g_ascii_string_to_unsigned(size_text, 10, 1, G_MAXUINT / 2, &size, NULL) ||
	    !venture_avatar_size_is_valid((guint)size * 2))
		return NULL;

	return g_strdup_printf("%.*s%u%s", (gint)(slash + 1 - url), url,
	                       (guint)size * 2, query);
}

void
venture_avatar_html_append(
	GString		*html,
	const gchar	*url,
	const gchar	*name,
	guint		 size
){
	g_return_if_fail(NULL != html);

	if (!venture_string_is_empty(url))
	{
		g_autofree gchar *twice = NULL;

		twice = venture_avatar_double_url(url);

		g_string_append_printf(html, "<img class=\"avatar avatar-%u\" src=\"", size);
		venture_html_escape_append(html, url);
		g_string_append(html, "\"");

		if (NULL != twice)
		{
			g_string_append(html, " srcset=\"");
			venture_html_escape_append(html, twice);
			g_string_append(html, " 2x\"");
		}

		g_string_append(html, " alt=\"");
		venture_html_escape_append(html, !venture_string_is_empty(name) ? name : "");
		g_string_append_printf(html, "\" width=\"%u\" height=\"%u\" "
		                       "loading=\"lazy\" decoding=\"async\">", size, size);
		return;
	}

	{
		g_autofree gchar *initials = NULL;

		initials = venture_avatar_initials(name);

		/* Hidden from a screen reader: the name is written beside every
		 * badge, and "R E" read out before "Ruth Ellery" is noise. */
		g_string_append_printf(html, "<span class=\"avatar avatar-%u avatar-initials\" "
		                       "aria-hidden=\"true\">", size);
		venture_html_escape_append(html, initials);
		g_string_append(html, "</span>");
	}
}

gchar *
venture_avatar_html(
	VentureActorNames	*viewer,
	gint64			 user_id,
	const gchar		*name,
	guint			 size
){
	g_autoptr(GString) html = NULL;
	g_autofree gchar *url = NULL;

	g_return_val_if_fail(NULL != viewer, NULL);

	html = g_string_new(NULL);
	url = venture_avatar_url_for(viewer, user_id, size);
	venture_avatar_html_append(html, url, name, size);

	return g_string_free(g_steal_pointer(&html), FALSE);
}

/* --- The store ------------------------------------------------------------- */

/* <state_dir>/avatars, the root every picture lives under. */
static gchar *
venture_avatar_root(VentureContext *context)
{
	return g_build_filename(venture_config_get_state_dir(
		venture_context_get_config(context)), "avatars", NULL);
}

static gchar *
venture_avatar_user_directory(gint64 user_id)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, user_id);
}

/*
 * A version is twenty lowercase hex digits. Anything else in the column
 * was not written by this store and must not become part of a path, nor
 * of an address on a page.
 */
gboolean
venture_avatar_version_is_valid(const gchar *version)
{
	gsize i;

	if ((NULL == version) || (VENTURE_AVATAR_VERSION_LENGTH != strlen(version)))
		return FALSE;

	for (i = 0; i < VENTURE_AVATAR_VERSION_LENGTH; i++)
	{
		if (!g_ascii_isxdigit(version[i]) || g_ascii_isupper(version[i]))
			return FALSE;
	}

	return TRUE;
}

static gchar *
venture_avatar_file_name(
	const gchar	*version,
	guint		 size
){
	return g_strdup_printf("%s-%u.png", version, size);
}

/* The digest of every rendition, in order: the version. */
static gchar *
venture_avatar_digest(GPtrArray *renditions)
{
	g_autoptr(GChecksum) checksum = NULL;
	guint i;

	checksum = g_checksum_new(G_CHECKSUM_SHA256);

	for (i = 0; i < renditions->len; i++)
	{
		GBytes *png = g_ptr_array_index(renditions, i);

		g_checksum_update(checksum, g_bytes_get_data(png, NULL),
		                  (gssize)g_bytes_get_size(png));
	}

	return g_strndup(g_checksum_get_string(checksum),
	                 VENTURE_AVATAR_VERSION_LENGTH);
}

/*
 * Deletes one version's files. Only ever after the record has stopped
 * naming them, so a failure leaves an orphan file, never a broken picture;
 * it is said in the log and otherwise ignored.
 */
static void
venture_avatar_unlink_version(
	const gchar	*directory,
	const gchar	*version
){
	guint i;

	if (!venture_avatar_version_is_valid(version))
		return;

	for (i = 0; i < G_N_ELEMENTS(venture_avatar_size_table); i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *path = NULL;

		name = venture_avatar_file_name(version, venture_avatar_size_table[i]);
		path = g_build_filename(directory, name, NULL);

		if ((0 != g_unlink(path)) && (ENOENT != errno))
			g_message("Could not remove the old picture %s: %s", path,
			          g_strerror(errno));
	}
}

/* The user row, read as trusted work, or NOT_FOUND. */
static VentureEntity *
venture_avatar_load_user(
	VentureDatabase	 *database,
	gint64		  user_id,
	GError		**error
){
	VentureEntity *user;

	user = (user_id > 0)
		? venture_database_get(database, VENTURE_TYPE_USER, user_id, NULL)
		: NULL;

	if ((NULL == user) || venture_entity_is_deleted(user))
	{
		g_clear_object(&user);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "User %" G_GINT64_FORMAT " not found", user_id);
		return NULL;
	}

	return user;
}

/* Saves @user as the store, which the validator lets change the version. */
static gboolean
venture_avatar_save_user(
	VentureDatabase		 *database,
	VentureEntity		 *user,
	const VentureActor	 *actor,
	GError			**error
){
	gboolean ok;

	g_object_set_data(G_OBJECT(database), VENTURE_AVATAR_FILING_KEY, user);
	ok = venture_database_save(database, user, actor, error);
	g_object_set_data(G_OBJECT(database), VENTURE_AVATAR_FILING_KEY, NULL);

	return ok;
}

gchar *
venture_avatar_set(
	VentureContext		 *context,
	gint64			  user_id,
	GBytes			 *data,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(GPtrArray) renditions = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	/* Everything about the picture is settled before anything is
	 * written, so a refused upload leaves no trace. */
	renditions = venture_avatar_render(data, error);

	if (NULL == renditions)
		return NULL;

	return venture_avatar_store(context, user_id, renditions, actor, error);
}

gchar *
venture_avatar_store(
	VentureContext		 *context,
	gint64			  user_id,
	GPtrArray		 *renditions,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *leaf = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *previous = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != renditions, NULL);

	/* Only what venture_avatar_render() makes: one PNG per size. */
	if (G_N_ELEMENTS(venture_avatar_size_table) != renditions->len)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "A picture is %u renditions, not %u",
		            (guint)G_N_ELEMENTS(venture_avatar_size_table),
		            renditions->len);
		return NULL;
	}

	database = venture_context_get_database(context);
	version = venture_avatar_digest(renditions);

	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);
	user = venture_avatar_load_user(database, user_id, error);

	if (NULL == user)
		return NULL;

	g_object_get(user, "avatar-version", &previous, NULL);

	root = venture_avatar_root(context);
	leaf = venture_avatar_user_directory(user_id);
	directory = g_build_filename(root, leaf, NULL);

	if (0 != g_mkdir_with_parents(directory, 0700))
	{
		gint saved = errno;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot create the picture store %s: %s", directory,
		            g_strerror(saved));
		return NULL;
	}

	/* The files first, then the record that names them: a reader never
	 * finds a version whose files are not there yet. */
	for (i = 0; i < renditions->len; i++)
	{
		GBytes *png = g_ptr_array_index(renditions, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *path = NULL;

		name = venture_avatar_file_name(version, venture_avatar_size_table[i]);
		path = g_build_filename(directory, name, NULL);

		if (!g_file_set_contents_full(path, g_bytes_get_data(png, NULL),
		                              (gssize)g_bytes_get_size(png),
		                              G_FILE_SET_CONTENTS_CONSISTENT, 0600,
		                              error))
		{
			g_prefix_error(error, "Cannot store the picture: ");

			if (0 != g_strcmp0(previous, version))
				venture_avatar_unlink_version(directory, version);

			return NULL;
		}
	}

	/* The same picture again: the files were rewritten with the same
	 * bytes, and the record already names them. */
	if (0 == g_strcmp0(previous, version))
		return g_steal_pointer(&version);

	g_object_set(user, "avatar-version", version, NULL);

	if (!venture_avatar_save_user(database, user, actor, error))
	{
		venture_avatar_unlink_version(directory, version);
		return NULL;
	}

	venture_avatar_unlink_version(directory, previous);

	return g_steal_pointer(&version);
}

gboolean
venture_avatar_remove(
	VentureContext		 *context,
	gint64			  user_id,
	const VentureActor	 *actor,
	GError			**error
){
	VentureDatabase *database;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *leaf = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *previous = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	database = venture_context_get_database(context);
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);
	user = venture_avatar_load_user(database, user_id, error);

	if (NULL == user)
		return FALSE;

	g_object_get(user, "avatar-version", &previous, NULL);

	if (venture_string_is_empty(previous))
		return TRUE;

	g_object_set(user, "avatar-version", "", NULL);

	if (!venture_avatar_save_user(database, user, actor, error))
		return FALSE;

	/* The record no longer names them: now the bytes go, not merely the
	 * pointer to them. A removed picture is removed. */
	root = venture_avatar_root(context);
	leaf = venture_avatar_user_directory(user_id);
	directory = g_build_filename(root, leaf, NULL);
	venture_avatar_unlink_version(directory, previous);

	/* Empty now unless something else was left in it; either is fine. */
	(void)g_rmdir(directory);

	return TRUE;
}

gboolean
venture_avatar_purge(
	VentureContext	 *context,
	gint64		  user_id,
	GError		**error
){
	g_autofree gchar *root = NULL;
	g_autofree gchar *leaf = NULL;
	g_autoptr(GPtrArray) names = NULL;
	struct dirent *entry;
	DIR *listing;
	gint directory;
	gint child;
	gint saved;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (user_id <= 0)
		return TRUE;

	root = venture_avatar_root(context);
	leaf = venture_avatar_user_directory(user_id);

	/* No store, or no directory for this person: nothing to remove. */
	directory = g_open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);

	if (directory < 0)
	{
		saved = errno;

		if (ENOENT == saved)
			return TRUE;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot open the picture store %s: %s", root,
		            g_strerror(saved));
		return FALSE;
	}

	child = openat(directory, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

	if (child < 0)
	{
		saved = errno;

		if (ENOENT == saved)
		{
			close(directory);
			return TRUE;
		}

		/* Not a directory this store made -- a link, or a file, in its
		 * place. The name itself goes; what a link points at is never
		 * followed, let alone removed. */
		if ((ELOOP == saved) || (ENOTDIR == saved))
		{
			if ((0 != unlinkat(directory, leaf, 0)) && (ENOENT != errno))
			{
				saved = errno;
				close(directory);
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
				            "Cannot remove %s/%s: %s", root, leaf,
				            g_strerror(saved));
				return FALSE;
			}

			close(directory);
			return TRUE;
		}

		close(directory);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot open the pictures of user %" G_GINT64_FORMAT ": %s",
		            user_id, g_strerror(saved));
		return FALSE;
	}

	/* Every name in it, read first and removed after, so the listing is
	 * not changing under readdir(). Each goes by unlinkat() relative to
	 * the directory opened above: a name is never resolved twice. */
	listing = fdopendir(child);

	if (NULL == listing)
	{
		saved = errno;
		close(child);
		close(directory);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot list the pictures of user %" G_GINT64_FORMAT ": %s",
		            user_id, g_strerror(saved));
		return FALSE;
	}

	names = g_ptr_array_new_with_free_func(g_free);

	while (NULL != (entry = readdir(listing)))
	{
		if ((0 == strcmp(entry->d_name, ".")) || (0 == strcmp(entry->d_name, "..")))
			continue;

		g_ptr_array_add(names, g_strdup(entry->d_name));
	}

	saved = 0;

	for (i = 0; i < names->len; i++)
	{
		const gchar *name = g_ptr_array_index(names, i);

		if ((0 != unlinkat(dirfd(listing), name, 0)) && (ENOENT != errno))
		{
			saved = errno;
			g_message("Could not remove the picture %s/%s/%s: %s", root,
			          leaf, name, g_strerror(saved));
		}
	}

	closedir(listing);

	if ((0 != unlinkat(directory, leaf, AT_REMOVEDIR)) && (ENOENT != errno))
	{
		if (0 == saved)
			saved = errno;
	}

	close(directory);

	if (0 != saved)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot remove every picture of user %" G_GINT64_FORMAT
		            " from %s: %s", user_id, root, g_strerror(saved));
		return FALSE;
	}

	return TRUE;
}

/* --- Deleting a user ------------------------------------------------------- */

/* On the context: the users deleted inside a transaction still open,
 * whose pictures go when it commits. */
#define VENTURE_AVATAR_PENDING_KEY "venture-avatar-pending-purges"

/* Purges each of @ids, saying in the log what could not be removed. */
static void
venture_avatar_purge_all(
	VentureContext	*context,
	GArray		*ids
){
	guint i;

	for (i = 0; i < ids->len; i++)
	{
		g_autoptr(GError) error = NULL;
		gint64 user_id;

		user_id = g_array_index(ids, gint64, i);

		if (!venture_avatar_purge(context, user_id, &error))
			g_warning("The picture of deleted user %" G_GINT64_FORMAT
			          " was not removed: %s", user_id, error->message);
	}
}

/*
 * A user was deleted: their picture goes with them. Not at once inside a
 * transaction, because a rollback would bring the account back without
 * its picture; then at the outermost commit, and not at all on rollback.
 */
static void
venture_avatar_on_deleted(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gpointer	 data
){
	VentureContext *context;
	GArray *pending;
	gint64 user_id;

	if (!VENTURE_IS_USER(entity))
		return;

	context = data;
	user_id = venture_entity_get_id(entity);

	if (user_id <= 0)
		return;

	if (venture_database_has_transaction(database))
	{
		pending = g_object_get_data(G_OBJECT(context), VENTURE_AVATAR_PENDING_KEY);
		g_array_append_val(pending, user_id);
		return;
	}

	{
		g_autoptr(GArray) one = NULL;

		one = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_array_append_val(one, user_id);
		venture_avatar_purge_all(context, one);
	}
}

static void
venture_avatar_on_transaction_finished(
	VentureDatabase	*database,
	gboolean	 committed,
	gpointer	 data
){
	VentureContext *context;
	GArray *pending;
	g_autoptr(GArray) due = NULL;

	(void)database;

	context = data;
	pending = g_object_get_data(G_OBJECT(context), VENTURE_AVATAR_PENDING_KEY);

	if ((NULL == pending) || (0 == pending->len))
		return;

	/* Taken whole before any purge runs, so a purge that somehow starts
	 * another transaction cannot see or change this list. */
	due = g_array_copy(pending);
	g_array_set_size(pending, 0);

	if (committed)
		venture_avatar_purge_all(context, due);
}

void
venture_avatar_install_store(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	if (NULL != g_object_get_data(G_OBJECT(context), VENTURE_AVATAR_PENDING_KEY))
		return;

	database = venture_context_get_database(context);

	/* Per context, not per database: each context has its own state
	 * directory, and so its own store, to clear. Disconnected when the
	 * context goes. */
	g_object_set_data_full(G_OBJECT(context), VENTURE_AVATAR_PENDING_KEY,
	                       g_array_new(FALSE, FALSE, sizeof(gint64)),
	                       (GDestroyNotify)g_array_unref);
	g_signal_connect_object(database, "entity-deleted",
	                        G_CALLBACK(venture_avatar_on_deleted), context, 0);
	g_signal_connect_object(database, "transaction-finished",
	                        G_CALLBACK(venture_avatar_on_transaction_finished),
	                        context, 0);
}

/*
 * One stored file, opened without following a link at any step below the
 * store's root, refused unless it is a regular file of a sane size.
 */
static GBytes *
venture_avatar_read_file(
	const gchar	 *root,
	const gchar	 *leaf,
	const gchar	 *name,
	GError		**error
){
	g_autofree gchar *buffer = NULL;
	struct stat info;
	gsize offset;
	gint directory;
	gint child;
	gint fd;

	directory = g_open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);

	if (directory < 0)
		goto missing;

	child = openat(directory, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	close(directory);

	if (child < 0)
		goto missing;

	/* Nonblocking, so fstat can refuse a FIFO without waiting on it. */
	fd = openat(child, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	close(child);

	if (fd < 0)
		goto missing;

	if ((0 != fstat(fd, &info)) || !S_ISREG(info.st_mode) || (info.st_size <= 0) ||
	    ((guint64)info.st_size > VENTURE_AVATAR_RENDITION_MAX_BYTES))
	{
		close(fd);
		goto missing;
	}

	buffer = g_malloc((gsize)info.st_size);
	offset = 0;

	while (offset < (gsize)info.st_size)
	{
		ssize_t got;

		got = read(fd, buffer + offset, (gsize)info.st_size - offset);

		if ((got < 0) && (EINTR == errno))
			continue;

		if (got <= 0)
		{
			close(fd);
			goto missing;
		}

		offset += (gsize)got;
	}

	close(fd);

	return g_bytes_new_take(g_steal_pointer(&buffer), offset);

missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
	                    "Picture not found");
	return NULL;
}

GBytes *
venture_avatar_read(
	VentureContext	 *context,
	gint64		  user_id,
	guint		  size,
	gchar		**version,
	GError		**error
){
	VentureDatabase *database;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *current = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *leaf = NULL;
	g_autofree gchar *name = NULL;
	GBytes *bytes;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (NULL != version)
		*version = NULL;

	if (!venture_avatar_size_is_valid(size))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No picture is kept at %u pixels", size);
		return NULL;
	}

	database = venture_context_get_database(context);
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(database), NULL);
	user = venture_avatar_load_user(database, user_id, error);

	if (NULL == user)
		return NULL;

	g_object_get(user, "avatar-version", &current, NULL);

	if (!venture_avatar_version_is_valid(current))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "Picture not found");
		return NULL;
	}

	root = venture_avatar_root(context);
	leaf = venture_avatar_user_directory(user_id);
	name = venture_avatar_file_name(current, size);
	bytes = venture_avatar_read_file(root, leaf, name, error);

	if ((NULL != bytes) && (NULL != version))
		*version = g_steal_pointer(&current);

	return bytes;
}

/* --- The record ------------------------------------------------------------ */

/* Only the store changes which picture a user has; see the header. */
static gboolean
venture_avatar_validate(
	VentureDatabase	 *database,
	VentureEntity	 *row,
	VentureEntity	 *previous,
	gpointer	  data,
	GError		**error
){
	g_autofree gchar *now = NULL;
	g_autofree gchar *before = NULL;

	(void)data;

	if (g_object_get_data(G_OBJECT(database), VENTURE_AVATAR_FILING_KEY) == row)
		return TRUE;

	g_object_get(row, "avatar-version", &now, NULL);

	if (NULL != previous)
		g_object_get(previous, "avatar-version", &before, NULL);

	/* Unset and empty are the same "no picture". */
	if (0 == g_strcmp0(venture_string_is_empty(now) ? "" : now,
	                   venture_string_is_empty(before) ? "" : before))
		return TRUE;

	venture_set_error_validation(error, "avatar-version",
		"is set by uploading a picture (the account page, the users page, "
		"or PUT /api/v1/users/:id/avatar), never written directly");

	return FALSE;
}

void
venture_avatar_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));

	venture_database_add_save_validator(database, VENTURE_TYPE_USER,
	                                    venture_avatar_validate, NULL, NULL);
}
