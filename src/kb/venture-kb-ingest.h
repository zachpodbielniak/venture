/*
 * venture-kb-ingest.h - Getting documents into a knowledge base
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Four kinds of input, and the differences matter:
 *
 *   - text (.org, .md, .txt, .html) is the article, verbatim
 *   - PDF and .docx are extracted to text, and the article holds the text
 *   - an archive (.zip, .tar.gz) is unpacked and each file inside is
 *     ingested by these same rules, recursively
 *   - anything else is stored with its text unread rather than guessed at
 *
 * The last is deliberate. A binary read as text produces a body of mojibake
 * that embeds to a vector, and a vector nobody can tell is meaningless is
 * worse than a missing one: it competes with real answers.
 */

#ifndef VENTURE_KB_INGEST_H
#define VENTURE_KB_INGEST_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureKbIngestResult: (copy-func venture_kb_ingest_result_copy) (free-func venture_kb_ingest_result_free)
 * @created: articles that did not exist before
 * @updated: articles whose source had changed
 * @unchanged: articles whose source hash still matched
 * @skipped: files that were not ingestible
 * @failed: files that should have worked and did not
 * @indexed: articles embedded as part of this run
 * @notes: (element-type utf8): one line per skip or failure, for the caller
 *   to show
 *
 * What an import or a sync did.
 *
 * Counted rather than returned as a list of ids because the useful answer to
 * "what did that do" is four numbers, and because a sync of a large tree
 * would otherwise build an array nobody reads.
 */
typedef struct
{
	guint	 created;
	guint	 updated;
	guint	 unchanged;
	guint	 skipped;
	guint	 failed;
	guint	 indexed;
	GPtrArray *notes;
} VentureKbIngestResult;

/**
 * venture_kb_ingest_result_new:
 *
 * Returns: (transfer full): a zeroed result
 */
VentureKbIngestResult *
venture_kb_ingest_result_new(void);

/**
 * venture_kb_ingest_result_copy:
 * @self: (nullable): value to copy
 *
 * Copies all owned data so each result can be released independently.
 * Returns: (transfer full) (nullable): an independent copy
 */
VentureKbIngestResult *
venture_kb_ingest_result_copy(const VentureKbIngestResult *self);

/**
 * venture_kb_ingest_result_free:
 * @self: (nullable): a result
 *
 * Frees a result.
 */
void
venture_kb_ingest_result_free(VentureKbIngestResult *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureKbIngestResult,
                              venture_kb_ingest_result_free)

/**
 * venture_kb_extract_text:
 * @bytes: the file's contents
 * @filename: (nullable): its name, used to pick an extractor
 * @out_format: (out): the format that was detected
 * @error: (out) (optional): return location for a #GError
 *
 * Pulls readable text out of a file.
 *
 * Text formats are returned verbatim after a UTF-8 check; a file that is not
 * valid UTF-8 is refused rather than repaired, because the repair silently
 * changes what the document says. PDFs go through poppler when the build has
 * it. A .docx is a zip, and its text lives in word/document.xml.
 *
 * Returns %NULL with @out_format set to %VENTURE_KB_FORMAT_OTHER and no
 * error for a file whose text simply cannot be read -- an image, a binary.
 * That is a skip, not a failure.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL
 */
gchar *
venture_kb_extract_text(
	GBytes			 *bytes,
	const gchar		 *filename,
	VentureKbFormat		 *out_format,
	GError			**error
);

/**
 * venture_kb_is_archive:
 * @filename: (nullable): a name
 * @bytes: (nullable): the contents, for a magic-number check
 *
 * Whether this should be unpacked rather than stored.
 *
 * Checks the bytes as well as the name, because an archive uploaded without
 * an extension is still an archive, and a .zip that is not one should be
 * stored rather than fed to the unpacker.
 *
 * Returns: %TRUE when it is an archive this build can read
 */
gboolean
venture_kb_is_archive(
	const gchar	*filename,
	GBytes		*bytes
);

/**
 * venture_kb_ingest_bytes:
 * @service: the knowledge-base service
 * @kb_id: the base to add to
 * @bytes: the file
 * @filename: the name to derive a title and slug from
 * @source_path: (nullable): where it came from, for sync
 * @actor: (nullable): who is doing this, for the audit trail
 * @result: (inout): counts and notes, accumulated across a run
 * @error: (out) (optional): return location for a #GError
 *
 * Adds one file, unpacking it first when it is an archive.
 *
 * An article whose source hash is unchanged is left alone and counted as
 * unchanged: re-importing the same file is cheap and does not churn the
 * index. A changed one is rewritten and re-embedded.
 *
 * Returns: %TRUE unless something failed that should not have
 */
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
);

/**
 * venture_kb_sync_directory:
 * @service: the knowledge-base service
 * @kb_id: the base to sync
 * @actor: (nullable): who is doing this, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Brings a base into line with the directory named by its source-path.
 *
 * Walks the tree, ingests anything new or changed, and *archives* articles
 * whose source file has gone. Archived rather than deleted: a file removed
 * from a checkout is usually a move, and destroying the article would take
 * its cross-references with it. An archived article is not searched, so the
 * observable behaviour is the same and the mistake is recoverable.
 *
 * Change is detected by hashing the file's bytes, not its extracted text. A
 * PDF re-saved with identical text is still seen as changed, which is the
 * cheap direction to be wrong in: the alternative pays the extraction cost
 * for every file on every sync just to decide it had nothing to do.
 *
 * Returns: (transfer full) (nullable): what it did, or %NULL on failure
 */
VentureKbIngestResult *
venture_kb_sync_directory(
	VentureKbService	 *service,
	gint64			  kb_id,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_kb_export:
 * @service: the knowledge-base service
 * @kb_id: the base to export
 * @format: `zip` or `tar.gz`
 * @out_bytes: (out): the archive
 * @error: (out) (optional): return location for a #GError
 *
 * Writes every article of a base into an archive.
 *
 * One file per article, named by slug and given the extension its format
 * exports as -- which is `.txt` for a PDF, because what the article holds is
 * the extracted text and writing that under a `.pdf` name produces a file
 * nothing can open. A manifest.json carries the metadata that has nowhere to
 * live in a plain file: the base's description, and each article's status,
 * tags, origin and source path.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_kb_export(
	VentureKbService	 *service,
	gint64			  kb_id,
	const gchar		 *format,
	GBytes			**out_bytes,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_KB_INGEST_H */
