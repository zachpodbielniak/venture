/*
 * test-plugin-blizzard.c - The Blizzard auction house plugin, end to end
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The plugin is built into $(OUTDIR)/plugins-optional/ -- deliberately not
 * where VENTURE_PLUGIN_PATH points every other fixture -- and each test
 * here loads it by path into a context of its own, with feeds on and a
 * GOLD currency (exponent 4) registered as an operator would.
 *
 * Battle.net is a scripted server (tests/venture-test-http.h) on port 0
 * serving the JSON in tests/fixtures/blizzard/, named in the source's
 * api_base and oauth_base and on feeds.allowed_origins: the same
 * deny-by-default allowlist a real install uses. Every request the
 * provider makes goes through the feeds HTTP helper, so these tests run
 * the real transport, the real worker thread and the real store.
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

#define FIXTURES VENTURE_TEST_FIXTURES "/blizzard"

/* What the scripted Battle.net accepts and issues. */
static const gchar client_id[] = "venture-client";
static const gchar client_secret[] = "client-SECRET-9911";
static const gchar access_token[] = "tok-BLIZZARD-SECRET-7731";

/* The two snapshots' Last-Modified, an hour apart. */
static const gchar first_modified[] = "Sat, 03 Oct 2026 10:00:00 GMT";
static const gchar later_modified[] = "Sat, 03 Oct 2026 11:00:00 GMT";

/* Keys the provider makes from the fixture's auctions. */
#define KEY_ORE		"2770"
#define KEY_CLOTH	"2589"
#define KEY_BLADE	"19019:b1472,6646:m9=70,28=2"
#define KEY_PET		"82800:p39.5"

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

/* Every log line, for the test that says the token never reaches one. */
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
save(
	Fixture		*fixture,
	VentureEntity	*entity
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, entity, NULL, &error))
		g_error("save %s: %s", venture_entity_get_display_name(entity), error->message);
}

static VentureEntity *
record(
	Fixture		*fixture,
	const gchar	*type
){
	g_autoptr(GError) error = NULL;
	VentureEntity *entity;

	entity = venture_entity_registry_create(venture_entity_registry_get_default(), type, &error);
	g_assert_no_error(error);
	venture_entity_set_organization_id(entity, fixture->org);

	return entity;
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) key = NULL;
	g_autofree gchar *plugin = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-blizzard-XXXXXX", &error);
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
	             "feeds-max-response-mb", (gint64)4,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* The registry is process-wide and an earlier test may have masked
	 * the feeds module; with this configuration applied, migrate again. */
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(fixture->database),
	                                                  key, &error));
	g_assert_no_error(error);

	/* GOLD as the operator registers it: copper is a ten-thousandth. */
	{
		g_autoptr(VentureEntity) gold = record(fixture, "currency");

		g_object_set(gold, "code", "GOLD", "name", "Gold", "exponent", (gint64)4, NULL);
		g_assert_true(venture_entity_set_field_from_string(gold, "book-treatment", "separate_book", NULL));
		save(fixture, gold);
	}

	/* The plugin, by path: it is not on VENTURE_PLUGIN_PATH, on purpose. */
	plugin = g_build_filename(VENTURE_TEST_OPTIONAL_PLUGINS, "blizzard-auctions.so", NULL);

	if (!g_file_test(plugin, G_FILE_TEST_EXISTS))
		g_error("%s is not built: `make plugins` builds plugins-optional too", plugin);

	fixture->manager = venture_plugin_manager_new(fixture->context);
	venture_context_set_plugin_manager(fixture->context, fixture->manager);
	g_assert_true(venture_plugin_manager_load_file(fixture->manager, plugin, &error));
	g_assert_no_error(error);
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
 * Battle.net, scripted
 * ========================================================================== */

static gchar *
basic_authorization(void)
{
	g_autofree gchar *pair = g_strdup_printf("%s:%s", client_id, client_secret);
	g_autofree gchar *encoded = g_base64_encode((const guchar *)pair, strlen(pair));

	return g_strconcat("Basic ", encoded, NULL);
}

/* An API path that wants the bearer token. */
static VentureTestRoute *
api(
	Fixture		*fixture,
	const gchar	*path,
	const gchar	*file
){
	VentureTestRoute *route;

	route = venture_test_http_route_file(&fixture->http, path, 200, FIXTURES, file);
	route->require_authorization = g_strconcat("Bearer ", access_token, NULL);

	return route;
}

static void
set_last_modified(
	VentureTestRoute	*route,
	const gchar		*when
){
	g_strfreev(route->headers);
	route->headers = g_new0(gchar *, 3);
	route->headers[0] = g_strdup("Last-Modified");
	route->headers[1] = g_strdup(when);
}

/* The whole of the API these tests need, at its first hour. */
static void
serve_battle_net(Fixture *fixture)
{
	g_autofree gchar *basic = basic_authorization();
	VentureTestRoute *route;

	route = venture_test_http_route_file(&fixture->http, "/token", 200, FIXTURES, "token.json");
	route->require_authorization = g_strdup(basic);

	api(fixture, "/data/wow/connected-realm/index", "connected-realm-index.json");
	api(fixture, "/data/wow/connected-realm/11", "connected-realm-11.json");
	api(fixture, "/data/wow/connected-realm/3676", "connected-realm-3676.json");
	set_last_modified(api(fixture, "/data/wow/connected-realm/11/auctions", "auctions-11.json"),
	                  first_modified);
	set_last_modified(api(fixture, "/data/wow/connected-realm/3676/auctions", "auctions-3676.json"),
	                  first_modified);
	set_last_modified(api(fixture, "/data/wow/auctions/commodities", "commodities.json"), first_modified);
	api(fixture, "/data/wow/item/2770", "item-2770.json");
	api(fixture, "/data/wow/item/2589", "item-2589.json");
	api(fixture, "/data/wow/item/2840", "item-2840.json");
	api(fixture, "/data/wow/item/19019", "item-19019.json");

	/* Thunderfury's icon: its media names a file on the CDN (here, this
	 * server), fetched once into the plugin's cache. */
	{
		g_autofree gchar *media = g_strdup_printf(
			"{\"assets\":[{\"key\":\"icon\",\"value\":\"%s/icons/56/135349.jpg\","
			"\"file_data_id\":135349}]}", fixture->http.origin);
		VentureTestRoute *media_route = venture_test_http_route(&fixture->http,
		                                                        "/data/wow/media/item/19019", 200, media);

		media_route->require_authorization = g_strconcat("Bearer ", access_token, NULL);
		venture_test_http_route(&fixture->http, "/icons/56/135349.jpg", 200, "\xff\xd8\xff fake jpeg");
	}
	api(fixture, "/data/wow/item/82800", "item-82800.json");
	api(fixture, "/data/wow/profession/index", "profession-index.json");
	api(fixture, "/data/wow/profession/164", "profession-164.json");
	api(fixture, "/data/wow/profession/186", "profession-186.json");
	api(fixture, "/data/wow/profession/164/skill-tier/2437", "skill-tier-164-2437.json");
	api(fixture, "/data/wow/profession/186/skill-tier/2572", "skill-tier-186-2572.json");
	api(fixture, "/data/wow/recipe/2657", "recipe-2657.json");
	api(fixture, "/data/wow/recipe/2660", "recipe-2660.json");
	api(fixture, "/data/wow/recipe/2661", "recipe-2661.json");
	api(fixture, "/data/wow/recipe/7777", "recipe-7777.json");
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

/* Lets the main loop take runs back until nothing is pending, bounded. */
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

/* The base settings, pointed at the scripted server, plus @extra. No
 * requests_per_hour: the provider's schema declares 36000, and the
 * quota assertions below are what show a schema default reaches a source
 * that leaves the setting out (it once ran with no budget at all). */
static gchar *
settings_text(
	Fixture		*fixture,
	const gchar	*extra
){
	return g_strdup_printf("region: us\n"
	                       "client_id: %s\n"
	                       "api_base: %s\n"
	                       "oauth_base: %s\n"
	                       "%s",
	                       client_id, fixture->http.origin, fixture->http.origin,
	                       (NULL != extra) ? extra : "");
}

static gint64
create_source(
	Fixture		*fixture,
	const gchar	*extra,
	const gchar	*secret,
	const gchar	*schedule
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = settings_text(fixture, extra);

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "US auction houses", "provider", "blizzard_auctions", "settings", settings,
	             "schedule", schedule, "currency", "GOLD", "venue-namespace", "us-realm",
	             "instrument-namespace", "wow-item", NULL);
	save(fixture, VENTURE_ENTITY(source));

	/* The secret is sealed, never in the settings. */
	if (NULL != secret)
	{
		g_autoptr(VentureIntegrationConnection) binding = NULL;
		g_autoptr(JsonObject) values = json_object_new();

		json_object_set_string_member(values, "client_secret", secret);
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
count_runs(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	gint64 count;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
	g_assert_true(venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ, source_id,
	                                           &error));
	count = venture_database_count(fixture->database, query, &error);
	g_assert_no_error(error);

	return count;
}

/*
 * Puts the source on its own schedule and waits for the scheduled run it
 * makes at once. A manual sync deliberately sends no If-Modified-Since
 * ("fetch it whatever the far end thinks"), so a conditional fetch is
 * only ever seen on a scheduled one. Saving the source refreezes it,
 * which starts its token and name caches afresh.
 */
static VentureEntity *
schedule_and_wait(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) error = NULL;
	gint64 before;
	gint64 deadline;

	settle(fixture);
	before = count_runs(fixture, source_id);
	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, source_id, &error);
	g_assert_no_error(error);
	g_object_set(source, "schedule", "auto", NULL);
	save(fixture, source);
	deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

	while (count_runs(fixture, source_id) <= before)
	{
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	settle(fixture);

	return latest_run(fixture, source_id);
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

static VentureSeriesRow *
current(
	VentureSeriesStore	*reader,
	const gchar		*venue,
	const gchar		*instrument
){
	g_autoptr(GError) error = NULL;
	VentureSeriesRow *row = NULL;

	g_assert_true(venture_series_store_get_current(reader, venue, instrument, &row, &error));
	g_assert_no_error(error);

	return row;
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

	/* memmem: a database file is full of NUL bytes. */
	return NULL != memmem(contents, length, needle, strlen(needle));
}

static gint64
source_unix(const gchar *http_date)
{
	g_autoptr(GDateTime) when = soup_date_time_new_from_http_string(http_date);

	return g_date_time_to_unix(when);
}

/* ==========================================================================
 * Settings
 * ========================================================================== */

/*
 * Settings are judged when the source is saved, and the currency when it
 * is frozen: a region Blizzard does not have, an api_base with a path in
 * it (which would point the token request somewhere other than /token),
 * the client secret typed into the settings, and a currency that is not
 * registered with exponent 4.
 *
 * What breaks if this regresses: a typo in the region syncs nothing and
 * says nothing; a secret in the settings is readable by every viewer of
 * the source; and copper prices land as hundredths of a gold, every
 * figure a hundred times too large.
 */
static void
test_settings(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autofree gchar *message = NULL;
	static const gchar *const bad[] = {
		"region: na\n",
		"api_base: https://evil.example/steal\n",
		"client_secret: in-the-settings\n",
		"connected_realm_ids: [11, -3]\n",
		"include_commodities: maybe\n",
		"locale: english\n",
		"realm_index_hours: 169\n",
		"item_names_per_fetch: 5001\n",
		/*
		 * PCRE's "$" matches just before a single trailing newline as
		 * well as at the true end of the subject, so "en_US\n" passed
		 * "^[a-z]{2}_[A-Z]{2}$" under the default compile options --
		 * and locale goes into the request's query string unescaped,
		 * which is exactly where that stray newline must never reach.
		 * What breaks if this regresses: a crafted locale smuggles a
		 * CRLF into the request this plugin sends to Battle.net.
		 */
		"locale: \"en_US\\n\"\n",
		NULL
	};
	guint i;

	(void)user_data;

	for (i = 0; NULL != bad[i]; i++)
	{
		g_autoptr(VentureDataSource) source = venture_data_source_new();
		g_autofree gchar *settings = NULL;

		/* One bad line beside a good client id: the refusal is that line's. */
		settings = g_strdup_printf("client_id: %s\n%s", client_id, bad[i]);
		venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
		g_object_set(source, "name", "Bad", "provider", "blizzard_auctions", "settings", settings,
		             "schedule", "manual", NULL);

		g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

	/* And the same settings, good, save. */
	{
		g_autoptr(VentureDataSource) source = venture_data_source_new();
		g_autofree gchar *settings = settings_text(fixture, "connected_realm_ids: [11, \"3676\"]\n"
		                                               "item_names_per_fetch: 5000\nrealm_index_hours: 0\n");

		venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
		g_object_set(source, "name", "Good", "provider", "blizzard_auctions", "settings", settings,
		             "schedule", "manual", NULL);
		save(fixture, VENTURE_ENTITY(source));
	}

	/* A currency nobody registered fails the run, naming the fix. */
	{
		gint64 id = create_source(fixture, "currency: SILVERMOON\nconnected_realm_ids: [11]\n",
		                          client_secret, "manual");

		serve_battle_net(fixture);
		run = sync_and_wait(fixture, id);
		message = run_text(run, "error");
		g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
		g_assert_nonnull(strstr(message, "exponent 4"));
		g_assert_cmpint(venture_test_http_hits(&fixture->http, "/token"), ==, 0);
	}
}

/* ==========================================================================
 * Sync
 * ========================================================================== */

/*
 * One realm and the commodity market, synced twice an hour apart:
 *
 * - the token is fetched once, by client credentials in HTTP Basic, and
 *   sent as a bearer header -- never in a query string;
 * - instruments are keyed with their variants (sorted, de-duplicated
 *   bonus ids, sorted modifiers, a pet's species and breed), a variant
 *   filed under its plain item, and named from the item endpoint, which
 *   also gives the vendor price the fee model reads;
 * - prices are copper in GOLD: a unit_price as given, a buyout divided by
 *   its quantity; a bid-only auction is skipped (and said so in the run),
 *   because it cannot be bought now and its bid is not a price to buy at;
 * - the venue is named from the connected realm, grouped by region;
 * - the second sync sends the first's Last-Modified, dates its snapshot by
 *   its own, and the sale estimate reads time_left's lower bound: a LONG
 *   auction gone within the hour sold, a SHORT one may have expired.
 *
 * And the token is nowhere: not in a run, not in the store's bytes, not
 * in a log line.
 *
 * What breaks if this regresses: the whole connector -- and the subtle
 * parts are the key format (two spellings of one variant split its
 * history), the copper exponent, and the expiry bound (counting expiries
 * as sales inflates every sale rate the arbitrage scan reads). A bid-only
 * auction listed at its bid made it the realm's cheapest -- Deals sent a
 * buyer there -- and its end a "sale".
 */
static void
test_sync(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) later = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *store = NULL;
	g_autofree gchar *wal = NULL;
	g_autoptr(VentureEntity) source = NULL;
	GLogFunc previous;
	gint64 id;

	(void)user_data;

	serve_battle_net(fixture);
	id = create_source(fixture, "connected_realm_ids: [11]\ninclude_commodities: true\n", client_secret,
	                   "manual");

	g_mutex_lock(&captured_lock);
	captured_log = g_string_new(NULL);
	g_mutex_unlock(&captured_lock);
	previous = g_log_set_default_handler(capture_log, NULL);

	first = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(first), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(first, "units"), ==, 2);

	/* The token: client credentials in, a bearer header out. */
	seen = venture_test_http_seen(&fixture->http, "/token", "Method");
	g_assert_cmpstr(seen, ==, "POST");
	g_clear_pointer(&seen, g_free);
	seen = venture_test_http_seen(&fixture->http, "/data/wow/connected-realm/11/auctions", "?");
	g_assert_null(strstr(seen, access_token));
	g_assert_nonnull(strstr(seen, "namespace=dynamic-us"));
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/token"), ==, 1);

	/* Two auctions refused (no price; no quantity), and one bid-only
	 * skipped, which is not a refusal: nothing is wrong with it. */
	notes = run_text(first, "notes");
	g_assert_nonnull(strstr(notes, "11: 1 bid-only auctions skipped"));
	g_assert_null(strstr(notes, "listed at their bid"));
	g_assert_cmpint(run_int(first, "refused"), ==, 2);

	reader = reader_of(fixture, id);

	{
		g_autoptr(VentureSeriesRow) ore = current(reader, "11", KEY_ORE);
		g_autoptr(VentureSeriesRow) blade = current(reader, "11", KEY_BLADE);
		g_autoptr(VentureSeriesRow) pet = current(reader, "11", KEY_PET);
		g_autoptr(VentureSeriesRow) cloth = current(reader, "11", KEY_CLOTH);
		g_autoptr(VentureSeriesRow) market = current(reader, "commodities", KEY_ORE);

		g_assert_nonnull(ore);
		g_assert_cmpint(ore->min_price, ==, 1000);
		g_assert_cmpint(ore->quantity, ==, 25);
		g_assert_cmpint(ore->listings, ==, 2);
		g_assert_cmpstr(ore->currency, ==, "GOLD");
		g_assert_cmpstr(ore->venue_name, ==, "Tichondrius");
		g_assert_cmpstr(ore->group_key, ==, "us");
		g_assert_cmpstr(ore->instrument_name, ==, "Copper Ore");
		g_assert_cmpint(ore->taken_at, ==, source_unix(first_modified));

		g_assert_nonnull(blade);
		g_assert_cmpint(blade->min_price, ==, 25000000);
		g_assert_cmpstr(blade->instrument_name, ==, "Thunderfury, Blessed Blade of the Windseeker");
		g_assert_nonnull(pet);
		g_assert_cmpint(pet->min_price, ==, 1500000);
		g_assert_cmpstr(pet->instrument_name, ==, "Pet Cage (pet 39)");
		/* Linen's only auction is bid-only: nothing to buy, no price. */
		g_assert_null(cloth);

		g_assert_nonnull(market);
		g_assert_cmpint(market->min_price, ==, 1100);
		g_assert_cmpstr(market->venue_name, ==, "US commodities");
	}

	{
		g_autoptr(VentureSeriesInstrumentRow) blade = NULL;
		g_autoptr(VentureSeriesInstrumentRow) plain = NULL;
		g_autoptr(VentureSeriesInstrumentRow) ore = NULL;

		g_assert_true(venture_series_store_get_instrument(reader, KEY_BLADE, &blade, &error));
		g_assert_cmpstr(blade->parent_key, ==, "19019");
		g_assert_cmpstr(blade->namespace_, ==, "wow-item");
		g_assert_nonnull(strstr(blade->attrs_json, "\"bonus_lists\":[1472,6646]"));
		g_assert_true(venture_series_store_get_instrument(reader, "19019", &plain, &error));
		g_assert_nonnull(plain);
		g_assert_true(venture_series_store_get_instrument(reader, KEY_ORE, &ore, &error));
		g_assert_cmpstr(ore->category, ==, "Trade Goods/Metal & Stone");
		g_assert_nonnull(strstr(ore->attrs_json, "\"vendor_sell\":5"));

		/* Links elsewhere: Wowhead with the variant's bonuses, and the
		 * token a venue's Undermine Exchange template fills in. */
		g_assert_nonnull(strstr(blade->attrs_json,
		                        "\"url\":\"https://www.wowhead.com/item=19019?bonus=1472:6646\""));
		g_assert_nonnull(strstr(blade->attrs_json, "\"undermine_item\":\"19019\""));
		g_assert_nonnull(strstr(ore->attrs_json, "\"url\":\"https://www.wowhead.com/item=2770\""));
	}

	{
		g_autoptr(VentureSeriesInstrumentRow) pet = NULL;
		g_autoptr(GPtrArray) venues = NULL;
		guint v;
		gboolean realm_linked = FALSE;

		g_assert_true(venture_series_store_get_instrument(reader, KEY_PET, &pet, &error));
		g_assert_nonnull(strstr(pet->attrs_json, "https://www.wowhead.com/battle-pet/39"));
		g_assert_nonnull(strstr(pet->attrs_json, "\"undermine_item\":\"82800-39\""));

		/* The realm's venue links to its page on Undermine Exchange by
		 * one of its realms' slugs. */
		venues = venture_series_store_list_venues(reader, &error);
		g_assert_no_error(error);

		for (v = 0; v < venues->len; v++)
		{
			VentureSeriesVenueRow *venue = g_ptr_array_index(venues, v);

			if ((0 == g_strcmp0(venue->key, "11")) && (NULL != venue->attrs_json) &&
			    (NULL != strstr(venue->attrs_json, "https://undermine.exchange/#us-")) &&
			    (NULL != strstr(venue->attrs_json, "/{undermine_item}")))
				realm_linked = TRUE;
		}

		g_assert_true(realm_linked);
	}

	/* The display: the name's colour by quality, the cached icon, and the
	 * game's tooltip lines in the game's order and colours. The icon was
	 * fetched once, into the plugin's own cache directory. */
	{
		g_autoptr(VentureSeriesInstrumentRow) blade = NULL;
		g_autofree gchar *icon = g_build_filename(fixture->state_dir, "plugin-cache", "blizzard-auctions",
		                                          "icons", "135349.jpg", NULL);

		g_assert_true(venture_series_store_get_instrument(reader, KEY_BLADE, &blade, &error));
		g_assert_nonnull(strstr(blade->attrs_json, "\"color\":\"#ff8000\""));
		g_assert_nonnull(strstr(blade->attrs_json, "\"icon\":\"/blizzard/icons/135349.jpg\""));
		g_assert_nonnull(strstr(blade->attrs_json, "{\"text\":\"Item Level 40\",\"color\":\"#ffd100\"}"));
		g_assert_nonnull(strstr(blade->attrs_json, "{\"text\":\"One-Hand\",\"right\":\"Sword\"}"));
		g_assert_nonnull(strstr(blade->attrs_json, "\"color\":\"#1eff00\""));
		g_assert_true(g_file_test(icon, G_FILE_TEST_IS_REGULAR));
		g_assert_cmpint(venture_test_http_hits(&fixture->http, "/icons/56/135349.jpg"), ==, 1);

		/* The plain item, listed only as a variant, carries the
		 * tooltip too: without it, the next build's state read the row
		 * as never looked up, and asked Blizzard for it every fetch. */
		{
			g_autoptr(VentureSeriesInstrumentRow) plain = NULL;

			g_assert_true(venture_series_store_get_instrument(reader, "19019", &plain, &error));
			g_assert_nonnull(plain);
			g_assert_nonnull(plain->attrs_json);
			g_assert_nonnull(strstr(plain->attrs_json, "\"icon\":\"/blizzard/icons/135349.jpg\""));
			g_assert_nonnull(strstr(plain->attrs_json, "\"url\":\"https://www.wowhead.com/item=19019\""));
			g_assert_null(strstr(plain->attrs_json, "bonus_lists"));
		}
	}

	/* An hour later: 1002 (LONG) is gone, so sold; 1004 (SHORT) is gone
	 * and may have expired; 1008 is new. 1005, the bid-only Linen, is
	 * gone too, but was never a listing: its end is no sale. */
	{
		VentureTestRoute *route = api(fixture, "/data/wow/connected-realm/11/auctions",
		                              "auctions-11-later.json");

		set_last_modified(route, later_modified);
	}

	/* On its own schedule, the realm is asked for what changed since the
	 * first snapshot's Last-Modified. */
	later = schedule_and_wait(fixture, id);
	g_assert_cmpint(run_status(later), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_clear_pointer(&seen, g_free);
	seen = venture_test_http_seen(&fixture->http, "/data/wow/connected-realm/11/auctions",
	                              "If-Modified-Since");
	g_assert_cmpstr(seen, ==, first_modified);

	{
		g_autoptr(GArray) ore_days = NULL;
		g_autoptr(GArray) pet_days = NULL;
		g_autoptr(GArray) cloth_days = NULL;
		g_autoptr(VentureSeriesRow) ore = current(reader, "11", KEY_ORE);
		VentureSeriesDay *day;

		g_assert_cmpint(ore->taken_at, ==, source_unix(later_modified));
		g_assert_cmpint(ore->quantity, ==, 30);

		ore_days = venture_series_store_daily(reader, "11", KEY_ORE, 0, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(ore_days->len, ==, 1);
		day = &g_array_index(ore_days, VentureSeriesDay, 0);
		g_assert_cmpint(day->sold_estimate, ==, 5);

		cloth_days = venture_series_store_daily(reader, "11", KEY_CLOTH, 0, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(cloth_days->len, ==, 0);

		pet_days = venture_series_store_daily(reader, "11", KEY_PET, 0, &error);
		day = &g_array_index(pet_days, VentureSeriesDay, 0);
		g_assert_cmpint(day->expired_estimate, ==, 1);
		g_assert_true((0 == day->sold_estimate) || (VENTURE_SERIES_NONE == day->sold_estimate));
	}

	g_log_set_default_handler(previous, NULL);

	/* The token is nowhere it could be read. */
	json = run_json(first);
	g_assert_null(strstr(json, access_token));
	g_assert_null(strstr(json, client_secret));
	g_clear_pointer(&json, g_free);
	json = run_json(later);
	g_assert_null(strstr(json, access_token));

	g_mutex_lock(&captured_lock);
	g_assert_null(strstr(captured_log->str, access_token));
	g_assert_null(strstr(captured_log->str, client_secret));
	g_string_free(captured_log, TRUE);
	captured_log = NULL;
	g_mutex_unlock(&captured_lock);

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, id, &error);
	store = g_build_filename(fixture->state_dir, "series", venture_entity_get_uuid(source), "store.db", NULL);
	wal = g_strconcat(store, "-wal", NULL);
	g_assert_true(g_file_test(store, G_FILE_TEST_EXISTS));
	g_assert_false(file_contains(store, access_token));
	g_assert_false(file_contains(wal, access_token));
	g_assert_false(file_contains(store, client_secret));
	g_assert_false(file_contains(wal, client_secret));
}

/* The units the provider lists for the base settings plus @extra, joined
 * by commas: what a source with them is frozen into. */
static gchar *
units_of(
	Fixture		*fixture,
	const gchar	*extra
){
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) settings = NULL;
	g_auto(GStrv) units = NULL;
	g_autofree gchar *text = settings_text(fixture, extra);
	VentureDataSourceProvider *provider;

	provider = venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(fixture->context), "blizzard_auctions");
	g_assert_nonnull(provider);
	settings = venture_feeds_parse_settings(text, &error);
	g_assert_no_error(error);
	units = venture_data_source_provider_list_units(provider, settings, &error);
	g_assert_no_error(error);

	return g_strjoinv(",", units);
}

/* What the realm list cache holds, as the one file in its directory. */
static gchar *
realm_cache(Fixture *fixture)
{
	g_autofree gchar *dir = g_build_filename(fixture->state_dir, "plugin-cache", "blizzard-auctions", "realms",
	                                         NULL);
	g_autoptr(GDir) listing = g_dir_open(dir, 0, NULL);
	g_autofree gchar *path = NULL;
	const gchar *name;
	gchar *contents = NULL;

	g_assert_nonnull(listing);
	name = g_dir_read_name(listing);
	g_assert_nonnull(name);
	path = g_build_filename(dir, name, NULL);
	g_assert_null(g_dir_read_name(listing));
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));

	return contents;
}

/*
 * A source naming no realm finds them itself. Before any index is known
 * its units are realm-index alone; the first sync reads the index, keeps
 * it, and the source is frozen again into one unit per realm -- the very
 * names an explicit list makes -- beside realm-index. A failed read of
 * the index keeps the last list (a source frozen afterwards still gets
 * every realm), and a realm the index gains becomes a unit.
 *
 * What breaks if this regresses: an empty list goes back to one unit for
 * the region (a realm's prices hours old, Deals pointing at auctions long
 * sold); a unit named unlike the explicit list's starts every realm's
 * history afresh when a source switches; a Battle.net outage takes every
 * realm's unit away; a realm Blizzard adds is never read.
 */
static void
test_realm_discovery(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) one = NULL;
	g_autoptr(VentureEntity) two = NULL;
	g_autoptr(VentureEntity) three = NULL;
	g_autoptr(VentureEntity) four = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autofree gchar *units = NULL;
	g_autofree gchar *listed = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *cache = NULL;
	g_autofree gchar *message = NULL;
	gint64 id;

	(void)user_data;

	serve_battle_net(fixture);

	/* Nothing known yet: the index is the only unit. */
	units = units_of(fixture, "include_commodities: false\n");
	g_assert_cmpstr(units, ==, "realm-index");
	g_clear_pointer(&units, g_free);

	id = create_source(fixture, "include_commodities: false\nitem_names_per_fetch: 0\nrealm_index_hours: 0\n",
	                   client_secret, "manual");

	one = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(one), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(one, "units"), ==, 1);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/index"), ==, 1);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/11/auctions"), ==, 0);
	notes = run_text(one, "notes");
	g_assert_nonnull(strstr(notes, "the index lists 2 connected realms (2 new, 0 gone)"));

	/* Kept, and the units are an explicit list's, in id order. */
	cache = realm_cache(fixture);
	g_assert_nonnull(strstr(cache, "\"ids\":[11,3676]"));
	units = units_of(fixture, "include_commodities: false\n");
	listed = units_of(fixture, "include_commodities: false\nconnected_realm_ids: [11, 3676]\n");
	g_assert_cmpstr(listed, ==, "11,3676");
	g_assert_cmpstr(units, ==, "11,3676,realm-index");

	/* The source was frozen again: this sync reads both realms. */
	two = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(two), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(two, "units"), ==, 3);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/11/auctions"), ==, 1);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/3676/auctions"), ==, 1);
	reader = reader_of(fixture, id);

	{
		g_autoptr(VentureSeriesRow) ore = current(reader, "3676", KEY_ORE);

		g_assert_nonnull(ore);
		g_assert_cmpint(ore->min_price, ==, 1800);
		g_assert_cmpstr(ore->venue_name, ==, "Area 52, Ner'zhul");
		/* No names asked for: the instrument has none yet. */
		g_assert_null(ore->instrument_name);
	}

	/* Battle.net's index fails: that unit fails, the realms are still
	 * read, and the list -- for this source and any frozen now -- stays. */
	venture_test_http_route(&fixture->http, "/data/wow/connected-realm/index", 503, "{}");
	three = sync_and_wait(fixture, id);
	message = run_text(three, "error");
	g_assert_nonnull(message);
	g_assert_nonnull(strstr(message, "realm-index"));
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/11/auctions"), ==, 2);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/3676/auctions"), ==, 2);
	g_clear_pointer(&units, g_free);
	units = units_of(fixture, "include_commodities: false\n");
	g_assert_cmpstr(units, ==, "11,3676,realm-index");

	/* A realm appears in the index: it is a unit from the next freeze. */
	{
		g_autofree gchar *index = g_strdup_printf(
			"{\"connected_realms\":["
			"{\"href\":\"%s/data/wow/connected-realm/3676?namespace=dynamic-us\"},"
			"{\"href\":\"%s/data/wow/connected-realm/1234?namespace=dynamic-us\"},"
			"{\"href\":\"%s/data/wow/connected-realm/11?namespace=dynamic-us\"}]}",
			fixture->http.origin, fixture->http.origin, fixture->http.origin);
		VentureTestRoute *route = venture_test_http_route(&fixture->http, "/data/wow/connected-realm/index",
		                                                  200, index);

		route->require_authorization = g_strconcat("Bearer ", access_token, NULL);
	}

	four = sync_and_wait(fixture, id);
	g_clear_pointer(&notes, g_free);
	notes = run_text(four, "notes");
	g_assert_nonnull(strstr(notes, "the index lists 3 connected realms (1 new, 0 gone)"));
	g_clear_pointer(&units, g_free);
	units = units_of(fixture, "include_commodities: false\n");
	g_assert_cmpstr(units, ==, "11,1234,3676,realm-index");
	g_clear_pointer(&cache, g_free);
	cache = realm_cache(fixture);
	g_assert_nonnull(strstr(cache, "\"ids\":[11,1234,3676]"));
}

/*
 * A source that listed its realms by id and is switched to an empty list
 * keeps each realm's history: once the index is read, its units have the
 * same names, so each asks Battle.net with the If-Modified-Since its
 * explicit unit last saw. And a fresh list is not read again within
 * realm_index_hours.
 *
 * What breaks if this regresses: switching a deployment from its listed
 * realms to discovery refetches every realm whole and loses the adaptive
 * schedule each had learned -- or, with names that differ, keeps two
 * histories of one realm.
 */
static void
test_realm_switch(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) third = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *ims = NULL;
	VentureTestRoute *route;
	gint64 id;

	(void)user_data;

	serve_battle_net(fixture);
	id = create_source(fixture, "connected_realm_ids: [11, 3676]\ninclude_commodities: false\n"
	                   "item_names_per_fetch: 0\n", client_secret, "manual");
	first = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(first), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(first, "units"), ==, 2);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/index"), ==, 0);

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, id, &error);
	g_assert_no_error(error);
	settings = settings_text(fixture, "include_commodities: false\nitem_names_per_fetch: 0\n");
	g_object_set(source, "settings", settings, NULL);
	save(fixture, source);

	second = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(second), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/index"), ==, 1);

	route = api(fixture, "/data/wow/connected-realm/11/auctions", "auctions-11.json");
	route->not_modified_when_asked = TRUE;
	route = api(fixture, "/data/wow/connected-realm/3676/auctions", "auctions-3676.json");
	route->not_modified_when_asked = TRUE;

	third = schedule_and_wait(fixture, id);
	g_assert_cmpint(run_status(third), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(third, "units"), ==, 3);
	g_assert_cmpint(run_int(third, "not-modified"), ==, 3);
	ims = venture_test_http_seen(&fixture->http, "/data/wow/connected-realm/3676/auctions", "If-Modified-Since");
	g_assert_cmpstr(ims, ==, first_modified);
	g_clear_pointer(&ims, g_free);
	ims = venture_test_http_seen(&fixture->http, "/data/wow/connected-realm/11/auctions", "If-Modified-Since");
	g_assert_cmpstr(ims, ==, first_modified);

	/* Read a moment ago: not asked for again. */
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/index"), ==, 1);
}

/*
 * include_bid_only, set, lists a bid-only auction at its bid and says so
 * -- the old behaviour, now only on request.
 *
 * What breaks if this regresses: an operator who wants bids in the store
 * cannot have them, or the default lets them back in.
 */
static void
test_bid_only_opt_in(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(VentureSeriesRow) cloth = NULL;
	g_autofree gchar *notes = NULL;
	gint64 id;

	(void)user_data;

	serve_battle_net(fixture);
	id = create_source(fixture, "connected_realm_ids: [11]\ninclude_commodities: false\n"
	                   "include_bid_only: true\nitem_names_per_fetch: 0\n", client_secret, "manual");
	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	notes = run_text(run, "notes");
	g_assert_nonnull(strstr(notes, "1 auctions have a bid and no buyout; listed at their bid"));
	g_assert_cmpint(run_int(run, "refused"), ==, 2);
	reader = reader_of(fixture, id);
	cloth = current(reader, "11", KEY_CLOTH);
	g_assert_nonnull(cloth);
	g_assert_cmpint(cloth->min_price, ==, 500);
}

/*
 * The commodity market costs 25 requests of the hourly budget, and
 * Blizzard charges it even when the answer is 304: the budget is spent
 * before the request goes out, never refunded.
 *
 * What breaks if this regresses: a source that polls commodities often
 * believes it has budget left and runs into Blizzard's own limit instead,
 * which answers 429 for every realm.
 */
static void
test_commodities_cost(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	VentureTestRoute *route;
	gint64 id;

	(void)user_data;

	serve_battle_net(fixture);
	route = api(fixture, "/data/wow/auctions/commodities", "commodities.json");
	set_last_modified(route, first_modified);
	route->not_modified_when_asked = TRUE;
	route = api(fixture, "/data/wow/connected-realm/11/auctions", "auctions-11.json");
	set_last_modified(route, first_modified);
	route->not_modified_when_asked = TRUE;

	id = create_source(fixture, "connected_realm_ids: [11]\nitem_names_per_fetch: 0\n", client_secret, "manual");
	first = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(first), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

	/* Nothing changed: two 304s. The source was saved to put it on its
	 * schedule, so a fresh token is asked for too: 1 + 1 + 25. */
	second = schedule_and_wait(fixture, id);
	g_assert_cmpint(run_status(second), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
	g_assert_cmpint(run_int(second, "not-modified"), ==, 2);
	g_assert_cmpint(run_int(second, "requests"), ==, 3);
	g_assert_cmpint(run_int(second, "quota-used") - run_int(first, "quota-used"), ==, 1 + 1 + 25);
	g_assert_cmpint(run_int(second, "quota-limit"), ==, 36000);
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/auctions/commodities"), ==, 2);
}

/*
 * A 429 is honoured: the unit fails with the far end's reason and is not
 * checked again before its Retry-After, while the commodity market in
 * the same run is stored.
 *
 * What breaks if this regresses: a rate-limited client keeps asking,
 * which is how a Battle.net client gets its key suspended.
 */
static void
test_rate_limited(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(JsonNode) status = NULL;
	g_autofree gchar *message = NULL;
	VentureTestRoute *route;
	JsonArray *sources;
	gint64 id;
	gint64 now;
	guint i;
	guint j;
	gboolean found = FALSE;

	(void)user_data;

	serve_battle_net(fixture);
	route = venture_test_http_route(&fixture->http, "/data/wow/connected-realm/11/auctions", 429, "{}");
	route->headers = g_new0(gchar *, 3);
	route->headers[0] = g_strdup("Retry-After");
	route->headers[1] = g_strdup("600");

	id = create_source(fixture, "connected_realm_ids: [11]\nitem_names_per_fetch: 0\n", client_secret, "auto");
	now = g_get_real_time() / G_USEC_PER_SEC;
	run = sync_and_wait(fixture, id);
	message = run_text(run, "error");

	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_PARTIAL);
	g_assert_nonnull(strstr(message, "429"));

	{
		g_autoptr(VentureSeriesStore) reader = reader_of(fixture, id);
		g_autoptr(VentureSeriesRow) market = current(reader, "commodities", KEY_ORE);

		g_assert_nonnull(market);
	}

	/* The realm's next check waits the ten minutes it was asked to. */
	status = venture_feeds_service_dup_status(service_of(fixture));
	sources = json_object_get_array_member(json_node_get_object(status), "sources");

	for (i = 0; i < json_array_get_length(sources); i++)
	{
		JsonObject *source = json_array_get_object_element(sources, i);
		JsonArray *units;

		if (json_object_get_int_member(source, "id") != id)
			continue;

		units = json_object_get_array_member(source, "units");

		for (j = 0; j < json_array_get_length(units); j++)
		{
			JsonObject *unit = json_array_get_object_element(units, j);

			if (0 != g_strcmp0(json_object_get_string_member(unit, "name"), "11"))
				continue;

			g_assert_cmpint(json_object_get_int_member(unit, "next_check"), >=, now + 590);
			found = TRUE;
		}
	}

	g_assert_true(found);
}

/*
 * Credentials Battle.net refuses fail the run with a reason that names
 * them, and so does a source with no secret sealed; neither run carries
 * the secret.
 *
 * What breaks if this regresses: a typo in the client id reads as "HTTP
 * 401" against an address the operator never typed, and nothing says
 * which half of the pair to check.
 */
static void
test_bad_credentials(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(VentureEntity) missing = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *json = NULL;
	gint64 id;

	(void)user_data;

	serve_battle_net(fixture);
	id = create_source(fixture, "connected_realm_ids: [11]\ninclude_commodities: false\n", "wrong-secret-0000",
	                   "manual");
	refused = sync_and_wait(fixture, id);
	message = run_text(refused, "error");

	g_assert_cmpint(run_status(refused), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "refused the client credentials (HTTP 401)"));
	g_assert_nonnull(strstr(message, "client_secret"));
	json = run_json(refused);
	g_assert_null(strstr(json, "wrong-secret-0000"));
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/data/wow/connected-realm/11/auctions"), ==, 0);

	g_clear_pointer(&message, g_free);
	id = create_source(fixture, "connected_realm_ids: [11]\ninclude_commodities: false\n", NULL, "manual");
	missing = sync_and_wait(fixture, id);
	message = run_text(missing, "error");
	g_assert_cmpint(run_status(missing), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "needs a client"));
}

/* ==========================================================================
 * The fee model and the export
 * ========================================================================== */

static JsonObject *
object_of(const gchar *json)
{
	g_autoptr(JsonNode) node = json_from_string(json, NULL);

	g_assert_nonnull(node);

	return json_object_ref(json_node_get_object(node));
}

static gint64
minor_of(JsonNode *money)
{
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GError) error = NULL;

	value = venture_money_from_json(money, NULL, &error);
	g_assert_no_error(error);

	return venture_money_get_amount(value);
}

/*
 * wow_auction: a 5% cut on a sale; a deposit of the vendor price by
 * duration (60% for 48 hours by default, 15% for 12, or the operator's
 * own share), refundable, and none when the vendor price is unknown;
 * nothing to buy; a currency that is not copper refused. A venue naming
 * it saves once the plugin is loaded, and a scan selling 2770 at realm
 * 3676 reads the vendor price from the store's attributes.
 *
 * What breaks if this regresses: every flip on an auction house is
 * priced without its cut, and listing losses (deposits lost to expiry)
 * read as zero.
 */
static void
test_fee_model(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureFeeModelRegistry *models;
	g_autoptr(VentureMoney) gross = venture_money_new(1000000, "GOLD", 4);
	g_autoptr(VentureMoney) dollars = venture_money_new(10000, "USD", 2);
	g_autoptr(JsonObject) attrs = object_of("{\"vendor_sell\":5000}");
	g_autoptr(GError) error = NULL;
	VentureFeeQuote quote;

	(void)user_data;

	models = venture_context_get_fee_models(fixture->context);
	g_assert_true(venture_fee_model_registry_has(models, "wow_auction"));

	{
		g_autoptr(JsonObject) params = object_of("{}");

		g_assert_true(venture_fee_model_registry_compute(models, "wow_auction", params, VENTURE_FEE_SIDE_SELL,
		                                                 gross, 2, NULL, attrs, &quote, &error));
		g_assert_no_error(error);
		g_assert_cmpint(venture_money_get_amount(quote.fee), ==, 50000);
		g_assert_cmpint(venture_money_get_amount(quote.deposit), ==, 6000);
		g_assert_true(quote.deposit_refundable);
		venture_fee_quote_clear(&quote);

		/* No vendor price known: no deposit, the cut still. */
		g_assert_true(venture_fee_model_registry_compute(models, "wow_auction", params, VENTURE_FEE_SIDE_SELL,
		                                                 gross, 2, NULL, NULL, &quote, NULL));
		g_assert_cmpint(venture_money_get_amount(quote.fee), ==, 50000);
		g_assert_null(quote.deposit);
		venture_fee_quote_clear(&quote);

		/* Buying out is the price, nothing more. */
		g_assert_true(venture_fee_model_registry_compute(models, "wow_auction", params, VENTURE_FEE_SIDE_BUY,
		                                                 gross, 2, NULL, attrs, &quote, NULL));
		g_assert_cmpint(venture_money_get_amount(quote.fee), ==, 0);
		g_assert_null(quote.deposit);
		venture_fee_quote_clear(&quote);

		g_assert_false(venture_fee_model_registry_compute(models, "wow_auction", params, VENTURE_FEE_SIDE_SELL,
		                                                  dollars, 1, NULL, attrs, &quote, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

	{
		g_autoptr(JsonObject) params = object_of("{\"duration_hours\":12,\"cut_percent\":\"2.5\"}");

		g_assert_true(venture_fee_model_registry_compute(models, "wow_auction", params, VENTURE_FEE_SIDE_SELL,
		                                                 gross, 2, NULL, attrs, &quote, NULL));
		g_assert_cmpint(venture_money_get_amount(quote.fee), ==, 25000);
		g_assert_cmpint(venture_money_get_amount(quote.deposit), ==, 1500);
		venture_fee_quote_clear(&quote);
	}

	{
		g_autoptr(JsonObject) params = object_of("{\"deposit_factor_percent\":10}");

		g_assert_true(venture_fee_model_registry_compute(models, "wow_auction", params, VENTURE_FEE_SIDE_SELL,
		                                                 gross, 2, NULL, attrs, &quote, NULL));
		g_assert_cmpint(venture_money_get_amount(quote.deposit), ==, 1000);
		venture_fee_quote_clear(&quote);
	}

	g_assert_true(venture_fee_model_registry_validate(models, "wow_auction", "cut_percent: 5\nduration_hours: 24",
	                                                  NULL));
	g_assert_false(venture_fee_model_registry_validate(models, "wow_auction", "duration_hours: 36", &error));
	g_clear_error(&error);
	g_assert_false(venture_fee_model_registry_validate(models, "wow_auction", "cut: 5", &error));
	g_assert_nonnull(strstr(error->message, "cut"));
	g_clear_error(&error);

	/* Through a scan: buy 2770 at Tichondrius, sell at Area 52. */
	{
		g_autoptr(VentureEntity) buy = record(fixture, "venue");
		g_autoptr(VentureEntity) sell = record(fixture, "venue");
		g_autoptr(VentureEntity) run = NULL;
		g_autoptr(JsonObject) options = NULL;
		g_autoptr(JsonNode) answer = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *key = NULL;
		JsonArray *rows;
		JsonObject *row = NULL;
		gint64 id;
		guint i;

		serve_battle_net(fixture);
		id = create_source(fixture, "connected_realm_ids: [11, 3676]\ninclude_commodities: false\n",
		                   client_secret, "manual");

		/* Area 52 an hour ago held five more ore, bought since: the ore
		 * sells there, so a scan may price a sale there at all. */
		set_last_modified(api(fixture, "/data/wow/connected-realm/3676/auctions", "auctions-3676-earlier.json"),
		                  "Mon, 05 Oct 2026 10:00:00 GMT");
		run = sync_and_wait(fixture, id);
		g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);
		g_clear_object(&run);
		set_last_modified(api(fixture, "/data/wow/connected-realm/3676/auctions", "auctions-3676.json"),
		                  "Mon, 05 Oct 2026 11:00:00 GMT");
		run = sync_and_wait(fixture, id);
		g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

		g_object_set(buy, "name", "Tichondrius", "key", "11", "namespace", "us-realm", "data-source-id", id,
		             "currency", "GOLD", "fee-model", "wow_auction", NULL);
		save(fixture, buy);
		g_object_set(sell, "name", "Area 52", "key", "3676", "namespace", "us-realm", "data-source-id", id,
		             "currency", "GOLD", "fee-model", "wow_auction", "fee-params", "duration_hours: 48",
		             NULL);
		save(fixture, sell);

		text = g_strdup_printf("{\"strategy\":\"spread\",\"data_source_id\":%" G_GINT64_FORMAT ","
		                       "\"instrument\":\"2770\",\"buy_venues\":\"11\",\"sell_venues\":\"3676\"}", id);
		options = object_of(text);
		answer = venture_arbitrage_scan_run(fixture->context, fixture->org, options, &error);
		g_assert_no_error(error);
		rows = json_object_get_array_member(json_node_get_object(answer), "rows");
		key = g_strdup_printf("spread:%" G_GINT64_FORMAT ":11>%" G_GINT64_FORMAT ":3676:2770", id, id);

		for (i = 0; i < json_array_get_length(rows); i++)
		{
			JsonObject *candidate = json_array_get_object_element(rows, i);

			if (0 == g_strcmp0(json_object_get_string_member(candidate, "key"), key))
				row = candidate;
		}

		g_assert_nonnull(row);

		/* 5% of 18.00 copper-units: 90; 60% of a 5-copper vendor price. */
		g_assert_cmpint(minor_of(json_object_get_member(json_object_get_object_member(row, "sell"), "fees")),
		                ==, 90);
		g_assert_cmpint(minor_of(json_object_get_member(json_object_get_object_member(row, "sell"), "deposit")),
		                ==, 3);
	}
}

/*
 * tsm: one line of TSM item strings, each item once in first-seen order,
 * a variant collapsed to its item, a pet as its species, a transform
 * row's inputs included and anything not a WoW key left out.
 *
 * What breaks if this regresses: the group pasted into TSM is missing
 * the reagents a craft needs, or carries a bonus-id spelling TSM reads as
 * a different item.
 */
static void
test_tsm_export(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonNode) rows = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	VentureExportFormatRegistry *formats;
	gsize size;

	(void)user_data;

	formats = venture_context_get_export_formats(fixture->context);
	g_assert_true(venture_export_format_registry_has(formats, "tsm"));
	g_assert_cmpstr(venture_export_format_registry_get_extension(formats, "tsm"), ==, "txt");

	rows = json_from_string(
		"[{\"instrument_key\":\"2770\"},"
		"{\"instrument_key\":\"" KEY_BLADE "\"},"
		"{\"instrument_key\":\"" KEY_PET "\"},"
		"{\"instrument_key\":\"2770\"},"
		"{\"instrument_key\":\"sku-1001\"},"
		"{\"instrument_key\":\"ev-1:Draw\"},"
		"{\"strategy\":\"transform\",\"instrument_key\":\"2840\","
		" \"inputs\":[{\"instrument_key\":\"2770\"},{\"instrument_key\":\"2589\"},{\"name\":\"unquoted\"}]}]",
		NULL);
	bytes = venture_export_format_registry_export(formats, "tsm", json_node_get_array(rows), NULL, &error);
	g_assert_no_error(error);
	text = g_strndup(g_bytes_get_data(bytes, &size), g_bytes_get_size(bytes));
	g_assert_cmpstr(text, ==, "i:2770,i:19019,p:39,i:2840,i:2589\n");

	/* An empty scan is an empty group. */
	{
		g_autoptr(JsonNode) none = json_from_string("[]", NULL);
		g_autoptr(GBytes) empty = venture_export_format_registry_export(formats, "tsm",
		                                                                json_node_get_array(none), NULL,
		                                                                &error);

		g_assert_no_error(error);
		g_assert_cmpuint(g_bytes_get_size(empty), ==, 1);
	}
}

/* ==========================================================================
 * Recipes
 * ========================================================================== */

static gint64
count_of(
	Fixture		*fixture,
	GType		 type
){
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	gint64 count;

	count = venture_database_count(fixture->database, query, &error);
	g_assert_no_error(error);

	return count;
}

static gchar *
import(
	Fixture		*fixture,
	gint64		 id,
	const gchar	*params_json
){
	g_autoptr(JsonNode) node = json_from_string(params_json, NULL);
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) answer = NULL;
	g_autoptr(GError) error = NULL;
	gchar *result = NULL;

	params = venture_action_parameters_from_json(node, &error);
	g_assert_no_error(error);
	answer = venture_action_registry_perform(venture_database_get_action_registry(fixture->database),
	                                         "data_source", id, "import_recipes", params, NULL,
	                                         VENTURE_USER_ROLE_OWNER, &error);

	if (NULL == answer)
		g_error("import_recipes %s: %s", params_json, error->message);

	g_object_get(answer, "result", &result, NULL);

	return result;
}

static VentureEntity *
recipe_named(
	Fixture		*fixture,
	const gchar	*name
){
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_RECIPE);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, name, &error));
	found = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(found->len, ==, 1);

	return g_object_ref(g_ptr_array_index(found, 0));
}

/*
 * import_recipes walks professions, tiers and recipes:
 *
 * - without create_products, recipes whose items have no product are
 *   skipped and say which (the instruments are still made);
 * - with it, each item gets a product through its instrument, each recipe
 *   its components -- two slots of one reagent merged into one line, a
 *   proc's range read as its minimum, an enchant (no item) skipped;
 * - max_recipes bounds a call and the cursor continues it;
 * - a second run changes nothing.
 *
 * What breaks if this regresses: the transform strategy has no recipes
 * to price; or a re-import doubles every component, and a craft consumes
 * twice the reagents it should.
 */
static void
test_import_recipes(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) venture = record(fixture, "venture");
	g_autoptr(VentureEntity) smelt = NULL;
	g_autoptr(VentureEntity) belt = NULL;
	g_autoptr(VentureEntity) bar = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *partial = NULL;
	g_autofree gchar *rest = NULL;
	g_autofree gchar *params = NULL;
	g_autoptr(GError) error = NULL;
	gint64 components;
	gint64 output;
	gint64 quantity;
	gint64 id;

	(void)user_data;

	serve_battle_net(fixture);
	id = create_source(fixture, "connected_realm_ids: [11]\n", client_secret, "manual");
	g_object_set(venture, "name", "Crafting", NULL);
	save(fixture, venture);

	/* No products may be made: every recipe is skipped, and says why. */
	first = import(fixture, id, "{\"max_recipes\":10}");
	g_assert_nonnull(strstr(first, "Read 4 recipes: 0 created"));
	g_assert_nonnull(strstr(first, "has no product"));
	g_assert_nonnull(strstr(first, "makes no item"));
	g_assert_nonnull(strstr(first, "Done"));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RECIPE), ==, 0);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_INSTRUMENT), >, 0);

	/* A bounded call hands back where to go on. */
	params = g_strdup_printf("{\"max_recipes\":2,\"create_products\":true,\"venture_id\":%" G_GINT64_FORMAT "}",
	                         venture_entity_get_id(venture));
	partial = import(fixture, id, params);
	g_assert_nonnull(strstr(partial, "Read 2 recipes: 2 created"));
	g_assert_nonnull(strstr(partial, "cursor=186/2572/0"));

	g_free(params);
	params = g_strdup_printf("{\"max_recipes\":10,\"create_products\":true,\"cursor\":\"186/2572/0\","
	                         "\"venture_id\":%" G_GINT64_FORMAT "}", venture_entity_get_id(venture));
	rest = import(fixture, id, params);
	g_assert_nonnull(strstr(rest, "Read 2 recipes: 1 created, 0 updated, 0 unchanged, 1 skipped"));
	g_assert_nonnull(strstr(rest, "Done"));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RECIPE), ==, 3);

	/* Smelt Copper: Copper Ore into one Copper Bar. */
	smelt = recipe_named(fixture, "Smelt Copper");
	g_object_get(smelt, "output-product-id", &output, "output-quantity", &quantity, NULL);
	g_assert_cmpint(quantity, ==, 1);
	bar = venture_database_get(fixture->database, VENTURE_TYPE_PRODUCT, output, &error);
	g_assert_nonnull(bar);
	g_assert_cmpstr(venture_entity_get_display_name(bar), ==, "Copper Bar");

	/* Copper Chain Belt: 4 + 2 Copper Bar merged, and a Linen Cloth. */
	belt = recipe_named(fixture, "Copper Chain Belt");
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_RECIPE_COMPONENT);
		g_autoptr(GPtrArray) lines = NULL;
		guint i;
		gboolean merged = FALSE;

		g_assert_true(venture_query_add_filter_int(query, "recipe-id", VENTURE_FILTER_OP_EQ,
		                                           venture_entity_get_id(belt), &error));
		lines = venture_database_find(fixture->database, query, &error);
		g_assert_cmpuint(lines->len, ==, 2);

		for (i = 0; i < lines->len; i++)
		{
			gint64 product;
			gint64 count;

			g_object_get(g_ptr_array_index(lines, i), "product-id", &product, "quantity", &count, NULL);

			if (product == output)
			{
				g_assert_cmpint(count, ==, 6);
				merged = TRUE;
			}
		}

		g_assert_true(merged);
	}

	/* The instrument for Copper Bar is the product's. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
		g_autoptr(GPtrArray) found = NULL;
		gint64 product;

		g_assert_true(venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, "2840", &error));
		found = venture_database_find(fixture->database, query, &error);
		g_assert_cmpuint(found->len, ==, 1);
		g_object_get(g_ptr_array_index(found, 0), "product-id", &product, NULL);
		g_assert_cmpint(product, ==, output);
	}

	/* Again, from the top: nothing changes. */
	components = count_of(fixture, VENTURE_TYPE_RECIPE_COMPONENT);
	g_free(params);
	params = g_strdup_printf("{\"create_products\":true,\"venture_id\":%" G_GINT64_FORMAT "}",
	                         venture_entity_get_id(venture));
	again = import(fixture, id, params);
	g_assert_nonnull(strstr(again, "Read 4 recipes: 0 created, 0 updated, 3 unchanged, 1 skipped"));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RECIPE), ==, 3);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_RECIPE_COMPONENT), ==, components);

	/* create_products without a venture is refused before anything runs. */
	{
		g_autoptr(JsonNode) node = json_from_string("{\"create_products\":true}", NULL);
		g_autoptr(GHashTable) bad = venture_action_parameters_from_json(node, NULL);
		g_autoptr(VentureEntity) refused = NULL;

		refused = venture_action_registry_perform(venture_database_get_action_registry(fixture->database),
		                                          "data_source", id, "import_recipes", bad, NULL,
		                                          VENTURE_USER_ROLE_OWNER, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

}

/* One GET on this thread's main context, which the server answering it
 * runs on too. */
typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} PageReply;

static void
page_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	PageReply *reply = data;

	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static gchar *
get_page(
	VentureWebServer	*server,
	SoupSession		*session,
	const gchar		*path
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	gchar *body;
	PageReply reply;

	memset(&reply, 0, sizeof(reply));
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, page_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	if (200 != soup_message_get_status(message))
		g_error("GET %s answered %u", path, soup_message_get_status(message));

	body = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	g_bytes_unref(reply.body);

	return body;
}

/*
 * Blizzard's API terms (section 2.13) require Blizzard to be named,
 * clearly and conspicuously, as the source of the data, without the
 * application appearing endorsed by or affiliated with Blizzard. After a
 * real sync the Trading pages that show the auctions say so -- with both
 * halves of the sentence -- and their JSON twins carry the same line.
 *
 * What breaks if this regresses: an install showing Blizzard's auction
 * data without the attribution its terms make a condition of using it.
 */
static void
test_attribution(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar line[] =
		"Auction house data provided by Blizzard Entertainment via the Battle.net API. "
		"Not affiliated with or endorsed by Blizzard Entertainment.";
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *path = NULL;
	JsonArray *lines;
	gint64 id;

	(void)user_data;

	g_assert_cmpstr(venture_data_source_provider_get_attribution(
		venture_data_source_provider_registry_lookup(
			venture_context_get_data_source_providers(fixture->context), "blizzard_auctions")), ==, line);

	serve_battle_net(fixture);
	id = create_source(fixture, "connected_realm_ids: [11]\ninclude_commodities: true\n", client_secret,
	                   "manual");
	run = sync_and_wait(fixture, id);
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_OK);

	g_object_set(fixture->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
	             "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	session = soup_session_new_with_options("timeout", 30, NULL);

	page = get_page(server, session, "/market/browse");
	g_assert_nonnull(strstr(page, "Copper Ore"));
	g_assert_nonnull(strstr(page, "<p class=\"source-attribution\" role=\"note\">"));
	g_assert_nonnull(strstr(page, line));
	g_clear_pointer(&page, g_free);

	path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/" KEY_ORE, id);
	page = get_page(server, session, path);
	g_assert_nonnull(strstr(page, line));
	g_clear_pointer(&page, g_free);

	page = get_page(server, session, "/feeds");
	g_assert_nonnull(strstr(page, line));

	json = get_page(server, session, "/api/v1/market/browse");
	answer = json_from_string(json, &error);
	g_assert_no_error(error);
	lines = json_object_get_array_member(json_node_get_object(answer), "attribution");
	g_assert_cmpuint(json_array_get_length(lines), ==, 1);
	g_assert_cmpstr(json_array_get_string_element(lines, 0), ==, line);

	venture_web_server_stop(server);
}

#endif /* VENTURE_HAVE_SQLITE */

/*
 * api_base and oauth_base replace the addresses the client secret and its
 * token are sent to. They are honoured only with
 * feeds.allow_endpoint_overrides (test and development): off, a source
 * naming one is refused at the save, and one saved while it was on fails
 * its run before the token request. A source that names neither uses
 * Blizzard's own addresses and saves either way.
 *
 * What breaks if this regresses: whoever may edit a data source points
 * oauth_base at an allowlisted origin they read and receives the client
 * secret in the token request's Basic header.
 */
static void
test_override_needs_the_switch(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDataSource) refused = NULL;
	g_autoptr(VentureDataSource) official = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *plain = NULL;
	gint64 id;

	(void)user_data;

	g_object_set(fixture->config, "feeds-allow-endpoint-overrides", FALSE, NULL);
	settings = settings_text(fixture, "connected_realm_ids: [11]\n");
	refused = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(refused), fixture->org);
	g_object_set(refused, "name", "Redirected", "provider", "blizzard_auctions", "settings", settings,
	             "schedule", "manual", NULL);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(refused), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "feeds.allow_endpoint_overrides"));
	g_clear_error(&error);

	plain = g_strdup_printf("region: eu\nclient_id: %s\n", client_id);
	official = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(official), fixture->org);
	g_object_set(official, "name", "Official", "provider", "blizzard_auctions", "settings", plain,
	             "schedule", "manual", NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(official), NULL, &error));
	g_assert_no_error(error);

	g_object_set(fixture->config, "feeds-allow-endpoint-overrides", TRUE, NULL);
	id = create_source(fixture, "connected_realm_ids: [11]\n", client_secret, "manual");
	g_object_set(fixture->config, "feeds-allow-endpoint-overrides", FALSE, NULL);
	serve_battle_net(fixture);

	run = sync_and_wait(fixture, id);
	message = run_text(run, "error");
	g_assert_cmpint(run_status(run), ==, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);
	g_assert_nonnull(strstr(message, "feeds.allow_endpoint_overrides"));
	g_assert_null(strstr(message, client_secret));
	g_assert_cmpint(venture_test_http_hits(&fixture->http, "/token"), ==, 0);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

#ifdef VENTURE_HAVE_SQLITE
#define ADD(name, fn) \
	g_test_add("/plugin-blizzard/" name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)

	ADD("settings", test_settings);
	ADD("sync", test_sync);
	ADD("realm-discovery", test_realm_discovery);
	ADD("realm-switch-keeps-history", test_realm_switch);
	ADD("bid-only-opt-in", test_bid_only_opt_in);
	ADD("commodities-cost-25-even-on-304", test_commodities_cost);
	ADD("rate-limited", test_rate_limited);
	ADD("bad-credentials", test_bad_credentials);
	ADD("override-needs-the-switch", test_override_needs_the_switch);
	ADD("fee-model", test_fee_model);
	ADD("tsm-export", test_tsm_export);
	ADD("import-recipes", test_import_recipes);
	ADD("attribution", test_attribution);
#undef ADD
#endif

	return g_test_run();
}
