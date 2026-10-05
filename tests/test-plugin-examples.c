/*
 * test-plugin-examples.c - The reference data source plugins that need no
 *                          build step: odds-api.c (crispy) and supplier-csv
 *                          (exec), and tsmctl (exec, around a fake tsmctl)
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
	             "feeds-allow-endpoint-overrides", TRUE,
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

		json_object_set_string_member(values, secret_name, secret);
		binding = venture_feeds_set_credentials(fixture->context, VENTURE_ENTITY(source), values, 0,
		                                        NULL, &error);
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

	/* The script names its service, and the scan that read its odds
	 * carries the line for whatever shows them. */
	{
		JsonArray *lines = json_object_get_array_member(json_node_get_object(answer), "attribution");

		g_assert_cmpuint(json_array_get_length(lines), ==, 1);
		g_assert_cmpstr(json_array_get_string_element(lines, 0), ==,
		                "Odds data from The Odds API (the-odds-api.com).");
	}
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

	/* The operator's own price list owes nobody a credit line: the
	 * manifest declares none, and a page of it shows none. */
	g_assert_null(venture_data_source_provider_get_attribution(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "supplier_csv")));
	{
		g_autoptr(JsonNode) browsed = NULL;
		VentureMarketdataBrowseQuery query;

		venture_marketdata_browse_query_init(&query);
		query.organization_id = fixture->org;
		query.data_source_id = id;
		browsed = venture_marketdata_browse(fixture->context, &query, &error);
		g_assert_no_error(error);
		g_assert_true(json_object_get_boolean_member(json_node_get_object(browsed), "available"));
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
			json_node_get_object(browsed), "attribution")), ==, 0);
	}

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

/* ==========================================================================
 * tsmctl, run as an exec plugin around a fake tsmctl
 * ========================================================================== */

/*
 * A stand-in for the dotfiles' tsmctl: it writes its argv and the two
 * environment variables the test cares about beside itself, then copies
 * the fixture export to the --file it was given. With --account=BROKEN it
 * writes half an export -- a snapshot that would empty an account's
 * positions -- and fails the way an ssh outage does.
 */
static const gchar fake_tsmctl[] =
	"#!/usr/bin/env bash\n"
	"set -euo pipefail\n"
	"here=\"$(CDPATH='' cd -- \"$(dirname -- \"${BASH_SOURCE[0]}\")\" && pwd)\"\n"
	"printf '%%s\\n' \"$@\" > \"${here}/args\"\n"
	"printf 'SSH_AUTH_SOCK=%%s\\nSECRET=%%s\\n' \"${SSH_AUTH_SOCK:-}\" "
	"\"${VENTURE_TEST_SERVER_SECRET:-}\" > \"${here}/env\"\n"
	"file=''\n"
	"broken=0\n"
	"for arg in \"$@\"\n"
	"do\n"
	"    case \"${arg}\" in\n"
	"        --file=*) file=\"${arg#--file=}\" ;;\n"
	"        --account=BROKEN) broken=1 ;;\n"
	"    esac\n"
	"done\n"
	"if (( broken ))\n"
	"then\n"
	"    printf '%%s\\n' '{\"type\":\"account_snapshot\",\"account\":\"Drgold-Thorium Brotherhood\","
	"\"at\":\"2026-10-01T00:00:00Z\",\"covers\":[\"positions\"]}' > \"${file}\"\n"
	"    echo 'tsmctl: ssh: connect to host mob-zach port 22: Connection refused' >&2\n"
	"    exit 3\n"
	"fi\n"
	"cp -- '%s' \"${file}\"\n";

typedef struct
{
	gchar	*bin;		/* the directory holding the fake */
	gchar	*saved_path;	/* the test's own PATH, put back after load */
} FakeTsmctl;

/*
 * Loads plugins/exec/tsmctl with PATH pointing at a directory that holds
 * the fake (or, with @with_fake FALSE, at one that does not). The exec
 * runtime captures PATH when the plugin loads, so PATH is the test's own
 * again straight after.
 */
static void
load_tsmctl(
	Fixture		*fixture,
	FakeTsmctl	*fake,
	gboolean	 with_fake
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *manifest = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *fixture_file = NULL;
	g_autofree gchar *search = NULL;

	fake->bin = g_build_filename(fixture->state_dir, "bin", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fake->bin, 0700), ==, 0);

	if (with_fake)
	{
		fixture_file = g_build_filename(VENTURE_TEST_FIXTURES, "tsmctl", "export.jsonl", NULL);
		script = g_strdup_printf(fake_tsmctl, fixture_file);
		path = g_build_filename(fake->bin, "tsmctl", NULL);
		g_assert_true(g_file_set_contents(path, script, -1, &error));
		g_assert_no_error(error);
		g_assert_cmpint(g_chmod(path, 0755), ==, 0);
	}

	fake->saved_path = g_strdup(g_getenv("PATH"));
	search = g_strdup_printf("%s:/usr/bin:/bin", fake->bin);
	g_setenv("PATH", search, TRUE);
	g_setenv("SSH_AUTH_SOCK", "/run/user/test/ssh-agent.socket", TRUE);
	g_setenv("VENTURE_TEST_SERVER_SECRET", "server-only-secret-value", TRUE);

	g_object_set(fixture->config, "plugins-allow-exec", TRUE, NULL);
	manifest = g_build_filename(VENTURE_TEST_PLUGIN_SOURCES, "exec", "tsmctl", "tsmctl.plugin.yaml", NULL);

	if (!venture_plugin_manager_load_file(fixture->manager, manifest, &error))
		g_error("tsmctl did not load: %s", error->message);

	if (NULL != fake->saved_path)
		g_setenv("PATH", fake->saved_path, TRUE);
	else
		g_unsetenv("PATH");

	g_unsetenv("SSH_AUTH_SOCK");
	g_unsetenv("VENTURE_TEST_SERVER_SECRET");

	g_assert_nonnull(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "tsmctl"));
}

static void
fake_tsmctl_clear(FakeTsmctl *fake)
{
	g_clear_pointer(&fake->bin, g_free);
	g_clear_pointer(&fake->saved_path, g_free);
}

/* GOLD as tsmctl writes it: four places, a copper a ten-thousandth. */
static void
register_gold(Fixture *fixture)
{
	g_autoptr(VentureEntity) gold = NULL;
	g_autoptr(GError) error = NULL;

	gold = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_CURRENCY, NULL));
	g_object_set(gold, "code", "GOLD", "name", "Gold", "exponent", (gint64)4, NULL);
	g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);
}

static gint64
create_gold_source(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*settings
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", name, "provider", "tsmctl", "settings", settings, "schedule", "manual",
	             "currency", "GOLD", NULL);

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error))
		g_error("%s: %s", name, error->message);

	return venture_entity_get_id(VENTURE_ENTITY(source));
}

static gchar *
fake_file(
	FakeTsmctl	*fake,
	const gchar	*name
){
	g_autofree gchar *path = g_build_filename(fake->bin, name, NULL);
	gchar *contents = NULL;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return NULL;

	return contents;
}

/*
 * tsmctl: the provider runs `tsmctl export --format venture` from the
 * server's PATH with the source's settings as --option=value arguments,
 * and the export lands in the store whole: two accounts (a character and
 * the warband bank), a balance in copper-exact gold, holdings, a position,
 * mail, four ledger rows and the AuctionDB figures -- the source's own
 * region sale rate among them. The program gets the ssh agent's socket
 * and never the server's other environment, and the provider names TSM
 * as the source without claiming an endorsement.
 *
 * What breaks if this regresses: a pulled source stores nothing (or a
 * different account set than a push of the same export), a setting
 * stops reaching tsmctl, a remote WoW install cannot be read because ssh
 * has no agent, or the server's secrets reach a program it runs.
 */
static void
test_tsmctl_sync(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	FakeTsmctl fake = { NULL, NULL };
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(VentureSeriesAccountRow) drgold = NULL;
	g_autoptr(VentureSeriesAccountRow) warbank = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GPtrArray) positions = NULL;
	g_autoptr(GPtrArray) inbound = NULL;
	g_autoptr(VentureSeriesRow) copper = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *env = NULL;
	g_auto(GStrv) argv = NULL;
	const gchar *attribution;
	gint64 txns = 0;
	gint64 id;

	(void)user_data;

	register_gold(fixture);
	load_tsmctl(fixture, &fake, TRUE);

	/* Accurate about where the data came from, and disclaiming the rest. */
	attribution = venture_data_source_provider_get_attribution(venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "tsmctl"));
	g_assert_nonnull(attribution);
	g_assert_nonnull(strstr(attribution, "TradeSkillMaster's AuctionDB"));
	g_assert_nonnull(strstr(attribution, "DataStore and Syndicator"));
	g_assert_nonnull(strstr(attribution, "Not affiliated with or endorsed by"));

	id = create_gold_source(fixture, "WoW (tsmctl)",
	                        "accounts: [ZAKMANN, \"123456789#1\"]\nsources: tsm,datastore\nmarket: scope\n"
	                        "since: 90d\ncurrency: gold\ninclude_internal: true\n");
	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(run, "refused"), ==, 0);

	/* What tsmctl was asked, exactly, and in what environment. */
	args = fake_file(&fake, "args");
	g_assert_nonnull(args);
	argv = g_strsplit(args, "\n", -1);
	g_assert_cmpstr(argv[0], ==, "export");
	g_assert_cmpstr(argv[1], ==, "--format");
	g_assert_cmpstr(argv[2], ==, "venture");
	g_assert_cmpstr(argv[3], ==, "--account=ZAKMANN");
	g_assert_cmpstr(argv[4], ==, "--account=123456789#1");
	g_assert_cmpstr(argv[5], ==, "--sources=tsm,datastore");
	g_assert_cmpstr(argv[6], ==, "--market=scope");
	g_assert_cmpstr(argv[7], ==, "--since=90d");
	g_assert_cmpstr(argv[8], ==, "--currency=GOLD");
	g_assert_cmpstr(argv[9], ==, "--include-internal");
	g_assert_true(g_str_has_prefix(argv[10], "--file="));
	g_assert_cmpstr(argv[11], ==, "");

	env = fake_file(&fake, "env");
	g_assert_nonnull(strstr(env, "SSH_AUTH_SOCK=/run/user/test/ssh-agent.socket\n"));
	g_assert_nonnull(strstr(env, "SECRET=\n"));

	/* The private export file is gone with the run. */
	{
		const gchar *file = argv[10] + strlen("--file=");
		g_autofree gchar *directory = g_path_get_dirname(file);

		g_assert_false(g_file_test(directory, G_FILE_TEST_EXISTS));
	}

	reader = reader_of(fixture, id);
	accounts = venture_series_store_list_accounts(reader, NULL, NULL, NULL, 1790000000, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(accounts->len, ==, 2);

	g_assert_true(venture_series_store_get_account(reader, "Drgold-Thorium Brotherhood", 1790000000,
	                                               &drgold, &error));
	g_assert_nonnull(drgold);
	g_assert_cmpstr(drgold->kind, ==, "character");
	g_assert_cmpstr(drgold->venue_key, ==, "thorium-brotherhood");
	g_assert_cmpint(drgold->holdings, ==, 4);
	g_assert_cmpint(drgold->positions, ==, 1);
	g_assert_cmpint(drgold->inbound, ==, 1);
	g_assert_cmpuint(drgold->balances->len, ==, 1);
	g_assert_cmpstr(g_array_index(drgold->balances, VentureSeriesAmount, 0).currency, ==, "GOLD");
	g_assert_cmpint(g_array_index(drgold->balances, VentureSeriesAmount, 0).amount, ==, 272495018);

	g_assert_true(venture_series_store_get_account(reader, "warbank:ZAKMANN", 1790000000, &warbank, &error));
	g_assert_nonnull(warbank);
	g_assert_cmpstr(warbank->kind, ==, "shared");
	g_assert_cmpint(warbank->holdings, ==, 1);

	positions = venture_series_store_list_positions(reader, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(positions->len, ==, 1);
	g_assert_cmpstr(((VentureSeriesPositionRow *)g_ptr_array_index(positions, 0))->key, ==, "1637378752");
	g_assert_cmpint(((VentureSeriesPositionRow *)g_ptr_array_index(positions, 0))->unit_price, ==, 8198);
	g_assert_cmpint(((VentureSeriesPositionRow *)g_ptr_array_index(positions, 0))->bid, ==, 7500);

	inbound = venture_series_store_list_inbound(reader, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(inbound->len, ==, 1);
	g_assert_cmpint(((VentureSeriesInboundRow *)g_ptr_array_index(inbound, 0))->money, ==, 155800);

	g_assert_true(venture_series_store_count_txns(reader, NULL, &txns, &error));
	g_assert_cmpint(txns, ==, 4);

	/* AuctionDB: the realm's market value, and TSM's region figures. */
	g_assert_true(venture_series_store_get_current(reader, "thorium-brotherhood", "2770", &copper, &error));
	g_assert_nonnull(copper);
	g_assert_cmpint(copper->market_value, ==, 8500);
	g_clear_pointer(&copper, venture_series_row_free);
	g_assert_true(venture_series_store_get_current(reader, "region-us", "2770", &copper, &error));
	g_assert_nonnull(copper);
	g_assert_cmpfloat_with_epsilon(copper->source_sale_rate, 0.31, 1e-9);
	g_assert_cmpfloat_with_epsilon(copper->source_sold_per_day, 1840.5, 1e-9);

	fake_tsmctl_clear(&fake);
	venture_currency_clear_registered();
}

/*
 * A failed export stores nothing. The fake writes half an export -- a
 * positions snapshot for a character with no position after it, which
 * applied would empty that character's auctions -- and exits 3; the run
 * fails naming the status, and the position the earlier sync stored is
 * still there. Settings that are not what they claim to be (an option
 * dressed as an account, an unknown market scope) are refused before
 * tsmctl runs at all.
 *
 * What breaks if this regresses: an ssh outage half way through an export
 * reads as "every auction sold", or a data source's settings inject an
 * option -- --host, say -- into the program VENTURE runs.
 */
static void
test_tsmctl_failure_stores_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	FakeTsmctl fake = { NULL, NULL };
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) broken_run = NULL;
	g_autoptr(VentureEntity) injected_run = NULL;
	g_autoptr(VentureEntity) market_run = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GPtrArray) positions = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *args_path = NULL;
	gint64 id;
	gint64 injected;
	gint64 market;

	(void)user_data;

	register_gold(fixture);
	load_tsmctl(fixture, &fake, TRUE);

	id = create_gold_source(fixture, "WoW (tsmctl)", "market: none\n");
	first = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(first), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

	/* The same source, now pointed at the account the fake fails on. */
	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, id, &error);
	g_assert_no_error(error);
	g_object_set(source, "settings", "accounts: BROKEN\n", NULL);
	g_assert_true(venture_database_save(fixture->database, source, NULL, &error));
	g_assert_no_error(error);

	broken_run = sync_and_wait(fixture, id);
	message = run_text(broken_run, "error");
	g_assert_cmpint(run_status(broken_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "tsmctl export exited with status 3"));

	reader = reader_of(fixture, id);
	positions = venture_series_store_list_positions(reader, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(positions->len, ==, 1);

	/* Refused before tsmctl runs: no args file is written. */
	args_path = g_build_filename(fake.bin, "args", NULL);
	g_assert_cmpint(g_unlink(args_path), ==, 0);

	injected = create_gold_source(fixture, "Injected", "accounts: [\"--host=evil.example\"]\n");
	injected_run = sync_and_wait(fixture, injected);
	g_clear_pointer(&message, g_free);
	message = run_text(injected_run, "error");
	g_assert_cmpint(run_status(injected_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "is not a WoW account folder name"));

	market = create_gold_source(fixture, "Bad market", "market: everything\n");
	market_run = sync_and_wait(fixture, market);
	g_clear_pointer(&message, g_free);
	message = run_text(market_run, "error");
	g_assert_cmpint(run_status(market_run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "market must be scope, all or none"));

	g_assert_false(g_file_test(args_path, G_FILE_TEST_EXISTS));

	fake_tsmctl_clear(&fake);
	venture_currency_clear_registered();
}

/*
 * With no tsmctl on the server's PATH the plugin still loads -- nothing
 * runs at load -- and a sync fails saying so, with the PATH it searched
 * and the push alternative.
 *
 * What breaks if this regresses: an install without the dotfiles gets a
 * bare "exit 127" instead of what to do about it.
 */
static void
test_tsmctl_missing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	FakeTsmctl fake = { NULL, NULL };
	g_autoptr(VentureEntity) run = NULL;
	g_autofree gchar *message = NULL;
	gint64 id;

	(void)user_data;

	register_gold(fixture);
	load_tsmctl(fixture, &fake, FALSE);

	id = create_gold_source(fixture, "WoW (tsmctl)", "");
	run = sync_and_wait(fixture, id);
	message = run_text(run, "error");
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "tsmctl is not on the server's PATH"));
	g_assert_nonnull(strstr(message, "tsmctl venture push"));

	fake_tsmctl_clear(&fake);
	venture_currency_clear_registered();
}

#endif /* VENTURE_HAVE_SQLITE */

/*
 * odds_api's api_base replaces the address its key is sent to. It is
 * honoured only with feeds.allow_endpoint_overrides, which is for test
 * and development: with the switch off a source naming one is refused
 * when it is saved, and one saved while it was on fails its run without
 * a request. A source with no api_base uses the official address and
 * saves either way.
 *
 * What breaks if this regresses: whoever may edit a data source points
 * api_base at another allowlisted origin and reads the paid key there.
 */
static void
test_odds_api_override_needs_the_switch(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDataSource) refused = NULL;
	g_autoptr(VentureDataSource) plain = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *message = NULL;
	gint64 id;

	(void)user_data;

	if (!load_odds_api(fixture))
		return;

	g_free(serve_odds(fixture));
	settings = odds_settings(fixture);
	g_object_set(fixture->config, "feeds-allow-endpoint-overrides", FALSE, NULL);

	refused = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(refused), fixture->org);
	g_object_set(refused, "name", "Redirected", "provider", "odds_api", "settings", settings,
	             "schedule", "manual", NULL);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(refused), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "feeds.allow_endpoint_overrides"));
	g_assert_nonnull(strstr(error->message, "api_base"));
	g_clear_error(&error);

	plain = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(plain), fixture->org);
	g_object_set(plain, "name", "Official", "provider", "odds_api",
	             "settings", "units: [soccer_epl]\n", "schedule", "manual", NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(plain), NULL, &error));
	g_assert_no_error(error);

	/* Saved while the switch was on, then the switch goes off: an edit
	 * that leaves api_base alone still saves, and the run refuses. */
	g_object_set(fixture->config, "feeds-allow-endpoint-overrides", TRUE, NULL);
	id = create_source(fixture, "Was allowed", "odds_api", settings, "api_key", api_key);
	g_object_set(fixture->config, "feeds-allow-endpoint-overrides", FALSE, NULL);
	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, id, &error);
	g_assert_no_error(error);
	g_object_set(source, "name", "Was allowed, renamed", NULL);
	g_assert_true(venture_database_save(fixture->database, source, NULL, &error));
	g_assert_no_error(error);

	run = sync_and_wait(fixture, id);
	message = run_text(run, "error");
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "feeds.allow_endpoint_overrides"));
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/v4/sports/soccer_epl/odds"), ==, 0);
}

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
	ADD("odds-api/override-needs-the-switch", test_odds_api_override_needs_the_switch);
	ADD("supplier-csv/sync", test_supplier_csv);
	ADD("supplier-csv/needs-allow-exec", test_supplier_csv_needs_allow_exec);
	ADD("tsmctl/sync", test_tsmctl_sync);
	ADD("tsmctl/failure-stores-nothing", test_tsmctl_failure_stores_nothing);
	ADD("tsmctl/missing", test_tsmctl_missing);
#undef ADD
#endif

	return g_test_run();
}
