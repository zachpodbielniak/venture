/*
 * test-trading-cli.c - venturectl's feeds, market, arbitrage and plugins
 * verbs, driven as a person drives them: the real binary against a real
 * server
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The server runs in this process on a kernel-chosen port with
 * authentication off (test-auth is where who may ask is decided), over a
 * store seeded through its writer the way test-arbitrage-scan seeds one:
 * a herb cheap at realm-a and dear at realm-b. venturectl runs as a
 * subprocess while this process's main loop serves it, so every call
 * below is a full round trip -- argument typing in the CLI, the query the
 * page would send, the answer the page would draw.
 *
 * What these protect: that a verb sends the question its page asks (a
 * dropped option answers a different question silently), that wrong
 * types are refused before anything is sent, that --stage holds back
 * every write it is accepted on, and that the calculators' figures are
 * the library's, to the minor unit.
 */

#include <venture.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "venture-test-util.h"

#ifdef VENTURE_HAVE_SQLITE

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

typedef struct
{
	gchar			*state_dir;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	gint64			 org;
	gint64			 source_id;
	gint64			 now;
	gint64			 realm_a;
	gint64			 realm_b;
} Fixture;

/* What one venturectl run said. */
typedef struct
{
	gboolean	 done;
	gchar		*out;
	gchar		*err;
	GError		*error;
	gint		 status;
} Run;

static void
run_clear(Run *run)
{
	g_clear_pointer(&run->out, g_free);
	g_clear_pointer(&run->err, g_free);
	g_clear_error(&run->error);
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static VentureEntity *
record(
	Fixture		*fixture,
	const gchar	*type
){
	VentureEntity *entity;

	entity = venture_entity_registry_create(venture_entity_registry_get_default(), type, NULL);
	g_assert_nonnull(entity);
	venture_entity_set_organization_id(entity, fixture->org);

	return entity;
}

static void
field(
	gpointer	 entity,
	const gchar	*name,
	const gchar	*value
){
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(entity), name, value, &error));
	g_assert_no_error(error);
}

/* Every live record of @type. */
static GPtrArray *
rows_of_type(
	Fixture		*fixture,
	GType		 type
){
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	venture_query_set_limit(query, 0);
	rows = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);

	return rows;
}

static guint
count_of(
	Fixture		*fixture,
	GType		 type
){
	g_autoptr(GPtrArray) rows = rows_of_type(fixture, type);

	return rows->len;
}

static guint
pending(Fixture *fixture)
{
	g_autoptr(GPtrArray) list = NULL;

	list = venture_confirmation_store_list_pending(venture_context_get_confirmations(fixture->context));

	return (NULL != list) ? list->len : 0;
}

static gchar *
id_text(gint64 id)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, id);
}

/* --- Running venturectl ------------------------------------------------- */

static void
run_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	Run *run = data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &run->out, &run->err, &run->error);
	run->done = TRUE;
}

static gboolean
run_timeout(gpointer process)
{
	g_subprocess_force_exit(process);

	return G_SOURCE_CONTINUE;
}

/*
 * Runs venturectl with @format (NULL: none, so a pipe gets JSON) and the
 * NULL-terminated @args, serving its requests from this process's main
 * loop until it exits. Bounded: a CLI that hangs is killed and fails.
 */
static void
cli_run(
	Fixture			*fixture,
	const gchar		*format,
	const gchar *const	*args,
	Run			*run
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	guint timeout;
	guint i;

	memset(run, 0, sizeof(*run));
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "VENTURE_TOKEN");
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(fixture->server)));

	if (NULL != format)
	{
		g_ptr_array_add(argv, g_strdup("-f"));
		g_ptr_array_add(argv, g_strdup(format));
	}

	for (i = 0; NULL != args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));

	g_ptr_array_add(argv, NULL);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);

	timeout = g_timeout_add_seconds(60, run_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, run_done, run);

	while (!run->done)
		g_main_context_iteration(NULL, TRUE);

	g_source_remove(timeout);
	g_assert_no_error(run->error);
	run->status = g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : -1;
}

/* Runs, asserts success, and returns stdout. */
static gchar *
cli_ok(
	Fixture			*fixture,
	const gchar		*format,
	const gchar *const	*args
){
	Run run;
	gchar *out;

	cli_run(fixture, format, args, &run);

	if (0 != run.status)
		g_error("venturectl %s %s exited %d: %s%s", args[0], (NULL != args[1]) ? args[1] : "", run.status,
		        run.out, run.err);

	out = g_steal_pointer(&run.out);
	run_clear(&run);

	return out;
}

/* Runs, asserts it exited @status, and that stderr says @fragment. */
static void
cli_refused(
	Fixture			*fixture,
	const gchar *const	*args,
	gint			 status,
	const gchar		*fragment
){
	Run run;

	cli_run(fixture, "json", args, &run);

	if ((run.status != status) || ((NULL != fragment) && (NULL == strstr(run.err, fragment))))
		g_error("venturectl %s %s: expected exit %d saying \"%s\", got %d: %s%s", args[0],
		        (NULL != args[1]) ? args[1] : "", status, (NULL != fragment) ? fragment : "", run.status,
		        run.out, run.err);

	run_clear(&run);
}

/* JSON from a run's stdout; it must parse. */
static JsonNode *
json_of(const gchar *text)
{
	g_autoptr(GError) error = NULL;
	JsonNode *node;

	node = venture_json_parse(text, &error);

	if (NULL == node)
		g_error("not JSON (%s): %s", error->message, text);

	return node;
}

static gint64
amount_of(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node = json_object_get_member(object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		g_error("%s is not money", member);

	return json_object_get_int_member(json_node_get_object(node), "amount");
}

/* ==========================================================================
 * The store and the fixture
 * ========================================================================== */

typedef struct
{
	const gchar	*instrument;
	guint64		 id;
	gint64		 price;
	gint64		 quantity;
} Offer;

static void
add_venue(
	VentureSeriesStore	*store,
	const gchar		*key,
	gint64			 seen_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesVenue venue;

	memset(&venue, 0, sizeof(venue));
	venue.key = key;
	venue.namespace_ = "realm";
	venue.name = key;
	venue.kind = "auction_house";
	venue.group_key = "eu";
	venue.currency = "USD";
	g_assert_true(venture_series_store_upsert_venue(store, &venue, seen_at, &error));
	g_assert_no_error(error);
}

static void
snapshot(
	VentureSeriesStore	*store,
	const gchar		*venue,
	gint64			 taken_at,
	const Offer		*offers,
	guint			 n_offers
){
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snap;
	VentureSeriesCommitResult result;
	guint i;

	snap = venture_series_store_begin_snapshot(store, venue, "USD", taken_at, taken_at + 30, TRUE, &error);
	g_assert_no_error(error);

	for (i = 0; i < n_offers; i++)
	{
		VentureSeriesListing listing;

		memset(&listing, 0, sizeof(listing));
		listing.instrument_key = offers[i].instrument;
		listing.listing_id = offers[i].id;
		listing.unit_price = offers[i].price;
		listing.quantity = offers[i].quantity;
		listing.side = VENTURE_SERIES_SIDE_SELL;
		listing.expires_in_min = -1;
		g_assert_true(venture_series_snapshot_add_listing(snap, &listing, &error));
		g_assert_no_error(error);
	}

	g_assert_true(venture_series_store_commit_snapshot(store, snap, &result, &error));
	g_assert_no_error(error);
}

/*
 * The market, in cents: Peacebloom ("herb") 10.00 x10 at realm-a, 20.00 x5
 * at realm-b and 18.00 x3 at realm-c; a vial at realm-b. Ten minutes old.
 */
static void
seed_store(
	Fixture	*fixture,
	gint64	 source_id
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	VentureSeriesInstrument instrument;
	gint64 t;
	static const Offer a[] = { { "herb", 1, 1000, 10 } };
	static const Offer b[] = { { "herb", 10, 2000, 5 }, { "vial", 11, 100, 10 } };
	static const Offer c[] = { { "herb", 20, 1800, 3 } };

	source = venture_database_get(fixture->database, VENTURE_TYPE_DATA_SOURCE, source_id, NULL);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	t = fixture->now - 600;
	add_venue(store, "realm-a", t);
	add_venue(store, "realm-b", t);
	add_venue(store, "realm-c", t);

	memset(&instrument, 0, sizeof(instrument));
	instrument.namespace_ = "item";
	instrument.kind = "item";
	instrument.key = "herb";
	instrument.name = "Peacebloom";
	instrument.category = "Herbs";
	g_assert_true(venture_series_store_upsert_instrument(store, &instrument, t, NULL, NULL, &error));
	instrument.key = "vial";
	instrument.name = "Empty Vial";
	instrument.category = "Parts";
	g_assert_true(venture_series_store_upsert_instrument(store, &instrument, t, NULL, NULL, &error));
	g_assert_no_error(error);

	snapshot(store, "realm-a", t, a, G_N_ELEMENTS(a));
	snapshot(store, "realm-b", t, b, G_N_ELEMENTS(b));
	snapshot(store, "realm-c", t, c, G_N_ELEMENTS(c));

	g_assert_true(venture_series_store_recompute_region(store, NULL, fixture->now, VENTURE_SERIES_NONE, NULL,
	                                                    NULL, &error));
	g_assert_no_error(error);
}

static gint64
venue_record(
	Fixture		*fixture,
	const gchar	*key,
	const gchar	*model,
	const gchar	*params,
	const gchar	*transfer
){
	g_autoptr(VentureEntity) venue = record(fixture, "venue");

	g_object_set(venue, "name", key, "key", key, "namespace", "realm", "data-source-id", fixture->source_id,
	             "fee-model", model, "fee-params", params, "currency", "USD", NULL);
	field(venue, "transfer-cost", transfer);
	save(fixture, venue);

	return ID(venue);
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDataSource) source = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-trading-cli-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, "feeds-enabled", TRUE,
	             "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
	             "security-require-auth", FALSE, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* The registry is process-wide; with this context's switches applied
	 * the feeds and arbitrage tables exist now. */
	g_assert_true(venture_database_migrate(fixture->database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	/* Manual, so nothing starts a worker behind the test's back. */
	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Auctions", "provider", "file_jsonl", "settings", "file: x.jsonl",
	             "schedule", "manual", "currency", "USD", "instrument-namespace", "item",
	             "venue-namespace", "realm", NULL);
	save(fixture, source);
	fixture->source_id = ID(source);
	fixture->now = g_get_real_time() / G_USEC_PER_SEC;

	seed_store(fixture, fixture->source_id);

	/* realm-a is free with a 1.00 move; realm-b takes a 5% cut and a 0.50
	 * move: two herbs flipped net 40.00 - 2.00 - 20.00 - 1.50 = 16.50. */
	fixture->realm_a = venue_record(fixture, "realm-a", "none", NULL, "1.00 USD");
	fixture->realm_b = venue_record(fixture, "realm-b", "percent", "cut_percent: 5", "0.50 USD");

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	venture_web_server_stop(fixture->server);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}
}

/* The opportunity's key with the fixture's source id in it. */
static gchar *
herb_key(Fixture *fixture)
{
	return g_strdup_printf("spread:%" G_GINT64_FORMAT ":realm-a>%" G_GINT64_FORMAT ":realm-b:herb",
	                       fixture->source_id, fixture->source_id);
}

/* ==========================================================================
 * Help
 * ========================================================================== */

/*
 * Every new verb is in --help with a worked example, and each group has
 * a `help` of its own that exits 0 and shows examples. What breaks if
 * this regresses: a verb nobody can discover, or a usage line with no
 * example of the money spelling it insists on.
 */
static void
test_help(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	const gchar *const help[] = { "--help", NULL };
	const gchar *const arbitrage[] = { "arbitrage", "help", NULL };
	const gchar *const market[] = { "market", "help", NULL };
	const gchar *const bare[] = { "arbitrage", NULL };
	g_autofree gchar *out = NULL;
	g_autofree gchar *arbitrage_out = NULL;
	g_autofree gchar *market_out = NULL;
	Run run;

	(void)user_data;

	out = cli_ok(fixture, NULL, help);
	g_assert_nonnull(strstr(out, "arbitrage scan"));
	g_assert_nonnull(strstr(out, "arbitrage calc surebet|back-lay|flip"));
	g_assert_nonnull(strstr(out, "market browse|deals|venues"));
	g_assert_nonnull(strstr(out, "plugins [list]"));
	g_assert_nonnull(strstr(out, "--stake"));
	g_assert_nonnull(strstr(out, "venturectl arbitrage calc surebet 2.10 2.05 --stake"));
	g_assert_nonnull(strstr(out, "venturectl market alerts evaluate 7 --dry-run"));

	arbitrage_out = cli_ok(fixture, NULL, arbitrage);
	g_assert_nonnull(strstr(arbitrage_out, "Examples:"));
	g_assert_nonnull(strstr(arbitrage_out, "venturectl arbitrage record 1 spread"));
	market_out = cli_ok(fixture, NULL, market);
	g_assert_nonnull(strstr(market_out, "Examples:"));
	g_assert_nonnull(strstr(market_out, "venturectl market instrument 1 2589 units=200"));

	/* A verb that reads organization_id at a fixed place says so on its
	 * own line; the blanket sentence under them does not say where. */
	g_assert_nonnull(strstr(market_out, "promote SOURCE_ID instrument|venue|account KEY [organization_id=N]"));
	g_assert_nonnull(strstr(market_out, "watchlist [ID] [organization_id=N]"));
	g_assert_nonnull(strstr(market_out, "alerts evaluate RULE_ID [--dry-run] [organization_id=N]"));

	/* --stage lists every verb its gate accepts. */
	g_assert_nonnull(strstr(out, "dedupe/journal post"));

	/* A bare group is a usage refusal (exit 2) that still shows them. */
	cli_run(fixture, NULL, bare, &run);
	g_assert_cmpint(run.status, ==, 2);
	g_assert_nonnull(strstr(run.err, "Examples:"));
	run_clear(&run);
}

/* ==========================================================================
 * market
 * ========================================================================== */

/*
 * The Trading pages' twins: browse, deals, venues, an instrument with a
 * bulk price, the watchlists and the alerts, each asked the page's own
 * question. Wrong types are refused by name before a request is made.
 */
static void
test_market(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *source = id_text(fixture->source_id);
	const gchar *const browse[] = { "market", "browse", "search=Peacebloom", "sort=min_price", NULL };
	const gchar *const venues[] = { "market", "venues", "group=eu", NULL };
	const gchar *const deals[] = { "market", "deals", "max_pct=100", "top=5", NULL };
	const gchar *const instrument[] = { "market", "instrument", source, "herb", "units=3", NULL };
	const gchar *const bad_page[] = { "market", "browse", "page=two", NULL };
	const gchar *const bad_name[] = { "market", "browse", "limit=5", NULL };
	const gchar *const bad_dir[] = { "market", "browse", "dir=sideways", NULL };
	const gchar *const bad_stock[] = { "market", "browse", "stock=maybe", NULL };
	const gchar *const no_key[] = { "market", "instrument", source, NULL };
	g_autoptr(JsonNode) browsed = NULL;
	g_autoptr(JsonNode) shown = NULL;
	g_autoptr(JsonNode) indexed = NULL;
	g_autoptr(JsonNode) dealt = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *table = NULL;
	JsonArray *rows;

	(void)user_data;

	out = cli_ok(fixture, "json", browse);
	browsed = json_of(out);
	rows = json_object_get_array_member(json_node_get_object(browsed), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	/* sort=min_price reached the store: cheapest first. */
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "venue_key"), ==,
	                "realm-a");
	g_assert_cmpint(amount_of(json_array_get_object_element(rows, 0), "min_price"), ==, 1000);

	/* The table names its columns and draws money as money. */
	table = cli_ok(fixture, "table", browse);
	g_assert_nonnull(strstr(table, "instrument"));
	g_assert_nonnull(strstr(table, "Peacebloom"));
	g_assert_nonnull(strstr(table, "realm-b"));
	g_assert_nonnull(strstr(table, "10.00 USD"));

	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "json", venues);
	indexed = json_of(out);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(indexed),
	                                                                     "venues")), ==, 3);

	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "json", deals);
	dealt = json_of(out);
	g_assert_true(json_object_has_member(json_node_get_object(dealt), "rows"));

	/* units=3 prices a bulk buy at the cheapest venue in stock: three
	 * herbs at realm-a's 10.00. */
	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "json", instrument);
	shown = json_of(out);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(json_node_get_object(shown),
	                                                                          "bulk"), "filled"), ==, 3);
	g_assert_cmpint(amount_of(json_object_get_object_member(json_node_get_object(shown), "bulk"), "cost"), ==,
	                3000);
	g_clear_pointer(&table, g_free);
	table = cli_ok(fixture, "table", instrument);
	g_assert_nonnull(strstr(table, "Bulk cost"));
	g_assert_nonnull(strstr(table, "30.00 USD"));

	cli_refused(fixture, bad_page, 2, "page is a whole number");
	cli_refused(fixture, bad_name, 2, "does not take \"limit\"");
	cli_refused(fixture, bad_dir, 2, "asc|desc");
	cli_refused(fixture, bad_stock, 2, "true or false");
	cli_refused(fixture, no_key, 2, "usage");
}

/*
 * Watchlists and alerts. `market promote` makes the instrument record a
 * rule and an entry need; `alerts evaluate --dry-run` says what would fire
 * and writes nothing, and without it the hit is written. --dry-run and
 * --stage are refused where they would do nothing.
 */
static void
test_market_lists_and_alerts(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *source = id_text(fixture->source_id);
	const gchar *const promote[] = { "market", "promote", source, "instrument", "herb", NULL };
	const gchar *const lists[] = { "market", "watchlist", NULL };
	const gchar *const alerts[] = { "market", "alerts", "count=10", NULL };
	const gchar *const browse_dry[] = { "--dry-run", "market", "browse", NULL };
	g_autoptr(JsonNode) promoted = NULL;
	g_autoptr(JsonNode) listed = NULL;
	g_autoptr(JsonNode) viewed = NULL;
	g_autoptr(JsonNode) dry = NULL;
	g_autoptr(JsonNode) wet = NULL;
	g_autoptr(VentureEntity) list = NULL;
	g_autoptr(VentureEntity) entry = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *list_id = NULL;
	g_autofree gchar *rule_id = NULL;
	gint64 instrument;

	(void)user_data;

	out = cli_ok(fixture, "json", promote);
	promoted = json_of(out);
	instrument = json_object_get_int_member(json_node_get_object(promoted), "id");
	g_assert_cmpint(instrument, >, 0);

	list = record(fixture, "watchlist");
	g_object_set(list, "name", "Herbs to buy", "group-key", "eu", NULL);
	save(fixture, list);
	entry = record(fixture, "watchlist_entry");
	g_object_set(entry, "watchlist-id", ID(list), "instrument-id", instrument, NULL);
	field(entry, "target-buy", "12.00 USD");
	save(fixture, entry);

	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "json", lists);
	listed = json_of(out);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(listed),
	                                                                     "watchlists")), ==, 1);

	list_id = id_text(ID(list));

	{
		const gchar *const one[] = { "market", "watchlist", list_id, NULL };

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", one);
		viewed = json_of(out);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(viewed),
		                                                                     "entries")), ==, 1);
	}

	/* Below 15.00: realm-a's 10.00 fires. */
	rule = record(fixture, "alert_rule");
	g_object_set(rule, "name", "cheap herb", "kind", VENTURE_ALERT_KIND_BELOW, "instrument-id", instrument,
	             "data-source-id", fixture->source_id, NULL);
	field(rule, "threshold", "15.00 USD");
	save(fixture, rule);
	rule_id = id_text(ID(rule));

	{
		const gchar *const evaluate_dry[] = { "market", "alerts", "evaluate", rule_id, "--dry-run", NULL };
		const gchar *const evaluate[] = { "market", "alerts", "evaluate", rule_id, NULL };
		const gchar *const staged[] = { "--stage", "market", "alerts", "evaluate", rule_id, NULL };

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", evaluate_dry);
		dry = json_of(out);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(dry), "candidates"), >=, 1);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(dry), "written"), ==, 0);
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ALERT_HIT), ==, 0);

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", evaluate);
		wet = json_of(out);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(wet), "written"), >=, 1);
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ALERT_HIT), >=, 1);

		cli_refused(fixture, staged, 2, "--stage only means something");
	}

	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "table", alerts);
	g_assert_nonnull(strstr(out, "cheap herb"));

	cli_refused(fixture, browse_dry, 2, "--dry-run");
}

/* ==========================================================================
 * arbitrage
 * ========================================================================== */

/*
 * The scan through the CLI is the scan: the spread row worked by hand in
 * test-arbitrage-scan (net 16.50 on 21.50) comes back with its key, and
 * the table numbers its rows for `record`. An option the scan would
 * silently ignore -- venture_id, which it never reads -- is refused.
 */
static void
test_scan(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	const gchar *const scan[] = { "arbitrage", "scan", "spread", "units=2", "category_path=Herbs", NULL };
	const gchar *const venture[] = { "arbitrage", "scan", "spread", "venture_id=2", NULL };
	const gchar *const bad_units[] = { "arbitrage", "scan", "units=two", NULL };
	const gchar *const bad_word[] = { "arbitrage", "scan", "spread", "deal", NULL };
	const gchar *const unknown[] = { "arbitrage", "scan", "spread", "astrology=yes", NULL };
	const gchar *const stage_scan[] = { "--stage", "arbitrage", "scan", NULL };
	const gchar *const stake_scan[] = { "--stake", "1.00 USD", "arbitrage", "scan", NULL };
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *table = NULL;
	g_autofree gchar *key = herb_key(fixture);
	JsonArray *rows;
	JsonObject *row = NULL;
	guint i;

	(void)user_data;

	out = cli_ok(fixture, "json", scan);
	answer = json_of(out);
	g_assert_true(json_object_get_boolean_member(json_node_get_object(answer), "available"));
	rows = json_object_get_array_member(json_node_get_object(answer), "rows");

	for (i = 0; i < json_array_get_length(rows); i++)
		if (0 == g_strcmp0(json_object_get_string_member(json_array_get_object_element(rows, i), "key"), key))
			row = json_array_get_object_element(rows, i);

	g_assert_nonnull(row);
	g_assert_cmpint(amount_of(row, "net"), ==, 1650);
	g_assert_cmpint(amount_of(row, "capital"), ==, 2150);

	table = cli_ok(fixture, "table", scan);
	g_assert_nonnull(strstr(table, "#"));
	g_assert_nonnull(strstr(table, key));
	g_assert_nonnull(strstr(table, "16.50 USD"));

	cli_refused(fixture, venture, 2, "venture_id");
	cli_refused(fixture, bad_units, 2, "units is a whole number");
	cli_refused(fixture, bad_word, 2, "\"deal\" is not one");
	cli_refused(fixture, unknown, 2, "astrology");
	cli_refused(fixture, stage_scan, 2, "--stage only means something");
	cli_refused(fixture, stake_scan, 2, "--stake belongs to arbitrage calc");
}

/*
 * Recording: by row, by key, staged, and from explicit legs in a file.
 * Each direct record is one trade with planned legs; each staged one is a
 * pending confirmation and no trade. An organization that is not a number
 * is refused by the CLI, typed from the action's own schema; legs that
 * are not JSON by the action; a legs file that is not there by the CLI,
 * as a usage error. None of them leaves a trade behind.
 */
static void
test_record(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *key = herb_key(fixture);
	g_autofree gchar *org = g_strdup_printf("organization_id=%" G_GINT64_FORMAT, fixture->org);
	g_autofree gchar *legs_path = NULL;
	g_autofree gchar *legs_arg = NULL;
	g_autofree gchar *legs_json = NULL;
	g_autofree gchar *out = NULL;
	g_autoptr(JsonNode) trade = NULL;
	g_autoptr(GError) error = NULL;
	guint trades;
	guint confirmations;

	(void)user_data;

	/* Row 1 of the same question is the herb flip, the only row. */
	{
		const gchar *const by_row[] = { "arbitrage", "record", "1", "spread", "units=2", "category_path=Herbs",
		                                NULL };

		out = cli_ok(fixture, "json", by_row);
		trade = json_of(out);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(trade), "strategy"), ==, "spread");
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(trade), "status"), ==, "planned");
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE), ==, 1);
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ARBITRAGE_LEG), >=, 2);
	}

	{
		const gchar *const by_key[] = { "arbitrage", "record", key, "units=2", "category_path=Herbs", NULL };
		const gchar *const gone[] = { "arbitrage", "record", "9", "spread", "units=2", "category_path=Herbs",
		                              NULL };

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", by_key);
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE), ==, 2);
		cli_refused(fixture, gone, 3, "no row 9");
	}

	/* --stage holds it: a confirmation, no trade, nothing posted. */
	trades = count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE);
	confirmations = pending(fixture);
	{
		const gchar *const staged[] = { "--stage", "arbitrage", "record", "1", "spread", "units=2",
		                                "category_path=Herbs", NULL };
		g_autoptr(JsonNode) answer = NULL;

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", staged);
		answer = json_of(out);
		g_assert_true(json_object_get_boolean_member(json_node_get_object(answer), "staged"));
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE), ==, trades);
		g_assert_cmpuint(pending(fixture), ==, confirmations + 1);
	}

	/* Explicit legs, from a file. */
	legs_path = g_build_filename(fixture->state_dir, "legs.json", NULL);
	legs_json = g_strdup_printf("[{\"kind\":\"buy\",\"venue_id\":%" G_GINT64_FORMAT ",\"amount\":\"10.00\"},"
	                            " {\"kind\":\"sell\",\"venue_id\":%" G_GINT64_FORMAT ",\"amount\":\"20.00\"}]",
	                            fixture->realm_a, fixture->realm_b);
	g_assert_true(g_file_set_contents(legs_path, legs_json, -1, &error));
	g_assert_no_error(error);
	legs_arg = g_strconcat("legs=@", legs_path, NULL);

	{
		const gchar *const explicit[] = { "arbitrage", "record", "name=By hand", "strategy=spread", org, legs_arg,
		                                  NULL };
		const gchar *const staged[] = { "--stage", "arbitrage", "record", "name=Held", org, legs_arg, NULL };
		const gchar *const not_json[] = { "arbitrage", "record", "name=Broken", org, "legs=[{oops", NULL };
		const gchar *const bad_org[] = { "arbitrage", "record", "name=Broken", "organization_id=abc", legs_arg,
		                                 NULL };
		const gchar *const no_file[] = { "arbitrage", "record", "name=Broken", org, "legs=@/nonexistent/legs",
		                                 NULL };

		trades = count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE);
		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", explicit);
		g_assert_nonnull(strstr(out, "By hand"));
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE), ==, trades + 1);

		confirmations = pending(fixture);
		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", staged);
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE), ==, trades + 1);
		g_assert_cmpuint(pending(fixture), ==, confirmations + 1);

		cli_refused(fixture, not_json, 1, NULL);
		cli_refused(fixture, bad_org, 1, NULL);
		cli_refused(fixture, no_file, 2, "nonexistent");
		g_assert_cmpuint(count_of(fixture, VENTURE_TYPE_ARBITRAGE_TRADE), ==, trades + 1);
	}
}

/*
 * The action conveniences: execute a planned leg, close, reopen, stage a
 * close, abandon. Each goes through the action with typed parameters;
 * the trade's status moves exactly as the actions move it.
 */
static void
test_actions(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	const gchar *const by_row[] = { "arbitrage", "record", "1", "spread", "units=2", "category_path=Herbs", NULL };
	g_autoptr(JsonNode) trade = NULL;
	g_autoptr(VentureEntity) reread = NULL;
	g_autoptr(GPtrArray) legs = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *trade_id = NULL;
	g_autofree gchar *leg_id = NULL;
	VentureArbitrageTradeStatus status;
	gint64 id;
	guint i;
	guint confirmations;

	(void)user_data;

	out = cli_ok(fixture, "json", by_row);
	trade = json_of(out);
	id = json_object_get_int_member(json_node_get_object(trade), "id");
	trade_id = id_text(id);

	legs = rows_of_type(fixture, VENTURE_TYPE_ARBITRAGE_LEG);

	for (i = 0; (i < legs->len) && (NULL == leg_id); i++)
	{
		VentureArbitrageLegKind kind;
		gint64 owner;

		g_object_get(g_ptr_array_index(legs, i), "kind", &kind, "trade-id", &owner, NULL);

		if ((owner == id) && (VENTURE_ARBITRAGE_LEG_KIND_BUY == kind))
			leg_id = id_text(ID(g_ptr_array_index(legs, i)));
	}

	g_assert_nonnull(leg_id);

	{
		const gchar *const execute[] = { "arbitrage", "execute", leg_id, NULL };
		const gchar *const close[] = { "arbitrage", "close", trade_id, NULL };
		const gchar *const reopen[] = { "arbitrage", "reopen", trade_id, NULL };
		const gchar *const stage_close[] = { "--stage", "arbitrage", "close", trade_id, NULL };
		const gchar *const abandon[] = { "arbitrage", "abandon", trade_id, "goods=keep", NULL };
		const gchar *const bad_id[] = { "arbitrage", "close", "four", NULL };
		const gchar *const bad_param[] = { "arbitrage", "close", trade_id, "when=now", NULL };
		g_autoptr(VentureEntity) leg = NULL;
		VentureArbitrageLegStatus leg_status;

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", execute);
		leg = venture_database_get(fixture->database, VENTURE_TYPE_ARBITRAGE_LEG,
		                           g_ascii_strtoll(leg_id, NULL, 10), NULL);
		g_object_get(leg, "status", &leg_status, NULL);
		g_assert_cmpint(leg_status, ==, VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED);

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", close);
		reread = venture_database_get(fixture->database, VENTURE_TYPE_ARBITRAGE_TRADE, id, NULL);
		g_object_get(reread, "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED);
		g_clear_object(&reread);

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", reopen);
		reread = venture_database_get(fixture->database, VENTURE_TYPE_ARBITRAGE_TRADE, id, NULL);
		g_object_get(reread, "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_ARBITRAGE_TRADE_STATUS_OPEN);
		g_clear_object(&reread);

		/* Staged: proposed, and the trade stays open. */
		confirmations = pending(fixture);
		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", stage_close);
		g_assert_cmpuint(pending(fixture), ==, confirmations + 1);
		reread = venture_database_get(fixture->database, VENTURE_TYPE_ARBITRAGE_TRADE, id, NULL);
		g_object_get(reread, "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_ARBITRAGE_TRADE_STATUS_OPEN);
		g_clear_object(&reread);

		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", abandon);
		reread = venture_database_get(fixture->database, VENTURE_TYPE_ARBITRAGE_TRADE, id, NULL);
		g_object_get(reread, "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_ARBITRAGE_TRADE_STATUS_ABANDONED);

		cli_refused(fixture, bad_id, 2, "usage");
		cli_refused(fixture, bad_param, 2, "Unknown action parameter: when");
	}
}

/*
 * The calculators answer the library's figures to the minor unit: a
 * surebet split computed here with venture_arbitrage_surebet() and
 * through the CLI agree stake by stake; a back/lay agrees with
 * venture_arbitrage_back_lay(); a flip's net is worked by hand. The odds
 * format reaches the calculator (it was dropped with the scan's export
 * `format`, which read American odds as decimal). Wrong types and a
 * stake where none belongs are refused.
 */
static void
test_calc(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	const gchar *const surebet[] = { "arbitrage", "calc", "surebet", "2.1", "2.2", "--stake", "100.00 USD",
	                                 NULL };
	const gchar *const american[] = { "arbitrage", "calc", "surebet", "odds=200 100", "format=american",
	                                  "--stake", "100.00 USD", NULL };
	const gchar *const back_lay[] = { "arbitrage", "calc", "back-lay", "3.0", "2.9", "--stake", "50.00 USD",
	                                  "commission=5", NULL };
	const gchar *const flip[] = { "arbitrage", "calc", "flip", "10.00 USD", "20.00 USD", "units=2", "cut=5",
	                              NULL };
	const gchar *const one_outcome[] = { "arbitrage", "calc", "surebet", "2.1", "--stake", "100.00 USD", NULL };
	const gchar *const bad_units[] = { "arbitrage", "calc", "flip", "1 USD", "2 USD", "units=x", NULL };
	const gchar *const flip_stake[] = { "arbitrage", "calc", "flip", "1 USD", "2 USD", "--stake", "1 USD", NULL };
	const gchar *const twice[] = { "arbitrage", "calc", "surebet", "2", "2", "stake=1 USD", "--stake", "1 USD",
	                               NULL };
	const gchar *const bad_format[] = { "arbitrage", "calc", "surebet", "2", "2", "format=roman", NULL };
	const gchar *const three[] = { "arbitrage", "calc", "back-lay", "3", "2.9", "2.8", "--stake", "1 USD", NULL };
	const gchar *const unknown[] = { "arbitrage", "calc", "astrology", NULL };
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(VentureMoney) stake = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	VentureArbitrageSurebet split;
	VentureArbitrageBackLay figures;
	const gdouble odds[] = { 2.1, 2.2 };
	JsonArray *outcomes;
	guint i;

	(void)user_data;

	out = cli_ok(fixture, "json", surebet);
	answer = json_of(out);
	total = venture_money_new(10000, "USD", 2);
	memset(&split, 0, sizeof(split));
	g_assert_true(venture_arbitrage_surebet(odds, 2, total, &split, &error));
	g_assert_no_error(error);
	outcomes = json_object_get_array_member(json_node_get_object(answer), "outcomes");
	g_assert_cmpuint(json_array_get_length(outcomes), ==, 2);

	for (i = 0; i < 2; i++)
		g_assert_cmpint(amount_of(json_array_get_object_element(outcomes, i), "stake"), ==,
		                venture_money_get_amount(g_ptr_array_index(split.stakes, i)));

	g_assert_cmpint(amount_of(json_node_get_object(answer), "profit"), ==,
	                venture_money_get_amount(split.profit));
	g_assert_cmpint(amount_of(json_node_get_object(answer), "payout"), ==,
	                venture_money_get_amount(split.payout));
	g_assert_cmpint(amount_of(json_node_get_object(answer), "payout_ideal"), ==,
	                venture_money_get_amount(split.payout_ideal));
	g_assert_cmpint(amount_of(json_node_get_object(answer), "residual"), ==,
	                venture_money_get_amount(split.residual));
	g_assert_true(json_object_get_boolean_member(json_node_get_object(answer), "is_surebet"));
	venture_arbitrage_surebet_clear(&split);

	/* +200 and +100 American are 3.0 and 2.0: 40.00 and 60.00 of 100.
	 * Read as decimal they would be 33.33 and 66.67. */
	g_clear_pointer(&out, g_free);
	g_clear_pointer(&answer, json_node_unref);
	out = cli_ok(fixture, "json", american);
	answer = json_of(out);
	outcomes = json_object_get_array_member(json_node_get_object(answer), "outcomes");
	g_assert_cmpint(amount_of(json_array_get_object_element(outcomes, 0), "stake"), ==, 4000);
	g_assert_cmpint(amount_of(json_array_get_object_element(outcomes, 1), "stake"), ==, 6000);

	g_clear_pointer(&out, g_free);
	g_clear_pointer(&answer, json_node_unref);
	out = cli_ok(fixture, "json", back_lay);
	answer = json_of(out);
	stake = venture_money_new(5000, "USD", 2);
	memset(&figures, 0, sizeof(figures));
	g_assert_true(venture_arbitrage_back_lay(3.0, 2.9, 0.05, stake, &figures, &error));
	g_assert_no_error(error);
	g_assert_cmpint(amount_of(json_node_get_object(answer), "lay_stake"), ==,
	                venture_money_get_amount(figures.lay_stake));
	g_assert_cmpint(amount_of(json_node_get_object(answer), "liability"), ==,
	                venture_money_get_amount(figures.liability));
	venture_arbitrage_back_lay_clear(&figures);

	/* Two at 10.00 sold at 20.00 less a 5% cut: 40.00 - 2.00 - 20.00. */
	g_clear_pointer(&out, g_free);
	g_clear_pointer(&answer, json_node_unref);
	out = cli_ok(fixture, "json", flip);
	answer = json_of(out);
	g_assert_cmpint(amount_of(json_node_get_object(answer), "net"), ==, 1800);
	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "table", flip);
	g_assert_nonnull(strstr(out, "Net"));
	g_assert_nonnull(strstr(out, "18.00 USD"));

	cli_refused(fixture, one_outcome, 2, NULL);
	cli_refused(fixture, bad_units, 2, "units is a whole number");
	cli_refused(fixture, flip_stake, 2, "takes no stake");
	cli_refused(fixture, twice, 2, "once");
	cli_refused(fixture, bad_format, 2, "auto|decimal|american|fractional");
	cli_refused(fixture, three, 2, "two values");
	cli_refused(fixture, unknown, 2, "surebet|back-lay|flip");
}

/*
 * export writes the format's bytes, to stdout or to -o FILE; registries
 * lists what can be asked for; -o elsewhere is refused.
 */
static void
test_export_and_registries(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *file = g_build_filename(fixture->state_dir, "out.csv", NULL);
	g_autofree gchar *key = herb_key(fixture);
	const gchar *const to_stdout[] = { "arbitrage", "export", "csv", "spread", "units=2", "category_path=Herbs",
	                                   NULL };
	const gchar *const to_file[] = { "arbitrage", "export", "csv", "units=2", "category_path=Herbs", "-o", file,
	                                 NULL };
	const gchar *const astrology[] = { "arbitrage", "export", "astrology", NULL };
	const gchar *const registries[] = { "arbitrage", "registries", NULL };
	const gchar *const output_elsewhere[] = { "-o", file, "arbitrage", "scan", NULL };
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *table = NULL;

	(void)user_data;

	out = cli_ok(fixture, NULL, to_stdout);
	g_assert_nonnull(strstr(out, "16.50 USD"));

	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, NULL, to_file);
	g_assert_true(g_file_get_contents(file, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(contents, key));

	cli_refused(fixture, astrology, 3, "format is one of");
	cli_refused(fixture, output_elsewhere, 2, "-o belongs to arbitrage export");

	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "json", registries);
	answer = json_of(out);
	g_assert_nonnull(strstr(out, "\"spread\""));
	g_assert_nonnull(strstr(out, "\"percent\""));
	g_assert_nonnull(strstr(out, "\"shopping_list\""));
	table = cli_ok(fixture, "table", registries);
	g_assert_nonnull(strstr(table, "fee model"));
	g_assert_nonnull(strstr(table, "scan options:"));
}

/*
 * plugins and feeds: plugins lists what loaded (nothing here, said in
 * words); feeds due and runs answer for the fixture's source.
 */
static void
test_plugins_and_feeds(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *source = id_text(fixture->source_id);
	const gchar *const plugins[] = { "plugins", "list", NULL };
	const gchar *const plugins_bad[] = { "plugins", "remove", NULL };
	const gchar *const due[] = { "feeds", "due", NULL };
	const gchar *const runs[] = { "feeds", "runs", source, NULL };
	g_autoptr(JsonNode) listed = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *table = NULL;

	(void)user_data;

	out = cli_ok(fixture, "json", plugins);
	listed = json_of(out);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(listed));
	table = cli_ok(fixture, "table", plugins);

	if (0 == json_array_get_length(json_node_get_array(listed)))
		g_assert_nonnull(strstr(table, "No plugins are loaded."));
	else
		g_assert_nonnull(strstr(table, "runtime"));

	cli_refused(fixture, plugins_bad, 2, "venturectl plugins list");

	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "json", due);
	g_clear_pointer(&out, g_free);
	out = cli_ok(fixture, "json", runs);
}

/*
 * A second organization through every family of verbs. Its source holds
 * the same herbs as the first one's, so the same question finds the same
 * flip -- under the second source's key, recorded into the second
 * organization -- when organization_id names it, and the first
 * organization's answer when it does not: a token's active organization
 * is always the default one, so this parameter is the only way a second
 * organization's market data is reached from the command line. An
 * organization that does not exist is NOT_FOUND (exit 3), and words after
 * a verb's positional ones that are not organization_id are refused.
 */
static void
test_second_organization(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(JsonNode) venue = NULL;
	g_autoptr(JsonNode) trade = NULL;
	g_autofree gchar *org = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *home = id_text(fixture->source_id);
	g_autofree gchar *out = NULL;
	g_autofree gchar *key = NULL;
	gint64 other_org;
	JsonArray *rows;

	(void)user_data;

	organization = VENTURE_ENTITY(venture_organization_new());
	g_object_set(organization, "name", "Evermoor Trading", "slug", "evermoor", NULL);
	save(fixture, organization);
	other_org = ID(organization);
	org = g_strdup_printf("organization_id=%" G_GINT64_FORMAT, other_org);

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), other_org);
	g_object_set(source, "name", "Evermoor auctions", "provider", "file_jsonl", "settings", "file: x.jsonl",
	             "schedule", "manual", "currency", "USD", "instrument-namespace", "item",
	             "venue-namespace", "realm", NULL);
	save(fixture, source);
	other = id_text(ID(source));
	seed_store(fixture, ID(source));
	key = g_strdup_printf("spread:%s:realm-a>%s:realm-b:herb", other, other);

	/* feeds: the other source's runs need its organization named. */
	{
		const gchar *const runs[] = { "feeds", "runs", other, org, NULL };
		const gchar *const unnamed[] = { "feeds", "runs", other, NULL };
		const gchar *const due[] = { "feeds", "due", org, NULL };
		const gchar *const extra[] = { "feeds", "runs", other, "count=2", NULL };

		out = cli_ok(fixture, "json", runs);
		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", due);
		g_clear_pointer(&out, g_free);
		cli_refused(fixture, unnamed, 3, "No such data source");
		cli_refused(fixture, extra, 2, "organization_id=N");
	}

	/* market: venues and a promotion, filed under the organization. */
	{
		const gchar *const venues[] = { "market", "venues", org, NULL };
		const gchar *const promote[] = { "market", "promote", other, "venue", "realm-a", org, NULL };
		const gchar *const unnamed[] = { "market", "promote", other, "venue", "realm-a", NULL };
		const gchar *const lists[] = { "market", "watchlist", org, NULL };
		const gchar *const browse[] = { "market", "browse", org, "search=Peacebloom", NULL };

		out = cli_ok(fixture, "json", venues);
		answer = json_of(out);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(answer),
		                                                                     "venues")), ==, 3);
		g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(json_object_get_array_member(
			json_node_get_object(answer), "venues"), 0), "data_source_id"), ==, ID(source));
		g_clear_pointer(&out, g_free);
		cli_refused(fixture, unnamed, 3, NULL);
		out = cli_ok(fixture, "json", promote);
		venue = json_of(out);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(venue), "organization_id"), ==,
		                other_org);
		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", lists);
		g_clear_pointer(&out, g_free);
		out = cli_ok(fixture, "json", browse);
		g_clear_pointer(&answer, json_node_unref);
		answer = json_of(out);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(answer),
		                                                                     "rows")), ==, 3);
		g_clear_pointer(&out, g_free);
	}

	/* arbitrage: the scan, its row recorded, its export. */
	{
		const gchar *const scan[] = { "arbitrage", "scan", "spread", "units=2", "category_path=Herbs", org,
		                              NULL };
		const gchar *const by_row[] = { "arbitrage", "record", "1", "spread", "units=2", "category_path=Herbs",
		                                org, NULL };
		const gchar *const export[] = { "arbitrage", "export", "csv", "spread", "units=2",
		                                "category_path=Herbs", org, NULL };
		const gchar *const nowhere[] = { "arbitrage", "scan", "organization_id=999999", NULL };
		const gchar *const not_a_number[] = { "arbitrage", "scan", "organization_id=two", NULL };
		const gchar *const home_scan[] = { "arbitrage", "scan", "spread", "units=2", "category_path=Herbs",
		                                   NULL };

		out = cli_ok(fixture, "json", scan);
		g_clear_pointer(&answer, json_node_unref);
		answer = json_of(out);
		rows = json_object_get_array_member(json_node_get_object(answer), "rows");
		g_assert_cmpuint(json_array_get_length(rows), ==, 1);
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "key"), ==, key);
		g_clear_pointer(&out, g_free);

		/* The default is unchanged: the first organization's flip. */
		out = cli_ok(fixture, "json", home_scan);
		g_assert_null(strstr(out, key));
		g_assert_nonnull(strstr(out, home));
		g_clear_pointer(&out, g_free);

		out = cli_ok(fixture, "json", by_row);
		trade = json_of(out);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(trade), "organization_id"), ==,
		                other_org);
		g_clear_pointer(&out, g_free);

		out = cli_ok(fixture, NULL, export);
		g_assert_nonnull(strstr(out, key));
		g_clear_pointer(&out, g_free);

		cli_refused(fixture, nowhere, 3, "There is no organization 999999");
		cli_refused(fixture, not_a_number, 2, "organization_id is a whole number");
	}
}

/*
 * feeds push ID FILE --wait: the file goes as JSON lines, the server
 * waits for the run, and the run is printed. A push without a file is a
 * usage error; a push to a source that is not a push source is the
 * server's refusal, with its exit code.
 *
 * What breaks if this regresses: the shell path tsmctl's systemd unit
 * could fall back to (an export piped into venturectl) pushes nothing, or
 * reports success for a refused push.
 */
static void
test_feeds_push(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *out = NULL;
	JsonObject *run;

	(void)user_data;

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Characters", "provider", "push", "currency", "USD", NULL);
	save(fixture, source);
	id = id_text(ID(source));
	other = id_text(fixture->source_id);

	path = g_build_filename(fixture->state_dir, "export.jsonl", NULL);
	g_assert_true(g_file_set_contents(path,
		"{\"type\":\"account\",\"key\":\"Drgold-Thorium\",\"kind\":\"character\"}\n"
		"{\"type\":\"txn\",\"id\":\"k1\",\"account\":\"Drgold-Thorium\",\"kind\":\"sale\","
		"\"instrument\":\"2770\",\"quantity\":3,\"amount\":\"79.77\","
		"\"at\":\"2026-10-03T18:53:54Z\"}\n", -1, NULL));

	{
		const gchar *const push[] = { "feeds", "push", id, path, "--wait", NULL };

		out = cli_ok(fixture, "json", push);
		node = json_of(out);
		run = json_node_get_object(node);
		g_assert_cmpstr(json_object_get_string_member(run, "status"), ==, "ok");
		g_assert_cmpstr(json_object_get_string_member(run, "trigger"), ==, "push");
		g_assert_cmpint(json_object_get_int_member(run, "data_source_id"), ==, ID(source));
	}

	{
		const gchar *const queued[] = { "feeds", "push", id, path, NULL };

		g_clear_pointer(&out, g_free);
		g_clear_pointer(&node, json_node_unref);
		out = cli_ok(fixture, "json", queued);
		node = json_of(out);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "status"), ==,
		                "queued");
	}

	{
		const gchar *const no_file[] = { "feeds", "push", id, NULL };
		const gchar *const not_push[] = { "feeds", "push", other, path, NULL };

		cli_refused(fixture, no_file, 2, "feeds push ID FILE|-");
		cli_refused(fixture, not_push, 4, "not by pushes");
	}
}

#define ADD(name, fn) \
	g_test_add("/trading-cli/" name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	ADD("help", test_help);
	ADD("market", test_market);
	ADD("market-lists-and-alerts", test_market_lists_and_alerts);
	ADD("scan", test_scan);
	ADD("record", test_record);
	ADD("actions", test_actions);
	ADD("calc", test_calc);
	ADD("export-and-registries", test_export_and_registries);
	ADD("plugins-and-feeds", test_plugins_and_feeds);
	ADD("feeds-push", test_feeds_push);
	ADD("second-organization", test_second_organization);

	return g_test_run();
}

#else /* !VENTURE_HAVE_SQLITE */

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);
	return g_test_run();
}

#endif /* VENTURE_HAVE_SQLITE */
