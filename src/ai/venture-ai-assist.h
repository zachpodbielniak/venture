/*
 * venture-ai-assist.h - The assistant at the ticket desk
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three judgements a model is genuinely good at and a form cannot make:
 * what shape of work a ticket is and how urgent, what a long thread
 * actually says, and what the reply ought to open with. Each reads the
 * ticket and answers; none of them writes. Applying a triage is a
 * separate call, so it goes through the same staging and audit as any
 * other change.
 *
 * The ticket's text is somebody else's writing -- often a stranger's --
 * so every prompt here is run through the toolless path
 * (venture_ai_service_complete()) and says outright that the text is
 * data. A model that cannot reach a tool cannot be talked into using
 * one.
 */

#ifndef VENTURE_AI_ASSIST_H
#define VENTURE_AI_ASSIST_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_ai_assist_available:
 * @context: the wiring
 *
 * Whether there is an assistant to ask. The pages use this to offer the
 * buttons or leave them out, rather than offering one that always fails.
 *
 * Returns: %TRUE when the AI module is on and a provider is configured
 */
gboolean
venture_ai_assist_available(VentureContext *context);

/**
 * venture_ai_assist_triage:
 * @context: the wiring
 * @ticket: the ticket
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a ticket and proposes how it should be filed: a priority, an
 * issue type, tags, a one-line summary and the sentiment of whoever
 * raised it, with a sentence saying why. Nothing is written. The shape
 * is what venture_ai_assist_apply_triage() accepts, so a page can show
 * it, let somebody edit it and then apply it.
 *
 * Returns: (transfer full) (nullable): a JSON object, or %NULL on error
 */
JsonNode *
venture_ai_assist_triage(
	VentureContext	 *context,
	VentureEntity	 *ticket,
	GError		**error
);

/**
 * venture_ai_assist_apply_triage:
 * @context: the wiring
 * @ticket: the ticket, updated in place but not saved
 * @proposal: a triage, as venture_ai_assist_triage() returns one
 * @error: (out) (optional): return location for a #GError
 *
 * Applies whichever of the proposed fields are present and understood,
 * leaving the rest alone, and merges the tags rather than replacing
 * them. The ticket is not saved: the caller decides whether this is a
 * direct write or a staged one, which is what keeps a model's
 * suggestion under the same policy as its other changes.
 *
 * Returns: %TRUE if anything changed
 */
gboolean
venture_ai_assist_apply_triage(
	VentureContext	 *context,
	VentureEntity	 *ticket,
	JsonNode	 *proposal,
	GError		**error
);

/**
 * venture_ai_assist_summarise:
 * @context: the wiring
 * @ticket: the ticket
 * @error: (out) (optional): return location for a #GError
 *
 * What the ticket and its whole thread amount to, in a short paragraph:
 * what was asked, what has been tried, and what it is waiting on. For
 * the ticket somebody is picking up at handover.
 *
 * Returns: (transfer full) (nullable): the summary, or %NULL on error
 */
gchar *
venture_ai_assist_summarise(
	VentureContext	 *context,
	VentureEntity	 *ticket,
	GError		**error
);

/**
 * venture_ai_assist_draft_reply:
 * @context: the wiring
 * @ticket: the ticket
 * @instruction: (nullable): what the reply should do, in a few words
 * @error: (out) (optional): return location for a #GError
 *
 * Drafts the reply, for a person to read and edit before it goes. Never
 * posted by this call: a drafted reply that sent itself would be the
 * one thing in this program that reached a customer without anybody
 * reading it.
 *
 * Returns: (transfer full) (nullable): the draft, or %NULL on error
 */
gchar *
venture_ai_assist_draft_reply(
	VentureContext	 *context,
	VentureEntity	 *ticket,
	const gchar	 *instruction,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_AI_ASSIST_H */
