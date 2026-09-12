/*
 * venture-sla.h - Service levels on tickets
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A policy says how fast a ticket of a given kind and priority must be
 * answered and resolved. When a ticket is first saved the matching policy
 * stamps two due times on it, and from then on the clocks are just those
 * two fields against the time of asking. First response is stamped by the
 * first visible reply. A sweep marks what has been missed and tells the
 * people concerned, so a breach is a fact in the table rather than a
 * calculation nobody ran.
 */

#ifndef VENTURE_SLA_H
#define VENTURE_SLA_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VentureSlaStatus:
 * @has_policy: whether any policy covered the ticket
 * @first_response: where the first-response clock stands
 * @resolution: where the resolution clock stands
 * @first_response_remaining: seconds until the first reply is due;
 *   negative once past, 0 when there is no clock or it was met
 * @resolution_remaining: seconds until resolution is due; negative once
 *   past, 0 when there is no clock or the ticket is closed
 * @responded: whether the first visible reply has been made
 * @closed: whether the ticket is done or cancelled
 *
 * A ticket's standing against its service level at one instant.
 */
typedef struct
{
	gboolean	 has_policy;
	VentureSlaState	 first_response;
	VentureSlaState	 resolution;
	gint64		 first_response_remaining;
	gint64		 resolution_remaining;
	gboolean	 responded;
	gboolean	 closed;
} VentureSlaStatus;

/**
 * venture_sla_install:
 * @context: the wiring
 *
 * Installs the save validator that stamps due times on a ticket, and the
 * hook that stamps its first reply. Called once by the context.
 */
void
venture_sla_install(VentureContext *context);

/**
 * venture_sla_find_policy:
 * @context: the wiring
 * @kind: the ticket's kind
 * @priority: the ticket's priority
 *
 * The active policy that covers a ticket: one naming both its kind and
 * priority first, then one naming either, then one covering everything.
 * Ties go to the lowest id, so the answer is stable.
 *
 * Returns: (transfer full) (nullable): the policy, or %NULL if none covers it
 */
VentureEntity *
venture_sla_find_policy(
	VentureContext		*context,
	VentureTicketKind	 kind,
	VenturePriority		 priority
);

/**
 * venture_sla_status:
 * @ticket: a ticket
 * @now: (nullable): the instant to measure at; %NULL for now
 * @out: (out): where the ticket stands
 *
 * Reads the ticket's clocks. Warning begins inside the last fifth of a
 * target, so a four-hour target warns with fifty minutes left.
 */
void
venture_sla_status(
	VentureEntity		*ticket,
	GDateTime		*now,
	VentureSlaStatus	*out
);

/**
 * venture_sla_status_to_json:
 * @status: a status
 *
 * Returns: (transfer full): the status as a JSON object
 */
JsonNode *
venture_sla_status_to_json(const VentureSlaStatus *status);

/**
 * venture_sla_sweep:
 * @context: the wiring
 * @limit: at most this many tickets marked; 0 for 200
 * @error: (out) (optional): return location for a #GError
 *
 * Finds open tickets whose resolution target has passed and are not yet
 * marked breached, marks them, and tells the assignee and the watchers.
 * Bounded, because it is run from a page load and an API call rather
 * than a thread. Idempotent: a ticket is marked once.
 *
 * Returns: how many tickets were newly marked, or -1 on error
 */
gint
venture_sla_sweep(
	VentureContext	 *context,
	guint		  limit,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_SLA_H */
