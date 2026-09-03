/*
 * venture-embedding.h - Turning text into vectors, and comparing them
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A thin wrapper over ai-glib's #AiEmbedder: which provider and which model
 * come from kb.* configuration, and the storage format is this file's own.
 *
 * The embedding provider is configured separately from the assistant's on
 * purpose. They answer different questions -- the assistant is whichever
 * model writes well, the embedder is whichever model the corpus was indexed
 * with -- and only one of them can be changed freely. Every stored vector
 * was made by a particular model, so switching embedders invalidates the
 * index; switching chat models must not.
 *
 * Vectors arrive unit-normalised from ai-glib, which makes a stored vector
 * directly comparable to any other without the store remembering what scale
 * it was on. They are kept as base64 of little-endian float32 in a text
 * column, because neither backend has a vector type: SQLite has none, and
 * the PostgreSQL one lives in an extension this deployment does not
 * install. Little-endian is named rather than native so a database moved
 * between architectures still reads back what it stored.
 */

#ifndef VENTURE_EMBEDDING_H
#define VENTURE_EMBEDDING_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_EMBEDDER (venture_embedder_get_type())
G_DECLARE_FINAL_TYPE(VentureEmbedder, venture_embedder, VENTURE, EMBEDDER,
                     GObject)

/**
 * venture_embedder_new:
 * @config: the configuration to read the provider, URL and model from
 * @error: (out) (optional): return location for a #GError
 *
 * Builds an embedding client from kb.embedding_* configuration.
 *
 * Fails rather than falling back when the provider name is not one it
 * knows: an install that quietly indexed with the wrong service would
 * produce a corpus whose vectors are all in the wrong space, and nothing
 * about the results would look broken -- just uniformly poor.
 *
 * Returns: (transfer full) (nullable): the client, or %NULL
 */
VentureEmbedder *
venture_embedder_new(
	VentureConfig	 *config,
	GError		**error
);

/**
 * venture_embedder_get_provider:
 * @self: an embedder
 *
 * Which ai-glib provider is behind this, as named in configuration.
 *
 * Returns: (transfer none): the provider name
 */
const gchar *
venture_embedder_get_provider(VentureEmbedder *self);

/**
 * venture_embedder_get_dimensions:
 * @self: an embedder
 *
 * The vector width this model produces, or 0 when the provider does not
 * publish one.
 *
 * Zero is not an error: a local server may serve a model ai-glib's table
 * does not list, and the width is then whatever comes back. It is here so a
 * caller can refuse a model whose width disagrees with an already-indexed
 * base before writing anything, rather than after.
 *
 * Returns: the width, or 0 when unknown
 */
gsize
venture_embedder_get_dimensions(VentureEmbedder *self);

/**
 * venture_embedder_get_model:
 * @self: an embedder
 *
 * The model identifier vectors from this client were produced with.
 *
 * Stored beside every vector, because vectors from two models cannot be
 * compared: the numbers are in different spaces and the cosine between them
 * is noise that reads as a weak match rather than as an error.
 *
 * Returns: (transfer none): the model id
 */
const gchar *
venture_embedder_get_model(VentureEmbedder *self);

/**
 * venture_embedder_embed:
 * @self: an embedder
 * @text: the passage to embed
 * @out_dims: (out): where to put the vector's length
 * @error: (out) (optional): return location for a #GError
 *
 * Embeds one passage.
 *
 * Returns: (transfer full) (nullable) (array length=out_dims): a
 *   unit-normalised vector, or %NULL
 */
gfloat *
venture_embedder_embed(
	VentureEmbedder	 *self,
	const gchar	 *text,
	gsize		 *out_dims,
	GError		**error
);

/**
 * venture_embedder_embed_many:
 * @self: an embedder
 * @texts: (array zero-terminated=1): the passages
 * @out_dims: (out): where to put the vector length shared by every result
 * @error: (out) (optional): return location for a #GError
 *
 * Embeds several passages in one request.
 *
 * A document is many passages, and a round trip each is most of the cost of
 * indexing one. ai-glib refuses a response whose vector count does not match
 * the request, which matters because the results are paired with the inputs
 * positionally: a dropped input would attach every later vector to the wrong
 * passage, and no later check could tell.
 *
 * Returns: (transfer full) (nullable) (element-type gpointer): one
 *   #gfloat array per input, in order, or %NULL
 */
GPtrArray *
venture_embedder_embed_many(
	VentureEmbedder	 *self,
	const gchar     *const *texts,
	gsize		 *out_dims,
	GError		**error
);

/**
 * venture_embedding_encode:
 * @vector: (array length=dims): the vector
 * @dims: its length
 *
 * Encodes a vector for storage as base64 of little-endian float32.
 *
 * Little-endian is named rather than native so a database moved between
 * architectures still reads back the vectors it stored.
 *
 * Returns: (transfer full): the encoded vector
 */
gchar *
venture_embedding_encode(
	const gfloat	*vector,
	gsize		 dims
);

/**
 * venture_embedding_decode:
 * @encoded: base64 of little-endian float32
 * @out_dims: (out): where to put the vector's length
 *
 * Decodes a stored vector.
 *
 * Returns: (transfer full) (nullable) (array length=out_dims): the vector,
 *   or %NULL when the input is not a whole number of floats
 */
gfloat *
venture_embedding_decode(
	const gchar	*encoded,
	gsize		*out_dims
);

/**
 * venture_embedding_cosine:
 * @a: (array length=dims): one vector
 * @b: (array length=dims): the other
 * @dims: their shared length
 *
 * Cosine similarity, in [-1, 1].
 *
 * Returns 0.0 when either vector is all zeroes rather than dividing by it.
 * A zero vector is what an embedding of an empty passage looks like, and
 * "no similarity" is the honest answer.
 *
 * Returns: the similarity
 */
gdouble
venture_embedding_cosine(
	const gfloat	*a,
	const gfloat	*b,
	gsize		 dims
);

G_END_DECLS

#endif /* VENTURE_EMBEDDING_H */
