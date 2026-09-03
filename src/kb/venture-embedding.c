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
venture_embedder_new(
	VentureConfig	 *config,
	GError		**error
){
	g_autoptr(VentureEmbedder) self = NULL;
	g_autoptr(GObject) client = NULL;
	g_autofree gchar *provider = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *model = NULL;
	g_autofree gchar *key_env = NULL;
	const AiEmbeddingModelInfo *info;
	AiProviderType provider_type;
	AiConfig *ai_config;

	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);

	g_object_get(config,
	             "kb-embedding-provider", &provider,
	             "kb-embedding-url", &url,
	             "kb-embedding-model", &model,
	             "kb-embedding-key-env", &key_env,
	             NULL);

	/*
	 * Deliberately its own provider and model, read from kb.* rather than
	 * ai.*. The assistant and the index answer different questions: the
	 * assistant is whichever model writes well, and embedding is whichever
	 * model the corpus was indexed with -- and that one cannot be changed
	 * casually, because every stored vector was made by it. Tying them
	 * together would mean switching chat models silently invalidated the
	 * whole index.
	 *
	 * It also allows the arrangement most installs want: a hosted model
	 * for the conversation, a local one for the documents, so the corpus
	 * never leaves the machine.
	 */
	if (0 == g_strcmp0(provider, "ollama"))
	{
		client = G_OBJECT(ai_ollama_client_new());
		provider_type = AI_PROVIDER_OLLAMA;
	}
	else if ((0 == g_strcmp0(provider, "openai")) ||
	         (0 == g_strcmp0(provider, "openai-compatible")))
	{
		client = G_OBJECT(ai_openai_client_new());
		provider_type = AI_PROVIDER_OPENAI;
	}
	else
	{
		/*
		 * Refused rather than defaulted. Falling back to ollama here
		 * would index the whole corpus with a model nobody chose, and
		 * the result would look like weak retrieval rather than like a
		 * configuration error.
		 */
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "\"%s\" is not an embedding provider I know. Use "
		            "\"ollama\", or \"openai\" for any OpenAI-compatible "
		            "endpoint.",
		            (NULL != provider) ? provider : "");
		return NULL;
	}

	/*
	 * ai-glib implements embedding on the Ollama and OpenAI clients only.
	 * Claude has no embeddings API and Gemini's is shaped differently, so
	 * this cannot currently fail -- but checking it here means adding a
	 * provider above without an AiEmbedder fails at construction rather
	 * than at the first indexing run.
	 */
	if (!AI_IS_EMBEDDER(client))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "The %s client cannot produce embeddings", provider);
		return NULL;
	}

	self = g_object_new(VENTURE_TYPE_EMBEDDER, NULL);
	self->client = AI_EMBEDDER(g_steal_pointer(&client));
	self->provider = g_strdup(provider);

	ai_config = ai_client_get_config(AI_CLIENT(self->client));

	if (!venture_string_is_empty(url))
		ai_config_set_base_url(ai_config, provider_type, url);

	/*
	 * Named, not stored. The same indirection as the database password:
	 * a key written into the config file is a key in every backup of it.
	 */
	if (!venture_string_is_empty(key_env))
	{
		const gchar *value;

		value = g_getenv(key_env);

		if (!venture_string_is_empty(value))
			ai_config_set_api_key(ai_config, provider_type, value);
	}

	self->model = venture_string_is_empty(model)
		? g_strdup(ai_embedder_get_default_embedding_model(self->client))
		: g_strdup(model);

	/*
	 * The width, when the provider publishes one. It is not required --
	 * an unlisted local model is passed through and the server decides --
	 * but knowing it lets a mismatch against an already-indexed base be
	 * reported before anything is written rather than after.
	 */
	info = ai_embedder_get_model_info(self->client, self->model);
	self->dims = (NULL != info) ? info->dimensions : 0;

	return g_steal_pointer(&self);
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
