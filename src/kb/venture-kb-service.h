/*
 * venture-kb-service.h - Indexing and searching knowledge bases
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Indexing is synchronous, which is a deliberate constraint rather than a
 * simplification. Coding runs hold the only background thread in VENTURE and
 * are forbidden from touching the database, because writing a record emits
 * entity-saved, whose handler enters podomation's nested main loop on the
 * default context -- driving that from a second thread is a context-ownership
 * failure, not merely a data race. So an article is embedded on the request
 * that saved it, and bulk work is an explicit command rather than a sweep
 * that happens on its own.
 *
 * Search is brute-force cosine over the chunks of the bases asked for.
 * Neither backend has a vector index: SQLite has none, and the PostgreSQL
 * one lives in an extension this deployment does not install. At a few
 * thousand passages the comparison is microseconds and the query that reads
 * them costs more, so an index nobody can build is not yet missed.
 */

#ifndef VENTURE_KB_SERVICE_H
#define VENTURE_KB_SERVICE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_KB_SERVICE (venture_kb_service_get_type())
G_DECLARE_FINAL_TYPE(VentureKbService, venture_kb_service, VENTURE, KB_SERVICE,
                     GObject)

/**
 * VentureKbHit:
 * @article_id: the article the passage belongs to
 * @kb_id: the knowledge base it belongs to
 * @chunk_id: the passage's own record id
 * @ordinal: the passage's position in the article
 * @score: cosine similarity to the query, 0.0 to 1.0
 * @title: the article's title
 * @kb_slug: the base's slug, so a result can say where it came from
 * @heading: (nullable): the heading the passage sits under
 * @text: the passage
 *
 * One search result.
 */
typedef struct
{
	gint64	 article_id;
	gint64	 kb_id;
	gint64	 chunk_id;
	gint64	 ordinal;
	gdouble	 score;
	gchar	*title;
	gchar	*kb_slug;
	gchar	*heading;
	gchar	*text;
} VentureKbHit;

/**
 * venture_kb_hit_free:
 * @self: (nullable): a hit
 *
 * Frees a search result.
 */
void
venture_kb_hit_free(VentureKbHit *self);

/**
 * venture_kb_service_new:
 * @context: the context
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the knowledge-base service.
 *
 * Fails when kb.enabled is false or the embedding provider is unusable. A
 * server whose knowledge bases are disabled still serves every other route,
 * so callers treat %NULL as "not available" rather than as a fatal error.
 *
 * Returns: (transfer full) (nullable): the service, or %NULL
 */
VentureKbService *
venture_kb_service_new(
	VentureContext	 *context,
	GError		**error
);

/**
 * venture_kb_service_get_context:
 * @self: the service
 *
 * Returns: (transfer none): the context, for the ingest and export paths
 *   that need the database and configuration
 */
VentureContext *
venture_kb_service_get_context(VentureKbService *self);

/**
 * venture_kb_service_get_embedder:
 * @self: the service
 *
 * Returns: (transfer none) (nullable): the most recently selected organization
 *   embedder, or NULL before indexing/search. Arbitrary text requires an
 *   explicit venture_embedder_new_for_organization() client.
 */
VentureEmbedder *
venture_kb_service_get_embedder(VentureKbService *self);

/**
 * venture_kb_service_index_article:
 * @self: the service
 * @article_id: the article to index
 * @actor: (nullable): who is doing this, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Splits an article into passages, embeds them and replaces its chunks.
 *
 * Replaces rather than appends: an article that shrank would otherwise keep
 * passages of text it no longer contains, and those passages stay findable
 * and cite an article that has since been corrected. Old chunks are deleted
 * inside the same transaction as the new ones, so a failure part-way leaves
 * the previous index rather than none.
 *
 * The base's embedding model is set from the embedder on first index and
 * checked against it afterwards. Indexing one article of a base with a
 * different model is refused: cosine between two models' vectors is noise
 * that reads exactly like a weak match.
 *
 * Returns: the number of passages written, or -1 on failure
 */
gint
venture_kb_service_index_article(
	VentureKbService	 *self,
	gint64			  article_id,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_kb_service_reindex:
 * @self: the service
 * @kb_id: the base to reindex, or 0 for every base
 * @force: whether to reindex articles that already have current chunks
 * @actor: (nullable): who is doing this, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Indexes every article that needs it.
 *
 * Without @force, an article whose chunks were made by the configured model
 * is left alone, which is what makes this safe to run repeatedly and cheap
 * when nothing changed. With it, everything is embedded again -- which is
 * what a change of model requires, and the only way to recover from one.
 *
 * Returns: the number of articles indexed, or -1 on failure
 */
gint
venture_kb_service_reindex(
	VentureKbService	 *self,
	gint64			  kb_id,
	gboolean		  force,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_kb_service_search:
 * @self: the service
 * @query: what to look for
 * @kb_ids: (nullable) (array length=n_kb_ids): bases to search, or %NULL for
 *   every base marked auto-retrieve
 * @n_kb_ids: how many
 * @limit: most results to return
 * @error: (out) (optional): return location for a #GError
 *
 * Searches passages by meaning.
 *
 * Only published articles are searched: a draft is knowledge somebody is
 * still deciding about, and an assistant citing one is quoting a document
 * its author has not stood behind.
 *
 * Returns: (transfer full) (nullable) (element-type VentureKbHit): the best
 *   passages, strongest first
 */
GPtrArray *
venture_kb_service_search(
	VentureKbService	 *self,
	const gchar		 *query,
	const gint64		 *kb_ids,
	gsize			  n_kb_ids,
	guint			  limit,
	GError			**error
);

/**
 * venture_kb_service_resolve_slugs:
 * @self: the service
 * @slugs: (array zero-terminated=1): base slugs, as typed after '#'
 * @out_n_ids: (out): how many ids were resolved
 * @error: (out) (optional): return location for a #GError
 *
 * Turns base slugs into ids.
 *
 * An unknown slug is an error naming it rather than a silent omission:
 * somebody who typed #handbok and got answers from every other base would
 * reasonably believe the handbook had been searched.
 *
 * Returns: (transfer full) (nullable) (array length=out_n_ids): the ids
 */
gint64 *
venture_kb_service_resolve_slugs(
	VentureKbService	 *self,
	const gchar     *const	 *slugs,
	gsize			 *out_n_ids,
	GError			**error
);

/**
 * venture_kb_service_delete_chunks:
 * @self: the service
 * @article_id: the article
 * @error: (out) (optional): return location for a #GError
 *
 * Removes every passage of an article.
 *
 * Hard deletes rather than soft ones. A chunk carries no history worth
 * keeping -- it is derived from the article, and the article's history is
 * the audit trail that matters -- and a soft-deleted chunk would still be
 * read back by the search that walks the table.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_kb_service_delete_chunks(
	VentureKbService	 *self,
	gint64			  article_id,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_KB_SERVICE_H */
