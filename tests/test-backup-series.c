/*
 * test-backup-series.c - Market-data series stores copied with installation
 * backups
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A series store holds history its source will never serve again, so an
 * installation backup copies every store beside the database: one
 * backup_run of scope "series" per store, the copy taken on the feeds
 * worker with SQLite's online backup API, the run recorded on the main
 * thread before and after. These tests hold that to what it promises --
 * the copy reads back the same rows, it is consistent while a writer
 * commits, retention prunes it per store, a failure stays on its own run,
 * and nothing happens when feeds or the setting are off.
 */

#include <venture.h>
#include <glib/gstdio.h>
#include <string.h>

#include "venture-test-util.h"

#ifdef VENTURE_HAVE_SQLITE

#include <sqlite3.h>

typedef struct
{
	gchar		*state_dir;
	gchar		*file_root;
	gchar		*backups;
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 org;
} Fixture;

/* user_data: GINT_TO_POINTER(TRUE) for feeds on. */
static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	fixture->state_dir = g_dir_make_tmp("venture-backup-series-XXXXXX", &error);
	g_assert_no_error(error);
	fixture->file_root = g_build_filename(fixture->state_dir, "files", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->file_root, 0700), ==, 0);
	fixture->backups = g_build_filename(fixture->state_dir, "backups", NULL);

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "feeds-enabled", (gboolean)GPOINTER_TO_INT(user_data),
	             "feeds-file-roots", fixture->file_root,
	             "feeds-run-window-minutes", (gint64)0,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* The registry is process-wide: with this context's modules applied,
	 * the feeds tables are created now (or stay masked with feeds off). */
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

/*
 * Lets the main loop take answers back until nothing is pending, bounded:
 * a test that can hang tells less than one that fails.
 */
static void
settle(Fixture *fixture)
{
	VentureFeedsService *service;
	gint64 deadline;

	service = venture_context_get_feeds_service(fixture->context);

	if (NULL == service)
		return;

	deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

	while (venture_feeds_service_count_pending(service) > 0)
	{
		if (g_get_monotonic_time() > deadline)
			g_error("the feeds worker did not settle within 30 seconds");

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* Files in the backups directory whose names start with @prefix. */
static guint
count_files(
	Fixture		*fixture,
	const gchar	*prefix,
	const gchar	*suffix
){
	g_autoptr(GDir) dir = NULL;
	const gchar *name;
	guint count;

	dir = g_dir_open(fixture->backups, 0, NULL);
	count = 0;

	while ((NULL != dir) && (NULL != (name = g_dir_read_name(dir))))
		if (g_str_has_prefix(name, prefix) && ((NULL == suffix) || g_str_has_suffix(name, suffix)))
			count++;

	return count;
}

/*
 * The worker is drained before the fixture goes: a copy still being
 * written while the state directory is removed is how a run leaves a
 * directory in /tmp behind. Nothing half-written may be left either.
 */
static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	settle(fixture);
	g_assert_cmpuint(count_files(fixture, "", ".partial"), ==, 0);

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_test_remove_tree(fixture->state_dir);
	g_free(fixture->state_dir);
	g_free(fixture->file_root);
	g_free(fixture->backups);
}

static VentureFeedsService *
service_of(Fixture *fixture)
{
	VentureFeedsService *service;

	service = venture_context_get_feeds_service(fixture->context);
	g_assert_nonnull(service);

	return service;
}

static VentureBackupScheduleService *
backups_of(Fixture *fixture)
{
	return venture_backup_schedule_service_get(fixture->database);
}

/* A JSON-lines source with one realm and one item at @price. */
static gint64
create_source(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*file
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;

	settings = g_strdup_printf("file: %s\n", file);
	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", name, "provider", "file_jsonl", "settings", settings,
	             "schedule", "manual", "currency", "GOLD", NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error));
	g_assert_no_error(error);

	return venture_entity_get_id(VENTURE_ENTITY(source));
}

/* Writes the source's file with one snapshot and syncs it into its store. */
static void
feed(
	Fixture		*fixture,
	gint64		 source_id,
	const gchar	*file,
	const gchar	*taken_at,
	const gchar	*price,
	gint		 quantity
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *lines = NULL;
	g_autoptr(GError) error = NULL;

	lines = g_strdup_printf(
		"{\"type\":\"venue\",\"key\":\"argent\",\"name\":\"Argent Dawn\",\"currency\":\"GOLD\",\"group\":\"eu\"}\n"
		"{\"type\":\"instrument\",\"key\":\"ore\",\"name\":\"Iron ore\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"taken_at\":\"%s\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"%s\",\"quantity\":%d,\"id\":\"%s\"}\n",
		taken_at, price, quantity, taken_at);
	path = g_build_filename(fixture->file_root, file, NULL);
	g_assert_true(g_file_set_contents(path, lines, -1, NULL));

	g_assert_true(venture_feeds_service_sync(service_of(fixture), source_id,
	                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
	g_assert_no_error(error);
	settle(fixture);
}

static gchar *
uuid_of(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;

	record = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, source_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(record);

	return g_strdup(venture_entity_get_uuid(record));
}

static VentureEntity *
make_schedule(
	Fixture	*fixture,
	gint64	 retention
){
	g_autoptr(GError) error = NULL;
	VentureBackupSchedule *schedule;

	schedule = venture_backup_schedule_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(schedule), fixture->org);
	g_object_set(schedule, "name", "Installation", "scope", "installation", "schedule", "",
	             "retention", retention, "destination", fixture->backups, "verify", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(schedule), NULL, &error));
	g_assert_no_error(error);

	return VENTURE_ENTITY(schedule);
}

/* Runs the installation backup; the database copy must succeed. */
static VentureEntity *
run_backup(
	Fixture		*fixture,
	VentureEntity	*schedule,
	const gchar	*at
){
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GError) error = NULL;
	VentureEntity *run;

	as_of = g_date_time_new_from_iso8601(at, NULL);
	g_assert_nonnull(as_of);
	run = venture_backup_schedule_service_run(backups_of(fixture), VENTURE_BACKUP_SCHEDULE(schedule),
	                                          as_of, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(run);

	return run;
}

/* backup_run rows of @scope, oldest first. */
static GPtrArray *
runs_of_scope(
	Fixture		*fixture,
	const gchar	*scope
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	query = venture_query_new(VENTURE_TYPE_BACKUP_RUN);
	venture_query_set_organization(query, fixture->org);
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_string(query, "scope", VENTURE_FILTER_OP_EQ, scope, &error));
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);

	return rows;
}

static gchar *
text(
	VentureEntity	*record,
	const gchar	*field
){
	gchar *value = NULL;

	g_object_get(record, field, &value, NULL);

	return value;
}

static gint64
number(
	VentureEntity	*record,
	const gchar	*field
){
	gint64 value = 0;

	g_object_get(record, field, &value, NULL);

	return value;
}

static VentureEntity *
reread(
	Fixture		*fixture,
	VentureEntity	*run
){
	g_autoptr(GError) error = NULL;
	VentureEntity *fresh;

	fresh = venture_database_get(fixture->database, VENTURE_TYPE_BACKUP_RUN,
	                             venture_entity_get_id(run), &error);
	g_assert_no_error(error);
	g_assert_nonnull(fresh);

	return fresh;
}

/* Copies @file to <dir>/store.db so a reader can open it as a store. */
static gchar *
store_dir_from_copy(
	Fixture		*fixture,
	const gchar	*file,
	const gchar	*name
){
	g_autofree gchar *contents = NULL;
	g_autofree gchar *target = NULL;
	gchar *dir;
	gsize length;

	dir = g_build_filename(fixture->state_dir, name, NULL);
	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	g_assert_true(g_file_get_contents(file, &contents, &length, NULL));
	target = g_build_filename(dir, VENTURE_SERIES_STORE_FILENAME, NULL);
	g_assert_true(g_file_set_contents(target, contents, (gssize)length, NULL));

	return dir;
}

static void
assert_current(
	VentureSeriesStore	*store,
	gint64			 min_price,
	gint64			 quantity
){
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_series_store_get_current(store, "argent", "ore", &row, &error));
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_assert_cmpint(row->min_price, ==, min_price);
	g_assert_cmpint(row->quantity, ==, quantity);
}

/* --- Round trip ----------------------------------------------------------------- */

/*
 * A store with data is copied beside the installation copy, recorded as
 * its own run, and the copy reads back the same current and daily rows --
 * then restores by the documented hand procedure over a store that has
 * moved on, and verifies as intact.
 *
 * What breaks if this regresses: the only copy of months of auction
 * history. A run that says "succeeded" over a file that does not open, or
 * opens empty, is worse than no backup, because it is believed.
 */
static void
test_round_trip(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) main_run = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(VentureSeriesStore) live = NULL;
	g_autoptr(VentureSeriesStore) copy = NULL;
	g_autoptr(GArray) live_daily = NULL;
	g_autoptr(GArray) copy_daily = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *sha = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *copy_dir = NULL;
	VentureEntity *run;
	gsize length;
	gint64 source_id;

	(void)user_data;

	source_id = create_source(fixture, "Argent", "argent.jsonl");
	feed(fixture, source_id, "argent.jsonl", "2026-10-03T12:00:00Z", "1.25", 20);
	uuid = uuid_of(fixture, source_id);

	schedule = make_schedule(fixture, 3);
	main_run = run_backup(fixture, schedule, "2026-10-04T01:00:00Z");

	/* The backup returned without waiting for the store: its copy is on
	 * the worker, recorded as running until the answer comes back. */
	{
		g_autoptr(GPtrArray) queued = runs_of_scope(fixture, "series");
		g_autofree gchar *queued_status = NULL;

		g_assert_cmpuint(queued->len, ==, 1);
		queued_status = text(g_ptr_array_index(queued, 0), "status");
		g_assert_cmpstr(queued_status, ==, "running");
	}

	settle(fixture);

	series = runs_of_scope(fixture, "series");
	g_assert_cmpuint(series->len, ==, 1);
	run = g_ptr_array_index(series, 0);
	status = text(run, "status");
	path = text(run, "path");
	sha = text(run, "sha256");
	g_assert_cmpstr(status, ==, "succeeded");
	g_assert_cmpint(number(run, "parent-run-id"), ==, venture_entity_get_id(main_run));
	g_assert_cmpint(number(run, "schedule-id"), ==, venture_entity_get_id(schedule));

	{
		g_autofree gchar *store = text(run, "store-uuid");
		g_autofree gchar *kind = text(run, "kind");

		g_assert_cmpstr(store, ==, uuid);
		g_assert_cmpstr(kind, ==, "backup");
	}

	/* The file beside the database copy, and the run vouches for it. */
	g_assert_true(g_str_has_prefix(path, fixture->backups));
	g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
	digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
	g_assert_cmpstr(digest, ==, sha);
	g_assert_cmpint(number(run, "size"), ==, (gint64)length);

	/* The copy answers as the live store does. */
	copy_dir = store_dir_from_copy(fixture, path, "copy");
	copy = venture_series_store_open_reader(copy_dir, &error);
	g_assert_no_error(error);
	live = venture_feeds_service_open_reader(service_of(fixture), source_id, &error);
	g_assert_no_error(error);
	assert_current(copy, 125, 20);
	assert_current(live, 125, 20);

	live_daily = venture_series_store_daily(live, "argent", "ore", 0, &error);
	g_assert_no_error(error);
	copy_daily = venture_series_store_daily(copy, "argent", "ore", 0, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(live_daily->len, >=, 1);
	g_assert_cmpuint(copy_daily->len, ==, live_daily->len);
	g_assert_cmpint(g_array_index(copy_daily, VentureSeriesDay, 0).min_price, ==,
	                g_array_index(live_daily, VentureSeriesDay, 0).min_price);
	g_clear_object(&copy);
	g_clear_object(&live);

	/* Verification: intact, at this build's schema, holding the row. */
	{
		g_autoptr(VentureEntity) fresh = NULL;
		g_autofree gchar *verification = NULL;
		g_autofree gchar *report = NULL;

		g_assert_true(venture_backup_schedule_service_verify(backups_of(fixture),
		                                                     VENTURE_BACKUP_RUN(run), NULL, &error));
		g_assert_no_error(error);
		fresh = reread(fixture, run);
		verification = text(fresh, "verification");
		g_assert_cmpstr(verification, ==, "tie");
		g_object_get(fresh, "report", &report, NULL);
		g_assert_nonnull(strstr(report, "\"integrity\":\"ok\""));
		g_assert_nonnull(strstr(report, "\"current_rows\":1"));

		/* Opening it immutable left the bytes the digest vouches for. */
		g_clear_pointer(&contents, g_free);
		g_clear_pointer(&digest, g_free);
		g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
		digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
		g_assert_cmpstr(digest, ==, sha);
	}

	/* The live store moves on; then the documented restore by hand: stop
	 * the store's writer, put the copy in place as store.db, drop the
	 * WAL files that belonged to the old file, start again. */
	feed(fixture, source_id, "argent.jsonl", "2026-10-04T12:00:00Z", "2.50", 7);
	live = venture_feeds_service_open_reader(service_of(fixture), source_id, &error);
	g_assert_no_error(error);
	assert_current(live, 250, 7);
	g_clear_object(&live);

	venture_feeds_shutdown(fixture->context);

	{
		g_autofree gchar *store_dir = NULL;
		g_autofree gchar *target = NULL;
		g_autofree gchar *wal = NULL;
		g_autofree gchar *shm = NULL;

		store_dir = venture_feeds_store_dir(fixture->config, uuid);
		target = g_build_filename(store_dir, VENTURE_SERIES_STORE_FILENAME, NULL);
		wal = g_strconcat(target, "-wal", NULL);
		shm = g_strconcat(target, "-shm", NULL);
		g_unlink(wal);
		g_unlink(shm);
		g_assert_true(g_file_set_contents(target, contents, (gssize)length, NULL));
	}

	live = venture_feeds_service_open_reader(service_of(fixture), source_id, &error);
	g_assert_no_error(error);
	assert_current(live, 125, 20);
}

/* --- Consistency under a writer ------------------------------------------------- */

#define WRITER_INSTRUMENTS (200)

/* Instruments that only weigh the store down: with their attributes the
 * file is thousands of pages, so a copy one page per step outlasts many
 * of the writer's commits. */
#define BALLAST_INSTRUMENTS (2000)

typedef struct
{
	gchar		*directory;
	gint		 commits;	/* atomic */
	gint		 stop;		/* atomic */
	gchar		*failure;
} Writer;

/*
 * Commits complete snapshots as fast as it can, every instrument at the
 * snapshot's own quantity: a copy that caught the store between two of
 * them would hold two different quantities in one venue's current rows.
 */
static gpointer
writer_thread(gpointer data)
{
	Writer *writer = data;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesVenue venue;
	gint64 snapshot;
	guint i;

	store = venture_series_store_open(writer->directory, &error);

	if (NULL == store)
	{
		writer->failure = g_strdup(error->message);
		return NULL;
	}

	memset(&venue, 0, sizeof venue);
	venue.key = "v";
	venue.currency = "USD";

	if (!venture_series_store_upsert_venue(store, &venue, 1000, &error) ||
	    !venture_series_store_begin(store, &error))
	{
		writer->failure = g_strdup(error->message);
		return NULL;
	}

	{
		g_autoptr(GString) attrs = g_string_new("{\"ballast\":\"");

		for (i = 0; i < 4000; i++)
			g_string_append_c(attrs, (gchar)('a' + (i % 26)));

		g_string_append(attrs, "\"}");

		for (i = 0; i < BALLAST_INSTRUMENTS; i++)
		{
			g_autofree gchar *key = g_strdup_printf("ballast-%04u", i);
			VentureSeriesInstrument instrument;

			memset(&instrument, 0, sizeof instrument);
			instrument.key = key;
			instrument.attrs_json = attrs->str;

			if (!venture_series_store_upsert_instrument(store, &instrument, 1000, NULL, NULL, &error))
			{
				writer->failure = g_strdup(error->message);
				return NULL;
			}
		}
	}

	if (!venture_series_store_commit(store, &error))
	{
		writer->failure = g_strdup(error->message);
		return NULL;
	}

	for (snapshot = 1; !g_atomic_int_get(&writer->stop); snapshot++)
	{
		g_autoptr(VentureSeriesSnapshot) open = NULL;
		VentureSeriesCommitResult result;

		open = venture_series_store_begin_snapshot(store, "v", NULL, 1000 + snapshot * 60,
		                                           1000 + snapshot * 60, TRUE, &error);

		for (i = 0; (NULL != open) && (i < WRITER_INSTRUMENTS); i++)
		{
			g_autofree gchar *key = g_strdup_printf("i%02u", i);
			VentureSeriesListing listing;

			memset(&listing, 0, sizeof listing);
			listing.instrument_key = key;
			listing.unit_price = 100 + i;
			listing.quantity = snapshot;
			listing.side = VENTURE_SERIES_SIDE_SELL;
			listing.expires_in_min = -1;

			if (!venture_series_snapshot_add_listing(open, &listing, &error))
				break;
		}

		if ((NULL == open) || (NULL != error) ||
		    !venture_series_store_commit_snapshot(store, g_steal_pointer(&open), &result, &error))
		{
			writer->failure = g_strdup(error->message);
			return NULL;
		}

		g_atomic_int_inc(&writer->commits);
	}

	return NULL;
}

typedef struct
{
	GMutex		 lock;
	GCond		 cond;
	gboolean	 done;
	GCancellable	*cancellable;
} Deadline;

/* Cancels the copy if it has not finished in twenty seconds, so a copy
 * that never ends fails the test instead of hanging it. */
static gpointer
deadline_thread(gpointer data)
{
	Deadline *deadline = data;
	gint64 until;

	until = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
	g_mutex_lock(&deadline->lock);

	while (!deadline->done)
		if (!g_cond_wait_until(&deadline->cond, &deadline->lock, until))
		{
			g_cancellable_cancel(deadline->cancellable);
			break;
		}

	g_mutex_unlock(&deadline->lock);

	return NULL;
}

/*
 * Copies taken while another thread commits snapshots are each the store
 * as of one commit -- every current row from one snapshot, the file whole
 * -- and they finish, however busy the writer.
 *
 * What breaks if this regresses: a backup of a store being fed is a
 * mixture of two moments -- prices from one snapshot, quantities from the
 * next -- that restores without complaint and lies about the market; or,
 * without the read transaction that pins the copy, SQLite restarts it at
 * every commit and a store fed faster than it can be read is never backed
 * up at all. The store here is thousands of pages copied one per step, so
 * the copy spans many commits.
 */
static void
test_consistent_under_writer(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = NULL;
	Writer writer;
	GThread *thread;
	gint64 deadline;
	guint copy;

	root = g_dir_make_tmp("venture-backup-series-writer-XXXXXX", &error);
	g_assert_no_error(error);

	memset(&writer, 0, sizeof writer);
	writer.directory = g_build_filename(root, "live", NULL);
	thread = g_thread_new("writer", writer_thread, &writer);

	deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

	while ((g_atomic_int_get(&writer.commits) < 3) && (NULL == writer.failure))
	{
		if (g_get_monotonic_time() > deadline)
			g_error("the writer committed nothing within 30 seconds");
		g_usleep(1000);
	}

	for (copy = 0; copy < 6; copy++)
	{
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autofree gchar *copy_dir = NULL;
		g_autofree gchar *destination = NULL;
		g_autofree gchar *sha = NULL;
		const gchar *venues[] = { "v", NULL };
		VentureSeriesFilter filter;
		VentureSeriesStoreCheck check;
		guint64 size;
		gint before;
		guint i;
		Deadline watchdog;
		GThread *watch;
		gboolean copied;

		copy_dir = g_strdup_printf("%s/copy-%u", root, copy);
		g_assert_cmpint(g_mkdir_with_parents(copy_dir, 0700), ==, 0);
		destination = g_build_filename(copy_dir, VENTURE_SERIES_STORE_FILENAME, NULL);

		before = g_atomic_int_get(&writer.commits);
		g_mutex_init(&watchdog.lock);
		g_cond_init(&watchdog.cond);
		watchdog.done = FALSE;
		watchdog.cancellable = g_cancellable_new();
		watch = g_thread_new("deadline", deadline_thread, &watchdog);

		copied = venture_series_store_backup(writer.directory, destination, 1, watchdog.cancellable,
		                                     &sha, &size, &error);

		g_mutex_lock(&watchdog.lock);
		watchdog.done = TRUE;
		g_cond_signal(&watchdog.cond);
		g_mutex_unlock(&watchdog.lock);
		g_thread_join(watch);
		g_object_unref(watchdog.cancellable);
		g_mutex_clear(&watchdog.lock);
		g_cond_clear(&watchdog.cond);

		g_assert_no_error(error);
		g_assert_true(copied);
		/* The copy spanned commits: the writer went on while it ran. */
		if (copy == 0)
			g_assert_cmpint(g_atomic_int_get(&writer.commits), >, before);
		g_assert_cmpuint(strlen(sha), ==, 64);
		g_assert_cmpuint(size, >, 0);

		g_assert_true(venture_series_store_check_file(destination, &check, &error));
		g_assert_no_error(error);
		g_assert_true(check.intact);
		g_assert_cmpint(check.current_rows, ==, WRITER_INSTRUMENTS);
		venture_series_store_check_clear(&check);

		reader = venture_series_store_open_reader(copy_dir, &error);
		g_assert_no_error(error);
		venture_series_filter_init(&filter);
		filter.venue_keys = venues;
		filter.count = 1000;
		rows = venture_series_store_list_current(reader, &filter, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(rows->len, ==, WRITER_INSTRUMENTS);

		for (i = 1; i < rows->len; i++)
			g_assert_cmpint(((VentureSeriesRow *)g_ptr_array_index(rows, i))->quantity, ==,
			                ((VentureSeriesRow *)g_ptr_array_index(rows, 0))->quantity);

		/* It was taken while the writer went on, not after it stopped. */
		g_assert_cmpint(((VentureSeriesRow *)g_ptr_array_index(rows, 0))->quantity, >=, before);
	}

	g_atomic_int_set(&writer.stop, 1);
	g_thread_join(thread);
	g_assert_null(writer.failure);
	g_assert_cmpint(g_atomic_int_get(&writer.commits), >, 3);

	/* A store that is not there is a named refusal, and leaves nothing. */
	{
		g_autofree gchar *missing = g_build_filename(root, "missing", NULL);
		g_autofree gchar *destination = g_build_filename(root, "never.sqlite", NULL);
		g_autofree gchar *partial = g_strconcat(destination, ".partial", NULL);

		g_assert_false(venture_series_store_backup(missing, destination, 0, NULL, NULL, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_clear_error(&error);
		g_assert_false(g_file_test(destination, G_FILE_TEST_EXISTS));
		g_assert_false(g_file_test(partial, G_FILE_TEST_EXISTS));
	}

	g_free(writer.directory);
	venture_test_remove_tree(root);
}

/* --- Retention ------------------------------------------------------------------ */

/*
 * Retention keeps N copies of each store, as it keeps N of the database,
 * and pruning one kind never reaches the other's files.
 *
 * What breaks if this regresses: either the disk fills with gigabyte
 * store copies nobody prunes, or one store's copies are pruned to make
 * room for another's and its history is kept nowhere.
 */
static void
test_pruning(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GPtrArray) installation = NULL;
	const gchar *const at[] = { "2026-10-01T01:00:00Z", "2026-10-02T01:00:00Z", "2026-10-03T01:00:00Z" };
	gint64 first;
	gint64 second;
	guint pruned;
	guint kept;
	guint i;

	(void)user_data;

	first = create_source(fixture, "Argent", "argent.jsonl");
	second = create_source(fixture, "Silvermoon", "silvermoon.jsonl");
	feed(fixture, first, "argent.jsonl", "2026-09-30T12:00:00Z", "1.25", 20);
	feed(fixture, second, "silvermoon.jsonl", "2026-09-30T12:00:00Z", "3.00", 4);

	schedule = make_schedule(fixture, 2);

	for (i = 0; i < G_N_ELEMENTS(at); i++)
	{
		g_autoptr(VentureEntity) run = run_backup(fixture, schedule, at[i]);

		settle(fixture);
	}

	series = runs_of_scope(fixture, "series");
	g_assert_cmpuint(series->len, ==, 6);
	pruned = 0;
	kept = 0;

	for (i = 0; i < series->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(series, i);
		g_autofree gchar *status = text(run, "status");
		g_autofree gchar *path = text(run, "path");

		if (0 == g_strcmp0(status, "pruned"))
		{
			/* The oldest copy of each store, and its file is gone. */
			g_assert_cmpuint(i, <, 2);
			g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
			pruned++;
		}
		else
		{
			g_assert_cmpstr(status, ==, "succeeded");
			g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
			kept++;
		}
	}

	g_assert_cmpuint(pruned, ==, 2);
	g_assert_cmpuint(kept, ==, 4);
	g_assert_cmpuint(count_files(fixture, "series-", ".sqlite"), ==, 4);

	/* The database copies keep their own two. */
	installation = runs_of_scope(fixture, "installation");
	g_assert_cmpuint(installation->len, ==, 3);
	g_assert_cmpuint(count_files(fixture, "installation-", ".sqlite"), ==, 2);

	/* The restore drill is about books: it picks the newest database
	 * copy, never a store copied after it. */
	{
		g_autoptr(VentureEntity) latest = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *scope = NULL;

		latest = venture_backup_schedule_service_latest_run(backups_of(fixture), fixture->org, &error);
		g_assert_no_error(error);
		scope = text(latest, "scope");
		g_assert_cmpstr(scope, ==, "installation");
	}
}

/* --- Switched off ----------------------------------------------------------------- */

/*
 * series.include_in_backup false: the database copy as before, no store
 * copied, no run recorded.
 *
 * What breaks if this regresses: an operator who keeps stores elsewhere
 * (a filesystem snapshot, a replica) and turned this off to save the disk
 * gets gigabytes written anyway on every backup.
 */
static void
test_switched_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autofree gchar *status = NULL;
	gint64 source_id;

	(void)user_data;

	source_id = create_source(fixture, "Argent", "argent.jsonl");
	feed(fixture, source_id, "argent.jsonl", "2026-10-03T12:00:00Z", "1.25", 20);
	g_object_set(fixture->config, "series-include-in-backup", FALSE, NULL);

	schedule = make_schedule(fixture, 3);
	run = run_backup(fixture, schedule, "2026-10-04T01:00:00Z");
	settle(fixture);

	status = text(run, "status");
	g_assert_cmpstr(status, ==, "succeeded");
	series = runs_of_scope(fixture, "series");
	g_assert_cmpuint(series->len, ==, 0);
	g_assert_cmpuint(count_files(fixture, "series-", NULL), ==, 0);
}

/*
 * Feeds off: the installation copy as before, and no thread, no store,
 * no run -- even with a store left on disk from when they were on.
 *
 * What breaks if this regresses: an install that never asked for market
 * data starts a worker thread on its backup, or a backup fails because a
 * module it does not have is missing.
 */
static void
test_feeds_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(VentureSeriesStore) left = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *left_dir = NULL;
	g_autofree gchar *status = NULL;

	(void)user_data;

	g_assert_null(venture_context_get_feeds_service(fixture->context));

	left_dir = venture_feeds_store_dir(fixture->config, "00000000-0000-4000-8000-000000000001");
	left = venture_series_store_open(left_dir, &error);
	g_assert_no_error(error);
	g_clear_object(&left);

	schedule = make_schedule(fixture, 3);
	run = run_backup(fixture, schedule, "2026-10-04T01:00:00Z");

	status = text(run, "status");
	g_assert_cmpstr(status, ==, "succeeded");
	series = runs_of_scope(fixture, "series");
	g_assert_cmpuint(series->len, ==, 0);
	g_assert_cmpuint(count_files(fixture, "series-", NULL), ==, 0);
	g_assert_null(venture_context_get_feeds_service(fixture->context));
}

/* --- Failures ------------------------------------------------------------------- */

/*
 * A store that cannot be copied fails on its own run, with SQLite's
 * reason; the database copy and the other store's copy still succeed, and
 * nothing half-written is left in the backup directory.
 *
 * What breaks if this regresses: one damaged store either fails the whole
 * installation backup -- the books go unbacked because of market data --
 * or fails silently, and the page shows a store as backed up that is not.
 */
static void
test_failure_is_recorded(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) main_run = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *good_uuid = NULL;
	g_autofree gchar *bad_uuid = NULL;
	g_autofree gchar *main_status = NULL;
	gint64 good;
	gint64 bad;
	guint i;

	(void)user_data;

	good = create_source(fixture, "Argent", "argent.jsonl");
	bad = create_source(fixture, "Broken", "broken.jsonl");
	feed(fixture, good, "argent.jsonl", "2026-10-03T12:00:00Z", "1.25", 20);
	feed(fixture, bad, "broken.jsonl", "2026-10-03T12:00:00Z", "1.25", 20);
	good_uuid = uuid_of(fixture, good);
	bad_uuid = uuid_of(fixture, bad);

	/* The broken store's file is replaced by something that is not a
	 * database at all. */
	{
		g_autofree gchar *store_dir = venture_feeds_store_dir(fixture->config, bad_uuid);
		g_autofree gchar *target = g_build_filename(store_dir, VENTURE_SERIES_STORE_FILENAME, NULL);
		g_autofree gchar *wal = g_strconcat(target, "-wal", NULL);
		g_autofree gchar *shm = g_strconcat(target, "-shm", NULL);

		venture_feeds_shutdown(fixture->context);
		g_unlink(wal);
		g_unlink(shm);
		g_assert_true(g_file_set_contents(target, "this is not an SQLite database, it is a "
		                                  "sentence long enough to fill a header page", -1, NULL));
	}

	schedule = make_schedule(fixture, 3);
	main_run = run_backup(fixture, schedule, "2026-10-04T01:00:00Z");
	settle(fixture);

	main_status = text(main_run, "status");
	g_assert_cmpstr(main_status, ==, "succeeded");

	series = runs_of_scope(fixture, "series");
	g_assert_cmpuint(series->len, ==, 2);

	for (i = 0; i < series->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(series, i);
		g_autofree gchar *store = text(run, "store-uuid");
		g_autofree gchar *status = text(run, "status");
		g_autofree gchar *message = text(run, "message");
		g_autofree gchar *path = text(run, "path");

		if (0 == g_strcmp0(store, bad_uuid))
		{
			g_assert_cmpstr(status, ==, "failed");
			g_assert_nonnull(message);
			g_assert_nonnull(strstr(message, "Backing up"));
			g_assert_true(venture_string_is_empty(path));

			/* A failed copy is not something to verify. */
			g_assert_false(venture_backup_schedule_service_verify(backups_of(fixture),
			                                                      VENTURE_BACKUP_RUN(run), NULL,
			                                                      &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
			g_clear_error(&error);
		}
		else
		{
			g_assert_cmpstr(store, ==, good_uuid);
			g_assert_cmpstr(status, ==, "succeeded");
			g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));
		}
	}

	g_assert_cmpuint(count_files(fixture, "series-", ".sqlite"), ==, 1);
}

/*
 * A copy recorded as running that nobody will answer -- the server
 * stopped while it was queued -- is marked interrupted by the next
 * backup; and a copy still in flight when the next backup comes is left
 * to finish, the new one recorded as skipped rather than run twice.
 *
 * What breaks if this regresses: the Backups page shows a store copy
 * "running" for ever after a restart, and back-to-back backups of a
 * large store start two gigabyte copies of it at once.
 */
static void
test_interrupted_and_overlapping(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) orphan = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) third = NULL;
	g_autoptr(VentureEntity) fresh = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *message = NULL;
	VentureActor actor;
	gint64 source_id;
	guint skipped;
	guint i;

	(void)user_data;

	source_id = create_source(fixture, "Argent", "argent.jsonl");
	feed(fixture, source_id, "argent.jsonl", "2026-10-03T12:00:00Z", "1.25", 20);
	uuid = uuid_of(fixture, source_id);
	schedule = make_schedule(fixture, 10);

	first = run_backup(fixture, schedule, "2026-10-04T01:00:00Z");
	settle(fixture);

	/* A copy the previous process queued and never answered. */
	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "test";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	orphan = venture_backup_schedule_service_begin_companion(backups_of(fixture), VENTURE_BACKUP_RUN(first),
	                                                         "series", "Series store: lost", uuid,
	                                                         &actor, &error);
	g_assert_no_error(error);

	/* Two backups with no turn of the main loop between: the first's copy
	 * cannot have been answered when the second looks. */
	second = run_backup(fixture, schedule, "2026-10-04T02:00:00Z");
	third = run_backup(fixture, schedule, "2026-10-04T03:00:00Z");
	settle(fixture);

	fresh = reread(fixture, orphan);
	status = text(fresh, "status");
	message = text(fresh, "message");
	g_assert_cmpstr(status, ==, "failed");
	g_assert_nonnull(strstr(message, "interrupted"));

	series = runs_of_scope(fixture, "series");
	skipped = 0;

	for (i = 0; i < series->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(series, i);
		g_autofree gchar *run_status = text(run, "status");
		g_autofree gchar *run_message = text(run, "message");

		g_assert_cmpstr(run_status, !=, "running");

		if (number(run, "parent-run-id") == venture_entity_get_id(third))
		{
			g_assert_cmpstr(run_status, ==, "failed");
			g_assert_nonnull(strstr(run_message, "still running"));
			skipped++;
		}
		else if (number(run, "parent-run-id") == venture_entity_get_id(second))
			g_assert_cmpstr(run_status, ==, "succeeded");
	}

	g_assert_cmpuint(skipped, ==, 1);

	/* An answer cannot land twice. */
	g_assert_false(venture_backup_schedule_service_finish_companion(backups_of(fixture),
	                                                                venture_entity_get_id(orphan),
	                                                                NULL, NULL, 0, "again", &actor,
	                                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

/* --- A copy does not hold the worker ----------------------------------------- */

/* The status of the series run for @uuid. */
static gchar *
series_status(
	Fixture		*fixture,
	const gchar	*uuid
){
	g_autoptr(GPtrArray) series = runs_of_scope(fixture, "series");
	guint i;

	for (i = 0; i < series->len; i++)
	{
		g_autofree gchar *store = text(g_ptr_array_index(series, i), "store-uuid");

		if (0 == g_strcmp0(store, uuid))
			return text(g_ptr_array_index(series, i), "status");
	}

	return NULL;
}

static gint64
count_source_runs(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
	g_autoptr(GError) error = NULL;
	gint64 count;

	g_assert_true(venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ,
	                                           source_id, &error));
	count = venture_database_count(fixture->database, query, &error);
	g_assert_no_error(error);

	return count;
}

/*
 * A store copy runs beside the worker's loop, not on it. One store is held
 * locked by another connection, so its copy waits in SQLite's busy handler;
 * meanwhile a sync of another source is asked for, fetched and recorded.
 * When the lock goes, the copy finishes and is recorded too.
 *
 * What breaks if this regresses: copying a large store -- or one that is
 * busy -- holds every fetch and every command for as long as it takes, so
 * a sync someone is waiting for, or a schedule, stalls until a backup is
 * done.
 */
static void
test_sync_during_copy(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) schedule = NULL;
	g_autoptr(VentureEntity) main_run = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *held_uuid = NULL;
	g_autofree gchar *held_dir = NULL;
	g_autofree gchar *held_path = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *path = NULL;
	sqlite3 *lock;
	sqlite3 *probe;
	gint64 held;
	gint64 other;
	gint64 runs_before;
	gint64 deadline;

	(void)user_data;

	held = create_source(fixture, "Argent", "argent.jsonl");
	feed(fixture, held, "argent.jsonl", "2026-10-03T12:00:00Z", "1.25", 20);
	other = create_source(fixture, "Brisk", "brisk.jsonl");
	feed(fixture, other, "brisk.jsonl", "2026-10-03T12:00:00Z", "2.50", 4);
	held_uuid = uuid_of(fixture, held);

	/* Off the worker -- deleted sources' stores are still copied -- so
	 * nothing of the worker's has the file open, and locked by a
	 * connection that never lets go until told. */
	record = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, held, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(fixture->database, record, NULL, &error));
	g_assert_no_error(error);
	venture_feeds_service_refresh(service_of(fixture));
	settle(fixture);

	held_dir = venture_feeds_store_dir(fixture->config, held_uuid);
	held_path = g_build_filename(held_dir, VENTURE_SERIES_STORE_FILENAME, NULL);
	g_assert_cmpint(sqlite3_open(held_path, &lock), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(lock, "PRAGMA locking_mode = EXCLUSIVE; BEGIN EXCLUSIVE; COMMIT;",
	                             NULL, NULL, NULL), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_open(held_path, &probe), ==, SQLITE_OK);
	g_assert_cmpint(sqlite3_exec(probe, "SELECT count(*) FROM sqlite_schema", NULL, NULL, NULL),
	                ==, SQLITE_BUSY);
	sqlite3_close(probe);

	schedule = make_schedule(fixture, 3);
	main_run = run_backup(fixture, schedule, "2026-10-04T01:00:00Z");

	/* A sync asked for after the copies were queued is recorded while
	 * the held copy is still waiting. */
	runs_before = count_source_runs(fixture, other);
	path = g_build_filename(fixture->file_root, "brisk.jsonl", NULL);
	g_assert_true(g_file_set_contents(path,
		"{\"type\":\"venue\",\"key\":\"argent\",\"name\":\"Argent Dawn\",\"currency\":\"GOLD\"}\n"
		"{\"type\":\"instrument\",\"key\":\"ore\",\"name\":\"Iron ore\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"taken_at\":\"2026-10-03T13:00:00Z\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"2.40\",\"quantity\":3,\"id\":\"b2\"}\n",
		-1, NULL));
	g_assert_true(venture_feeds_service_sync(service_of(fixture), other,
	                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
	g_assert_no_error(error);

	deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

	while (count_source_runs(fixture, other) == runs_before)
	{
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	status = series_status(fixture, held_uuid);
	g_assert_cmpstr(status, ==, "running");
	g_clear_pointer(&status, g_free);

	/* Let go: the copy finishes and is recorded. */
	sqlite3_close(lock);
	settle(fixture);
	status = series_status(fixture, held_uuid);
	g_assert_cmpstr(status, ==, "succeeded");
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/backup-series/consistent-under-writer", test_consistent_under_writer);
	g_test_add("/backup-series/round-trip", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up,
	           test_round_trip, fixture_tear_down);
	g_test_add("/backup-series/pruning", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up,
	           test_pruning, fixture_tear_down);
	g_test_add("/backup-series/switched-off", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up,
	           test_switched_off, fixture_tear_down);
	g_test_add("/backup-series/feeds-off", Fixture, GINT_TO_POINTER(FALSE), fixture_set_up,
	           test_feeds_off, fixture_tear_down);
	g_test_add("/backup-series/failure-is-recorded", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up,
	           test_failure_is_recorded, fixture_tear_down);
	g_test_add("/backup-series/sync-during-copy", Fixture, GINT_TO_POINTER(TRUE), fixture_set_up,
	           test_sync_during_copy, fixture_tear_down);
	g_test_add("/backup-series/interrupted-and-overlapping", Fixture, GINT_TO_POINTER(TRUE),
	           fixture_set_up, test_interrupted_and_overlapping, fixture_tear_down);

	return g_test_run();
}

#else /* !VENTURE_HAVE_SQLITE */

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	return g_test_run();
}

#endif /* VENTURE_HAVE_SQLITE */
