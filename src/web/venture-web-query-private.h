/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-web-query-private.h - What the query workers hold, for tests
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * The query workers' admission (venture-web-query.inc, "Who may have a
 * worker") is process-wide state; a test reads it here to show that
 * every path -- answered, refused, timed out, cancelled, disconnected --
 * gives back what it held. Server code only, outside the GIR.
 */

#ifndef VENTURE_WEB_QUERY_PRIVATE_H
#define VENTURE_WEB_QUERY_PRIVATE_H

#include <venture.h>

G_BEGIN_DECLS

/*
 * venture_web_query_get_counts:
 * @out_admitted: (out) (optional): jobs admitted and not yet done with:
 *   running, or queued for a worker (one a request gave up on included,
 *   until a worker skips it)
 * @out_running: (out) (optional): jobs a worker is running now
 * @out_holders: (out) (optional): workspaces and principals holding at
 *   least one admitted job; zero once every count is back to zero
 * @out_ran: (out) (optional): jobs whose work a worker ran, since the
 *   process started: unchanged across a request, nothing was evaluated
 * @out_skipped: (out) (optional): queued jobs a worker found cancelled or
 *   given up and skipped without running, since the process started
 *
 * Reads the admission counts under their lock. Any thread.
 */
void
venture_web_query_get_counts(
	guint	*out_admitted,
	guint	*out_running,
	guint	*out_holders,
	guint64	*out_ran,
	guint64	*out_skipped
);

/*
 * venture_web_server_query_off_loop:
 * @self: the server
 * @work: (scope call): LQL work, run on a query worker
 * @data: (closure work): its data, kept until this returns
 * @cancellable: (nullable): cancelled, the work is given up or stopped
 * @error: (out) (optional): return location for a #GError
 *
 * venture_web_query_off_loop() with no request: admitted as the server's
 * own work, and waited for on this stack (no fiber). The tests' door to
 * the admission and the queue -- a job cancelled before a worker reaches
 * it -- that no route can reach on cue. Main thread.
 *
 * Returns: %TRUE once @work has returned; %FALSE with @error set when it
 *   did not run
 */
gboolean
venture_web_server_query_off_loop(
	VentureWebServer	 *self,
	VentureParallelFunc	  work,
	gpointer		  data,
	GCancellable		 *cancellable,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_WEB_QUERY_PRIVATE_H */
