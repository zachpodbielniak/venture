/*
 * venture-kb-ingest.c - Getting documents into a knowledge base
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

#ifdef VENTURE_HAVE_POPPLER
#include <poppler.h>
#endif

#ifdef VENTURE_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include <libxml/parser.h>
#include <libxml/tree.h>

/*
 * How deep a nested archive may go, and how many files one run may take.
 *
 * A zip containing a zip containing a zip is a zip bomb whether or not
 * anybody meant it that way, and the recursion is what turns a small upload
 * into an unbounded one. The entry cap is configurable; the depth is not,
 * because no legitimate document set is nested four deep.
 */
#define VENTURE_KB_MAX_ARCHIVE_DEPTH 3

VentureKbIngestResult *
venture_kb_ingest_result_new(void)
{
	VentureKbIngestResult *self;

	self = g_new0(VentureKbIngestResult, 1);
	self->notes = g_ptr_array_new_with_free_func(g_free);

	return self;
}

VentureKbIngestResult *
venture_kb_ingest_result_copy(const VentureKbIngestResult *self)
{
	VentureKbIngestResult *copy;
	guint i;
	if (self == NULL) return NULL;
	copy = g_new(VentureKbIngestResult, 1);
	*copy = *self;
	copy->notes = g_ptr_array_new_with_free_func(g_free);
	if (self->notes != NULL)
		for (i = 0; i < self->notes->len; i++)
			g_ptr_array_add(copy->notes, g_strdup(g_ptr_array_index(self->notes, i)));
	return copy;
}

void
venture_kb_ingest_result_free(VentureKbIngestResult *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->notes, g_ptr_array_unref);
	g_free(self);
}

static void
venture_kb_note(
	VentureKbIngestResult	*result,
	const gchar		*format,
	...
){
	va_list args;

	if ((NULL == result) || (NULL == result->notes))
		return;

	/*
	 * Bounded. A sync of a tree full of images would otherwise build a
	 * note per file and hand the caller a page of them, which is not a
	 * report so much as a second problem.
	 */
	if (result->notes->len >= 50)
		return;

	va_start(args, format);
	g_ptr_array_add(result->notes, g_strdup_vprintf(format, args));
	va_end(args);
}

/* ==========================================================================
 * Extraction
 * ========================================================================== */

#ifdef VENTURE_HAVE_LIBARCHIVE
/*
 * Reads one named member out of an archive held in memory.
 *
 * Used for .docx, which is a zip whose text lives in word/document.xml.
 */
static GBytes *
venture_kb_archive_member(
	GBytes		*bytes,
	const gchar	*wanted
){
	struct archive *reader;
	struct archive_entry *entry;
	GBytes *found = NULL;
	gsize size = 0;
	gconstpointer data;

	data = g_bytes_get_data(bytes, &size);
	reader = archive_read_new();
	archive_read_support_filter_all(reader);
	archive_read_support_format_all(reader);

	if (ARCHIVE_OK != archive_read_open_memory(reader, (void *)data, size))
	{
		archive_read_free(reader);
		return NULL;
	}

	while (ARCHIVE_OK == archive_read_next_header(reader, &entry))
	{
		const gchar *path = archive_entry_pathname(entry);
		g_autoptr(GByteArray) buffer = NULL;
		gint64 entry_size;

		if (0 != g_strcmp0(path, wanted))
			continue;

		entry_size = archive_entry_size(entry);

		if ((entry_size <= 0) || (entry_size > (64 * 1024 * 1024)))
			break;

		buffer = g_byte_array_sized_new((guint)entry_size);
		g_byte_array_set_size(buffer, (guint)entry_size);

		if (archive_read_data(reader, buffer->data, (size_t)entry_size) ==
		    (la_ssize_t)entry_size)
			found = g_byte_array_free_to_bytes(
				g_steal_pointer(&buffer));

		break;
	}

	archive_read_free(reader);

	return found;
}

/*
 * Collects the text of an OOXML body.
 *
 * Word puts each run's characters in <w:t> and each paragraph in <w:p>. Only
 * those two matter: everything else in the part is formatting, and joining
 * every text node without regard to paragraphs runs the whole document into
 * one line, which chunks terribly.
 */
static void
venture_kb_docx_walk(
	xmlNode	*node,
	GString	*out
){
	xmlNode *child;

	for (child = node; NULL != child; child = child->next)
	{
		if (XML_ELEMENT_NODE == child->type)
		{
			if (0 == g_strcmp0((const gchar *)child->name, "t"))
			{
				g_autofree xmlChar *content = NULL;

				content = xmlNodeGetContent(child);

				if (NULL != content)
					g_string_append(out, (const gchar *)content);

				continue;
			}

			venture_kb_docx_walk(child->children, out);

			if (0 == g_strcmp0((const gchar *)child->name, "p"))
				g_string_append_c(out, '\n');

			continue;
		}
	}
}

static gchar *
venture_kb_extract_docx(GBytes *bytes)
{
	g_autoptr(GBytes) part = NULL;
	g_autoptr(GString) out = NULL;
	xmlDoc *document;
	gsize size = 0;
	gconstpointer data;

	part = venture_kb_archive_member(bytes, "word/document.xml");

	if (NULL == part)
		return NULL;

	data = g_bytes_get_data(part, &size);
	document = xmlReadMemory(data, (gint)size, "document.xml", NULL,
	                         XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
	                         XML_PARSE_NONET);

	if (NULL == document)
		return NULL;

	out = g_string_new(NULL);
	venture_kb_docx_walk(xmlDocGetRootElement(document), out);
	xmlFreeDoc(document);

	if (0 == out->len)
		return NULL;

	return g_strdup(out->str);
}
#endif /* VENTURE_HAVE_LIBARCHIVE */

#ifdef VENTURE_HAVE_POPPLER
static gchar *
venture_kb_extract_pdf(GBytes *bytes)
{
	g_autoptr(PopplerDocument) document = NULL;
	g_autoptr(GString) out = NULL;
	gint pages;
	gint i;

	document = poppler_document_new_from_bytes(bytes, NULL, NULL);

	if (NULL == document)
		return NULL;

	out = g_string_new(NULL);
	pages = poppler_document_get_n_pages(document);

	for (i = 0; i < pages; i++)
	{
		g_autoptr(PopplerPage) page = NULL;
		g_autofree gchar *text = NULL;

		page = poppler_document_get_page(document, i);

		if (NULL == page)
			continue;

		text = poppler_page_get_text(page);

		if (venture_string_is_empty(text))
			continue;

		/*
		 * A page break is a heading-free boundary, and marking it
		 * gives the chunker somewhere natural to split a long PDF.
		 */
		if (out->len > 0)
			g_string_append(out, "\n\n");

		g_string_append(out, text);
	}

	if (0 == out->len)
		return NULL;

	return g_strdup(out->str);
}
#endif /* VENTURE_HAVE_POPPLER */

gchar *
venture_kb_extract_text(
	GBytes			 *bytes,
	const gchar		 *filename,
	VentureKbFormat		 *out_format,
	GError			**error
){
	VentureKbFormat format;
	gconstpointer data;
	gsize size = 0;

	g_return_val_if_fail(NULL != bytes, NULL);
	g_return_val_if_fail(NULL != out_format, NULL);

	format = venture_kb_format_from_path(filename);
	*out_format = format;
	data = g_bytes_get_data(bytes, &size);

	if (0 == size)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The file is empty");
		return NULL;
	}

	switch (format)
	{
	case VENTURE_KB_FORMAT_ORG:
	case VENTURE_KB_FORMAT_MARKDOWN:
	case VENTURE_KB_FORMAT_TEXT:
	case VENTURE_KB_FORMAT_HTML:
		/*
		 * Validated, not repaired. A file that is not UTF-8 is
		 * refused: substituting replacement characters silently
		 * changes what the document says, and the change is invisible
		 * until somebody reads the passage it landed in.
		 */
		if (!g_utf8_validate(data, (gssize)size, NULL))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s is not valid UTF-8",
			            (NULL != filename) ? filename : "The file");
			return NULL;
		}

		return g_strndup(data, size);

	case VENTURE_KB_FORMAT_PDF:
#ifdef VENTURE_HAVE_POPPLER
		{
			gchar *text = venture_kb_extract_pdf(bytes);

			if (NULL == text)
			{
				/*
				 * A scanned PDF has no text layer. Stored
				 * with an empty body rather than refused --
				 * the file is still filed, and the article
				 * says what it is.
				 */
				*out_format = VENTURE_KB_FORMAT_PDF;
				return NULL;
			}

			return text;
		}
#else
		*out_format = VENTURE_KB_FORMAT_PDF;
		return NULL;
#endif

	case VENTURE_KB_FORMAT_DOCX:
#ifdef VENTURE_HAVE_LIBARCHIVE
		return venture_kb_extract_docx(bytes);
#else
		*out_format = VENTURE_KB_FORMAT_DOCX;
		return NULL;
#endif

	case VENTURE_KB_FORMAT_OTHER:
	default:
		/*
		 * No error. A file whose text cannot be read is a skip, not a
		 * failure -- an import of a documentation tree that happens to
		 * contain screenshots should not stop at the first one.
		 */
		return NULL;
	}
}

gboolean
venture_kb_is_archive(
	const gchar	*filename,
	GBytes		*bytes
){
	const guchar *data;
	gsize size = 0;

	if (NULL == bytes)
		return FALSE;

	data = g_bytes_get_data(bytes, &size);

	if (size < 4)
		return FALSE;

	/*
	 * The bytes decide, not the name. An archive uploaded without an
	 * extension is still an archive, and a .zip that is not one should be
	 * stored rather than handed to the unpacker.
	 *
	 * .docx is a zip and is deliberately excluded here: it is a document
	 * with a known extractor, and unpacking it would file its XML parts
	 * as articles.
	 */
	if ((NULL != filename) &&
	    (VENTURE_KB_FORMAT_DOCX == venture_kb_format_from_path(filename)))
		return FALSE;

	/* PK\003\004 -- zip. */
	if ((0x50 == data[0]) && (0x4b == data[1]) &&
	    (0x03 == data[2]) && (0x04 == data[3]))
		return TRUE;

	/* \037\213 -- gzip, which covers .tar.gz and .tgz. */
	if ((0x1f == data[0]) && (0x8b == data[1]))
		return TRUE;

	/* \375 7zXZ -- xz. */
	if ((size >= 6) && (0xfd == data[0]) && ('7' == data[1]) &&
	    ('z' == data[2]) && ('X' == data[3]) && ('Z' == data[4]))
		return TRUE;

	/* "ustar" at offset 257 -- an uncompressed tar. */
	if (size > 262)
	{
		if (0 == memcmp(data + 257, "ustar", 5))
			return TRUE;
	}

	return FALSE;
}

/* ==========================================================================
 * Ingest
 * ========================================================================== */

/*
 * Finds the article a file already produced, by source path when there is
 * one and by slug otherwise.
 *
 * Source path is the better key: a file renamed on disk should become a new
 * article, and a file whose title changed inside should not.
 */
static VentureEntity *
venture_kb_find_article(
	VentureKbService	*service,
	gint64			 kb_id,
	const gchar		*source_path,
	const gchar		*slug
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	VentureContext *context;

	context = venture_kb_service_get_context(service);
	query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_int(query, "kb-id", VENTURE_FILTER_OP_EQ,
	                                  kb_id, NULL))
		return NULL;

	if (!venture_string_is_empty(source_path))
	{
		if (!venture_query_add_filter_string(query, "source-path",
		                                     VENTURE_FILTER_OP_EQ,
		                                     source_path, NULL))
			return NULL;
	}
	else if (!venture_string_is_empty(slug))
	{
		if (!venture_query_add_filter_string(query, "slug",
		                                     VENTURE_FILTER_OP_EQ, slug,
		                                     NULL))
			return NULL;
	}
	else
	{
		return NULL;
	}

	found = venture_database_find(venture_context_get_database(context),
	                              query, NULL);

	if ((NULL == found) || (0 == found->len))
		return NULL;

	return g_object_ref(g_ptr_array_index(found, 0));
}

/*
 * A title from the path a file arrived under.
 *
 * The whole relative path rather than the basename, because a folder or an
 * archive routinely holds several files with the same name --- README.md in
 * every directory is the normal case, not a corner one --- and two articles
 * both called "readme" are indistinguishable in a search result. Directory
 * separators become " / ", which reads as a breadcrumb.
 *
 * The extension goes, and hyphens and underscores become spaces:
 * "guides/getting-started.org" is "guides / getting started".
 */
static gchar *
venture_kb_title_from_path(const gchar *path)
{
	g_autoptr(GString) out = NULL;
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *normalised = NULL;
	gsize i;

	if (venture_string_is_empty(path))
		return g_strdup("Untitled");

	/* A browser sends a directory upload with forward slashes whatever the
	 * platform, and an archive member always uses them. */
	normalised = g_strdup(path);
	g_strdelimit(normalised, "\\", '/');

	parts = g_strsplit(normalised, "/", -1);
	out = g_string_new(NULL);

	for (i = 0; (NULL != parts) && (NULL != parts[i]); i++)
	{
		g_autofree gchar *piece = NULL;
		gchar *dot;
		gchar *p;

		if (venture_string_is_empty(parts[i]) ||
		    (0 == g_strcmp0(parts[i], ".")))
			continue;

		piece = g_strdup(parts[i]);

		/* Only the last component loses its extension: a directory
		 * called "v1.2" is not an extension. */
		if (NULL == parts[i + 1])
		{
			dot = strrchr(piece, '.');

			if ((NULL != dot) && (dot != piece))
				*dot = '\0';
		}

		for (p = piece; '\0' != *p; p++)
		{
			if (('-' == *p) || ('_' == *p))
				*p = ' ';
		}

		g_strstrip(piece);

		if (venture_string_is_empty(piece))
			continue;

		if (out->len > 0)
			g_string_append(out, " / ");

		g_string_append(out, piece);
	}

	if (0 == out->len)
		return g_strdup("Untitled");

	return g_strdup(out->str);
}

static gboolean
venture_kb_ingest_one(
	VentureKbService	 *service,
	gint64			  kb_id,
	GBytes			 *bytes,
	const gchar		 *filename,
	const gchar		 *source_path,
	const VentureActor	 *actor,
	VentureKbIngestResult	 *result,
	guint			  depth,
	GError			**error
);

#ifdef VENTURE_HAVE_LIBARCHIVE
/*
 * Unpacks an archive and ingests each member.
 *
 * Directory entries and anything absolute or containing ".." are skipped:
 * nothing here writes to disk, so a traversal cannot escape anywhere, but a
 * member named "../../etc/passwd" would still become an article with a
 * nonsense source path that a later sync would try to reconcile.
 */
static gboolean
venture_kb_ingest_archive(
	VentureKbService	 *service,
	gint64			  kb_id,
	GBytes			 *bytes,
	const gchar		 *display_path,
	const gchar		 *source_path,
	const VentureActor	 *actor,
	VentureKbIngestResult	 *result,
	guint			  depth,
	GError			**error
){
	struct archive *reader;
	struct archive_entry *entry;
	VentureContext *context;
	VentureConfig *config;
	gconstpointer data;
	gsize size = 0;
	gint64 max_entries = 2000;
	guint taken = 0;
	gboolean ok = TRUE;

	if (depth >= VENTURE_KB_MAX_ARCHIVE_DEPTH)
	{
		venture_kb_note(result,
			"%s: archives nested more than %d deep are not unpacked",
			display_path, VENTURE_KB_MAX_ARCHIVE_DEPTH);
		result->skipped++;
		return TRUE;
	}

	context = venture_kb_service_get_context(service);
	config = venture_context_get_config(context);
	g_object_get(config, "kb-max-archive-entries", &max_entries, NULL);

	data = g_bytes_get_data(bytes, &size);
	reader = archive_read_new();
	archive_read_support_filter_all(reader);
	archive_read_support_format_all(reader);

	if (ARCHIVE_OK != archive_read_open_memory(reader, (void *)data, size))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s could not be opened as an archive", display_path);
		archive_read_free(reader);
		return FALSE;
	}

	while (ok && (ARCHIVE_OK == archive_read_next_header(reader, &entry)))
	{
		const gchar *path = archive_entry_pathname(entry);
		g_autoptr(GByteArray) buffer = NULL;
		g_autoptr(GBytes) member = NULL;
		g_autofree gchar *member_path = NULL;
		gchar *member_source = NULL;
		gint64 entry_size;

		if (venture_string_is_empty(path))
			continue;

		if (AE_IFREG != archive_entry_filetype(entry))
			continue;

		if (g_str_has_prefix(path, "/") || (NULL != strstr(path, "..")))
		{
			venture_kb_note(result, "%s: skipped a member named %s",
			                display_path, path);
			result->skipped++;
			continue;
		}

		if ((max_entries > 0) && (taken >= (guint)max_entries))
		{
			venture_kb_note(result,
				"%s: stopped after %" G_GINT64_FORMAT " files",
				display_path, max_entries);
			break;
		}

		entry_size = archive_entry_size(entry);

		if (entry_size <= 0)
			continue;

		if (entry_size > (64 * 1024 * 1024))
		{
			venture_kb_note(result, "%s: %s is too large", display_path,
			                path);
			result->skipped++;
			continue;
		}

		buffer = g_byte_array_sized_new((guint)entry_size);
		g_byte_array_set_size(buffer, (guint)entry_size);

		if (archive_read_data(reader, buffer->data, (size_t)entry_size) !=
		    (la_ssize_t)entry_size)
		{
			venture_kb_note(result, "%s: %s could not be read",
			                display_path, path);
			result->failed++;
			continue;
		}

		member = g_byte_array_free_to_bytes(g_steal_pointer(&buffer));
		taken++;

		/*
		 * The display path is the archive's plus the member's, so two
		 * READMEs in different directories stay distinguishable. The
		 * source path is only extended when there was one -- an
		 * uploaded archive has none, and inventing one would make the
		 * next sync archive everything it contained.
		 */
		member_path = g_build_filename(display_path, path, NULL);

		if (!venture_string_is_empty(source_path))
			member_source = g_build_filename(source_path, path, NULL);

		ok = venture_kb_ingest_one(service, kb_id, member, member_path,
		                           member_source, actor, result,
		                           depth + 1, error);
		g_clear_pointer(&member_source, g_free);
	}

	archive_read_free(reader);

	return ok;
}
#endif /* VENTURE_HAVE_LIBARCHIVE */

static gboolean
venture_kb_ingest_one(
	VentureKbService	 *service,
	gint64			  kb_id,
	GBytes			 *bytes,
	const gchar		 *filename,
	const gchar		 *source_path,
	const VentureActor	 *actor,
	VentureKbIngestResult	 *result,
	guint			  depth,
	GError			**error
){
	g_autoptr(VentureEntity) article = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *existing_hash = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *slug = NULL;
	VentureContext *context;
	VentureKbFormat format;
	gboolean created = FALSE;
	gsize size = 0;

	if (venture_kb_is_archive(filename, bytes))
	{
#ifdef VENTURE_HAVE_LIBARCHIVE
		/*
		 * The members inherit this archive's *display* path for their
		 * titles, and its source path -- which is NULL for an upload --
		 * for sync. Passing the filename as a source path, as this once
		 * did, gave uploaded members a path like "docs.zip/a/b.org" that
		 * is on no disk, so the next sync of that base archived every one
		 * of them for having "gone".
		 */
		return venture_kb_ingest_archive(service, kb_id, bytes, filename,
		                                 source_path, actor, result, depth,
		                                 error);
#else
		venture_kb_note(result,
			"%s: this build cannot read archives", filename);
		result->skipped++;
		return TRUE;
#endif
	}

	context = venture_kb_service_get_context(service);
	g_bytes_get_data(bytes, &size);
	hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);

	/*
	 * Identity comes from the whole relative path, not the basename. A
	 * folder or an archive routinely holds several README.md, and slugging
	 * the basename makes the second one overwrite the first -- silently,
	 * because an existing slug is treated as the same document.
	 */
	title = venture_kb_title_from_path(filename);
	slug = venture_kb_slugify(title);

	article = venture_kb_find_article(service, kb_id, source_path, slug);

	if (NULL != article)
	{
		g_object_get(article, "source-hash", &existing_hash, NULL);

		/*
		 * Unchanged files cost a hash and nothing else. This is what
		 * makes a sync of a large tree cheap enough to run often,
		 * which is what makes it useful at all.
		 */
		if ((NULL != existing_hash) && (0 == g_strcmp0(existing_hash, hash)))
		{
			result->unchanged++;
			return TRUE;
		}
	}
	else
	{
		article = VENTURE_ENTITY(venture_kb_article_new());
		created = TRUE;
	}

	text = venture_kb_extract_text(bytes, filename, &format, &local_error);

	if (NULL != local_error)
	{
		venture_kb_note(result, "%s: %s", filename, local_error->message);
		result->skipped++;
		return TRUE;
	}

	if ((NULL == text) && (VENTURE_KB_FORMAT_OTHER == format))
	{
		venture_kb_note(result, "%s: not a document I can read",
		                filename);
		result->skipped++;
		return TRUE;
	}

	g_object_set(article,
	             "kb-id", kb_id,
	             "title", title,
	             "slug", slug,
	             "body", (NULL != text) ? text : "",
	             "format", format,
	             "source-path", source_path,
	             "source-hash", hash,
	             "size-bytes", (gint64)size,
	             NULL);

	if (created)
		g_object_set(article, "status",
		             VENTURE_KB_ARTICLE_STATUS_PUBLISHED, NULL);

	/*
	 * An article that was archived and whose file has come back is
	 * published again. A sync that left it archived would quietly ignore
	 * a file the operator has restored.
	 */
	{
		VentureKbArticleStatus status;

		g_object_get(article, "status", &status, NULL);

		if (VENTURE_KB_ARTICLE_STATUS_ARCHIVED == status)
			g_object_set(article, "status",
			             VENTURE_KB_ARTICLE_STATUS_PUBLISHED, NULL);
	}

	{
		g_autoptr(VentureEntity) base = NULL;

		base = venture_database_get(venture_context_get_database(context),
		                            VENTURE_TYPE_KNOWLEDGE_BASE, kb_id,
		                            NULL);

		if (NULL != base)
			venture_entity_set_organization_id(article,
				venture_entity_get_organization_id(base));
	}

	if (!venture_database_save(venture_context_get_database(context),
	                           article, actor, error))
		return FALSE;

	if (created)
		result->created++;
	else
		result->updated++;

	/*
	 * Indexed here rather than left to a later pass, so an import is
	 * searchable when it returns. A failure to embed is a note rather
	 * than a failure of the import: the article is filed, and a reindex
	 * will pick it up.
	 */
	if (venture_kb_service_index_article(service,
	                                     venture_entity_get_id(article),
	                                     actor, &local_error) < 0)
	{
		venture_kb_note(result, "%s: stored but not indexed: %s",
		                filename,
		                (NULL != local_error) ? local_error->message
		                                      : "unknown error");
		g_clear_error(&local_error);
	}
	else
	{
		result->indexed++;
	}

	return TRUE;
}

gboolean
venture_kb_ingest_bytes(
	VentureKbService	 *service,
	gint64			  kb_id,
	GBytes			 *bytes,
	const gchar		 *filename,
	const gchar		 *source_path,
	const VentureActor	 *actor,
	VentureKbIngestResult	 *result,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(service), FALSE);
	g_return_val_if_fail(NULL != bytes, FALSE);
	g_return_val_if_fail(NULL != result, FALSE);

	return venture_kb_ingest_one(service, kb_id, bytes, filename,
	                             source_path, actor, result, 0, error);
}

/* ==========================================================================
 * Sync
 * ========================================================================== */

static void
venture_kb_walk_directory(
	const gchar	*root,
	const gchar	*relative,
	GPtrArray	*out,
	guint		 depth
){
	g_autofree gchar *absolute = NULL;
	g_autoptr(GDir) dir = NULL;
	const gchar *name;

	/* Deep enough for any documentation tree, and shallow enough that a
	 * symlink loop cannot run forever. */
	if (depth > 16)
		return;

	absolute = (NULL != relative)
		? g_build_filename(root, relative, NULL)
		: g_strdup(root);

	dir = g_dir_open(absolute, 0, NULL);

	if (NULL == dir)
		return;

	while (NULL != (name = g_dir_read_name(dir)))
	{
		g_autofree gchar *child_relative = NULL;
		g_autofree gchar *child_absolute = NULL;

		/* Dotfiles are skipped: .git alone would otherwise import
		 * thousands of objects nobody wants to search. */
		if ('.' == name[0])
			continue;

		child_relative = (NULL != relative)
			? g_build_filename(relative, name, NULL)
			: g_strdup(name);
		child_absolute = g_build_filename(root, child_relative, NULL);

		if (g_file_test(child_absolute, G_FILE_TEST_IS_DIR))
		{
			venture_kb_walk_directory(root, child_relative, out,
			                          depth + 1);
			continue;
		}

		if (g_file_test(child_absolute, G_FILE_TEST_IS_REGULAR))
			g_ptr_array_add(out, g_steal_pointer(&child_relative));
	}
}

VentureKbIngestResult *
venture_kb_sync_directory(
	VentureKbService	 *service,
	gint64			  kb_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureKbIngestResult) result = NULL;
	g_autoptr(VentureEntity) base = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) articles = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *root = NULL;
	VentureContext *context;
	VentureDatabase *database;
	guint i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(service), NULL);

	context = venture_kb_service_get_context(service);
	database = venture_context_get_database(context);

	base = venture_database_get(database, VENTURE_TYPE_KNOWLEDGE_BASE, kb_id,
	                            error);

	if (NULL == base)
		return NULL;

	g_object_get(base, "source-path", &root, NULL);

	if (venture_string_is_empty(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "This knowledge base has no source directory "
		                    "to sync from");
		return NULL;
	}

	if (!g_file_test(root, G_FILE_TEST_IS_DIR))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "%s is not a directory on this server", root);
		return NULL;
	}

	result = venture_kb_ingest_result_new();
	files = g_ptr_array_new_with_free_func(g_free);
	venture_kb_walk_directory(root, NULL, files, 0);

	seen = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = 0; i < files->len; i++)
	{
		const gchar *relative = g_ptr_array_index(files, i);
		g_autofree gchar *absolute = NULL;
		g_autoptr(GBytes) bytes = NULL;
		g_autofree gchar *contents = NULL;
		gsize length = 0;

		absolute = g_build_filename(root, relative, NULL);

		if (!g_file_get_contents(absolute, &contents, &length, NULL))
		{
			venture_kb_note(result, "%s could not be read", relative);
			result->failed++;
			continue;
		}

		g_hash_table_add(seen, (gpointer)relative);
		bytes = g_bytes_new_take(g_steal_pointer(&contents), length);

		if (!venture_kb_ingest_bytes(service, kb_id, bytes, relative,
		                             relative, actor, result, error))
			return NULL;
	}

	/*
	 * Articles whose file has gone are archived, not deleted. A file
	 * removed from a checkout is usually a move, and destroying the
	 * article would take its cross-references with it. Archived articles
	 * are not searched, so the observable behaviour is the same and the
	 * mistake is recoverable.
	 */
	query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "kb-id", VENTURE_FILTER_OP_EQ,
	                                  kb_id, error))
		return NULL;

	articles = venture_database_find(database, query, error);

	if (NULL == articles)
		return NULL;

	for (i = 0; i < articles->len; i++)
	{
		VentureEntity *article = g_ptr_array_index(articles, i);
		g_autofree gchar *source_path = NULL;
		VentureKbArticleStatus status;

		g_object_get(article, "source-path", &source_path, "status",
		             &status, NULL);

		/* An article written in the browser has no source path and is
		 * not the sync's business. */
		if (venture_string_is_empty(source_path))
			continue;

		if (g_hash_table_contains(seen, source_path))
			continue;

		if (VENTURE_KB_ARTICLE_STATUS_ARCHIVED == status)
			continue;

		g_object_set(article, "status",
		             VENTURE_KB_ARTICLE_STATUS_ARCHIVED, NULL);

		if (!venture_database_save(database, article, actor, error))
			return NULL;

		venture_kb_note(result, "%s has gone from disk; archived",
		                source_path);
	}

	/*
	 * Re-read before stamping. Indexing the first article claims the
	 * base's embedding model and saves it, so the copy loaded at the top
	 * of this function is a version behind by now and the optimistic
	 * concurrency check refuses it -- correctly, and with a message about
	 * somebody else's change that would send the reader looking for a
	 * second writer that does not exist.
	 */
	g_clear_object(&base);
	base = venture_database_get(database, VENTURE_TYPE_KNOWLEDGE_BASE, kb_id,
	                            error);

	if (NULL == base)
		return NULL;

	now = venture_time_now();
	g_object_set(base, "synced-at", now, NULL);

	if (!venture_database_save(database, base, actor, error))
		return NULL;

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Export
 * ========================================================================== */

#ifdef VENTURE_HAVE_LIBARCHIVE
static la_ssize_t
venture_kb_export_write(
	struct archive	*archive,
	void		*client_data,
	const void	*buffer,
	size_t		 length
){
	GByteArray *out = client_data;

	(void)archive;
	g_byte_array_append(out, buffer, (guint)length);

	return (la_ssize_t)length;
}

static gint
venture_kb_export_noop(
	struct archive	*archive,
	void		*client_data
){
	(void)archive;
	(void)client_data;

	return ARCHIVE_OK;
}

static gboolean
venture_kb_export_add(
	struct archive	*writer,
	const gchar	*path,
	const gchar	*contents
){
	struct archive_entry *entry;
	gsize length;
	gboolean ok;

	length = (NULL != contents) ? strlen(contents) : 0;

	entry = archive_entry_new();
	archive_entry_set_pathname(entry, path);
	archive_entry_set_size(entry, (la_int64_t)length);
	archive_entry_set_filetype(entry, AE_IFREG);
	archive_entry_set_perm(entry, 0644);

	ok = (ARCHIVE_OK == archive_write_header(writer, entry));

	if (ok && (length > 0))
		ok = (archive_write_data(writer, contents, length) ==
		      (la_ssize_t)length);

	archive_entry_free(entry);

	return ok;
}
#endif /* VENTURE_HAVE_LIBARCHIVE */

gboolean
venture_kb_export(
	VentureKbService	 *service,
	gint64			  kb_id,
	const gchar		 *format,
	GBytes			**out_bytes,
	GError			**error
){
#ifdef VENTURE_HAVE_LIBARCHIVE
	g_autoptr(VentureEntity) base = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) articles = NULL;
	g_autoptr(GByteArray) out = NULL;
	g_autoptr(JsonBuilder) manifest = NULL;
	g_autoptr(JsonNode) manifest_root = NULL;
	g_autofree gchar *manifest_text = NULL;
	g_autofree gchar *base_name = NULL;
	g_autofree gchar *base_slug = NULL;
	g_autofree gchar *base_description = NULL;
	g_autoptr(GHashTable) used = NULL;
	VentureContext *context;
	struct archive *writer;
	gboolean gzip;
	gboolean ok = TRUE;
	guint i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(service), FALSE);
	g_return_val_if_fail(NULL != out_bytes, FALSE);

	gzip = (0 == g_strcmp0(format, "tar.gz")) ||
	       (0 == g_strcmp0(format, "tgz")) ||
	       (0 == g_strcmp0(format, "targz"));

	if (!gzip && (0 != g_strcmp0(format, "zip")) && (NULL != format))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not an export format. Use zip or tar.gz.",
		            format);
		return FALSE;
	}

	context = venture_kb_service_get_context(service);
	base = venture_database_get(venture_context_get_database(context),
	                            VENTURE_TYPE_KNOWLEDGE_BASE, kb_id, error);

	if (NULL == base)
		return FALSE;

	g_object_get(base, "name", &base_name, "slug", &base_slug,
	             "description", &base_description, NULL);

	query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "kb-id", VENTURE_FILTER_OP_EQ,
	                                  kb_id, error))
		return FALSE;

	articles = venture_database_find(venture_context_get_database(context),
	                                 query, error);

	if (NULL == articles)
		return FALSE;

	out = g_byte_array_new();
	writer = archive_write_new();

	if (gzip)
	{
		archive_write_add_filter_gzip(writer);
		archive_write_set_format_pax_restricted(writer);
	}
	else
	{
		archive_write_set_format_zip(writer);
	}

	if (ARCHIVE_OK != archive_write_open2(writer, out,
	                                      venture_kb_export_noop,
	                                      venture_kb_export_write,
	                                      venture_kb_export_noop,
	                                      venture_kb_export_noop))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "Could not start writing the archive");
		archive_write_free(writer);
		return FALSE;
	}

	/*
	 * The metadata a plain file has nowhere to hold: status, tags, what
	 * generated the article, where it was synced from. Without this an
	 * export is a pile of text files that cannot be imported back into
	 * the same shape.
	 */
	manifest = json_builder_new();
	json_builder_begin_object(manifest);
	json_builder_set_member_name(manifest, "knowledge_base");
	json_builder_begin_object(manifest);
	json_builder_set_member_name(manifest, "name");
	json_builder_add_string_value(manifest, base_name);
	json_builder_set_member_name(manifest, "slug");
	json_builder_add_string_value(manifest, base_slug);
	json_builder_set_member_name(manifest, "description");
	json_builder_add_string_value(manifest, base_description);
	json_builder_end_object(manifest);
	json_builder_set_member_name(manifest, "articles");
	json_builder_begin_array(manifest);

	used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; (i < articles->len) && ok; i++)
	{
		VentureEntity *article = g_ptr_array_index(articles, i);
		g_autofree gchar *title = NULL;
		g_autofree gchar *slug = NULL;
		g_autofree gchar *body = NULL;
		g_autofree gchar *tags = NULL;
		g_autofree gchar *source_path = NULL;
		g_autofree gchar *origin_type = NULL;
		g_autofree gchar *filename = NULL;
		VentureKbFormat article_format;
		VentureKbArticleStatus status;
		gint64 origin_id;

		g_object_get(article, "title", &title, "slug", &slug,
		             "body", &body, "format", &article_format,
		             "status", &status, "tags", &tags,
		             "source-path", &source_path,
		             "origin-type", &origin_type, "origin-id", &origin_id,
		             NULL);

		if (venture_string_is_empty(slug))
		{
			g_clear_pointer(&slug, g_free);
			slug = venture_kb_slugify(title);
		}

		if (venture_string_is_empty(slug))
		{
			g_clear_pointer(&slug, g_free);
			slug = g_strdup_printf("article-%" G_GINT64_FORMAT,
			                       venture_entity_get_id(article));
		}

		filename = g_strdup_printf("articles/%s%s", slug,
			venture_kb_format_extension(article_format));

		/*
		 * Two articles can share a slug -- slugs are unique per base
		 * only when something enforced it, and an imported tree may
		 * have "readme" in several directories. A collision in the
		 * archive would silently drop one, so the id disambiguates.
		 */
		if (g_hash_table_contains(used, filename))
		{
			g_clear_pointer(&filename, g_free);
			filename = g_strdup_printf("articles/%s-%" G_GINT64_FORMAT
			                           "%s", slug,
			                           venture_entity_get_id(article),
			                           venture_kb_format_extension(
			                               article_format));
		}

		g_hash_table_add(used, g_strdup(filename));

		ok = venture_kb_export_add(writer, filename, body);

		json_builder_begin_object(manifest);
		json_builder_set_member_name(manifest, "file");
		json_builder_add_string_value(manifest, filename);
		json_builder_set_member_name(manifest, "title");
		json_builder_add_string_value(manifest, title);
		json_builder_set_member_name(manifest, "slug");
		json_builder_add_string_value(manifest, slug);
		json_builder_set_member_name(manifest, "format");
		json_builder_add_string_value(manifest,
			venture_enum_to_nick(VENTURE_TYPE_KB_FORMAT,
			                     (gint)article_format));
		json_builder_set_member_name(manifest, "status");
		json_builder_add_string_value(manifest,
			venture_enum_to_nick(VENTURE_TYPE_KB_ARTICLE_STATUS,
			                     (gint)status));
		json_builder_set_member_name(manifest, "tags");
		json_builder_add_string_value(manifest, tags);
		json_builder_set_member_name(manifest, "source_path");
		json_builder_add_string_value(manifest, source_path);
		json_builder_set_member_name(manifest, "origin_type");
		json_builder_add_string_value(manifest, origin_type);
		json_builder_set_member_name(manifest, "origin_id");
		json_builder_add_int_value(manifest, origin_id);
		json_builder_end_object(manifest);
	}

	json_builder_end_array(manifest);
	json_builder_end_object(manifest);
	manifest_root = json_builder_get_root(manifest);
	manifest_text = venture_json_to_string(manifest_root, TRUE);

	if (ok)
		ok = venture_kb_export_add(writer, "manifest.json",
		                           manifest_text);

	archive_write_close(writer);
	archive_write_free(writer);

	if (!ok)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "The archive could not be written");
		return FALSE;
	}

	*out_bytes = g_byte_array_free_to_bytes(g_steal_pointer(&out));

	return TRUE;
#else
	(void)service;
	(void)kb_id;
	(void)format;
	(void)out_bytes;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
	                    "This build has no libarchive, so it cannot write "
	                    "an export archive");
	return FALSE;
#endif
}
