/*
 * venture-work-service.h - Running a coding agent against a ticket
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A run takes minutes. Everything else in VENTURE takes milliseconds and
 * runs on one main loop, so a run cannot live there: htmx-glib's handlers
 * return a response directly and cannot yield, and podomation's dispatch
 * blocks its caller. This service therefore owns the only background thread
 * in the program.
 *
 * Three rules keep that thread from becoming a source of races, and the
 * third is the one worth stating loudly:
 *
 * 1. The agent thread runs its own #GMainContext. Every subprocess, timer
 *    and provider callback belonging to a run attaches there, so the
 *    server's loop never waits on any of them.
 *
 * 2. The agent thread never touches #VentureDatabase, #VentureAutomation or
 *    any #VentureEntity. Writing a record emits `entity-saved`, whose
 *    automation handler enters podomation, which runs a nested main loop on
 *    the *default* context -- driving that from a second thread is a
 *    context-ownership failure, not merely a data race, and the database's
 *    mutex would not help.
 *
 * 3. State crosses as plain data. A job goes out as a struct of strings and
 *    scalars; progress comes back the same way and is applied to the run
 *    record on the main thread, which is the only place a record is ever
 *    written.
 */

#ifndef VENTURE_WORK_SERVICE_H
#define VENTURE_WORK_SERVICE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_WORK_SERVICE (venture_work_service_get_type())

G_DECLARE_FINAL_TYPE(VentureWorkService, venture_work_service,
                     VENTURE, WORK_SERVICE, GObject)

/**
 * venture_work_service_new:
 * @context: the wiring
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the service and starts its thread.
 *
 * Returns %NULL with %VENTURE_ERROR_CONFIG when runs are disabled, which is
 * the default and a normal state rather than a failure -- the rest of the
 * forge integration works without this.
 *
 * Any run left in flight by a previous process is reconciled here, before
 * the thread starts. Nothing is resumed: nobody observed how those runs
 * ended, so nothing may be concluded from them.
 *
 * Returns: (transfer full) (nullable): the service, or %NULL
 */
VentureWorkService *
venture_work_service_new(
	VentureContext	 *context,
	GError		**error
);

/**
 * venture_work_service_start_for_ticket:
 * @self: a #VentureWorkService
 * @ticket_id: the ticket to work on
 * @requested_by: (nullable): who asked, for the record
 * @error: (out) (optional): return location for a #GError
 *
 * Queues a run against @ticket_id, if a rule allows one.
 *
 * Returns the id of the #VentureForgeRun that was created. The run has not
 * started when this returns and will not have started when the request that
 * called it has been answered -- that is the point.
 *
 * Refuses with %VENTURE_ERROR_CONFLICT when a run for that ticket is already
 * live, and with %VENTURE_ERROR_NOT_FOUND when no rule governs it.
 *
 * Returns: the run's id, or 0 on error
 */
gint64
venture_work_service_start_for_ticket(
	VentureWorkService	 *self,
	gint64			  ticket_id,
	const gchar		 *requested_by,
	GError			**error
);

/**
 * venture_work_service_cancel:
 * @self: a #VentureWorkService
 * @run_id: the run to stop
 *
 * Stops a run, queued or in flight. Always allowed and always idempotent: a
 * caller who wants a run stopped should not have to know how far it got.
 *
 * Returns: %TRUE if a run was found to stop
 */
gboolean
venture_work_service_cancel(
	VentureWorkService	*self,
	gint64			 run_id
);

/**
 * venture_work_service_count_live:
 * @self: a #VentureWorkService
 *
 * Returns: how many runs are queued or in flight
 */
guint
venture_work_service_count_live(VentureWorkService *self);

G_END_DECLS

#endif /* VENTURE_WORK_SERVICE_H */
