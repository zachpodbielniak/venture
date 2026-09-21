/*
 * venture-embedding.c - Turning text into vectors, and storing them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

struct _VentureEmbedder
{
	GObject parent_instance;

	AiEmbedder	*client;
	gchar		*provider;
	gchar		*model;
	gsize		 dims;
};

G_DEFINE_FINAL_TYPE(VentureEmbedder, venture_embedder, G_TYPE_OBJECT)

static void
venture_embedder_finalize(GObject *object)
{
	VentureEmbedder *self;

	self = VENTURE_EMBEDDER(object);

	g_clear_object(&self->client);
	g_clear_pointer(&self->provider, g_free);
	g_clear_pointer(&self->model, g_free);

	G_OBJECT_CLASS(venture_embedder_parent_class)->finalize(object);
}

static void
venture_embedder_class_init(VentureEmbedderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_embedder_finalize;
}

static void
venture_embedder_init(VentureEmbedder *self)
{
}

VentureEmbedder *
venture_embedder_new(VentureConfig *config, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		"Select an organization embedding binding; ambient kb.embedding_* credentials are no longer used");
	return NULL;
}

VentureEmbedder *venture_embedder_new_for_organization(VentureContext *context, gint64 organization_id, GError **error)
{
	VentureDatabase *database;
	g_autoptr(AiProvider) provider = NULL;
	VentureEmbedder *self;
	const AiEmbeddingModelInfo *info;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	database = venture_context_get_database(context);
	provider = venture_ai_provider_service_create_provider(
		venture_ai_provider_service_get(database), organization_id, "embedding",
		venture_access_policy_get_actor(venture_database_get_access_policy(database)), error);
	if (!provider) return NULL;
	if (!AI_IS_EMBEDDER(provider)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The selected organization provider cannot embed"); return NULL;
	}
	self = g_object_new(VENTURE_TYPE_EMBEDDER, NULL);
	self->client = g_object_ref(AI_EMBEDDER(provider));
	self->provider = g_strdup(ai_provider_get_name(provider));
	self->model = g_strdup(ai_embedder_get_default_embedding_model(self->client));
	info = ai_embedder_get_model_info(self->client, self->model);
	self->dims = info ? info->dimensions : 0;
	return self;
}

const gchar *
venture_embedder_get_model(VentureEmbedder *self)
{
	g_return_val_if_fail(VENTURE_IS_EMBEDDER(self), NULL);

	return self->model;
}

const gchar *
venture_embedder_get_provider(VentureEmbedder *self)
{
	g_return_val_if_fail(VENTURE_IS_EMBEDDER(self), NULL);

	return self->provider;
}

gsize
venture_embedder_get_dimensions(VentureEmbedder *self)
{
	g_return_val_if_fail(VENTURE_IS_EMBEDDER(self), 0);

	return self->dims;
}

GPtrArray *
venture_embedder_embed_many(
	VentureEmbedder	 *self,
	const gchar     *const *texts,
	gsize		 *out_dims,
	GError		**error
){
	g_autoptr(AiEmbedding) embedding = NULL;
	g_autoptr(GPtrArray) vectors = NULL;
	gsize dims;
	gsize count;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_EMBEDDER(self), NULL);
	g_return_val_if_fail(NULL != texts, NULL);
	g_return_val_if_fail(NULL != out_dims, NULL);

	*out_dims = 0;

	embedding = ai_embedder_embed(self->client, texts, self->model, NULL,
	                              error);

	if (NULL == embedding)
		return NULL;

	dims = ai_embedding_get_dimensions(embedding);
	count = ai_embedding_get_n_vectors(embedding);
	vectors = g_ptr_array_new_with_free_func(g_free);

	/*
	 * Copied out of the AiEmbedding rather than borrowed: the vectors
	 * belong to it and die with it, and every caller here outlives the
	 * request by storing what it got.
	 */
	for (i = 0; i < count; i++)
	{
		const gfloat *vector;
		gfloat *copy;

		vector = ai_embedding_get_vector(embedding, i);

		if (NULL == vector)
			continue;

		copy = g_new0(gfloat, dims);
		memcpy(copy, vector, dims * sizeof(gfloat));
		g_ptr_array_add(vectors, copy);
	}

	*out_dims = dims;

	return g_steal_pointer(&vectors);
}

gfloat *
venture_embedder_embed(
	VentureEmbedder	 *self,
	const gchar	 *text,
	gsize		 *out_dims,
	GError		**error
){
	g_autoptr(GPtrArray) vectors = NULL;
	const gchar *one[2];

	g_return_val_if_fail(VENTURE_IS_EMBEDDER(self), NULL);
	g_return_val_if_fail(NULL != out_dims, NULL);

	one[0] = (NULL != text) ? text : "";
	one[1] = NULL;

	vectors = venture_embedder_embed_many(self, one, out_dims, error);

	if (NULL == vectors)
		return NULL;

	if (0 == vectors->len)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "The embedding service returned no vector");
		return NULL;
	}

	/* Stolen out of the array so the free func does not take it. */
	return g_ptr_array_steal_index(vectors, 0);
}

gchar *
venture_embedding_encode(
	const gfloat	*vector,
	gsize		 dims
){
	g_autofree guchar *bytes = NULL;
	gsize i;

	g_return_val_if_fail(NULL != vector, NULL);
	g_return_val_if_fail(dims > 0, NULL);

	bytes = g_new0(guchar, dims * 4);

	for (i = 0; i < dims; i++)
	{
		/*
		 * Through a union rather than a cast: type-punning a float
		 * through a pointer is undefined, and -O2 is entitled to
		 * assume the two never alias.
		 */
		union { gfloat f; guint32 u; } bits;
		guint32 word;

		bits.f = vector[i];
		word = GUINT32_TO_LE(bits.u);
		memcpy(bytes + (i * 4), &word, 4);
	}

	return g_base64_encode(bytes, dims * 4);
}

gfloat *
venture_embedding_decode(
	const gchar	*encoded,
	gsize		*out_dims
){
	g_autofree guchar *bytes = NULL;
	g_autofree gfloat *vector = NULL;
	gsize length = 0;
	gsize dims;
	gsize i;

	g_return_val_if_fail(NULL != out_dims, NULL);

	*out_dims = 0;

	if (venture_string_is_empty(encoded))
		return NULL;

	bytes = g_base64_decode(encoded, &length);

	/*
	 * A length that is not a whole number of floats is refused rather
	 * than truncated. It means the column holds something that is not a
	 * vector, and half-reading it would produce a plausible-looking score
	 * against whatever the bytes happened to be.
	 */
	if ((NULL == bytes) || (0 == length) || (0 != (length % 4)))
		return NULL;

	dims = length / 4;
	vector = g_new0(gfloat, dims);

	for (i = 0; i < dims; i++)
	{
		union { gfloat f; guint32 u; } bits;
		guint32 word;

		memcpy(&word, bytes + (i * 4), 4);
		bits.u = GUINT32_FROM_LE(word);
		vector[i] = bits.f;
	}

	*out_dims = dims;

	return g_steal_pointer(&vector);
}

gdouble
venture_embedding_cosine(
	const gfloat	*a,
	const gfloat	*b,
	gsize		 dims
){
	/*
	 * ai-glib's, so there is one implementation of this arithmetic and
	 * one place for its zero-vector and scale behaviour to be defined.
	 */
	return ai_embedding_cosine(a, b, dims);
}
