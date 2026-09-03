/*
 * venture-kb-service.c - Indexing and searching knowledge bases
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

struct _VentureKbService
{
	GObject parent_instance;

	VentureContext	*context;
	VentureEmbedder	*embedder;
	gsize		 chunk_chars;
	gsize		 chunk_overlap;
	guint		 search_limit;
};

G_DEFINE_FINAL_TYPE(VentureKbService, venture_kb_service, G_TYPE_OBJECT)

static void
venture_kb_service_finalize(GObject *object)
{
	VentureKbService *self;

	self = VENTURE_KB_SERVICE(object);

	g_clear_object(&self->context);
	g_clear_object(&self->embedder);

	G_OBJECT_CLASS(venture_kb_service_parent_class)->finalize(object);
}

static void
venture_kb_service_class_init(VentureKbServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_kb_service_finalize;
}

static void
venture_kb_service_init(VentureKbService *self)
{
}

void
venture_kb_hit_free(VentureKbHit *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->title, g_free);
	g_clear_pointer(&self->kb_slug, g_free);
	g_clear_pointer(&self->heading, g_free);
	g_clear_pointer(&self->text, g_free);
	g_free(self);
}

VentureKbService *
venture_kb_service_new(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(VentureKbService) self = NULL;
	VentureConfig *config;
	gboolean enabled;
	gint64 chunk_chars;
	gint64 chunk_overlap;
	gint64 search_limit;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	config = venture_context_get_config(context);
	g_object_get(config,
	             "kb-enabled", &enabled,
	             "kb-chunk-chars", &chunk_chars,
	             "kb-chunk-overlap", &chunk_overlap,
	             "kb-search-limit", &search_limit,
	             NULL);

	if (!enabled)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Knowledge bases are disabled in configuration");
		return NULL;
	}

	self = g_object_new(VENTURE_TYPE_KB_SERVICE, NULL);
	self->context = g_object_ref(context);
	self->embedder = venture_embedder_new(config, error);

	if (NULL == self->embedder)
		return NULL;

	self->chunk_chars = (chunk_chars > 0) ? (gsize)chunk_chars : 1200;
	self->chunk_overlap = (chunk_overlap >= 0) ? (gsize)chunk_overlap : 200;
	self->search_limit = (search_limit > 0) ? (guint)search_limit : 8;

	return g_steal_pointer(&self);
}

VentureContext *
venture_kb_service_get_context(VentureKbService *self)
{
	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(self), NULL);

	return self->context;
}

VentureEmbedder *
venture_kb_service_get_embedder(VentureKbService *self)
{
	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(self), NULL);

	return self->embedder;
}

/*
 * Every chunk of an article, including ones a previous model wrote.
 */
static GPtrArray *
venture_kb_service_chunks_of(
	VentureKbService	 *self,
	gint64			  article_id,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_KB_CHUNK);
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "article-id",
	                                  VENTURE_FILTER_OP_EQ, article_id,
	                                  error))
		return NULL;

	return venture_database_find(
		venture_context_get_database(self->context), query, error);
}

gboolean
venture_kb_service_delete_chunks(
	VentureKbService	 *self,
	gint64			  article_id,
	GError			**error
){
	g_autoptr(GPtrArray) chunks = NULL;
	VentureDatabase *database;
	guint i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(self), FALSE);

	database = venture_context_get_database(self->context);
	chunks = venture_kb_service_chunks_of(self, article_id, error);

	if (NULL == chunks)
		return FALSE;

	for (i = 0; i < chunks->len; i++)
	{
		/*
		 * Purged, not soft deleted. A chunk is derived from its
		 * article and carries no history of its own -- the article's
		 * audit trail is the one that matters -- and a soft-deleted
		 * chunk would still be read back by the search that walks
		 * this table, so every reindex would make the corpus slower
		 * and the old text would stay findable.
		 */
		if (!venture_database_purge(database,
		                            g_ptr_array_index(chunks, i),
		                            NULL, error))
			return FALSE;
	}

	return TRUE;
}

/*
 * Records the model a base is indexed with, and refuses a second one.
 *
 * Vectors from two models are not comparable: the numbers are in different
 * spaces and the cosine between them is noise that reads exactly like a weak
 * match. A base must therefore be entirely one model's work, and the moment
 * to notice otherwise is before writing rather than during a search that
 * quietly returns the wrong passages.
 */
static gboolean
venture_kb_service_claim_model(
	VentureKbService	 *self,
	gint64			  kb_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) entity = NULL;
	g_autofree gchar *existing = NULL;
	const gchar *model;
	gint64 dims;

	model = venture_embedder_get_model(self->embedder);
	entity = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_KNOWLEDGE_BASE, kb_id, error);

	if (NULL == entity)
		return FALSE;

	g_object_get(entity, "embedding-model", &existing, NULL);

	if (!venture_string_is_empty(existing))
	{
		if (0 != g_strcmp0(existing, model))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			            "This knowledge base was indexed with \"%s\" and "
			            "the configured model is \"%s\". Vectors from two "
			            "models cannot be compared -- reindex the base "
			            "with --force to move it.",
			            existing, model);
			return FALSE;
		}

		return TRUE;
	}

	dims = (gint64)venture_embedder_get_dimensions(self->embedder);
	g_object_set(entity, "embedding-model", model, NULL);

	if (dims > 0)
		g_object_set(entity, "embedding-dims", dims, NULL);

	return venture_database_save(
		venture_context_get_database(self->context), entity, actor, error);
}

gint
venture_kb_service_index_article(
	VentureKbService	 *self,
	gint64			  article_id,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureEntity) article = NULL;
	g_autoptr(GPtrArray) passages = NULL;
	g_autoptr(GPtrArray) vectors = NULL;
	g_autoptr(GPtrArray) texts = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *body = NULL;
	VentureDatabase *database;
	VentureKbFormat format;
	const gchar *model;
	gint64 kb_id;
	gsize dims = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(self), -1);

	database = venture_context_get_database(self->context);
	article = venture_database_get(database, VENTURE_TYPE_KB_ARTICLE,
	                               article_id, error);

	if (NULL == article)
		return -1;

	g_object_get(article, "body", &body, "kb-id", &kb_id, "format", &format,
	             NULL);

	if (0 == kb_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "This article belongs to no knowledge base");
		return -1;
	}

	if (!venture_kb_service_claim_model(self, kb_id, actor, error))
		return -1;

	model = venture_embedder_get_model(self->embedder);
	passages = venture_kb_chunk_text(body, format, self->chunk_chars,
	                                 self->chunk_overlap);

	/*
	 * An empty article is not a failure. It is a stub somebody has
	 * created and not written yet, and it should stop being findable
	 * rather than keep whatever it used to say.
	 */
	if (0 == passages->len)
	{
		if (!venture_kb_service_delete_chunks(self, article_id, error))
			return -1;

		now = venture_time_now();
		g_object_set(article, "embedded-at", now, "embedding-model",
		             model, NULL);

		if (!venture_database_save(database, article, actor, error))
			return -1;

		return 0;
	}

	texts = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < passages->len; i++)
		g_ptr_array_add(texts, venture_kb_chunk_embed_text(
			g_ptr_array_index(passages, i)));

	g_ptr_array_add(texts, NULL);

	vectors = venture_embedder_embed_many(
		self->embedder, (const gchar *const *)texts->pdata, &dims, error);

	if (NULL == vectors)
		return -1;

	if (vectors->len != passages->len)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Asked for %u passages and got %u vectors",
		            passages->len, vectors->len);
		return -1;
	}

	/*
	 * The whole replacement is one transaction. A failure part-way would
	 * otherwise leave an article with some of its old passages and some
	 * of its new ones, which is worse than either: the stale ones stay
	 * findable and nothing says which is which.
	 */
	if (!venture_database_begin(database, error))
		return -1;

	if (!venture_kb_service_delete_chunks(self, article_id, error))
	{
		venture_database_rollback(database);
		return -1;
	}

	now = venture_time_now();

	for (i = 0; i < passages->len; i++)
	{
		const VentureKbPassage *passage;
		g_autoptr(VentureKbChunk) chunk = NULL;
		g_autofree gchar *encoded = NULL;

		passage = g_ptr_array_index(passages, i);
		encoded = venture_embedding_encode(g_ptr_array_index(vectors, i),
		                                   dims);

		chunk = venture_kb_chunk_new();
		g_object_set(chunk,
		             "article-id", article_id,
		             "kb-id", kb_id,
		             "ordinal", (gint64)passage->ordinal,
		             "text", passage->text,
		             "heading", passage->heading,
		             "embedding", encoded,
		             "dims", (gint64)dims,
		             "embedding-model", model,
		             "char-count", (gint64)g_utf8_strlen(passage->text, -1),
		             NULL);

		venture_entity_set_organization_id(VENTURE_ENTITY(chunk),
			venture_entity_get_organization_id(article));

		if (!venture_database_save(database, VENTURE_ENTITY(chunk), actor,
		                           error))
		{
			venture_database_rollback(database);
			return -1;
		}
	}

	g_object_set(article, "embedded-at", now, "embedding-model", model,
	             NULL);

	if (!venture_database_save(database, article, actor, error))
	{
		venture_database_rollback(database);
		return -1;
	}

	if (!venture_database_commit(database, error))
		return -1;

	return (gint)passages->len;
}

gint
venture_kb_service_reindex(
	VentureKbService	 *self,
	gint64			  kb_id,
	gboolean		  force,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) articles = NULL;
	const gchar *model;
	gint indexed = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(self), -1);

	model = venture_embedder_get_model(self->embedder);
	query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
	venture_query_set_limit(query, 0);

	if (0 != kb_id)
	{
		if (!venture_query_add_filter_int(query, "kb-id",
		                                  VENTURE_FILTER_OP_EQ, kb_id,
		                                  error))
			return -1;
	}

	articles = venture_database_find(
		venture_context_get_database(self->context), query, error);

	if (NULL == articles)
		return -1;

	for (i = 0; i < articles->len; i++)
	{
		VentureEntity *article = g_ptr_array_index(articles, i);
		g_autoptr(GDateTime) embedded_at = NULL;
		g_autofree gchar *article_model = NULL;

		g_object_get(article, "embedded-at", &embedded_at,
		             "embedding-model", &article_model, NULL);

		/*
		 * Skipped when it is already this model's work, which is what
		 * makes a reindex cheap to run repeatedly. An article indexed
		 * by a different model is never skipped, force or not: its
		 * vectors cannot be compared with the rest of the base.
		 */
		if (!force && (NULL != embedded_at) &&
		    (0 == g_strcmp0(article_model, model)))
			continue;

		if (venture_kb_service_index_article(
			self, venture_entity_get_id(article), actor, error) < 0)
			return -1;

		indexed++;
	}

	return indexed;
}

gint64 *
venture_kb_service_resolve_slugs(
	VentureKbService	 *self,
	const gchar     *const	 *slugs,
	gsize			 *out_n_ids,
	GError			**error
){
	g_autofree gint64 *ids = NULL;
	gsize count = 0;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(self), NULL);
	g_return_val_if_fail(NULL != slugs, NULL);
	g_return_val_if_fail(NULL != out_n_ids, NULL);

	*out_n_ids = 0;

	while (NULL != slugs[count])
		count++;

	if (0 == count)
		return NULL;

	ids = g_new0(gint64, count);

	for (i = 0; i < count; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) found = NULL;

		query = venture_query_new(VENTURE_TYPE_KNOWLEDGE_BASE);
		venture_query_set_limit(query, 1);

		if (!venture_query_add_filter_string(query, "slug",
		                                     VENTURE_FILTER_OP_EQ,
		                                     slugs[i], error))
			return NULL;

		found = venture_database_find(
			venture_context_get_database(self->context), query, error);

		if (NULL == found)
			return NULL;

		/*
		 * Named rather than skipped. Somebody who typed #handbok and
		 * silently got answers from everywhere else would reasonably
		 * believe the handbook had been searched and had nothing to
		 * say.
		 */
		if (0 == found->len)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "There is no knowledge base called \"%s\"",
			            slugs[i]);
			return NULL;
		}

		ids[i] = venture_entity_get_id(g_ptr_array_index(found, 0));
	}

	*out_n_ids = count;

	return g_steal_pointer(&ids);
}

/*
 * The bases to search when the caller named none: those marked
 * auto-retrieve.
 *
 * A base of drafts, or one kept for export, should not silently answer every
 * question -- so participation is a property of the base rather than a
 * default.
 */
static gint64 *
venture_kb_service_auto_bases(
	VentureKbService	 *self,
	gsize			 *out_count,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) bases = NULL;
	g_autofree gint64 *ids = NULL;
	guint i;

	*out_count = 0;

	query = venture_query_new(VENTURE_TYPE_KNOWLEDGE_BASE);
	venture_query_set_limit(query, 0);

	bases = venture_database_find(
		venture_context_get_database(self->context), query, error);

	if (NULL == bases)
		return NULL;

	if (0 == bases->len)
		return NULL;

	ids = g_new0(gint64, bases->len);

	for (i = 0; i < bases->len; i++)
	{
		VentureEntity *base = g_ptr_array_index(bases, i);
		gboolean automatic = FALSE;

		g_object_get(base, "auto-retrieve", &automatic, NULL);

		if (automatic)
			ids[(*out_count)++] = venture_entity_get_id(base);
	}

	if (0 == *out_count)
		return NULL;

	return g_steal_pointer(&ids);
}

typedef struct
{
	gchar	*title;
	gchar	*kb_slug;
	gint64	 kb_id;
} VentureKbArticleInfo;

static void
venture_kb_article_info_free(VentureKbArticleInfo *info)
{
	if (NULL == info)
		return;

	g_clear_pointer(&info->title, g_free);
	g_clear_pointer(&info->kb_slug, g_free);
	g_free(info);
}

static gint
venture_kb_hit_compare(
	gconstpointer	 a,
	gconstpointer	 b
){
	const VentureKbHit *left = *(const VentureKbHit *const *)a;
	const VentureKbHit *right = *(const VentureKbHit *const *)b;

	if (left->score < right->score)
		return 1;

	if (left->score > right->score)
		return -1;

	return 0;
}

GPtrArray *
venture_kb_service_search(
	VentureKbService	 *self,
	const gchar		 *query_text,
	const gint64		 *kb_ids,
	gsize			  n_kb_ids,
	guint			  limit,
	GError			**error
){
	g_autoptr(GHashTable) articles = NULL;
	g_autoptr(GHashTable) slugs = NULL;
	g_autoptr(GPtrArray) hits = NULL;
	g_autofree gint64 *automatic = NULL;
	g_autofree gfloat *query_vector = NULL;
	VentureDatabase *database;
	const gchar *model;
	gsize query_dims = 0;
	gsize count;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_KB_SERVICE(self), NULL);

	if (venture_string_is_empty(query_text))
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "There is nothing to search for");
		return NULL;
	}

	database = venture_context_get_database(self->context);
	model = venture_embedder_get_model(self->embedder);

	if (0 == limit)
		limit = self->search_limit;

	count = n_kb_ids;

	if (0 == count)
	{
		automatic = venture_kb_service_auto_bases(self, &count, error);

		if ((NULL == automatic) && (NULL != error) && (NULL != *error))
			return NULL;

		kb_ids = automatic;
	}

	/*
	 * No base to search is an empty result, not an error. An install that
	 * has not made one yet should get "nothing found" from the assistant
	 * rather than a failure it has to explain.
	 */
	if ((NULL == kb_ids) || (0 == count))
		return g_ptr_array_new_with_free_func(
			(GDestroyNotify)venture_kb_hit_free);

	/* The base slugs, so a result can say where it came from. */
	slugs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                              g_free);

	for (i = 0; i < count; i++)
	{
		g_autoptr(VentureEntity) base = NULL;
		g_autofree gchar *slug = NULL;
		gint64 *key;

		base = venture_database_get(database, VENTURE_TYPE_KNOWLEDGE_BASE,
		                            kb_ids[i], NULL);

		if (NULL == base)
			continue;

		g_object_get(base, "slug", &slug, NULL);
		key = g_new0(gint64, 1);
		*key = kb_ids[i];
		g_hash_table_insert(slugs, key, g_steal_pointer(&slug));
	}

	/*
	 * Published articles of those bases, gathered first so a chunk can be
	 * accepted or rejected without a query of its own. A draft is
	 * knowledge somebody is still deciding about, and an assistant citing
	 * one is quoting a document its author has not stood behind.
	 */
	articles = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                 (GDestroyNotify)venture_kb_article_info_free);

	for (i = 0; i < count; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) found = NULL;
		guint j;

		query = venture_query_new(VENTURE_TYPE_KB_ARTICLE);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter_int(query, "kb-id",
		                                  VENTURE_FILTER_OP_EQ, kb_ids[i],
		                                  error))
			return NULL;

		found = venture_database_find(database, query, error);

		if (NULL == found)
			return NULL;

		for (j = 0; j < found->len; j++)
		{
			VentureEntity *article = g_ptr_array_index(found, j);
			VentureKbArticleInfo *info;
			VentureKbArticleStatus status;
			gint64 *key;

			g_object_get(article, "status", &status, NULL);

			if (VENTURE_KB_ARTICLE_STATUS_PUBLISHED != status)
				continue;

			info = g_new0(VentureKbArticleInfo, 1);
			info->kb_id = kb_ids[i];
			info->kb_slug = g_strdup(g_hash_table_lookup(slugs,
			                                             &kb_ids[i]));
			g_object_get(article, "title", &info->title, NULL);

			key = g_new0(gint64, 1);
			*key = venture_entity_get_id(article);
			g_hash_table_insert(articles, key, info);
		}
	}

	if (0 == g_hash_table_size(articles))
		return g_ptr_array_new_with_free_func(
			(GDestroyNotify)venture_kb_hit_free);

	query_vector = venture_embedder_embed(self->embedder, query_text,
	                                      &query_dims, error);

	if (NULL == query_vector)
		return NULL;

	hits = g_ptr_array_new_with_free_func((GDestroyNotify)venture_kb_hit_free);

	for (i = 0; i < count; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) chunks = NULL;
		guint j;

		query = venture_query_new(VENTURE_TYPE_KB_CHUNK);
		venture_query_set_limit(query, 0);

		if (!venture_query_add_filter_int(query, "kb-id",
		                                  VENTURE_FILTER_OP_EQ, kb_ids[i],
		                                  error))
			return NULL;

		chunks = venture_database_find(database, query, error);

		if (NULL == chunks)
			return NULL;

		for (j = 0; j < chunks->len; j++)
		{
			VentureEntity *chunk = g_ptr_array_index(chunks, j);
			const VentureKbArticleInfo *info;
			g_autofree gchar *encoded = NULL;
			g_autofree gchar *chunk_model = NULL;
			g_autofree gfloat *vector = NULL;
			VentureKbHit *hit;
			gint64 article_id;
			gsize dims = 0;

			g_object_get(chunk, "article-id", &article_id,
			             "embedding", &encoded,
			             "embedding-model", &chunk_model, NULL);

			info = g_hash_table_lookup(articles, &article_id);

			if (NULL == info)
				continue;

			/*
			 * A passage embedded by another model is skipped
			 * rather than scored. Comparing across models produces
			 * a number in [-1, 1] that looks exactly like a weak
			 * match, so including them would not fail -- it would
			 * quietly rank real answers below noise.
			 */
			if (0 != g_strcmp0(chunk_model, model))
				continue;

			vector = venture_embedding_decode(encoded, &dims);

			if ((NULL == vector) || (dims != query_dims))
				continue;

			hit = g_new0(VentureKbHit, 1);
			hit->article_id = article_id;
			hit->kb_id = info->kb_id;
			hit->chunk_id = venture_entity_get_id(chunk);
			hit->score = venture_embedding_cosine(query_vector, vector,
			                                      dims);
			hit->title = g_strdup(info->title);
			hit->kb_slug = g_strdup(info->kb_slug);

			g_object_get(chunk, "ordinal", &hit->ordinal,
			             "heading", &hit->heading,
			             "text", &hit->text, NULL);

			g_ptr_array_add(hits, hit);
		}
	}

	g_ptr_array_sort(hits, venture_kb_hit_compare);

	while (hits->len > limit)
		g_ptr_array_remove_index(hits, hits->len - 1);

	return g_steal_pointer(&hits);
}
