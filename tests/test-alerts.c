/*
 * test-alerts.c - Alert rules over market data, and their hits
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Most tests seed a data source's series store directly through its
 * writer, close it, and evaluate a rule by hand -- the same evaluation a
 * feed run makes, over a read handle -- so every kind is held to figures
 * the test chose. One test runs the real feeds worker over a JSON-lines
 * file, which is the path that finds candidates on the worker thread and
 * writes them on the main one. Each fixture has its own state directory,
 * removed with venture_test_remove_tree().
 */

#include <venture.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "venture-test-util.h"

/* A build without SQLite has no series store to evaluate against. */
#ifdef VENTURE_HAVE_SQLITE

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

typedef struct
{
	gchar			*state_dir;
	gchar			*file_root;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	gint64			 org;
	gint64			 source_id;
	gint64			 now;
	gint64			 t0;
	gint64			 t1;
	gint64			 t2;
} Fixture;

/* --- Helpers ---------------------------------------------------------------- */

static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_nonnull(error);
	g_assert_false(saved);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static VentureMoney *
money_of(const gchar *text)
{
	VentureMoney *money;

	money = venture_money_from_string(text, NULL, NULL);
	g_assert_nonnull(money);

	return money;
}

static VentureEntity *
reread(
	Fixture	*fixture,
	GType	 type,
	gint64	 id
){
	g_autoptr(GError) error = NULL;
	VentureEntity *record;

	record = venture_database_get(fixture->database, type, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(record);

	return record;
}

static gint64
count_of(
	Fixture		*fixture,
	GType		 type,
	const gchar	*field,
	gint64		 value
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	gint64 count;

	query = venture_query_new(type);

	if (NULL != field)
		g_assert_true(venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, value, &error));

	count = venture_database_count(fixture->database, query, &error);
	g_assert_no_error(error);

	return count;
}

/* An active editor's membership of @organization_id: an alert's recipient
 * must be able to read the organization the hit is about. */
static void
member_of(
	Fixture		*fixture,
	gint64		 user_id,
	gint64		 organization_id
){
	g_autoptr(VentureEntity) membership = NULL;

	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", user_id,
	                          "organization-id", organization_id,
	                          "role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
	save(fixture, membership);
}

static gint64
user(
	Fixture		*fixture,
	const gchar	*username
){
	g_autoptr(VentureUser) record = NULL;

	record = venture_user_new();
	g_object_set(record, "username", username, "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	save(fixture, record);
	member_of(fixture, ID(record), fixture->org);

	return ID(record);
}

/* An alert rule of @kind, filed under the default organization, unsaved. */
static VentureEntity *
new_rule(
	Fixture			*fixture,
	const gchar		*name,
	VentureAlertKind	 kind
){
	VentureEntity *rule;

	rule = VENTURE_ENTITY(venture_alert_rule_new());
	venture_entity_set_organization_id(rule, fixture->org);
	g_object_set(rule, "name", name, "kind", kind, NULL);

	return rule;
}

/* An instrument record naming the store's key, in the fixture's source:
 * the one already there, or a new one (a key is unique per namespace). */
static gint64
instrument(
	Fixture		*fixture,
	const gchar	*key,
	gint64		 product_id
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
	g_assert_true(venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, key, NULL));
	record = venture_database_find_one(fixture->database, query, NULL);

	if (NULL != record)
		return ID(record);

	record = VENTURE_ENTITY(venture_instrument_new());
	venture_entity_set_organization_id(record, fixture->org);
	g_object_set(record, "name", key, "key", key, "namespace", "wow-item",
	             "data-source-id", fixture->source_id, "product-id", product_id, NULL);
	save(fixture, record);

	return ID(record);
}

static gint64
venue(
	Fixture		*fixture,
	const gchar	*key
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_venue_new());
	venture_entity_set_organization_id(record, fixture->org);
	g_object_set(record, "name", key, "key", key, "namespace", "realm",
	             "data-source-id", fixture->source_id, NULL);
	save(fixture, record);

	return ID(record);
}

/* Evaluates @rule now and hands back the report. */
static JsonObject *
evaluate(
	Fixture		*fixture,
	VentureEntity	*rule,
	gboolean	 record
){
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_marketdata_alerts_evaluate(fixture->context, rule, record, &report, &error));
	g_assert_no_error(error);
	g_assert_nonnull(report);

	return json_object_ref(json_node_get_object(report));
}

static gint64
report_int(
	JsonObject	*report,
	const gchar	*member
){
	return json_object_get_int_member(report, member);
}

/* Whether some match's subject is @subject. */
static gboolean
report_has(
	JsonObject	*report,
	const gchar	*subject
){
	JsonArray *matches;
	guint i;

	matches = json_object_get_array_member(report, "matches");

	for (i = 0; i < json_array_get_length(matches); i++)
	{
		if (0 == g_strcmp0(json_object_get_string_member(json_array_get_object_element(matches, i),
		                                                  "subject"), subject))
			return TRUE;
	}

	return FALSE;
}

static const gchar *
report_message(
	JsonObject	*report,
	guint		 index
){
	return json_object_get_string_member(
		json_array_get_object_element(json_object_get_array_member(report, "matches"), index),
		"message");
}

/* Evaluates @rule as of @now -- the account kinds' clock -- and hands
 * back the report. */
static JsonObject *
evaluate_at(
	Fixture		*fixture,
	VentureEntity	*rule,
	gboolean	 record,
	gint64		 now
){
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_marketdata_alerts_evaluate_at(fixture->context, rule, record, now, &report,
	                                                    &error));
	g_assert_no_error(error);
	g_assert_nonnull(report);

	return json_object_ref(json_node_get_object(report));
}

/* The message of the match about @subject; fails the test without one. */
static const gchar *
report_says(
	JsonObject	*report,
	const gchar	*subject
){
	JsonArray *matches;
	guint i;

	matches = json_object_get_array_member(report, "matches");

	for (i = 0; i < json_array_get_length(matches); i++)
	{
		JsonObject *match = json_array_get_object_element(matches, i);

		if (0 == g_strcmp0(json_object_get_string_member(match, "subject"), subject))
			return json_object_get_string_member(match, "message");
	}

	g_error("no match about %s", subject);
	return NULL;
}

/* --- The store -------------------------------------------------------------- */

static VentureSeriesStore *
writer(Fixture *fixture)
{
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	VentureSeriesStore *store;

	source = reread(fixture, VENTURE_TYPE_DATA_SOURCE, fixture->source_id);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	return store;
}

static void
add_venue(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*group,
	const gchar		*currency,
	gint64			 seen_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesVenue venue;

	memset(&venue, 0, sizeof(venue));
	venue.key = key;
	venue.namespace_ = "realm";
	venue.name = key;
	venue.kind = "auction_house";
	venue.group_key = group;
	venue.currency = currency;
	g_assert_true(venture_series_store_upsert_venue(store, &venue, seen_at, &error));
	g_assert_no_error(error);
}

static void
add_instrument(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*name,
	const gchar		*category,
	gint64			 seen_at
){
	g_autoptr(GError) error = NULL;
	VentureSeriesInstrument instrument;
	gboolean created;
	gboolean refused;

	memset(&instrument, 0, sizeof(instrument));
	instrument.key = key;
	instrument.namespace_ = "wow-item";
	instrument.name = name;
	instrument.kind = "item";
	instrument.category = category;
	g_assert_true(venture_series_store_upsert_instrument(store, &instrument, seen_at,
	                                                     &created, &refused, &error));
	g_assert_no_error(error);
	g_assert_false(refused);
}

typedef struct
{
	const gchar	*instrument;
	guint64		 id;
	gint64		 price;
	gint64		 quantity;
} Offer;

/* One complete snapshot: every offer the venue lists at @taken_at. */
static void
snapshot(
	VentureSeriesStore	*store,
	const gchar		*venue,
	const gchar		*currency,
	gint64			 taken_at,
	const Offer		*offers,
	guint			 n_offers
){
	g_autoptr(GError) error = NULL;
	VentureSeriesSnapshot *snap;
	VentureSeriesCommitResult result;
	guint i;

	snap = venture_series_store_begin_snapshot(store, venue, currency, taken_at, taken_at + 60,
	                                           TRUE, &error);
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
	g_assert_false(result.duplicate);
}

/*
 * The store every evaluation reads. Four realms: a, b and d in the EU
 * group in USD, c in the US in EUR. Copper ore ("ore") everywhere: at a
 * it fell from 1.50 to 1.20 between yesterday and an hour ago, b has it at
 * 1.10, d at 1.40 (one unit), c at 1.00 EUR. Peacebloom ("herb") sold out
 * at a an hour ago, and came back at b an hour ago after selling out
 * yesterday. A copper bar ("bar") was first seen at a an hour ago. The EU
 * region median of ore's minimums is 1.20.
 */
static void
seed_store(Fixture *fixture)
{
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	static const Offer a0[] = { { "ore", 1, 150, 10 }, { "herb", 3, 75, 12 } };
	static const Offer a2[] = { { "ore", 5, 120, 2 }, { "ore", 4, 130, 3 }, { "bar", 6, 500, 1 } };
	static const Offer b0[] = { { "ore", 10, 110, 4 }, { "herb", 11, 80, 2 } };
	static const Offer b1[] = { { "ore", 10, 110, 4 } };
	static const Offer b2[] = { { "ore", 10, 110, 4 }, { "herb", 12, 85, 1 } };
	static const Offer c2[] = { { "ore", 20, 100, 7 } };
	static const Offer d2[] = { { "ore", 30, 140, 1 } };
	VentureSeriesEntry entries[2];

	store = writer(fixture);

	add_venue(store, "realm-a", "eu", "USD", fixture->t0);
	add_venue(store, "realm-b", "eu", "USD", fixture->t0);
	add_venue(store, "realm-c", "us", "EUR", fixture->t0);
	add_venue(store, "realm-d", "eu", "USD", fixture->t0);
	add_instrument(store, "ore", "Copper Ore", "Materials/Ore", fixture->t0);
	add_instrument(store, "herb", "Peacebloom", "Herbs", fixture->t0);
	add_instrument(store, "bar", "Copper Bar", "Materials/Bars", fixture->t0);

	snapshot(store, "realm-a", "USD", fixture->t0, a0, G_N_ELEMENTS(a0));
	snapshot(store, "realm-a", "USD", fixture->t1, a0, G_N_ELEMENTS(a0));
	snapshot(store, "realm-a", "USD", fixture->t2, a2, G_N_ELEMENTS(a2));
	snapshot(store, "realm-b", "USD", fixture->t0, b0, G_N_ELEMENTS(b0));
	snapshot(store, "realm-b", "USD", fixture->t1, b1, G_N_ELEMENTS(b1));
	snapshot(store, "realm-b", "USD", fixture->t2, b2, G_N_ELEMENTS(b2));
	snapshot(store, "realm-c", "EUR", fixture->t2, c2, G_N_ELEMENTS(c2));
	snapshot(store, "realm-d", "USD", fixture->t2, d2, G_N_ELEMENTS(d2));

	g_assert_true(venture_series_store_recompute_region(store, NULL, fixture->now,
	                                                    VENTURE_SERIES_NONE, NULL, NULL, &error));
	g_assert_no_error(error);

	memset(entries, 0, sizeof(entries));
	entries[0].key = "n1";
	entries[0].title = "Patch notes: Copper Ore drop rates halved";
	entries[0].url = "https://example.com/n1";
	entries[0].published_at = VENTURE_SERIES_NONE;
	entries[0].instrument_key = "ore";
	entries[1].key = "n2";
	entries[1].title = "Server maintenance";
	entries[1].summary = "Nothing about ore here? Yes: ORE.";
	entries[1].published_at = VENTURE_SERIES_NONE;
	g_assert_true(venture_series_store_add_entries(store, entries, G_N_ELEMENTS(entries),
	                                               fixture->now - 600, NULL, &error));
	g_assert_no_error(error);
}

/* --- The fixture ------------------------------------------------------------ */

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDataSource) source = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-alerts-XXXXXX", &error);
	g_assert_no_error(error);
	fixture->file_root = g_build_filename(fixture->state_dir, "files", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->file_root, 0700), ==, 0);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, "feeds-enabled", TRUE,
	             "feeds-file-roots", fixture->file_root, "feeds-run-window-minutes", (gint64)0,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* The registry is process-wide; with this context's switches applied
	 * the feeds and marketdata tables exist now. */
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	/* Manual, so nothing starts a worker behind the test's back. */
	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Auctions", "provider", "file_jsonl",
	             "settings", "file: realm.jsonl", "schedule", "manual", "currency", "USD",
	             "instrument-namespace", "wow-item", "venue-namespace", "realm", NULL);
	save(fixture, source);
	fixture->source_id = ID(source);

	fixture->now = g_get_real_time() / G_USEC_PER_SEC;
	fixture->t0 = fixture->now - 2 * 86400;
	fixture->t1 = fixture->now - 86400;
	fixture->t2 = fixture->now - 3600;
}

/* Lets the main loop take feed runs back until nothing is pending,
 * bounded: a test that can hang tells less than one that fails. */
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

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_clear_pointer(&fixture->file_root, g_free);

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* --- The rule's own rules ---------------------------------------------------- */

typedef struct
{
	const gchar		*why;
	VentureAlertKind	 kind;
	const gchar		*threshold;	/* money, or NULL */
	gdouble			 number;
	const gchar		*pattern;
	VentureMarketdataBasis	 basis;
	gint64			 window;
	gint64			 cooldown;
	gboolean		 scoped;
	const gchar		*fragment;	/* NULL: saved */
} RuleCase;

/*
 * Every kind carries exactly what it reads: a threshold where it compares
 * one, nothing where it does not, a basis it can read, a window for a
 * spike, a bounded plain-text pattern for a match; and every rule but an
 * undercut or a match watches something.
 *
 * What breaks if this regresses: a rule saved without its threshold never
 * fires and never says why; a price left on an out_of_stock rule reads as
 * if it mattered; a spike over a basis the hourly series does not keep
 * compares nothing; an unbounded pattern is work nobody bounded; an
 * expiring rule with no hours watches nothing, and an instrument on a
 * collect_ready rule reads as if it narrowed what is waiting.
 */
static void
test_alerts_rule_validation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *long_pattern = NULL;
	gint64 ore;
	guint i;
	const RuleCase cases[] = {
		{ "below needs a price", VENTURE_ALERT_KIND_BELOW, NULL, 0, NULL, 0, 0, 60, TRUE, "is required" },
		{ "below reads no number", VENTURE_ALERT_KIND_BELOW, "1.00 USD", 5, NULL, 0, 0, 60, TRUE, "does not apply" },
		{ "below is fine", VENTURE_ALERT_KIND_BELOW, "1.00 USD", 0, NULL, 0, 0, 60, TRUE, NULL },
		{ "a negative price", VENTURE_ALERT_KIND_ABOVE, "-1.00 USD", 0, NULL, 0, 0, 60, TRUE, "negative" },
		{ "below watches something", VENTURE_ALERT_KIND_BELOW, "1.00 USD", 0, NULL, 0, 0, 60, FALSE, "watches something" },
		{ "pct is not against min", VENTURE_ALERT_KIND_PCT_VS_REFERENCE, NULL, 80, NULL, VENTURE_MARKETDATA_BASIS_MIN, 0, 60, TRUE, "reference" },
		{ "pct needs a percent", VENTURE_ALERT_KIND_PCT_VS_REFERENCE, NULL, 0, NULL, VENTURE_MARKETDATA_BASIS_REGION_MEDIAN, 0, 60, TRUE, "percent" },
		{ "pct is fine", VENTURE_ALERT_KIND_PCT_VS_REFERENCE, NULL, 80, NULL, VENTURE_MARKETDATA_BASIS_REGION_MEDIAN, 0, 60, TRUE, NULL },
		{ "spread sells at a price", VENTURE_ALERT_KIND_SPREAD, "0.10 USD", 0, NULL, VENTURE_MARKETDATA_BASIS_QUANTITY, 0, 60, TRUE, "is a number" },
		{ "shortage counts units", VENTURE_ALERT_KIND_SHORTAGE, NULL, 0.5, NULL, 0, 0, 60, TRUE, "at least 1" },
		{ "shortage reads no price", VENTURE_ALERT_KIND_SHORTAGE, "1.00 USD", 3, NULL, 0, 0, 60, TRUE, "does not apply" },
		{ "spike needs a window", VENTURE_ALERT_KIND_SPIKE, NULL, 20, NULL, VENTURE_MARKETDATA_BASIS_MIN, 0, 60, TRUE, "1 to 336" },
		{ "spike window bounded", VENTURE_ALERT_KIND_SPIKE, NULL, 20, NULL, VENTURE_MARKETDATA_BASIS_MIN, 337, 60, TRUE, "1 to 336" },
		{ "spike reads the hourly series", VENTURE_ALERT_KIND_SPIKE, NULL, 20, NULL, VENTURE_MARKETDATA_BASIS_REGION_MEDIAN, 6, 60, TRUE, "hourly" },
		{ "spike needs a change", VENTURE_ALERT_KIND_SPIKE, NULL, 0, NULL, VENTURE_MARKETDATA_BASIS_MIN, 6, 60, TRUE, "other than 0" },
		{ "a window only for a spike", VENTURE_ALERT_KIND_OUT_OF_STOCK, NULL, 0, NULL, 0, 6, 60, TRUE, "does not apply" },
		{ "out of stock reads no price", VENTURE_ALERT_KIND_OUT_OF_STOCK, "1.00 USD", 0, NULL, 0, 0, 60, TRUE, "does not apply" },
		{ "a match needs a pattern", VENTURE_ALERT_KIND_ENTRY_MATCH, NULL, 0, "   ", 0, 0, 60, FALSE, "is required" },
		{ "a match is fine unscoped", VENTURE_ALERT_KIND_ENTRY_MATCH, NULL, 0, "nerf", 0, 0, 60, FALSE, NULL },
		{ "a pattern only for a match", VENTURE_ALERT_KIND_BACK_IN_STOCK, NULL, 0, "nerf", 0, 0, 60, TRUE, "does not apply" },
		{ "undercut is fine unscoped", VENTURE_ALERT_KIND_UNDERCUT, NULL, 0, NULL, 0, 0, 60, FALSE, NULL },
		{ "cooldown bounded", VENTURE_ALERT_KIND_UNDERCUT, NULL, 0, NULL, 0, 0, -1, FALSE, "Cooldown" },
		{ "expiring needs hours", VENTURE_ALERT_KIND_POSITION_EXPIRING, NULL, 0, NULL, 0, 0, 60, FALSE, "hours ahead" },
		{ "expiring hours bounded", VENTURE_ALERT_KIND_POSITION_EXPIRING, NULL, 721, NULL, 0, 0, 60, FALSE, "at most 720" },
		{ "expiring reads no price", VENTURE_ALERT_KIND_POSITION_EXPIRING, "1.00 USD", 2, NULL, 0, 0, 60, FALSE, "does not apply" },
		{ "expiring is fine unscoped", VENTURE_ALERT_KIND_POSITION_EXPIRING, NULL, 2, NULL, 0, 0, 60, FALSE, NULL },
		{ "expiring may be scoped", VENTURE_ALERT_KIND_POSITION_EXPIRING, NULL, 720, NULL, 0, 0, 60, TRUE, NULL },
		{ "mail expiring takes part hours", VENTURE_ALERT_KIND_INBOUND_EXPIRING, NULL, 0.5, NULL, 0, 0, 60, FALSE, NULL },
		{ "mail expiring needs hours", VENTURE_ALERT_KIND_INBOUND_EXPIRING, NULL, -1, NULL, 0, 0, 60, FALSE, "hours ahead" },
		{ "stale needs days", VENTURE_ALERT_KIND_ACCOUNT_STALE, NULL, 0, NULL, 0, 0, 60, FALSE, "days an account" },
		{ "stale days bounded", VENTURE_ALERT_KIND_ACCOUNT_STALE, NULL, 366, NULL, 0, 0, 60, FALSE, "at most 365" },
		{ "stale watches accounts", VENTURE_ALERT_KIND_ACCOUNT_STALE, NULL, 7, NULL, 0, 0, 60, TRUE, "watches accounts" },
		{ "stale reads no pattern", VENTURE_ALERT_KIND_ACCOUNT_STALE, NULL, 7, "x", 0, 0, 60, FALSE, "does not apply" },
		{ "stale is fine", VENTURE_ALERT_KIND_ACCOUNT_STALE, NULL, 7, NULL, 0, 0, 60, FALSE, NULL },
		{ "collect reads no number", VENTURE_ALERT_KIND_COLLECT_READY, NULL, 1, NULL, 0, 0, 60, FALSE, "does not apply" },
		{ "collect watches accounts", VENTURE_ALERT_KIND_COLLECT_READY, NULL, 0, NULL, 0, 0, 60, TRUE, "watches accounts" },
		{ "collect reads no window", VENTURE_ALERT_KIND_COLLECT_READY, NULL, 0, NULL, 0, 6, 60, FALSE, "does not apply" },
		{ "collect is fine", VENTURE_ALERT_KIND_COLLECT_READY, NULL, 0, NULL, 0, 0, 60, FALSE, NULL },
	};

	(void)user_data;

	ore = instrument(fixture, "ore", 0);

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureEntity) rule = NULL;
		g_autoptr(VentureMoney) threshold = NULL;

		g_test_message("%s", cases[i].why);
		rule = new_rule(fixture, cases[i].why, cases[i].kind);

		if (NULL != cases[i].threshold)
			threshold = money_of(cases[i].threshold);

		g_object_set(rule, "threshold", threshold, "threshold-number", cases[i].number,
		             "pattern", cases[i].pattern, "basis", cases[i].basis,
		             "window-hours", cases[i].window, "cooldown-minutes", cases[i].cooldown,
		             "instrument-id", cases[i].scoped ? ore : (gint64)0, NULL);

		if (NULL == cases[i].fragment)
			save(fixture, rule);
		else
			save_refused(fixture, rule, cases[i].fragment);
	}

	/* A pattern is bounded text, never an expression. */
	long_pattern = g_strnfill(VENTURE_ALERTS_MAX_PATTERN + 1, 'x');
	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "long", VENTURE_ALERT_KIND_ENTRY_MATCH);

		g_object_set(rule, "pattern", long_pattern, NULL);
		save_refused(fixture, rule, "at most");
	}

	/* A venue or a group, and only a user who exists to tell. */
	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "where", VENTURE_ALERT_KIND_UNDERCUT);

		g_object_set(rule, "venue-id", venue(fixture, "realm-a"), "group-key", "eu", NULL);
		save_refused(fixture, rule, "not both");
		g_object_set(rule, "group-key", NULL, "notify-username", "nobody", NULL);
		save_refused(fixture, rule, "no active user");
		user(fixture, "trader");
		g_object_set(rule, "notify-username", "trader", NULL);
		save(fixture, rule);
	}

	/* A user of another organization is no recipient: the hit carries
	 * this organization's instruments and prices to their inbox. The
	 * refusal is the same as for nobody, so it says nothing of who
	 * exists elsewhere. */
	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "elsewhere", VENTURE_ALERT_KIND_UNDERCUT);
		g_autoptr(VentureEntity) other = NULL;
		g_autoptr(VentureUser) outsider = venture_user_new();

		other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other books", "slug", "other-books", NULL);
		save(fixture, other);
		g_object_set(outsider, "username", "outsider", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
		save(fixture, outsider);
		member_of(fixture, ID(outsider), ID(other));
		g_object_set(rule, "notify-username", "outsider", NULL);
		save_refused(fixture, rule, "no active user of this organization");
	}

	/* A category of instruments is a tree for instruments, and a rule may
	 * name one: the reference is judged as an instrument's would be. */
	{
		g_autoptr(VentureEntity) category = NULL;
		g_autoptr(VentureEntity) rule = NULL;

		category = VENTURE_ENTITY(venture_category_new());
		venture_entity_set_organization_id(category, fixture->org);
		g_object_set(category, "name", "Herbs", "applies-to", "instrument", NULL);
		save(fixture, category);

		rule = new_rule(fixture, "herbs", VENTURE_ALERT_KIND_OUT_OF_STOCK);
		g_object_set(rule, "category-id", ID(category), NULL);
		save(fixture, rule);
	}

	/* A new rule is on, with an hour's quiet. */
	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "defaults", VENTURE_ALERT_KIND_BELOW);
		gboolean enabled;
		gint64 cooldown;

		g_object_get(rule, "enabled", &enabled, "cooldown-minutes", &cooldown, NULL);
		g_assert_true(enabled);
		g_assert_cmpint(cooldown, ==, 60);
	}
}

/* --- Every kind -------------------------------------------------------------- */

/* A saved rule of @kind on instrument @key. */
static VentureEntity *
rule_on(
	Fixture			*fixture,
	VentureAlertKind	 kind,
	const gchar		*key
){
	VentureEntity *rule;

	rule = new_rule(fixture, "rule", kind);
	g_object_set(rule, "instrument-id", instrument(fixture, key, 0), NULL);

	return rule;
}

/*
 * Each kind fires on exactly the venues the seeded figures say it should,
 * and on no other: the venue's newest snapshot, in the threshold's
 * currency, compared the way the docs say.
 *
 * What breaks if this regresses: a below rule firing on a euro price
 * against a dollar threshold, a pct rule read against the wrong region
 * figure, a spread pairing a venue with itself, a stock alert firing on a
 * first sighting (every instrument of a first sync) or not at all.
 */
static void
test_alerts_kinds(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	seed_store(fixture);

	/* below: a at 1.20 and b at 1.10 are at or under 1.20; d is not; c is
	 * in euros, which a dollar threshold says nothing about. */
	{
		g_autoptr(VentureEntity) rule = rule_on(fixture, VENTURE_ALERT_KIND_BELOW, "ore");
		g_autoptr(VentureMoney) threshold = money_of("1.20 USD");
		g_autoptr(JsonObject) report = NULL;

		g_object_set(rule, "threshold", threshold, NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 2);
		g_assert_true(report_has(report, "realm-a|ore"));
		g_assert_true(report_has(report, "realm-b|ore"));
		g_assert_cmpint(report_int(report, "written"), ==, 0);
	}

	/* above: only d, at 1.40, is at or over 1.25. */
	{
		g_autoptr(VentureEntity) rule = NULL;
		g_autoptr(VentureMoney) threshold = money_of("1.25 USD");
		g_autoptr(JsonObject) report = NULL;

		rule = new_rule(fixture, "above", VENTURE_ALERT_KIND_ABOVE);
		g_object_set(rule, "threshold", threshold, NULL);

		/* Scoped by a watchlist this time: its entries' keys. */
		{
			g_autoptr(VentureEntity) list = VENTURE_ENTITY(venture_watchlist_new());
			g_autoptr(VentureEntity) entry = VENTURE_ENTITY(venture_watchlist_entry_new());

			venture_entity_set_organization_id(list, fixture->org);
			g_object_set(list, "name", "Ores", "group-key", "eu", NULL);
			save(fixture, list);
			venture_entity_set_organization_id(entry, fixture->org);
			g_object_set(entry, "watchlist-id", ID(list), "instrument-id",
			             instrument(fixture, "ore", 0), NULL);
			save(fixture, entry);
			g_object_set(rule, "watchlist-id", ID(list), NULL);
		}
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "realm-d|ore"));
		g_assert_nonnull(strstr(report_message(report, 0), "at or above 1.25 USD"));
	}

	/* pct_vs_reference: against the EU median of minimums (1.20), only b
	 * (1.10, 91.7%) is at or under 95%; c's region is another. */
	{
		g_autoptr(VentureEntity) rule = rule_on(fixture, VENTURE_ALERT_KIND_PCT_VS_REFERENCE, "ore");
		g_autoptr(JsonObject) report = NULL;

		g_object_set(rule, "threshold-number", 95.0,
		             "basis", VENTURE_MARKETDATA_BASIS_REGION_MEDIAN, NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "realm-b|ore"));
		g_assert_nonnull(strstr(report_message(report, 0), "91.7%"));
	}

	/* spread: buy at b (1.10), sell at the best other EU minimum, d's
	 * 1.40: 0.30 apart, at least 0.25 -- and about the buy venue only. */
	{
		g_autoptr(VentureEntity) rule = rule_on(fixture, VENTURE_ALERT_KIND_SPREAD, "ore");
		g_autoptr(VentureMoney) threshold = money_of("0.25 USD");
		g_autoptr(JsonObject) report = NULL;

		g_object_set(rule, "threshold", threshold, "basis", VENTURE_MARKETDATA_BASIS_MIN,
		             "group-key", "eu", NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "realm-b|ore"));
		g_assert_nonnull(strstr(report_message(report, 0), "sell at realm-d"));

		/* Wider than the gap: nothing. */
		g_clear_pointer(&threshold, venture_money_free);
		threshold = money_of("0.31 USD");
		g_object_set(rule, "threshold", threshold, NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 0);
	}

	/* out_of_stock and back_in_stock: the snapshot that crossed zero, and
	 * never a first sighting. */
	{
		g_autoptr(VentureEntity) out = rule_on(fixture, VENTURE_ALERT_KIND_OUT_OF_STOCK, "herb");
		g_autoptr(VentureEntity) back = rule_on(fixture, VENTURE_ALERT_KIND_BACK_IN_STOCK, "herb");
		g_autoptr(VentureEntity) first = rule_on(fixture, VENTURE_ALERT_KIND_BACK_IN_STOCK, "bar");
		g_autoptr(JsonObject) out_report = NULL;
		g_autoptr(JsonObject) back_report = NULL;
		g_autoptr(JsonObject) first_report = NULL;

		save(fixture, out);
		save(fixture, back);
		save(fixture, first);
		out_report = evaluate(fixture, out, FALSE);
		back_report = evaluate(fixture, back, FALSE);
		first_report = evaluate(fixture, first, FALSE);
		g_assert_cmpint(report_int(out_report, "candidates"), ==, 1);
		g_assert_true(report_has(out_report, "realm-a|herb"));
		g_assert_cmpint(report_int(back_report, "candidates"), ==, 1);
		g_assert_true(report_has(back_report, "realm-b|herb"));
		g_assert_nonnull(strstr(report_message(back_report, 0), "back in stock at realm-b: 1 on offer"));
		g_assert_cmpint(report_int(first_report, "candidates"), ==, 0);
	}

	/* shortage: fewer than 3 on offer is d (1); a has 5, b 4, c 7. */
	{
		g_autoptr(VentureEntity) rule = rule_on(fixture, VENTURE_ALERT_KIND_SHORTAGE, "ore");
		g_autoptr(JsonObject) report = NULL;

		g_object_set(rule, "threshold-number", 3.0, NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "realm-d|ore"));
	}

	/* spike: a's minimum fell 20% against the hourly point twelve hours
	 * or more back; b's did not move. A rise of 10% finds nothing. */
	{
		g_autoptr(VentureEntity) rule = rule_on(fixture, VENTURE_ALERT_KIND_SPIKE, "ore");
		g_autoptr(JsonObject) report = NULL;

		g_object_set(rule, "threshold-number", -15.0, "basis", VENTURE_MARKETDATA_BASIS_MIN,
		             "window-hours", (gint64)12, NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "realm-a|ore"));
		g_assert_nonnull(strstr(report_message(report, 0), "min fell 20.0% in 12 hours"));

		g_object_set(rule, "threshold-number", 10.0, NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 0);
	}

	/* entry_match: plain text, either case, title or summary. */
	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "news", VENTURE_ALERT_KIND_ENTRY_MATCH);
		g_autoptr(JsonObject) report = NULL;

		g_object_set(rule, "pattern", "copper ORE", NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "entry:n1"));
		g_assert_nonnull(strstr(report_message(report, 0), "https://example.com/n1"));

		/* A summary matches too; a scope keeps entries about others out. */
		g_object_set(rule, "pattern", "ore.", NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "entry:n2"));
		g_object_set(rule, "instrument-id", instrument(fixture, "ore-news", 0), NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate(fixture, rule, FALSE);
		g_assert_cmpint(report_int(report, "candidates"), ==, 0);
	}
}

/*
 * undercut: an open listing at its venue, against the cheapest unit that
 * venue offers now. A listing that is still the cheapest, one that has
 * ended, and one at a venue with no prices here are left alone.
 *
 * What breaks if this regresses: a seller is told about their own
 * listing, or about one sold last week, or never about the one a rival
 * just undercut.
 */
static void
test_alerts_undercut(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(GDateTime) listed = NULL;
	g_autoptr(JsonObject) report = NULL;
	gint64 realm_a;
	gint64 undercut_id = 0;
	guint i;
	const struct
	{
		const gchar		*price;
		VentureListingOutcome	 outcome;
		gboolean		 undercut;
	} listings[] = {
		{ "1.25 USD", VENTURE_LISTING_OUTCOME_OPEN, TRUE },	/* 1.20 is on offer */
		{ "1.15 USD", VENTURE_LISTING_OUTCOME_OPEN, FALSE },	/* still the cheapest */
		{ "1.50 USD", VENTURE_LISTING_OUTCOME_SOLD, FALSE },	/* ended */
	};

	(void)user_data;

	seed_store(fixture);

	product = VENTURE_ENTITY(venture_product_new());
	venture_entity_set_organization_id(product, fixture->org);
	g_object_set(product, "name", "Copper Ore", NULL);
	save(fixture, product);
	instrument(fixture, "ore", ID(product));
	realm_a = venue(fixture, "realm-a");
	listed = g_date_time_new_now_utc();

	for (i = 0; i < G_N_ELEMENTS(listings); i++)
	{
		g_autoptr(VentureEntity) listing = VENTURE_ENTITY(venture_listing_new());
		g_autoptr(VentureMoney) price = money_of(listings[i].price);

		venture_entity_set_organization_id(listing, fixture->org);
		g_object_set(listing, "product-id", ID(product), "quantity", (gint64)2,
		             "unit-price", price, "listed-at", listed, "venue-id", realm_a,
		             "outcome", listings[i].outcome,
		             "quantity-sold", (VENTURE_LISTING_OUTCOME_SOLD == listings[i].outcome)
		                              ? (gint64)2 : (gint64)0, NULL);
		save(fixture, listing);

		if (listings[i].undercut)
			undercut_id = ID(listing);
	}

	rule = new_rule(fixture, "my listings", VENTURE_ALERT_KIND_UNDERCUT);
	save(fixture, rule);
	report = evaluate(fixture, rule, TRUE);
	g_assert_cmpint(report_int(report, "candidates"), ==, 1);
	g_assert_cmpint(report_int(report, "written"), ==, 1);

	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
		g_autoptr(VentureEntity) hit = NULL;
		g_autoptr(VentureMoney) observed = NULL;
		g_autoptr(VentureMoney) reference = NULL;
		g_autofree gchar *observed_text = NULL;
		g_autofree gchar *reference_text = NULL;
		gint64 listing_id;
		gint64 venue_id;
		VentureAlertKind kind;

		hit = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(hit);
		g_object_get(hit, "listing-id", &listing_id, "venue-id", &venue_id, "kind", &kind,
		             "observed", &observed, "reference", &reference, NULL);
		g_assert_cmpint(listing_id, ==, undercut_id);
		g_assert_cmpint(venue_id, ==, realm_a);
		g_assert_cmpint(kind, ==, VENTURE_ALERT_KIND_UNDERCUT);
		observed_text = venture_money_to_string(observed);
		reference_text = venture_money_to_string(reference);
		g_assert_cmpstr(observed_text, ==, "1.20 USD");
		g_assert_cmpstr(reference_text, ==, "1.25 USD");
		g_assert_cmpint(venture_entity_get_organization_id(hit), ==, fixture->org);
	}
}

/*
 * A rule is quiet about the same instrument at the same venue for its
 * cooldown, and never speaks twice about one snapshot.
 *
 * What breaks if this regresses: every feed run re-sends the same snipe to
 * the inbox and the webhook while the price sits there, or -- with the
 * snapshot check gone -- a hand evaluation after a run says it all again.
 */
static void
test_alerts_cooldown(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureMoney) threshold = money_of("1.20 USD");
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) again = NULL;
	g_autoptr(JsonObject) later = NULL;
	g_autoptr(JsonObject) uncooled = NULL;

	(void)user_data;

	seed_store(fixture);
	rule = rule_on(fixture, VENTURE_ALERT_KIND_BELOW, "ore");
	g_object_set(rule, "threshold", threshold, "cooldown-minutes", (gint64)60, NULL);
	save(fixture, rule);

	first = evaluate(fixture, rule, TRUE);
	g_assert_cmpint(report_int(first, "written"), ==, 2);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(rule)), ==, 2);

	/* The same snapshots: nothing new to say. */
	again = evaluate(fixture, rule, TRUE);
	g_assert_cmpint(report_int(again, "written"), ==, 0);
	g_assert_cmpint(report_int(again, "cooled"), ==, 2);

	/* A newer snapshot at b half an hour on: inside the hour's quiet. */
	{
		g_autoptr(VentureSeriesStore) store = writer(fixture);
		static const Offer b3[] = { { "ore", 10, 105, 4 } };

		snapshot(store, "realm-b", "USD", fixture->now - 1800, b3, G_N_ELEMENTS(b3));
	}

	later = evaluate(fixture, rule, TRUE);
	g_assert_cmpint(report_int(later, "written"), ==, 0);
	g_assert_cmpint(report_int(later, "cooled"), ==, 2);

	/* No cooldown: every new snapshot speaks; the unchanged one still not. */
	g_object_set(rule, "cooldown-minutes", (gint64)0, NULL);
	save(fixture, rule);
	uncooled = evaluate(fixture, rule, TRUE);
	g_assert_cmpint(report_int(uncooled, "written"), ==, 1);
	g_assert_cmpint(report_int(uncooled, "cooled"), ==, 1);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(rule)), ==, 3);
}

/*
 * One evaluation writes at most VENTURE_ALERTS_MAX_HITS_PER_RUN hits and
 * says how many it left out; a category scope reaches instruments the
 * store files under it that nobody promoted.
 *
 * What breaks if this regresses: a loose rule on a big store writes a hit,
 * a notification and a webhook per instrument on its first sync, or the
 * overflow vanishes without a word.
 */
static void
test_alerts_cap(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GArray) offers = NULL;
	g_autoptr(GPtrArray) keys = NULL;
	g_autoptr(VentureEntity) category = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureMoney) threshold = money_of("10.00 USD");
	g_autoptr(JsonObject) report = NULL;
	guint total;
	guint i;

	(void)user_data;

	total = VENTURE_ALERTS_MAX_HITS_PER_RUN + 20;
	store = writer(fixture);
	add_venue(store, "bulk-realm", "eu", "USD", fixture->t0);
	offers = g_array_new(FALSE, TRUE, sizeof(Offer));
	keys = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < total; i++)
	{
		Offer offer;
		gchar *key = g_strdup_printf("bulk-%03u", i);

		g_ptr_array_add(keys, key);
		add_instrument(store, key, key, "Bulk/Goods", fixture->t0);
		offer.instrument = key;
		offer.id = i + 1;
		offer.price = 100;
		offer.quantity = 1;
		g_array_append_val(offers, offer);
	}

	snapshot(store, "bulk-realm", "USD", fixture->t2, (const Offer *)offers->data, offers->len);
	g_clear_object(&store);

	category = VENTURE_ENTITY(venture_category_new());
	venture_entity_set_organization_id(category, fixture->org);
	g_object_set(category, "name", "Bulk", "applies-to", "instrument", NULL);
	save(fixture, category);

	rule = new_rule(fixture, "everything cheap", VENTURE_ALERT_KIND_BELOW);
	g_object_set(rule, "category-id", ID(category), "threshold", threshold, NULL);
	save(fixture, rule);

	report = evaluate(fixture, rule, TRUE);
	g_assert_cmpint(report_int(report, "candidates"), ==, total);
	g_assert_cmpint(report_int(report, "written"), ==, VENTURE_ALERTS_MAX_HITS_PER_RUN);
	g_assert_cmpint(report_int(report, "over_cap"), ==, 20);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(report, "notes")), ==, 1);
	g_assert_nonnull(strstr(json_array_get_string_element(json_object_get_array_member(report, "notes"), 0),
	                        "20 hits not written"));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, NULL, 0), ==,
	                VENTURE_ALERTS_MAX_HITS_PER_RUN);
}

/*
 * More open listings than the old single read took, newest last: the one
 * just posted is undercut, and a thousand older ones at a price nobody
 * beats are ahead of it by id.
 *
 * What breaks if this regresses: the freeze reads oldest first with a
 * bound of a thousand again, and an operator with a big book is never
 * told about the listing a rival undercut a minute after it went up --
 * silently, because nothing said the bound was reached.
 */
static void
test_alerts_undercut_many_listings(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(GDateTime) listed = NULL;
	g_autoptr(JsonObject) report = NULL;
	g_autoptr(GError) error = NULL;
	gint64 realm_a;
	gint64 newest = 0;
	guint total;
	guint i;

	(void)user_data;

	seed_store(fixture);

	product = VENTURE_ENTITY(venture_product_new());
	venture_entity_set_organization_id(product, fixture->org);
	g_object_set(product, "name", "Copper Ore", NULL);
	save(fixture, product);
	instrument(fixture, "ore", ID(product));
	realm_a = venue(fixture, "realm-a");
	listed = g_date_time_new_now_utc();
	total = 1200;

	/* One transaction: twelve hundred saves, one commit. */
	g_assert_true(venture_database_begin(fixture->database, &error));
	g_assert_no_error(error);

	for (i = 0; i < total; i++)
	{
		g_autoptr(VentureEntity) listing = VENTURE_ENTITY(venture_listing_new());
		g_autoptr(VentureMoney) price = NULL;

		/* 1.10 is under realm-a's cheapest (1.20): nobody undercuts it.
		 * The last, at 1.25, is undercut. */
		price = money_of((i + 1 < total) ? "1.10 USD" : "1.25 USD");
		venture_entity_set_organization_id(listing, fixture->org);
		g_object_set(listing, "product-id", ID(product), "quantity", (gint64)1,
		             "unit-price", price, "listed-at", listed, "venue-id", realm_a,
		             "outcome", VENTURE_LISTING_OUTCOME_OPEN, NULL);
		save(fixture, listing);
		newest = ID(listing);
	}

	g_assert_true(venture_database_commit(fixture->database, &error));
	g_assert_no_error(error);

	rule = new_rule(fixture, "my listings", VENTURE_ALERT_KIND_UNDERCUT);
	save(fixture, rule);
	report = evaluate(fixture, rule, FALSE);
	g_assert_cmpint(report_int(report, "candidates"), ==, 1);

	{
		g_autofree gchar *subject = g_strdup_printf("realm-a|ore|listing:%" G_GINT64_FORMAT, newest);

		g_assert_true(report_has(report, subject));
	}

	/* Under the bound, nothing is said about one. */
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(report, "notes")), ==, 0);
}

/* --- The operator's accounts --------------------------------------------------- */

/*
 * The store an account rule reads, all relative to @now:
 *
 * - Drgold (character, Thorium Brotherhood, at thorium), last seen ten
 *   days ago, with 3 ore listed until exactly two hours on (p-inside),
 *   herbs until a second past that (p-outside), a mail with 12.50 lost
 *   in exactly five hours (m1) and one with 4 ore a second past that (m2);
 * - Mule (character, same realm), seen yesterday, with ore whose listing
 *   ran out a minute ago (p-expired) and a mail of 5.00 lost ten seconds
 *   ago (m4);
 * - Alt (character, Argent Dawn, at argent), never seen by the source,
 *   first heard of eight days ago, with a bar listed until an hour on
 *   (p-argent), one listed until exactly now (p-now) and a cash-on-delivery
 *   bill lost in an hour (m3);
 * - the warband bank (shared), seen thirty days ago.
 */
static void
seed_accounts(
	Fixture	*fixture,
	gint64	 now
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesAccountBatch batch;
	VentureSeriesAccountResult result;
	gint64 at;
	const VentureSeriesAccount accounts[] = {
		{ "Drgold-Thorium", "Drgold", "character", "Thorium Brotherhood", "thorium", NULL, now - 10 * 86400, NULL },
		{ "Mule-Thorium", "Mule", "character", "Thorium Brotherhood", "thorium", NULL, now - 86400, NULL },
		{ "Alt-Argent", "Alt", "character", "Argent Dawn", "argent", NULL, VENTURE_SERIES_NONE, NULL },
		{ "Warband", "Warband bank", "shared", NULL, NULL, NULL, now - 30 * 86400, NULL },
	};
	VentureSeriesPosition positions[] = {
		{ "p-inside", "Drgold-Thorium", "thorium", "ore", 3, 150, VENTURE_SERIES_NONE, now + 7200, VENTURE_SERIES_NONE, 0 },
		{ "p-outside", "Drgold-Thorium", "thorium", "herb", 1, 80, VENTURE_SERIES_NONE, now + 7201, VENTURE_SERIES_NONE, 0 },
		{ "p-expired", "Mule-Thorium", "thorium", "ore", 2, 140, VENTURE_SERIES_NONE, now - 60, VENTURE_SERIES_NONE, 0 },
		{ "p-argent", "Alt-Argent", "argent", "bar", 1, 500, VENTURE_SERIES_NONE, now + 3600, VENTURE_SERIES_NONE, 0 },
		{ "p-now", "Alt-Argent", "argent", "bar", 1, 500, VENTURE_SERIES_NONE, now, VENTURE_SERIES_NONE, 0 },
	};
	VentureSeriesInbound inbound[] = {
		{ "m1", "Drgold-Thorium", "Auction House", "Auction successful", 1250, VENTURE_SERIES_NONE, NULL, VENTURE_SERIES_NONE, now + 5 * 3600, FALSE, 0 },
		{ "m2", "Drgold-Thorium", "Auction House", "Auction expired", VENTURE_SERIES_NONE, VENTURE_SERIES_NONE, "ore", 4, now + 5 * 3600 + 1, TRUE, 0 },
		{ "m3", "Alt-Argent", "Trader", "Bill", VENTURE_SERIES_NONE, 100, NULL, VENTURE_SERIES_NONE, now + 3600, FALSE, 0 },
		{ "m4", "Mule-Thorium", "Auction House", "Auction successful", 500, VENTURE_SERIES_NONE, NULL, VENTURE_SERIES_NONE, now - 10, FALSE, 0 },
	};
	guint i;

	at = now - 8 * 86400;

	for (i = 0; i < G_N_ELEMENTS(positions); i++)
		positions[i].at = at;

	for (i = 0; i < G_N_ELEMENTS(inbound); i++)
		inbound[i].at = at;

	store = writer(fixture);
	add_instrument(store, "ore", "Copper Ore", "Materials/Ore", at);
	add_instrument(store, "herb", "Peacebloom", "Herbs", at);
	add_instrument(store, "bar", "Copper Bar", "Materials/Bars", at);

	memset(&batch, 0, sizeof(batch));
	batch.currency = "USD";
	batch.accounts = accounts;
	batch.n_accounts = G_N_ELEMENTS(accounts);
	batch.positions = positions;
	batch.n_positions = G_N_ELEMENTS(positions);
	batch.inbound = inbound;
	batch.n_inbound = G_N_ELEMENTS(inbound);
	g_assert_true(venture_series_store_apply_accounts(store, &batch, at, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result.positions, ==, G_N_ELEMENTS(positions));
	g_assert_cmpint(result.inbound, ==, G_N_ELEMENTS(inbound));
}

/*
 * position_expiring and inbound_expiring fire on what runs out after now
 * and at most the rule's hours on -- the boundary itself inside, a second
 * past it out, an expiry exactly now out -- narrowed by the account's
 * group or the position's venue, and by an instrument when scoped.
 *
 * What breaks if this regresses: the operator hears about a listing that
 * already lapsed (it is collect_ready's), never hears about the one that
 * lapses on the hour, or a rule narrowed to one realm reports every
 * realm's listings.
 */
static void
test_alerts_expiring(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	gint64 now;

	(void)user_data;

	now = fixture->now;
	seed_accounts(fixture, now);

	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "expiring", VENTURE_ALERT_KIND_POSITION_EXPIRING);
		g_autoptr(JsonObject) report = NULL;

		g_object_set(rule, "threshold-number", 2.0, NULL);
		save(fixture, rule);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 2);
		g_assert_true(report_has(report, "position:p-inside"));
		g_assert_true(report_has(report, "position:p-argent"));
		g_assert_cmpstr(report_says(report, "position:p-inside"), ==,
		                "Drgold (Thorium Brotherhood): 3 x Copper Ore expires in 2 h");
		g_assert_cmpstr(report_says(report, "position:p-argent"), ==,
		                "Alt (Argent Dawn): 1 x Copper Bar expires in 1 h");

		/* A group narrows to its accounts. */
		g_object_set(rule, "group-key", "Thorium Brotherhood", NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "position:p-inside"));

		/* A venue narrows to its positions. */
		g_object_set(rule, "group-key", NULL, "venue-id", venue(fixture, "argent"), NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "position:p-argent"));

		/* An instrument narrows to what sells it: ore at no venue but
		 * thorium, so nothing at argent. A category reaches the store's
		 * own filing. */
		g_object_set(rule, "venue-id", (gint64)0, "instrument-id", instrument(fixture, "ore", 0), NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "position:p-inside"));

		/* An hour and a half on, p-argent has lapsed and p-outside has
		 * come inside the window beside p-inside. */
		g_object_set(rule, "instrument-id", (gint64)0, NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate_at(fixture, rule, FALSE, now + 5400);
		g_assert_cmpint(report_int(report, "candidates"), ==, 2);
		g_assert_true(report_has(report, "position:p-inside"));
		g_assert_true(report_has(report, "position:p-outside"));
	}

	/* Mail: five hours reaches m1 (on the boundary) and m3, not m2 (a
	 * second past) nor m4 (lost already). */
	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "mail", VENTURE_ALERT_KIND_INBOUND_EXPIRING);
		g_autoptr(JsonObject) report = NULL;
		const gchar *message;

		g_object_set(rule, "threshold-number", 5.0, NULL);
		save(fixture, rule);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 2);
		g_assert_true(report_has(report, "inbound:m1"));
		g_assert_true(report_has(report, "inbound:m3"));
		message = report_says(report, "inbound:m1");
		g_assert_cmpstr(message, ==,
		                "Drgold (Thorium Brotherhood): mail from Auction House \"Auction successful\" "
		                "(12.50 USD) expires in 5 h");
		g_assert_nonnull(strstr(report_says(report, "inbound:m3"), "cash on delivery 1.00 USD"));

		/* Scoped to ore: only a mail carrying ore, m2, which needs the
		 * extra second. */
		g_object_set(rule, "instrument-id", instrument(fixture, "ore", 0), "threshold-number", 6.0, NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "inbound:m2"));
		g_assert_nonnull(strstr(report_says(report, "inbound:m2"), "returned mail"));
		g_assert_nonnull(strstr(report_says(report, "inbound:m2"), "4 x Copper Ore"));
	}
}

/*
 * account_stale fires on a character not seen for longer than the rule's
 * days (one never seen counts from when the store first heard of it),
 * never on a shared bank, and once per spell; collect_ready fires per
 * account with money or goods in its mail or positions already expired,
 * and not for a bill or a mail already lost.
 *
 * What breaks if this regresses: a warband bank nobody opens reads as an
 * abandoned character; a stale account nags every hour of its absence;
 * an expired listing waiting at the mailbox is never mentioned, or a
 * cash-on-delivery bill reads as money to collect.
 */
static void
test_alerts_accounts(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	gint64 now;

	(void)user_data;

	now = fixture->now;
	seed_accounts(fixture, now);

	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "stale", VENTURE_ALERT_KIND_ACCOUNT_STALE);
		g_autoptr(JsonObject) report = NULL;
		g_autoptr(JsonObject) later = NULL;
		g_autoptr(JsonObject) narrower = NULL;

		/* Seven days: Drgold (ten) and Alt (eight, from first heard of). */
		g_object_set(rule, "threshold-number", 7.0, NULL);
		save(fixture, rule);
		report = evaluate_at(fixture, rule, TRUE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 2);
		g_assert_cmpint(report_int(report, "written"), ==, 2);
		g_assert_true(report_has(report, "account:Drgold-Thorium"));
		g_assert_true(report_has(report, "account:Alt-Argent"));
		g_assert_true(g_str_has_prefix(report_says(report, "account:Drgold-Thorium"),
		                               "Drgold (Thorium Brotherhood) has not been seen for 10 days (since "));

		/* Two days on, the same spells: nothing new to say. */
		later = evaluate_at(fixture, rule, TRUE, now + 2 * 86400);
		g_assert_cmpint(report_int(later, "written"), ==, 0);
		g_assert_cmpint(report_int(later, "cooled"), ==, 2);

		/* Eight days exactly is not older than eight days. */
		g_object_set(rule, "threshold-number", 8.0, NULL);
		save(fixture, rule);
		narrower = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(narrower, "candidates"), ==, 1);
		g_assert_true(report_has(narrower, "account:Drgold-Thorium"));

		/* The hit names the account and where it is. */
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
			g_autoptr(VentureEntity) hit = NULL;
			g_autoptr(GDateTime) observed_at = NULL;
			g_autofree gchar *account_key = NULL;
			g_autofree gchar *venue_key = NULL;

			g_assert_true(venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ,
			                                              "account:Drgold-Thorium", NULL));
			hit = venture_database_find_one(fixture->database, query, NULL);
			g_assert_nonnull(hit);
			g_object_get(hit, "account-key", &account_key, "venue-key", &venue_key,
			             "observed-at", &observed_at, NULL);
			g_assert_cmpstr(account_key, ==, "Drgold-Thorium");
			g_assert_cmpstr(venue_key, ==, "thorium");
			g_assert_cmpint(g_date_time_to_unix(observed_at), ==, now - 10 * 86400);
		}
	}

	{
		g_autoptr(VentureEntity) rule = new_rule(fixture, "collect", VENTURE_ALERT_KIND_COLLECT_READY);
		g_autoptr(JsonObject) report = NULL;

		save(fixture, rule);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 3);
		g_assert_cmpstr(report_says(report, "collect:Drgold-Thorium"), ==,
		                "Drgold (Thorium Brotherhood): 2 mails to collect (12.50 USD, 4 items)");
		g_assert_cmpstr(report_says(report, "collect:Mule-Thorium"), ==,
		                "Mule (Thorium Brotherhood): 1 listing expired, awaiting login");
		g_assert_cmpstr(report_says(report, "collect:Alt-Argent"), ==,
		                "Alt (Argent Dawn): 1 listing expired, awaiting login");

		/* Narrowed to a group. */
		g_object_set(rule, "group-key", "Argent Dawn", NULL);
		save(fixture, rule);
		g_clear_pointer(&report, json_object_unref);
		report = evaluate_at(fixture, rule, FALSE, now);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, "collect:Alt-Argent"));
	}
}

/*
 * A position told about once per cooldown: the same moment again says
 * nothing, half an hour on is inside the hour's quiet (while one that
 * only now came into the window is told), an hour and a minute on it
 * speaks again -- and the one that has since lapsed does not.
 *
 * What breaks if this regresses: every feed run in the last two hours of
 * a listing's life sends the same warning, or the warning is never
 * repeated however long the cooldown has run.
 */
static void
test_alerts_expiring_cooldown(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) again = NULL;
	g_autoptr(JsonObject) half = NULL;
	g_autoptr(JsonObject) later = NULL;
	gint64 now;

	(void)user_data;

	now = fixture->now;
	seed_accounts(fixture, now);
	rule = new_rule(fixture, "expiring", VENTURE_ALERT_KIND_POSITION_EXPIRING);
	g_object_set(rule, "threshold-number", 2.0, NULL);
	save(fixture, rule);

	first = evaluate_at(fixture, rule, TRUE, now);
	g_assert_cmpint(report_int(first, "written"), ==, 2);

	again = evaluate_at(fixture, rule, TRUE, now);
	g_assert_cmpint(report_int(again, "written"), ==, 0);
	g_assert_cmpint(report_int(again, "cooled"), ==, 2);

	/* Half an hour on, the two told are inside the hour's quiet; p-outside
	 * has come into the window and is told for the first time. */
	half = evaluate_at(fixture, rule, TRUE, now + 1800);
	g_assert_cmpint(report_int(half, "candidates"), ==, 3);
	g_assert_cmpint(report_int(half, "written"), ==, 1);
	g_assert_cmpint(report_int(half, "cooled"), ==, 2);

	/* An hour and a minute on: p-argent lapsed at now + 3600 and is not
	 * a candidate; p-inside's hour is up and it speaks again; p-outside
	 * was told half an hour ago and stays quiet. */
	later = evaluate_at(fixture, rule, TRUE, now + 3660);
	g_assert_cmpint(report_int(later, "candidates"), ==, 2);
	g_assert_cmpint(report_int(later, "written"), ==, 1);
	g_assert_cmpint(report_int(later, "cooled"), ==, 1);
	g_assert_true(report_has(later, "position:p-inside"));
	g_assert_true(report_has(later, "position:p-outside"));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(rule)), ==, 4);

	/* The hit carries the account and the position's keys. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
		g_autoptr(VentureEntity) hit = NULL;
		g_autoptr(VentureMoney) observed = NULL;
		g_autofree gchar *observed_text = NULL;
		g_autofree gchar *account_key = NULL;
		g_autofree gchar *venue_key = NULL;
		g_autofree gchar *instrument_key = NULL;
		VentureAlertKind kind;

		g_assert_true(venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ,
		                                              "position:p-inside", NULL));
		hit = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(hit);
		g_object_get(hit, "account-key", &account_key, "venue-key", &venue_key,
		             "instrument-key", &instrument_key, "kind", &kind, "observed", &observed, NULL);
		g_assert_cmpint(kind, ==, VENTURE_ALERT_KIND_POSITION_EXPIRING);
		g_assert_cmpstr(account_key, ==, "Drgold-Thorium");
		g_assert_cmpstr(venue_key, ==, "thorium");
		g_assert_cmpstr(instrument_key, ==, "ore");
		observed_text = venture_money_to_string(observed);
		g_assert_cmpstr(observed_text, ==, "1.50 USD");
	}
}

/* --- Delivery ------------------------------------------------------------------- */

typedef struct
{
	guint		 received;
	gchar		*last_event;
	GPtrArray	*types;		/* what on_created told the tally */
} Delivery;

static void
endpoint_handler(
	SoupServer		*server,
	SoupServerMessage	*message,
	const gchar		*path,
	GHashTable		*query,
	gpointer		 user_data
){
	Delivery *delivery = user_data;

	(void)server;
	(void)path;
	(void)query;

	delivery->received++;
	g_free(delivery->last_event);
	delivery->last_event = g_strdup(soup_message_headers_get_one(
		soup_server_message_get_request_headers(message), "X-Venture-Event"));
	soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
	soup_server_message_set_response(message, "text/plain", SOUP_MEMORY_COPY, "ok", 2);
}

static gboolean
tally_handler(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	Delivery *delivery = user_data;
	const gchar *argument;

	(void)context;
	(void)name;
	(void)error;

	argument = venture_automation_argument(params, 0);
	g_ptr_array_add(delivery->types, g_strdup((NULL != argument) ? argument : ""));

	if (NULL != result)
		*result = g_variant_ref_sink(venture_automation_result_new(1, "counted", NULL));

	return TRUE;
}

static VentureAutomation *
start_rules(
	Fixture		*fixture,
	const gchar	*rules
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pods_path = NULL;
	VentureAutomation *automation;

	pods_path = g_build_filename(fixture->state_dir, "automations.pod", NULL);
	g_assert_true(g_file_set_contents(pods_path, rules, -1, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	venture_context_set_automation(fixture->context, automation);
	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	return automation;
}

static void
stop_rules(
	Fixture			*fixture,
	VentureAutomation	*automation
){
	venture_automation_stop(automation);
	venture_context_set_automation(fixture->context, NULL);
	g_object_unref(automation);
}

/*
 * A hit is one record, and its creation is the whole event: the inbox of
 * the rule's recipient (named, else its creator), a webhook delivery of
 * alert_hit.created, and an automation on_created -- with no separate
 * alert event to keep in step.
 *
 * What breaks if this regresses: the hit is written but nobody hears --
 * a personal owner silencing the webhook, the system actor's create
 * filtered out like its updates, the inbox addressed to nobody.
 */
static void
test_alerts_delivery(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(SoupServer) endpoint = NULL;
	g_autoptr(GSList) uris = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureMoney) threshold = money_of("1.15 USD");
	g_autoptr(JsonObject) report = NULL;
	g_autofree gchar *url = NULL;
	VentureAutomation *automation;
	Delivery delivery = { 0, NULL, NULL };
	VentureActor actor;
	gint64 trader;
	gint64 waited;
	gboolean heard;
	guint i;

	(void)user_data;

	seed_store(fixture);
	delivery.types = g_ptr_array_new_with_free_func(g_free);
	trader = user(fixture, "trader");

	/* The far end, on a port the kernel chose. */
	endpoint = soup_server_new(NULL, NULL);
	soup_server_add_handler(endpoint, "/", endpoint_handler, &delivery, NULL);
	g_assert_true(soup_server_listen_local(endpoint, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(endpoint);
	url = g_uri_to_string(uris->data);
	g_slist_free_full(g_steal_pointer(&uris), (GDestroyNotify)g_uri_unref);

	webhook = VENTURE_ENTITY(venture_webhook_new());
	venture_entity_set_organization_id(webhook, fixture->org);
	g_object_set(webhook, "name", "Alerts", "url", url, "events", "alert_hit.created",
	             "active", TRUE, NULL);
	save(fixture, webhook);

	g_assert_true(venture_automation_handler_registry_add(
		venture_context_get_automation_handlers(fixture->context), "tally", "Counts",
		tally_handler, &delivery, NULL, &error));
	g_assert_no_error(error);
	automation = start_rules(fixture,
		"pod watcher = venture->new();\n"
		"watcher->on_created => venture->tally(\"{event->type}\");\n");

	/* Created by the trader, naming nobody to tell: the trader is told. */
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "trader";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	rule = rule_on(fixture, VENTURE_ALERT_KIND_BELOW, "ore");
	g_object_set(rule, "name", "cheap ore", "threshold", threshold, NULL);
	g_assert_true(venture_database_save(fixture->database, rule, &actor, &error));
	g_assert_no_error(error);

	report = evaluate(fixture, rule, TRUE);
	g_assert_cmpint(report_int(report, "written"), ==, 1);

	/* The inbox. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_NOTIFICATION);
		g_autoptr(VentureEntity) note = NULL;
		g_autofree gchar *title = NULL;
		g_autofree gchar *target = NULL;
		VentureNotificationKind kind;

		g_assert_true(venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, trader, NULL));
		note = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(note);
		g_object_get(note, "kind", &kind, "title", &title, "target-type", &target, NULL);
		g_assert_cmpint(kind, ==, VENTURE_NOTIFICATION_KIND_ALERT);
		g_assert_cmpstr(target, ==, "alert_hit");
		g_assert_true(g_str_has_prefix(title, "cheap ore: "));
		g_assert_nonnull(strstr(title, "realm-b"));
	}

	/* The automation heard the hit's creation. */
	heard = FALSE;

	for (i = 0; i < delivery.types->len; i++)
		heard = heard || (0 == g_strcmp0(g_ptr_array_index(delivery.types, i), "alert_hit"));

	g_assert_true(heard);

	/* And the webhook, once it lands. */
	for (waited = 0; (waited < 5000) && (0 == delivery.received); waited += 10)
	{
		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(10 * 1000);
	}

	while (g_main_context_iteration(NULL, FALSE))
		;

	g_assert_cmpuint(delivery.received, ==, 1);
	g_assert_cmpstr(delivery.last_event, ==, "alert_hit.created");

	/* A named recipient wins over the creator. */
	{
		g_autoptr(JsonObject) second = NULL;
		gint64 other = user(fixture, "partner");

		g_object_set(rule, "notify-username", "partner", "cooldown-minutes", (gint64)0, NULL);
		save(fixture, rule);
		{
			g_autoptr(VentureSeriesStore) store = writer(fixture);
			static const Offer b3[] = { { "ore", 10, 105, 4 } };

			snapshot(store, "realm-b", "USD", fixture->now - 1800, b3, G_N_ELEMENTS(b3));
		}
		second = evaluate(fixture, rule, TRUE);
		g_assert_cmpint(report_int(second, "written"), ==, 1);
		g_assert_cmpint(count_of(fixture, VENTURE_TYPE_NOTIFICATION, "user-id", other), ==, 1);

		/* Its webhook lands here too, not in the next test's loop. */
		for (waited = 0; (waited < 5000) && (delivery.received < 2); waited += 10)
		{
			if (!g_main_context_iteration(NULL, FALSE))
				g_usleep(10 * 1000);
		}

		g_assert_cmpuint(delivery.received, ==, 2);

		/* A membership revoked since the rule was saved stops the
		 * delivery; the hit is still written. */
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
			g_autoptr(VentureEntity) membership = NULL;
			g_autoptr(JsonObject) third = NULL;
			g_autoptr(VentureSeriesStore) store = writer(fixture);
			static const Offer b4[] = { { "ore", 10, 104, 4 } };

			g_assert_true(venture_query_add_filter_int(query, "user-id", VENTURE_FILTER_OP_EQ, other, NULL));
			membership = venture_database_find_one(fixture->database, query, NULL);
			g_assert_nonnull(membership);
			g_object_set(membership, "active", FALSE, NULL);
			save(fixture, membership);

			snapshot(store, "realm-b", "USD", fixture->now - 900, b4, G_N_ELEMENTS(b4));
			third = evaluate(fixture, rule, TRUE);
			g_assert_cmpint(report_int(third, "written"), ==, 1);
			g_assert_cmpint(count_of(fixture, VENTURE_TYPE_NOTIFICATION, "user-id", other), ==, 1);

			for (waited = 0; (waited < 5000) && (delivery.received < 3); waited += 10)
			{
				if (!g_main_context_iteration(NULL, FALSE))
					g_usleep(10 * 1000);
			}
		}
	}

	stop_rules(fixture, automation);
	soup_server_disconnect(endpoint);

	g_ptr_array_unref(delivery.types);
	g_free(delivery.last_event);
}

typedef struct
{
	VentureEntity	*rule;
	guint		 calls;
	GError		*error;
} Probe;

static gboolean
probe_handler(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	Probe *probe = user_data;

	(void)name;
	(void)params;
	(void)result;
	(void)error;

	probe->calls++;
	g_clear_error(&probe->error);
	g_assert_false(venture_marketdata_alerts_evaluate(context, probe->rule, TRUE, NULL,
	                                                  &probe->error));

	return TRUE;
}

/*
 * A rule is never evaluated from inside an automation handler.
 *
 * What breaks if this regresses: a hit written there raises no on_created
 * (the cascade guard swallows it), so a pod that evaluates rules would
 * write hits no other rule ever hears about -- and one written on every
 * record change would evaluate the whole store per save.
 */
static void
test_alerts_not_inside_automation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureMoney) threshold = money_of("1.20 USD");
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(GError) error = NULL;
	VentureAutomation *automation;
	Probe probe = { NULL, 0, NULL };

	(void)user_data;

	seed_store(fixture);
	rule = rule_on(fixture, VENTURE_ALERT_KIND_BELOW, "ore");
	g_object_set(rule, "threshold", threshold, NULL);
	save(fixture, rule);
	probe.rule = rule;

	g_assert_true(venture_automation_handler_registry_add(
		venture_context_get_automation_handlers(fixture->context), "probe", "Evaluates",
		probe_handler, &probe, NULL, &error));
	g_assert_no_error(error);
	automation = start_rules(fixture,
		"pod watcher = venture->new();\n"
		"watcher->on_created => venture->probe(\"{event->type}\");\n");

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "anything", NULL);
	save(fixture, ticket);

	g_assert_cmpuint(probe.calls, >=, 1);
	g_assert_error(probe.error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, NULL, 0), ==, 0);
	g_assert_false(venture_automation_is_dispatching(automation));

	stop_rules(fixture, automation);
	g_clear_error(&probe.error);

	/* Outside, the same rule writes. */
	{
		g_autoptr(JsonObject) report = evaluate(fixture, rule, TRUE);

		g_assert_cmpint(report_int(report, "written"), ==, 2);
	}
}

/* --- The worker ------------------------------------------------------------------ */

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

static VentureEntity *
sync_and_wait(Fixture *fixture)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_feeds_service_sync(venture_context_get_feeds_service(fixture->context),
	                                         fixture->source_id,
	                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
	g_assert_no_error(error);
	settle(fixture);

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);

	return venture_database_find_one(fixture->database, query, NULL);
}

/*
 * A real feed run: the rules frozen with the source on the main thread,
 * candidates found by the worker after the commit, hits written back on
 * the main thread as the system -- a price under the line and a news entry
 * that names the item. A run of the same snapshot again says nothing new.
 *
 * What breaks if this regresses: the worker reads the database (a context
 * failure, not a race), alerts are only ever found by hand, or a re-sync
 * repeats every hit.
 */
static void
test_alerts_worker_path(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) price_rule = NULL;
	g_autoptr(VentureEntity) news_rule = NULL;
	g_autoptr(VentureEntity) idle_rule = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureMoney) threshold = money_of("1.30 USD");
	g_autofree gchar *path = NULL;
	gint64 ore;

	(void)user_data;

	path = write_root_file(fixture, "realm.jsonl",
		"{\"type\":\"venue\",\"key\":\"argent\",\"name\":\"Argent Dawn\",\"currency\":\"USD\",\"group\":\"eu\"}\n"
		"{\"type\":\"instrument\",\"key\":\"ore\",\"name\":\"Iron ore\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"taken_at\":\"2026-10-03T12:00:00Z\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"1.25\",\"quantity\":20,\"id\":\"a1\"}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"1.50\",\"quantity\":5,\"id\":\"a2\"}\n"
		"{\"type\":\"entry\",\"key\":\"n1\",\"title\":\"Iron ore nerfed in the next patch\",\"url\":\"https://example.com/n1\"}\n");

	ore = instrument(fixture, "ore", 0);
	price_rule = new_rule(fixture, "cheap ore", VENTURE_ALERT_KIND_BELOW);
	g_object_set(price_rule, "instrument-id", ore, "threshold", threshold, NULL);
	save(fixture, price_rule);
	news_rule = new_rule(fixture, "nerfs", VENTURE_ALERT_KIND_ENTRY_MATCH);
	g_object_set(news_rule, "pattern", "ORE NERF", NULL);
	save(fixture, news_rule);

	/* Switched off: frozen without it. */
	idle_rule = new_rule(fixture, "off", VENTURE_ALERT_KIND_ABOVE);
	g_object_set(idle_rule, "instrument-id", ore, "threshold", threshold, "enabled", FALSE, NULL);
	save(fixture, idle_rule);

	run = sync_and_wait(fixture);
	g_assert_nonnull(run);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(price_rule)), ==, 1);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(news_rule)), ==, 1);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(idle_rule)), ==, 0);

	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
		g_autoptr(VentureEntity) hit = NULL;
		g_autoptr(GDateTime) observed_at = NULL;
		g_autofree gchar *venue_key = NULL;
		gint64 instrument_id;
		gint64 source_id;

		g_assert_true(venture_query_add_filter_int(query, "rule-id", VENTURE_FILTER_OP_EQ,
		                                           ID(price_rule), NULL));
		hit = venture_database_find_one(fixture->database, query, NULL);
		g_object_get(hit, "instrument-id", &instrument_id, "data-source-id", &source_id,
		             "venue-key", &venue_key, "observed-at", &observed_at, NULL);
		g_assert_cmpint(instrument_id, ==, ore);
		g_assert_cmpint(source_id, ==, fixture->source_id);
		g_assert_cmpstr(venue_key, ==, "argent");
		g_assert_cmpint(g_date_time_get_year(observed_at), ==, 2026);
	}

	/* The same file again: the same snapshot, nothing new to say. */
	again = sync_and_wait(fixture);
	g_assert_cmpint(ID(again), >, ID(run));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, NULL, 0), ==, 2);
}

static gchar *
iso(gint64 unix_time)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(unix_time);

	return g_date_time_format(when, "%Y-%m-%dT%H:%M:%SZ");
}

/* Pushes @text to push source @source_id and waits, bounded, for its run;
 * the hooks' main-thread halves run inside the wait. */
static VentureEntity *
push_and_wait(
	Fixture		*fixture,
	gint64		 source_id,
	const gchar	*text
){
	VentureFeedsService *service;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *push_id = NULL;
	VentureDataSourceRunStatus status;
	VentureEntity *run;
	gint64 run_id = 0;

	service = venture_context_get_feeds_service(fixture->context);
	body = g_bytes_new(text, strlen(text));
	g_assert_true(venture_feeds_service_push(service, source_id, body, &push_id, &error));
	g_assert_no_error(error);
	g_assert_true(venture_feeds_service_wait_push(service, push_id, 60, &run_id));
	g_assert_cmpint(run_id, >, 0);
	settle(fixture);

	run = reread(fixture, VENTURE_TYPE_DATA_SOURCE_RUN, run_id);
	g_object_get(run, "status", &status, NULL);
	g_assert_cmpint(status, !=, VENTURE_DATA_SOURCE_RUN_STATUS_FAILED);

	return run;
}

/*
 * The account kinds on the real path: a push of the operator's accounts
 * through the feeds worker, the rules frozen with the source, candidates
 * found after the commit at the run's time, hits written on the main
 * thread. The mirror then makes the position a listing, and a later
 * evaluation links the hit to it; an undercut rule finds that listing
 * from its mirror-state alone, with the instrument's product link gone.
 * The same push again is inside the cooldown.
 *
 * What breaks if this regresses: account alerts only ever fire by hand;
 * a push repeats every warning; a mirrored listing is never compared
 * unless a product query happens to find it.
 */
static void
test_alerts_accounts_worker_path(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) venture = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) expiring = NULL;
	g_autoptr(VentureEntity) collect = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(GString) body = NULL;
	g_autofree gchar *settings = NULL;
	g_autofree gchar *taken = NULL;
	g_autofree gchar *expires = NULL;
	g_autofree gchar *mail_expires = NULL;
	gint64 listing_id = 0;

	(void)user_data;

	venture = VENTURE_ENTITY(venture_venture_new());
	venture_entity_set_organization_id(venture, fixture->org);
	g_object_set(venture, "name", "Gold", NULL);
	save(fixture, venture);

	settings = g_strdup_printf("create_products: true\nproducts_venture_id: %" G_GINT64_FORMAT "\n",
	                           ID(venture));
	source = VENTURE_ENTITY(venture_data_source_new());
	venture_entity_set_organization_id(source, fixture->org);
	g_object_set(source, "name", "Characters", "provider", "push", "settings", settings,
	             "schedule", "manual", "currency", "USD", "instrument-namespace", "wow-item",
	             "venue-namespace", "wow-realm", NULL);
	save(fixture, source);

	expiring = new_rule(fixture, "expiring", VENTURE_ALERT_KIND_POSITION_EXPIRING);
	g_object_set(expiring, "threshold-number", 2.0, "data-source-id", ID(source), NULL);
	save(fixture, expiring);
	collect = new_rule(fixture, "collect", VENTURE_ALERT_KIND_COLLECT_READY);
	save(fixture, collect);

	taken = iso(fixture->now - 60);
	expires = iso(fixture->now + 3600);
	mail_expires = iso(fixture->now + 86400);
	body = g_string_new(NULL);
	g_string_append_printf(body,
		"{\"type\":\"instrument\",\"key\":\"2770\",\"name\":\"Copper Ore\",\"kind\":\"item\"}\n"
		"{\"type\":\"venue\",\"key\":\"thorium\",\"name\":\"Thorium Brotherhood\",\"kind\":\"auction_house\",\"group\":\"us\",\"currency\":\"USD\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"thorium\",\"taken_at\":\"%s\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"thorium\",\"instrument\":\"2770\",\"price\":\"1.20\",\"quantity\":5,\"id\":\"x1\"}\n"
		"{\"type\":\"listing\",\"venue\":\"thorium\",\"instrument\":\"2770\",\"price\":\"1.50\",\"quantity\":3,\"id\":\"x2\"}\n"
		"{\"type\":\"account\",\"key\":\"Drgold-Thorium\",\"name\":\"Drgold\",\"kind\":\"character\",\"group\":\"Thorium Brotherhood\",\"venue\":\"thorium\"}\n"
		"{\"type\":\"position\",\"account\":\"Drgold-Thorium\",\"venue\":\"thorium\",\"id\":\"p1\",\"instrument\":\"2770\",\"quantity\":3,\"price\":\"1.50\",\"expires_at\":\"%s\"}\n"
		"{\"type\":\"inbound\",\"account\":\"Drgold-Thorium\",\"id\":\"m1\",\"sender\":\"Auction House\",\"money\":\"12.50\",\"expires_at\":\"%s\"}\n",
		taken, expires, mail_expires);

	run = push_and_wait(fixture, ID(source), body->str);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(expiring)), ==, 1);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(collect)), ==, 1);

	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
		g_autoptr(VentureEntity) hit = NULL;
		g_autofree gchar *account_key = NULL;
		g_autofree gchar *subject = NULL;
		g_autofree gchar *message = NULL;
		gint64 source_id;

		g_assert_true(venture_query_add_filter_int(query, "rule-id", VENTURE_FILTER_OP_EQ,
		                                           ID(collect), NULL));
		hit = venture_database_find_one(fixture->database, query, NULL);
		g_object_get(hit, "account-key", &account_key, "subject", &subject, "message", &message,
		             "data-source-id", &source_id, NULL);
		g_assert_cmpstr(account_key, ==, "Drgold-Thorium");
		g_assert_cmpstr(subject, ==, "collect:Drgold-Thorium");
		g_assert_cmpstr(message, ==, "Drgold (Thorium Brotherhood): 1 mail to collect (12.50 USD)");
		g_assert_cmpint(source_id, ==, ID(source));
	}

	/* The mirror made the position a listing; a hit written now names it. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LISTING);
		g_autoptr(VentureEntity) listing = NULL;
		g_autoptr(VentureEntity) rule = NULL;
		g_autoptr(VentureEntity) hit = NULL;
		g_autoptr(VentureQuery) hits = NULL;
		g_autoptr(JsonObject) report = NULL;
		g_autofree gchar *external_id = NULL;
		gint64 hit_listing;

		external_id = g_strdup_printf("%s:p1", venture_entity_get_uuid(source));
		g_assert_true(venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ,
		                                              external_id, NULL));
		listing = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(listing);
		listing_id = ID(listing);

		rule = new_rule(fixture, "expiring again", VENTURE_ALERT_KIND_POSITION_EXPIRING);
		g_object_set(rule, "threshold-number", 2.0, NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, TRUE);
		g_assert_cmpint(report_int(report, "written"), ==, 1);

		hits = venture_query_new(VENTURE_TYPE_ALERT_HIT);
		g_assert_true(venture_query_add_filter_int(hits, "rule-id", VENTURE_FILTER_OP_EQ, ID(rule), NULL));
		hit = venture_database_find_one(fixture->database, hits, NULL);
		g_object_get(hit, "listing-id", &hit_listing, NULL);
		g_assert_cmpint(hit_listing, ==, listing_id);
	}

	/* Undercut from the mirror-state: the instrument no longer names the
	 * product, so only the listing's own word says what it sells. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
		g_autoptr(VentureEntity) instrument_record = NULL;
		g_autoptr(VentureEntity) rule = NULL;
		g_autoptr(JsonObject) report = NULL;
		g_autofree gchar *subject = NULL;

		g_assert_true(venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, "2770", NULL));
		instrument_record = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(instrument_record);
		g_object_set(instrument_record, "product-id", (gint64)0, NULL);
		save(fixture, instrument_record);

		rule = new_rule(fixture, "undercut", VENTURE_ALERT_KIND_UNDERCUT);
		g_object_set(rule, "data-source-id", ID(source), NULL);
		save(fixture, rule);
		report = evaluate(fixture, rule, FALSE);
		subject = g_strdup_printf("thorium|2770|listing:%" G_GINT64_FORMAT, listing_id);
		g_assert_cmpint(report_int(report, "candidates"), ==, 1);
		g_assert_true(report_has(report, subject));
	}

	/* The same push again: inside the hour's quiet. */
	again = push_and_wait(fixture, ID(source), body->str);
	g_assert_cmpint(ID(again), >, ID(run));
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(expiring)), ==, 1);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, "rule-id", ID(collect)), ==, 1);
}

/*
 * With feeds off there is no store, and an evaluation says so; with
 * marketdata off the types are gone and a feed run evaluates nothing.
 *
 * What breaks if this regresses: a rule evaluated against nothing reads
 * as "nothing fired", or a module switched off keeps writing hits its
 * pages can no longer show.
 */
static void
test_alerts_modules_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureMoney) threshold = money_of("1.30 USD");
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	(void)user_data;

	rule = rule_on(fixture, VENTURE_ALERT_KIND_BELOW, "ore");
	g_object_set(rule, "threshold", threshold, NULL);
	save(fixture, rule);

	g_object_set(fixture->config, "feeds-enabled", FALSE, NULL);
	g_assert_false(venture_marketdata_alerts_evaluate(fixture->context, rule, FALSE, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);
	g_object_set(fixture->config, "feeds-enabled", TRUE, NULL);

	path = write_root_file(fixture, "realm.jsonl",
		"{\"type\":\"venue\",\"key\":\"argent\",\"name\":\"Argent Dawn\",\"currency\":\"USD\",\"group\":\"eu\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"taken_at\":\"2026-10-03T12:00:00Z\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"1.25\",\"quantity\":20,\"id\":\"a1\"}\n");

	venture_config_set_module_enabled(fixture->config, "marketdata", FALSE);
	g_assert_false(venture_marketdata_alerts_evaluate(fixture->context, rule, FALSE, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_cmpint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "alert_rule"),
	                ==, G_TYPE_INVALID);

	run = sync_and_wait(fixture);
	g_assert_nonnull(run);

	venture_config_set_module_enabled(fixture->config, "marketdata", TRUE);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_ALERT_HIT, NULL, 0), ==, 0);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/alerts/rule-validation", test_alerts_rule_validation);
	ADD("/alerts/kinds", test_alerts_kinds);
	ADD("/alerts/undercut", test_alerts_undercut);
	ADD("/alerts/undercut-many-listings", test_alerts_undercut_many_listings);
	ADD("/alerts/expiring", test_alerts_expiring);
	ADD("/alerts/expiring-cooldown", test_alerts_expiring_cooldown);
	ADD("/alerts/accounts", test_alerts_accounts);
	ADD("/alerts/cooldown", test_alerts_cooldown);
	ADD("/alerts/cap", test_alerts_cap);
	ADD("/alerts/delivery", test_alerts_delivery);
	ADD("/alerts/not-inside-automation", test_alerts_not_inside_automation);
	ADD("/alerts/worker-path", test_alerts_worker_path);
	ADD("/alerts/accounts-worker-path", test_alerts_accounts_worker_path);
	ADD("/alerts/modules-off", test_alerts_modules_off);

#undef ADD

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

#endif
