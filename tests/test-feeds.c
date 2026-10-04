/*
 * test-feeds.c - Market data feeds: providers, the worker, the runs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The worker is a real thread with a real HTTP session, so these tests
 * talk to a scripted HTTP server on a thread of its own, bound to port 0,
 * and wait for runs the way a person would: by letting the main loop take
 * them back, bounded, until nothing is pending. Every fixture gets its own
 * state directory -- the stores live there -- and removes it with
 * venture_test_remove_tree().
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif

#include <venture.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

/* A build without SQLite has no series store and no feeds worker. */
#ifdef VENTURE_HAVE_SQLITE

/* --- A scripted HTTP server -------------------------------------------------------- */

typedef struct
{
	guint		 status;
	gchar		*body;
	gsize		 body_length;	/* 0: strlen(body) */
	gchar		*last_modified;
	gchar		*retry_after;
	gchar		*location;
	guint		 delay_ms;
	gboolean	 not_modified_when_asked;
	gboolean	 echo_authorization;
} Script;

typedef struct
{
	GMutex		 lock;
	GCond		 ready;
	GMainContext	*context;
	GMainLoop	*loop;
	GThread		*thread;
	gchar		*origin;
	gchar		*neighbor;
	GHashTable	*scripts;	/* path -> Script */
	GHashTable	*hits;		/* path -> count */
	gchar		*last_if_modified_since;
	gchar		*last_authorization;
	gchar		*last_query;
	gint		 neighbor_hits;
} Scripted;

static void
script_free(gpointer data)
{
	Script *script = data;

	g_free(script->body);
	g_free(script->last_modified);
	g_free(script->retry_after);
	g_free(script->location);
	g_free(script);
}

typedef struct
{
	SoupServerMessage	*message;
	SoupServer		*server;
} Delayed;

static gboolean
scripted_unpause(gpointer data)
{
	Delayed *delayed = data;

	soup_server_message_unpause(delayed->message);
	g_object_unref(delayed->message);
	g_free(delayed);

	return G_SOURCE_REMOVE;
}

static void
scripted_neighbor(
	SoupServer		*server,
	SoupServerMessage	*message,
	const gchar		*path,
	GHashTable		*query,
	gpointer		 data
){
	Scripted *scripted = data;

	(void)server;
	(void)path;
	(void)query;

	g_atomic_int_inc(&scripted->neighbor_hits);
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_STATIC, "[]", 2);
}

static void
scripted_handle(
	SoupServer		*server,
	SoupServerMessage	*message,
	const gchar		*path,
	GHashTable		*query,
	gpointer		 data
){
	Scripted *scripted = data;
	SoupMessageHeaders *request;
	SoupMessageHeaders *response;
	Script *script;
	const gchar *ims;
	g_autofree gchar *body = NULL;
	gsize length;
	gint *hits;

	(void)query;

	request = soup_server_message_get_request_headers(message);
	response = soup_server_message_get_response_headers(message);
	ims = soup_message_headers_get_one(request, "If-Modified-Since");

	g_mutex_lock(&scripted->lock);
	script = g_hash_table_lookup(scripted->scripts, path);
	hits = g_hash_table_lookup(scripted->hits, path);

	if (NULL == hits)
	{
		hits = g_new0(gint, 1);
		g_hash_table_insert(scripted->hits, g_strdup(path), hits);
	}

	(*hits)++;
	g_free(scripted->last_if_modified_since);
	scripted->last_if_modified_since = g_strdup(ims);
	g_free(scripted->last_authorization);
	scripted->last_authorization = g_strdup(soup_message_headers_get_one(request, "Authorization"));
	g_free(scripted->last_query);
	scripted->last_query = g_strdup(g_uri_get_query(soup_server_message_get_uri(message)));

	if (NULL == script)
	{
		g_mutex_unlock(&scripted->lock);
		soup_server_message_set_status(message, 404, NULL);
		return;
	}

	if (script->not_modified_when_asked && (NULL != ims))
	{
		g_mutex_unlock(&scripted->lock);
		soup_server_message_set_status(message, 304, NULL);
		return;
	}

	if (script->echo_authorization)
		body = g_strdup_printf("{\"items\":[{\"id\":\"x\",\"price\":\"1.00\",\"name\":\"%s\"}]}",
		                       (NULL != scripted->last_authorization) ? scripted->last_authorization : "");
	else
		body = g_memdup2(script->body, (script->body_length > 0) ? script->body_length
		                                                         : strlen(script->body) + 1);

	length = script->echo_authorization ? strlen(body)
	       : (script->body_length > 0) ? script->body_length : strlen(script->body);

	if (NULL != script->last_modified)
		soup_message_headers_replace(response, "Last-Modified", script->last_modified);

	if (NULL != script->retry_after)
		soup_message_headers_replace(response, "Retry-After", script->retry_after);

	if (NULL != script->location)
		soup_message_headers_replace(response, "Location", script->location);

	soup_server_message_set_status(message, script->status, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_TAKE,
	                                 g_steal_pointer(&body), length);

	if (script->delay_ms > 0)
	{
		Delayed *delayed = g_new0(Delayed, 1);
		GSource *source;

		delayed->message = g_object_ref(message);
		delayed->server = server;
		soup_server_message_pause(message);
		source = g_timeout_source_new(script->delay_ms);
		g_source_set_callback(source, scripted_unpause, delayed, NULL);
		g_source_attach(source, scripted->context);
		g_source_unref(source);
	}

	g_mutex_unlock(&scripted->lock);
}

static gchar *
scripted_listen(SoupServer *server)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;
	gchar *origin;

	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	origin = g_uri_to_string_partial(uris->data, G_URI_HIDE_PASSWORD);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	/* "http://127.0.0.1:PORT/" -> "http://127.0.0.1:PORT" */
	if (g_str_has_suffix(origin, "/"))
		origin[strlen(origin) - 1] = '\0';

	return origin;
}

/* Only the synthetic listeners run here; nothing of the application's
 * crosses onto this thread. */
static gpointer
scripted_thread(gpointer data)
{
	Scripted *scripted = data;
	g_autoptr(SoupServer) server = NULL;
	g_autoptr(SoupServer) neighbor = NULL;

	g_main_context_push_thread_default(scripted->context);
	server = soup_server_new(NULL, NULL);
	neighbor = soup_server_new(NULL, NULL);
	soup_server_add_handler(server, NULL, scripted_handle, scripted, NULL);
	soup_server_add_handler(neighbor, NULL, scripted_neighbor, scripted, NULL);

	g_mutex_lock(&scripted->lock);
	scripted->neighbor = scripted_listen(neighbor);
	scripted->origin = scripted_listen(server);
	g_cond_signal(&scripted->ready);
	g_mutex_unlock(&scripted->lock);

	g_main_loop_run(scripted->loop);

	soup_server_disconnect(server);
	soup_server_disconnect(neighbor);
	g_main_context_pop_thread_default(scripted->context);

	return NULL;
}

static void
scripted_start(Scripted *scripted)
{
	gint64 deadline;

	g_mutex_init(&scripted->lock);
	g_cond_init(&scripted->ready);
	scripted->context = g_main_context_new();
	scripted->loop = g_main_loop_new(scripted->context, FALSE);
	scripted->scripts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, script_free);
	scripted->hits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	g_mutex_lock(&scripted->lock);
	scripted->thread = g_thread_new("feeds-http-fixture", scripted_thread, scripted);
	deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

	while (NULL == scripted->origin)
		g_assert_true(g_cond_wait_until(&scripted->ready, &scripted->lock, deadline));

	g_mutex_unlock(&scripted->lock);
}

static gboolean
scripted_quit(gpointer data)
{
	g_main_loop_quit(data);

	return G_SOURCE_REMOVE;
}

static void
scripted_stop(Scripted *scripted)
{
	if (NULL == scripted->thread)
		return;

	g_main_context_invoke(scripted->context, scripted_quit, scripted->loop);
	g_thread_join(scripted->thread);
	scripted->thread = NULL;
	g_main_loop_unref(scripted->loop);
	g_main_context_unref(scripted->context);
	g_hash_table_unref(scripted->scripts);
	g_hash_table_unref(scripted->hits);
	g_free(scripted->origin);
	g_free(scripted->neighbor);
	g_free(scripted->last_if_modified_since);
	g_free(scripted->last_authorization);
	g_free(scripted->last_query);
	g_cond_clear(&scripted->ready);
	g_mutex_clear(&scripted->lock);
}

static Script *
scripted_add(
	Scripted	*scripted,
	const gchar	*path,
	guint		 status,
	const gchar	*body
){
	Script *script;

	script = g_new0(Script, 1);
	script->status = status;
	script->body = g_strdup((NULL != body) ? body : "");

	g_mutex_lock(&scripted->lock);
	g_hash_table_replace(scripted->scripts, g_strdup(path), script);
	g_mutex_unlock(&scripted->lock);

	/* Scripts are set up before any request is made, so handing back the
	 * pointer for a caller to adjust is safe. */
	return script;
}

static gint
scripted_hits(
	Scripted	*scripted,
	const gchar	*path
){
	gint *hits;
	gint count;

	g_mutex_lock(&scripted->lock);
	hits = g_hash_table_lookup(scripted->hits, path);
	count = (NULL != hits) ? *hits : 0;
	g_mutex_unlock(&scripted->lock);

	return count;
}

/* --- The fixture ------------------------------------------------------------------- */

typedef struct
{
	gchar			*state_dir;
	gchar			*file_root;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	gint64			 org;
	Scripted		 http;
} Fixture;

/* An auction answer: one realm's listings, in minor units like Blizzard's. */
static const gchar auction_body[] =
	"{\"auctions\":["
	"{\"id\":101,\"item\":{\"id\":2770,\"name\":\"Copper ore\"},\"buyout\":\"1.25\",\"quantity\":10},"
	"{\"id\":102,\"item\":{\"id\":2770,\"name\":\"Copper ore\"},\"buyout\":\"1.40\",\"quantity\":5},"
	"{\"id\":103,\"item\":{\"id\":2447,\"name\":\"Peacebloom\"},\"buyout\":0.75,\"quantity\":12}"
	"]}";

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-feeds-XXXXXX", &error);
	g_assert_no_error(error);
	fixture->file_root = g_build_filename(fixture->state_dir, "files", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->file_root, 0700), ==, 0);

	scripted_start(&fixture->http);

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "feeds-enabled", TRUE,
	             "feeds-allowed-origins", fixture->http.origin,
	             "feeds-file-roots", fixture->file_root,
	             "feeds-run-window-minutes", (gint64)0,
	             "feeds-request-timeout", (gint64)5,
	             "feeds-max-response-mb", (gint64)1,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* The registry is process-wide and a test before this one may have
	 * masked the module; with this context's configuration applied, the
	 * feeds tables are created now. */
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_assert_true(venture_integration_service_set_key(
		venture_integration_service_get(fixture->database), key, &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	scripted_stop(&fixture->http);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_clear_pointer(&fixture->file_root, g_free);
}

static VentureFeedsService *
service_of(Fixture *fixture)
{
	VentureFeedsService *service;

	service = venture_context_get_feeds_service(fixture->context);
	g_assert_nonnull(service);

	return service;
}

/*
 * Lets the main loop take runs back until nothing is pending, bounded: a
 * test that can hang tells less than one that fails.
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

static gint64
create_source(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*provider,
	const gchar	*settings,
	const gchar	*schedule
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", name, "provider", provider, "settings", settings,
	             "schedule", schedule, "currency", "USD", NULL);
	if (!venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error))
		g_error("%s: %s", name, error->message);

	return venture_entity_get_id(VENTURE_ENTITY(source));
}

static VentureEntity *
get_source(
	Fixture	*fixture,
	gint64	 id
){
	g_autoptr(GError) error = NULL;
	VentureEntity *source;

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(source);

	return source;
}

/* The source's newest run, or NULL. */
static VentureEntity *
latest_run(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GError) error = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
	g_assert_true(venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ,
	                                           source_id, &error));
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	runs = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);

	return (runs->len > 0) ? g_object_ref(g_ptr_array_index(runs, 0)) : NULL;
}

static gint64
count_runs(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	gint64 count;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
	g_assert_true(venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ,
	                                           source_id, &error));
	count = venture_database_count(fixture->database, query, &error);
	g_assert_no_error(error);

	return count;
}

/* Syncs and waits for the run it makes. */
static VentureEntity *
sync_and_wait(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(GError) error = NULL;
	VentureEntity *run;

	g_assert_true(venture_feeds_service_sync(service_of(fixture), source_id,
	                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
	g_assert_no_error(error);
	settle(fixture);

	run = latest_run(fixture, source_id);
	g_assert_nonnull(run);

	return run;
}

static gint64
run_int(
	VentureEntity	*run,
	const gchar	*field
){
	gint64 value;

	g_object_get(run, field, &value, NULL);

	return value;
}

static VentureDataSourceRunStatus
run_status(VentureEntity *run)
{
	VentureDataSourceRunStatus status;

	g_object_get(run, "status", &status, NULL);

	return status;
}

static gchar *
run_text(
	VentureEntity	*run,
	const gchar	*field
){
	gchar *value = NULL;

	g_object_get(run, field, &value, NULL);

	return value;
}

static gint64
store_rows(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesFilter filter;
	gint64 count = 0;

	reader = venture_feeds_service_open_reader(service_of(fixture), source_id, &error);

	if (NULL == reader)
		return -1;

	venture_series_filter_init(&filter);
	g_assert_true(venture_series_store_count_current(reader, &filter, &count, &error));
	g_assert_no_error(error);

	return count;
}

static gchar *
http_settings(
	Fixture		*fixture,
	const gchar	*path,
	const gchar	*extra
){
	return g_strdup_printf("url: %s%s\n"
	                       "items: auctions\n"
	                       "fields:\n"
	                       "  instrument: item.id\n"
	                       "  name: item.name\n"
	                       "  price: buyout\n"
	                       "  quantity: quantity\n"
	                       "  id: id\n"
	                       "%s", fixture->http.origin, path, (NULL != extra) ? extra : "");
}

static gchar *
write_root_file(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*contents
){
	gchar *path;

	path = g_build_filename(fixture->file_root, name, NULL);
	g_assert_true(g_file_set_contents(path, contents, -1, NULL));

	return path;
}

/* --- Records and the module -------------------------------------------------------- */

/*
 * The module is opt-in, and a new source starts enabled.
 *
 * What breaks if this regresses: an install that never asked for feeds
 * would start calling outside hosts, and a source added through the form
 * would sit switched off with nothing on the page to say why.
 */
static void
test_feeds_module_is_opt_in(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;
	gboolean enabled;

	config = venture_config_new();
	database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, database);

	g_assert_false(venture_context_module_enabled(context, "feeds"));
	g_assert_null(venture_context_get_feeds_service(context));

	source = venture_data_source_new();
	g_object_get(source, "enabled", &enabled, NULL);
	g_assert_true(enabled);

	/* With the module off its records are hidden like any module's, and
	 * trying to save a scheduled source starts nothing. */
	venture_entity_set_organization_id(VENTURE_ENTITY(source),
	                                   venture_context_get_default_organization_id(context));
	g_object_set(source, "name", "Ignored", "provider", "http_json", "schedule", "auto", NULL);
	g_assert_false(venture_database_save(database, VENTURE_ENTITY(source), NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	while (g_main_context_iteration(NULL, FALSE))
		;

	g_assert_null(venture_context_get_feeds_service(context));
	g_assert_null(g_object_get_data(G_OBJECT(context), "venture-feeds-service"));
}

/*
 * A source's settings are checked against its provider at the save.
 *
 * What breaks if this regresses: a credential typed into the settings
 * would be stored in the clear on a record every viewer reads; a typo in
 * the schedule would leave a source that never runs and never says so.
 */
static void
test_feeds_settings_are_validated(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	struct
	{
		const gchar	*provider;
		const gchar	*settings;
		const gchar	*schedule;
		const gchar	*problem;
	} refused[] = {
		{ "nope", "", "manual", "No data source provider named nope" },
		{ "http_json", "url: x\nfields: {instrument: id}\ntoken: abcdef", "manual", "credential" },
		{ "http_json", "fields: {instrument: id}", "manual", "needs the setting url" },
		{ "http_json", "url: x\nfields: {instrument: id}", "every so often", "schedule" },
		{ "http_json", "url: x\nfields: {instrument: id}\ncost: many", "manual", "cost must be an integer" },
		{ "http_json", "url: x\nfields: {instrument: id}\nrecord_types: [invoice]", "manual", "cannot write invoice" },
		{ "file_jsonl", "- a\n- b", "manual", "mapping" },
	};
	g_autoptr(GError) error = NULL;
	guint i;

	(void)user_data;

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autoptr(VentureDataSource) source = venture_data_source_new();
		g_autoptr(GError) save_error = NULL;

		venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
		g_object_set(source, "name", "Bad", "provider", refused[i].provider,
		             "settings", refused[i].settings, "schedule", refused[i].schedule, NULL);
		g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &save_error));
		g_assert_nonnull(save_error);

		if (NULL == strstr(save_error->message, refused[i].problem))
			g_error("case %u: \"%s\" does not mention \"%s\"", i, save_error->message,
			        refused[i].problem);
	}

	/* track: known with nothing known keeps nothing, and says so -- with
	 * marketdata off. With it on, the source's instrument records are
	 * known too, and they can only be made once the source exists, so an
	 * empty list is accepted (test-marketdata covers that half). */
	{
		g_autoptr(VentureDataSource) source = venture_data_source_new();

		venture_config_set_module_enabled(fixture->config, "marketdata", FALSE);
		venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
		g_object_set(source, "name", "Known", "provider", "file_jsonl", "settings", "file: x.jsonl",
		             "track", VENTURE_DATA_SOURCE_TRACK_KNOWN, NULL);
		g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
		venture_config_set_module_enabled(fixture->config, "marketdata", TRUE);
	}

	/* The uuid names the store's directory, and the generic API writes
	 * it: "../escape" would make, purge and remove a store outside
	 * series/, or open another source's. */
	{
		g_autoptr(VentureDataSource) source = venture_data_source_new();

		venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
		g_object_set(source, "name", "Escape", "provider", "file_jsonl", "settings", "file: x.jsonl",
		             "uuid", "../escape", NULL);
		g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

	/* And a good one saves, with a cron schedule. */
	create_source(fixture, "Good", "file_jsonl", "file: x.jsonl", "15 * * * *");
}

/* --- A provider that ignores its cancellable ---------------------------------------- */

static gint stuck_entered = 0;
static gint stuck_release = 0;

/* Blocks until released (or thirty seconds), whatever the cancellable says. */
static VentureFeedBatch *
stuck_fetch(
	VentureFeedRequest	 *request,
	gpointer		  user_data,
	GError			**error
){
	gint64 until;

	(void)request;
	(void)user_data;

	until = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
	g_atomic_int_set(&stuck_entered, 1);

	while (!g_atomic_int_get(&stuck_release) && (g_get_monotonic_time() < until))
		g_usleep(10000);

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "released");

	return NULL;
}

/*
 * Stopping the worker is bounded even while a provider ignores its
 * cancellable. The ten-second wait for in-flight fetches was a blocking
 * iteration with nothing to wake it, so a stuck fetch held stop() -- and
 * the main thread joining it, from context dispose or a request turning
 * feeds off -- forever. What breaks if this regresses: switching feeds
 * off hangs the server.
 */
static void
test_feeds_stop_is_bounded(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDataSourceProvider) provider = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 deadline;
	gint64 started;

	(void)user_data;

	g_atomic_int_set(&stuck_entered, 0);
	g_atomic_int_set(&stuck_release, 0);
	provider = venture_func_data_source_provider_new("stuck", "Stuck", NULL, stuck_fetch, NULL, NULL);
	g_assert_true(venture_data_source_provider_registry_add(
		venture_context_get_data_source_providers(fixture->context), provider, &error));
	g_assert_no_error(error);

	id = create_source(fixture, "Stuck", "stuck", "", "manual");
	g_assert_true(venture_feeds_service_sync(service_of(fixture), id,
	                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
	g_assert_no_error(error);

	deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

	while (!g_atomic_int_get(&stuck_entered))
	{
		if (g_get_monotonic_time() > deadline)
			g_error("the stuck provider was never called");

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	started = g_get_monotonic_time();
	venture_feeds_shutdown(fixture->context);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 15 * G_TIME_SPAN_SECOND);

	/* Let the provider's thread end before the fixture goes. */
	g_atomic_int_set(&stuck_release, 1);
	g_usleep(100000);
}

/*
 * A feeds.allowed_origins entry that can never match, or a relative
 * feeds.file_roots entry, refuses the configuration and names the entry,
 * with any password masked; it used to be skipped, and every fetch was
 * denied as if the entry had never been written. A store directory stays
 * under series/ whatever uuid reaches it.
 */
static void
test_feeds_config_is_validated(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *series = NULL;

	g_object_set(config, "feeds-allowed-origins", "https://ok.example, https://[::1]:8443,", "feeds-file-roots",
	             "/srv/feeds,", NULL);
	g_assert_true(venture_feeds_validate_config(config, &error));
	g_assert_no_error(error);

	g_object_set(config, "feeds-allowed-origins", "https://ok.example,http://remote.example", NULL);
	g_assert_false(venture_feeds_validate_config(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "http://remote.example"));
	g_clear_error(&error);

	g_object_set(config, "feeds-allowed-origins", "https://user:hunter2@api.example", NULL);
	g_assert_false(venture_feeds_validate_config(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_null(strstr(error->message, "hunter2"));
	g_clear_error(&error);

	g_object_set(config, "feeds-allowed-origins", "https://api.example/v1", NULL);
	g_assert_false(venture_feeds_validate_config(config, &error));
	g_clear_error(&error);

	g_object_set(config, "feeds-allowed-origins", "", "feeds-file-roots", "build/demo/market", NULL);
	g_assert_false(venture_feeds_validate_config(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "feeds.file_roots"));
	g_clear_error(&error);

	g_object_set(config, "state-dir", "/var/lib/venture", NULL);
	escaped = venture_feeds_store_dir(config, "../../etc");
	series = g_build_filename("/var/lib/venture", "series", "", NULL);
	g_assert_true(g_str_has_prefix(escaped, series));
	g_assert_null(strstr(escaped, ".."));
	state = venture_feeds_store_dir(config, "6b1a2c1e-3f1d-4a8e-9a77-5c3b2e1d0f9a");
	g_assert_cmpstr(state, ==, "/var/lib/venture/series/6b1a2c1e-3f1d-4a8e-9a77-5c3b2e1d0f9a");
}

/* --- Exact conversions ---------------------------------------------------------------- */

/*
 * Prices cross into minor units exactly, and odds into the store's scale.
 *
 * What breaks if this regresses: a price with a tenth of a cent rounded
 * into the store is a price nobody quoted, and odds read through a double
 * drift a millionth at a time into a surebet that is not there.
 */
static void
test_feeds_exact_conversions(void)
{
	gint64 value;

	g_assert_true(venture_feed_decimal_to_minor("12.50", "USD", &value, NULL));
	g_assert_cmpint(value, ==, 1250);
	g_assert_true(venture_feed_decimal_to_minor("12.5", "USD", &value, NULL));
	g_assert_cmpint(value, ==, 1250);
	g_assert_true(venture_feed_decimal_to_minor("-3", "USD", &value, NULL));
	g_assert_cmpint(value, ==, -300);
	g_assert_false(venture_feed_decimal_to_minor("12.505", "USD", &value, NULL));
	g_assert_true(venture_feed_decimal_to_minor("100", "JPY", &value, NULL));
	g_assert_cmpint(value, ==, 100);
	g_assert_false(venture_feed_decimal_to_minor("100.5", "JPY", &value, NULL));
	g_assert_false(venture_feed_decimal_to_minor("1e3", "USD", &value, NULL));
	g_assert_false(venture_feed_decimal_to_minor("99999999999999999999", "USD", &value, NULL));
	g_assert_false(venture_feed_decimal_to_minor("1.00", NULL, &value, NULL));

	g_assert_true(venture_feed_decimal_to_odds("2.50", NULL, &value, NULL));
	g_assert_cmpint(value, ==, 2500000);
	g_assert_true(venture_feed_decimal_to_odds("+150", "american", &value, NULL));
	g_assert_cmpint(value, ==, 2500000);
	g_assert_true(venture_feed_decimal_to_odds("-200", "american", &value, NULL));
	g_assert_cmpint(value, ==, 1500000);
	g_assert_true(venture_feed_decimal_to_odds("5/2", "fractional", &value, NULL));
	g_assert_cmpint(value, ==, 3500000);
	/* 1/3 rounds half to even at the sixth place. */
	g_assert_true(venture_feed_decimal_to_odds("1/3", "fractional", &value, NULL));
	g_assert_cmpint(value, ==, 1333333);
	g_assert_false(venture_feed_decimal_to_odds("1.0", NULL, &value, NULL));
	g_assert_false(venture_feed_decimal_to_odds("+50", "american", &value, NULL));
}

/*
 * A listing's time left travels in the protocol and reaches the store.
 *
 * What breaks if this regresses: without expires_in_min every listing
 * that vanished between snapshots is counted as sold, and the sale rate
 * reads as everything selling.
 */
static void
test_feeds_listing_expiry_in_the_protocol(void)
{
	g_autoptr(VentureJsonlMessage) good = NULL;
	g_autoptr(VentureJsonlMessage) bad = NULL;
	g_autoptr(GError) error = NULL;

	good = venture_jsonl_message_parse("{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\","
	                                   "\"price\":\"1.00\",\"expires_in_min\":30}", -1, 1, &error);
	g_assert_no_error(error);
	g_assert_nonnull(good);
	g_assert_cmpint(venture_jsonl_message_get_int(good, "expires_in_min", -1), ==, 30);

	bad = venture_jsonl_message_parse("{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\","
	                                  "\"price\":\"1.00\",\"expires_in_min\":-5}", -1, 2, &error);
	g_assert_null(bad);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION);
}

/* --- Scheduling -------------------------------------------------------------------- */

/*
 * The adaptive schedule is Undermine's: 45 seconds early after new data,
 * then 1, 5, 15 and 30 minutes when nothing new comes, never sooner than a
 * Retry-After.
 *
 * What breaks if this regresses: a source polled every minute all hour,
 * which is what gets a key revoked, or a venue that updated and was not
 * looked at for half an hour.
 */
static void
test_feeds_adaptive_interval(void)
{
	const gint64 now = 1790000000;
	guint step = 2;
	gint64 next;

	next = venture_feeds_schedule_next_auto(TRUE, now + 3600, 0, &step, now);
	g_assert_cmpint(next, ==, now + 3600 - VENTURE_FEEDS_CHECK_EARLY);
	g_assert_cmpuint(step, ==, 0);

	/* Expected already: late, so the first back-off step. */
	next = venture_feeds_schedule_next_auto(TRUE, now - 10, 0, &step, now);
	g_assert_cmpint(next, ==, now + 60);

	/* Nothing learned yet: an hour. */
	next = venture_feeds_schedule_next_auto(TRUE, VENTURE_SERIES_NONE, 0, &step, now);
	g_assert_cmpint(next, ==, now + VENTURE_SERIES_INTERVAL_DEFAULT);

	/* Nothing new, again and again: 1, 5, 15, 30, 30 minutes. */
	step = 0;
	g_assert_cmpint(venture_feeds_schedule_next_auto(FALSE, VENTURE_SERIES_NONE, 0, &step, now), ==, now + 60);
	g_assert_cmpint(venture_feeds_schedule_next_auto(FALSE, VENTURE_SERIES_NONE, 0, &step, now), ==, now + 300);
	g_assert_cmpint(venture_feeds_schedule_next_auto(FALSE, VENTURE_SERIES_NONE, 0, &step, now), ==, now + 900);
	g_assert_cmpint(venture_feeds_schedule_next_auto(FALSE, VENTURE_SERIES_NONE, 0, &step, now), ==, now + 1800);
	g_assert_cmpint(venture_feeds_schedule_next_auto(FALSE, VENTURE_SERIES_NONE, 0, &step, now), ==, now + 1800);

	/* A Retry-After is never cut short, even after new data. */
	step = 0;
	g_assert_cmpint(venture_feeds_schedule_next_auto(TRUE, now + 30, 600, &step, now), ==, now + 600);
}

/* --- HTTP ---------------------------------------------------------------------------- */

/*
 * A 200 is stored: the venue the unit names, the items as instruments,
 * the listings as a snapshot dated by Last-Modified.
 *
 * What breaks if this regresses: the whole module. Also that a JSON
 * number price (0.75) is read as the decimal it was written as, and that
 * Last-Modified, not the fetch, dates the snapshot.
 */
static void
test_feeds_http_ok(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(VentureSeriesRow) row = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	Script *script;
	gint64 id;

	(void)user_data;

	script = scripted_add(&fixture->http, "/realm/eu-1.json", 200, auction_body);
	script->last_modified = g_strdup("Sat, 03 Oct 2026 10:00:00 GMT");
	settings = http_settings(fixture, "/realm/{unit}.json", "units: [eu-1]\n");
	id = create_source(fixture, "Realm one", "http_json", settings, "manual");

	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(run, "units"), ==, 1);
	g_assert_cmpint(run_int(run, "http-status"), ==, 200);
	g_assert_cmpint(run_int(run, "requests"), ==, 1);
	g_assert_cmpint(run_int(run, "new-instruments"), ==, 2);
	g_assert_cmpint(run_int(run, "rows"), >, 0);
	g_assert_cmpint(run_int(run, "bytes"), ==, (gint64)strlen(auction_body));

	reader = venture_feeds_service_open_reader(service_of(fixture), id, &error);
	g_assert_no_error(error);
	g_assert_true(venture_series_store_get_current(reader, "eu-1", "2770", &row, &error));
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_assert_cmpint(row->min_price, ==, 125);
	g_assert_cmpint(row->quantity, ==, 15);
	g_assert_cmpstr(row->currency, ==, "USD");
	g_assert_cmpint(row->taken_at, ==, 1791021600);
	g_clear_pointer(&row, venture_series_row_free);

	g_assert_true(venture_series_store_get_current(reader, "eu-1", "2447", &row, &error));
	g_assert_nonnull(row);
	g_assert_cmpint(row->min_price, ==, 75);
}

/*
 * A 304 is counted and writes nothing, and a scheduled fetch asks with
 * If-Modified-Since from the last Last-Modified.
 *
 * What breaks if this regresses: an hourly source downloading the whole
 * auction house every minute, or a not-modified answer filed as a snapshot
 * that empties the market.
 */
static void
test_feeds_http_not_modified(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	Script *script;
	gint64 rows_before;
	gint64 id;

	(void)user_data;

	script = scripted_add(&fixture->http, "/realm.json", 200, auction_body);
	script->last_modified = g_strdup("Sat, 03 Oct 2026 10:00:00 GMT");
	script->not_modified_when_asked = TRUE;
	settings = http_settings(fixture, "/realm.json", NULL);
	id = create_source(fixture, "Realm", "http_json", settings, "manual");

	first = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(first), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	rows_before = store_rows(fixture, id);
	g_assert_cmpint(rows_before, ==, 2);

	/* Switched to its own schedule, the unit is due at once and asks
	 * whether anything changed since 10:00. */
	source = get_source(fixture, id);
	g_object_set(source, "schedule", "auto", NULL);
	g_assert_true(venture_database_save(fixture->database, source, NULL, &error));
	g_assert_no_error(error);

	{
		gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

		while (count_runs(fixture, id) < 2)
		{
			g_assert_cmpint(g_get_monotonic_time(), <, deadline);

			if (!g_main_context_iteration(NULL, FALSE))
				g_usleep(2000);
		}
	}

	second = latest_run(fixture, id);
	g_assert_cmpint(run_status(second), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(second, "trigger"), ==, VENTURE_DATA_SOURCE_RUN_TRIGGER_SCHEDULE);
	g_assert_cmpint(run_int(second, "not-modified"), ==, 1);
	g_assert_cmpint(run_int(second, "rows"), ==, 0);
	g_assert_cmpint(run_int(second, "http-status"), ==, 304);
	g_mutex_lock(&fixture->http.lock);
	g_assert_cmpstr(fixture->http.last_if_modified_since, ==, "Sat, 03 Oct 2026 10:00:00 GMT");
	g_mutex_unlock(&fixture->http.lock);
	g_assert_cmpint(store_rows(fixture, id), ==, rows_before);

	/* And the unit backs off rather than asking again at once. */
	{
		g_autoptr(JsonNode) status = venture_feeds_service_dup_status(service_of(fixture));
		JsonArray *sources = json_object_get_array_member(json_node_get_object(status), "sources");
		JsonObject *unit = json_array_get_object_element(
			json_object_get_array_member(json_array_get_object_element(sources, 0), "units"), 0);

		g_assert_cmpint(json_object_get_int_member(unit, "backoff"), ==, 1);
		g_assert_cmpint(json_object_get_int_member(unit, "next_check"), >=,
		                g_get_real_time() / G_USEC_PER_SEC + 50);
	}
}

/*
 * A 429 is a failure that waits its Retry-After.
 *
 * What breaks if this regresses: a rate-limited source hammering the far
 * end every minute until its key is revoked.
 */
static void
test_feeds_http_rate_limited(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(JsonNode) status = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *message = NULL;
	Script *script;
	JsonObject *unit;
	gint64 id;

	(void)user_data;

	script = scripted_add(&fixture->http, "/busy.json", 429, "slow down");
	script->retry_after = g_strdup("600");
	settings = http_settings(fixture, "/busy.json", NULL);
	id = create_source(fixture, "Busy", "http_json", settings, "auto");

	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_cmpint(run_int(run, "http-status"), ==, 429);
	message = run_text(run, "error");
	g_assert_nonnull(strstr(message, "429"));

	status = venture_feeds_service_dup_status(service_of(fixture));
	unit = json_array_get_object_element(json_object_get_array_member(
		json_array_get_object_element(json_object_get_array_member(json_node_get_object(status),
		                                                           "sources"), 0), "units"), 0);
	g_assert_cmpint(json_object_get_int_member(unit, "next_check"), >=,
	                g_get_real_time() / G_USEC_PER_SEC + 590);
}

/*
 * Answers that are not data fail the run and store nothing: a 500, a
 * redirect (never followed), an oversized body, a deadline, broken JSON.
 *
 * What breaks if this regresses: a redirect would carry the source's
 * Authorization header to a host the answer chose; an oversized body
 * would be read into memory whole; a stalled host would hold the worker.
 */
static void
test_feeds_http_failures(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	struct
	{
		const gchar	*path;
		guint		 status;
		const gchar	*problem;
		gint		 http_status;
	} cases[] = {
		{ "/error.json", 500, "HTTP 500", 500 },
		{ "/moved.json", 302, "redirect", 302 },
		{ "/huge.json", 200, "larger than feeds.max_response_mb", 200 },
		{ "/slow.json", 200, "did not answer within", 0 },
		{ "/broken.json", 200, "not well-formed JSON", 200 },
	};
	g_autofree gchar *huge = NULL;
	Script *script;
	guint i;

	(void)user_data;

	g_object_set(fixture->config, "feeds-request-timeout", (gint64)1, NULL);

	scripted_add(&fixture->http, "/error.json", 500, "oops");
	script = scripted_add(&fixture->http, "/moved.json", 302, "");
	script->location = g_strdup_printf("%s/elsewhere.json", fixture->http.neighbor);
	huge = g_malloc(2 * 1024 * 1024 + 1);
	memset(huge, ' ', 2 * 1024 * 1024);
	huge[2 * 1024 * 1024] = '\0';
	scripted_add(&fixture->http, "/huge.json", 200, huge);
	script = scripted_add(&fixture->http, "/slow.json", 200, auction_body);
	script->delay_ms = 3000;
	scripted_add(&fixture->http, "/broken.json", 200, "{\"auctions\": [ {\"id\":");

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureEntity) run = NULL;
		g_autofree gchar *settings = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *message = NULL;
		gint64 id;

		settings = http_settings(fixture, cases[i].path, NULL);
		name = g_strdup_printf("Case %u", i);
		id = create_source(fixture, name, "http_json", settings, "manual");
		run = sync_and_wait(fixture, id);
		message = run_text(run, "error");

		g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
		g_assert_nonnull(message);

		if (NULL == strstr(message, cases[i].problem))
			g_error("%s: \"%s\" does not mention \"%s\"", cases[i].path, message, cases[i].problem);

		g_assert_cmpint(run_int(run, "http-status"), ==, cases[i].http_status);
		g_assert_cmpint(run_int(run, "rows"), ==, 0);
	}

	/* The redirect was never followed. */
	g_assert_cmpint(g_atomic_int_get(&fixture->http.neighbor_hits), ==, 0);
}

/*
 * Only an origin on feeds.allowed_origins is fetched.
 *
 * What breaks if this regresses: a data source -- any administrator's,
 * or a hosted tenant's -- becomes a way to make the server call anything
 * it can reach, including itself.
 */
static void
test_feeds_origin_allowlist(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *message = NULL;
	gint64 id;

	(void)user_data;

	settings = g_strdup_printf("url: %s/anything.json\nfields: {instrument: id}\n", fixture->http.neighbor);
	id = create_source(fixture, "Elsewhere", "http_json", settings, "manual");
	run = sync_and_wait(fixture, id);
	message = run_text(run, "error");

	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "not one of feeds.allowed_origins"));
	g_assert_cmpint(g_atomic_int_get(&fixture->http.neighbor_hits), ==, 0);
}

/*
 * A source's hourly budget defers what it cannot afford.
 *
 * What breaks if this regresses: a source with a quota from its provider
 * -- Blizzard's, the odds API's -- spending next month's budget today, or
 * a deferral recorded as a failure that pages somebody.
 */
static void
test_feeds_quota_deferral(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *notes = NULL;
	gint64 id;

	(void)user_data;

	scripted_add(&fixture->http, "/r/a.json", 200, auction_body);
	scripted_add(&fixture->http, "/r/b.json", 200, auction_body);
	settings = http_settings(fixture, "/r/{unit}.json", "units: [a, b]\nrequests_per_hour: 1\n");
	id = create_source(fixture, "Budgeted", "http_json", settings, "manual");

	first = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(first), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(first, "requests"), ==, 1);
	g_assert_cmpint(run_int(first, "quota-used"), ==, 1);
	g_assert_cmpint(run_int(first, "quota-limit"), ==, 1);
	notes = run_text(first, "notes");
	g_assert_nonnull(strstr(notes, "budget"));
	g_assert_cmpint(scripted_hits(&fixture->http, "/r/a.json"), ==, 1);
	g_assert_cmpint(scripted_hits(&fixture->http, "/r/b.json"), ==, 0);

	/* The whole budget spent: everything waits, and that is not failure. */
	second = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(second), ==, VENTURE_DATA_SOURCE_RUN_STATUS_DEFERRED);
	g_assert_cmpint(run_int(second, "requests"), ==, 0);
}

/* --- Files ----------------------------------------------------------------------------- */

/*
 * A file source reads only under feeds.file_roots: not ../, not a sibling
 * whose name starts with the root's, not through a symlink out.
 *
 * What breaks if this regresses: a data source -- written by anybody who
 * may write one -- reads /etc/shadow, the database file, or another
 * tenant's directory, into a store a page renders.
 */
static void
test_feeds_file_roots(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *outside = NULL;
	g_autofree gchar *evil_dir = NULL;
	g_autofree gchar *evil = NULL;
	g_autofree gchar *link = NULL;
	const gchar *line = "{\"type\":\"venue\",\"key\":\"v\"}\n";
	gchar *files[4];
	guint i;

	(void)user_data;

	outside = g_build_filename(fixture->state_dir, "outside.jsonl", NULL);
	g_assert_true(g_file_set_contents(outside, line, -1, NULL));
	evil_dir = g_strconcat(fixture->file_root, "-evil", NULL);
	g_assert_cmpint(g_mkdir_with_parents(evil_dir, 0700), ==, 0);
	evil = g_build_filename(evil_dir, "x.jsonl", NULL);
	g_assert_true(g_file_set_contents(evil, line, -1, NULL));
	link = g_build_filename(fixture->file_root, "link.jsonl", NULL);
	g_assert_cmpint(symlink(outside, link), ==, 0);

	files[0] = g_strdup("../outside.jsonl");
	files[1] = g_strdup(evil);
	files[2] = g_strdup("link.jsonl");
	files[3] = g_strdup(outside);

	for (i = 0; i < G_N_ELEMENTS(files); i++)
	{
		g_autoptr(VentureEntity) run = NULL;
		g_autofree gchar *settings = g_strdup_printf("file: %s\n", files[i]);
		g_autofree gchar *name = g_strdup_printf("Escape %u", i);
		g_autofree gchar *message = NULL;
		gint64 id;

		id = create_source(fixture, name, "file_jsonl", settings, "manual");
		run = sync_and_wait(fixture, id);
		message = run_text(run, "error");

		g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);

		if (NULL == strstr(message, "feeds.file_roots"))
			g_error("%s: \"%s\" does not name feeds.file_roots", files[i], message);

		g_free(files[i]);
	}

	venture_test_remove_tree(evil_dir);
}

/*
 * A JSON-lines file, a CSV and an exec plugin all reach the store.
 *
 * What breaks if this regresses: the three ways to feed VENTURE without
 * writing C. The JSON-lines file also carries listing ids and their time
 * left, and the exec plugin a cursor the next fetch is handed.
 */
static void
test_feeds_providers_reach_the_store(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) jsonl_run = NULL;
	g_autoptr(VentureEntity) csv_run = NULL;
	g_autofree gchar *jsonl = NULL;
	g_autofree gchar *csv = NULL;
	gint64 jsonl_id;
	gint64 csv_id;

	(void)user_data;

	jsonl = write_root_file(fixture, "realm.jsonl",
		"{\"type\":\"venue\",\"key\":\"argent\",\"name\":\"Argent Dawn\",\"currency\":\"GOLD\",\"group\":\"eu\"}\n"
		"{\"type\":\"instrument\",\"key\":\"ore\",\"name\":\"Iron ore\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"taken_at\":\"2026-10-03T12:00:00Z\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"1.25\",\"quantity\":20,\"id\":\"a1\",\"expires_in_min\":7200}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"1.50\",\"quantity\":5,\"id\":\"a2\"}\n"
		"{\"type\":\"quote\",\"venue\":\"bookie\",\"instrument\":\"match-home\",\"odds\":\"2.10\"}\n"
		"{\"type\":\"entry\",\"key\":\"n1\",\"title\":\"Patch notes\",\"url\":\"https://example.com/n1\"}\n");
	jsonl_id = create_source(fixture, "Lines", "file_jsonl", "file: realm.jsonl\n", "manual");
	jsonl_run = sync_and_wait(fixture, jsonl_id);
	g_assert_cmpint(run_status(jsonl_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(jsonl_run, "rows"), >=, 3);

	{
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(VentureSeriesRow) row = NULL;
		g_autoptr(GPtrArray) quotes = NULL;
		g_autoptr(GPtrArray) entries = NULL;
		g_autoptr(GError) error = NULL;

		reader = venture_feeds_service_open_reader(service_of(fixture), jsonl_id, &error);
		g_assert_no_error(error);
		g_assert_true(venture_series_store_get_current(reader, "argent", "ore", &row, &error));
		g_assert_nonnull(row);
		g_assert_cmpint(row->min_price, ==, 125);
		g_assert_cmpint(row->quantity, ==, 25);
		g_assert_cmpstr(row->currency, ==, "GOLD");

		quotes = venture_series_store_list_quotes(reader, "match-home", NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(quotes->len, ==, 1);

		entries = venture_series_store_list_entries(reader, 0, 10, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(entries->len, ==, 1);
	}

	csv = write_root_file(fixture, "prices.csv",
		"sku,name,price,qty\n"
		"w-1,\"Widget, large\",4.20,3\n"
		"w-2,Gadget,9.99,1\n"
		"w-3,Broken,,1\n");
	csv_id = create_source(fixture, "Supplier", "csv",
	                       "file: prices.csv\n"
	                       "units: [supplier]\n"
	                       "fields: {instrument: sku, name: name, price: price, quantity: qty}\n",
	                       "manual");
	csv_run = sync_and_wait(fixture, csv_id);

	/* The row with no price is refused alone; the rest is stored. */
	g_assert_cmpint(run_status(csv_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(csv_run, "refused"), ==, 1);
	g_assert_cmpint(store_rows(fixture, csv_id), ==, 2);

	{
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;
		g_autoptr(GError) error = NULL;

		reader = venture_feeds_service_open_reader(service_of(fixture), csv_id, &error);
		g_assert_true(venture_series_store_get_instrument(reader, "w-1", &instrument, &error));
		g_assert_nonnull(instrument);
		g_assert_cmpstr(instrument->name, ==, "Widget, large");
	}
}

/*
 * The demo's market data goes through the real path: the generator's
 * files (tools/venture-demo-market.sh, one day of it) read by file_jsonl
 * sources, parsed, batched and committed by the worker. Every realm's
 * newest snapshot is the anchor, nothing is refused, listings that
 * vanished with time left count as sold, the bookmakers' quotes hang off
 * their events, and a second sync of the same files writes nothing.
 *
 * What breaks if this regresses: `make demo` dies at its first sync with
 * a protocol error naming a line number, or seeds an empty market -- and
 * tests/demo-market.sh, which checks the files in Python, cannot see a
 * disagreement between its reading of the protocol and the server's.
 */
static void
test_feeds_demo_market_files(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureEntity) odds_run = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(VentureSeriesStore) odds_reader = NULL;
	g_autoptr(VentureSeriesRow) leaf = NULL;
	g_autoptr(GPtrArray) quotes = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *anchor_text = NULL;
	g_autofree gchar *err = NULL;
	static const gchar *const realms[] = {
		"silverfen", "thornmere", "moonwell", "greyharbor", "emberfall", "duskwatch"
	};
	const gchar *argv[] = { "bash", NULL, "--out", NULL, "--anchor", NULL, "--days", "1", NULL };
	gint64 anchor;
	gint64 rows;
	gint64 sold;
	gint status;
	guint i;
	gint64 id;
	gint64 odds_id;

	(void)user_data;

	/* GOLD as the demo registers it: a copper is a ten-thousandth, and
	 * the generator writes four places. Unregistered, every price would
	 * be refused for its places. */
	{
		g_autoptr(VentureEntity) gold = NULL;

		gold = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_CURRENCY, NULL));
		g_object_set(gold, "code", "GOLD", "name", "Gold", "exponent", (gint64)4, NULL);
		g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
		g_assert_no_error(error);
	}

	/* On the hour and in the past, as the demo's economy clock is. */
	anchor = (g_get_real_time() / G_USEC_PER_SEC) / 3600 * 3600 - 3600;
	anchor_text = g_strdup_printf("%" G_GINT64_FORMAT, anchor);
	script = g_build_filename(VENTURE_TEST_TOOLS, "venture-demo-market.sh", NULL);
	argv[1] = script;
	argv[3] = fixture->file_root;
	argv[5] = anchor_text;
	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
	                           NULL, NULL, NULL, &err, &status, &error));
	g_assert_no_error(error);
	if (!g_spawn_check_wait_status(status, &error))
		g_error("venture-demo-market.sh: %s: %s", error->message, err);

	id = create_source(fixture, "Evermoor", "file_jsonl",
	                   "file: evermoor/{unit}.jsonl\n"
	                   "units: [silverfen, thornmere, moonwell, greyharbor, emberfall, duskwatch]\n",
	                   "manual");
	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(run, "units"), ==, 6);
	g_assert_cmpint(run_int(run, "refused"), ==, 0);
	g_assert_cmpint(run_int(run, "new-instruments"), ==, 41);
	rows = store_rows(fixture, id);
	g_assert_cmpint(rows, >, 200);

	reader = venture_feeds_service_open_reader(service_of(fixture), id, &error);
	g_assert_no_error(error);
	sold = 0;

	for (i = 0; i < G_N_ELEMENTS(realms); i++)
	{
		VentureSeriesVenueState state;
		g_autoptr(GArray) days = NULL;
		guint d;

		g_assert_true(venture_series_store_get_venue_state(reader, realms[i], &state, &error));
		g_assert_no_error(error);
		g_assert_true(state.found);
		g_assert_cmpint(state.last_taken_at, ==, anchor);

		days = venture_series_store_daily(reader, realms[i], "silverleaf", 0, &error);
		g_assert_no_error(error);

		for (d = 0; d < days->len; d++)
		{
			const VentureSeriesDay *day = &g_array_index(days, VentureSeriesDay, d);

			if (VENTURE_SERIES_NONE != day->sold_estimate)
				sold += day->sold_estimate;
		}
	}

	/* The listings churn: what vanished early was sold. */
	g_assert_cmpint(sold, >, 0);

	/* The planted craft input, under the demo's alert line, in gold. */
	g_assert_true(venture_series_store_get_current(reader, "thornmere", "silverleaf", &leaf, &error));
	g_assert_no_error(error);
	g_assert_nonnull(leaf);
	g_assert_cmpstr(leaf->currency, ==, "GOLD");
	g_assert_cmpint(leaf->min_price, <, 1000);
	g_assert_cmpint(leaf->taken_at, ==, anchor);

	/* The same files again: every snapshot is a duplicate. */
	again = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(again), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(store_rows(fixture, id), ==, rows);

	odds_id = create_source(fixture, "Books", "file_jsonl", "file: usd/odds.jsonl\n", "manual");
	odds_run = sync_and_wait(fixture, odds_id);
	g_assert_cmpint(run_status(odds_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	odds_reader = venture_feeds_service_open_reader(service_of(fixture), odds_id, &error);
	g_assert_no_error(error);
	quotes = venture_series_store_list_quotes(odds_reader, NULL, "evt-varga-okafor", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(quotes->len, >=, 4);

	/* The registry is process-wide; the next test starts without GOLD. */
	venture_currency_clear_registered();
}

static gchar *
install_exec_provider(Fixture *fixture)
{
	g_autofree gchar *directory = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *script = NULL;
	gchar *manifest;
	gsize length;

	directory = g_build_filename(fixture->state_dir, "plugins", "realms", NULL);
	g_assert_cmpint(g_mkdir_with_parents(directory, 0755), ==, 0);
	source = g_build_filename(VENTURE_TEST_FIXTURES, "exec", "feed-provider.sh", NULL);
	g_assert_true(g_file_get_contents(source, &contents, &length, NULL));
	script = g_build_filename(directory, "realms.sh", NULL);
	g_assert_true(g_file_set_contents(script, contents, (gssize)length, NULL));
	g_assert_cmpint(g_chmod(script, 0755), ==, 0);

	manifest = g_build_filename(directory, "realms.plugin.yaml", NULL);
	g_assert_true(g_file_set_contents(manifest,
		"name: realms\n"
		"runtime: exec\n"
		"protocol: 1\n"
		"entry: realms.sh\n"
		"exec:\n"
		"  timeout: 20\n"
		"provides:\n"
		"  - kind: data_source_provider\n"
		"    name: realm_feed\n"
		"    label: Realms over exec\n"
		"    command: fetch\n", -1, NULL));

	return manifest;
}

/*
 * An exec plugin's program is a provider, and plugins.allow_exec stops it
 * at the next run, not only at load.
 *
 * What breaks if this regresses: a scripted supplier feed that never
 * runs, or one that keeps running after the operator switched exec off.
 */
/*
 * track: known keeps the instruments the settings list and, with the
 * marketdata module on, the instrument records filed under the source --
 * read when the source is frozen, on the main thread.
 *
 * What breaks if this regresses: promoting an instrument does not make
 * the source keep it, so a known-only source stores nothing but its
 * hand-typed list.
 */
static void
test_feeds_track_known_instrument_records(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(VentureSeriesRow) ore = NULL;
	g_autoptr(VentureSeriesRow) herb = NULL;
	g_autoptr(VentureInstrument) instrument = NULL;
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	path = write_root_file(fixture, "known.jsonl",
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"taken_at\":\"2026-10-03T12:00:00Z\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"1.25\",\"quantity\":2}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"herb\",\"price\":\"0.50\",\"quantity\":4}\n");

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Known only", "provider", "file_jsonl",
	             "settings", "file: known.jsonl\n", "schedule", "manual", "currency", "USD",
	             "track", VENTURE_DATA_SOURCE_TRACK_KNOWN, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error));
	g_assert_no_error(error);

	instrument = venture_instrument_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(instrument), fixture->org);
	g_object_set(instrument, "name", "Ore", "key", "ore",
	             "data-source-id", venture_entity_get_id(VENTURE_ENTITY(source)), NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(instrument), NULL, &error));
	g_assert_no_error(error);

	run = sync_and_wait(fixture, venture_entity_get_id(VENTURE_ENTITY(source)));
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

	reader = venture_feeds_service_open_reader(service_of(fixture),
	                                           venture_entity_get_id(VENTURE_ENTITY(source)), &error);
	g_assert_no_error(error);
	g_assert_true(venture_series_store_get_current(reader, "argent", "ore", &ore, &error));
	g_assert_nonnull(ore);
	g_assert_true(venture_series_store_get_current(reader, "argent", "herb", &herb, &error));
	g_assert_null(herb);
}

static void
test_feeds_exec_provider(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) partial = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *message = NULL;
	gint64 id;

	(void)user_data;

	g_object_set(fixture->config, "plugins-allow-exec", TRUE, NULL);
	manager = venture_plugin_manager_new(fixture->context);
	venture_context_set_plugin_manager(fixture->context, manager);
	manifest = install_exec_provider(fixture);
	g_assert_true(venture_plugin_manager_load_file(manager, manifest, &error));
	g_assert_no_error(error);
	g_assert_nonnull(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "realm_feed"));

	id = create_source(fixture, "Realms", "realm_feed", "units: [kazzak, broken]\n", "manual");
	run = sync_and_wait(fixture, id);

	/* kazzak stored everything; broken stored one listing then failed. */
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_PARTIAL);
	g_assert_cmpint(run_int(run, "units"), ==, 2);
	message = run_text(run, "error");
	g_assert_nonnull(strstr(message, "the far end stopped answering"));

	{
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(VentureSeriesRow) row = NULL;
		g_autoptr(VentureSeriesRow) broken = NULL;

		reader = venture_feeds_service_open_reader(service_of(fixture), id, &error);
		g_assert_true(venture_series_store_get_current(reader, "kazzak", "ore", &row, &error));
		g_assert_nonnull(row);
		g_assert_cmpint(row->quantity, ==, 25);
		g_assert_true(venture_series_store_get_current(reader, "broken", "ore", &broken, &error));
		g_assert_nonnull(broken);
		g_assert_cmpint(broken->quantity, ==, 20);
	}

	g_clear_pointer(&message, g_free);
	g_object_set(fixture->config, "plugins-allow-exec", FALSE, NULL);
	refused = sync_and_wait(fixture, id);
	message = run_text(refused, "error");
	g_assert_cmpint(run_status(refused), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "plugins.allow_exec is off"));

	venture_context_set_plugin_manager(fixture->context, NULL);
	(void)partial;
}

/*
 * A batch that ends in an error is stored up to it and the run is
 * partial.
 *
 * What breaks if this regresses: either the good half of a feed is thrown
 * away because the far end fell over at the end, or the failure is
 * swallowed and the run reads as fine.
 */
static void
test_feeds_partial_batch(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *message = NULL;
	gint64 id;

	(void)user_data;

	file = write_root_file(fixture, "partial.jsonl",
		"{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\",\"price\":\"2.00\"}\n"
		"{\"type\":\"error\",\"message\":\"page two timed out\"}\n");
	id = create_source(fixture, "Partial", "file_jsonl", "file: partial.jsonl\n", "manual");
	run = sync_and_wait(fixture, id);
	message = run_text(run, "error");

	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_PARTIAL);
	g_assert_nonnull(strstr(message, "page two timed out"));
	g_assert_cmpint(store_rows(fixture, id), ==, 1);

	/* A line that breaks the protocol fails the unit outright. */
	{
		g_autoptr(VentureEntity) broken = NULL;
		g_autofree gchar *broken_message = NULL;
		gint64 broken_id;

		write_root_file(fixture, "bad.jsonl",
			"{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\",\"price\":2.0}\n");
		broken_id = create_source(fixture, "Bad lines", "file_jsonl", "file: bad.jsonl\n", "manual");
		broken = sync_and_wait(fixture, broken_id);
		broken_message = run_text(broken, "error");
		g_assert_cmpint(run_status(broken), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
		g_assert_nonnull(strstr(broken_message, "Line 1"));
	}
}

/* --- The records sink ----------------------------------------------------------------- */

static gint64
count_rates(
	Fixture		*fixture,
	gboolean	 include_deleted
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	gint64 count;

	query = venture_query_new(VENTURE_TYPE_EXCHANGE_RATE);
	venture_query_set_include_deleted(query, include_deleted);
	g_assert_true(venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ,
	                                              "ecb-feed", &error));
	count = venture_database_count(fixture->database, query, &error);
	g_assert_no_error(error);

	return count;
}

/*
 * Record upserts are matched, idempotent, and restore a deleted row.
 *
 * What breaks if this regresses: every sync adds another identical
 * exchange rate, or a rate somebody deleted makes every later run fail a
 * unique index -- the mail-sync stall, again.
 */
static void
test_feeds_records_sink(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) third = NULL;
	g_autoptr(VentureEntity) ignored = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rates = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 other;

	(void)user_data;

	write_root_file(fixture, "rates.jsonl",
		"{\"type\":\"record\",\"record_type\":\"exchange_rate\",\"fields\":{"
		"\"from_currency\":\"EUR\",\"to_currency\":\"USD\",\"rate_numerator\":108,"
		"\"rate_denominator\":100,\"effective_at\":\"2026-10-01T00:00:00Z\",\"source\":\"ecb-feed\"}}\n");
	id = create_source(fixture, "Rates", "file_jsonl", "file: rates.jsonl\nrecord_types: [exchange_rate]\n",
	                   "manual");

	first = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(first), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(first, "records"), ==, 1);
	g_assert_cmpint(count_rates(fixture, TRUE), ==, 1);

	second = sync_and_wait(fixture, id);
	g_assert_cmpint(run_int(second, "records"), ==, 1);
	g_assert_cmpint(count_rates(fixture, TRUE), ==, 1);

	/* Deleted by hand, brought back by the next run, not duplicated. */
	query = venture_query_new(VENTURE_TYPE_EXCHANGE_RATE);
	g_assert_true(venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ, "ecb-feed", &error));
	rates = venture_database_find(fixture->database, query, &error);
	g_assert_cmpuint(rates->len, ==, 1);
	g_assert_true(venture_database_delete(fixture->database, g_ptr_array_index(rates, 0), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(count_rates(fixture, FALSE), ==, 0);

	third = sync_and_wait(fixture, id);
	g_assert_cmpint(run_int(third, "records"), ==, 1);
	g_assert_cmpint(count_rates(fixture, FALSE), ==, 1);
	g_assert_cmpint(count_rates(fixture, TRUE), ==, 1);

	/* A source that does not accept the type writes nothing, and says so. */
	other = create_source(fixture, "Rates, unaccepted", "file_jsonl", "file: rates.jsonl\n", "manual");
	ignored = sync_and_wait(fixture, other);
	{
		g_autofree gchar *notes = run_text(ignored, "notes");

		g_assert_cmpint(run_int(ignored, "records"), ==, 0);
		g_assert_nonnull(strstr(notes, "records not written"));
	}
}

/* --- Credentials ------------------------------------------------------------------- */

static GMutex captured_lock;
static GString *captured_log;

static void
capture_log(
	const gchar	*domain,
	GLogLevelFlags	 level,
	const gchar	*message,
	gpointer	 data
){
	(void)domain;
	(void)level;
	(void)data;

	g_mutex_lock(&captured_lock);
	g_string_append(captured_log, message);
	g_string_append_c(captured_log, '\n');
	g_mutex_unlock(&captured_lock);
}

static gboolean
file_contains(
	const gchar	*path,
	const gchar	*needle
){
	g_autofree gchar *contents = NULL;
	gsize length;

	if (!g_file_get_contents(path, &contents, &length, NULL))
		return FALSE;

	/* memmem, not g_strstr_len(): a database file is full of NUL bytes,
	 * and a string search stops at the first. */
	return NULL != memmem(contents, length, needle, strlen(needle));
}

/*
 * A credential never reaches a run, an API answer, the log or the store:
 * not when the far end echoes it into an item's name, not when an error
 * quotes the address it was in.
 *
 * What breaks if this regresses: anybody who can read a data source run
 * -- every viewer -- reads the provider's API key.
 */
static void
test_feeds_credentials_never_leak(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar token[] = "tok-SECRET-4242";
	static const gchar api_key[] = "key-SECRET-7373";
	g_autoptr(VentureEntity) ok_run = NULL;
	g_autoptr(VentureEntity) bad_run = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(JsonObject) values = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonNode) serialized = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *store = NULL;
	g_autofree gchar *wal = NULL;
	g_autofree gchar *report = NULL;
	GLogFunc previous;
	Script *script;
	gint64 id;

	(void)user_data;

	script = scripted_add(&fixture->http, "/echo.json", 200, NULL);
	script->echo_authorization = TRUE;
	scripted_add(&fixture->http, "/fail.json", 500, "no");
	settings = g_strdup_printf("url: %s/echo.json?key={secret:api_key}\n"
	                           "items: items\n"
	                           "headers: {Authorization: \"Bearer {secret:token}\"}\n"
	                           "fields: {instrument: id, name: name, price: price}\n",
	                           fixture->http.origin);
	id = create_source(fixture, "Keyed", "http_json", settings, "manual");

	source = get_source(fixture, id);
	key = g_strconcat("feed-", venture_entity_get_uuid(source), NULL);
	values = json_object_new();
	json_object_set_string_member(values, "token", token);
	json_object_set_string_member(values, "api_key", api_key);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, values);
	binding = venture_integration_service_configure(venture_integration_service_get(fixture->database),
		fixture->org, key, venture_entity_get_uuid(source), "live", node, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(binding);

	g_mutex_init(&captured_lock);
	captured_log = g_string_new(NULL);
	previous = g_log_set_default_handler(capture_log, NULL);

	settle(fixture);
	ok_run = sync_and_wait(fixture, id);

	/* The credentials were sent... */
	g_mutex_lock(&fixture->http.lock);
	g_assert_cmpstr(fixture->http.last_authorization, ==, "Bearer tok-SECRET-4242");
	g_assert_nonnull(strstr(fixture->http.last_query, api_key));
	g_mutex_unlock(&fixture->http.lock);
	g_assert_cmpint(run_status(ok_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

	/* ...and a failing address that carried one is not quoted. */
	{
		g_autofree gchar *failing = g_strdup_printf("url: %s/fail.json?key={secret:api_key}\n"
		                                            "fields: {instrument: id}\n", fixture->http.origin);

		g_object_set(source, "settings", failing, NULL);
	}
	g_assert_true(venture_database_save(fixture->database, source, NULL, &error));
	g_assert_no_error(error);
	bad_run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(bad_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);

	report = venture_feeds_service_test(service_of(fixture), id, NULL, &error);
	g_assert_null(report);
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, api_key));
	g_clear_error(&error);

	g_log_set_default_handler(previous, NULL);

	serialized = venture_serializable_to_json(VENTURE_SERIALIZABLE(ok_run), TRUE);
	json = json_to_string(serialized, FALSE);
	g_assert_null(strstr(json, token));
	g_clear_pointer(&json, g_free);
	g_clear_pointer(&serialized, json_node_unref);
	serialized = venture_serializable_to_json(VENTURE_SERIALIZABLE(bad_run), TRUE);
	json = json_to_string(serialized, FALSE);
	g_assert_null(strstr(json, token));
	g_assert_null(strstr(json, api_key));

	g_mutex_lock(&captured_lock);
	g_assert_null(strstr(captured_log->str, token));
	g_assert_null(strstr(captured_log->str, api_key));
	g_mutex_unlock(&captured_lock);
	g_string_free(captured_log, TRUE);
	captured_log = NULL;

	/* The far end echoed the token into an item's name; the store has the
	 * redaction, not the token. */
	store = g_build_filename(fixture->state_dir, "series", venture_entity_get_uuid(source), "store.db", NULL);
	wal = g_strconcat(store, "-wal", NULL);
	g_assert_true(g_file_test(store, G_FILE_TEST_EXISTS));
	g_assert_false(file_contains(store, token));
	g_assert_false(file_contains(wal, token));
	{
		g_autoptr(VentureSeriesStore) reader = NULL;
		g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;

		reader = venture_feeds_service_open_reader(service_of(fixture), id, &error);
		g_assert_no_error(error);
		g_assert_true(venture_series_store_get_instrument(reader, "x", &instrument, &error));
		g_assert_nonnull(instrument);
		g_assert_cmpstr(instrument->name, ==, "Bearer [redacted]");
	}

	g_assert_true(file_contains(store, "[redacted]") || file_contains(wal, "[redacted]"));
}

/* --- Threads and transactions ------------------------------------------------------- */

typedef struct
{
	VentureDatabase	*database;
	gboolean	 saved;
} OffThread;

static gpointer
save_off_thread(gpointer data)
{
	OffThread *off = data;
	g_autoptr(VentureVenture) venture = venture_venture_new();

	g_object_set(venture, "name", "From elsewhere", NULL);
	off->saved = venture_database_save(off->database, VENTURE_ENTITY(venture), NULL, NULL);

	return NULL;
}

/*
 * A write from a thread other than the database's own is a critical.
 *
 * What breaks if this regresses: a worker that saves a record runs the
 * automation engine, the inbox and webhooks on its own thread, and the
 * failure shows up as an automation that silently stops firing, nowhere
 * near the save.
 */
static void
test_feeds_home_thread_critical(void)
{
	if (g_test_subprocess())
	{
		g_autoptr(VentureDatabase) database = NULL;
		g_autoptr(GError) error = NULL;
		OffThread off;
		GThread *thread;

		database = venture_database_new("sqlite://:memory:", &error);
		g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
		off.database = database;
		off.saved = FALSE;
		thread = g_thread_new("wrong-thread", save_off_thread, &off);
		g_thread_join(thread);
		return;
	}

	g_test_trap_subprocess(NULL, 0, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_failed();
	g_test_trap_assert_stderr("*venture_database_save() called on a thread other than*");
}

/*
 * A run that comes back while a transaction is open waits for it.
 *
 * What breaks if this regresses: the run is written inside somebody
 * else's transaction and rolled back with it, so a sync that happened is
 * never recorded, or the outer writer's rollback takes a run it did not
 * make.
 */
static void
test_feeds_handoff_waits_for_the_transaction(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	gint64 deadline;
	gint64 id;

	(void)user_data;

	write_root_file(fixture, "one.jsonl",
		"{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\",\"price\":\"1.00\"}\n");
	id = create_source(fixture, "Waiting", "file_jsonl", "file: one.jsonl\n", "manual");

	g_assert_true(venture_database_begin(fixture->database, &error));
	g_assert_true(venture_feeds_service_sync(service_of(fixture), id,
	                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
	g_assert_no_error(error);

	/* Long enough for the worker to finish and hand the run back. */
	deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;

	while (g_get_monotonic_time() < deadline)
	{
		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	g_assert_cmpint(store_rows(fixture, id), ==, 1);
	g_assert_cmpint(count_runs(fixture, id), ==, 0);
	g_assert_cmpuint(venture_feeds_service_count_pending(service_of(fixture)), >=, 1);

	g_assert_true(venture_database_commit(fixture->database, &error));
	g_assert_no_error(error);
	settle(fixture);

	g_assert_cmpint(count_runs(fixture, id), ==, 1);
}

static gboolean
thread_named(const gchar *name)
{
	g_autoptr(GDir) tasks = NULL;
	const gchar *task;

	tasks = g_dir_open("/proc/self/task", 0, NULL);

	if (NULL == tasks)
		return FALSE;

	while (NULL != (task = g_dir_read_name(tasks)))
	{
		g_autofree gchar *path = g_build_filename("/proc/self/task", task, "comm", NULL);
		g_autofree gchar *comm = NULL;

		if (g_file_get_contents(path, &comm, NULL, NULL) && g_str_has_prefix(comm, name))
			return TRUE;
	}

	return FALSE;
}

/*
 * A sync is pending from the moment it is asked for until its run is
 * written: count_pending() never reads zero in between.
 *
 * What breaks if this regresses: the sync command is freed on the worker
 * before the pass that answers it starts on the worker's next timer, and a
 * caller polling count_pending() -- every test's settle(), a CLI waiting
 * on --wait -- sees nothing pending in that gap and reads the runs before
 * the one it asked for exists. It failed test-alerts about one run in
 * five under load before the queued sync was counted as live.
 */
static void
test_feeds_pending_covers_a_queued_sync(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *path = NULL;
	gint64 id;
	guint i;

	(void)user_data;

	path = write_root_file(fixture, "pending.jsonl",
		"{\"type\":\"venue\",\"key\":\"argent\",\"currency\":\"USD\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"currency\":\"USD\","
		"\"taken_at\":\"2026-10-03T12:00:00Z\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\","
		"\"price\":\"1.25\",\"quantity\":2,\"id\":\"a1\"}\n");
	id = create_source(fixture, "Pending", "file_jsonl", "file: pending.jsonl\n", "manual");

	for (i = 0; i < 25; i++)
	{
		g_autoptr(GError) error = NULL;
		gint64 deadline;

		g_assert_true(venture_feeds_service_sync(service_of(fixture), id,
		                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
		g_assert_no_error(error);
		deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

		/* Poll without sleeping: the gap is a few microseconds wide. */
		while (venture_feeds_service_count_pending(service_of(fixture)) > 0)
		{
			if (g_get_monotonic_time() > deadline)
				g_error("the feeds worker did not settle within 30 seconds");

			g_main_context_iteration(NULL, FALSE);
		}

		g_assert_cmpint(count_runs(fixture, id), ==, (gint64)i + 1);
	}
}

/*
 * The worker starts only when asked, stops when feeds go off, and is
 * joined when the context goes.
 *
 * What breaks if this regresses: a thread outliving the context that
 * started it -- writing into a store whose directory a test already
 * removed, or keeping a server from exiting.
 */
static void
test_feeds_worker_lifecycle(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	VentureFeedsService *service;
	gint64 id;

	(void)user_data;

	write_root_file(fixture, "one.jsonl",
		"{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\",\"price\":\"1.00\"}\n");
	id = create_source(fixture, "Manual", "file_jsonl", "file: one.jsonl\n", "manual");

	/* A manual source starts nothing. */
	service = service_of(fixture);
	settle(fixture);
	g_assert_false(venture_feeds_service_is_running(service));
	g_assert_false(thread_named("venture-feeds"));

	run = sync_and_wait(fixture, id);
	g_assert_true(venture_feeds_service_is_running(service));
	g_assert_true(thread_named("venture-feeds"));

	/* Switched off: stopped, and nothing to reach. */
	g_object_set(fixture->config, "feeds-enabled", FALSE, NULL);
	g_assert_null(venture_context_get_feeds_service(fixture->context));
	g_assert_false(thread_named("venture-feeds"));

	/* Back on, synced, and the context dropped: joined. */
	g_object_set(fixture->config, "feeds-enabled", TRUE, NULL);
	g_clear_object(&run);
	run = sync_and_wait(fixture, id);
	g_assert_true(thread_named("venture-feeds"));
	g_clear_object(&fixture->context);
	g_assert_false(thread_named("venture-feeds"));
}

/* --- Actions, automation, history ---------------------------------------------------- */

/*
 * The test action reports and writes nothing; purge_history deletes the
 * store and nothing else; feeds_sync queues from a rule.
 *
 * What breaks if this regresses: a test that writes a run (or a store)
 * nobody asked for; a purge that takes the source's records with it; an
 * automation step that waits on somebody else's server.
 */
static void
test_feeds_actions_and_automation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureEntity) purged = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(GVariant) answer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *store_dir = NULL;
	g_autofree gchar *id_text = NULL;
	VentureActionRegistry *actions;
	VentureAutomationHandlerRegistry *handlers;
	gint64 id;

	(void)user_data;

	write_root_file(fixture, "one.jsonl",
		"{\"type\":\"listing\",\"venue\":\"v\",\"instrument\":\"i\",\"price\":\"1.00\"}\n");
	id = create_source(fixture, "Acted on", "file_jsonl", "file: one.jsonl\n", "manual");
	source = get_source(fixture, id);
	store_dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	actions = venture_database_get_action_registry(fixture->database);
	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);

	result = venture_action_registry_perform(actions, "data_source", id, "test", params, NULL,
	                                         VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_object_get(result, "result", &message, NULL);
	g_assert_nonnull(strstr(message, "default: "));
	g_assert_nonnull(strstr(message, "1 listings"));
	g_assert_cmpint(count_runs(fixture, id), ==, 0);
	g_assert_false(g_file_test(store_dir, G_FILE_TEST_EXISTS));

	handlers = venture_context_get_automation_handlers(fixture->context);
	g_assert_true(venture_automation_handler_registry_has_event(handlers, "feed_synced"));
	g_assert_true(venture_automation_handler_registry_has_event(handlers, "feed_failed"));
	id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	g_assert_true(venture_automation_handler_registry_call(handlers, fixture->context, "feeds_sync",
	                                                       g_variant_new_string(id_text), &answer, &error));
	g_assert_no_error(error);
	settle(fixture);
	run = latest_run(fixture, id);
	g_assert_nonnull(run);
	g_assert_cmpint(run_int(run, "trigger"), ==, VENTURE_DATA_SOURCE_RUN_TRIGGER_AUTOMATION);
	g_assert_true(g_file_test(store_dir, G_FILE_TEST_IS_DIR));

	purged = venture_action_registry_perform(actions, "data_source", id, "purge_history", params, NULL,
	                                         VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	settle(fixture);
	g_assert_false(g_file_test(store_dir, G_FILE_TEST_EXISTS));
	g_assert_cmpint(count_runs(fixture, id), ==, 1);
	g_clear_object(&source);
	source = get_source(fixture, id);
	g_assert_false(venture_entity_is_deleted(source));
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/feeds/module-is-opt-in", test_feeds_module_is_opt_in);
	g_test_add_func("/feeds/exact-conversions", test_feeds_exact_conversions);
	g_test_add_func("/feeds/listing-expiry-in-the-protocol", test_feeds_listing_expiry_in_the_protocol);
	g_test_add_func("/feeds/adaptive-interval", test_feeds_adaptive_interval);
	g_test_add_func("/feeds/home-thread-critical", test_feeds_home_thread_critical);
	g_test_add_func("/feeds/config-is-validated", test_feeds_config_is_validated);
	g_test_add("/feeds/settings-are-validated", Fixture, NULL, fixture_set_up,
	           test_feeds_settings_are_validated, fixture_tear_down);
	g_test_add("/feeds/http-ok", Fixture, NULL, fixture_set_up, test_feeds_http_ok, fixture_tear_down);
	g_test_add("/feeds/http-not-modified", Fixture, NULL, fixture_set_up,
	           test_feeds_http_not_modified, fixture_tear_down);
	g_test_add("/feeds/http-rate-limited", Fixture, NULL, fixture_set_up,
	           test_feeds_http_rate_limited, fixture_tear_down);
	g_test_add("/feeds/http-failures", Fixture, NULL, fixture_set_up,
	           test_feeds_http_failures, fixture_tear_down);
	g_test_add("/feeds/origin-allowlist", Fixture, NULL, fixture_set_up,
	           test_feeds_origin_allowlist, fixture_tear_down);
	g_test_add("/feeds/quota-deferral", Fixture, NULL, fixture_set_up,
	           test_feeds_quota_deferral, fixture_tear_down);
	g_test_add("/feeds/file-roots", Fixture, NULL, fixture_set_up,
	           test_feeds_file_roots, fixture_tear_down);
	g_test_add("/feeds/providers-reach-the-store", Fixture, NULL, fixture_set_up,
	           test_feeds_providers_reach_the_store, fixture_tear_down);
	g_test_add("/feeds/demo-market-files", Fixture, NULL, fixture_set_up,
	           test_feeds_demo_market_files, fixture_tear_down);
	g_test_add("/feeds/exec-provider", Fixture, NULL, fixture_set_up,
	           test_feeds_exec_provider, fixture_tear_down);
	g_test_add("/feeds/stop-is-bounded", Fixture, NULL, fixture_set_up,
	           test_feeds_stop_is_bounded, fixture_tear_down);
	g_test_add("/feeds/partial-batch", Fixture, NULL, fixture_set_up,
	           test_feeds_partial_batch, fixture_tear_down);
	g_test_add("/feeds/records-sink", Fixture, NULL, fixture_set_up,
	           test_feeds_records_sink, fixture_tear_down);
	g_test_add("/feeds/credentials-never-leak", Fixture, NULL, fixture_set_up,
	           test_feeds_credentials_never_leak, fixture_tear_down);
	g_test_add("/feeds/handoff-waits-for-the-transaction", Fixture, NULL, fixture_set_up,
	           test_feeds_handoff_waits_for_the_transaction, fixture_tear_down);
	g_test_add("/feeds/pending-covers-a-queued-sync", Fixture, NULL, fixture_set_up,
	           test_feeds_pending_covers_a_queued_sync, fixture_tear_down);
	g_test_add("/feeds/worker-lifecycle", Fixture, NULL, fixture_set_up,
	           test_feeds_worker_lifecycle, fixture_tear_down);
	g_test_add("/feeds/actions-and-automation", Fixture, NULL, fixture_set_up,
	           test_feeds_actions_and_automation, fixture_tear_down);
	g_test_add("/feeds/track-known-instrument-records", Fixture, NULL, fixture_set_up,
	           test_feeds_track_known_instrument_records, fixture_tear_down);

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
