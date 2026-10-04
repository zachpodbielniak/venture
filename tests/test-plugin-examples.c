/*
 * test-plugin-examples.c - The reference data source plugins that need no
 *                          build step: odds-api.c (crispy) and supplier-csv
 *                          (exec)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Both are loaded from the source tree exactly as an operator's plugin
 * directory would hold them: plugins/scripts/odds-api.c compiled on demand
 * by crispy (the test skips only when there is no compiler or no
 * development include tree to compile against), and
 * plugins/exec/supplier-csv/supplier-csv.plugin.yaml run as a program with
 * plugins.allow_exec on.
 *
 * the-odds-api is a scripted server on port 0 serving
 * tests/fixtures/odds-api/, on feeds.allowed_origins like any far end. Its
 * fixture plants one surebet -- Arsenal v Chelsea, best odds 2.10 / 4.20 /
 * 3.80 across two bookmakers, S = 0.977 -- and one near miss, which a
 * `cover` scan over the synced store must tell apart.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif

#include <venture.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <string.h>

#include "venture-test-util.h"
#include "venture-test-http.h"

#ifdef VENTURE_HAVE_SQLITE

static const gchar api_key[] = "odds-KEY-5b1c9e";

/* ==========================================================================
 * The fixture
 * ========================================================================== */

typedef struct
{
	gchar			*state_dir;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VenturePluginManager	*manager;
	gint64			 org;
	VentureTestHttp		 http;
} Fixture;

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

	if (NULL != captured_log)
	{
		g_string_append(captured_log, message);
		g_string_append_c(captured_log, '\n');
	}

	g_mutex_unlock(&captured_lock);
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-plugin-examples-XXXXXX", &error);
	g_assert_no_error(error);
	venture_test_http_start(&fixture->http);

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "feeds-enabled", TRUE,
	             "feeds-allowed-origins", fixture->http.origin,
	             "feeds-run-window-minutes", (gint64)0,
	             "feeds-request-timeout", (gint64)5,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(fixture->database),
	                                                  key, &error));
	g_assert_no_error(error);

	fixture->manager = venture_plugin_manager_new(fixture->context);
	venture_context_set_plugin_manager(fixture->context, fixture->manager);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	if (NULL != fixture->context)
		venture_context_set_plugin_manager(fixture->context, NULL);

	g_clear_object(&fixture->manager);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_test_http_stop(&fixture->http);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}
}

/* ==========================================================================
 * Sources and runs
 * ========================================================================== */

static VentureFeedsService *
service_of(Fixture *fixture)
{
	VentureFeedsService *service = venture_context_get_feeds_service(fixture->context);

	g_assert_nonnull(service);

	return service;
}

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
	const gchar	*secret_name,
	const gchar	*secret
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", name, "provider", provider, "settings", settings, "schedule", "manual",
	             "currency", "USD", NULL);

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error))
		g_error("%s: %s", name, error->message);

	if (NULL != secret)
	{
		g_autoptr(VentureIntegrationConnection) binding = NULL;
		g_autoptr(JsonObject) values = json_object_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *key = g_strconcat("feed-", venture_entity_get_uuid(VENTURE_ENTITY(source)), NULL);

		json_object_set_string_member(values, secret_name, secret);
		json_node_set_object(node, values);
		binding = venture_integration_service_configure(venture_integration_service_get(fixture->database),
			fixture->org, key, venture_entity_get_uuid(VENTURE_ENTITY(source)), "live", node, 0, NULL,
			&error);
		g_assert_no_error(error);
		g_assert_nonnull(binding);
	}

	return venture_entity_get_id(VENTURE_ENTITY(source));
}

static VentureEntity *
latest_run(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GError) error = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
	g_assert_true(venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ, source_id,
	                                           &error));
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	runs = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);

	return (runs->len > 0) ? g_object_ref(g_ptr_array_index(runs, 0)) : NULL;
}

static VentureEntity *
sync_and_wait(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(GError) error = NULL;
	VentureEntity *run;

	settle(fixture);
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

static gchar *
run_json(VentureEntity *run)
{
	g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(run), TRUE);

	return json_to_string(node, FALSE);
}

static VentureSeriesStore *
reader_of(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(GError) error = NULL;
	VentureSeriesStore *reader;

	reader = venture_feeds_service_open_reader(service_of(fixture), source_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reader);

	return reader;
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

	return NULL != memmem(contents, length, needle, strlen(needle));
}

/* ==========================================================================
 * odds-api.c, compiled by crispy
 * ========================================================================== */

/* Compiles and loads the script, or says why the test cannot. */
static gboolean
load_odds_api(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *compiler = NULL;

	compiler = g_find_program_in_path("gcc");

	if ((NULL == compiler) || !g_file_test(VENTURE_DEV_INCLUDE_DIR, G_FILE_TEST_IS_DIR))
	{
		g_test_skip("no compiler, or no development include tree, to compile a crispy plugin");
		return FALSE;
	}

	path = g_build_filename(VENTURE_TEST_PLUGIN_SOURCES, "scripts", "odds-api.c", NULL);

	if (!venture_plugin_manager_load_file(fixture->manager, path, &error))
		g_error("odds-api.c did not load: %s", error->message);

	g_assert_nonnull(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "odds_api"));

	return TRUE;
}

/*
 * The odds endpoint, with the quota headers the real one sends, wanting
 * the key in the query string as the real one does. The fixture's two
 * kick-offs are moved to a week and eight days from now: a cover scan
 * leaves out a match that has started, so dates fixed in the file would
 * make the surebet test fail on the day the first one passed. Returns
 * the first match's kick-off as served.
 */
static gchar *
serve_odds(Fixture *fixture)
{
	VentureTestRoute *route;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) body = NULL;
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) first = g_date_time_add_days(now, 7);
	g_autoptr(GDateTime) second = g_date_time_add_days(now, 8);
	g_autofree gchar *contents = NULL;
	g_autofree gchar *path = NULL;
	gchar *first_stamp;
	g_autofree gchar *second_stamp = NULL;

	path = g_build_filename(VENTURE_TEST_FIXTURES, "odds-api", "soccer_epl.json", NULL);
	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_no_error(error);
	first_stamp = g_date_time_format(first, "%Y-%m-%dT%H:%M:%SZ");
	second_stamp = g_date_time_format(second, "%Y-%m-%dT%H:%M:%SZ");
	body = g_string_new(contents);
	g_assert_cmpuint(g_string_replace(body, "2026-10-10T14:00:00Z", first_stamp, 0), ==, 1);
	g_assert_cmpuint(g_string_replace(body, "2026-10-11T16:30:00Z", second_stamp, 0), ==, 1);

	route = venture_test_http_route(&fixture->http, "/v4/sports/soccer_epl/odds", 200, body->str);
	route->headers = g_strsplit("x-requests-remaining|480|x-requests-used|20|x-requests-last|3", "|", -1);

	return first_stamp;
}

static gchar *
odds_settings(Fixture *fixture)
{
	return g_strdup_printf("units: [soccer_epl]\nregions: uk\napi_base: %s\nrequests_per_hour: 30\n",
	                       fixture->http.origin);
}

/*
 * odds_api: events are parent instruments called "Home vs Away" with
 * their start time; outcomes their children keyed event:outcome;
 * bookmakers venues grouped by region; prices back quotes in decimal odds
 * dated by their market's last update; a spread market and a malformed
 * price left out and counted; the far end's quota headers recorded on the
 * run in place of its own budget.
 *
 * What breaks if this regresses: the cover strategy has no events to
 * read, or reads a handicap price as a head-to-head one and finds
 * surebets that are not there.
 */
static void
test_odds_api_sync(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *kickoff = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *notes = NULL;
	gint64 id;

	(void)user_data;

	if (!load_odds_api(fixture))
		return;

	kickoff = serve_odds(fixture);
	settings = odds_settings(fixture);
	id = create_source(fixture, "EPL odds", "odds_api", settings, "api_key", api_key);
	run = sync_and_wait(fixture, id);

	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(run, "quota-used"), ==, 20);
	g_assert_cmpint(run_int(run, "quota-limit"), ==, 500);
	g_assert_cmpint(run_int(run, "refused"), ==, 1);
	notes = run_text(run, "notes");
	g_assert_nonnull(strstr(notes, "cost 3"));
	g_assert_nonnull(strstr(notes, "480 left"));

	/* The key went where the API wants it, escaped, with h2h decimal. */
	query = venture_test_http_seen(&fixture->http, "/v4/sports/soccer_epl/odds", "?");
	g_assert_nonnull(strstr(query, "apiKey=odds-KEY-5b1c9e"));
	g_assert_nonnull(strstr(query, "markets=h2h"));
	g_assert_nonnull(strstr(query, "oddsFormat=decimal"));
	g_assert_nonnull(strstr(query, "regions=uk"));

	reader = reader_of(fixture, id);

	{
		g_autoptr(VentureSeriesInstrumentRow) event = NULL;
		g_autoptr(VentureSeriesInstrumentRow) outcome = NULL;

		g_assert_true(venture_series_store_get_instrument(reader, "ev-ars-che", &event, &error));
		g_assert_nonnull(event);
		g_assert_cmpstr(event->name, ==, "Arsenal vs Chelsea");
		g_assert_cmpstr(event->kind, ==, "event");
		g_assert_nonnull(strstr(event->attrs_json, kickoff));

		g_assert_true(venture_series_store_get_instrument(reader, "ev-ars-che:Draw", &outcome, &error));
		g_assert_nonnull(outcome);
		g_assert_cmpstr(outcome->kind, ==, "outcome");
		g_assert_cmpstr(outcome->parent_key, ==, "ev-ars-che");
	}

	{
		g_autoptr(GPtrArray) venues = venture_series_store_list_venues(reader, &error);
		guint i;
		guint bookmakers = 0;

		g_assert_no_error(error);

		for (i = 0; i < venues->len; i++)
		{
			VentureSeriesVenueRow *venue = g_ptr_array_index(venues, i);

			g_assert_cmpstr(venue->kind, ==, "bookmaker");
			g_assert_cmpstr(venue->group_key, ==, "uk");

			if (0 == g_strcmp0(venue->key, "betfair_sb_uk"))
				g_assert_cmpstr(venue->name, ==, "Betfair Sportsbook");

			bookmakers++;
		}

		g_assert_cmpuint(bookmakers, ==, 2);
	}

	{
		g_autoptr(GPtrArray) quotes = NULL;
		g_autoptr(GDateTime) updated = g_date_time_new_from_iso8601("2026-10-04T09:59:30Z", NULL);
		guint i;
		gboolean seen_betfair = FALSE;

		quotes = venture_series_store_list_quotes(reader, "ev-ars-che:Chelsea", NULL, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(quotes->len, ==, 2);

		for (i = 0; i < quotes->len; i++)
		{
			VentureSeriesQuoteRow *quote = g_ptr_array_index(quotes, i);

			g_assert_cmpint(quote->side, ==, VENTURE_SERIES_QUOTE_BACK);
			g_assert_cmpstr(quote->currency, ==, "");

			if (0 == g_strcmp0(quote->venue_key, "betfair_sb_uk"))
			{
				g_assert_cmpint(quote->value, ==, 4200000);
				g_assert_cmpint(quote->taken_at, ==, g_date_time_to_unix(updated));
				seen_betfair = TRUE;
			}
			else
				g_assert_cmpint(quote->value, ==, 3600000);
		}

		g_assert_true(seen_betfair);
	}

	/* The bad Draw price at Betfair is not stored as anything. */
	{
		g_autoptr(GPtrArray) draw = venture_series_store_list_quotes(reader, "ev-liv-eve:Draw", NULL, &error);

		g_assert_cmpuint(draw->len, ==, 1);
	}
}

/*
 * The key travels in a query string because the API insists, and goes
 * nowhere else: not into a run (good or refused), not into the store's
 * bytes, not into a log line. A refused key fails the run saying so, and
 * a missing one says where to set it.
 *
 * What breaks if this regresses: every viewer of a data source run reads
 * a paid API key.
 */
static void
test_odds_api_key_never_leaks(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) good = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(VentureEntity) missing = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *store = NULL;
	g_autofree gchar *wal = NULL;
	GLogFunc previous;
	gint64 id;
	gint64 wrong;
	gint64 none;

	(void)user_data;

	if (!load_odds_api(fixture))
		return;

	g_free(serve_odds(fixture));
	settings = odds_settings(fixture);

	g_mutex_lock(&captured_lock);
	captured_log = g_string_new(NULL);
	g_mutex_unlock(&captured_lock);
	previous = g_log_set_default_handler(capture_log, NULL);

	id = create_source(fixture, "EPL odds", "odds_api", settings, "api_key", api_key);
	good = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(good), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

	/* A key the far end refuses (the route wants another path's key). */
	{
		VentureTestRoute *route = venture_test_http_route(&fixture->http, "/v4/sports/soccer_epl/odds", 401,
		                                                  "{\"message\":\"Invalid key odds-KEY-5b1c9e\"}");

		(void)route;
	}

	wrong = create_source(fixture, "Wrong key", "odds_api", settings, "api_key", api_key);
	refused = sync_and_wait(fixture, wrong);
	message = run_text(refused, "error");
	g_assert_cmpint(run_status(refused), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "refused the key (HTTP 401)"));
	g_assert_null(strstr(message, api_key));

	g_clear_pointer(&message, g_free);
	none = create_source(fixture, "No key", "odds_api", settings, NULL, NULL);
	missing = sync_and_wait(fixture, none);
	message = run_text(missing, "error");
	g_assert_nonnull(strstr(message, "api_key"));

	g_log_set_default_handler(previous, NULL);

	json = run_json(good);
	g_assert_null(strstr(json, api_key));
	g_clear_pointer(&json, g_free);
	json = run_json(refused);
	g_assert_null(strstr(json, api_key));

	g_mutex_lock(&captured_lock);
	g_assert_null(strstr(captured_log->str, api_key));
	g_string_free(captured_log, TRUE);
	captured_log = NULL;
	g_mutex_unlock(&captured_lock);

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, id, &error);
	store = g_build_filename(fixture->state_dir, "series", venture_entity_get_uuid(source), "store.db", NULL);
	wal = g_strconcat(store, "-wal", NULL);
	g_assert_true(g_file_test(store, G_FILE_TEST_EXISTS));
	g_assert_false(file_contains(store, api_key));
	g_assert_false(file_contains(wal, api_key));
}

/*
 * A cover scan over the synced odds finds the surebet the fixture
 * planted -- Arsenal v Chelsea across two bookmakers, S = 0.977 -- and not
 * the near miss, whose best odds sum to more than one.
 *
 * What breaks if this regresses: the odds feed and the scan stop
 * speaking the same shape (parent instruments, outcome children, back
 * quotes), and the surebet page is empty with a full store behind it.
 */
static void
test_odds_api_cover(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(JsonNode) options_node = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *options = NULL;
	g_autofree gchar *surebet = NULL;
	g_autofree gchar *near_miss = NULL;
	JsonArray *rows;
	JsonObject *found = NULL;
	gint64 id;
	guint i;

	(void)user_data;

	if (!load_odds_api(fixture))
		return;

	g_free(serve_odds(fixture));
	settings = odds_settings(fixture);
	id = create_source(fixture, "EPL odds", "odds_api", settings, "api_key", api_key);
	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

	options = g_strdup_printf("{\"strategy\":\"cover\",\"total_stake\":\"100.00 USD\","
	                          "\"data_source_id\":%" G_GINT64_FORMAT "}", id);
	options_node = json_from_string(options, NULL);
	answer = venture_arbitrage_scan_run(fixture->context, fixture->org, json_node_get_object(options_node),
	                                    &error);
	g_assert_no_error(error);
	rows = json_object_get_array_member(json_node_get_object(answer), "rows");
	surebet = g_strdup_printf("cover:%" G_GINT64_FORMAT ":ev-ars-che", id);
	near_miss = g_strdup_printf("cover:%" G_GINT64_FORMAT ":ev-liv-eve", id);

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		const gchar *key = json_object_get_string_member(row, "key");

		if (0 == g_strcmp0(key, surebet))
			found = row;

		if (0 == g_strcmp0(key, near_miss))
			g_assert_false(json_object_get_boolean_member(row, "is_surebet"));
	}

	g_assert_nonnull(found);
	g_assert_true(json_object_get_boolean_member(found, "is_surebet"));
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(found, "legs")), ==, 3);
}

/* ==========================================================================
 * supplier-csv, run as an exec plugin
 * ========================================================================== */

static void
load_supplier_csv(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *manifest = NULL;

	manifest = g_build_filename(VENTURE_TEST_PLUGIN_SOURCES, "exec", "supplier-csv",
	                            "supplier-csv.plugin.yaml", NULL);

	if (!venture_plugin_manager_load_file(fixture->manager, manifest, &error))
		g_error("supplier-csv did not load: %s", error->message);

	g_assert_nonnull(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "supplier_csv"));
}

/*
 * supplier_csv: the sample price list becomes a supplier venue, a SKU
 * instrument per line (quoted names with commas and doubled quotes read
 * whole), and a listing per SKU in stock at its landed price -- price plus
 * shipping, added exactly -- in one complete snapshot; a SKU with no stock
 * is out of stock. include_shipping: false lists the bare price. And it
 * stops at plugins.allow_exec off, and at a file outside its directory.
 *
 * What breaks if this regresses: a dropshipping scan buys at a price
 * without the shipping it really pays, or an editor's setting reads a
 * file the server's user can read and the operator never published.
 */
static void
test_supplier_csv(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) bare_run = NULL;
	g_autoptr(VentureEntity) outside = NULL;
	g_autoptr(VentureEntity) off = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *message = NULL;
	gint64 id;
	gint64 bare;
	gint64 escape;

	(void)user_data;

	g_object_set(fixture->config, "plugins-allow-exec", TRUE, NULL);
	load_supplier_csv(fixture);

	id = create_source(fixture, "Acme", "supplier_csv",
	                   "venue: acme\nvenue_name: Acme Wholesale\ngroup: us\n", NULL, NULL);
	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	reader = reader_of(fixture, id);

	{
		g_autoptr(VentureSeriesRow) board = NULL;
		g_autoptr(VentureSeriesRow) cone = NULL;
		g_autoptr(VentureSeriesRow) towel = NULL;
		g_autoptr(VentureSeriesRow) trivet = NULL;

		g_assert_true(venture_series_store_get_current(reader, "acme", "DS-1001", &board, &error));
		g_assert_nonnull(board);
		g_assert_cmpint(board->min_price, ==, 835);
		g_assert_cmpint(board->quantity, ==, 120);
		g_assert_cmpstr(board->currency, ==, "USD");
		g_assert_cmpstr(board->venue_name, ==, "Acme Wholesale");
		g_assert_cmpstr(board->group_key, ==, "us");

		g_assert_true(venture_series_store_get_current(reader, "acme", "DS-1004", &cone, &error));
		g_assert_cmpstr(cone->instrument_name, ==, "Ceramic pour-over cone \"V60\" style");
		g_assert_cmpint(cone->min_price, ==, 685);

		/* Free shipping is the bare price. */
		g_assert_true(venture_series_store_get_current(reader, "acme", "DS-1006", &towel, &error));
		g_assert_cmpint(towel->min_price, ==, 235);

		/* No stock: no listing, and nothing in stock to buy. */
		g_assert_true(venture_series_store_get_current(reader, "acme", "DS-1005", &trivet, &error));
		g_assert_true((NULL == trivet) || (trivet->quantity <= 0));
	}

	{
		g_autoptr(VentureSeriesInstrumentRow) mat = NULL;
		g_autoptr(GPtrArray) venues = venture_series_store_list_venues(reader, &error);

		g_assert_true(venture_series_store_get_instrument(reader, "DS-1002", &mat, &error));
		g_assert_cmpstr(mat->name, ==, "Silicone baking mat, set of 2");
		g_assert_cmpstr(mat->kind, ==, "sku");
		g_assert_nonnull(strstr(mat->attrs_json, "\"shipping\":\"0.85\""));
		g_assert_cmpuint(venues->len, ==, 1);
		g_assert_cmpstr(((VentureSeriesVenueRow *)g_ptr_array_index(venues, 0))->kind, ==, "supplier");
	}

	/* Without shipping: the supplier's own price. */
	bare = create_source(fixture, "Acme bare", "supplier_csv", "venue: acme\ninclude_shipping: false\n",
	                     NULL, NULL);
	bare_run = sync_and_wait(fixture, bare);
	g_assert_cmpint(run_status(bare_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	{
		g_autoptr(VentureSeriesStore) bare_reader = reader_of(fixture, bare);
		g_autoptr(VentureSeriesRow) board = NULL;

		g_assert_true(venture_series_store_get_current(bare_reader, "acme", "DS-1001", &board, &error));
		g_assert_cmpint(board->min_price, ==, 640);
	}

	/* A file outside the plugin's directory is refused by the program. */
	escape = create_source(fixture, "Escape", "supplier_csv", "file: ../../../tests/fixtures/schema.json\n",
	                       NULL, NULL);
	outside = sync_and_wait(fixture, escape);
	message = run_text(outside, "error");
	g_assert_cmpint(run_status(outside), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "plain name in the plugin's directory"));

	/* And plugins.allow_exec off stops it at the next run. */
	g_clear_pointer(&message, g_free);
	g_object_set(fixture->config, "plugins-allow-exec", FALSE, NULL);
	off = sync_and_wait(fixture, id);
	message = run_text(off, "error");
	g_assert_cmpint(run_status(off), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "plugins.allow_exec is off"));
}

/*
 * With plugins.allow_exec off from the start, the manifest does not load
 * at all, so no supplier_csv provider exists to name.
 *
 * What breaks if this regresses: an install that never opted in runs a
 * program because a directory happened to hold one.
 */
static void
test_supplier_csv_needs_allow_exec(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *manifest = NULL;

	(void)user_data;

	g_object_set(fixture->config, "plugins-allow-exec", FALSE, NULL);
	manifest = g_build_filename(VENTURE_TEST_PLUGIN_SOURCES, "exec", "supplier-csv",
	                            "supplier-csv.plugin.yaml", NULL);

	g_assert_false(venture_plugin_manager_load_file(fixture->manager, manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_null(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "supplier_csv"));
}

#endif /* VENTURE_HAVE_SQLITE */

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

#ifdef VENTURE_HAVE_SQLITE
#define ADD(name, fn) \
	g_test_add("/plugin-examples/" name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)

	ADD("odds-api/sync", test_odds_api_sync);
	ADD("odds-api/key-never-leaks", test_odds_api_key_never_leaks);
	ADD("odds-api/cover-finds-the-surebet", test_odds_api_cover);
	ADD("supplier-csv/sync", test_supplier_csv);
	ADD("supplier-csv/needs-allow-exec", test_supplier_csv_needs_allow_exec);
#undef ADD
#endif

	return g_test_run();
}
