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
 * venture_ai_confirmation_get_entity_type:
 * @self: a #VentureAiConfirmation
 *
 * Retrieves the record type the staged change would write.
 *
 * Exposed so the approval route can require the same role the direct route
 * for that type would. Without it, a change the REST API refuses to an
 * editor could be made by asking the assistant for it and approving the
 * result -- staging launders the authorisation, which is the opposite of
 * what staging is for.
 *
 * Returns: the #GType, or %G_TYPE_INVALID for a change that writes no record
 */
GType
venture_ai_confirmation_get_entity_type(VentureAiConfirmation *self);

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
 * venture_ai_service_answer_in_thread:
 * @self: a #VentureAiService
 * @history: (nullable) (element-type VentureChatMessage): the conversation so
 *   far, oldest first; the new question must not be included
 * @message: what the operator asked
 * @principal: (nullable): who is asking, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Like venture_ai_service_answer(), but the model sees the whole thread
 * before the new question, which is what makes a resumed conversation a
 * conversation. Tool exchanges from earlier turns are not replayed; a tool
 * result is a snapshot, and the model re-reads rather than trusting one.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL on error
 */
gchar *
venture_ai_service_answer_in_thread(
	VentureAiService	 *self,
	GPtrArray		 *history,
	const gchar		 *message,
	VentureAuthPrincipal	 *principal,
	GError			**error
);

/**
 * venture_ai_service_answer_with_images:
 * @self: a #VentureAiService
 * @history: (nullable) (element-type VentureChatMessage): the conversation
 *   so far, oldest first
 * @message: what the operator asked
 * @images: (nullable) (element-type GBytes): images attached to this turn,
 *   in the order they were attached
 * @mime_types: (nullable) (array zero-terminated=1): the MIME type of each
 *   image, parallel to @images
 * @principal: (nullable): who is asking, for the audit trail
 * @error: (out) (optional): return location for a #GError
 *
 * Like venture_ai_service_answer_in_thread(), but the model is also shown
 * the attached images. This is what turns "here are screenshots of the
 * campaign setup" into staged records: a vision-capable model reads the
 * figures off the picture and calls the ordinary create tool with them,
 * under the ordinary confirmation policy.
 *
 * Images are attached to this turn only. The stored transcript names the
 * documents instead, so a long conversation does not re-upload every
 * screenshot on every later question.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL on error
 */
gchar *
venture_ai_service_answer_with_images(
	VentureAiService	 *self,
	GPtrArray		 *history,
	const gchar		 *message,
	GPtrArray		 *images,
	const gchar *const	 *mime_types,
	VentureAuthPrincipal	 *principal,
	GError			**error
);

/**
 * venture_ai_url_is_fetchable:
 * @url: the address the model asked for
 * @error: (out) (optional): return location for a #GError
 *
 * Whether @url may be fetched by the AI's page-fetching tool: http or
 * https only, and no host resolving to a loopback, link-local, site-local,
 * multicast or carrier-grade-NAT address.
 *
 * Public because it is a security boundary and therefore deserves a test
 * that does not depend on a model's willingness to attempt the request.
 * The model picks the address, and a page it reads can suggest the next
 * one; without this check, "summarise this link" is a route to the cloud
 * metadata endpoint or the database port on the container network.
 *
 * Returns: %TRUE if the address may be fetched
 */
gboolean
venture_ai_url_is_fetchable(
	const gchar	 *url,
	GError		**error
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
 * venture_ai_service_find:
 * @self: a #VentureAiService
 * @confirmation_id: the identifier from the staged change
 *
 * Looks a pending staged change up without deciding it, so a caller can
 * check what it would write before allowing anybody to approve it.
 *
 * Returns: (transfer none) (nullable): the confirmation, or %NULL
 */
VentureAiConfirmation *
venture_ai_service_find(
	VentureAiService	*self,
	const gchar		*confirmation_id
);

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
