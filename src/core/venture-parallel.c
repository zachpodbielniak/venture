/*
 * venture-parallel.c - Reads answered on worker threads while the main
 * thread is idle
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * VENTURE was written for one thread: the entity and module registries,
 * the configuration, the currency registry, the automation engine and
 * every signal handler assume nothing else runs beside them. Proving that
 * of every line a read touches would be a rewrite, and a wrong guess is a
 * crash under load and nowhere else.
 *
 * So the read workers are fenced instead. The gate is a reader-writer
 * lock the main thread holds as writer at all times except while its
 * main loop sleeps in poll(). A worker holds it as reader for a whole
 * job, so jobs overlap each other and never the main thread. What a job
 * may share with other jobs is the remaining risk, and that is small and
 * named: the access policy's per-thread state, a database lease each,
 * and caches that are already locked (the currency registry, the entity
 * property layout).
 *
 * It is a hand-rolled lock because the main thread must win: glibc's
 * rwlock prefers readers, and a steady stream of reads would starve every
 * write. Here, once the main thread wants the gate back, no new job
 * enters until it is idle again.
 */

#include "venture.h"

typedef struct
{
	VentureParallelFunc	 func;
	VentureParallelFunc	 outside;
	VentureParallelDoneFunc	 done;
	gpointer		 user_data;
} Job;

static GMutex		 gate_lock;
static GCond		 gate_cond;
static guint		 gate_readers;
static gboolean		 gate_writer;		/* the owner holds it */
static guint		 gate_writer_waiting;
static guint		 gate_max_readers;
static GThread		*gate_owner;
static GMainContext	*gate_context;
static GPollFunc	 gate_previous_poll;
static GThreadPool	*gate_workers;
static guint64		 gate_jobs_run;
static guint		 gate_installs;		/* servers sharing the gate */

static void
gate_take_exclusive(void)
{
	g_mutex_lock(&gate_lock);
	gate_writer_waiting++;
	while (gate_writer || gate_readers > 0)
		g_cond_wait(&gate_cond, &gate_lock);
	gate_writer_waiting--;
	gate_writer = TRUE;
	g_mutex_unlock(&gate_lock);
}

static void
gate_give_exclusive(void)
{
	g_mutex_lock(&gate_lock);
	gate_writer = FALSE;
	g_cond_broadcast(&gate_cond);
	g_mutex_unlock(&gate_lock);
}

static void
gate_take_shared(void)
{
	g_mutex_lock(&gate_lock);
	while (gate_writer || gate_writer_waiting > 0)
		g_cond_wait(&gate_cond, &gate_lock);
	gate_readers++;
	gate_max_readers = MAX(gate_max_readers, gate_readers);
	g_mutex_unlock(&gate_lock);
}

static void
gate_give_shared(void)
{
	g_mutex_lock(&gate_lock);
	gate_readers--;
	if (0 == gate_readers)
		g_cond_broadcast(&gate_cond);
	g_mutex_unlock(&gate_lock);
}

/*
 * The only place the owner lets go: its outermost loop, about to sleep.
 * A nested loop -- a handler waiting for a webhook test or a feeds push --
 * is still inside that handler, mid-way through whatever it was doing, so
 * it keeps the gate. GLib calls this without the context's own lock.
 */
static gint
gate_poll(GPollFD *fds, guint n_fds, gint timeout)
{
	gint result;

	if (g_thread_self() != gate_owner || g_main_depth() > 0)
		return gate_previous_poll(fds, n_fds, timeout);

	gate_give_exclusive();
	result = gate_previous_poll(fds, n_fds, timeout);
	gate_take_exclusive();
	return result;
}

static gboolean
job_done(gpointer data)
{
	Job *job = data;

	if (NULL != job->done)
		job->done(job->user_data);
	return G_SOURCE_REMOVE;
}

static void
job_run(gpointer data, gpointer pool_data)
{
	Job *job = data;
	(void)pool_data;

	gate_take_shared();
	job->func(job->user_data);
	gate_give_shared();

	/* Work on what the job read and nothing shared -- turning records
	 * into JSON -- needs no gate, and holding it would keep the main
	 * thread waiting for something it cannot disturb. */
	if (NULL != job->outside)
		job->outside(job->user_data);
	__atomic_add_fetch(&gate_jobs_run, 1, __ATOMIC_RELAXED);

	g_main_context_invoke_full(gate_context, G_PRIORITY_DEFAULT, job_done, job, g_free);
}

void
venture_parallel_install(guint workers)
{
	g_autoptr(GError) error = NULL;

	g_return_if_fail(workers > 0);

	/* A second server in the same process shares the gate and the
	 * workers the first started. */
	if (NULL != gate_owner)
	{
		g_return_if_fail(g_thread_self() == gate_owner);
		gate_installs++;
		return;
	}
	gate_installs = 1;

	gate_owner = g_thread_self();
	gate_context = g_main_context_ref(g_main_context_default());
	gate_take_exclusive();
	gate_previous_poll = g_main_context_get_poll_func(gate_context);
	g_main_context_set_poll_func(gate_context, gate_poll);

	gate_workers = g_thread_pool_new(job_run, NULL, (gint)workers, FALSE, &error);
	if (NULL == gate_workers)
		g_error("Cannot start %u read workers: %s", workers, error->message);
}

void
venture_parallel_uninstall(void)
{
	g_return_if_fail(g_thread_self() == gate_owner);

	if (--gate_installs > 0)
		return;

	/* Jobs still queued need the gate to finish; open it for good while
	 * they drain, then stop answering polls. */
	g_main_context_set_poll_func(gate_context, gate_previous_poll);
	gate_give_exclusive();
	g_thread_pool_free(g_steal_pointer(&gate_workers), FALSE, TRUE);
	gate_previous_poll = NULL;
	gate_owner = NULL;
	g_clear_pointer(&gate_context, g_main_context_unref);
}

gboolean
venture_parallel_is_installed(void)
{
	return NULL != gate_owner;
}

void
venture_parallel_submit(
	VentureParallelFunc	 func,
	VentureParallelFunc	 outside,
	VentureParallelDoneFunc	 done,
	gpointer		 user_data
){
	g_autoptr(GError) error = NULL;
	Job *job;

	g_return_if_fail(NULL != func);
	g_return_if_fail(NULL != gate_workers);

	job = g_new0(Job, 1);
	job->func = func;
	job->outside = outside;
	job->done = done;
	job->user_data = user_data;
	if (!g_thread_pool_push(gate_workers, job, &error))
		g_error("Cannot queue a read job: %s", error->message);
}

guint64
venture_parallel_get_jobs_run(void)
{
	return __atomic_load_n(&gate_jobs_run, __ATOMIC_RELAXED);
}

guint
venture_parallel_get_max_concurrent(void)
{
	guint max;

	g_mutex_lock(&gate_lock);
	max = gate_max_readers;
	g_mutex_unlock(&gate_lock);
	return max;
}
