/*
 * venture-ai-factory.h - The assistant on the factory floor
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Four pieces of writing the factory asks of somebody at exactly the
 * moment they have least time for it: what a release means to the people
 * who use it, why an incident happened, what a red build is actually
 * complaining about, and where everything stands this morning. A model is
 * good at all four, because all four are reading a pile of records and
 * saying what they amount to.
 *
 * Like the desk's judgements, each reads and answers and none writes. A
 * draft is a draft: applying one to a record is a separate save, under the
 * same policy and audit as any other change. And like them, every prompt
 * goes down the toolless path and says the text is data -- a ticket title
 * and a build log are both written by somebody else.
 *
 * The facts come from venture-factory.c, not from the model. The briefing
 * narrates venture_factory_next_actions(); it does not decide what is on
 * the list.
 */

#ifndef VENTURE_AI_FACTORY_H
#define VENTURE_AI_FACTORY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_ai_factory_release_notes:
 * @context: the wiring
 * @release: the release
 * @audience: (nullable): who it is for, in a few words; customers when %NULL
 * @error: (out) (optional): return location for a #GError
 *
 * Release notes for the people who use the product, from the tickets the
 * release carries and its changelog: what is new, what is fixed, anything
 * they must do. The changelog lists tickets; this says what they mean.
 * Nothing is written to the release.
 *
 * Returns: (transfer full) (nullable): the notes as Markdown, or %NULL
 */
gchar *
venture_ai_factory_release_notes(
	VentureContext	 *context,
	VentureEntity	 *release,
	const gchar	 *audience,
	GError		**error
);

/**
 * venture_ai_factory_postmortem:
 * @context: the wiring
 * @incident: the incident
 * @error: (out) (optional): return location for a #GError
 *
 * Drafts a blameless postmortem from what the records say: the incident,
 * where and what was running, the deployment that introduced it, the fix
 * ticket and its thread. Sections for what happened, the impact, the
 * timeline, the cause, and what changes. It is told to say where the
 * records do not say, rather than to fill the gap. Nothing is written to
 * the incident.
 *
 * Returns: (transfer full) (nullable): the draft as Markdown, or %NULL
 */
gchar *
venture_ai_factory_postmortem(
	VentureContext	 *context,
	VentureEntity	 *incident,
	GError		**error
);

/**
 * venture_ai_factory_build_triage:
 * @context: the wiring
 * @build: a build, usually a failed one
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a build and its log excerpt and says what went wrong: a
 * `category` (one of compile, test, lint, dependency, infrastructure,
 * flaky, configuration, unknown), a one-sentence `summary`, the `cause`
 * as far as the log shows it, a `suggestion` for the fix, whether a
 * `retry` alone is likely to pass, and a `confidence` of low, medium or
 * high. A build with no log excerpt is refused: there is nothing to read,
 * and a model will otherwise guess.
 *
 * Returns: (transfer full) (nullable): a JSON object, or %NULL on error
 */
JsonNode *
venture_ai_factory_build_triage(
	VentureContext	 *context,
	VentureEntity	 *build,
	GError		**error
);

/**
 * venture_ai_factory_briefing:
 * @context: the wiring
 * @organization_ids: (array length=n_organizations) (nullable): scope
 * @n_organizations: how many
 * @error: (out) (optional): return location for a #GError
 *
 * Where the factory stands and what needs somebody, as a few short
 * paragraphs for the start of a day: venture_factory_describe() and
 * venture_factory_next_actions(), narrated. The model orders and words
 * it; it is told to add nothing that is not in those two.
 *
 * Returns: (transfer full) (nullable): the briefing, or %NULL on error
 */
gchar *
venture_ai_factory_briefing(
	VentureContext	 *context,
	const gint64	 *organization_ids,
	gsize		  n_organizations,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_AI_FACTORY_H */
