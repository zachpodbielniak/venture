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
 * change it: it stages a #VentureConfirmation carrying a human-readable
 * diff, and the tool result tells the model the change is awaiting approval.
 * Only an explicit approval applies it, and the applied write is audited
 * with the prompt that caused it.
 *
 * The queue those land in is #VentureConfirmationStore on the context, not
 * a table in here. It is shared with the REST API's staged writes, because
 * somebody answering "an agent wants to record a $12 expense" should have
 * one place to look rather than one per surface.
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

/* For #AiEvent, which a streaming turn narrates itself with. */
#include <ai-glib.h>

G_BEGIN_DECLS

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
/**
 * VentureAiStreamFunc:
 * @service: the #VentureAiService answering
 * @event: (transfer none): what just happened
 * @user_data: the data passed to venture_ai_service_answer_stream_async()
 *
 * Called for each thing that happens during a streaming turn: a chunk of
 * prose, a tool starting, a tool finishing, token usage. The events are
 * provider-neutral -- see #AiEvent -- so a consumer reads the same stream
 * whichever model is configured.
 *
 * Called on the thread that started the turn, from the main loop.
 */
typedef void (*VentureAiStreamFunc)(
	VentureAiService	*service,
	AiEvent			*event,
	gpointer		 user_data
);

/**
 * venture_ai_service_is_busy:
 * @self: a #VentureAiService
 *
 * Whether a streaming turn is in flight. One turn at a time: the
 * executor's streaming flag and the prompt an audit entry is stamped with
 * are single slots on the service.
 *
 * Returns: %TRUE while a turn is being answered
 */
gboolean
venture_ai_service_is_busy(VentureAiService *self);

/**
 * venture_ai_service_answer_stream_async:
 * @self: a #VentureAiService
 * @history: (nullable) (element-type VentureChatMessage): the conversation
 *   so far, oldest first
 * @message: what the operator asked
 * @images: (nullable) (element-type GBytes): images attached to this turn
 * @mime_types: (nullable) (array zero-terminated=1): the MIME type of each
 *   image, parallel to @images
 * @principal: (nullable): who is asking, for the audit trail
 * @on_event: (scope notified) (nullable): called as the turn unfolds
 * @event_data: data for @on_event
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the turn is finished
 * @user_data: data for @callback
 *
 * Like venture_ai_service_answer_with_images(), but asynchronous, and the
 * turn narrates itself through @on_event as it goes. The tool loop is the
 * same one: a model may call tools and go round again, and @on_event says
 * so, so the operator watching sees why an answer is taking its time.
 *
 * A provider that cannot stream still works -- it simply reports nothing
 * until the answer is whole. A second turn while one is in flight fails
 * with %VENTURE_ERROR_CONFLICT rather than queueing.
 */
void
venture_ai_service_answer_stream_async(
	VentureAiService	 *self,
	GPtrArray		 *history,
	const gchar		 *message,
	GPtrArray		 *images,
	const gchar *const	 *mime_types,
	VentureAuthPrincipal	 *principal,
	VentureAiStreamFunc	  on_event,
	gpointer		  event_data,
	GCancellable		 *cancellable,
	GAsyncReadyCallback	  callback,
	gpointer		  user_data
);

/**
 * venture_ai_service_answer_stream_finish:
 * @self: a #VentureAiService
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes venture_ai_service_answer_stream_async().
 *
 * Returns: (transfer full) (nullable): the whole reply, or %NULL on error
 */
gchar *
venture_ai_service_answer_stream_finish(
	VentureAiService	 *self,
	GAsyncResult		 *result,
	GError			**error
);

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
 * venture_ai_service_describe_tools:
 * @self: a #VentureAiService
 *
 * Returns: (transfer full): a JSON array naming every tool available to the
 *   model under the current policy
 */
JsonNode *
venture_ai_service_describe_tools(VentureAiService *self);

/**
 * venture_ai_service_complete:
 * @self: a #VentureAiService
 * @system_prompt: what the model is for, this once
 * @user_text: the turn
 * @error: (out) (optional): return location for a #GError
 *
 * One turn with no tools at all: the configured provider and model, a
 * system prompt for this job, and a single question. Used where the
 * answer wanted is a judgement about text rather than an errand -- a
 * ticket's classification, a summary, a draft reply -- and where letting
 * the model reach the record tools would be both slower and a way for
 * text somebody else wrote to steer a tool call.
 *
 * Returns: (transfer full) (nullable): the reply, or %NULL on error
 */
gchar *
venture_ai_service_complete(
	VentureAiService	 *self,
	const gchar		 *system_prompt,
	const gchar		 *user_text,
	GError			**error
);

/**
 * venture_ai_service_execute_tool:
 * @self: assistant service
 * @tool_use: an invocation of a registered tool
 * @principal: authenticated caller
 * @error: failure
 *
 * Executes the same callback used by a model turn, without a provider call.
 * Returns: (transfer full) (nullable): the tool's response
 */
gchar *venture_ai_service_execute_tool(VentureAiService *self, AiToolUse *tool_use, VentureAuthPrincipal *principal, GError **error);
G_END_DECLS

#endif /* VENTURE_AI_SERVICE_H */
