/*
 * venture-feeds-metrics.c - The feeds' families in GET /metrics
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two halves. What happens -- runs by status, units, rows, requests -- is
 * counted into the registry when the service writes a run, on the main
 * thread. What is -- the newest run and the newest success, the budget
 * spent this hour, a store's size and its log's, each venue's newest
 * snapshot, the last upkeep -- is read at a scrape by a collector, on the
 * main thread too: the run records, the worker's published status (a
 * string under its own lock), stat() on the store's files, and a reader
 * handle of its own on each store. The worker is never asked anything.
 *
 * Labels: `source` is the data source's id; `venue` is a venue's key in
 * its store, tens per source. Nothing is labelled by instrument.
 */

#include "venture.h"

#include <glib/gstdio.h>

#include "feeds/venture-feeds-private.h"

#define FEEDS_METRICS_COLLECTOR "feeds"

/* The scrape-time families, written grouped: every sample of one, then
 * the next, as the format requires. */
typedef enum
{
	FM_SOURCE_INFO = 0,
	FM_LAST_RUN_TIME,
	FM_LAST_RUN_DURATION,
	FM_LAST_RUN_STATUS,
	FM_LAST_SUCCESS_TIME,
	FM_BUDGET_USED,
	FM_BUDGET_LIMIT,
	FM_IN_FLIGHT,
	FM_STORE_FILE,
	FM_STORE_WAL,
	FM_STORE_FREE,
	FM_STORE_INCREMENTAL,
	FM_VENUE_SNAPSHOT_TIME,
	FM_VENUE_SNAPSHOT_AGE,
	FM_UPKEEP_RUNNING,
	FM_UPKEEP_LAST_TIME,
	FM_UPKEEP_LAST_DURATION,
	FM_UPKEEP_LAST_FAILED,
	FM_UPKEEP_LAST_RECLAIMED,
	FM_UPKEEP_LAST_DELETED,
	FM_UPKEEP_LAST_PAGES,
	FM_COUNT
} FeedsMetric;

static const struct
{
	const gchar		*name;
	VentureMetricsKind	 kind;
	const gchar		*help;
} feeds_metric_families[FM_COUNT] = {
	{ "venture_feed_source_info", VENTURE_METRICS_GAUGE,
	  "A data source: always 1, its name, provider and organization are the labels" },
	{ "venture_feed_last_run_timestamp_seconds", VENTURE_METRICS_GAUGE,
	  "When the source's newest recorded run finished, Unix seconds" },
	{ "venture_feed_last_run_duration_seconds", VENTURE_METRICS_GAUGE,
	  "How long the newest recorded run took" },
	{ "venture_feed_last_run_status", VENTURE_METRICS_GAUGE,
	  "The newest recorded run's status: 1 for the status it has, 0 for the others" },
	{ "venture_feed_last_success_timestamp_seconds", VENTURE_METRICS_GAUGE,
	  "When the newest run that stored something (ok or partial) finished, Unix seconds" },
	{ "venture_feed_budget_used", VENTURE_METRICS_GAUGE,
	  "Requests spent from the source's hourly budget in the current hour" },
	{ "venture_feed_budget_limit", VENTURE_METRICS_GAUGE,
	  "The source's hourly request budget; 0 for none" },
	{ "venture_feed_in_flight", VENTURE_METRICS_GAUGE,
	  "1 while a pass of the source is running" },
	{ "venture_series_store_file_bytes", VENTURE_METRICS_GAUGE,
	  "Size of the source's series store file" },
	{ "venture_series_store_wal_bytes", VENTURE_METRICS_GAUGE,
	  "Size of the store's write-ahead log" },
	{ "venture_series_store_free_bytes", VENTURE_METRICS_GAUGE,
	  "Bytes of the store file on its free list: in the file, holding nothing" },
	{ "venture_series_store_incremental_vacuum", VENTURE_METRICS_GAUGE,
	  "1 when the store can hand free pages back (auto_vacuum incremental); 0 until it is rebuilt" },
	{ "venture_feed_venue_last_snapshot_timestamp_seconds", VENTURE_METRICS_GAUGE,
	  "When the venue's newest snapshot was taken, Unix seconds" },
	{ "venture_feed_venue_snapshot_age_seconds", VENTURE_METRICS_GAUGE,
	  "Seconds since the venue's newest snapshot was taken, at the scrape" },
	{ "venture_series_upkeep_running", VENTURE_METRICS_GAUGE,
	  "1 while the store's upkeep is under way or asked for" },
	{ "venture_series_upkeep_last_timestamp_seconds", VENTURE_METRICS_GAUGE,
	  "When the store's last upkeep finished, Unix seconds" },
	{ "venture_series_upkeep_last_duration_seconds", VENTURE_METRICS_GAUGE,
	  "How long the store's last upkeep took" },
	{ "venture_series_upkeep_last_failed", VENTURE_METRICS_GAUGE,
	  "1 when the store's last upkeep stopped on an error" },
	{ "venture_series_upkeep_last_reclaimed_bytes", VENTURE_METRICS_GAUGE,
	  "Bytes of file and log the last upkeep gave back to the disk (negative: grew)" },
	{ "venture_series_upkeep_last_deleted_rows", VENTURE_METRICS_GAUGE,
	  "Rows the last upkeep deleted, by table" },
	{ "venture_series_upkeep_last_vacuumed_pages", VENTURE_METRICS_GAUGE,
	  "Free pages the last upkeep handed back to the file system" },
};

/* --- Counted as runs are written ------------------------------------------------- */

static const struct
{
	const gchar	*name;
	const gchar	*help;
} feeds_run_counters[] = {
	{ "venture_feed_runs_total", "Runs recorded, by status" },
	{ "venture_feed_units_total", "Units fetched: a realm, a page, a file" },
	{ "venture_feed_units_not_modified_total", "Units the far end said had not changed" },
	{ "venture_feed_rows_total", "Rows written to the series store" },
	{ "venture_feed_new_instruments_total", "Instruments the store had not seen before" },
	{ "venture_feed_refused_total", "Listings and instruments refused" },
	{ "venture_feed_requests_total", "HTTP requests made" },
	{ "venture_feed_bytes_total", "Bytes of answers read" },
};

static void
feeds_metrics_describe(VentureMetrics *metrics)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(feeds_run_counters); i++)
		venture_metrics_describe(metrics, feeds_run_counters[i].name, VENTURE_METRICS_COUNTER,
		                         feeds_run_counters[i].help);
}

void
venture_feeds_metrics_count_run(
	VentureContext	*context,
	VentureFeedRun	*run
){
	VentureMetrics *metrics;
	g_autofree gchar *source = NULL;
	g_autofree gchar *by_status = NULL;
	g_autofree gchar *id = NULL;
	gdouble values[G_N_ELEMENTS(feeds_run_counters)];
	guint i;

	g_return_if_fail(NULL != run);

	metrics = venture_metrics_for_context(context);
	feeds_metrics_describe(metrics);

	id = g_strdup_printf("%" G_GINT64_FORMAT, run->source_id);
	source = venture_metrics_labels("source", id, NULL);
	by_status = venture_metrics_labels("source", id, "status",
	                                   venture_enum_to_nick(VENTURE_TYPE_DATA_SOURCE_RUN_STATUS,
	                                                        (gint)run->status), NULL);

	values[0] = 1;
	values[1] = (gdouble)run->units;
	values[2] = (gdouble)run->not_modified;
	values[3] = (gdouble)run->rows;
	values[4] = (gdouble)run->new_instruments;
	values[5] = (gdouble)run->refused;
	values[6] = (gdouble)run->requests;
	values[7] = (gdouble)run->bytes;

	for (i = 0; i < G_N_ELEMENTS(feeds_run_counters); i++)
		venture_metrics_add(metrics, feeds_run_counters[i].name, (0 == i) ? by_status : source, values[i]);
}

/* --- Read at a scrape ----------------------------------------------------------- */

static gint64
feeds_metrics_time(
	VentureEntity	*record,
	const gchar	*property
){
	g_autoptr(GDateTime) when = NULL;

	g_object_get(record, property, &when, NULL);

	return (NULL != when) ? g_date_time_to_unix(when) : 0;
}

/* The source's newest run, of @status when it is zero or more. */
static VentureEntity *
feeds_metrics_newest_run(
	VentureDatabase	*database,
	gint64		 source_id,
	gint		 status
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);

	if (!venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ, source_id, NULL))
		return NULL;

	if ((status >= 0) &&
	    !venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, status, NULL))
		return NULL;

	venture_query_add_order(query, "finished-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);

	return venture_database_find_one(database, query, NULL);
}

/* The worker's published entry for a source, or NULL. */
static JsonObject *
feeds_metrics_worker_source(
	JsonNode	*status,
	gint64		 source_id
){
	JsonArray *sources;
	guint i;

	if ((NULL == status) || !JSON_NODE_HOLDS_OBJECT(status))
		return NULL;

	sources = json_object_get_array_member(json_node_get_object(status), "sources");

	for (i = 0; (NULL != sources) && (i < json_array_get_length(sources)); i++)
	{
		JsonObject *entry = json_array_get_object_element(sources, i);

		if ((NULL != entry) && (json_object_get_int_member_with_default(entry, "id", 0) == source_id))
			return entry;
	}

	return NULL;
}

static guint64
feeds_metrics_file_size(
	const gchar	*directory,
	const gchar	*name
){
	g_autofree gchar *path = g_build_filename(directory, name, NULL);
	GStatBuf info;

	return (0 == g_stat(path, &info)) ? (guint64)info.st_size : 0;
}

static void
feeds_metrics_sample(
	GString		**families,
	FeedsMetric	  metric,
	const gchar	 *labels,
	gdouble		  value
){
	venture_metrics_write_sample(families[metric], feeds_metric_families[metric].name, labels, value);
}

/* The last upkeep, from the store's meta. */
static void
feeds_metrics_upkeep(
	GString			**families,
	const gchar		 *source,
	const gchar		 *id,
	VentureSeriesStore	 *reader
){
	static const gchar *const tables[] = {
		"hourly", "daily", "quotes", "snapshots", "entries", "balances", "txns",
		"current", "instruments", "listing_sets", NULL
	};
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *last;
	JsonObject *deleted;
	gint64 started;
	gint64 finished;
	guint i;

	text = venture_series_store_get_meta(reader, VENTURE_SERIES_META_UPKEEP, NULL);
	node = (NULL != text) ? json_from_string(text, NULL) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return;

	last = json_node_get_object(node);
	started = json_object_get_int_member_with_default(last, "started_at", 0);
	finished = json_object_get_int_member_with_default(last, "finished_at", 0);

	feeds_metrics_sample(families, FM_UPKEEP_LAST_TIME, source, (gdouble)finished);
	feeds_metrics_sample(families, FM_UPKEEP_LAST_DURATION, source, (gdouble)MAX(finished - started, 0));
	feeds_metrics_sample(families, FM_UPKEEP_LAST_FAILED, source,
	                     json_object_has_member(last, "error") ? 1 : 0);
	feeds_metrics_sample(families, FM_UPKEEP_LAST_RECLAIMED, source,
	                     (gdouble)json_object_get_int_member_with_default(last, "reclaimed_bytes", 0));
	feeds_metrics_sample(families, FM_UPKEEP_LAST_PAGES, source,
	                     (gdouble)json_object_get_int_member_with_default(last, "pages_vacuumed", 0));

	deleted = json_object_get_object_member(last, "deleted");

	for (i = 0; (NULL != deleted) && (NULL != tables[i]); i++)
	{
		g_autofree gchar *labels = venture_metrics_labels("source", id, "table", tables[i], NULL);

		feeds_metrics_sample(families, FM_UPKEEP_LAST_DELETED, labels,
		                     (gdouble)json_object_get_int_member_with_default(deleted, tables[i], 0));
	}
}

/* Everything a store says about itself: its size, its venues' ages and
 * its last upkeep. A store that cannot be read (none yet, or one its
 * writer has not upgraded) says only what stat() can. */
static void
feeds_metrics_store(
	GString		**families,
	const gchar	 *source,
	const gchar	 *id,
	const gchar	 *store_dir,
	gint64		  now
){
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GPtrArray) venues = NULL;
	VentureSeriesFileSize size;
	guint i;

	if (!g_file_test(store_dir, G_FILE_TEST_IS_DIR))
		return;

	feeds_metrics_sample(families, FM_STORE_FILE, source,
	                     (gdouble)feeds_metrics_file_size(store_dir, VENTURE_SERIES_STORE_FILENAME));
	feeds_metrics_sample(families, FM_STORE_WAL, source,
	                     (gdouble)feeds_metrics_file_size(store_dir, VENTURE_SERIES_STORE_FILENAME "-wal"));

	reader = venture_series_store_open_reader(store_dir, NULL);

	if (NULL == reader)
		return;

	if (venture_series_store_get_file_size(reader, &size, NULL))
	{
		feeds_metrics_sample(families, FM_STORE_FREE, source, (gdouble)size.free_bytes);
		feeds_metrics_sample(families, FM_STORE_INCREMENTAL, source, (2 == size.auto_vacuum) ? 1 : 0);
	}

	venues = venture_series_store_list_venues(reader, NULL);

	for (i = 0; (NULL != venues) && (i < venues->len); i++)
	{
		VentureSeriesVenueRow *venue = g_ptr_array_index(venues, i);
		g_autofree gchar *labels = NULL;

		/* A venue known only from an account or a statistic has no
		 * snapshot to be old. */
		if (VENTURE_SERIES_NONE == venue->last_taken_at)
			continue;

		labels = venture_metrics_labels("source", id, "venue", venue->key,
		                                "name", (NULL != venue->name) ? venue->name : venue->key, NULL);
		feeds_metrics_sample(families, FM_VENUE_SNAPSHOT_TIME, labels, (gdouble)venue->last_taken_at);
		feeds_metrics_sample(families, FM_VENUE_SNAPSHOT_AGE, labels,
		                     (gdouble)MAX(now - venue->last_taken_at, (gint64)0));
	}

	feeds_metrics_upkeep(families, source, id, reader);
}

static void
feeds_metrics_collect(
	VentureMetrics	*metrics,
	GString		*out,
	gpointer	 user_data
){
	VentureContext *context = user_data;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(JsonNode) status = NULL;
	GString *families[FM_COUNT];
	VentureFeedsService *service;
	VentureDatabase *database;
	gint64 now;
	guint i;

	(void)metrics;

	if (!venture_context_module_enabled(context, "feeds"))
		return;

	database = venture_context_get_database(context);
	now = g_get_real_time() / G_USEC_PER_SEC;

	/* The install's sources, every organization's: the scraper is the
	 * operator, not a member of one of them. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 500);
	sources = venture_database_find(database, query, NULL);

	if (NULL == sources)
		return;

	/* Only a running service has a worker to ask; asking for one would
	 * start it. */
	service = g_object_get_data(G_OBJECT(context), "venture-feeds-service");
	status = (NULL != service) ? venture_feeds_service_dup_status(service) : NULL;

	for (i = 0; i < FM_COUNT; i++)
		families[i] = g_string_new(NULL);

	for (i = 0; i < sources->len; i++)
	{
		VentureEntity *record = g_ptr_array_index(sources, i);
		g_autoptr(VentureEntity) newest = NULL;
		g_autoptr(VentureEntity) ok = NULL;
		g_autoptr(VentureEntity) partial = NULL;
		g_autofree gchar *id = NULL;
		g_autofree gchar *source = NULL;
		g_autofree gchar *info = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *provider = NULL;
		g_autofree gchar *organization = NULL;
		g_autofree gchar *store_dir = NULL;
		JsonObject *worker;
		gint64 source_id;
		gint64 success;
		guint s;

		source_id = venture_entity_get_id(record);
		id = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
		organization = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_organization_id(record));
		g_object_get(record, "name", &name, "provider", &provider, NULL);
		source = venture_metrics_labels("source", id, NULL);
		info = venture_metrics_labels("source", id, "name", name, "provider", provider,
		                              "organization", organization, NULL);
		feeds_metrics_sample(families, FM_SOURCE_INFO, info, 1);

		newest = feeds_metrics_newest_run(database, source_id, -1);

		if (NULL != newest)
		{
			gint64 started = feeds_metrics_time(newest, "started-at");
			gint64 finished = feeds_metrics_time(newest, "finished-at");
			VentureDataSourceRunStatus run_status;

			g_object_get(newest, "status", &run_status, NULL);
			feeds_metrics_sample(families, FM_LAST_RUN_TIME, source, (gdouble)finished);
			feeds_metrics_sample(families, FM_LAST_RUN_DURATION, source,
			                     (gdouble)MAX(finished - started, (gint64)0));

			for (s = VENTURE_DATA_SOURCE_RUN_STATUS_OK; s <= VENTURE_DATA_SOURCE_RUN_STATUS_DEFERRED; s++)
			{
				g_autofree gchar *labels = venture_metrics_labels(
					"source", id, "status",
					venture_enum_to_nick(VENTURE_TYPE_DATA_SOURCE_RUN_STATUS, (gint)s), NULL);

				feeds_metrics_sample(families, FM_LAST_RUN_STATUS, labels, ((guint)run_status == s) ? 1 : 0);
			}
		}

		ok = feeds_metrics_newest_run(database, source_id, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
		partial = feeds_metrics_newest_run(database, source_id, VENTURE_DATA_SOURCE_RUN_STATUS_PARTIAL);
		success = MAX((NULL != ok) ? feeds_metrics_time(ok, "finished-at") : 0,
		              (NULL != partial) ? feeds_metrics_time(partial, "finished-at") : 0);

		if (success > 0)
			feeds_metrics_sample(families, FM_LAST_SUCCESS_TIME, source, (gdouble)success);

		worker = feeds_metrics_worker_source(status, source_id);

		if (NULL != worker)
		{
			JsonNode *upkeep = json_object_get_member(worker, "upkeep");

			feeds_metrics_sample(families, FM_BUDGET_USED, source,
			                     (gdouble)json_object_get_int_member_with_default(worker, "quota_used", 0));
			feeds_metrics_sample(families, FM_BUDGET_LIMIT, source,
			                     (gdouble)json_object_get_int_member_with_default(worker, "quota_limit", 0));
			feeds_metrics_sample(families, FM_IN_FLIGHT, source,
			                     json_object_get_boolean_member_with_default(worker, "in_flight", FALSE) ? 1 : 0);
			feeds_metrics_sample(families, FM_UPKEEP_RUNNING, source,
			                     ((NULL != upkeep) && JSON_NODE_HOLDS_OBJECT(upkeep)) ? 1 : 0);
		}

		store_dir = venture_feeds_store_dir(venture_context_get_config(context), venture_entity_get_uuid(record));
		feeds_metrics_store(families, source, id, store_dir, now);
	}

	for (i = 0; i < FM_COUNT; i++)
	{
		if (0 != families[i]->len)
		{
			venture_metrics_write_family(out, feeds_metric_families[i].name, feeds_metric_families[i].kind,
			                             feeds_metric_families[i].help);
			g_string_append_len(out, families[i]->str, (gssize)families[i]->len);
		}

		g_string_free(families[i], TRUE);
	}
}

void
venture_feeds_metrics_install(VentureContext *context)
{
	VentureMetrics *metrics;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	metrics = venture_metrics_for_context(context);
	feeds_metrics_describe(metrics);

	/* The context owns the registry, so it outlives the collector and is
	 * not referenced by it. */
	venture_metrics_add_collector(metrics, FEEDS_METRICS_COLLECTOR, feeds_metrics_collect, context, NULL);
}
