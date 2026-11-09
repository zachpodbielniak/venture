/*
 * venture-parallel.h - Reads answered on worker threads while the main
 * thread is idle
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef VENTURE_PARALLEL_H
#define VENTURE_PARALLEL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VentureParallelFunc:
 * @user_data: (closure): the job's data
 *
 * A read job's body, run on a worker thread while it holds the gate's
 * shared side (see venture_parallel_install()).
 */
typedef void (*VentureParallelFunc)(gpointer user_data);

/**
 * VentureParallelDoneFunc:
 * @user_data: (closure): the job's data
 *
 * Runs on the thread that installed the gate, after the job's body.
 */
typedef void (*VentureParallelDoneFunc)(gpointer user_data);

/**
 * venture_parallel_install:
 * @workers: worker threads to run read jobs on, at least 1
 *
 * Makes the calling thread the gate's owner and starts the workers.
 *
 * The owner holds the gate exclusively at all times except while its
 * default main context sits in its outermost poll -- waiting for
 * something to happen, running nothing. Workers take the gate shared for
 * a job's whole body, so jobs run side by side with each other and never
 * beside any code on the owner's thread: every write, signal handler,
 * timer and registry reload there keeps the single-threaded world it
 * was written for. A nested main loop does not open the gate, so a
 * handler waiting in one is never overlapped either. The gate prefers the
 * owner: once it wakes, no new job starts until it is idle again.
 *
 * Installs are counted: a second server in the same process shares the
 * gate and the first install's workers, and each install is matched by
 * one venture_parallel_uninstall().
 */
void
venture_parallel_install(guint workers);

/**
 * venture_parallel_uninstall:
 *
 * Drops one install. The last waits for every queued job's body to
 * finish, stops the workers and gives the gate up. Call it on the owner's thread. Done callbacks of
 * jobs that finished are still dispatched on the owner's main context
 * afterwards.
 */
void
venture_parallel_uninstall(void);

/**
 * venture_parallel_is_installed:
 *
 * Returns: %TRUE between venture_parallel_install() and
 *   venture_parallel_uninstall()
 */
gboolean
venture_parallel_is_installed(void);

/**
 * venture_parallel_submit:
 * @func: (scope async): the job's body, run on a worker under the gate
 * @outside: (scope async) (nullable): run next on the same worker, after
 *   the gate is given back
 * @done: (scope async): run afterwards on the owner's main context
 * @user_data: (closure): data for all three
 *
 * Queues a read job. @func must not write, must not touch anything only
 * the owner's thread may (see AGENTS.md, the thread list), and must
 * bind its own access-policy state and database lease. @outside may
 * touch only what @func produced and data that never changes after
 * startup or carries its own lock (class metadata, the currency
 * registry): the owner may be running beside it.
 */
void
venture_parallel_submit(
	VentureParallelFunc	 func,
	VentureParallelFunc	 outside,
	VentureParallelDoneFunc	 done,
	gpointer		 user_data
);

/**
 * venture_parallel_get_jobs_run:
 *
 * Returns: how many job bodies have finished since startup
 */
guint64
venture_parallel_get_jobs_run(void);

/**
 * venture_parallel_get_max_concurrent:
 *
 * Returns: the most job bodies that have been inside the gate at once
 */
guint
venture_parallel_get_max_concurrent(void);

G_END_DECLS

#endif /* VENTURE_PARALLEL_H */
