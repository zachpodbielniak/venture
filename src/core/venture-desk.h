/*
 * venture-desk.h - The workdesk: macros, worklogs, sprints, bulk edits
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The operations a tracker and a helpdesk do to tickets that a form
 * cannot express in one press: apply a macro, log time, read a sprint's
 * burn, change forty records at once, and read what happened to a record
 * as one timeline. Each is one implementation, called by the page, the
 * API, venturectl and the assistant alike.
 */

#ifndef VENTURE_DESK_H
#define VENTURE_DESK_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_desk_install:
 * @context: the wiring
 *
 * Installs the hook that keeps a ticket's logged hours in step with its
 * worklogs. Called once by the context.
 */
void
venture_desk_install(VentureContext *context);

/* --- Macros ---------------------------------------------------------------- */

/**
 * venture_desk_apply_macro:
 * @context: the wiring
 * @ticket: the ticket, updated in place
 * @macro: the macro
 * @actor: (nullable): who is applying it; its name fills {me}
 * @error: (out) (optional): return location for a #GError
 *
 * Applies a macro: adds its reply as a comment, then changes whatever
 * the macro says to change and saves the ticket. One transaction, so a
 * refused status leaves no orphan comment. An inactive macro is refused.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_desk_apply_macro(
	VentureContext		 *context,
	VentureEntity		 *ticket,
	VentureEntity		 *macro,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_desk_list_macros:
 * @context: the wiring
 *
 * The active macros in menu order.
 *
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   the macros
 */
GPtrArray *
venture_desk_list_macros(VentureContext *context);

/**
 * venture_desk_find_macro:
 * @context: the wiring
 * @name_or_id: a macro's name, or its id as digits
 *
 * Returns: (transfer full) (nullable): the macro
 */
VentureEntity *
venture_desk_find_macro(
	VentureContext	*context,
	const gchar	*name_or_id
);

/* --- Worklogs -------------------------------------------------------------- */

/**
 * venture_desk_log_work:
 * @context: the wiring
 * @ticket_id: the ticket
 * @hours: how long; must be positive
 * @note: (nullable): what on
 * @actor: (nullable): who; its name is the worklog's author
 * @error: (out) (optional): return location for a #GError
 *
 * Records time against a ticket. The ticket's logged-hours follows.
 *
 * Returns: (transfer full) (nullable): the worklog
 */
VentureEntity *
venture_desk_log_work(
	VentureContext		 *context,
	gint64			  ticket_id,
	gdouble			  hours,
	const gchar		 *note,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_desk_logged_hours:
 * @database: the database
 * @ticket_id: the ticket
 *
 * Returns: the hours logged against the ticket, summed from the worklogs
 */
gdouble
venture_desk_logged_hours(
	VentureDatabase	*database,
	gint64		 ticket_id
);

/* --- Sprints --------------------------------------------------------------- */

/**
 * VentureSprintProgress:
 * @tickets: how many tickets are planned into it
 * @done: how many are done or cancelled
 * @points: the points those tickets carry
 * @points_done: the points of the finished ones
 * @capacity: what the sprint said it could take
 * @days_total: the sprint's length in days, 0 when undated
 * @days_left: days until it ends; negative once over
 *
 * A sprint's burn.
 */
typedef struct
{
	gint64	tickets;
	gint64	done;
	gint64	points;
	gint64	points_done;
	gint64	capacity;
	gint64	days_total;
	gint64	days_left;
} VentureSprintProgress;

/**
 * venture_desk_sprint_progress:
 * @context: the wiring
 * @sprint: the sprint
 * @out: (out): the burn
 *
 * Counts a sprint's tickets and points.
 */
void
venture_desk_sprint_progress(
	VentureContext		*context,
	VentureEntity		*sprint,
	VentureSprintProgress	*out
);

/**
 * venture_desk_sprint_to_json:
 * @context: the wiring
 * @sprint: the sprint
 * @with_tickets: whether to list the tickets, grouped by status
 *
 * A sprint with its progress, as the API and the assistant see it.
 *
 * Returns: (transfer full): a JSON object
 */
JsonNode *
venture_desk_sprint_to_json(
	VentureContext	*context,
	VentureEntity	*sprint,
	gboolean	 with_tickets
);

/**
 * venture_desk_list_sprints:
 * @context: the wiring
 * @organization_ids: (nullable) (array length=n_organizations): the scope
 * @n_organizations: how many
 * @error: (out) (optional): return location for a #GError
 *
 * Every sprint, active first, then planned soonest first, then finished
 * newest first.
 *
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   the sprints
 */
GPtrArray *
venture_desk_list_sprints(
	VentureContext	 *context,
	const gint64	 *organization_ids,
	gsize		  n_organizations,
	GError		**error
);

/* --- Bulk edits ------------------------------------------------------------ */

/**
 * venture_desk_bulk_update:
 * @context: the wiring
 * @entity_type: the record type
 * @ids: (array length=n_ids): which records
 * @n_ids: how many
 * @changes: field to text value, in the wire spelling or the property one
 * @actor: (nullable): who
 * @error: (out) (optional): return location for a #GError
 *
 * Sets the same fields on many records, in one transaction: a value one
 * of them refuses leaves all of them as they were. Each record's own
 * validation, references and validators run exactly as for a single save,
 * and each is audited on its own.
 *
 * Returns: how many records changed, or -1 on error
 */
gint
venture_desk_bulk_update(
	VentureContext		 *context,
	GType			  entity_type,
	const gint64		 *ids,
	gsize			  n_ids,
	JsonObject		 *changes,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_desk_bulk_delete:
 * @context: the wiring
 * @entity_type: the record type
 * @ids: (array length=n_ids): which records
 * @n_ids: how many
 * @actor: (nullable): who
 * @error: (out) (optional): return location for a #GError
 *
 * Soft-deletes many records in one transaction.
 *
 * Returns: how many were removed, or -1 on error
 */
gint
venture_desk_bulk_delete(
	VentureContext		 *context,
	GType			  entity_type,
	const gint64		 *ids,
	gsize			  n_ids,
	const VentureActor	 *actor,
	GError			**error
);

/* --- The timeline ---------------------------------------------------------- */

/**
 * venture_desk_activity:
 * @context: the wiring
 * @target_type: the record type
 * @target_id: the record
 * @limit: at most this many entries; 0 for 100
 * @error: (out) (optional): return location for a #GError
 *
 * Everything that happened to a record, newest first: each audited
 * change with who made it and what moved, and for a ticket its comments
 * and worklogs woven in by time. This is the timeline a record's page
 * shows and `GET /api/v1/activity/:type/:id` returns.
 *
 * Returns: (transfer full) (nullable): a JSON array
 */
JsonNode *
venture_desk_activity(
	VentureContext	 *context,
	const gchar	 *target_type,
	gint64		  target_id,
	guint		  limit,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_DESK_H */
