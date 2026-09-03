/*
 * venture-kb-crossref.h - Connecting knowledge to the records it bears on
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The point of the whole feature: an idea's page saying which passages of
 * which handbook bear on it, without anybody having filed the connection.
 *
 * A record's text is embedded and compared against the corpus, and links
 * above a threshold are recorded. Which record types participate is derived
 * from the field table rather than listed here -- a type with a long text
 * field has something to compare, one without does not -- so a record type
 * added by a plugin next week is covered without a line changing.
 */

#ifndef VENTURE_KB_CROSSREF_H
#define VENTURE_KB_CROSSREF_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * venture_kb_crossref_type_is_eligible:
 * @entity_type: a registered record type
 *
 * Whether this type has enough text to cross-reference.
 *
 * True when the type declares at least one %VENTURE_FIELD_KIND_TEXT field.
 * That is the same declaration the web form uses to decide between a one-line
 * box and a textarea, so the answer tracks what the type actually holds
 * rather than a list somebody has to remember to update.
 *
 * The knowledge-base types themselves are excluded: cross-referencing an
 * article against the corpus it is part of finds itself, and linking chunks
 * to chunks is noise.
 *
 * Returns: %TRUE when the type participates
 */
gboolean
venture_kb_crossref_type_is_eligible(GType entity_type);

/**
 * venture_kb_crossref_record:
 * @service: the knowledge-base service
 * @type_name: the registered record type name
 * @record_id: the record
 * @actor: (nullable): who is doing this, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Computes and stores the knowledge that bears on one record.
 *
 * Replaces that record's existing links rather than adding to them: a
 * record whose description was rewritten is about something else now, and
 * keeping the old links would cite passages for text that has gone.
 *
 * Links below kb.crossref_min_score are not recorded at all. A weak match is
 * not a weak signal here -- with a corpus of any size, everything matches
 * everything a little, and a page listing its ten least-irrelevant passages
 * is worse than a page listing none.
 *
 * Returns: how many links were written, or -1 on failure
 */
gint
venture_kb_crossref_record(
	VentureKbService	 *service,
	const gchar		 *type_name,
	gint64			  record_id,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_kb_crossref_links_for:
 * @service: the knowledge-base service
 * @type_name: the registered record type name
 * @record_id: the record
 * @error: (out) (optional): return location for a #GError
 *
 * The stored links for one record, strongest first.
 *
 * Reads what venture_kb_crossref_record() wrote rather than recomputing, so
 * rendering a page costs a query instead of an embedding request. The
 * consequence is that links are as old as the last cross-reference, which is
 * why each one records when it was computed.
 *
 * Returns: (transfer full) (nullable) (element-type VentureEntity): the
 *   links
 */
GPtrArray *
venture_kb_crossref_links_for(
	VentureKbService	 *service,
	const gchar		 *type_name,
	gint64			  record_id,
	GError			**error
);

/**
 * venture_kb_crossref_sweep:
 * @service: the knowledge-base service
 * @type_name: (nullable): one type, or %NULL for every eligible type
 * @limit: most records to process, or 0 for no limit
 * @actor: (nullable): who is doing this, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Cross-references many records.
 *
 * Deliberately an explicit operation with a limit rather than something that
 * happens on save. Each record costs an embedding request, so a sweep of a
 * populated install is minutes of them -- and coding runs hold the only
 * background thread in VENTURE and may not touch the database, so there is
 * nowhere to hide the work. Making it a command means the operator chooses
 * when to pay.
 *
 * Returns: how many records were processed, or -1 on failure
 */
gint
venture_kb_crossref_sweep(
	VentureKbService	 *service,
	const gchar		 *type_name,
	guint			  limit,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_kb_article_from_record:
 * @service: the knowledge-base service
 * @kb_id: the base to write into
 * @type_name: the registered record type name
 * @record_id: the record to write from
 * @actor: (nullable): who is doing this, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a knowledge-base article from a record.
 *
 * For turning a research note, a resolved ticket or a closed deal into
 * something the assistant can find later. The article is built from the
 * record's own text -- its name field as the title, its text fields as the
 * body -- rather than by asking a model to summarise it, because a summary
 * is a second version of the truth and this one is meant to be citable.
 *
 * origin-type and origin-id record where it came from, so the article says
 * what it was written from and a second call updates that article rather
 * than making another.
 *
 * Returns: the article's id, or -1 on failure
 */
gint64
venture_kb_article_from_record(
	VentureKbService	 *service,
	gint64			  kb_id,
	const gchar		 *type_name,
	gint64			  record_id,
	const VentureActor	 *actor,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_KB_CROSSREF_H */
