/*
 * venture-feeds-service.h - Market data feeds, on the main thread
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The main-thread half of the feeds module. It reads data_source records,
 * their sealed credentials and the configuration, freezes each source into
 * a #VentureFeedSource and hands it to the worker thread; it takes each
 * finished run back, writes the data_source_run record as the system, runs
 * the records sink, emits `feed_synced` or `feed_failed` to automation and
 * calls every hook's after-run function. Everything that touches the
 * database is here, and runs on the thread that opened it.
 *
 * Get it with venture_context_get_feeds_service(): %NULL while the module
 * is off, and nothing is started until a source is enabled or somebody asks
 * for a sync.
 */

#ifndef VENTURE_FEEDS_SERVICE_H
#define VENTURE_FEEDS_SERVICE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* --- Hooks: what later modules hang on every run -------------------------------- */

/**
 * VentureFeedsHookFreezeFunc:
 * @context: the context; main thread
 * @data_source: the data_source record being frozen
 * @user_data: the hook's data
 * @out_free: (out): how to free what is returned, on any thread
 *
 * Makes the hook's frozen copy for one source -- alert rules, watched
 * instruments -- from main-thread state. Called whenever the source is
 * frozen again. What it returns is read on the worker thread and must not
 * change.
 *
 * Returns: (transfer full) (nullable): the frozen data
 */
typedef gpointer (*VentureFeedsHookFreezeFunc) (
	VentureContext	 *context,
	VentureEntity	 *data_source,
	gpointer	  user_data,
	GDestroyNotify	 *out_free
);

/**
 * VentureFeedsHookCommitFunc:
 * @store: the source's store, writer handle; worker thread
 * @source: the frozen source
 * @frozen: (nullable): what the hook's freeze returned for it
 * @run: the run so far; attach an answer with venture_feed_run_set_payload()
 * @user_data: the hook's data, read on this thread too
 *
 * Called on the worker after each unit's batch is committed. It may read
 * the store; it must not touch the database, the configuration or anything
 * else of the main thread's.
 */
typedef void (*VentureFeedsHookCommitFunc) (
	VentureSeriesStore	*store,
	VentureFeedSource	*source,
	gconstpointer		 frozen,
	VentureFeedRun		*run,
	gpointer		 user_data
);

/**
 * VentureFeedsHookRunFunc:
 * @context: the context; main thread
 * @run: what the run did, payloads included
 * @run_record: (nullable): the data_source_run just written, or %NULL when
 *   it could not be (its source was deleted)
 * @user_data: the hook's data
 *
 * Called on the main thread after the run record is written, outside any
 * transaction: write records, notify, emit -- as the system.
 */
typedef void (*VentureFeedsHookRunFunc) (
	VentureContext	*context,
	VentureFeedRun	*run,
	VentureEntity	*run_record,
	gpointer	 user_data
);

/**
 * venture_feeds_add_hook:
 * @context: the context
 * @name: the hook's name: lower case, unique; also the payload's name
 * @freeze: (nullable) (scope notified): main thread, per source
 * @commit: (nullable) (scope notified): worker thread, per unit committed
 * @run: (nullable) (scope notified): main thread, per run written
 * @user_data: (closure): read from both threads; must not change
 * @destroy: (nullable): frees @user_data with the context
 *
 * Registers a hook on every source's runs. Sources are frozen again, so a
 * hook added while the worker runs reaches the next pass.
 *
 * Returns: %TRUE when it was added; %FALSE for a name already held
 */
gboolean
venture_feeds_add_hook(
	VentureContext			*context,
	const gchar			*name,
	VentureFeedsHookFreezeFunc	 freeze,
	VentureFeedsHookCommitFunc	 commit,
	VentureFeedsHookRunFunc		 run,
	gpointer			 user_data,
	GDestroyNotify			 destroy
);

/**
 * venture_feeds_queue_refresh:
 * @context: the context
 *
 * Asks for every source to be frozen again on the next turn of the main
 * loop -- after the transaction that caused it -- when a feeds service
 * exists; nothing otherwise. Coalesced: a thousand calls are one freeze.
 * For a module whose hook freezes records of its own (alert rules, open
 * listings), called when one of those is saved or deleted.
 */
void
venture_feeds_queue_refresh(VentureContext *context);

/* --- The service ------------------------------------------------------------------- */

#define VENTURE_TYPE_FEEDS_SERVICE (venture_feeds_service_get_type())

G_DECLARE_FINAL_TYPE(VentureFeedsService, venture_feeds_service, VENTURE, FEEDS_SERVICE, GObject)

/**
 * venture_feeds_service_sync:
 * @self: the service
 * @data_source_id: a data_source in any organization the caller may reach
 * @trigger: who asked
 * @error: (out) (optional): return location for a #GError
 *
 * Queues a run of every unit of the source, now, starting the worker if
 * it is not running. Never waits for the run: it is recorded as a
 * data_source_run when it ends. A source that cannot be frozen -- its
 * provider gone, exec switched off -- gets a failed run at once, saying
 * why.
 *
 * Returns: %TRUE when queued
 */
gboolean
venture_feeds_service_sync(
	VentureFeedsService		 *self,
	gint64				  data_source_id,
	VentureDataSourceRunTrigger	  trigger,
	GError				**error
);

/**
 * venture_feeds_service_open_request:
 * @self: the service
 * @data_source_id: a data_source
 * @unit: (nullable): the unit the request is for; %NULL for the first
 * @error: (out) (optional): return location for a #GError
 *
 * Freezes the source as a run would -- its credentials, the allowlist,
 * the limits, its provider's own frozen data -- and makes one request for
 * a main-thread action that has to reach the far end (a provider's own
 * import, say). Use it with venture_feed_request_http_send(), which blocks
 * on a private main context for at most the deadline per call; there is
 * no session for the async form. It does not count against the source's
 * budget, has no cursor and no If-Modified-Since, and writes nothing.
 *
 * Returns: (transfer full) (nullable): the request
 */
VentureFeedRequest *
venture_feeds_service_open_request(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	const gchar		 *unit,
	GError			**error
);

/**
 * venture_feeds_service_test:
 * @self: the service
 * @data_source_id: a data_source
 * @unit: (nullable): which unit; %NULL for the first
 * @error: (out) (optional): return location for a #GError
 *
 * Fetches one unit and says what came back, writing nothing anywhere --
 * not the store, not a run, not a record. Blocks the main thread up to
 * the fetch deadline, iterating a private main context so nothing else
 * runs nested inside it. Does not count against the source's budget.
 *
 * Returns: (transfer full) (nullable): a one-line report, credentials
 *   redacted
 */
gchar *
venture_feeds_service_test(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	const gchar		 *unit,
	GError			**error
);

/**
 * venture_feeds_service_purge_history:
 * @self: the service
 * @data_source_id: a data_source
 * @error: (out) (optional): return location for a #GError
 *
 * Deletes the source's series store, on the thread that holds its writer.
 * The records -- the source, its runs -- are untouched. Cannot be undone.
 *
 * Returns: %TRUE when the deletion was done or queued
 */
gboolean
venture_feeds_service_purge_history(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	GError			**error
);

/**
 * venture_feeds_service_dup_status:
 * @self: the service
 *
 * Returns: (transfer full): the worker's published status; an empty
 *   source list while it is not running
 */
JsonNode *
venture_feeds_service_dup_status(VentureFeedsService *self);

/**
 * venture_feeds_service_count_pending:
 * @self: the service
 *
 * Commands the worker has not taken, passes in flight, runs not yet
 * delivered and runs waiting for a moment with no transaction open. Zero
 * means quiet; tests wait for it, bounded.
 *
 * Returns: the count
 */
guint
venture_feeds_service_count_pending(VentureFeedsService *self);

/**
 * venture_feeds_service_is_running:
 * @self: the service
 *
 * Returns: whether the worker thread exists
 */
gboolean
venture_feeds_service_is_running(VentureFeedsService *self);

/**
 * venture_feeds_service_open_reader:
 * @self: the service
 * @data_source_id: a data_source
 * @error: (out) (optional): return location for a #GError
 *
 * A read-only handle on the source's store, for the main thread: pages,
 * reports, the price oracle. %VENTURE_ERROR_NOT_FOUND until its first
 * run has written something. One handle per thread; close it when done.
 *
 * Returns: (transfer full) (nullable): the handle
 */
VentureSeriesStore *
venture_feeds_service_open_reader(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	GError			**error
);

/**
 * venture_feeds_service_refresh:
 * @self: the service
 *
 * Freezes every data source again and hands them to the worker, starting
 * it if a source is enabled. Called after a source or its credentials
 * change, the configuration changes or a hook is added; the service does
 * it itself on those signals, after the transaction that caused them.
 */
void
venture_feeds_service_refresh(VentureFeedsService *self);

/**
 * venture_feeds_service_shutdown:
 * @self: the service
 *
 * Stops the worker and joins its thread. Runs not yet delivered are lost;
 * the store has everything they wrote. Idempotent.
 */
void
venture_feeds_service_shutdown(VentureFeedsService *self);

/**
 * venture_context_get_data_source_providers:
 * @self: a context
 *
 * The providers a data source may name: the built-in http_json, csv and
 * file_jsonl, then whatever plugins register as they load. On the context
 * because plugins load before the feeds service exists, and whether or
 * not the module is on, so turning it on needs no reload.
 *
 * Returns: (transfer none): the registry
 */
VentureDataSourceProviderRegistry *
venture_context_get_data_source_providers(VentureContext *self);

/**
 * venture_context_get_feeds_service:
 * @self: a context
 *
 * The feeds service, made on first use. Nothing starts until a source is
 * enabled or a sync is asked for.
 *
 * Returns: (transfer none) (nullable): the service, or %NULL while the
 *   feeds module is off
 */
VentureFeedsService *
venture_context_get_feeds_service(VentureContext *self);

/**
 * venture_feeds_shutdown:
 * @context: a context
 *
 * Stops the context's feeds worker, if one runs, and drops the service.
 * Called when the context is disposed and when the module is switched off.
 */
void
venture_feeds_shutdown(VentureContext *context);

/**
 * venture_feeds_store_dir:
 * @config: the configuration
 * @uuid: a data_source's uuid
 *
 * Where a source's store lives: <state_dir>/series/<uuid>. Derived every
 * time, never stored.
 *
 * Returns: (transfer full): the directory
 */
gchar *
venture_feeds_store_dir(
	VentureConfig	*config,
	const gchar	*uuid
);

/**
 * venture_feeds_install:
 * @context: a context
 *
 * The data_source save validator and the sync, test and purge_history
 * actions, once per database; and this context's listeners, which keep
 * the worker in step with the records. Called by venture_context_new().
 */
void
venture_feeds_install(VentureContext *context);

/**
 * venture_feeds_register_automation:
 * @registry: the automation handler registry
 *
 * The `feeds_sync` handler and the `feed_synced` and `feed_failed`
 * events. Registered whether or not the module is on; the handler refuses
 * while it is off.
 */
void
venture_feeds_register_automation(VentureAutomationHandlerRegistry *registry);

/**
 * venture_feeds_register_provides:
 * @registry: the plugin provides registry
 *
 * The `data_source_provider` kind: an exec plugin's program as a data
 * source provider. Registered by the context with the kinds core knows.
 */
void
venture_feeds_register_provides(VenturePluginProvidesRegistry *registry);

/**
 * venture_feeds_parse_settings:
 * @text: (nullable): YAML text; empty is an empty mapping
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the settings as a JSON object
 */
JsonObject *
venture_feeds_parse_settings(
	const gchar	 *text,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_FEEDS_SERVICE_H */
