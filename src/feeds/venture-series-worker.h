/*
 * venture-series-worker.h - The thread that fetches market data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One thread, its own GMainContext, its own HTTP session and the writer
 * handle of every source's series store. The main thread hands it frozen
 * sources (#VentureFeedSource) and sync requests; it decides when each unit
 * is due, fetches, writes the store, recomputes region figures, purges
 * history, and hands back each run as plain data (#VentureFeedRun) through
 * the main context. It never touches #VentureDatabase, the entity
 * registry, the configuration or a plugin's settings -- everything it
 * needs was frozen before it saw it -- and it logs with g_message(), never
 * g_warning(), which the test harness makes fatal from any thread.
 *
 * Only the feeds service drives it. Later modules hook in through
 * venture_feeds_add_hook(), not here.
 */

#ifndef VENTURE_SERIES_WORKER_H
#define VENTURE_SERIES_WORKER_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_FEEDS_CHECK_EARLY:
 *
 * Seconds before a venue's expected update that the adaptive schedule
 * checks it: Undermine's 45, so a fresh snapshot is caught on its first
 * minute without polling a stale one all hour.
 */
#define VENTURE_FEEDS_CHECK_EARLY (45)

/**
 * VENTURE_FEEDS_PROBE_MIN:
 *
 * The shortest wait for a venue's expected update worth a probe halfway
 * there: under twenty minutes the probe and its two follow-ups would
 * cost more requests than the minutes they could save.
 */
#define VENTURE_FEEDS_PROBE_MIN (20 * 60)

/**
 * VENTURE_FEEDS_REGION_EVERY:
 *
 * Seconds between region recomputes of one store: half an hour, as
 * Undermine does. A manual sync recomputes at once.
 */
#define VENTURE_FEEDS_REGION_EVERY (1800)

/* --- A run ----------------------------------------------------------------------- */

/**
 * VentureFeedRun:
 *
 * What a run did, as plain data: figures, the first error, notes, the
 * record upserts it gathered, and whatever hooks attached. Built on the
 * worker, read on the main thread; never both at once.
 */
typedef struct _VentureFeedRun VentureFeedRun;

#define VENTURE_TYPE_FEED_RUN (venture_feed_run_get_type())

GType venture_feed_run_get_type(void) G_GNUC_CONST;

/**
 * venture_feed_run_ref:
 * @self: a run
 *
 * Returns: (transfer full): @self
 */
VentureFeedRun *
venture_feed_run_ref(VentureFeedRun *self);

/**
 * venture_feed_run_unref:
 * @self: (transfer full): a run
 */
void
venture_feed_run_unref(VentureFeedRun *self);

/**
 * venture_feed_run_get_source_id:
 * @self: a run
 *
 * Returns: the data_source record's id
 */
gint64
venture_feed_run_get_source_id(VentureFeedRun *self);

/**
 * venture_feed_run_get_organization_id:
 * @self: a run
 *
 * Returns: the source's organization
 */
gint64
venture_feed_run_get_organization_id(VentureFeedRun *self);

/**
 * venture_feed_run_get_status:
 * @self: a run
 *
 * Returns: how it ended
 */
VentureDataSourceRunStatus
venture_feed_run_get_status(VentureFeedRun *self);

/**
 * venture_feed_run_get_trigger:
 * @self: a run
 *
 * Returns: what started it
 */
VentureDataSourceRunTrigger
venture_feed_run_get_trigger(VentureFeedRun *self);

/**
 * venture_feed_run_get_rows:
 * @self: a run
 *
 * Returns: rows written to the store
 */
gint64
venture_feed_run_get_rows(VentureFeedRun *self);

/**
 * venture_feed_run_get_error:
 * @self: a run
 *
 * Returns: (transfer none) (nullable): its first error, redacted
 */
const gchar *
venture_feed_run_get_error(VentureFeedRun *self);

/**
 * venture_feed_run_get_venues:
 * @self: a run
 *
 * The venues whose snapshots the run committed, for a hook that only
 * wants to look at what changed.
 *
 * Returns: (transfer none) (array zero-terminated=1): venue keys
 */
const gchar *const *
venture_feed_run_get_venues(VentureFeedRun *self);

/**
 * venture_feed_run_get_started_at:
 * @self: a run
 *
 * When the run's first fetch began, in Unix seconds: a hook that wants
 * "what this run brought" asks the store for rows that arrived since.
 *
 * Returns: the start
 */
gint64
venture_feed_run_get_started_at(VentureFeedRun *self);

/**
 * venture_feed_run_get_push_id:
 * @self: a run
 *
 * The id venture_feeds_service_push() handed back when the body this run
 * read was pushed, so the one who pushed can find the run it made.
 *
 * Returns: (transfer none) (nullable): the push's id, or %NULL for a run
 *   that fetched
 */
const gchar *
venture_feed_run_get_push_id(VentureFeedRun *self);

/**
 * venture_feed_run_add_note:
 * @self: a run
 * @note: (nullable): one line; empty is ignored
 *
 * Adds a note the run record will carry, on the worker while the run is
 * being built -- a hook's commit function may call it to say what it left
 * out. A run keeps at most forty: it is a record, not a log.
 */
void
venture_feed_run_add_note(
	VentureFeedRun	*self,
	const gchar	*note
);

/**
 * venture_feed_run_set_payload:
 * @self: a run
 * @name: the hook's name
 * @payload: (transfer full) (nullable): plain JSON for the main thread
 *
 * A hook's answer, attached on the worker after a commit and read back on
 * the main thread after the run is written. Calling it again replaces it.
 */
void
venture_feed_run_set_payload(
	VentureFeedRun	*self,
	const gchar	*name,
	JsonNode	*payload
);

/**
 * venture_feed_run_get_payload:
 * @self: a run
 * @name: the hook's name
 *
 * Returns: (transfer none) (nullable): what the hook attached
 */
JsonNode *
venture_feed_run_get_payload(
	VentureFeedRun	*self,
	const gchar	*name
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureFeedRun, venture_feed_run_unref)

/* --- Scheduling ------------------------------------------------------------------- */

/**
 * venture_feeds_schedule_next_auto:
 * @new_data: whether the fetch brought a snapshot newer than the store had
 * @next_expected: when the store expects the unit's venue to update next
 *   (its newest snapshot plus its learned interval), or
 *   %VENTURE_SERIES_NONE when it has learned nothing
 * @retry_after: seconds the far end asked to be left alone, or 0
 * @backoff_step: (inout): how many checks in a row found nothing new
 * @now: the time now, Unix seconds
 *
 * The adaptive schedule, Undermine's. After new data: check
 * %VENTURE_FEEDS_CHECK_EARLY seconds before the venue's expected update,
 * or in a minute if that has passed, or in an hour if nothing has been
 * learned -- and, when the expected update is at least
 * %VENTURE_FEEDS_PROBE_MIN away, first probe halfway there, so a learned
 * interval that is a multiple of the real one is caught and unlearned.
 * Nothing new before the expected update (a probe that found the old
 * snapshot; pass @next_expected): look again in 1, then 5 minutes, then
 * at the expected update. Nothing new after it, or a failure (pass
 * %VENTURE_SERIES_NONE): back off 1, 5, 15, then 30 minutes. Never sooner
 * than a Retry-After.
 *
 * Returns: when to check the unit next, Unix seconds
 */
gint64
venture_feeds_schedule_next_auto(
	gboolean	 new_data,
	gint64		 next_expected,
	gint64		 retry_after,
	guint		*backoff_step,
	gint64		 now
);

/* --- The worker ---------------------------------------------------------------- */

#define VENTURE_TYPE_SERIES_WORKER (venture_series_worker_get_type())

G_DECLARE_FINAL_TYPE(VentureSeriesWorker, venture_series_worker, VENTURE, SERIES_WORKER, GObject)

/**
 * venture_series_worker_new:
 *
 * Starts the thread. #VentureSeriesWorker::run-finished and
 * #VentureSeriesWorker::backup-finished are emitted on the default main
 * context, where the feeds service's own retries run too, whatever context
 * the caller had pushed.
 *
 * Returns: (transfer full): the worker, running
 */
VentureSeriesWorker *
venture_series_worker_new(void);

/**
 * venture_series_worker_set_source:
 * @self: a worker
 * @source: a frozen source; the worker takes a reference
 *
 * Adds a source or replaces its frozen copy. What the worker learned about
 * the source -- each unit's schedule, its open store -- is kept.
 */
void
venture_series_worker_set_source(
	VentureSeriesWorker	*self,
	VentureFeedSource	*source
);

/**
 * venture_series_worker_remove_source:
 * @self: a worker
 * @source_id: a data_source id
 *
 * Forgets a source and closes its store. A fetch in flight finishes and
 * its run is still handed back, and so is a scheduled window still
 * gathering passes: finished now rather than dropped.
 */
void
venture_series_worker_remove_source(
	VentureSeriesWorker	*self,
	gint64			 source_id
);

/**
 * venture_series_worker_sync:
 * @self: a worker
 * @source: the source, frozen now, so a sync uses what the record says
 *   this moment
 * @trigger: who asked
 *
 * Queues a run of every unit, now. A second sync while one is waiting
 * joins it.
 */
void
venture_series_worker_sync(
	VentureSeriesWorker		*self,
	VentureFeedSource		*source,
	VentureDataSourceRunTrigger	 trigger
);

/**
 * venture_series_worker_push:
 * @self: a worker
 * @source: the source, frozen now
 * @push_id: the id the run will carry
 * @body: (transfer none): the JSON lines that were pushed
 *
 * Queues a run of one unit, "push", that reads @body through the source's
 * provider instead of fetching anything: the `push` provider parses it
 * exactly as `file_jsonl` parses a file. Each push is a run of its own,
 * in the order they arrived, after any pass already in flight.
 */
void
venture_series_worker_push(
	VentureSeriesWorker	*self,
	VentureFeedSource	*source,
	const gchar		*push_id,
	GBytes			*body
);

/**
 * venture_series_worker_purge:
 * @self: a worker
 * @source_id: a data_source id
 * @store_dir: its store's directory
 *
 * Closes the source's store and deletes it, on the thread that owns the
 * writer. The next fetch starts a new one.
 */
void
venture_series_worker_purge(
	VentureSeriesWorker	*self,
	gint64			 source_id,
	const gchar		*store_dir
);

/**
 * venture_series_worker_backup:
 * @self: a worker
 * @tag: the caller's id for this copy (the backup_run's id), handed back
 * @store_dir: the store's directory
 * @destination: the file to write
 *
 * Queues a copy of a store with venture_series_store_backup(), taken on a
 * short-lived thread of its own -- a store can be gigabytes, the main
 * thread serves requests and the worker's loop serves every fetch -- that
 * touches nothing but its own SQLite connections. The copy is the store as
 * of one commit (venture_series_store_backup() pins a read), so it never
 * sees a write half done. The answer comes back on the main context as
 * #VentureSeriesWorker::backup-finished. A copy still running when the
 * worker stops is cancelled and left unanswered; its caller must notice
 * that itself.
 */
void
venture_series_worker_backup(
	VentureSeriesWorker	*self,
	gint64			 tag,
	const gchar		*store_dir,
	const gchar		*destination
);

/**
 * venture_series_worker_dup_status:
 * @self: a worker
 *
 * What the worker is doing, as last published: every source, whether a
 * fetch is in flight, each unit's next check and back-off, the quota.
 * Safe from any thread.
 *
 * Returns: (transfer full): a JSON object
 */
JsonNode *
venture_series_worker_dup_status(VentureSeriesWorker *self);

/**
 * venture_series_worker_count_live:
 * @self: a worker
 *
 * Commands not yet taken, passes in flight and runs not yet delivered to
 * the main context. Tests wait for zero, bounded.
 *
 * Returns: the count
 */
guint
venture_series_worker_count_live(VentureSeriesWorker *self);

/**
 * venture_series_worker_stop:
 * @self: a worker
 *
 * Cancels every fetch and store copy, stops the loop and joins the thread.
 * Runs not yet delivered, and every scheduled window still gathering
 * passes (finished now), are kept for venture_series_worker_take_flushed()
 * rather than emitted. Idempotent.
 */
void
venture_series_worker_stop(VentureSeriesWorker *self);

/**
 * venture_series_worker_take_flushed:
 * @self: a stopped worker
 *
 * The runs venture_series_worker_stop() kept: finished, never emitted. The
 * caller writes them -- or says why it could not.
 *
 * Returns: (transfer full) (element-type VentureFeedRun): the runs, oldest
 *   first as they were kept; empty when there were none
 */
GPtrArray *
venture_series_worker_take_flushed(VentureSeriesWorker *self);

/**
 * venture_series_worker_delete_store:
 * @store_dir: a store's directory
 * @error: (out) (optional): return location for a #GError
 *
 * Deletes a store's files and its directory. Only on the thread that owns
 * the writer, or when no thread does.
 *
 * Returns: %TRUE when nothing is left
 */
gboolean
venture_series_worker_delete_store(
	const gchar	 *store_dir,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_SERIES_WORKER_H */
