/*
 * venture-web-query.inc - LQL over HTTP: POST /api/v1/query, the query
 * widget's runner, and where a query runs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included from venture-web-server.c after venture-web-later.inc.
 *
 * Where a query runs (docs/lql.org, "Where a query runs"). A query is the
 * one request a caller can make arbitrarily expensive without writing
 * anything: string building, sorting, joins, sixty-four fetches. So no
 * query is evaluated on the main loop.
 *
 *   - Every route that can run one answers from a fiber of the main loop
 *     (venture_web_answer_later(), as the Stripe routes do), which keeps
 *     the request's own access snapshot, its hosted admission, its
 *     tenant request scope and the hosted finish check. Every one of
 *     those routes is VENTURE_HOSTED_ROUTE_NONE, so no support capability
 *     is admitted, and the fiber's plain tenant scope is the one the
 *     pipeline would have given it.
 *   - The caller's environment is built inside that fiber, in the
 *     request's access scope, and the query is prepared there: parsed,
 *     checked against the caller's refusals, the schema of every source
 *     it names built (venture_lql_query_prepare()).
 *   - The evaluation runs on a query worker (a bounded pool of
 *     VENTURE_WEB_QUERY_THREADS) with a GCancellable, and the fiber
 *     yields, so the main loop serves everybody else meanwhile.
 *   - A worker never touches the database (AGENTS.md, the thread list).
 *     Each fetch it needs is handed back to the fiber, which wakes, reads
 *     it on the main thread inside the request's own scopes as the
 *     caller's snapshot, hands the rows over and yields again. One fetch
 *     is one bounded read (query.max_rows + 1 rows): what one list page
 *     costs the main thread, and the loop runs between two of them.
 *   - The query's deadline -- query.timeout_ms and a grace period -- and
 *     a client that goes away both cancel it: the evaluation stops at its
 *     next step and a fetch not yet read is never read.
 *
 * The one helper for all of it is venture_web_query_off_loop(): any LQL
 * work -- run a query, find the ids a filter keeps, check or complete a
 * person's text -- with a cancellable, on an environment built in the
 * request. venture_web_query_run() is POST /api/v1/query's and the query
 * widget's use of it.
 *
 * Off a fiber -- a stack that cannot yield, a nested main loop, an open
 * transaction -- the worker still evaluates, and the calling stack waits
 * for it, reading its fetches meanwhile. That blocks the main loop for as
 * long as the query takes, bounded by the same deadline; no query route
 * takes this path, and it exists so a query is never evaluated where the
 * database lives.
 *
 * The workers are shared by every request of the process, so who may
 * have one is bounded ("Who may have a worker" below): per hosted
 * workspace, per principal, and in all. A request over a bound is refused
 * at once (VENTURE_ERROR_BUSY: 503 and Retry-After), and one that waits
 * past its deadline for a worker stops waiting and is answered (TIMEOUT);
 * its job is skipped, never run.
 */

#include "web/venture-web-query-private.h"

/* Query workers: how many queries are evaluated at once, process-wide. */
#define VENTURE_WEB_QUERY_THREADS (4)

/*
 * Who may have a worker (docs/lql.org, "Who may have a worker"). A job
 * counts from the moment it is admitted until a worker is done with it --
 * running it, or skipping it because it was given up -- so a job holds
 * its place while it waits as much as while it runs.
 *
 *   - One hosted workspace: at most VENTURE_WEB_QUERY_PER_WORKSPACE jobs,
 *     one fewer than there are workers, so whatever one workspace does,
 *     a worker is left that it cannot reach. Hosted http_concurrency (8
 *     by default) is higher than the pool, so without this one workspace's
 *     slow queries would hold every worker for every tenant.
 *   - One principal -- a user, and every token of theirs, since a token
 *     acts as its user; the same person minting tokens gains nothing --
 *     at most VENTURE_WEB_QUERY_PER_PRINCIPAL, below the workspace's: one
 *     person cannot take their colleagues' share. A query widget page,
 *     a chart and a bar run their queries one after another, so one
 *     person's page needs one at a time; two leaves room for a second
 *     tab or the bar's assist while a slow query runs.
 *   - The process: at most VENTURE_WEB_QUERY_THREADS +
 *     VENTURE_WEB_QUERY_QUEUE_MAX jobs, so the queue is bounded however
 *     many principals ask. Sixteen waiting is four rounds of the pool: a
 *     job further back would wait out its deadline behind slow queries
 *     (query.timeout_ms each), and is better told to come back.
 *
 * Over any of them a request is refused at once with VENTURE_ERROR_BUSY:
 * 503 and Retry-After for the API, the bar's problem text on a page.
 * They are constants, not query.* settings: the query.* settings bound
 * one query, these bound the pool, which is itself a constant -- a cap
 * configured at or above it would reopen the hole it closes.
 */
#define VENTURE_WEB_QUERY_PER_WORKSPACE (VENTURE_WEB_QUERY_THREADS - 1)
#define VENTURE_WEB_QUERY_PER_PRINCIPAL (2)
#define VENTURE_WEB_QUERY_QUEUE_MAX (16)

G_STATIC_ASSERT(VENTURE_WEB_QUERY_PER_WORKSPACE < VENTURE_WEB_QUERY_THREADS);
G_STATIC_ASSERT(VENTURE_WEB_QUERY_PER_PRINCIPAL < VENTURE_WEB_QUERY_PER_WORKSPACE);

/* How long past query.timeout_ms a request waits before cancelling its
 * query itself: LQL's own deadline normally answers first. */
#define VENTURE_WEB_QUERY_GRACE_MS (1000)

/* How soon a wake that found a transaction open looks again. */
#define VENTURE_WEB_QUERY_REARM_MS (10)

/* --- Who may have a worker ------------------------------------------------ */

/*
 * The admission counts, process-wide, since the pool is. Touched by the
 * main thread (admitting) and by workers (starting, finishing), so under
 * @lock. A GMutex in static storage needs no initialising.
 */
static struct
{
	GMutex		 lock;
	GHashTable	*held;		/* key -> GUINT_TO_POINTER(jobs); none at 0 */
	guint		 admitted;	/* running + queued */
	guint		 running;
	guint64		 ran;		/* jobs whose work ran, ever */
	guint64		 skipped;	/* jobs skipped unrun, ever */
} venture_web_query_admission;

/* How many jobs @key holds, with the lock held. */
static guint
venture_web_query_held_locked(const gchar *key)
{
	if ((NULL == key) || (NULL == venture_web_query_admission.held))
		return 0;

	return GPOINTER_TO_UINT(g_hash_table_lookup(venture_web_query_admission.held, key));
}

/* Adds @delta (+1 or -1) to @key's jobs, with the lock held; a key that
 * reaches zero is dropped, so an idle server holds nothing. */
static void
venture_web_query_hold_locked(
	const gchar	*key,
	gint		 delta
){
	guint jobs;

	if (NULL == key)
		return;

	if (NULL == venture_web_query_admission.held)
		venture_web_query_admission.held = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                                         g_free, NULL);

	jobs = venture_web_query_held_locked(key);
	g_return_if_fail((delta > 0) || (jobs > 0));
	jobs = (guint)((gint)jobs + delta);

	if (0 == jobs)
		g_hash_table_remove(venture_web_query_admission.held, key);
	else
		g_hash_table_insert(venture_web_query_admission.held, g_strdup(key),
		                    GUINT_TO_POINTER(jobs));
}

void
venture_web_query_get_counts(
	guint	*out_admitted,
	guint	*out_running,
	guint	*out_holders,
	guint64	*out_ran,
	guint64	*out_skipped
){
	g_mutex_lock(&venture_web_query_admission.lock);

	if (NULL != out_admitted)
		*out_admitted = venture_web_query_admission.admitted;

	if (NULL != out_running)
		*out_running = venture_web_query_admission.running;

	if (NULL != out_holders)
		*out_holders = (NULL != venture_web_query_admission.held)
			? g_hash_table_size(venture_web_query_admission.held) : 0;

	if (NULL != out_ran)
		*out_ran = venture_web_query_admission.ran;

	if (NULL != out_skipped)
		*out_skipped = venture_web_query_admission.skipped;

	g_mutex_unlock(&venture_web_query_admission.lock);
}

/*
 * The keys a request's jobs count against: its hosted workspace (none
 * when the install is not hosted) and its principal, within that
 * workspace. The principal is worked out once per request and kept on
 * it: a page runs several pieces of LQL work.
 */
static void
venture_web_query_keys(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	gchar			**out_workspace,
	gchar			**out_principal
){
	VentureTenantService *service;
	const gchar *scope = "local";
	const gchar *cached;

	service = venture_tenant_service_get(venture_context_get_database(self->context));

	if (venture_tenant_service_is_enabled(service))
	{
		scope = venture_tenant_service_get_workspace_id(service);
		*out_workspace = g_strdup_printf("workspace:%s", scope);
	}
	else
		*out_workspace = NULL;

	if (NULL == request)
	{
		*out_principal = g_strdup_printf("%s/internal", scope);
		return;
	}

	cached = g_object_get_data(G_OBJECT(request), "venture-web-query-principal");

	if (NULL == cached)
	{
		g_autoptr(VentureAuthPrincipal) principal = NULL;
		gchar *key;

		/* A token acts as its user, and counts as them. */
		principal = venture_auth_authenticate(self->auth, request);

		if ((NULL != principal) && (principal->user_id > 0))
			key = g_strdup_printf("%s/user:%" G_GINT64_FORMAT, scope, principal->user_id);
		else if ((NULL != principal) && (principal->token_id > 0))
			key = g_strdup_printf("%s/token:%" G_GINT64_FORMAT, scope, principal->token_id);
		else
			key = g_strdup_printf("%s/anonymous", scope);

		g_object_set_data_full(G_OBJECT(request), "venture-web-query-principal", key, g_free);
		cached = key;
	}

	*out_principal = g_strdup(cached);
}

/*
 * Admits one job for @workspace (nullable) and @principal, or refuses it
 * with VENTURE_ERROR_BUSY saying which cap it met. On the main thread.
 */
static gboolean
venture_web_query_admit(
	const gchar	 *workspace,
	const gchar	 *principal,
	GError		**error
){
	gboolean admitted = FALSE;

	g_mutex_lock(&venture_web_query_admission.lock);

	if (venture_web_query_held_locked(workspace) >= VENTURE_WEB_QUERY_PER_WORKSPACE)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_BUSY,
		            "This workspace already has %d queries running or waiting, the most "
		            "one workspace may; try again shortly", VENTURE_WEB_QUERY_PER_WORKSPACE);
	else if (venture_web_query_held_locked(principal) >= VENTURE_WEB_QUERY_PER_PRINCIPAL)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_BUSY,
		            "You already have %d queries running or waiting, the most one person "
		            "may; try again shortly", VENTURE_WEB_QUERY_PER_PRINCIPAL);
	else if (venture_web_query_admission.admitted >=
	         VENTURE_WEB_QUERY_THREADS + VENTURE_WEB_QUERY_QUEUE_MAX)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_BUSY,
		            "Every query worker is taken and %d queries are waiting; try again "
		            "shortly", VENTURE_WEB_QUERY_QUEUE_MAX);
	else
	{
		venture_web_query_hold_locked(workspace, 1);
		venture_web_query_hold_locked(principal, 1);
		venture_web_query_admission.admitted++;
		admitted = TRUE;
	}

	g_mutex_unlock(&venture_web_query_admission.lock);

	return admitted;
}

/* Gives back what venture_web_query_admit() took. Any thread. */
static void
venture_web_query_release(
	const gchar	*workspace,
	const gchar	*principal
){
	g_mutex_lock(&venture_web_query_admission.lock);
	venture_web_query_hold_locked(workspace, -1);
	venture_web_query_hold_locked(principal, -1);

	if (venture_web_query_admission.admitted > 0)
		venture_web_query_admission.admitted--;
	else
		g_critical("venture: a query job was released twice");

	g_mutex_unlock(&venture_web_query_admission.lock);
}

/* --- One query on a worker ------------------------------------------------ */

/*
 * One piece of LQL work handed to a worker. Shared by the worker, the
 * stack waiting for it and every wake source in flight, so reference
 * counted; @lock guards what the worker and the main thread both touch.
 * @data is the waiting stack's: the worker runs @work only once it has
 * marked the job @started, and the stack gives a job up (@abandoned, and
 * goes) only while it is not, so @data is never touched after its stack
 * has gone.
 */
typedef struct
{
	gint			 refs;		/* atomic */
	GMutex			 lock;
	GCond			 changed;	/* for a waiter off a fiber */

	/* Main thread only. */
	VentureFiber		*fiber;		/* yielded, waiting; NULL otherwise */
	VentureDatabase		*database;	/* owned */

	/* Fixed before the worker starts. */
	VentureParallelFunc	 work;
	gpointer		 data;
	GCancellable		*cancellable;	/* owned; never NULL */
	gchar			*workspace;	/* admission key; nullable */
	gchar			*principal;	/* admission key */

	/* Under @lock. */
	GQueue			 reads;		/* VentureLqlRead, to read on main */
	gboolean		 blocking;	/* the waiter is not a fiber */
	gboolean		 wake_pending;	/* a wake source is attached */
	gboolean		 started;	/* a worker is running @work */
	gboolean		 finished;	/* a worker is done with the job */
	gboolean		 skipped;	/* ... without running @work */
	gboolean		 abandoned;	/* the waiter gave it up unstarted */
	gboolean		 expired;	/* its deadline passed */
} VentureWebQueryJob;

static VentureWebQueryJob *
venture_web_query_job_ref(VentureWebQueryJob *job)
{
	g_atomic_int_inc(&job->refs);
	return job;
}

static void
venture_web_query_job_unref(gpointer data)
{
	VentureWebQueryJob *job = data;
	VentureLqlRead *read;

	if (!g_atomic_int_dec_and_test(&job->refs))
		return;

	/* Reads never performed are given up: their executor has gone. */
	while (NULL != (read = g_queue_pop_head(&job->reads)))
		venture_lql_read_free(read);

	g_clear_object(&job->database);
	g_clear_object(&job->cancellable);
	g_free(job->workspace);
	g_free(job->principal);
	g_cond_clear(&job->changed);
	g_mutex_clear(&job->lock);
	g_free(job);
}

static gboolean venture_web_query_job_wake(gpointer data);

/*
 * Tells the waiter something changed, with @job->lock held: a fiber gets
 * one wake source on the main context (never more than one pending), a
 * plain stack its condition.
 */
static void
venture_web_query_job_notify_locked(VentureWebQueryJob *job)
{
	GSource *source;

	if (job->blocking)
	{
		g_cond_broadcast(&job->changed);
		return;
	}

	if (job->wake_pending)
		return;

	job->wake_pending = TRUE;

	/* Default priority, as a request's own dispatch: an idle source would
	 * starve behind every other request on a busy server. */
	source = g_idle_source_new();
	g_source_set_priority(source, G_PRIORITY_DEFAULT);
	g_source_set_callback(source, venture_web_query_job_wake,
	                      venture_web_query_job_ref(job), venture_web_query_job_unref);
	g_source_set_static_name(source, "venture-query-wake");
	g_source_attach(source, g_main_context_default());
	g_source_unref(source);
}

/*
 * On the main thread: resumes the yielded fiber so it can read what the
 * worker asked for, or take the answer. Not inside somebody's open
 * transaction -- a nested loop elsewhere may be the one dispatching this,
 * and the fiber would read rows nobody committed -- so it looks again
 * shortly instead, as the Stripe transport's waiter does.
 */
static gboolean
venture_web_query_job_wake(gpointer data)
{
	VentureWebQueryJob *job = data;
	VentureFiber *fiber = job->fiber;

	if ((NULL != fiber) && venture_database_has_transaction(job->database))
	{
		GSource *again = g_timeout_source_new(VENTURE_WEB_QUERY_REARM_MS);

		/* @wake_pending stays set: this source is the pending one. */
		g_source_set_priority(again, G_PRIORITY_DEFAULT);
		g_source_set_callback(again, venture_web_query_job_wake,
		                      venture_web_query_job_ref(job), venture_web_query_job_unref);
		g_source_set_static_name(again, "venture-query-wake");
		g_source_attach(again, g_main_context_default());
		g_source_unref(again);
		return G_SOURCE_REMOVE;
	}

	g_mutex_lock(&job->lock);
	job->wake_pending = FALSE;
	g_mutex_unlock(&job->lock);

	/* The fiber stopped waiting already (its answer came with an earlier
	 * wake): nothing to resume. */
	if (NULL != fiber)
		venture_fiber_wake(fiber);

	return G_SOURCE_REMOVE;
}

/* A #VentureLqlReadDispatch, on the worker: the read goes to the waiter. */
static void
venture_web_query_job_dispatch(
	VentureLqlRead	*read,
	gpointer	 user_data
){
	VentureWebQueryJob *job = user_data;

	g_mutex_lock(&job->lock);
	g_queue_push_tail(&job->reads, read);
	venture_web_query_job_notify_locked(job);
	g_mutex_unlock(&job->lock);
}

/*
 * A query worker: runs the work with its fetches handed back, then says
 * so. The work touches no database, registry or configuration: what it
 * needs of them was done on the main thread first.
 *
 * A job given up while it waited (its stack has gone, @data with it) or
 * cancelled before it started -- the deadline, the client leaving -- is
 * skipped: @work never runs. Either way the job's admission is given
 * back here, before the waiter hears it is done, so a request that has
 * been answered holds nothing.
 */
static void
venture_web_query_worker(
	gpointer	data,
	gpointer	pool_data
){
	VentureWebQueryJob *job = data;
	gboolean skip;

	(void)pool_data;

	g_mutex_lock(&job->lock);
	skip = job->abandoned || g_cancellable_is_cancelled(job->cancellable);
	job->started = !skip;
	g_mutex_unlock(&job->lock);

	if (skip)
	{
		g_mutex_lock(&venture_web_query_admission.lock);
		venture_web_query_admission.skipped++;
		g_mutex_unlock(&venture_web_query_admission.lock);
	}
	else
	{
		g_mutex_lock(&venture_web_query_admission.lock);
		venture_web_query_admission.running++;
		venture_web_query_admission.ran++;
		g_mutex_unlock(&venture_web_query_admission.lock);

		venture_lql_set_read_dispatch(venture_web_query_job_dispatch, job);
		job->work(job->data);
		venture_lql_set_read_dispatch(NULL, NULL);

		g_mutex_lock(&venture_web_query_admission.lock);
		venture_web_query_admission.running--;
		g_mutex_unlock(&venture_web_query_admission.lock);
	}

	venture_web_query_release(job->workspace, job->principal);

	g_mutex_lock(&job->lock);
	job->finished = TRUE;
	job->skipped = skip;
	venture_web_query_job_notify_locked(job);
	g_mutex_unlock(&job->lock);

	/* The worker's own reference; after this it touches nothing of @job. */
	venture_web_query_job_unref(job);
}

/*
 * The pool every query worker comes from: shared, bounded, exclusive --
 * its VENTURE_WEB_QUERY_THREADS threads are started when it is made, and
 * the GError of making them is checked. Made on first use, under a lock;
 * a pool that could not be made is tried again by the next query rather
 * than given up on for good.
 *
 * Exclusive, because a shared pool starts its threads in
 * g_thread_pool_push(), and GLib (2.88, gthreadpool.c) queues the item
 * even when that start fails and push returns FALSE: a caller that took
 * FALSE for "not queued" and freed the item freed something a worker
 * would later run. An exclusive pool's push starts nothing.
 */
static GThreadPool *
venture_web_query_pool(GError **error)
{
	static GMutex lock;
	static GThreadPool *pool = NULL;
	GThreadPool *made;

	g_mutex_lock(&lock);

	if (NULL == pool)
		pool = g_thread_pool_new(venture_web_query_worker, NULL,
		                         VENTURE_WEB_QUERY_THREADS, TRUE, error);

	made = pool;
	g_mutex_unlock(&lock);

	return made;
}

/* Tells the waiter its unstarted job has something to decide: it gives a
 * cancelled job up rather than wait for a worker. Any thread. */
static void
venture_web_query_job_poke(VentureWebQueryJob *job)
{
	g_mutex_lock(&job->lock);

	if (!job->started && !job->finished)
		venture_web_query_job_notify_locked(job);

	g_mutex_unlock(&job->lock);
}

/*
 * The job's cancellable was cancelled -- by the deadline, the client
 * going, or whoever else holds it -- on whichever thread did it: a waiter
 * whose job is not on a worker yet hears it at once, and gives it up.
 */
static void
venture_web_query_job_cancelled(
	GCancellable	*cancellable,
	gpointer	 data
){
	(void)cancellable;

	venture_web_query_job_poke(data);
}

/* The deadline: a query still running this long is cancelled, and one
 * still waiting for a worker stops waiting. */
static gboolean
venture_web_query_job_deadline(gpointer data)
{
	VentureWebQueryJob *job = data;

	g_mutex_lock(&job->lock);
	job->expired = TRUE;
	g_mutex_unlock(&job->lock);

	g_cancellable_cancel(job->cancellable);

	return G_SOURCE_REMOVE;
}

/* The client went away: its query is not worth finishing, or starting. */
static void
venture_web_query_job_abandoned(
	GCancellable	*client,
	gpointer	 data
){
	VentureWebQueryJob *job = data;

	(void)client;

	g_cancellable_cancel(job->cancellable);
}

/*
 * venture_web_query_off_loop:
 * @self: the server
 * @request: (nullable): the request being answered, for its disconnect
 *   hint and whose job it is (its workspace and principal)
 * @environment: (nullable): the environment @work uses, built in this
 *   request: every source's schema is built here first, so @work may ask
 *   any source for its columns. %NULL when @work's sources are prepared
 *   already (venture_lql_query_prepare())
 * @work: (scope call): the LQL work, run on a query worker
 * @data: (closure work): its data; this stack's, kept until @work returns
 * @cancellable: (nullable): what @work passes to LQL; cancelled here at
 *   the deadline (query.timeout_ms and a grace) and when the client goes
 * @error: (out) (optional): return location for a #GError
 *
 * Runs @work(@data) off the main loop: on a query worker, with every
 * fetch it makes read back here, on this stack, inside the request's own
 * scopes as the caller's snapshot, while a fiber yields between them (see
 * the comment at the top). Call it on the main thread, from the route.
 *
 * @work may do anything with LQL on an environment built in this request
 * -- check, plan, complete, run -- and nothing else: no database, entity
 * registry, configuration or context. A #VentureLqlSource's reads and its
 * schema are taken care of; anything else @work needs of the database is
 * read before the call or after it.
 *
 * The job is admitted first ("Who may have a worker" above): over a cap
 * it is refused at once, VENTURE_ERROR_BUSY. One still waiting for a
 * worker at its deadline, or whose client went away, is given up --
 * VENTURE_ERROR_TIMEOUT -- and the worker that reaches it skips it.
 *
 * Returns: %FALSE, with @error set, when @work did not run (refused,
 *   given up, or skipped); %TRUE once @work has returned
 */
static gboolean
venture_web_query_off_loop(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	LqlEnvironment		 *environment,
	VentureParallelFunc	  work,
	gpointer		  data,
	GCancellable		 *cancellable,
	GError			**error
){
	g_autoptr(GError) pool_error = NULL;
	g_autoptr(GError) push_error = NULL;
	g_autofree gchar *workspace = NULL;
	g_autofree gchar *principal = NULL;
	VentureWebQueryJob *job;
	VentureFiber *fiber;
	GThreadPool *pool;
	GSource *deadline;
	GCancellable *client;
	gboolean ran = TRUE;
	gboolean past_deadline = FALSE;
	gulong abandoned = 0;
	gulong cancelled = 0;
	gint64 timeout_ms = VENTURE_LQL_DEFAULT_TIMEOUT_MS;
	gint64 deadline_at;

	/* --- admitted, or refused at once -------------------------------- */
	venture_web_query_keys(self, request, &workspace, &principal);

	if (!venture_web_query_admit(workspace, principal, error))
		return FALSE;

	pool = venture_web_query_pool(&pool_error);

	if (NULL == pool)
	{
		/* No thread to be had. Not run here instead: refused, as busy,
		 * and the next query tries to make the pool again. */
		g_warning("venture: the query workers could not be started: %s",
		          pool_error->message);
		venture_web_query_release(workspace, principal);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_BUSY,
		            "No query worker could be started; try again shortly");
		return FALSE;
	}

	/* Every schema the work could reach, from the registry, here. */
	if (NULL != environment)
		venture_lql_environment_prepare_sources(environment);

	g_object_get(venture_context_get_config(self->context),
	             "query-timeout-ms", &timeout_ms, NULL);

	job = g_new0(VentureWebQueryJob, 1);
	job->refs = 1;
	g_mutex_init(&job->lock);
	g_cond_init(&job->changed);
	g_queue_init(&job->reads);
	job->database = g_object_ref(venture_context_get_database(self->context));
	job->work = work;
	job->data = data;
	/* Always one to cancel, so a job can be given up whatever @work
	 * passes to LQL. */
	job->cancellable = (NULL != cancellable) ? g_object_ref(cancellable) : g_cancellable_new();
	job->workspace = g_steal_pointer(&workspace);
	job->principal = g_steal_pointer(&principal);

	/* A fiber yields while it waits; anything else blocks, and so does a
	 * fiber with a transaction open (a request served meanwhile would
	 * read inside it). */
	fiber = venture_fiber_current();

	if ((NULL != fiber) && venture_database_has_transaction(job->database))
		fiber = NULL;

	job->fiber = fiber;
	job->blocking = (NULL == fiber);

	/* --- what cancels it --------------------------------------------- */
	deadline_at = g_get_monotonic_time() +
		(timeout_ms + VENTURE_WEB_QUERY_GRACE_MS) * G_TIME_SPAN_MILLISECOND;
	deadline = g_timeout_source_new((guint)(timeout_ms + VENTURE_WEB_QUERY_GRACE_MS));
	g_source_set_priority(deadline, G_PRIORITY_DEFAULT);
	g_source_set_callback(deadline, venture_web_query_job_deadline,
	                      venture_web_query_job_ref(job), venture_web_query_job_unref);
	g_source_set_static_name(deadline, "venture-query-deadline");
	g_source_attach(deadline, g_main_context_default());

	client = (NULL != request) ? venture_web_later_get_cancellable(request) : NULL;

	if (NULL != client)
		abandoned = g_cancellable_connect(client,
			G_CALLBACK(venture_web_query_job_abandoned),
			venture_web_query_job_ref(job), venture_web_query_job_unref);

	/* However it is cancelled, the waiter hears of it (and at once when
	 * it is cancelled already). */
	cancelled = g_cancellable_connect(job->cancellable,
		G_CALLBACK(venture_web_query_job_cancelled),
		venture_web_query_job_ref(job), venture_web_query_job_unref);

	/* --- to a worker ----------------------------------------------------- */
	/* The worker's reference goes with the job. An exclusive pool starts
	 * no thread here, so FALSE cannot happen; were it to, GLib has queued
	 * the job all the same (see venture_web_query_pool()), a worker will
	 * run it and drop that reference, and so it is waited for as queued. */
	if (!g_thread_pool_push(pool, venture_web_query_job_ref(job), &push_error))
		g_warning("venture: a query worker could not be started (the query is queued): %s",
		          (NULL != push_error) ? push_error->message : "unknown");

	/* --- wait, reading what the worker asks for -------------------------- */
	for (;;)
	{
		GQueue reads = G_QUEUE_INIT;
		gboolean finished;
		gboolean skipped;
		gboolean expired;
		gboolean give_up;
		VentureLqlRead *read;

		g_mutex_lock(&job->lock);

		if (job->blocking)
		{
			/* Off a fiber: the condition, and the deadline by hand, since
			 * the main loop is not running to fire it. Once it has passed
			 * the work is cancelled and the wait has no deadline left:
			 * waiting until it again would return at once, every time,
			 * and spin this thread until the worker ends. */
			while (g_queue_is_empty(&job->reads) && !job->finished &&
			       (job->started || !g_cancellable_is_cancelled(job->cancellable)))
			{
				if (past_deadline)
				{
					g_cond_wait(&job->changed, &job->lock);
					continue;
				}

				if (!g_cond_wait_until(&job->changed, &job->lock, deadline_at))
				{
					past_deadline = TRUE;
					job->expired = TRUE;

					/* Not under the lock: a cancelled handler may want it. */
					g_mutex_unlock(&job->lock);
					g_cancellable_cancel(job->cancellable);
					g_mutex_lock(&job->lock);
				}
			}
		}

		/* Not yet on a worker and cancelled (the deadline, the client):
		 * given up here. The worker that reaches it skips it. */
		give_up = !job->started && !job->finished &&
		          g_cancellable_is_cancelled(job->cancellable);

		if (give_up)
			job->abandoned = TRUE;

		reads = job->reads;
		g_queue_init(&job->reads);
		finished = job->finished;
		skipped = job->skipped;
		expired = job->expired;
		g_mutex_unlock(&job->lock);

		/* Read here: the request's own stack and scopes, the caller's
		 * snapshot (venture_lql_read_perform()). The worker cannot have
		 * finished with a read outstanding, so @finished means none, and
		 * a job not started has made none. */
		while (NULL != (read = g_queue_pop_head(&reads)))
			venture_lql_read_perform(read);

		if (give_up || (finished && skipped))
		{
			if (expired)
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
				            "No query worker was free before the deadline (query.timeout_ms, "
				            "%" G_GINT64_FORMAT " ms); try again shortly", timeout_ms);
			else
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
				                    "The query was cancelled before a worker started it");

			ran = FALSE;
			break;
		}

		if (finished)
			break;

		if (!job->blocking)
			venture_fiber_yield();
	}

	if (0 != abandoned)
		g_cancellable_disconnect(client, abandoned);

	if (0 != cancelled)
		g_cancellable_disconnect(job->cancellable, cancelled);

	g_source_destroy(deadline);
	g_source_unref(deadline);
	job->fiber = NULL;
	venture_web_query_job_unref(job);

	return ran;
}

gboolean
venture_web_server_query_off_loop(
	VentureWebServer	 *self,
	VentureParallelFunc	  work,
	gpointer		  data,
	GCancellable		 *cancellable,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_WEB_SERVER(self), FALSE);

	return venture_web_query_off_loop(self, NULL, NULL, work, data, cancellable, error);
}

/* One prepared query, as work for venture_web_query_off_loop(). */
typedef struct
{
	VentureLqlPrepared	*prepared;
	GCancellable		*cancellable;
	JsonNode		*document;
	GError			*error;
} VentureWebQueryRun;

static void
venture_web_query_run_work(gpointer data)
{
	VentureWebQueryRun *run = data;

	run->document = venture_lql_query_execute(run->prepared, run->cancellable, &run->error);
}

/*
 * One query as the request's caller, the whole way: prepared here, inside
 * the request's access scope, and evaluated off the main loop. The
 * disconnect hint is the one venture_web_answer_later() leaves on the
 * request.
 */
static JsonNode *
venture_web_query_run(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	LqlEnvironment		 *environment,
	const gchar		 *text,
	gboolean		  explain,
	GError			**error
){
	g_autoptr(VentureLqlPrepared) prepared = NULL;
	g_autoptr(GCancellable) cancellable = NULL;
	VentureWebQueryRun run;

	/* Never handed to LQL, whose lexer reads UTF-8 alone: refused before
	 * anything is prepared or a worker is asked. */
	if (!venture_lql_text_check_utf8(text, error))
		return NULL;

	/* Prepared here: its sources' schemas are built by the walk, so the
	 * off-loop call need not build every other one. */
	prepared = venture_lql_query_prepare(environment, text, explain, error);

	if (NULL == prepared)
		return NULL;

	cancellable = g_cancellable_new();
	memset(&run, 0, sizeof run);
	run.prepared = prepared;
	run.cancellable = cancellable;

	if (!venture_web_query_off_loop(self, request, NULL, venture_web_query_run_work, &run,
	                                cancellable, error))
		return NULL;

	if (NULL == run.document)
		g_propagate_error(error, run.error);

	return run.document;
}

/* --- POST /api/v1/query ------------------------------------------------- */

/* The members a query request may carry; anything else is refused. */
static const gchar *const venture_web_query_members[] = {
	"query", "format", "explain", NULL
};

/*
 * A query's failure, as the standard error body plus where in the query
 * it is (line, column, and LQL's caret excerpt in detail) when it has a
 * place.
 */
static HtmxResponse *
venture_web_query_error_response(
	const gchar	*text,
	const GError	*error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *response;
	guint line = 0;
	guint column = 0;
	gboolean placed;

	placed = venture_lql_query_error_position(error, &line, &column);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	venture_json_builder_add_error(builder, error);
	json_builder_set_member_name(builder, "line");

	if (placed)
		json_builder_add_int_value(builder, line);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "column");

	if (placed)
		json_builder_add_int_value(builder, column);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "detail");

	if (placed && (NULL != text))
	{
		g_autoptr(GError) positioned = NULL;
		g_autofree gchar *detail = NULL;

		/* lql_error_format() draws the caret; it reads LQL's domain. */
		positioned = g_error_new_literal(LQL_ERROR, LQL_ERROR_SYNTAX, error->message);
		detail = lql_error_format(text, positioned);
		json_builder_add_string_value(builder, detail);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	response = venture_web_json_response(node,
		(VENTURE_ERROR == error->domain)
			? venture_error_to_http_status((VentureError)error->code) : 500);
	g_object_set_data_full(G_OBJECT(response), "venture-error", g_error_copy(error),
	                       (GDestroyNotify)g_error_free);

	/* Every worker the caller may have is taken (503): when to come back. */
	if ((VENTURE_ERROR == error->domain) && (VENTURE_ERROR_BUSY == error->code))
	{
		htmx_response_add_header(response, "Retry-After", VENTURE_WEB_BUSY_RETRY_AFTER);
		htmx_response_add_header(response, "Cache-Control", "no-store");
	}

	return response;
}

/*
 * POST /api/v1/query - one LQL query, read-only, as the caller.
 *
 * The body is {"query": TEXT, "format": "json" | "csv", "explain": BOOL};
 * ?format=csv does what "format" does. The caller is admitted as the
 * list API admits a reader, an unqualified fetch is scoped to the
 * business selected (or ?organization_id=N and everything beneath it),
 * and a type the list API would refuse is refused the same way. The
 * answer is the venture.query/1 document (docs/api.org), or its rows as
 * CSV. Nothing here writes: the environment holds VENTURE's record
 * sources and nothing that reads a file, runs a program or talks SQL.
 */
static HtmxResponse *
venture_web_api_query(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) document = NULL;
	g_autoptr(GArray) tree = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GList) members = NULL;
	const gchar *text = NULL;
	const gchar *format = NULL;
	const gchar *organization;
	gboolean explain = FALSE;
	JsonObject *object;
	GList *item;

	(void)params;

	body = htmx_request_get_json(request, NULL);

	if ((NULL == body) || !JSON_NODE_HOLDS_OBJECT(body))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The request body must be a JSON object: "
		                    "{\"query\": \"fetch ticket | limit 10\"}");
		return venture_web_error_response(error);
	}

	/* --- the request document, strictly ---------------------------- */
	object = json_node_get_object(body);
	members = json_object_get_members(object);

	for (item = members; NULL != item; item = item->next)
	{
		if (!g_strv_contains(venture_web_query_members, item->data))
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "A query request has no member \"%s\"; it takes query, format "
			            "and explain", (const gchar *)item->data);
			return venture_web_error_response(error);
		}
	}

	if (json_object_has_member(object, "query"))
	{
		JsonNode *member = json_object_get_member(object, "query");

		if (JSON_NODE_HOLDS_VALUE(member) && (G_TYPE_STRING == json_node_get_value_type(member)))
			text = json_node_get_string(member);
	}

	if (NULL == text)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "\"query\" must be the LQL text, such as "
		                    "\"fetch ticket | summarize n = count(), by: status\"");
		return venture_web_error_response(error);
	}

	/* Before an environment is built or anything reaches LQL, whose lexer
	 * reads UTF-8 alone (venture_lql_text_check_utf8()). json-glib already
	 * refuses a body that is not UTF-8, raw or as a lone surrogate escape
	 * (a 400 above); this holds whatever the parser does. */
	if (!venture_lql_text_check_utf8(text, &error))
		return venture_web_query_error_response(NULL, error);

	if (json_object_has_member(object, "format"))
	{
		JsonNode *member = json_object_get_member(object, "format");

		if (JSON_NODE_HOLDS_VALUE(member) && (G_TYPE_STRING == json_node_get_value_type(member)))
			format = json_node_get_string(member);
		else
			format = "";
	}

	if (NULL == format)
		format = htmx_request_get_query_param(request, "format");

	/* Echoed in its refusal below, so the same check. */
	if (!venture_lql_text_check_utf8(format, &error))
		return venture_web_error_response(error);

	if ((NULL != format) && (0 != g_strcmp0(format, "json")) && (0 != g_strcmp0(format, "csv")))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a query format; use json or csv", format);
		return venture_web_error_response(error);
	}

	if (json_object_has_member(object, "explain"))
	{
		JsonNode *member = json_object_get_member(object, "explain");

		if (!JSON_NODE_HOLDS_VALUE(member) || (G_TYPE_BOOLEAN != json_node_get_value_type(member)))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "\"explain\" must be true or false");
			return venture_web_error_response(error);
		}

		explain = json_node_get_boolean(member);
	}

	/* --- the caller's environment ------------------------------------ */
	/* ?organization_id=N is the business a list would be scoped to with
	 * it, and everything beneath; otherwise the one picked in the
	 * browser, as GET /api/v1/<type> does. */
	organization = htmx_request_get_query_param(request, "organization_id");
	tree = venture_web_organization_tree(self, !venture_string_is_empty(organization)
		? g_ascii_strtoll(organization, NULL, 10)
		: venture_web_active_organization(self, request));
	environment = venture_web_lql_environment_for(self, request,
		(const gint64 *)tree->data, tree->len, &error);

	if (NULL == environment)
		return venture_web_error_response(error);

	/* Off the main loop, the fetches read here (see the top). */
	document = venture_web_query_run(self, request, environment, text, explain, &error);

	if (NULL == document)
		return venture_web_query_error_response(text, error);

	if (0 == g_strcmp0(format, "csv"))
	{
		g_autofree gchar *csv = venture_lql_query_to_csv(document);
		HtmxResponse *response;

		response = htmx_response_new_with_content(csv);
		htmx_response_set_content_type(response, "text/csv; charset=utf-8");
		htmx_response_set_status(response, 200);

		return response;
	}

	return venture_web_json_response(document, 200);
}

/* Answered from a fiber, so the query runs off the main loop. */
VENTURE_WEB_LATER_ROUTE(venture_web_api_query_later, venture_web_api_query)

/* --- The query widget's runner: as the viewer --------------------------- */

/*
 * What a dashboard's query widgets run through while one request renders
 * the page: the request (whose credential is the viewer's), the scope's
 * organizations, and the environment, built on the first query and kept
 * for the rest of the page.
 */
typedef struct
{
	VentureWebServer	*server;
	HtmxRequest		*request;
	const gint64		*organizations;
	gsize			 n_organizations;
	LqlEnvironment		*environment;
	GError			*refusal;
} VentureWebWidgetQuery;

static void
venture_web_widget_query_clear(VentureWebWidgetQuery *runner)
{
	g_clear_object(&runner->environment);
	g_clear_error(&runner->refusal);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(VentureWebWidgetQuery, venture_web_widget_query_clear)

/* A #VentureWidgetQueryFunc: @query as the viewer of the page. */
static JsonNode *
venture_web_widget_query_run(
	const gchar	 *query,
	gpointer	  user_data,
	GError		**error
){
	VentureWebWidgetQuery *runner = user_data;

	if ((NULL == runner->environment) && (NULL == runner->refusal))
		runner->environment = venture_web_lql_environment_for(runner->server,
			runner->request, runner->organizations, runner->n_organizations,
			&runner->refusal);

	if (NULL == runner->environment)
	{
		g_propagate_error(error, g_error_copy(runner->refusal));
		return NULL;
	}

	return venture_web_query_run(runner->server, runner->request,
	                             runner->environment, query, FALSE, error);
}

/* Points @scope's query function at @runner, for @request's viewer. */
static void
venture_web_widget_query_bind(
	VentureWebServer	*server,
	HtmxRequest		*request,
	VentureWidgetScope	*scope,
	VentureWebWidgetQuery	*runner
){
	runner->server = server;
	runner->request = request;
	runner->organizations = scope->organization_ids;
	runner->n_organizations = scope->n_organizations;
	scope->query = venture_web_widget_query_run;
	scope->query_data = runner;
}
