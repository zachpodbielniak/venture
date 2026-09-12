/*
 * venture-ai-models.h - which models a provider has, and what effort it takes
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A catalogue, so a form can offer the models a provider actually has
 * rather than a text box and a guess.
 *
 * None of it is written by hand. ai-glib declares every model it knows as
 * a constant in its provider headers, and tools/venture-models.sh turns
 * those into a table at build time; the provider list comes from
 * #AiProviderType. Both therefore move when the submodule does, which is
 * the only way a list like this stays true.
 */

#ifndef VENTURE_AI_MODELS_H
#define VENTURE_AI_MODELS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * venture_ai_providers:
 *
 * Every provider ai-glib can construct, by the name it answers to.
 *
 * Returns: (transfer none) (array zero-terminated=1): the provider names
 */
const gchar *const *
venture_ai_providers(void);

/**
 * venture_ai_provider_is_cli:
 * @provider: a provider name
 *
 * Whether @provider drives a command-line agent rather than an HTTP API.
 *
 * It is the distinction that decides nearly everything else about a
 * session: a CLI provider is a subprocess started in the workspace that
 * brings its own tools, and it is the kind that takes an effort level.
 *
 * Returns: %TRUE for a CLI provider
 */
gboolean
venture_ai_provider_is_cli(const gchar *provider);

/**
 * venture_ai_provider_models:
 * @provider: a provider name
 *
 * The models ai-glib names for @provider, or %NULL when it names none --
 * openai-compatible, where the models are whatever the configured
 * endpoint happens to serve and no list could be right.
 *
 * The list is what ai-glib knows, not what is installed: it names models
 * for ollama, but which of them have actually been pulled is a question
 * only that daemon can answer. A model outside the list can still be
 * typed, which is what the form falls back to.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the models
 */
const gchar *const *
venture_ai_provider_models(const gchar *provider);

/**
 * venture_ai_provider_default_model:
 * @provider: a provider name
 *
 * What @provider uses when nothing is chosen, or %NULL when ai-glib
 * declares no default and the provider decides for itself.
 *
 * Returns: (transfer none) (nullable): the model
 */
const gchar *
venture_ai_provider_default_model(const gchar *provider);

/**
 * venture_ai_provider_efforts:
 * @provider: a provider name
 *
 * The effort levels @provider accepts, or %NULL when it takes none.
 *
 * Effort is a property of the provider rather than of the model, because
 * that is how ai-glib passes it: a flag on the command line, spelled
 * differently per CLI and folded onto whatever that CLI supports. Two
 * providers take none at all -- an HTTP API has no such flag, and cursor
 * bakes the level into the model id, so its "effort" is chosen by picking
 * `claude-opus-5-high` over `claude-opus-5-low`.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the levels
 */
const gchar *const *
venture_ai_provider_efforts(const gchar *provider);

G_END_DECLS

#endif /* VENTURE_AI_MODELS_H */
