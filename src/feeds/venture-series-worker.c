/*
 * venture-series-worker.c - The thread that fetches market data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Ownership, which is the whole design:
 *
 * - The main thread owns the worker object, frozen sources and nothing the
 *   worker touches. It talks to the thread only by invoking commands on
 *   the thread's context (g_main_context_invoke_full()), each carrying
 *   plain data.
 * - The thread owns everything under WorkerSource: each source's parsed
 *   settings, its store's writer handle, its units' schedules, its quota,
 *   the HTTP session. No lock guards them because nothing else reads them.
 * - The one thing both sides read is the published status and the live
 *   count, under a mutex.
 * - A finished run goes back by g_main_context_invoke_full() on the
 *   default main context, holding a reference on the worker, and is
 *   emitted there as #VentureSeriesWorker::run-finished. The worker never
 *   writes the database; the service does, on the main thread.
 * - A store copy runs on a short-lived thread of its own (a GTask), which
 *   touches nothing but its own SQLite connections: a copy of a large
 *   store would otherwise hold this loop -- every fetch, every command --
 *   for as long as it takes.
 *
 * A source's units are fetched one after another, never at once: the
 * quota is per source, the store has one writer, and the far end is one
 * host. Different sources run concurrently, interleaved on the loop.
 */

#include "venture.h"
#include "feeds/venture-feeds-private.h"

#include <errno.h>
#include <string.h>

/* Never sleep longer than this between looks at the schedule, so a cron
 * schedule's minute and a clock jump are both noticed. */
#define WORKER_MAX_SLEEP_MS (60 * 1000)

/* The back-off after a check that found nothing new: 1, 5, 15, 30 min. */
static const gint64 worker_backoff[] = { 60, 300, 900, 1800 };

/* --- Runs ---------------------------------------------------------------------- */

G_DEFINE_BOXED_TYPE(VentureFeedRun, venture_feed_run, venture_feed_run_ref, venture_feed_run_unref)

VentureFeedRun *
venture_feed_run_new_internal(
	VentureFeedSource		*source,
	VentureDataSourceRunTrigger	 trigger,
	gint64				 started_at
){
	VentureFeedRun *self;

	self = g_new0(VentureFeedRun, 1);
	g_atomic_ref_count_init(&self->ref);
	self->source_id = source->id;
	self->organization_id = source->organization_id;
	self->trigger = trigger;
	self->started_at = started_at;
	self->finished_at = started_at;
	self->quota_limit = source->requests_per_hour;
	self->remote_used = -1;
	self->remote_limit = -1;
	self->max_records = source->max_records;
	self->notes = g_ptr_array_new_with_free_func(g_free);
	self->records = g_ptr_array_new_with_free_func(venture_feeds_record_free);
	self->venues = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(self->venues, NULL);
	self->payloads = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                       (GDestroyNotify)json_node_unref);

	return self;
}

VentureFeedRun *
venture_feed_run_ref(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	g_atomic_ref_count_inc(&self->ref);

	return self;
}

void
venture_feed_run_unref(VentureFeedRun *self)
{
	if (NULL == self)
		return;

	if (!g_atomic_ref_count_dec(&self->ref))
		return;

	g_free(self->error);
	g_ptr_array_unref(self->notes);
	g_ptr_array_unref(self->records);
	g_ptr_array_unref(self->venues);
	g_hash_table_unref(self->payloads);
	g_free(self->push_id);
	g_free(self);
}

const gchar *
venture_feed_run_get_push_id(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->push_id;
}

gint64
venture_feed_run_get_source_id(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->source_id;
}

gint64
venture_feed_run_get_organization_id(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->organization_id;
}

VentureDataSourceRunStatus
venture_feed_run_get_status(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);

	return self->status;
}

VentureDataSourceRunTrigger
venture_feed_run_get_trigger(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, VENTURE_DATA_SOURCE_RUN_TRIGGER_SCHEDULE);

	return self->trigger;
}

gint64
venture_feed_run_get_rows(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->rows;
}

const gchar *
venture_feed_run_get_error(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->error;
}

const gchar *const *
venture_feed_run_get_venues(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return (const gchar *const *)self->venues->pdata;
}

gint64
venture_feed_run_get_started_at(VentureFeedRun *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->started_at;
}

void
venture_feed_run_set_payload(
	VentureFeedRun	*self,
	const gchar	*name,
	JsonNode	*payload
){
	g_return_if_fail(NULL != self);
	g_return_if_fail(NULL != name);

	if (NULL == payload)
		g_hash_table_remove(self->payloads, name);
	else
		g_hash_table_replace(self->payloads, g_strdup(name), payload);
}

JsonNode *
venture_feed_run_get_payload(
	VentureFeedRun	*self,
	const gchar	*name
){
	g_return_val_if_fail(NULL != self, NULL);

	return (NULL != name) ? g_hash_table_lookup(self->payloads, name) : NULL;
}

/* At most this many notes on one run: it is a record, not a log. */
#define WORKER_MAX_RUN_NOTES (40)

void
venture_feed_run_add_note(
	VentureFeedRun	*self,
	const gchar	*note
){
	g_return_if_fail(NULL != self);

	if (venture_string_is_empty(note) || (self->notes->len >= WORKER_MAX_RUN_NOTES))
		return;

	g_ptr_array_add(self->notes, g_strdup(note));
}

static void
venture_feed_run_add_venue(
	VentureFeedRun	*self,
	const gchar	*venue
){
	guint i;

	for (i = 0; i + 1 < self->venues->len; i++)
	{
		if (0 == g_strcmp0(g_ptr_array_index(self->venues, i), venue))
			return;
	}

	/* Keep the trailing NULL last. */
	g_ptr_array_insert(self->venues, (gint)(self->venues->len - 1), g_strdup(venue));
}

/*
 * The status follows from the units: all deferred is deferred, none failed
 * is ok, none succeeded is failed, the rest partial.
 */
void
venture_feed_run_finish(
	VentureFeedRun	*self,
	gint64		 finished_at
){
	self->finished_at = finished_at;

	if ((self->deferred > 0) && (0 == self->succeeded) && (0 == self->failed))
		self->status = VENTURE_DATA_SOURCE_RUN_STATUS_DEFERRED;
	else if (0 == self->failed)
		self->status = VENTURE_DATA_SOURCE_RUN_STATUS_OK;
	else if (0 == self->succeeded)
		self->status = VENTURE_DATA_SOURCE_RUN_STATUS_FAILED;
	else
		self->status = VENTURE_DATA_SOURCE_RUN_STATUS_PARTIAL;
}

/* --- Scheduling ---------------------------------------------------------------- */

gint64
venture_feeds_schedule_next_auto(
	gboolean	 new_data,
	gint64		 next_expected,
	gint64		 retry_after,
	guint		*backoff_step,
	gint64		 now
){
	gint64 next;
	guint step;

	g_return_val_if_fail(NULL != backoff_step, now + worker_backoff[0]);

	if (new_data && (retry_after <= 0))
	{
		*backoff_step = 0;

		if (VENTURE_SERIES_NONE == next_expected)
			return now + VENTURE_SERIES_INTERVAL_DEFAULT;

		next = next_expected - VENTURE_FEEDS_CHECK_EARLY;

		/* Expected already: the venue is late, which is what the
		 * back-off is for, starting at its first step. */
		return (next > now) ? next : now + worker_backoff[0];
	}

	step = MIN(*backoff_step, G_N_ELEMENTS(worker_backoff) - 1);
	next = now + MAX(worker_backoff[step], retry_after);
	*backoff_step = MIN(step + 1, G_N_ELEMENTS(worker_backoff) - 1);

	return next;
}

/* --- Worker state ------------------------------------------------------------------ */

typedef struct
{
	gchar	 *name;
	gint64	  due;		/* G_MAXINT64: not scheduled */
	guint	  backoff;
	gchar	**venues;	/* what the last fetch of it produced */
} WorkerUnit;

typedef struct _WorkerSource WorkerSource;
typedef struct _WorkerPass WorkerPass;

/* A body somebody pushed, waiting for its own run. */
typedef struct
{
	gchar	*id;
	GBytes	*body;
} WorkerPush;

static void
worker_push_free(WorkerPush *push)
{
	if (NULL == push)
		return;

	g_free(push->id);
	g_clear_pointer(&push->body, g_bytes_unref);
	g_free(push);
}

struct _VentureSeriesWorker
{
	GObject			 parent_instance;

	GMainContext		*main_context;	/* the default one: where runs are delivered */
	GMainContext		*context;
	GMainLoop		*loop;
	GThread			*thread;
	GAsyncQueue		*started;
	GCancellable		*cancellable;

	GMutex			 lock;		/* guards status, live, pending, flushed */
	gchar			*status;	/* JSON text: nested JSON-GLib nodes
						 * are not safe to share between threads */
	guint			 live;
	GPtrArray		*pending;	/* WorkerDelivery: handed back, not yet emitted */
	GPtrArray		*flushed;	/* VentureFeedRun: kept for the service at stop */

	/* The thread's alone. */
	GHashTable		*sources;	/* gint64* -> WorkerSource */
	SoupSession		*session;
	GSource			*timer;
	guint			 copies;	/* store copies on their own threads */

	/* Set by stop() on the main thread, read here: atomic. */
	gint			 stopping;
};

struct _WorkerSource
{
	VentureSeriesWorker	*worker;
	VentureFeedSource	*spec;
	VentureSeriesStore	*store;
	GHashTable		*units;		/* name -> WorkerUnit */
	FeedQuota		 quota;
	gboolean		 quota_loaded;

	WorkerPass		*pass;		/* in flight */
	VentureFeedRun		*open_run;	/* scheduled passes gathered */
	GQueue			 pushes;	/* WorkerPush, oldest first */
	gboolean		 manual_pending;
	VentureDataSourceRunTrigger manual_trigger;

	gint64			 last_region;
	gint64			 last_purge;
	gint64			 last_cron;
	gboolean		 removed;
};

struct _WorkerPass
{
	WorkerSource		*source;
	GPtrArray		*units;		/* gchar* */
	guint			 index;
	VentureFeedRun		*run;		/* the run it adds to */
	gboolean		 manual;
	gboolean		 failed;
	gboolean		 wrote;
	VentureFeedRequest	*request;	/* the unit in flight */
	gint64			 started_at;
	WorkerPush		*push;		/* the body this pass reads, if pushed */
};

enum
{
	SIGNAL_RUN_FINISHED,
	SIGNAL_BACKUP_FINISHED,
	N_SIGNALS
};

static guint worker_signals[N_SIGNALS] = { 0 };

G_DEFINE_FINAL_TYPE(VentureSeriesWorker, venture_series_worker, G_TYPE_OBJECT)

static gint64
worker_now(void)
{
	return g_get_real_time() / G_USEC_PER_SEC;
}

static void
worker_live_add(
	VentureSeriesWorker	*self,
	gint			 delta
){
	g_mutex_lock(&self->lock);
	self->live = (guint)MAX((gint)self->live + delta, 0);
	g_mutex_unlock(&self->lock);
}

static void
worker_unit_free(gpointer data)
{
	WorkerUnit *unit = data;

	g_free(unit->name);
	g_strfreev(unit->venues);
	g_free(unit);
}

static void
worker_source_free(gpointer data)
{
	WorkerSource *source = data;
	WorkerPush *push;

	/* A push never started is live work no run will answer now. */
	while (NULL != (push = g_queue_pop_head(&source->pushes)))
	{
		worker_live_add(source->worker, -1);
		worker_push_free(push);
	}

	g_clear_object(&source->store);
	g_clear_pointer(&source->units, g_hash_table_unref);
	g_clear_pointer(&source->open_run, venture_feed_run_unref);
	g_clear_pointer(&source->spec, venture_feed_source_unref);
	g_free(source);
}

/*
 * A sync asked for and not yet started is live work: the command that
 * asked has been freed and the pass that answers starts on the worker's
 * next timer, and in between nothing else counts it. Without this a test
 * waiting for count_pending() to reach zero could find it zero between
 * the two and look for a run that had not happened yet.
 */
static void
worker_source_set_manual(
	WorkerSource	*source,
	gboolean	 pending
){
	if (pending == source->manual_pending)
		return;

	source->manual_pending = pending;
	worker_live_add(source->worker, pending ? 1 : -1);
}

static void worker_reschedule(VentureSeriesWorker *self);
static JsonObject *worker_parse_object(const gchar *json);
static void worker_publish_status(VentureSeriesWorker *self);
static void worker_pass_next(WorkerPass *pass);

/* --- Delivery to the main thread ------------------------------------------------- */

typedef struct
{
	VentureSeriesWorker	*worker;
	VentureFeedRun		*run;		/* NULL once stop() took it */
} WorkerDelivery;

static void
worker_delivery_free(gpointer data)
{
	WorkerDelivery *delivery = data;

	worker_live_add(delivery->worker, -1);
	g_clear_pointer(&delivery->run, venture_feed_run_unref);
	g_object_unref(delivery->worker);
	g_free(delivery);
}

static gboolean
worker_deliver(gpointer data)
{
	WorkerDelivery *delivery = data;
	VentureSeriesWorker *self = delivery->worker;
	g_autoptr(VentureFeedRun) run = NULL;

	/* stop() takes every run not yet emitted for the service to write
	 * itself; one it took is gone from the list and from here. */
	g_mutex_lock(&self->lock);

	if (g_ptr_array_remove_fast(self->pending, delivery))
		run = g_steal_pointer(&delivery->run);

	g_mutex_unlock(&self->lock);

	if (NULL != run)
		g_signal_emit(self, worker_signals[SIGNAL_RUN_FINISHED], 0, run);

	return G_SOURCE_REMOVE;
}

/* Keeps a run for venture_series_worker_take_flushed(): the loop it would
 * have been delivered on is stopping. */
static void
worker_keep_flushed(
	VentureSeriesWorker	*self,
	VentureFeedRun		*run
){
	g_mutex_lock(&self->lock);
	g_ptr_array_add(self->flushed, venture_feed_run_ref(run));
	g_mutex_unlock(&self->lock);
}

/*
 * Hands a finished run to the main context. The reference on the worker
 * keeps it alive until the main thread has seen the run, however long the
 * main loop takes to get there. While stopping, the run is kept for the
 * service instead: stop() is about to join this thread, and the service
 * writes what it is handed then.
 */
static void
worker_hand_back(
	VentureSeriesWorker	*self,
	VentureFeedRun		*run
){
	WorkerDelivery *delivery;

	if (g_atomic_int_get(&self->stopping))
	{
		worker_keep_flushed(self, run);
		return;
	}

	delivery = g_new0(WorkerDelivery, 1);
	delivery->worker = g_object_ref(self);
	delivery->run = venture_feed_run_ref(run);
	worker_live_add(self, 1);

	g_mutex_lock(&self->lock);
	g_ptr_array_add(self->pending, delivery);
	g_mutex_unlock(&self->lock);

	g_main_context_invoke_full(self->main_context, G_PRIORITY_DEFAULT, worker_deliver,
	                           delivery, worker_delivery_free);
}

/*
 * A scheduled window still gathering passes, finished now and handed
 * back: the source is going (removed, or the worker stopping), and a
 * window dropped here was a run nobody ever recorded.
 */
static void
worker_flush_open_run(WorkerSource *source)
{
	VentureFeedRun *open = source->open_run;

	if (NULL == open)
		return;

	venture_feed_run_finish(open, worker_now());
	worker_hand_back(source->worker, open);
	g_clear_pointer(&source->open_run, venture_feed_run_unref);
}

/* A store copy's answer, plain data for the main context. */
typedef struct
{
	VentureSeriesWorker	*worker;
	gint64			 tag;
	gchar			*destination;
	gchar			*sha256;
	guint64			 size;
	gchar			*error;
} WorkerBackupDelivery;

static void
worker_backup_delivery_free(gpointer data)
{
	WorkerBackupDelivery *delivery = data;

	worker_live_add(delivery->worker, -1);
	g_object_unref(delivery->worker);
	g_free(delivery->destination);
	g_free(delivery->sha256);
	g_free(delivery->error);
	g_free(delivery);
}

static gboolean
worker_backup_deliver(gpointer data)
{
	WorkerBackupDelivery *delivery = data;

	if (!g_atomic_int_get(&delivery->worker->stopping))
		g_signal_emit(delivery->worker, worker_signals[SIGNAL_BACKUP_FINISHED], 0,
		              delivery->tag, delivery->destination, delivery->sha256,
		              delivery->size, delivery->error);

	return G_SOURCE_REMOVE;
}

/* One store copy, on its own thread. */
typedef struct
{
	VentureSeriesWorker	*worker;	/* not owned: stop() waits for copies */
	gint64			 tag;
	gchar			*store_dir;
	gchar			*destination;
	gchar			*sha256;
	guint64			 size;
	gchar			*error;
} WorkerCopy;

static void
worker_copy_free(gpointer data)
{
	WorkerCopy *copy = data;

	g_free(copy->store_dir);
	g_free(copy->destination);
	g_free(copy->sha256);
	g_free(copy->error);
	g_free(copy);
}

/* The copy itself: its own connections to the store and the file, and
 * nothing of the worker's. */
static void
worker_copy_thread(
	GTask		*task,
	gpointer	 source_object,
	gpointer	 task_data,
	GCancellable	*cancellable
){
	WorkerCopy *copy = task_data;
	g_autoptr(GError) error = NULL;

	(void)source_object;

	if (!venture_series_store_backup(copy->store_dir, copy->destination, 0, cancellable,
	                                 &copy->sha256, &copy->size, &error))
		copy->error = g_strdup(error->message);

	g_task_return_boolean(task, TRUE);
}

/* Back on the worker's loop: the answer goes on to the main context. */
static void
worker_copy_done(
	GObject		*source_object,
	GAsyncResult	*result,
	gpointer	 user_data
){
	WorkerCopy *copy = g_task_get_task_data(G_TASK(result));
	VentureSeriesWorker *self = copy->worker;
	WorkerBackupDelivery *delivery;

	(void)source_object;
	(void)user_data;

	self->copies--;

	if (NULL != copy->error)
		g_message("feeds: backup %" G_GINT64_FORMAT " of %s failed: %s",
		          copy->tag, copy->store_dir, copy->error);

	if (g_atomic_int_get(&self->stopping))
	{
		worker_live_add(self, -1);
		return;
	}

	/* The live count taken when the copy started is handed on to the
	 * delivery, so a test never sees zero between the two. */
	delivery = g_new0(WorkerBackupDelivery, 1);
	delivery->worker = g_object_ref(self);
	delivery->tag = copy->tag;
	delivery->destination = g_strdup(copy->destination);
	delivery->sha256 = g_steal_pointer(&copy->sha256);
	delivery->size = copy->size;
	delivery->error = g_steal_pointer(&copy->error);

	g_main_context_invoke_full(self->main_context, G_PRIORITY_DEFAULT, worker_backup_deliver,
	                           delivery, worker_backup_delivery_free);
}

/*
 * Copies one store on a thread of its own and hands the answer back.
 * The copy reads through a connection of its own, inside one read
 * transaction, so a source whose writer is open keeps writing and the
 * copy is the store as of one commit. It used to run here, on the loop:
 * a gigabyte store held every fetch and every command -- and with them
 * every sync somebody was waiting for -- until it was done.
 */
static void
worker_backup_store(
	VentureSeriesWorker	*self,
	gint64			 tag,
	const gchar		*store_dir,
	const gchar		*destination
){
	g_autoptr(GTask) task = NULL;
	WorkerCopy *copy;

	copy = g_new0(WorkerCopy, 1);
	copy->worker = self;
	copy->tag = tag;
	copy->store_dir = g_strdup(store_dir);
	copy->destination = g_strdup(destination);

	self->copies++;
	worker_live_add(self, 1);

	/* Made on this thread, so it completes on this thread's loop. */
	task = g_task_new(NULL, self->cancellable, worker_copy_done, NULL);
	g_task_set_task_data(task, copy, worker_copy_free);
	g_task_run_in_thread(task, worker_copy_thread);
}

/* --- Stores ---------------------------------------------------------------------- */

static gboolean
worker_store_open(
	WorkerSource	 *source,
	GError		**error
){
	if (NULL != source->store)
		return TRUE;

	source->store = venture_series_store_open(source->spec->store_dir, error);

	if (NULL == source->store)
		return FALSE;

	venture_series_store_set_max_bytes(source->store, source->spec->max_store_bytes);

	return TRUE;
}

/* The quota survives a restart in the store's meta, so a restart is not a
 * fresh budget. */
static void
worker_quota_load(WorkerSource *source)
{
	g_autofree gchar *window = NULL;
	g_autofree gchar *spent = NULL;

	if (source->quota_loaded || (NULL == source->store))
		return;

	window = venture_series_store_get_meta(source->store, "quota_window", NULL);
	spent = venture_series_store_get_meta(source->store, "quota_spent", NULL);
	source->quota.window_start = (NULL != window) ? g_ascii_strtoll(window, NULL, 10) : 0;
	source->quota.spent = (NULL != spent) ? g_ascii_strtoll(spent, NULL, 10) : 0;
	source->quota_loaded = TRUE;
}

static void
worker_quota_save(WorkerSource *source)
{
	g_autofree gchar *window = NULL;
	g_autofree gchar *spent = NULL;
	g_autoptr(GError) error = NULL;

	if ((NULL == source->store) || (0 == source->spec->requests_per_hour))
		return;

	window = g_strdup_printf("%" G_GINT64_FORMAT, source->quota.window_start);
	spent = g_strdup_printf("%" G_GINT64_FORMAT, source->quota.spent);

	if (!venture_series_store_set_meta(source->store, "quota_window", window, &error) ||
	    !venture_series_store_set_meta(source->store, "quota_spent", spent, &error))
		g_message("feeds: source %" G_GINT64_FORMAT ": could not keep its quota: %s",
		          source->spec->id, error->message);
}

gboolean
venture_series_worker_delete_store(
	const gchar	 *store_dir,
	GError		**error
){
	static const gchar *const files[] = { "store.db", "store.db-wal", "store.db-shm", NULL };
	guint i;

	g_return_val_if_fail(NULL != store_dir, FALSE);

	for (i = 0; NULL != files[i]; i++)
	{
		g_autofree gchar *path = g_build_filename(store_dir, files[i], NULL);

		if ((0 != g_unlink(path)) && (ENOENT != errno))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
			            "Could not delete the store's %s: %s", files[i], g_strerror(errno));
			return FALSE;
		}
	}

	if ((0 != g_rmdir(store_dir)) && (ENOENT != errno))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Could not remove the store's directory: %s", g_strerror(errno));
		return FALSE;
	}

	return TRUE;
}

/* --- Units' schedule ------------------------------------------------------------- */

typedef enum
{
	SCHEDULE_AUTO = 0,
	SCHEDULE_HOURLY,
	SCHEDULE_MANUAL,
	SCHEDULE_CRON
} WorkerSchedule;

static WorkerSchedule
worker_schedule_kind(VentureFeedSource *spec)
{
	if (!spec->enabled || (0 == g_strcmp0(spec->schedule, "manual")))
		return SCHEDULE_MANUAL;

	if ((NULL == spec->schedule) || (0 == g_strcmp0(spec->schedule, "auto")) ||
	    ('\0' == spec->schedule[0]))
		return SCHEDULE_AUTO;

	if (0 == g_strcmp0(spec->schedule, "hourly"))
		return SCHEDULE_HOURLY;

	return SCHEDULE_CRON;
}

/* The next minute boundary, when a cron schedule is looked at again. */
static gint64
worker_next_minute(gint64 now)
{
	return (now / 60 + 1) * 60;
}

/*
 * Where a unit stands when a source arrives or changes: a new unit of an
 * auto or hourly source is due now, a cron source's units at the next
 * minute, a manual source's never. A unit already known keeps its time
 * unless the schedule now says it never runs.
 */
static void
worker_source_sync_units(WorkerSource *source)
{
	g_autoptr(GHashTable) wanted = NULL;
	GHashTableIter iter;
	gpointer key;
	WorkerSchedule kind;
	gint64 now;
	guint i;

	now = worker_now();
	kind = worker_schedule_kind(source->spec);
	wanted = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = 0; (NULL != source->spec->units) && (NULL != source->spec->units[i]); i++)
	{
		const gchar *name = source->spec->units[i];
		WorkerUnit *unit;

		g_hash_table_add(wanted, (gpointer)name);
		unit = g_hash_table_lookup(source->units, name);

		if (NULL == unit)
		{
			unit = g_new0(WorkerUnit, 1);
			unit->name = g_strdup(name);
			unit->due = (SCHEDULE_CRON == kind) ? worker_next_minute(now) : now;
			g_hash_table_insert(source->units, unit->name, unit);
		}

		if (SCHEDULE_MANUAL == kind)
			unit->due = G_MAXINT64;
		else if (G_MAXINT64 == unit->due)
			unit->due = (SCHEDULE_CRON == kind) ? worker_next_minute(now) : now;
	}

	/* A unit the settings no longer name is forgotten. */
	g_hash_table_iter_init(&iter, source->units);

	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		if (!g_hash_table_contains(wanted, key))
			g_hash_table_iter_remove(&iter);
	}
}

/* --- Ingesting one batch ----------------------------------------------------------- */

static gchar *
worker_redact(
	WorkerSource	*source,
	const gchar	*text
){
	return venture_feed_source_redact(source->spec, text);
}

/* Whether track: known lets an instrument through. */
static gboolean
worker_known(
	WorkerSource	*source,
	const gchar	*instrument
){
	return !source->spec->track_known ||
	       ((NULL != source->spec->known) && (NULL != instrument) &&
	        g_hash_table_contains(source->spec->known, instrument));
}

typedef struct
{
	gboolean	 new_data;	/* a snapshot newer than the store had */
	gint64		 rows;
	GPtrArray	*venues;	/* gchar*: venues snapshotted */
} WorkerIngest;

/*
 * The operator's accounts, inside the unit's transaction. The batch's own
 * arrays go to the store as they are -- no copy of fifty thousand ledger
 * rows -- unless the source has credentials to redact, when the free text
 * (a name, a subject, a counterparty) is copied through the redactor
 * first. A balance not in the source's currency is dropped and counted:
 * the JSON-lines reader refuses one already, a native provider is told
 * here.
 *
 * track: known is not applied: it narrows what the market tracks, and an
 * operator's own bags are not a market to narrow.
 */
static gboolean
worker_ingest_accounts(
	WorkerSource		 *source,
	VentureFeedBatch	 *batch,
	gint64			  fetched_at,
	VentureFeedRun		 *run,
	WorkerIngest		 *out,
	const gchar		 *unit,
	GError			**error
){
	VentureFeedSource *spec = source->spec;
	g_autoptr(GPtrArray) owned = NULL;
	g_autoptr(GArray) accounts = NULL;
	g_autoptr(GArray) balances = NULL;
	g_autoptr(GArray) inbound = NULL;
	g_autoptr(GArray) txns = NULL;
	VentureSeriesAccountBatch view;
	VentureSeriesAccountResult result;
	gint64 dropped;
	guint i;

	if (!venture_feed_batch_get_accounts(batch, &view))
		return TRUE;

	view.currency = spec->currency;
	dropped = 0;

	for (i = 0; i < view.n_balances; i++)
	{
		if ((NULL == spec->currency) ||
		    (0 != g_ascii_strcasecmp(view.balances[i].currency, spec->currency)))
			dropped++;
	}

	if (dropped > 0)
	{
		g_autofree gchar *note = NULL;

		balances = g_array_new(FALSE, FALSE, sizeof(VentureSeriesBalance));

		for (i = 0; i < view.n_balances; i++)
		{
			if ((NULL != spec->currency) &&
			    (0 == g_ascii_strcasecmp(view.balances[i].currency, spec->currency)))
				g_array_append_val(balances, view.balances[i]);
		}

		view.balances = (const VentureSeriesBalance *)(gpointer)balances->data;
		view.n_balances = balances->len;
		run->refused += dropped;
		note = g_strdup_printf("%s: %" G_GINT64_FORMAT " balances not in the source's currency "
		                       "(%s) were dropped", unit, dropped,
		                       (NULL != spec->currency) ? spec->currency : "none is set");
		venture_feed_run_add_note(run, note);
	}

	if ((NULL != spec->secret_values) && (spec->secret_values->len > 0))
	{
		owned = g_ptr_array_new_with_free_func(g_free);
		accounts = g_array_sized_new(FALSE, FALSE, sizeof(VentureSeriesAccount), view.n_accounts);
		inbound = g_array_sized_new(FALSE, FALSE, sizeof(VentureSeriesInbound), view.n_inbound);
		txns = g_array_sized_new(FALSE, FALSE, sizeof(VentureSeriesTxn), view.n_txns);

		for (i = 0; i < view.n_accounts; i++)
		{
			VentureSeriesAccount row = view.accounts[i];
			gchar *name = worker_redact(source, row.name);
			gchar *attrs = worker_redact(source, row.attrs_json);

			g_ptr_array_add(owned, name);
			g_ptr_array_add(owned, attrs);
			row.name = name;
			row.attrs_json = attrs;
			g_array_append_val(accounts, row);
		}

		for (i = 0; i < view.n_inbound; i++)
		{
			VentureSeriesInbound row = view.inbound[i];
			gchar *sender = worker_redact(source, row.sender);
			gchar *subject = worker_redact(source, row.subject);

			g_ptr_array_add(owned, sender);
			g_ptr_array_add(owned, subject);
			row.sender = sender;
			row.subject = subject;
			g_array_append_val(inbound, row);
		}

		for (i = 0; i < view.n_txns; i++)
		{
			VentureSeriesTxn row = view.txns[i];
			gchar *counterparty = worker_redact(source, row.counterparty);
			gchar *label = worker_redact(source, row.source);

			g_ptr_array_add(owned, counterparty);
			g_ptr_array_add(owned, label);
			row.counterparty = counterparty;
			row.source = label;
			g_array_append_val(txns, row);
		}

		view.accounts = (const VentureSeriesAccount *)(gpointer)accounts->data;
		view.inbound = (const VentureSeriesInbound *)(gpointer)inbound->data;
		view.txns = (const VentureSeriesTxn *)(gpointer)txns->data;
	}

	memset(&result, 0, sizeof(result));

	if (!venture_series_store_apply_accounts(source->store, &view, fetched_at, &result, error))
		return FALSE;

	out->rows += result.rows_written;
	out->new_data = out->new_data || (result.rows_written > 0);
	run->new_instruments += result.instruments_new;
	run->refused += result.instruments_refused;

	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%s: accounts %" G_GINT64_FORMAT " (%" G_GINT64_FORMAT " new), "
		                       "holdings %" G_GINT64_FORMAT ", positions %" G_GINT64_FORMAT
		                       ", inbound %" G_GINT64_FORMAT ", balance changes %" G_GINT64_FORMAT
		                       ", removed %" G_GINT64_FORMAT "; ledger %" G_GINT64_FORMAT
		                       " new, %" G_GINT64_FORMAT " updated, %" G_GINT64_FORMAT
		                       " unchanged",
		                       unit, result.accounts, result.accounts_new, result.holdings,
		                       result.positions, result.inbound, result.balances, result.removed,
		                       result.txns_new, result.txns_updated, result.txns_unchanged);
		venture_feed_run_add_note(run, note);
	}

	if (result.stale > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%s: %" G_GINT64_FORMAT " account snapshots were older than the "
		                       "state they would replace and changed nothing", unit,
		                       result.stale);
		venture_feed_run_add_note(run, note);
	}

	return TRUE;
}

/*
 * Writes a batch into the store in one transaction. Every string a
 * provider handed over goes through the source's redaction first: a far
 * end that echoes a credential into an item's name must not put it into a
 * file that is kept for ever.
 */
static gboolean
worker_ingest(
	WorkerSource		 *source,
	VentureFeedRequest	 *request,
	VentureFeedBatch	 *batch,
	VentureFeedRun		 *run,
	WorkerIngest		 *out,
	GError			**error
){
	VentureFeedSource *spec;
	VentureSeriesStore *store;
	g_autoptr(GHashTable) venues_seen = NULL;
	gint64 fetched_at;
	gint64 skipped;
	guint i;

	spec = source->spec;
	store = source->store;
	fetched_at = venture_feed_request_get_fetched_at(request);
	venues_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	skipped = 0;

	if (!venture_series_store_begin(store, error))
		return FALSE;

	for (i = 0; i < batch->venues->len; i++)
	{
		FeedBatchVenue *venue = g_ptr_array_index(batch->venues, i);
		g_autofree gchar *name = worker_redact(source, venue->name);
		g_autofree gchar *attrs = worker_redact(source, venue->attrs_json);
		VentureSeriesVenue row;

		row.key = venue->key;
		row.namespace_ = spec->venue_namespace;
		row.name = name;
		row.kind = venue->kind;
		row.group_key = venue->group_key;
		row.currency = venue->currency;
		row.attrs_json = attrs;

		if (!venture_series_store_upsert_venue(store, &row, fetched_at, error))
			goto fail;

		g_hash_table_add(venues_seen, g_strdup(venue->key));
	}

	for (i = 0; i < batch->instruments->len; i++)
	{
		FeedBatchInstrument *instrument = g_ptr_array_index(batch->instruments, i);
		g_autofree gchar *name = NULL;
		g_autofree gchar *attrs = NULL;
		VentureSeriesInstrument row;
		gboolean created = FALSE;
		gboolean refused = FALSE;

		if (!worker_known(source, instrument->key))
		{
			skipped++;
			continue;
		}

		name = worker_redact(source, instrument->name);
		attrs = worker_redact(source, instrument->attrs_json);
		row.key = instrument->key;
		row.namespace_ = spec->instrument_namespace;
		row.name = name;
		row.kind = instrument->kind;
		row.category = instrument->category;
		row.parent_key = instrument->parent_key;
		row.attrs_json = attrs;

		if (!venture_series_store_upsert_instrument(store, &row, fetched_at, &created,
		                                            &refused, error))
			goto fail;

		if (created)
			run->new_instruments++;

		if (refused)
			run->refused++;
	}

	for (i = 0; i < batch->snapshots->len; i++)
	{
		FeedBatchSnapshot *snapshot = g_ptr_array_index(batch->snapshots, i);
		g_autoptr(VentureSeriesSnapshot) open = NULL;
		VentureSeriesCommitResult result;
		const gchar *currency;
		guint j;

		/* A snapshot of a venue the batch did not describe still needs
		 * a row to hang from. */
		if (!g_hash_table_contains(venues_seen, snapshot->venue_key))
		{
			VentureSeriesVenue row;

			memset(&row, 0, sizeof(row));
			row.key = snapshot->venue_key;
			row.namespace_ = spec->venue_namespace;

			if (!venture_series_store_upsert_venue(store, &row, fetched_at, error))
				goto fail;

			g_hash_table_add(venues_seen, g_strdup(snapshot->venue_key));
		}

		currency = (NULL != snapshot->currency) ? snapshot->currency : spec->currency;
		open = venture_series_store_begin_snapshot(store, snapshot->venue_key, currency,
		                                           snapshot->taken_at, fetched_at,
		                                           snapshot->complete, error);

		if (NULL == open)
			goto fail;

		for (j = 0; j < snapshot->listings->len; j++)
		{
			FeedBatchListing *listing = &g_array_index(snapshot->listings, FeedBatchListing, j);
			VentureSeriesListing row;

			if (!worker_known(source, listing->instrument_key))
			{
				skipped++;
				continue;
			}

			row.instrument_key = listing->instrument_key;
			row.listing_id = listing->listing_id;
			row.unit_price = listing->unit_price;
			row.quantity = listing->quantity;
			row.side = listing->side;
			row.expires_in_min = listing->expires_in_min;

			/* Refused alone: counted, and the snapshot goes on. */
			if (!venture_series_snapshot_add_listing(open, &row, NULL))
				run->refused++;
		}

		for (j = 0; j < snapshot->stats->len; j++)
		{
			FeedBatchStats *stats = &g_array_index(snapshot->stats, FeedBatchStats, j);
			VentureSeriesStats row;

			if (!worker_known(source, stats->instrument_key))
			{
				skipped++;
				continue;
			}

			venture_series_stats_init(&row);
			row.historical = stats->historical;
			row.sale_rate = stats->sale_rate;
			row.sold_per_day = stats->sold_per_day;
			row.instrument_key = stats->instrument_key;
			row.min_price = stats->min_price;
			row.market_value = stats->market_value;
			row.mean = stats->mean;
			row.median = stats->median;
			row.sale_avg = stats->sale_avg;
			row.quantity = stats->quantity;
			row.listings = stats->listings;
			row.sold = stats->sold;

			if (!venture_series_snapshot_add_stats(open, &row, NULL))
				run->refused++;
		}

		memset(&result, 0, sizeof(result));

		if (!venture_series_store_commit_snapshot(store, g_steal_pointer(&open), &result, error))
			goto fail;

		out->rows += result.rows_written;
		run->new_instruments += result.instruments_new;
		run->refused += result.listings_refused + result.instruments_refused;

		if (!result.duplicate && !result.late)
		{
			out->new_data = TRUE;
			g_ptr_array_add(out->venues, g_strdup(snapshot->venue_key));
			venture_feed_run_add_venue(run, snapshot->venue_key);
		}
	}

	if (batch->quotes->len > 0)
	{
		g_autoptr(GArray) rows = NULL;
		VentureSeriesCommitResult result;

		rows = g_array_new(FALSE, TRUE, sizeof(VentureSeriesQuote));

		for (i = 0; i < batch->quotes->len; i++)
		{
			FeedBatchQuote *quote = g_ptr_array_index(batch->quotes, i);
			VentureSeriesQuote row;

			if (!worker_known(source, quote->instrument_key))
			{
				skipped++;
				continue;
			}

			row.venue_key = quote->venue_key;
			row.instrument_key = quote->instrument_key;
			row.side = quote->side;
			row.value = quote->value;
			row.currency = quote->currency;
			row.liquidity = quote->liquidity;
			row.taken_at = quote->taken_at;
			g_array_append_val(rows, row);
		}

		memset(&result, 0, sizeof(result));

		if ((rows->len > 0) &&
		    !venture_series_store_add_quotes(store, (const VentureSeriesQuote *)(gpointer)rows->data,
		                                     rows->len, &result, error))
			goto fail;

		out->rows += result.rows_written;
		run->new_instruments += result.instruments_new;
		run->refused += result.instruments_refused;

		if (result.rows_written > 0)
			out->new_data = TRUE;
	}

	if (batch->entries->len > 0)
	{
		g_autoptr(GPtrArray) owned = NULL;
		g_autoptr(GArray) rows = NULL;
		gint64 added = 0;

		owned = g_ptr_array_new_with_free_func(g_free);
		rows = g_array_new(FALSE, TRUE, sizeof(VentureSeriesEntry));

		for (i = 0; i < batch->entries->len; i++)
		{
			FeedBatchEntry *entry = g_ptr_array_index(batch->entries, i);
			VentureSeriesEntry row;
			gchar *title = worker_redact(source, entry->title);
			gchar *summary = worker_redact(source, entry->summary);
			gchar *url = worker_redact(source, entry->url);

			g_ptr_array_add(owned, title);
			g_ptr_array_add(owned, summary);
			g_ptr_array_add(owned, url);

			row.key = entry->key;
			row.title = title;
			row.url = url;
			row.summary = summary;
			row.published_at = entry->published_at;
			row.venue_key = entry->venue_key;
			row.instrument_key = entry->instrument_key;
			row.attrs_json = NULL;
			g_array_append_val(rows, row);
		}

		if (!venture_series_store_add_entries(store, (const VentureSeriesEntry *)(gpointer)rows->data,
		                                      rows->len, fetched_at, &added, error))
			goto fail;

		out->rows += added;

		if (added > 0)
			out->new_data = TRUE;
	}

	if (!worker_ingest_accounts(source, batch, fetched_at, run, out,
	                            venture_feed_request_get_unit(request), error))
		goto fail;

	/* Where the unit resumes, and what it last saw, beside the data they
	 * describe and in the same transaction. */
	if (NULL != batch->cursor)
	{
		g_autofree gchar *key = g_strconcat("cursor:", venture_feed_request_get_unit(request), NULL);

		if (!venture_series_store_set_meta(store, key, batch->cursor, error))
			goto fail;
	}

	if (VENTURE_SERIES_NONE != venture_feed_request_get_last_modified(request))
	{
		g_autofree gchar *key = g_strconcat("ims:", venture_feed_request_get_unit(request), NULL);
		g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT,
		                                          venture_feed_request_get_last_modified(request));

		if (!venture_series_store_set_meta(store, key, value, error))
			goto fail;
	}

	if (!venture_series_store_commit(store, error))
		goto fail;

	if (skipped > 0)
	{
		g_autofree gchar *note = g_strdup_printf(
			"%s: %" G_GINT64_FORMAT " items skipped: track is known and they are not in instruments",
			venture_feed_request_get_unit(request), skipped);

		venture_feed_run_add_note(run, note);
	}

	return TRUE;

fail:
	venture_series_store_rollback(store);
	return FALSE;
}

/* The batch's record upserts, kept for the main thread, up to the run's
 * bound; the rest are counted. */
static void
worker_keep_records(
	WorkerSource		*source,
	VentureFeedBatch	*batch,
	VentureFeedRun		*run
){
	guint i;

	for (i = 0; i < batch->records->len; i++)
	{
		FeedBatchRecord *record = g_ptr_array_index(batch->records, i);
		FeedBatchRecord *copy;

		/* Only the types the source accepts, as frozen; the main thread
		 * judges the rest -- the module, the fields -- when it writes. */
		if ((NULL == source->spec->record_types) ||
		    !g_strv_contains((const gchar *const *)source->spec->record_types, record->record_type))
		{
			run->records_dropped++;
			continue;
		}

		if (run->records->len >= run->max_records)
		{
			run->records_dropped++;
			continue;
		}

		copy = g_new0(FeedBatchRecord, 1);
		copy->record_type = g_strdup(record->record_type);
		copy->fields_json = worker_redact(source, record->fields_json);
		copy->match = g_strdupv(record->match);
		g_ptr_array_add(run->records, copy);
	}
}

/* --- One unit ---------------------------------------------------------------------- */

static void
worker_run_note(
	WorkerSource	*source,
	VentureFeedRun	*run,
	const gchar	*unit,
	const gchar	*text
){
	g_autofree gchar *redacted = worker_redact(source, text);
	g_autofree gchar *note = g_strdup_printf("%s: %s", unit, redacted);

	venture_feed_run_add_note(run, note);
}

static void
worker_run_error(
	WorkerSource	*source,
	VentureFeedRun	*run,
	const gchar	*unit,
	const gchar	*message
){
	g_autofree gchar *redacted = worker_redact(source, message);

	if (NULL == run->error)
		run->error = g_strdup_printf("%s: %s", unit, redacted);
	else
		worker_run_note(source, run, unit, message);
}

/* The earliest next update the store expects of the venues a unit
 * produced, or NONE. */
static gint64
worker_unit_expected(
	WorkerSource	*source,
	WorkerUnit	*unit
){
	gint64 expected;
	guint i;

	expected = VENTURE_SERIES_NONE;

	for (i = 0; (NULL != unit->venues) && (NULL != unit->venues[i]); i++)
	{
		VentureSeriesVenueState state;

		if (!venture_series_store_get_venue_state(source->store, unit->venues[i], &state, NULL) ||
		    !state.found || (VENTURE_SERIES_NONE == state.next_expected))
			continue;

		if ((VENTURE_SERIES_NONE == expected) || (state.next_expected < expected))
			expected = state.next_expected;
	}

	return expected;
}

typedef enum
{
	UNIT_NEW = 0,
	UNIT_SAME,
	UNIT_FAILED,
	UNIT_DEFERRED
} WorkerOutcome;

static void
worker_unit_reschedule(
	WorkerSource	*source,
	WorkerUnit	*unit,
	WorkerOutcome	 outcome,
	gint64		 retry_after,
	gint64		 deferred_until,
	gint64		 started_at
){
	WorkerSchedule kind;
	gint64 now;

	now = worker_now();
	kind = worker_schedule_kind(source->spec);

	if (UNIT_DEFERRED == outcome)
	{
		/* A spent budget is not the far end's fault: wait for the
		 * window, keep the back-off where it was. */
		unit->due = MAX(deferred_until, now + 1);
		return;
	}

	switch (kind)
	{
	case SCHEDULE_MANUAL:
		unit->due = G_MAXINT64;
		break;

	case SCHEDULE_HOURLY:
		unit->due = MAX(started_at + 3600, now + retry_after);
		break;

	case SCHEDULE_CRON:
		unit->due = MAX(worker_next_minute(now), now + retry_after);
		break;

	case SCHEDULE_AUTO:
	default:
		unit->due = venture_feeds_schedule_next_auto(
			UNIT_NEW == outcome,
			(UNIT_NEW == outcome) ? worker_unit_expected(source, unit) : VENTURE_SERIES_NONE,
			retry_after, &unit->backoff, now);
		break;
	}
}

/*
 * The far end's quota, as a provider read it from the answer: kept on the
 * run (the newest wins) and said once per unit in its notes, so the run
 * page shows how close the account is to its limit.
 */
static void
worker_run_remote_quota(
	WorkerSource		*source,
	VentureFeedRun		*run,
	const gchar		*unit_name,
	VentureFeedBatch	*batch
){
	g_autofree gchar *note = NULL;
	gint64 used;
	gint64 remaining;

	if (!venture_feed_batch_get_remote_quota(batch, &used, &remaining))
		return;

	run->remote_used = used;
	run->remote_limit = (remaining > G_MAXINT64 - used) ? G_MAXINT64 : used + remaining;
	note = g_strdup_printf("the far end counts %" G_GINT64_FORMAT " requests used and %"
	                       G_GINT64_FORMAT " left", used, remaining);
	worker_run_note(source, run, unit_name, note);
}

static void
worker_unit_fetched(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 user_data
){
	WorkerPass *pass = user_data;
	WorkerSource *source = pass->source;
	VentureFeedRun *run = pass->run;
	g_autoptr(VentureFeedBatch) batch = NULL;
	g_autoptr(VentureFeedRequest) request = NULL;
	g_autoptr(GError) error = NULL;
	WorkerOutcome outcome;
	WorkerUnit *unit;
	const gchar *unit_name;
	gint64 retry_after;
	gint status;

	request = g_steal_pointer(&pass->request);
	batch = venture_data_source_provider_fetch_finish(VENTURE_DATA_SOURCE_PROVIDER(object),
	                                                  result, &error);
	unit_name = venture_feed_request_get_unit(request);
	unit = g_hash_table_lookup(source->units, unit_name);
	retry_after = venture_feed_request_get_retry_after(request);

	venture_feed_request_get_quota(request, &source->quota);

	run->units++;
	run->requests += venture_feed_request_get_requests(request);
	run->bytes += venture_feed_request_get_bytes(request);
	status = venture_feed_request_get_http_status(request);

	/* The last status worth reporting: a failure over a success. */
	if ((0 != status) && ((200 == run->http_status) || (0 == run->http_status) ||
	                      (status >= 300)))
		run->http_status = status;

	if (NULL == batch)
	{
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		{
			/* The worker is stopping; nothing more happens. */
			run->failed++;
			outcome = UNIT_FAILED;
		}
		else if (g_error_matches(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_QUOTA))
		{
			run->deferred++;
			worker_run_note(source, run, unit_name, error->message);
			outcome = UNIT_DEFERRED;
		}
		else
		{
			run->failed++;
			pass->failed = TRUE;
			worker_run_error(source, run, unit_name, error->message);
			outcome = UNIT_FAILED;
		}
	}
	else if (venture_feed_batch_get_not_modified(batch))
	{
		worker_run_remote_quota(source, run, unit_name, batch);
		run->not_modified++;
		run->succeeded++;
		outcome = UNIT_SAME;
	}
	else
	{
		g_autoptr(GError) ingest_error = NULL;
		g_autoptr(GPtrArray) venues = g_ptr_array_new_with_free_func(g_free);
		WorkerIngest ingest = { FALSE, 0, NULL };
		const gchar *partial;
		gint64 partial_retry = 0;
		guint i;

		ingest.venues = venues;
		run->refused += batch->refused;
		worker_run_remote_quota(source, run, unit_name, batch);

		for (i = 0; i < batch->notes->len; i++)
			worker_run_note(source, run, unit_name, g_ptr_array_index(batch->notes, i));

		if (!worker_store_open(source, &ingest_error) ||
		    !worker_ingest(source, request, batch, run, &ingest, &ingest_error))
		{
			run->failed++;
			pass->failed = TRUE;
			worker_run_error(source, run, unit_name, ingest_error->message);
			outcome = UNIT_FAILED;
		}
		else
		{
			run->rows += ingest.rows;
			pass->wrote = pass->wrote || (ingest.rows > 0);
			worker_keep_records(source, batch, run);

			/* What the unit produced, for its next check. */
			if ((NULL != unit) && (venues->len > 0))
			{
				g_ptr_array_add(venues, NULL);
				g_strfreev(unit->venues);
				unit->venues = (gchar **)g_ptr_array_steal(venues, NULL);
				venues = g_ptr_array_new_with_free_func(g_free);
			}

			partial = venture_feed_batch_get_error(batch, &partial_retry);

			if (NULL != partial)
			{
				/* Stored, and also failed: both are true. */
				run->succeeded++;
				run->failed++;
				pass->failed = TRUE;
				worker_run_error(source, run, unit_name, partial);
				retry_after = MAX(retry_after, partial_retry);
				outcome = ingest.new_data ? UNIT_NEW : UNIT_FAILED;
			}
			else
			{
				run->succeeded++;
				outcome = ingest.new_data ? UNIT_NEW : UNIT_SAME;
			}

			/* Hooks see the store right after the commit, on this
			 * thread, with what they froze on the main one. */
			for (i = 0; (NULL != source->spec->hooks) && (i < source->spec->hooks->len); i++)
			{
				FeedFrozenHook *hook = g_ptr_array_index(source->spec->hooks, i);

				if (NULL != hook->hook->commit)
					hook->hook->commit(source->store, source->spec, hook->frozen, run,
					                   hook->hook->user_data);
			}
		}
	}

	if ((NULL != unit) && !source->removed)
		worker_unit_reschedule(source, unit, outcome, retry_after,
		                       venture_feed_request_get_deferred_until(request),
		                       pass->started_at);

	pass->index++;
	worker_pass_next(pass);
}

static void
worker_pass_free(WorkerPass *pass)
{
	g_clear_pointer(&pass->push, worker_push_free);
	g_clear_object(&pass->request);
	g_clear_pointer(&pass->units, g_ptr_array_unref);
	g_clear_pointer(&pass->run, venture_feed_run_unref);
	g_free(pass);
}

/*
 * After the last unit: region figures and retention on their own clocks,
 * the quota kept, then the run -- handed back now if it was asked for, or
 * when the scheduled window closes.
 */
static void
worker_pass_finish(WorkerPass *pass)
{
	WorkerSource *source = pass->source;
	VentureSeriesWorker *self = source->worker;
	gint64 now;

	now = worker_now();

	if (!g_atomic_int_get(&self->stopping) && (NULL != source->store))
	{
		g_autoptr(GError) error = NULL;

		/* A sync asked for by hand recomputes at once; a push -- which
		 * may come every few minutes, mostly the operator's own rows --
		 * waits for the half-hour like a scheduled pass. */
		if (pass->wrote &&
		    ((pass->manual && (NULL == pass->push)) ||
		     (now - source->last_region >= VENTURE_FEEDS_REGION_EVERY)))
		{
			VentureSeriesRegionResult region;

			if (venture_series_store_recompute_region(source->store, NULL, now,
			                                          (NULL != source->spec->min_currency)
			                                          ? source->spec->min_value : VENTURE_SERIES_NONE,
			                                          source->spec->min_currency, &region, &error))
				source->last_region = now;
			else
			{
				worker_run_note(source, pass->run, "region", error->message);
				g_clear_error(&error);
			}
		}

		if (now - source->last_purge >= 86400)
		{
			VentureSeriesPurgeResult purged;

			if (venture_series_store_purge(source->store, now, source->spec->hourly_days,
			                               source->spec->daily_days, &purged, &error))
				source->last_purge = now;
			else
			{
				worker_run_note(source, pass->run, "retention", error->message);
				g_clear_error(&error);
			}
		}

		worker_quota_save(source);
	}

	/* The far end's own count, when a provider read one, is the one it
	 * will enforce; otherwise the source's own hourly budget. */
	if (pass->run->remote_used >= 0)
	{
		pass->run->quota_used = pass->run->remote_used;
		pass->run->quota_limit = pass->run->remote_limit;
	}
	else
		pass->run->quota_used = source->quota.spent;

	if (pass->manual)
	{
		venture_feed_run_finish(pass->run, now);
		worker_hand_back(self, pass->run);
	}
	else
	{
		VentureFeedRun *open = source->open_run;

		open->finished_at = now;

		/* A failure is told at once; quiet passes wait for the window. */
		if (pass->failed || (0 == source->spec->run_window) ||
		    (now - open->started_at >= (gint64)source->spec->run_window))
		{
			venture_feed_run_finish(open, now);
			worker_hand_back(self, open);
			g_clear_pointer(&source->open_run, venture_feed_run_unref);
		}
	}

	source->pass = NULL;
	worker_pass_free(pass);
	worker_live_add(self, -1);

	if (source->removed)
	{
		worker_flush_open_run(source);
		worker_source_set_manual(source, FALSE);
		g_hash_table_remove(self->sources, &source->spec->id);
	}

	worker_publish_status(self);
	worker_reschedule(self);
}

static void
worker_pass_next(WorkerPass *pass)
{
	WorkerSource *source = pass->source;
	VentureSeriesWorker *self = source->worker;
	g_autofree gchar *cursor = NULL;
	g_autofree gchar *ims = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) settings = NULL;
	g_autoptr(JsonObject) secrets = NULL;
	const gchar *unit_name;
	gint64 if_modified_since;

	if (g_atomic_int_get(&self->stopping) || (pass->index >= pass->units->len))
	{
		worker_pass_finish(pass);
		return;
	}

	unit_name = g_ptr_array_index(pass->units, pass->index);

	/* The store is opened before the fetch so the unit's cursor and
	 * Last-Modified can go with it. */
	if (!worker_store_open(source, &error))
	{
		pass->run->units++;
		pass->run->failed++;
		pass->failed = TRUE;
		worker_run_error(source, pass->run, unit_name, error->message);
		pass->index++;
		worker_pass_next(pass);
		return;
	}

	worker_quota_load(source);

	{
		g_autofree gchar *cursor_key = g_strconcat("cursor:", unit_name, NULL);
		g_autofree gchar *ims_key = g_strconcat("ims:", unit_name, NULL);

		cursor = venture_series_store_get_meta(source->store, cursor_key, NULL);
		ims = venture_series_store_get_meta(source->store, ims_key, NULL);
	}

	if_modified_since = (NULL != ims) ? g_ascii_strtoll(ims, NULL, 10) : VENTURE_SERIES_NONE;

	/* A manual sync asks for the data whatever the far end thinks has
	 * changed: no If-Modified-Since. */
	if (pass->manual)
		if_modified_since = VENTURE_SERIES_NONE;

	/*
	 * The request gets JSON of its own, parsed now, and a copy of the
	 * quota: a provider may read them on a GTask thread while this one
	 * replaces the source, and JSON-GLib's reference counts are not
	 * atomic. The quota is copied back when the fetch ends.
	 */
	settings = worker_parse_object(source->spec->settings_json);
	secrets = worker_parse_object(source->spec->secrets_json);
	pass->request = venture_feed_request_new_internal(source->spec, unit_name, settings,
	                                                  secrets, self->session,
	                                                  &source->quota, cursor, if_modified_since,
	                                                  worker_now(), self->cancellable);

	/* The request holds its own references now. Ours must go before the
	 * fetch starts, not when this function returns: a provider may take
	 * and drop references on the same objects from its GTask thread
	 * already, and two threads moving one non-atomic count lose updates
	 * -- a leak, or a free while the other still reads. */
	g_clear_pointer(&settings, json_object_unref);
	g_clear_pointer(&secrets, json_object_unref);

	if (NULL != pass->push)
		venture_feed_request_set_body(pass->request, pass->push->body);

	venture_data_source_provider_fetch_async(source->spec->provider, pass->request,
	                                         self->cancellable, worker_unit_fetched, pass);
}

/*
 * Starts a pass over a source: every unit when asked, else the units that
 * are due. A source with a pass in flight is left alone; its due units
 * wait for the next look.
 */
static void
worker_pass_start(
	WorkerSource	*source,
	gboolean	 manual
){
	VentureSeriesWorker *self = source->worker;
	GHashTableIter iter;
	gpointer value;
	WorkerPass *pass;
	gint64 now;

	if ((NULL != source->pass) || source->removed)
		return;

	now = worker_now();
	pass = g_new0(WorkerPass, 1);
	pass->source = source;
	pass->units = g_ptr_array_new_with_free_func(g_free);
	pass->manual = manual;
	pass->started_at = now;

	g_hash_table_iter_init(&iter, source->units);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		WorkerUnit *unit = value;

		if (manual || (unit->due <= now))
			g_ptr_array_add(pass->units, g_strdup(unit->name));
	}

	if (0 == pass->units->len)
	{
		/* Asked for and nothing to do: answered all the same. */
		if (manual)
			worker_source_set_manual(source, FALSE);

		worker_pass_free(pass);
		return;
	}

	/* The same order every time: the units' names. */
	g_ptr_array_sort_values(pass->units, (GCompareFunc)g_strcmp0);

	/* Counted before the sync it answers stops being: never a moment at
	 * zero in between. */
	worker_live_add(self, 1);

	if (manual)
	{
		pass->run = venture_feed_run_new_internal(source->spec, source->manual_trigger, now);
		worker_source_set_manual(source, FALSE);
	}
	else
	{
		if (NULL == source->open_run)
			source->open_run = venture_feed_run_new_internal(source->spec,
			                                                 VENTURE_DATA_SOURCE_RUN_TRIGGER_SCHEDULE,
			                                                 now);
		pass->run = venture_feed_run_ref(source->open_run);
	}

	source->pass = pass;
	worker_publish_status(self);
	worker_pass_next(pass);
}

/*
 * Starts the run of the oldest body pushed to a source: one unit, "push",
 * read by the source's provider from the body rather than fetched, and
 * handed back as soon as it ends -- the one who pushed may be waiting.
 */
static void
worker_pass_start_push(WorkerSource *source)
{
	VentureSeriesWorker *self = source->worker;
	WorkerPass *pass;
	gint64 now;

	if ((NULL != source->pass) || source->removed || g_queue_is_empty(&source->pushes))
		return;

	now = worker_now();
	pass = g_new0(WorkerPass, 1);
	pass->source = source;
	pass->units = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(pass->units, g_strdup("push"));
	pass->manual = TRUE;
	pass->started_at = now;
	pass->push = g_queue_pop_head(&source->pushes);
	pass->run = venture_feed_run_new_internal(source->spec, VENTURE_DATA_SOURCE_RUN_TRIGGER_PUSH,
	                                          now);
	pass->run->push_id = g_strdup(pass->push->id);

	/* The pass is counted before the push it answers stops being. */
	worker_live_add(self, 1);
	worker_live_add(self, -1);

	source->pass = pass;
	worker_publish_status(self);
	worker_pass_next(pass);
}

/* --- The timer ------------------------------------------------------------------------ */

static gboolean
worker_tick(gpointer data)
{
	VentureSeriesWorker *self = data;
	GHashTableIter iter;
	gpointer value;
	gint64 now;

	g_clear_pointer(&self->timer, g_source_unref);

	if (g_atomic_int_get(&self->stopping))
		return G_SOURCE_REMOVE;

	now = worker_now();
	g_hash_table_iter_init(&iter, self->sources);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		WorkerSource *source = value;

		if (NULL != source->pass)
			continue;

		/* A body somebody pushed is answered before anything else:
		 * whoever sent it may be waiting on the run. */
		if (!g_queue_is_empty(&source->pushes))
		{
			worker_pass_start_push(source);
			continue;
		}

		if (source->manual_pending)
		{
			worker_pass_start(source, TRUE);
			continue;
		}

		/*
		 * A cron schedule is judged at each minute by the same rule a
		 * report pack's is: due units run, the rest wait for the next
		 * minute.
		 */
		if (SCHEDULE_CRON == worker_schedule_kind(source->spec))
		{
			g_autoptr(GDateTime) as_of = g_date_time_new_from_unix_utc(now);
			g_autoptr(GDateTime) last = (source->last_cron > 0)
				? g_date_time_new_from_unix_utc(source->last_cron) : NULL;
			GHashTableIter units;
			gpointer unit_value;
			gboolean any_due = FALSE;

			g_hash_table_iter_init(&units, source->units);

			while (g_hash_table_iter_next(&units, NULL, &unit_value))
				any_due = any_due || (((WorkerUnit *)unit_value)->due <= now);

			if (!any_due)
				continue;

			if (1 == venture_report_pack_schedule_due(source->spec->schedule, last, as_of, NULL))
			{
				source->last_cron = now;
				worker_pass_start(source, FALSE);
				continue;
			}

			g_hash_table_iter_init(&units, source->units);

			while (g_hash_table_iter_next(&units, NULL, &unit_value))
				((WorkerUnit *)unit_value)->due = worker_next_minute(now);

			continue;
		}

		worker_pass_start(source, FALSE);
	}

	worker_reschedule(self);

	return G_SOURCE_REMOVE;
}

/* Sleeps until the earliest due unit, at most a minute. */
static void
worker_reschedule(VentureSeriesWorker *self)
{
	GHashTableIter iter;
	gpointer value;
	gint64 earliest;
	gint64 now;
	gint64 wait_ms;

	if (g_atomic_int_get(&self->stopping))
		return;

	if (NULL != self->timer)
	{
		g_source_destroy(self->timer);
		g_clear_pointer(&self->timer, g_source_unref);
	}

	earliest = G_MAXINT64;
	g_hash_table_iter_init(&iter, self->sources);

	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		WorkerSource *source = value;
		GHashTableIter units;
		gpointer unit;

		if (NULL != source->pass)
			continue;

		if (source->manual_pending || !g_queue_is_empty(&source->pushes))
		{
			earliest = 0;
			break;
		}

		g_hash_table_iter_init(&units, source->units);

		while (g_hash_table_iter_next(&units, NULL, &unit))
			earliest = MIN(earliest, ((WorkerUnit *)unit)->due);
	}

	if (G_MAXINT64 == earliest)
		return;

	now = worker_now();
	wait_ms = (earliest <= now) ? 0 : MIN((earliest - now) * 1000, WORKER_MAX_SLEEP_MS);

	self->timer = (0 == wait_ms) ? g_idle_source_new() : g_timeout_source_new((guint)wait_ms);
	g_source_set_callback(self->timer, worker_tick, self, NULL);
	g_source_attach(self->timer, self->context);
}

/* --- Status ------------------------------------------------------------------------ */

static void
worker_publish_status(VentureSeriesWorker *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GList) ids = NULL;
	GList *l;

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "sources");
	json_builder_begin_array(builder);

	ids = g_hash_table_get_values(self->sources);

	for (l = ids; NULL != l; l = l->next)
	{
		WorkerSource *source = l->data;
		g_autoptr(GList) units = NULL;
		GList *u;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, source->spec->id);
		json_builder_set_member_name(builder, "uuid");
		json_builder_add_string_value(builder, source->spec->uuid);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, source->spec->name);
		json_builder_set_member_name(builder, "provider");
		json_builder_add_string_value(builder, source->spec->provider_name);
		json_builder_set_member_name(builder, "schedule");
		json_builder_add_string_value(builder, source->spec->schedule);
		json_builder_set_member_name(builder, "enabled");
		json_builder_add_boolean_value(builder, source->spec->enabled);
		json_builder_set_member_name(builder, "in_flight");
		json_builder_add_boolean_value(builder, NULL != source->pass);
		json_builder_set_member_name(builder, "sync_queued");
		json_builder_add_boolean_value(builder, source->manual_pending);
		json_builder_set_member_name(builder, "quota_used");
		json_builder_add_int_value(builder, source->quota.spent);
		json_builder_set_member_name(builder, "quota_limit");
		json_builder_add_int_value(builder, source->spec->requests_per_hour);
		json_builder_set_member_name(builder, "units");
		json_builder_begin_array(builder);

		units = g_hash_table_get_values(source->units);

		for (u = units; NULL != u; u = u->next)
		{
			WorkerUnit *unit = u->data;

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "name");
			json_builder_add_string_value(builder, unit->name);
			json_builder_set_member_name(builder, "next_check");

			if (G_MAXINT64 == unit->due)
				json_builder_add_null_value(builder);
			else
				json_builder_add_int_value(builder, unit->due);

			json_builder_set_member_name(builder, "backoff");
			json_builder_add_int_value(builder, unit->backoff);
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	{
		g_autoptr(JsonNode) root = json_builder_get_root(builder);
		gchar *text = json_to_string(root, FALSE);

		g_mutex_lock(&self->lock);
		g_free(self->status);
		self->status = text;
		g_mutex_unlock(&self->lock);
	}
}

/* --- Commands from the main thread --------------------------------------------- */

typedef enum
{
	COMMAND_SET = 0,
	COMMAND_REMOVE,
	COMMAND_SYNC,
	COMMAND_PURGE,
	COMMAND_BACKUP,
	COMMAND_PUSH
} WorkerCommandKind;

typedef struct
{
	VentureSeriesWorker		*worker;
	WorkerCommandKind		 kind;
	VentureFeedSource		*spec;
	gint64				 id;
	VentureDataSourceRunTrigger	 trigger;
	gchar				*store_dir;
	gchar				*destination;
	gchar				*push_id;
	GBytes				*body;
} WorkerCommand;

static void
worker_command_free(gpointer data)
{
	WorkerCommand *command = data;

	worker_live_add(command->worker, -1);
	g_clear_pointer(&command->spec, venture_feed_source_unref);
	g_free(command->store_dir);
	g_free(command->destination);
	g_free(command->push_id);
	g_clear_pointer(&command->body, g_bytes_unref);
	g_free(command);
}

/* Parses the frozen JSON on this thread, for this thread. */
static JsonObject *
worker_parse_object(const gchar *json)
{
	g_autoptr(JsonNode) node = NULL;

	node = (NULL != json) ? json_from_string(json, NULL) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return json_object_new();

	return json_object_ref(json_node_get_object(node));
}

static WorkerSource *
worker_install(
	VentureSeriesWorker	*self,
	VentureFeedSource	*spec
){
	WorkerSource *source;

	source = g_hash_table_lookup(self->sources, &spec->id);

	if (NULL == source)
	{
		source = g_new0(WorkerSource, 1);
		source->worker = self;
		source->units = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, worker_unit_free);
		source->spec = venture_feed_source_ref(spec);
		g_hash_table_insert(self->sources, &source->spec->id, source);
	}
	else
	{
		/* A store moved -- another state directory -- is closed here and
		 * opened where the new spec says. */
		if ((NULL != source->store) &&
		    (0 != g_strcmp0(source->spec->store_dir, spec->store_dir)))
			g_clear_object(&source->store);

		/* The key in the table is the old spec's id field; the ids are
		 * equal, but the pointer must stay valid, so re-insert. */
		g_hash_table_steal(self->sources, &source->spec->id);
		venture_feed_source_unref(source->spec);
		source->spec = venture_feed_source_ref(spec);
		g_hash_table_insert(self->sources, &source->spec->id, source);
	}

	source->removed = FALSE;

	if (NULL != source->store)
		venture_series_store_set_max_bytes(source->store, spec->max_store_bytes);

	worker_source_sync_units(source);

	return source;
}

static gboolean
worker_command(gpointer data)
{
	WorkerCommand *command = data;
	VentureSeriesWorker *self = command->worker;
	WorkerSource *source;

	if (g_atomic_int_get(&self->stopping))
		return G_SOURCE_REMOVE;

	switch (command->kind)
	{
	case COMMAND_SET:
		worker_install(self, command->spec);
		break;

	case COMMAND_SYNC:
		source = worker_install(self, command->spec);

		/* A sync waiting already: this one joins it. */
		if (!source->manual_pending)
		{
			worker_source_set_manual(source, TRUE);
			source->manual_trigger = command->trigger;
		}
		break;

	case COMMAND_REMOVE:
		source = g_hash_table_lookup(self->sources, &command->id);

		if (NULL != source)
		{
			/* A pass in flight finishes; the source goes after it. */
			if (NULL != source->pass)
			{
				source->removed = TRUE;
			}
			else
			{
				worker_flush_open_run(source);
				worker_source_set_manual(source, FALSE);
				g_hash_table_remove(self->sources, &command->id);
			}
		}
		break;

	case COMMAND_PURGE:
	{
		g_autoptr(GError) error = NULL;

		source = g_hash_table_lookup(self->sources, &command->id);

		if (NULL != source)
		{
			g_clear_object(&source->store);
			source->quota_loaded = FALSE;
		}

		if (!venture_series_worker_delete_store(command->store_dir, &error))
			g_message("feeds: source %" G_GINT64_FORMAT ": %s", command->id, error->message);
		break;
	}

	case COMMAND_BACKUP:
		worker_backup_store(self, command->id, command->store_dir, command->destination);
		break;

	case COMMAND_PUSH:
	{
		WorkerPush *push;

		source = worker_install(self, command->spec);
		push = g_new0(WorkerPush, 1);
		push->id = g_steal_pointer(&command->push_id);
		push->body = g_steal_pointer(&command->body);

		/* Live until its run is handed back: a test waiting for the
		 * worker to settle must not find it idle in between. */
		worker_live_add(self, 1);
		g_queue_push_tail(&source->pushes, push);
		break;
	}
	}

	worker_publish_status(self);
	worker_reschedule(self);

	return G_SOURCE_REMOVE;
}

static void
worker_send(
	VentureSeriesWorker	*self,
	WorkerCommand		*command
){
	command->worker = self;
	worker_live_add(self, 1);
	g_main_context_invoke_full(self->context, G_PRIORITY_DEFAULT, worker_command, command,
	                           worker_command_free);
}

void
venture_series_worker_set_source(
	VentureSeriesWorker	*self,
	VentureFeedSource	*source
){
	WorkerCommand *command;

	g_return_if_fail(VENTURE_IS_SERIES_WORKER(self));
	g_return_if_fail(NULL != source);

	command = g_new0(WorkerCommand, 1);
	command->kind = COMMAND_SET;
	command->spec = venture_feed_source_ref(source);
	worker_send(self, command);
}

void
venture_series_worker_remove_source(
	VentureSeriesWorker	*self,
	gint64			 source_id
){
	WorkerCommand *command;

	g_return_if_fail(VENTURE_IS_SERIES_WORKER(self));

	command = g_new0(WorkerCommand, 1);
	command->kind = COMMAND_REMOVE;
	command->id = source_id;
	worker_send(self, command);
}

void
venture_series_worker_sync(
	VentureSeriesWorker		*self,
	VentureFeedSource		*source,
	VentureDataSourceRunTrigger	 trigger
){
	WorkerCommand *command;

	g_return_if_fail(VENTURE_IS_SERIES_WORKER(self));
	g_return_if_fail(NULL != source);

	command = g_new0(WorkerCommand, 1);
	command->kind = COMMAND_SYNC;
	command->spec = venture_feed_source_ref(source);
	command->trigger = trigger;
	worker_send(self, command);
}

void
venture_series_worker_push(
	VentureSeriesWorker	*self,
	VentureFeedSource	*source,
	const gchar		*push_id,
	GBytes			*body
){
	WorkerCommand *command;

	g_return_if_fail(VENTURE_IS_SERIES_WORKER(self));
	g_return_if_fail(NULL != source);
	g_return_if_fail(NULL != push_id);
	g_return_if_fail(NULL != body);

	command = g_new0(WorkerCommand, 1);
	command->kind = COMMAND_PUSH;
	command->spec = venture_feed_source_ref(source);
	command->push_id = g_strdup(push_id);
	command->body = g_bytes_ref(body);
	worker_send(self, command);
}

void
venture_series_worker_purge(
	VentureSeriesWorker	*self,
	gint64			 source_id,
	const gchar		*store_dir
){
	WorkerCommand *command;

	g_return_if_fail(VENTURE_IS_SERIES_WORKER(self));
	g_return_if_fail(NULL != store_dir);

	command = g_new0(WorkerCommand, 1);
	command->kind = COMMAND_PURGE;
	command->id = source_id;
	command->store_dir = g_strdup(store_dir);
	worker_send(self, command);
}

void
venture_series_worker_backup(
	VentureSeriesWorker	*self,
	gint64			 tag,
	const gchar		*store_dir,
	const gchar		*destination
){
	WorkerCommand *command;

	g_return_if_fail(VENTURE_IS_SERIES_WORKER(self));
	g_return_if_fail(NULL != store_dir);
	g_return_if_fail(NULL != destination);

	command = g_new0(WorkerCommand, 1);
	command->kind = COMMAND_BACKUP;
	command->id = tag;
	command->store_dir = g_strdup(store_dir);
	command->destination = g_strdup(destination);
	worker_send(self, command);
}

JsonNode *
venture_series_worker_dup_status(VentureSeriesWorker *self)
{
	g_autofree gchar *text = NULL;
	JsonNode *status;

	g_return_val_if_fail(VENTURE_IS_SERIES_WORKER(self), NULL);

	g_mutex_lock(&self->lock);
	text = g_strdup(self->status);
	g_mutex_unlock(&self->lock);

	status = (NULL != text) ? json_from_string(text, NULL) : NULL;

	return (NULL != status) ? status : json_from_string("{\"sources\":[]}", NULL);
}

guint
venture_series_worker_count_live(VentureSeriesWorker *self)
{
	guint live;

	g_return_val_if_fail(VENTURE_IS_SERIES_WORKER(self), 0);

	g_mutex_lock(&self->lock);
	live = self->live;
	g_mutex_unlock(&self->lock);

	return live;
}

/* --- The thread ----------------------------------------------------------------------- */

static gboolean
worker_flag_expired(gpointer data)
{
	*(gboolean *)data = TRUE;

	return G_SOURCE_REMOVE;
}

static gpointer
worker_thread(gpointer data)
{
	VentureSeriesWorker *self = data;

	g_main_context_push_thread_default(self->context);

	/* The session is this thread's: libsoup sessions belong to the
	 * context they were made on. */
	self->session = venture_feeds_session_new();
	self->sources = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, worker_source_free);

	g_async_queue_push(self->started, GINT_TO_POINTER(1));
	g_main_loop_run(self->loop);

	/* Torn down here, on the thread that owns them: stores closed by the
	 * thread that opened them, the session on its own context. */
	if (NULL != self->timer)
	{
		g_source_destroy(self->timer);
		g_clear_pointer(&self->timer, g_source_unref);
	}

	/* Fetches cancelled by stop() complete with an error; let them, so
	 * their passes are freed by their own code. The deadline is a source
	 * of its own: a blocking iteration with nothing else to wake it never
	 * returns while a provider ignores its cancellable, and stop() --
	 * which a request turning feeds off reaches -- would join forever. */
	{
		GSource *wake;
		gboolean expired = FALSE;
		gboolean busy = TRUE;

		wake = g_timeout_source_new_seconds(10);
		g_source_set_callback(wake, worker_flag_expired, &expired, NULL);
		g_source_attach(wake, self->context);

		while (busy && !expired)
		{
			GHashTableIter iter;
			gpointer value;

			/* A store copy's thread is cancelled too, and ends at its
			 * next step; its completion lands on this context. */
			busy = (self->copies > 0);
			g_hash_table_iter_init(&iter, self->sources);

			while (g_hash_table_iter_next(&iter, NULL, &value))
				busy = busy || (NULL != ((WorkerSource *)value)->pass);

			if (busy)
				g_main_context_iteration(self->context, TRUE);
		}

		g_source_destroy(wake);
		g_source_unref(wake);

		/* Every window still gathering passes is a run: finished now and
		 * kept for the service, which writes it once stop() returns. */
		{
			GHashTableIter iter;
			gpointer value;

			g_hash_table_iter_init(&iter, self->sources);

			while (g_hash_table_iter_next(&iter, NULL, &value))
				worker_flush_open_run(value);
		}

		if (busy)
		{
			/* A fetch that will not end. Its pass still points at its
			 * source, the session and this context; freeing them would
			 * hand its completion freed memory. Left behind on purpose,
			 * with the context never iterated again, so the completion
			 * is never dispatched. */
			g_message("feeds: a fetch ignored its cancellation for 10 seconds; the worker "
			          "stops without it");
			self->sources = NULL;
			self->session = NULL;
			g_main_context_pop_thread_default(self->context);

			return NULL;
		}
	}

	g_clear_pointer(&self->sources, g_hash_table_unref);
	g_clear_object(&self->session);

	while (g_main_context_iteration(self->context, FALSE))
		;

	g_main_context_pop_thread_default(self->context);

	return NULL;
}

static gboolean
worker_quit(gpointer data)
{
	g_main_loop_quit(data);

	return G_SOURCE_REMOVE;
}

void
venture_series_worker_stop(VentureSeriesWorker *self)
{
	g_return_if_fail(VENTURE_IS_SERIES_WORKER(self));

	if (NULL == self->thread)
		return;

	g_atomic_int_set(&self->stopping, TRUE);
	g_cancellable_cancel(self->cancellable);
	g_main_context_invoke(self->context, worker_quit, self->loop);
	g_thread_join(self->thread);
	self->thread = NULL;

	/* Runs handed back but not yet emitted -- their idle has not run --
	 * are the service's to write now; the idle finds itself gone from
	 * the list and does nothing. */
	g_mutex_lock(&self->lock);

	while (self->pending->len > 0)
	{
		WorkerDelivery *delivery = g_ptr_array_steal_index(self->pending, self->pending->len - 1);

		if (NULL != delivery->run)
			g_ptr_array_add(self->flushed, g_steal_pointer(&delivery->run));
	}

	g_mutex_unlock(&self->lock);
}

GPtrArray *
venture_series_worker_take_flushed(VentureSeriesWorker *self)
{
	GPtrArray *runs;

	g_return_val_if_fail(VENTURE_IS_SERIES_WORKER(self), NULL);

	g_mutex_lock(&self->lock);
	runs = g_steal_pointer(&self->flushed);
	self->flushed = g_ptr_array_new_with_free_func((GDestroyNotify)venture_feed_run_unref);
	g_mutex_unlock(&self->lock);

	return runs;
}

static void
venture_series_worker_finalize(GObject *object)
{
	VentureSeriesWorker *self = VENTURE_SERIES_WORKER(object);

	venture_series_worker_stop(self);

	g_clear_pointer(&self->loop, g_main_loop_unref);
	g_clear_pointer(&self->context, g_main_context_unref);
	g_clear_pointer(&self->main_context, g_main_context_unref);
	g_clear_pointer(&self->started, g_async_queue_unref);
	g_clear_object(&self->cancellable);
	g_clear_pointer(&self->pending, g_ptr_array_unref);
	g_clear_pointer(&self->flushed, g_ptr_array_unref);
	g_free(self->status);
	g_mutex_clear(&self->lock);

	G_OBJECT_CLASS(venture_series_worker_parent_class)->finalize(object);
}

static void
venture_series_worker_class_init(VentureSeriesWorkerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_series_worker_finalize;

	/**
	 * VentureSeriesWorker::run-finished:
	 * @self: the worker
	 * @run: what the run did
	 *
	 * Emitted on the default main context, never on the thread.
	 */
	worker_signals[SIGNAL_RUN_FINISHED] =
		g_signal_new("run-finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL, G_TYPE_NONE, 1, VENTURE_TYPE_FEED_RUN);

	/**
	 * VentureSeriesWorker::backup-finished:
	 * @self: the worker
	 * @tag: the tag venture_series_worker_backup() was given
	 * @destination: the file it was asked to write
	 * @sha256: (nullable): the copy's SHA-256, or %NULL when it failed
	 * @size: the copy's size in bytes
	 * @error: (nullable): why it failed, or %NULL
	 *
	 * Emitted on the default main context, never on the thread.
	 */
	worker_signals[SIGNAL_BACKUP_FINISHED] =
		g_signal_new("backup-finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL, G_TYPE_NONE, 5, G_TYPE_INT64, G_TYPE_STRING,
		             G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_STRING);
}

static void
venture_series_worker_init(VentureSeriesWorker *self)
{
	g_mutex_init(&self->lock);
	self->pending = g_ptr_array_new();
	self->flushed = g_ptr_array_new_with_free_func((GDestroyNotify)venture_feed_run_unref);
}

VentureSeriesWorker *
venture_series_worker_new(void)
{
	VentureSeriesWorker *self;

	self = g_object_new(VENTURE_TYPE_SERIES_WORKER, NULL);
	/*
	 * The default main context, never the caller's thread-default: the
	 * service's own retries -- a run waiting for a transaction to end, a
	 * store copy's answer, a refresh -- are g_timeout_add()/g_idle_add()
	 * on the default context, and a worker first started while somebody
	 * had a private context pushed (a nested loop, the Test action's)
	 * delivered its runs to a context nobody iterated again.
	 */
	self->main_context = g_main_context_ref(g_main_context_default());
	self->context = g_main_context_new();
	self->loop = g_main_loop_new(self->context, FALSE);
	self->cancellable = g_cancellable_new();
	self->started = g_async_queue_new();
	self->thread = g_thread_new("venture-feeds", worker_thread, self);

	/* Running before anything is sent, so no command lands on a context
	 * nobody iterates. */
	g_async_queue_pop(self->started);

	return self;
}
