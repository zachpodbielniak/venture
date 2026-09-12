/*
 * venture-ai-models.c - which models a provider has, and what effort it takes
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/*
 * One provider's models, as the generator writes them.
 *
 * Declared here rather than in the header because the generated file is
 * the only thing that builds one, and a type in a header invites somebody
 * to write a second table by hand.
 */
typedef struct
{
	const gchar		*provider;
	const gchar *const	*models;
	const gchar		*fallback;
} VentureModelSet;

#include "venture-models.h"

/*
 * The providers, from ai-glib's own enumeration.
 *
 * Walked rather than listed: ai_provider_type_to_string() is the naming
 * authority, and a provider added to the enum appears here without anybody
 * editing this file. The walk stops at the first name the library does not
 * recognise, which is how it finds the end.
 */
static const gchar *const *
venture_ai_providers_build(void)
{
	static const gchar *names[32];
	static gsize initialized = 0;

	if (g_once_init_enter(&initialized))
	{
		gsize n;
		gint i;

		n = 0;

		for (i = 0; (i < 31) && (n < 31); i++)
		{
			const gchar *name;

			name = ai_provider_type_to_string((AiProviderType)i);

			if ((NULL == name) || (0 == g_strcmp0(name, "unknown")))
				break;

			names[n++] = name;
		}

		names[n] = NULL;
		g_once_init_leave(&initialized, 1);
	}

	return names;
}

const gchar *const *
venture_ai_providers(void)
{
	return venture_ai_providers_build();
}

/*
 * The CLI providers, by name.
 *
 * A list rather than a test on the constructed object, because answering
 * "is this a CLI provider" by building one would spawn nothing but would
 * still read configuration and resolve an executable, for a question a
 * form asks about every provider on every page load.
 */
gboolean
venture_ai_provider_is_cli(const gchar *provider)
{
	static const gchar *const cli[] = {
		"claude-code", "claude-tmux", "codex-cli", "cursor", "opencode",
		"grok-build", "antigravity", NULL
	};

	return (NULL != provider) && g_strv_contains(cli, provider);
}

static const VentureModelSet *
venture_ai_model_set(const gchar *provider)
{
	gsize i;

	if (NULL == provider)
		return NULL;

	for (i = 0; NULL != venture_model_sets[i].provider; i++)
	{
		if (0 == g_strcmp0(venture_model_sets[i].provider, provider))
			return &venture_model_sets[i];
	}

	return NULL;
}

const gchar *const *
venture_ai_provider_models(const gchar *provider)
{
	const VentureModelSet *set;

	set = venture_ai_model_set(provider);

	if ((NULL == set) || (NULL == set->models) || (NULL == set->models[0]))
		return NULL;

	return set->models;
}

const gchar *
venture_ai_provider_default_model(const gchar *provider)
{
	const VentureModelSet *set;

	set = venture_ai_model_set(provider);

	if ((NULL == set) || venture_string_is_empty(set->fallback))
		return NULL;

	return set->fallback;
}

const gchar *const *
venture_ai_provider_efforts(const gchar *provider)
{
	/*
	 * The base set ai-glib's CLI client validates. Each provider folds
	 * what it cannot express -- grok-build turns "max" into "xhigh",
	 * antigravity turns both onto "high" -- so offering all four is
	 * honest about what may be asked for rather than about what each
	 * one does with it.
	 */
	static const gchar *const levels[] = {
		"low", "medium", "high", "max", NULL
	};

	if (!venture_ai_provider_is_cli(provider))
		return NULL;

	/* Cursor has no effort flag: the level is part of the model id, so
	 * the model list is already the effort list. */
	if (0 == g_strcmp0(provider, "cursor"))
		return NULL;

	return levels;
}
