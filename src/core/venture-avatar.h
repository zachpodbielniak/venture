/*
 * venture-avatar.h - Profile pictures for user accounts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A user may carry a picture. What is uploaded is checked from its bytes
 * before any decoder sees them (size, magic number, the dimensions its
 * header claims), decoded with GdkPixbuf, cropped to a square and written
 * again as small PNGs of fixed sizes. Only those renditions are kept: the
 * upload itself, and everything it carried besides pixels -- EXIF, GPS,
 * comments, colour profiles -- is gone by construction, because the PNGs
 * are written here from the pixels alone.
 *
 * The renditions live under <state_dir>/avatars/<user id>/, named by a
 * digest of all of them; the user record's "avatar-version" names the
 * current set. A new picture is a new digest and so a new address, which
 * is what lets the address be cached forever.
 *
 * Everything that draws a person asks venture_avatar_html(): a picture
 * when there is one the reader may see, the initials badge otherwise.
 * A build without GdkPixbuf has the whole API; it refuses uploads and
 * draws initials.
 */

#ifndef VENTURE_AVATAR_H
#define VENTURE_AVATAR_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_AVATAR_MAX_BYTES:
 *
 * The largest upload accepted, in bytes. A picture of a face needs far
 * less; the cap is checked before anything else looks at the bytes, and
 * the HTTP layer refuses a body much past it before it is buffered.
 */
#define VENTURE_AVATAR_MAX_BYTES (5 * 1024 * 1024)

/**
 * VENTURE_AVATAR_MAX_SIDE:
 *
 * The most pixels an upload may claim on either side. Checked against
 * the header before decoding, and again by the decoder when it learns
 * the size, so a small file claiming an enormous image (a decompression
 * bomb) is refused without the image ever being allocated.
 */
#define VENTURE_AVATAR_MAX_SIDE (4096)

/**
 * VENTURE_AVATAR_RENDITION_MAX_BYTES:
 *
 * The most a stored rendition is read back as. A 192 pixel PNG is tens of
 * kilobytes; anything bigger on disk is not one this code wrote.
 */
#define VENTURE_AVATAR_RENDITION_MAX_BYTES (1024 * 1024)

/**
 * VENTURE_AVATAR_SIZE_SMALL:
 *
 * The small badge: comments, lists, the account menu.
 */
#define VENTURE_AVATAR_SIZE_SMALL (32)

/**
 * VENTURE_AVATAR_SIZE_LARGE:
 *
 * The large picture: the account page and a person's own page.
 */
#define VENTURE_AVATAR_SIZE_LARGE (96)

/**
 * venture_avatar_sizes:
 * @n_sizes: (out): how many there are
 *
 * Every size a picture is kept at, in pixels, smallest first: 32 and 96,
 * and twice each (64 and 192) for high-density screens. A request for
 * any other size is not found.
 *
 * Returns: (array length=n_sizes) (transfer none): the sizes
 */
const guint *
venture_avatar_sizes(guint *n_sizes);

/**
 * venture_avatar_size_is_valid:
 * @size: a size in pixels
 *
 * Returns: %TRUE when @size is one of venture_avatar_sizes()
 */
gboolean
venture_avatar_size_is_valid(guint size);

/**
 * venture_avatar_is_available:
 *
 * Whether this build can take a picture at all. Without GdkPixbuf at
 * build time it cannot; uploads are refused and everybody is drawn by
 * their initials.
 *
 * Returns: %TRUE when uploads can be accepted
 */
gboolean
venture_avatar_is_available(void);

/**
 * venture_avatar_sniff:
 * @data: (nullable): the uploaded bytes
 * @width: (out) (optional): the width the header claims
 * @height: (out) (optional): the height the header claims
 *
 * What the bytes are, read from the bytes: a PNG, a JPEG or a WebP
 * picture, by its magic number and the header that follows. A file name
 * and a declared content type are whatever the sender said, and are never
 * asked. Everything else -- SVG, GIF, BMP, TIFF, ICO, XPM, text, a
 * truncated header -- is %NULL.
 *
 * Returns: (nullable): "image/png", "image/jpeg" or "image/webp", or %NULL
 */
const gchar *
venture_avatar_sniff(
	GBytes	*data,
	gint	*width,
	gint	*height
);

/**
 * venture_avatar_check_upload:
 * @data: (nullable): the uploaded bytes
 * @error: return location for a #GError, or %NULL
 *
 * Every check that needs no decoder, in the order a refusal should name:
 * that something arrived, its size (VENTURE_AVATAR_MAX_BYTES), its kind
 * (venture_avatar_sniff()) and the dimensions its header claims
 * (VENTURE_AVATAR_MAX_SIDE). Refusals are %VENTURE_ERROR_VALIDATION.
 *
 * Returns: (nullable): the sniffed content type, or %NULL with @error set
 */
const gchar *
venture_avatar_check_upload(
	GBytes	 *data,
	GError	**error
);

/**
 * venture_avatar_png_encode:
 * @pixels: (array): 8-bit RGB or RGBA rows
 * @width: the width in pixels, at least 1
 * @height: the height in pixels, at least 1
 * @rowstride: bytes from one row to the next
 * @has_alpha: whether each pixel has a fourth, alpha byte
 * @error: return location for a #GError, or %NULL
 *
 * A PNG of exactly these pixels: IHDR, one IDAT, IEND, and no other chunk
 * -- no text, no EXIF, no colour profile, no time. Written here rather
 * than by GdkPixbuf, whose encoder on current Fedora runs in a sandboxed
 * helper process that a container may not be able to start; this one
 * needs only GLib's zlib stream. The output is the same bytes for the
 * same pixels, which keeps a picture's digest stable.
 *
 * Returns: (transfer full) (nullable): the PNG, or %NULL with @error set
 */
GBytes *
venture_avatar_png_encode(
	const guint8	 *pixels,
	gint		  width,
	gint		  height,
	gint		  rowstride,
	gboolean	  has_alpha,
	GError		**error
);

/**
 * venture_avatar_render:
 * @data: the uploaded bytes
 * @error: return location for a #GError, or %NULL
 *
 * venture_avatar_check_upload(), then decode with GdkPixbuf -- a loader
 * for the sniffed type only, refusing in size-prepared anything larger
 * than VENTURE_AVATAR_MAX_SIDE -- then crop the middle square and scale
 * it to every size in venture_avatar_sizes(). %VENTURE_ERROR_UNSUPPORTED
 * in a build without GdkPixbuf; %VENTURE_ERROR_VALIDATION when the bytes
 * cannot be decoded, or are WebP and this server has no WebP decoder.
 *
 * Returns: (transfer full) (element-type GBytes) (nullable): one PNG per
 *   size, in the order of venture_avatar_sizes(), or %NULL with @error set
 */
GPtrArray *
venture_avatar_render(
	GBytes	 *data,
	GError	**error
);

/**
 * venture_avatar_initials:
 * @name: (nullable): a person's name
 *
 * The badge drawn when there is no picture: "Ruth Ellery" is "RE",
 * "zach" is "Z", and a name with no letter or digit in it is "?".
 *
 * Returns: (transfer full): one or two characters
 */
gchar *
venture_avatar_initials(const gchar *name);

/**
 * venture_avatar_url:
 * @user_id: the account
 * @version: the picture's version, from "avatar-version"
 * @size: one of venture_avatar_sizes()
 *
 * The address a picture is served at:
 * "/avatar/<user id>/<size>?v=<version>". The version makes the address
 * change with the picture, so it may be cached forever.
 *
 * Returns: (transfer full): the address
 */
gchar *
venture_avatar_url(
	gint64		 user_id,
	const gchar	*version,
	guint		 size
);

/**
 * venture_avatar_url_for:
 * @viewer: the reader's names cache, from venture_actor_names_new()
 * @user_id: the account
 * @size: one of venture_avatar_sizes()
 *
 * The address of @user_id's picture for this reader, or %NULL when there
 * is no picture or the reader may not see it. Non-%NULL exactly when
 * `GET` on it answers 200 for the reader, so it is what an API row's
 * "avatar_url" holds.
 *
 * Returns: (transfer full) (nullable): the address, or %NULL
 */
gchar *
venture_avatar_url_for(
	VentureActorNames	*viewer,
	gint64			 user_id,
	guint			 size
);

/**
 * venture_avatar_html_append:
 * @html: where to append
 * @url: (nullable): the picture's address, from venture_avatar_url_for()
 *   or an API row's "avatar_url"; %NULL draws the initials
 * @name: (nullable): who it is, for the alt text and the initials
 * @size: the box, in pixels; the 2x rendition is offered for HiDPI
 *
 * Appends one person's badge: an <img> with alt, width, height,
 * loading="lazy" and a srcset for twice the density, or the initials in
 * a <span>. Both carry the class "avatar", so a container that sizes it
 * sizes either; the initials are aria-hidden because the name is always
 * written beside a badge.
 */
void
venture_avatar_html_append(
	GString		*html,
	const gchar	*url,
	const gchar	*name,
	guint		 size
);

/**
 * venture_avatar_html:
 * @viewer: the reader's names cache, from venture_actor_names_new()
 * @user_id: the account, or 0 for somebody who is not one (a token, an
 *   import, a deleted user); 0 always draws the initials
 * @name: (nullable): who it is, as the page names them
 * @size: VENTURE_AVATAR_SIZE_SMALL, VENTURE_AVATAR_SIZE_LARGE, or another
 *   of venture_avatar_sizes() that is not a 2x size
 *
 * The one way a person is drawn: their picture when they have one and
 * the reader may see it (venture_actor_names_may_see_user()), else the
 * initials badge. One @viewer per page, made in the reader's scope.
 *
 * Returns: (transfer full): the markup
 */
gchar *
venture_avatar_html(
	VentureActorNames	*viewer,
	gint64			 user_id,
	const gchar		*name,
	guint			 size
);

/**
 * venture_avatar_html_for_username:
 * @viewer: the reader's names cache, from venture_actor_names_new()
 * @username: (nullable): a username, or a token actor string, as
 *   venture_actor_names_user_id_for() takes
 * @display_name: (nullable): who it is, as the page names them; @username
 *   when %NULL
 * @size: VENTURE_AVATAR_SIZE_SMALL, VENTURE_AVATAR_SIZE_LARGE, or another
 *   of venture_avatar_sizes() that is not a 2x size
 *
 * venture_avatar_html() for a page that has a name rather than an account
 * id. A name the reader may not see, and a name that is not an account,
 * draw the initials -- never a picture beside a name that is not shown.
 *
 * Returns: (transfer full): the markup
 */
gchar *
venture_avatar_html_for_username(
	VentureActorNames	*viewer,
	const gchar		*username,
	const gchar		*display_name,
	guint			 size
);

/**
 * venture_avatar_set:
 * @context: the application context
 * @user_id: whose picture it is
 * @data: the uploaded bytes
 * @actor: (nullable): who is responsible, for the audit log
 * @error: return location for a #GError, or %NULL
 *
 * Renders @data (venture_avatar_render()), writes the renditions and
 * points the user record at them; the previous picture's files are
 * removed once the record no longer names them. Trusted: the caller
 * has already decided @actor may change @user_id's picture (the person
 * themself, or a global owner), and the record is written as internal
 * work so that a person who may not edit user records may still change
 * their own picture.
 *
 * Returns: (transfer full) (nullable): the new version, or %NULL with
 *   @error set
 */
gchar *
venture_avatar_set(
	VentureContext		 *context,
	gint64			  user_id,
	GBytes			 *data,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_avatar_remove:
 * @context: the application context
 * @user_id: whose picture to remove
 * @actor: (nullable): who is responsible, for the audit log
 * @error: return location for a #GError, or %NULL
 *
 * Clears the user's picture and deletes its files; they are drawn by
 * their initials again. Removing a picture that is not there succeeds.
 * Trusted, as venture_avatar_set() is.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_avatar_remove(
	VentureContext		 *context,
	gint64			  user_id,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_avatar_read:
 * @context: the application context
 * @user_id: whose picture
 * @size: one of venture_avatar_sizes()
 * @version: (out) (optional) (transfer full): the current version
 * @error: return location for a #GError, or %NULL
 *
 * The stored PNG. Mechanical: whether the reader may see it is the
 * caller's question (venture_actor_names_may_see_user()). An unknown
 * size, an account with no picture, a deleted account and a file that is
 * missing, a symbolic link or not a regular file are all
 * %VENTURE_ERROR_NOT_FOUND.
 *
 * Returns: (transfer full) (nullable): the PNG, or %NULL with @error set
 */
GBytes *
venture_avatar_read(
	VentureContext	 *context,
	gint64		  user_id,
	guint		  size,
	gchar		**version,
	GError		**error
);

/**
 * venture_avatar_install:
 * @database: the repository
 *
 * Installs the save validator that lets only the avatar store change a
 * user's "avatar-version": a REST client, a form or a staged change that
 * named a version would point the record at files that do not exist.
 */
void
venture_avatar_install(VentureDatabase *database);

G_END_DECLS

#endif /* VENTURE_AVATAR_H */
