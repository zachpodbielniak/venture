/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Hosted images: pictures for email, served on a public address because
 * a mail app fetching one sends no cookie and no credential.
 *
 * The bytes live in the attachment store as an ordinary document (the one
 * upload store; nothing new on disk), and the hosted image record names
 * that document, the token in its address and what the bytes are. What
 * the bytes are is read from the bytes: a declared content type or a file
 * name is whatever the uploader's browser said, and an SVG -- which can
 * carry script -- is not served at all.
 */
#include "venture-email-design-private.h"
#include <glib/gstdio.h>
#include <string.h>

#define FILING_KEY VENTURE_EMAIL_DESIGN_FILING
/* No hex digit and no vowel: a token is never a 32-digit hex run (the
 * preview withholds those as bearer links; an image is not one) and never
 * spells a word. Thirty-two characters of 5 bits each is 160 bits. */
static const gchar token_alphabet[] = "ghjkmnpqrstvwxyzGHJKMNPQRSTVWXYZ";

static guint32 be32(const guint8 *p) { return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3]; }
static guint16 be16(const guint8 *p) { return (guint16)((p[0] << 8) | p[1]); }
static guint16 le16(const guint8 *p) { return (guint16)(p[0] | (p[1] << 8)); }
static guint32 le24(const guint8 *p) { return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16); }

const gchar *venture_hosted_image_sniff(GBytes *data, gint *width, gint *height, const gchar **extension)
{
	gsize size = 0;
	const guint8 *p = data != NULL ? g_bytes_get_data(data, &size) : NULL;
	gint w = 0, h = 0;
	const gchar *type = NULL, *ext = NULL;
	if (p == NULL)
		return NULL;
	if (size >= 24 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(p + 12, "IHDR", 4) == 0) {
		w = (gint)MIN(be32(p + 16), (guint32)G_MAXINT); h = (gint)MIN(be32(p + 20), (guint32)G_MAXINT);
		type = "image/png"; ext = "png";
	} else if (size >= 10 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) {
		w = le16(p + 6); h = le16(p + 8);
		type = "image/gif"; ext = "gif";
	} else if (size >= 30 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) {
		if (memcmp(p + 12, "VP8X", 4) == 0) {
			w = (gint)le24(p + 24) + 1; h = (gint)le24(p + 27) + 1;
		} else if (memcmp(p + 12, "VP8L", 4) == 0 && p[20] == 0x2f) {
			guint32 bits = (guint32)p[21] | ((guint32)p[22] << 8) | ((guint32)p[23] << 16) | ((guint32)p[24] << 24);
			w = (gint)(bits & 0x3fff) + 1; h = (gint)((bits >> 14) & 0x3fff) + 1;
		} else if (memcmp(p + 12, "VP8 ", 4) == 0 && p[23] == 0x9d && p[24] == 0x01 && p[25] == 0x2a) {
			w = le16(p + 26) & 0x3fff; h = le16(p + 28) & 0x3fff;
		}
		if (w > 0) { type = "image/webp"; ext = "webp"; }
	} else if (size >= 4 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff) {
		/* Walk the markers to the frame header, which holds the size. */
		gsize i = 2;
		while (i + 9 < size) {
			guint8 marker;
			guint16 length;
			if (p[i] != 0xff) break;
			marker = p[i + 1];
			if (marker == 0xff) { i++; continue; }
			if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) { i += 2; continue; }
			length = be16(p + i + 2);
			if (length < 2) break;
			if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
				h = be16(p + i + 5); w = be16(p + i + 7);
				type = "image/jpeg"; ext = "jpg";
				break;
			}
			i += 2 + (gsize)length;
		}
	}
	if (type == NULL || w <= 0 || h <= 0)
		return NULL;
	if (width) *width = w;
	if (height) *height = h;
	if (extension) *extension = ext;
	return type;
}
static const gchar *extension_for(const gchar *type)
{
	if (g_strcmp0(type, "image/png") == 0) return "png";
	if (g_strcmp0(type, "image/jpeg") == 0) return "jpg";
	if (g_strcmp0(type, "image/gif") == 0) return "gif";
	if (g_strcmp0(type, "image/webp") == 0) return "webp";
	return NULL;
}
/* The sniffed type, as a string that outlives the row it came from. */
static const gchar *type_for(const gchar *extension)
{
	if (g_strcmp0(extension, "png") == 0) return "image/png";
	if (g_strcmp0(extension, "jpg") == 0) return "image/jpeg";
	if (g_strcmp0(extension, "gif") == 0) return "image/gif";
	return "image/webp";
}
static gchar *new_token(void)
{
	g_autofree gchar *hex = venture_generate_token(20);
	GString *out = g_string_new(NULL);
	guint64 buffer = 0;
	guint bits = 0, i;
	for (i = 0; hex[i] && hex[i + 1]; i += 2) {
		buffer = (buffer << 8) | (guint64)((g_ascii_xdigit_value(hex[i]) << 4) | g_ascii_xdigit_value(hex[i + 1]));
		bits += 8;
		while (bits >= 5) {
			g_string_append_c(out, token_alphabet[(buffer >> (bits - 5)) & 31]);
			bits -= 5;
		}
	}
	return g_string_free(out, FALSE);
}
static gchar *attachment_root(VentureContext *context)
{
	return g_build_filename(venture_config_get_state_dir(venture_context_get_config(context)), "attachments", NULL);
}
VentureEntity *venture_hosted_image_store(VentureContext *context, gint64 organization_id, const gchar *filename,
	GBytes *data, const gchar *alt, const VentureActor *actor, GError **error)
{
	VentureDatabase *database;
	g_autoptr(VentureEntity) document = NULL;
	g_autoptr(VentureEntity) image = NULL;
	g_autofree gchar *root = NULL, *token = NULL, *path = NULL, *checksum = NULL, *name = NULL;
	const gchar *type, *ext = NULL;
	gint width = 0, height = 0;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	database = venture_context_get_database(context);
	if (data == NULL || g_bytes_get_size(data) == 0) {
		venture_set_error_validation(error, "Image", "no file arrived");
		return NULL;
	}
	if (g_bytes_get_size(data) > VENTURE_HOSTED_IMAGE_MAX_BYTES) {
		venture_set_error_validation(error, "Image", "it is larger than %d MB; mail apps download every image of every message",
			VENTURE_HOSTED_IMAGE_MAX_BYTES / (1024 * 1024));
		return NULL;
	}
	type = venture_hosted_image_sniff(data, &width, &height, &ext);
	if (type == NULL) {
		venture_set_error_validation(error, "Image", "it is not a PNG, JPEG, GIF or WebP picture");
		return NULL;
	}
	if (width > VENTURE_HOSTED_IMAGE_MAX_SIDE || height > VENTURE_HOSTED_IMAGE_MAX_SIDE) {
		venture_set_error_validation(error, "Image", "it is %d by %d pixels; at most %d on a side", width, height, VENTURE_HOSTED_IMAGE_MAX_SIDE);
		return NULL;
	}
	if (organization_id <= 0) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Organization not found");
		return NULL;
	}
	if (!venture_email_design_require_manager(context, organization_id, error))
		return NULL;
	root = attachment_root(context);
	if (g_mkdir_with_parents(root, 0700) != 0) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Cannot create the attachment store");
		return NULL;
	}
	token = new_token();
	{
		g_autofree gchar *stored = g_strdup_printf("email-image-%s.%s", token, ext);
		path = g_build_filename(root, stored, NULL);
	}
	if (!g_file_set_contents_full(path, g_bytes_get_data(data, NULL), (gssize)g_bytes_get_size(data),
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
		return NULL;
	checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
	name = g_strdup(!venture_string_is_empty(filename) ? filename : token);
	g_strdelimit(name, "/\\\r\n\t", '_');
	if (!venture_database_begin(database, error)) {
		g_unlink(path);
		return NULL;
	}
	document = VENTURE_ENTITY(venture_document_new());
	g_object_set(document, "title", name, "kind", "email-image", "mime-type", type, "path", path,
		"size-bytes", (gint64)g_bytes_get_size(data), "hash", checksum, NULL);
	venture_entity_set_organization_id(document, organization_id);
	ok = venture_document_service_save_attachment(venture_document_service_get(database), document, root, actor, error);
	if (ok) {
		image = g_object_new(VENTURE_TYPE_HOSTED_IMAGE, "organization-id", organization_id, "name", name,
			"alt-text", alt != NULL ? alt : "", "document-id", venture_entity_get_id(document), "token", token,
			"content-type", type, "width", (gint64)width, "height", (gint64)height,
			"size-bytes", (gint64)g_bytes_get_size(data), NULL);
		g_object_set_data(G_OBJECT(database), FILING_KEY, image);
		ok = venture_database_save(database, image, actor, error);
		g_object_set_data(G_OBJECT(database), FILING_KEY, NULL);
	}
	if (ok)
		ok = venture_database_commit(database, error);
	else
		venture_database_rollback(database);
	if (!ok) {
		g_unlink(path);
		return NULL;
	}
	return g_steal_pointer(&image);
}
gchar *venture_hosted_image_url(VentureContext *context, VentureEntity *image)
{
	g_autofree gchar *base = NULL, *token = NULL, *type = NULL;
	gint64 port = 0;
	g_object_get(venture_context_get_config(context), "server-base-url", &base, "server-port", &port, NULL);
	g_object_get(image, "token", &token, "content-type", &type, NULL);
	if (venture_string_is_empty(base)) {
		g_free(base);
		base = g_strdup_printf("http://localhost:%" G_GINT64_FORMAT, port);
	}
	while (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';
	return g_strdup_printf("%s/i/%" G_GINT64_FORMAT "/%s.%s", base, venture_entity_get_organization_id(image),
		token, extension_for(type) != NULL ? extension_for(type) : "img");
}
GBytes *venture_hosted_image_read(VentureContext *context, gint64 organization_id, const gchar *file,
	const gchar **content_type, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autofree gchar *token = NULL, *type = NULL, *root = NULL;
	const gchar *dot, *p;
	VentureEntity *image;
	gint64 document_id = 0;
	*content_type = NULL;
	dot = file != NULL ? strrchr(file, '.') : NULL;
	if (dot == NULL || dot - file < 20 || dot - file > 64 || organization_id <= 0)
		goto missing;
	token = g_strndup(file, dot - file);
	for (p = token; *p; p++)
		if (strchr(token_alphabet, *p) == NULL)
			goto missing;
	/* The token is the capability: read as the system, then hold the row
	 * to the organization in the address. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_HOSTED_IMAGE);
	venture_query_set_organization(query, organization_id);
	if (!venture_query_add_filter_string(query, "token", VENTURE_FILTER_OP_EQ, token, NULL))
		goto missing;
	venture_query_set_limit(query, 2);
	found = venture_database_find(database, query, NULL);
	if (found == NULL || found->len != 1)
		goto missing;
	image = g_ptr_array_index(found, 0);
	if (venture_entity_is_deleted(image) || venture_entity_get_organization_id(image) != organization_id)
		goto missing;
	g_object_get(image, "content-type", &type, "document-id", &document_id, NULL);
	if (g_strcmp0(extension_for(type), dot + 1) != 0)
		goto missing;
	document = venture_database_get(database, VENTURE_TYPE_DOCUMENT, document_id, NULL);
	if (document == NULL || venture_entity_is_deleted(document) || venture_entity_get_organization_id(document) != organization_id)
		goto missing;
	root = attachment_root(context);
	{
		GBytes *bytes = venture_document_service_read_attachment(venture_document_service_get(database), document, root,
			VENTURE_HOSTED_IMAGE_MAX_BYTES, error);
		if (bytes == NULL)
			return NULL;
		*content_type = type_for(dot + 1);
		return bytes;
	}
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Image not found");
	return NULL;
}

/* Only the store makes one, and nothing changes what one is. */
static gboolean image_validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	static const gchar *const fixed[] = { "document-id", "token", "content-type", "width", "height", "size-bytes", NULL };
	guint i;
	(void)data;
	if (previous == NULL) {
		if (g_object_get_data(G_OBJECT(database), FILING_KEY) != row) {
			venture_set_error_validation(error, "Image", "upload images through Email design; a hosted image is made from its file");
			return FALSE;
		}
		return TRUE;
	}
	for (i = 0; fixed[i]; i++) {
		GValue a = G_VALUE_INIT, b = G_VALUE_INIT;
		gboolean same;
		g_object_get_property(G_OBJECT(row), fixed[i], &a);
		g_object_get_property(G_OBJECT(previous), fixed[i], &b);
		same = G_VALUE_HOLDS_STRING(&a) ? g_strcmp0(g_value_get_string(&a), g_value_get_string(&b)) == 0
			: g_value_get_int64(&a) == g_value_get_int64(&b);
		g_value_unset(&a); g_value_unset(&b);
		if (!same) {
			venture_set_error_validation(error, fixed[i], "is read from the uploaded file and cannot change; upload a new image");
			return FALSE;
		}
	}
	if (venture_entity_get_organization_id(row) != venture_entity_get_organization_id(previous)) {
		venture_set_error_validation(error, "Image", "cannot move to another organization");
		return FALSE;
	}
	return TRUE;
}
void venture_hosted_image_install(VentureDatabase *database)
{
	venture_database_add_save_validator(database, VENTURE_TYPE_HOSTED_IMAGE, image_validate, NULL, NULL);
}
