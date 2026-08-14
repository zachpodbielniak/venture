/*
 * venture-ai-service.h - AI as a participant, with bounded authority
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The AI is given tools that reach the same repository the web UI and the
 * CLI use -- not a parallel, laxer path. What differs is authority.
 *
 * Read tools run unattended. A tool that would change a record does not
 * change it: it stages a #VentureAiConfirmation carrying a human-readable
 * diff, and the tool result tells the model the change is awaiting approval.
 * Only an explicit approval applies it, and the applied write is audited
 * with the prompt that caused it.
 *
 * This is the default rather than an option because the failure mode it
 * prevents is not a UI annoyance. A model that misreads "the etsy fees were
 * wrong last month" and rewrites forty expense records has created a tax
 * problem, and nothing downstream would notice.
 *
 * Policy is configurable: read_only removes the write tools entirely,
 * autonomous applies them immediately. Both are audited.
 */

#ifndef VENTURE_AI_SERVICE_H
#define VENTURE_AI_SERVICE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* --- Confirmations ------------------------------------------------------- */

#define VENTURE_TYPE_AI_CONFIRMATION (venture_ai_confirmation_get_type())

G_DECLARE_FINAL_TYPE(VentureAiConfirmation, venture_ai_confirmation,
                     VENTURE, AI_CONFIRMATION, GObject)

/**
 * venture_ai_confirmation_get_id:
 * @self: a #VentureAiConfirmation
 *
 * Returns: (transfer none): the identifier used to approve or reject it
 */
const gchar *
venture_ai_confirmation_get_id(VentureAiConfirmation *self);

/**
 * venture_ai_confirmation_get_summary:
 * @self: a #VentureAiConfirmation
 *
 * Returns: (transfer none): a one-line description of the pending change
 */
const gchar *
venture_ai_confirmation_get_summary(VentureAiConfirmation *self);

/**
 * venture_ai_confirmation_get_diff:
 * @self: a #VentureAiConfirmation
 *
 * Returns: (transfer none) (nullable): the field-by-field change
 */
JsonNode *
venture_ai_confirmation_get_diff(VentureAiConfirmation *self);

/**
 * venture_ai_confirmation_get_state:
 * @self: a #VentureAiConfirmation
 *
 * Returns: where the staged change stands
 */
VentureConfirmationState
venture_ai_confirmation_get_state(VentureAiConfirmation *self);

/**
 * venture_ai_confirmation_to_json:
 * @self: a #VentureAiConfirmation
 *
 * Returns: (transfer full): the confirmation as JSON, for the UI and the API
 */
JsonNode *
venture_ai_confirmation_to_json(VentureAiConfirmation *self);

/* --- Service ------------------------------------------------------------- */

#define VENTURE_TYPE_AI_SERVICE (venture_ai_service_get_type())

G_DECLARE_FINAL_TYPE(VentureAiService, venture_ai_service,
                     VENTURE, AI_SERVICE, GObject)

/**
 * venture_ai_service_new:
 * @context: the wiring
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the AI service, resolving the provider and credentials from
 * configuration.
 *
 * Returns %NULL with %VENTURE_ERROR_CONFIG when AI is disabled or has no
 * credentials. That is not a fatal condition: the server runs without it and
 * the chat dock says so.
 *
 * Returns: (transfer full) (nullable): the service, or %NULL
 */
VentureAiService *
venture_ai_service_new(
	VentureContext	 *context,
	GError		**error
);

/**
 * venture_ai_service_answer:
 * @self: a #VentureAiService
 * @message: what the operator asked
 * @principal: (nullable): who is asking, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Runs one exchange, including any tool calls the model makes.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL on error
 */
gchar *
venture_ai_service_answer(
	VentureAiService	 *self,
	const gchar		 *message,
	VentureAuthPrincipal	 *principal,
	GError			**error
);

/**
 * venture_ai_service_get_policy:
 * @self: a #VentureAiService
 *
 * Returns: how much authority tool calls currently have
 */
VentureAiPolicy
venture_ai_service_get_policy(VentureAiService *self);

/**
 * venture_ai_service_list_pending:
 * @self: a #VentureAiService
 *
 * Lists staged changes still awaiting a decision, dropping any that have
 * expired.
 *
 * Returns: (transfer container) (element-type VentureAiConfirmation): the
 *   pending confirmations
 */
GPtrArray *
venture_ai_service_list_pending(VentureAiService *self);

/**
 * venture_ai_service_approve:
 * @self: a #VentureAiService
 * @confirmation_id: the identifier from the staged change
 * @principal: (nullable): who approved it
 * @error: (out) (optional): return location for a #GError
 *
 * Applies a staged change and audits it as an AI-originated mutation that a
 * named person approved.
 *
 * Returns: %TRUE if the change was applied
 */
gboolean
venture_ai_service_approve(
	VentureAiService	 *self,
	const gchar		 *confirmation_id,
	VentureAuthPrincipal	 *principal,
	GError			**error
);

/**
 * venture_ai_service_reject:
 * @self: a #VentureAiService
 * @confirmation_id: the identifier from the staged change
 * @principal: (nullable): who rejected it
 * @error: (out) (optional): return location for a #GError
 *
 * Discards a staged change, recording the refusal.
 *
 * Returns: %TRUE if the change was discarded
 */
gboolean
venture_ai_service_reject(
	VentureAiService	 *self,
	const gchar		 *confirmation_id,
	VentureAuthPrincipal	 *principal,
	GError			**error
);

/**
 * venture_ai_service_describe_tools:
 * @self: a #VentureAiService
 *
 * Returns: (transfer full): a JSON array naming every tool available to the
 *   model under the current policy
 */
JsonNode *
venture_ai_service_describe_tools(VentureAiService *self);

G_END_DECLS

#endif /* VENTURE_AI_SERVICE_H */
