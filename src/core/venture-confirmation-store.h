/*
 * venture-confirmation-store.h - Changes proposed but not yet made
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureConfirmation is a write that has been described and not
 * performed: the record with the change already applied in memory, the
 * field-by-field diff a person is asked to approve, and who asked for it.
 * A #VentureConfirmationStore is the queue of them.
 *
 * This began inside #VentureAiService, because the in-process assistant was
 * the only thing that staged anything. It is here now because it is not
 * about AI: an outside agent holding an API token proposes changes for the
 * same reason the assistant does, and a person answering them wants one
 * queue rather than two. The store therefore hangs off #VentureContext and
 * exists whether or not AI is configured -- with AI off, the assistant is
 * absent and the queue still works.
 *
 * Two properties are the whole point, and both are load-bearing:
 *
 *   - Staging changes nothing. The record is built and validated in memory
 *     and the database is not touched until an approval.
 *
 *   - Approval applies the staged object, so it cannot produce a different
 *     outcome from the diff that was shown. It also cannot overwrite work
 *     done in the meantime: the staged record carries the version it was
 *     read at, and venture_database_save() refuses an update whose expected
 *     version no longer matches. A stale approval is a conflict, reported
 *     with what changed underneath, never a silent overwrite.
 */

#ifndef VENTURE_CONFIRMATION_STORE_H
#define VENTURE_CONFIRMATION_STORE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* --- One staged change --------------------------------------------------- */

#define VENTURE_TYPE_CONFIRMATION (venture_confirmation_get_type())

G_DECLARE_FINAL_TYPE(VentureConfirmation, venture_confirmation,
                     VENTURE, CONFIRMATION, GObject)

/**
 * venture_confirmation_get_id:
 * @self: a #VentureConfirmation
 *
 * Returns: (transfer none): the identifier used to approve or reject it
 */
const gchar *
venture_confirmation_get_id(VentureConfirmation *self);

/**
 * venture_confirmation_get_summary:
 * @self: a #VentureConfirmation
 *
 * Returns: (transfer none): a one-line description of the pending change
 */
const gchar *
venture_confirmation_get_summary(VentureConfirmation *self);

/**
 * venture_confirmation_get_diff:
 * @self: a #VentureConfirmation
 *
 * Retrieves the change, field by field, with sensitive fields marked as
 * changed rather than shown.
 *
 * Returns: (transfer none) (nullable): the diff, or %NULL for a deletion
 */
JsonNode *
venture_confirmation_get_diff(VentureConfirmation *self);

/**
 * venture_confirmation_get_entity_type:
 * @self: a #VentureConfirmation
 *
 * Retrieves the record type the staged change would write.
 *
 * Exposed so the approval route can require the same role the direct route
 * for that type would. Without it, a change the REST API refuses to an
 * editor could be made by staging it and approving the result -- staging
 * would launder the authorisation, which is the opposite of what staging is
 * for.
 *
 * Returns: the #GType, or %G_TYPE_INVALID for a change that writes no record
 */
GType
venture_confirmation_get_entity_type(VentureConfirmation *self);

/**
 * venture_confirmation_to_json:
 * @self: a #VentureConfirmation
 *
 * Renders the confirmation for the UI and the API.
 *
 * It carries everything needed to decide without access to the request that
 * made it: the record type, the record's identifier and label when there is
 * one, what would change, who asked and how, and when it expires. A decision
 * inbox somewhere else is the intended reader, and it cannot go and look at
 * the HTTP call that produced this.
 *
 * Returns: (transfer full): the confirmation as JSON
 */
JsonNode *
venture_confirmation_to_json(VentureConfirmation *self);

/* --- The queue ----------------------------------------------------------- */

#define VENTURE_TYPE_CONFIRMATION_STORE (venture_confirmation_store_get_type())

G_DECLARE_FINAL_TYPE(VentureConfirmationStore, venture_confirmation_store,
                     VENTURE, CONFIRMATION_STORE, GObject)

/**
 * venture_confirmation_store_new:
 * @database: the repository staged changes are applied through
 * @ttl_seconds: how long a change may wait for a decision
 * @limit: the most changes that may wait at once; 0 means no limit
 *
 * Creates a queue of staged changes.
 *
 * Returns: (transfer full): a new #VentureConfirmationStore
 */
VentureConfirmationStore *
venture_confirmation_store_new(
	VentureDatabase	*database,
	gint64		 ttl_seconds,
	gint64		 limit
);

/**
 * venture_confirmation_store_stage:
 * @self: a #VentureConfirmationStore
 * @action: whether the change creates, updates or deletes
 * @staged: the record with the change already applied in memory
 * @original: (nullable): the record as it stood before, for the diff and for
 *   reporting what moved underneath a stale approval; %NULL for a creation
 * @origin: who asked, and for an AI-driven change the prompt that caused it
 * @via: how it arrived, e.g. `assistant` or `rest-api`
 * @error: (out) (optional): return location for a #GError
 *
 * Records a change without performing it.
 *
 * Nothing is written: @staged is held as it is, so approving it later
 * applies exactly what the diff described. Staging an identical change twice
 * returns the waiting confirmation rather than adding a second -- models
 * retry when a call answers "awaiting approval" rather than "ok", and three
 * cards for one change means approving all three makes three records.
 *
 * Returns: (transfer none) (nullable): the confirmation, or %NULL if the
 *   queue is full
 */
VentureConfirmation *
venture_confirmation_store_stage(
	VentureConfirmationStore	 *self,
	VentureAuditAction		  action,
	VentureEntity			 *staged,
	VentureEntity			 *original,
	const VentureActor		 *origin,
	const gchar			 *via,
	GError				**error
);

/**
 * venture_confirmation_store_list_pending:
 * @self: a #VentureConfirmationStore
 *
 * Lists changes still awaiting a decision, oldest first, dropping any that
 * have expired.
 *
 * Returns: (transfer container) (element-type VentureConfirmation): the
 *   pending confirmations
 */
GPtrArray *
venture_confirmation_store_list_pending(VentureConfirmationStore *self);

/**
 * venture_confirmation_store_find:
 * @self: a #VentureConfirmationStore
 * @confirmation_id: the identifier from the staged change
 *
 * Looks a pending change up without deciding it, so a caller can check what
 * it would write before allowing anybody to approve it.
 *
 * Returns: (transfer none) (nullable): the confirmation, or %NULL
 */
VentureConfirmation *
venture_confirmation_store_find(
	VentureConfirmationStore	*self,
	const gchar			*confirmation_id
);

/**
 * venture_confirmation_store_approve:
 * @self: a #VentureConfirmationStore
 * @confirmation_id: the identifier from the staged change
 * @approver: (nullable): who approved it
 * @error: (out) (optional): return location for a #GError
 *
 * Applies a staged change through the same repository call an ordinary write
 * uses, and audits it as a change somebody proposed and somebody approved.
 *
 * The staged record carries the version it was read at, so an approval that
 * would overwrite a change made in the meantime fails with
 * %VENTURE_ERROR_CONFLICT naming the fields that moved. The confirmation is
 * then dropped: its staged object is pinned to a version that will never
 * match again, so approving it a second time could not succeed either.
 *
 * A decided confirmation is freed, so a #VentureConfirmation borrowed from
 * venture_confirmation_store_find() must not be used after this returns.
 * Copy its id first if it is needed afterwards.
 *
 * Returns: %TRUE if the change was applied
 */
gboolean
venture_confirmation_store_approve(
	VentureConfirmationStore	 *self,
	const gchar			 *confirmation_id,
	const gchar			 *approver,
	GError				**error
);

/**
 * venture_confirmation_store_reject:
 * @self: a #VentureConfirmationStore
 * @confirmation_id: the identifier from the staged change
 * @approver: (nullable): who rejected it
 * @error: (out) (optional): return location for a #GError
 *
 * Discards a staged change and records the refusal, because knowing what was
 * proposed and denied is as useful as knowing what was done.
 *
 * Like approving, this frees the confirmation.
 *
 * Returns: %TRUE if the change was discarded
 */
gboolean
venture_confirmation_store_reject(
	VentureConfirmationStore	 *self,
	const gchar			 *confirmation_id,
	const gchar			 *approver,
	GError				**error
);

/**
 * venture_confirmation_parse_stage_flag:
 * @value: (nullable): the `stage` query parameter, or %NULL if absent
 * @out_stage: (out): whether the write should be staged
 * @error: (out) (optional): return location for a #GError
 *
 * Reads the `stage` parameter that turns a REST write into a proposal.
 *
 * Absent means no. `1`, `true`, `yes` and `on` mean yes; `0`, `false`, `no`
 * and `off` mean no. **Anything else is refused**, and that is the whole
 * reason this is a function rather than a comparison at each call site: a
 * spelling the server does not recognise, treated as false, applies the
 * write the caller was trying to hold back. A refusal costs a retry; the
 * permissive reading costs the record.
 *
 * Returns: %TRUE if @value was understood
 */
gboolean
venture_confirmation_parse_stage_flag(
	const gchar	 *value,
	gboolean	 *out_stage,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_CONFIRMATION_STORE_H */
