/*
 * test-marketdata.c - Venues, instruments, promotion and the price oracle
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The oracle reads the series store a data source keeps. These tests seed
 * that store directly through its writer -- the feeds worker is what fills
 * it in production, and test-feeds covers that half -- and then ask the
 * oracle, comparing every figure with what the store itself says, so a
 * basis wired to the wrong column fails here. Each fixture has its own
 * state directory, removed with venture_test_remove_tree().
 */

#include <venture.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "venture-test-util.h"

/* A build without SQLite has no series store to read. */
#ifdef VENTURE_HAVE_SQLITE

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

typedef struct
{
	gchar			*state_dir;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	gint64			 org;
	gint64			 source_id;
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

static gint64
product(
	Fixture		*fixture,
	const gchar	*name
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_product_new());
	g_object_set(record, "name", name, NULL);
	venture_entity_set_organization_id(record, fixture->org);
	save(fixture, record);

	return ID(record);
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

/* --- The store -------------------------------------------------------------- */

static void
add_venue(
	VentureSeriesStore	*store,
	const gchar		*key,
	const gchar		*kind,
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
	venue.kind = kind;
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
	const gchar		*kind,
	const gchar		*category,
	const gchar		*parent,
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
	instrument.kind = kind;
	instrument.category = category;
	instrument.parent_key = parent;
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

/* One complete snapshot of a venue: every offer it lists at @taken_at. */
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
	g_assert_nonnull(snap);

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
 * The store every test reads. Two EU realms priced in USD and one US realm
 * priced in EUR; copper ore seen three times at realm-a (one listing sold
 * between the first two), and once elsewhere; a bar, a herb, and an event
 * with an outcome for promotion.
 */
static void
seed_store(Fixture *fixture)
{
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	gint64 now;
	static const Offer a0[] = {
		{ "2770", 1, 125, 10 }, { "2770", 2, 140, 5 }, { "2447", 3, 75, 12 }
	};
	static const Offer a1[] = {
		{ "2770", 1, 125, 10 }, { "2770", 4, 130, 3 }, { "2447", 3, 75, 12 }
	};
	static const Offer a2[] = {
		{ "2770", 4, 130, 3 }, { "2770", 5, 120, 2 }, { "2447", 3, 75, 12 },
		{ "2840", 6, 500, 1 }
	};
	static const Offer b2[] = { { "2770", 10, 110, 4 }, { "2447", 11, 80, 1 } };
	static const Offer c2[] = { { "2770", 20, 100, 7 } };

	source = reread(fixture, VENTURE_TYPE_DATA_SOURCE, fixture->source_id);
	dir = venture_feeds_store_dir(fixture->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(dir, &error);
	g_assert_no_error(error);

	now = g_get_real_time() / G_USEC_PER_SEC;
	fixture->t0 = now - 2 * 86400;
	fixture->t1 = now - 86400;
	fixture->t2 = now - 3600;

	add_venue(store, "realm-a", "Auction House", "eu", "USD", fixture->t0);
	add_venue(store, "realm-b", "auction_house", "eu", "USD", fixture->t0);
	add_venue(store, "realm-c", "auction-house", "us", "EUR", fixture->t0);
	add_instrument(store, "2770", "Copper Ore", "item", "Materials/Ore", NULL, fixture->t0);
	add_instrument(store, "2447", "Peacebloom", "item", NULL, NULL, fixture->t0);
	add_instrument(store, "2840", "Copper Bar", "item", NULL, NULL, fixture->t0);
	add_instrument(store, "ev1", "Final", "event", NULL, NULL, fixture->t0);
	add_instrument(store, "ev1-home", "Home to win", "Outcome", NULL, "ev1", fixture->t0);

	snapshot(store, "realm-a", "USD", fixture->t0, a0, G_N_ELEMENTS(a0));
	snapshot(store, "realm-a", "USD", fixture->t1, a1, G_N_ELEMENTS(a1));
	snapshot(store, "realm-a", "USD", fixture->t2, a2, G_N_ELEMENTS(a2));
	snapshot(store, "realm-b", "USD", fixture->t2, b2, G_N_ELEMENTS(b2));
	snapshot(store, "realm-c", "EUR", fixture->t2, c2, G_N_ELEMENTS(c2));

	g_assert_true(venture_series_store_recompute_region(store, NULL, now, VENTURE_SERIES_NONE,
	                                                    NULL, NULL, &error));
	g_assert_no_error(error);
}

/* A read handle on the fixture's store, for comparing with. */
static VentureSeriesStore *
reader(Fixture *fixture)
{
	g_autoptr(GError) error = NULL;
	VentureSeriesStore *store;

	store = venture_feeds_service_open_reader(venture_context_get_feeds_service(fixture->context),
	                                          fixture->source_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(store);

	return store;
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

	fixture->state_dir = g_dir_make_tmp("venture-marketdata-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, "feeds-enabled", TRUE, NULL);

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
	             "settings", "file: x.jsonl", "schedule", "manual", "currency", "USD",
	             "instrument-namespace", "wow-item", "venue-namespace", "realm", NULL);
	save(fixture, source);
	fixture->source_id = ID(source);
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

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* Promotes an instrument and, when @product_id is set, files it under it. */
static gint64
promoted(
	Fixture		*fixture,
	const gchar	*key,
	gint64		 product_id
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) instrument = NULL;

	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org,
	                                                    fixture->source_id, key, NULL,
	                                                    &instrument, &error));
	g_assert_no_error(error);
	g_assert_nonnull(instrument);

	if (product_id > 0)
	{
		g_object_set(instrument, "product-id", product_id, NULL);
		save(fixture, instrument);
	}

	return ID(instrument);
}

/* Asks for a price; the evidence is handed back when @out_evidence is set. */
static VentureMoney *
ask(
	Fixture				 *fixture,
	const VentureMarketdataQuestion	 *question,
	VentureMarketdataEvidence	**out_evidence
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMarketdataEvidence) evidence = NULL;
	VentureMoney *price = NULL;

	g_assert_true(venture_marketdata_reference_price(fixture->context, question, &price,
	                                                 &evidence, &error));
	g_assert_no_error(error);
	g_assert_nonnull(evidence);

	if (NULL != out_evidence)
		*out_evidence = g_steal_pointer(&evidence);

	return price;
}

static void
assert_money(
	VentureMoney	*money,
	const gchar	*expected
){
	g_autofree gchar *text = NULL;

	g_assert_nonnull(money);
	text = venture_money_to_string(money);
	g_assert_cmpstr(text, ==, expected);
}

/* The store's minor units as money text in @currency, for comparing. */
static gchar *
minor_text(
	gint64		 amount,
	const gchar	*currency
){
	g_autoptr(VentureMoney) money = NULL;

	g_assert_cmpint(amount, !=, VENTURE_SERIES_NONE);
	money = venture_money_new_for_currency(amount, currency);

	return venture_money_to_string(money);
}

/* --- Records ---------------------------------------------------------------- */

/*
 * The module is on by default and needs market; its references are derived
 * and unique, deleted rows included; an instrument tree has no loop; a
 * watchlist names an instrument once, in one currency.
 *
 * What breaks if this regresses: two records for one store row (and two
 * answers for one product), a promotion that fails on a deleted row's
 * index forever, an event whose outcomes are its parents, or a watchlist
 * with two targets for one instrument.
 */
static void
test_marketdata_records(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureInstrument) first = NULL;
	g_autoptr(VentureInstrument) second = NULL;
	g_autoptr(VentureInstrument) child = NULL;
	g_autoptr(VentureEntity) parent = NULL;
	g_autoptr(VentureVenue) venue = NULL;
	g_autoptr(VentureWatchlist) list = NULL;
	g_autoptr(VentureWatchlistEntry) entry = NULL;
	g_autoptr(VentureWatchlistEntry) again = NULL;
	g_autoptr(VentureMoney) buy = NULL;
	g_autoptr(VentureMoney) sell = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	VentureModule *module;

	(void)user_data;

	module = venture_module_registry_lookup(venture_context_get_modules(fixture->context),
	                                        "marketdata");
	g_assert_nonnull(module);
	g_assert_true(venture_module_is_enabled(module));
	g_assert_true(g_strv_contains(venture_module_get_requires(module), "market"));
	g_assert_true(g_strv_contains(venture_module_get_suggests(module), "feeds"));

	/* The reference is derived, whatever was typed into it. */
	first = venture_instrument_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(first), fixture->org);
	g_object_set(first, "name", "Copper Ore", "namespace", "wow-item", "key", "2770",
	             "external-ref", "typed", NULL);
	save(fixture, first);
	g_object_get(first, "external-ref", &ref, NULL);
	g_assert_cmpstr(ref, ==, "wow-item:2770");
	g_assert_cmpint(venture_entity_get_organization_id(VENTURE_ENTITY(first)), ==, fixture->org);

	/* A second record for the same row is refused, and so is one beside a
	 * deleted record: the unique index counts it. */
	second = venture_instrument_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(second), fixture->org);
	g_object_set(second, "name", "Ore again", "namespace", "wow-item", "key", "2770", NULL);
	save_refused(fixture, second, "already has the reference wow-item:2770");
	g_assert_true(venture_database_delete(fixture->database, VENTURE_ENTITY(first), NULL, &error));
	g_assert_no_error(error);
	save_refused(fixture, second, "restore it instead");

	/* A namespace may not carry the separator. */
	g_object_set(second, "namespace", "wow:item", NULL);
	save_refused(fixture, second, "colon");

	/* A record with no key has no reference to keep unique. */
	g_object_set(second, "namespace", NULL, "key", NULL, NULL);
	save(fixture, second);
	g_clear_pointer(&ref, g_free);
	g_object_get(second, "external-ref", &ref, NULL);
	g_assert_null(ref);

	/* No loop: a child cannot become its parent's parent. */
	child = venture_instrument_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(child), fixture->org);
	g_object_set(child, "name", "Home to win", "parent-id", ID(second), NULL);
	save(fixture, child);
	parent = reread(fixture, VENTURE_TYPE_INSTRUMENT, ID(second));
	g_object_set(parent, "parent-id", ID(child), NULL);
	save_refused(fixture, parent, "");
	g_object_set(child, "parent-id", ID(child), NULL);
	save_refused(fixture, child, "own parent");

	/* A venue's currency is a code, kept in capitals. */
	venue = venture_venue_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(venue), fixture->org);
	g_object_set(venue, "name", "Argent Dawn", "key", "ad", "currency", "nope!", NULL);
	save_refused(fixture, venue, "not a currency code");
	g_object_set(venue, "currency", "usd", "transfer-hours", (gint64)-1, NULL);
	save_refused(fixture, venue, "Transfer hours");
	g_object_set(venue, "transfer-hours", (gint64)2, NULL);
	save(fixture, venue);
	{
		g_autofree gchar *currency = NULL;

		g_object_get(venue, "currency", &currency, NULL);
		g_assert_cmpstr(currency, ==, "USD");
	}

	/* A watchlist names an instrument once, with targets in one currency. */
	list = venture_watchlist_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(list), fixture->org);
	g_object_set(list, "name", "Herbs", "group-key", "eu", NULL);
	save(fixture, list);

	buy = money_of("1.00 USD");
	sell = money_of("2.00 EUR");
	entry = venture_watchlist_entry_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), fixture->org);
	g_object_set(entry, "watchlist-id", ID(list), "instrument-id", ID(child),
	             "target-buy", buy, "target-sell", sell, NULL);
	save_refused(fixture, entry, "share one currency");
	g_clear_pointer(&sell, venture_money_free);
	sell = money_of("2.00 USD");
	g_object_set(entry, "target-sell", sell, NULL);
	save(fixture, entry);

	again = venture_watchlist_entry_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(again), fixture->org);
	g_object_set(again, "watchlist-id", ID(list), "instrument-id", ID(child), NULL);
	save_refused(fixture, again, "already on this watchlist");
	g_object_set(again, "instrument-id", (gint64)0, NULL);
	save_refused(fixture, again, "Instrument");
}

/*
 * A listing names the venue it is listed at, a venue of its own
 * organization, and only while marketdata is on.
 *
 * What breaks if this regresses: the undercut alert has no venue to compare
 * a listing with, or a listing points at another organization's venue.
 */
static void
test_marketdata_listing_venue(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureVenue) venue = NULL;
	g_autoptr(VentureVenue) theirs = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureListing) listing = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(GDateTime) listed = NULL;
	g_autoptr(VentureEntity) back = NULL;
	gint64 venue_id;

	(void)user_data;

	venue = venture_venue_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(venue), fixture->org);
	g_object_set(venue, "name", "Realm A", "kind", VENTURE_VENUE_KIND_AUCTION_HOUSE, NULL);
	save(fixture, venue);

	organization = VENTURE_ENTITY(venture_organization_new());
	g_object_set(organization, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, organization);
	theirs = venture_venue_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(theirs), ID(organization));
	g_object_set(theirs, "name", "Their realm", NULL);
	save(fixture, theirs);

	price = money_of("1.25 USD");
	listed = venture_time_from_string("2026-03-01", NULL);
	listing = venture_listing_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(listing), fixture->org);
	g_object_set(listing, "product-id", product(fixture, "Ore"), "quantity", (gint64)5,
	             "unit-price", price, "listed-at", listed, "venue-id", ID(theirs), NULL);
	save_refused(fixture, listing, "another organization");
	g_object_set(listing, "venue-id", ID(venue), NULL);
	save(fixture, listing);

	back = reread(fixture, VENTURE_TYPE_LISTING, ID(listing));
	g_object_get(back, "venue-id", &venue_id, NULL);
	g_assert_cmpint(venue_id, ==, ID(venue));

	/* An observation filed under a series: name could never be asked for. */
	{
		g_autoptr(VentureEntity) observation = NULL;
		g_autoptr(GDateTime) when = venture_time_now();

		observation = VENTURE_ENTITY(venture_price_observation_new());
		venture_entity_set_organization_id(observation, fixture->org);
		g_object_set(observation, "product-id", product(fixture, "Herb"), "source", "series:min",
		             "price", price, "observed-at", when, NULL);
		save_refused(fixture, observation, "cannot start with");
	}

	/* Marketdata off: the reference is refused when written, the listing
	 * still saves without it. */
	venture_config_set_module_enabled(fixture->config, "marketdata", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "marketdata"));
	g_clear_object(&listing);
	listing = venture_listing_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(listing), fixture->org);
	g_object_set(listing, "product-id", product(fixture, "Bar"), "quantity", (gint64)1,
	             "unit-price", price, "listed-at", listed, "venue-id", ID(venue), NULL);
	save_refused(fixture, listing, "marketdata");
	g_object_set(listing, "venue-id", (gint64)0, NULL);
	save(fixture, listing);
}

/* --- Promotion -------------------------------------------------------------- */

/*
 * Promotion makes a record from the store's row once: again returns the
 * same record, a deleted one comes back, a parent is promoted first, a
 * kind is read from the store's free text and a category from its path.
 *
 * What breaks if this regresses: every visit to an instrument page makes a
 * duplicate, a deleted instrument blocks its key forever, or outcomes lose
 * their event.
 */
static void
test_marketdata_promotion(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) ore = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(VentureEntity) restored = NULL;
	g_autoptr(VentureEntity) outcome = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) venue = NULL;
	g_autoptr(VentureEntity) venue_again = NULL;
	g_autoptr(VentureEntity) materials = NULL;
	g_autoptr(VentureEntity) category = NULL;
	g_autoptr(GPtrArray) known = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *group = NULL;
	g_autofree gchar *currency = NULL;
	VentureInstrumentKind kind;
	VentureVenueKind venue_kind;
	gint64 parent_id;
	gint64 source_id;
	gint64 category_id;

	(void)user_data;

	/* Nothing stored yet: said, not guessed. */
	g_assert_false(venture_marketdata_promote_instrument(fixture->context, fixture->org,
	                                                     fixture->source_id, "2770", NULL,
	                                                     &ore, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_nonnull(strstr(error->message, "stored nothing yet"));
	g_clear_error(&error);

	seed_store(fixture);

	/* A category path matches an existing tree; categories are never made. */
	materials = VENTURE_ENTITY(venture_category_new());
	venture_entity_set_organization_id(materials, fixture->org);
	g_object_set(materials, "name", "Materials", NULL);
	save(fixture, materials);
	category = VENTURE_ENTITY(venture_category_new());
	venture_entity_set_organization_id(category, fixture->org);
	g_object_set(category, "name", "Ore", "parent-id", ID(materials), NULL);
	save(fixture, category);

	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org,
	                                                    fixture->source_id, "2770", NULL,
	                                                    &ore, &error));
	g_assert_no_error(error);
	g_object_get(ore, "name", &name, "kind", &kind, "external-ref", &ref,
	             "data-source-id", &source_id, "category-id", &category_id, NULL);
	g_assert_cmpstr(name, ==, "Copper Ore");
	g_assert_cmpint(kind, ==, VENTURE_INSTRUMENT_KIND_ITEM);
	g_assert_cmpstr(ref, ==, "wow-item:2770");
	g_assert_cmpint(source_id, ==, fixture->source_id);
	g_assert_cmpint(category_id, ==, ID(category));

	/* Idempotent: the same record, with what was edited on it kept. */
	g_object_set(ore, "name", "Copper ore (mine)", NULL);
	save(fixture, ore);
	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org,
	                                                    fixture->source_id, "2770", NULL,
	                                                    &again, &error));
	g_assert_no_error(error);
	g_assert_cmpint(ID(again), ==, ID(ore));
	g_clear_pointer(&name, g_free);
	g_object_get(again, "name", &name, NULL);
	g_assert_cmpstr(name, ==, "Copper ore (mine)");

	/* Deleted: restored, not duplicated. */
	g_assert_true(venture_database_delete(fixture->database, again, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org,
	                                                    fixture->source_id, "2770", NULL,
	                                                    &restored, &error));
	g_assert_no_error(error);
	g_assert_cmpint(ID(restored), ==, ID(ore));
	g_assert_false(venture_entity_is_deleted(restored));

	/* The parent first. */
	g_assert_true(venture_marketdata_promote_instrument(fixture->context, fixture->org,
	                                                    fixture->source_id, "ev1-home", NULL,
	                                                    &outcome, &error));
	g_assert_no_error(error);
	g_object_get(outcome, "parent-id", &parent_id, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INSTRUMENT_KIND_OUTCOME);
	g_assert_cmpint(parent_id, >, 0);
	event = reread(fixture, VENTURE_TYPE_INSTRUMENT, parent_id);
	g_object_get(event, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INSTRUMENT_KIND_EVENT);

	/* Unknown keys and other organizations' sources are not found. */
	g_assert_false(venture_marketdata_promote_instrument(fixture->context, fixture->org,
	                                                     fixture->source_id, "nope", NULL,
	                                                     NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(venture_marketdata_promote_instrument(fixture->context, fixture->org + 999,
	                                                     fixture->source_id, "2770", NULL,
	                                                     NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A venue: kind from "Auction House", group and currency from the row. */
	g_assert_true(venture_marketdata_promote_venue(fixture->context, fixture->org,
	                                               fixture->source_id, "realm-a", NULL,
	                                               &venue, &error));
	g_assert_no_error(error);
	g_object_get(venue, "kind", &venue_kind, "group-key", &group, "currency", &currency, NULL);
	g_assert_cmpint(venue_kind, ==, VENTURE_VENUE_KIND_AUCTION_HOUSE);
	g_assert_cmpstr(group, ==, "eu");
	g_assert_cmpstr(currency, ==, "USD");
	g_assert_true(venture_marketdata_promote_venue(fixture->context, fixture->org,
	                                               fixture->source_id, "realm-a", NULL,
	                                               &venue_again, &error));
	g_assert_cmpint(ID(venue_again), ==, ID(venue));

	/* Promoted instruments are what track: known keeps. */
	source = reread(fixture, VENTURE_TYPE_DATA_SOURCE, fixture->source_id);
	known = venture_marketdata_known_keys(fixture->context, source);
	g_assert_true(g_ptr_array_find_with_equal_func(known, "2770", g_str_equal, NULL));
	g_assert_true(g_ptr_array_find_with_equal_func(known, "ev1", g_str_equal, NULL));
	g_assert_false(g_ptr_array_find_with_equal_func(known, "2447", g_str_equal, NULL));
}

/*
 * With marketdata on, a source that tracks only known instruments needs no
 * list in its settings: its instrument records are the list.
 *
 * What breaks if this regresses: a source made to track promoted
 * instruments cannot be saved before any instrument can name it.
 */
static void
test_marketdata_track_known(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDataSource) source = NULL;

	(void)user_data;

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Known only", "provider", "file_jsonl",
	             "settings", "file: y.jsonl", "schedule", "manual",
	             "track", VENTURE_DATA_SOURCE_TRACK_KNOWN, NULL);
	save(fixture, source);

	/* Off, the list is all there is, and an empty one keeps nothing. */
	venture_config_set_module_enabled(fixture->config, "marketdata", FALSE);
	g_clear_object(&source);
	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Known, nothing listed", "provider", "file_jsonl",
	             "settings", "file: z.jsonl", "schedule", "manual",
	             "track", VENTURE_DATA_SOURCE_TRACK_KNOWN, NULL);
	save_refused(fixture, source, "must list the instruments");
}

/* --- The oracle ------------------------------------------------------------- */

/*
 * Every price basis against the store's own figures, at a venue, across a
 * group, and from history.
 *
 * What breaks if this regresses: a basis reads the wrong column, a group's
 * cheapest is taken across currencies, or "as of yesterday" answers with
 * today's price.
 */
static void
test_marketdata_oracle_bases(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(VentureSeriesRow) current = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketdataQuestion question;
	VentureSeriesReference reference;
	VentureSeriesRegion region;
	gint64 ore;
	guint i;

	(void)user_data;

	seed_store(fixture);
	ore = promoted(fixture, "2770", 0);
	store = reader(fixture);

	venture_marketdata_question_init(&question);
	question.organization_id = fixture->org;
	question.instrument_id = ore;

	/* min at a venue: the cheapest unit now. */
	{
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureMarketdataEvidence) evidence = NULL;

		question.basis = VENTURE_MARKETDATA_BASIS_MIN;
		question.where = "realm-a";
		price = ask(fixture, &question, &evidence);
		assert_money(price, "1.20 USD");
		g_assert_cmpstr(evidence->origin, ==, "series");
		g_assert_cmpstr(evidence->venue_key, ==, "realm-a");
		g_assert_cmpint(evidence->data_source_id, ==, fixture->source_id);
		g_assert_cmpint(evidence->instrument_id, ==, ore);
		g_assert_cmpint(evidence->taken_at, ==, fixture->t2);
	}

	/* min across a group: the cheapest venue in it. */
	{
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureMarketdataEvidence) evidence = NULL;

		question.where = "eu";
		price = ask(fixture, &question, &evidence);
		assert_money(price, "1.10 USD");
		g_assert_cmpstr(evidence->venue_key, ==, "realm-b");
		g_assert_cmpstr(evidence->group_key, ==, "eu");
	}

	/* min everywhere: one currency at a time -- the cheapest venue's,
	 * unless a preferred one is offered somewhere. */
	{
		g_autoptr(VentureMoney) any = NULL;
		g_autoptr(VentureMoney) preferred = NULL;

		question.where = NULL;
		any = ask(fixture, &question, NULL);
		assert_money(any, "1.00 EUR");
		question.prefer_currency = "USD";
		preferred = ask(fixture, &question, NULL);
		assert_money(preferred, "1.10 USD");
		question.prefer_currency = NULL;
	}

	/* market at a venue: the store's market value for it. */
	g_assert_true(venture_series_store_get_current(store, "realm-a", "2770", &current, &error));
	g_assert_no_error(error);
	g_assert_nonnull(current);
	{
		g_autoptr(VentureMoney) price = NULL;
		g_autofree gchar *expected = minor_text(current->market_value, "USD");

		question.basis = VENTURE_MARKETDATA_BASIS_MARKET;
		question.where = "realm-a";
		price = ask(fixture, &question, NULL);
		assert_money(price, expected);
	}

	/* History: as of shortly after the second snapshot, its minimum. */
	{
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureMoney) nothing = NULL;
		g_autoptr(GDateTime) then = g_date_time_new_from_unix_utc(fixture->t1 + 600);
		g_autoptr(GDateTime) long_ago = g_date_time_new_from_unix_utc(fixture->t0 - 90 * 86400);

		question.basis = VENTURE_MARKETDATA_BASIS_MIN;
		question.at = then;
		price = ask(fixture, &question, NULL);
		assert_money(price, "1.25 USD");

		question.at = long_ago;
		nothing = ask(fixture, &question, NULL);
		g_assert_null(nothing);
		question.at = NULL;
	}

	/* The daily-history bases, at a venue and for the group. */
	memset(&reference, 0, sizeof(reference));
	g_assert_true(venture_series_store_reference(store, "realm-a", NULL, "2770",
	                                             g_get_real_time() / G_USEC_PER_SEC,
	                                             &reference, &error));
	g_assert_no_error(error);
	{
		static const VentureMarketdataBasis bases[] = {
			VENTURE_MARKETDATA_BASIS_MARKET_14D, VENTURE_MARKETDATA_BASIS_HISTORICAL_60D,
			VENTURE_MARKETDATA_BASIS_SALE_AVG
		};

		for (i = 0; i < G_N_ELEMENTS(bases); i++)
		{
			g_autoptr(VentureMoney) price = NULL;
			g_autofree gchar *expected = NULL;
			gint64 value;

			value = (VENTURE_MARKETDATA_BASIS_MARKET_14D == bases[i]) ? reference.market_14d
			      : (VENTURE_MARKETDATA_BASIS_HISTORICAL_60D == bases[i]) ? reference.historical_60d
			      : reference.sale_avg;
			question.basis = bases[i];
			question.where = "realm-a";
			price = ask(fixture, &question, NULL);

			if (VENTURE_SERIES_NONE == value)
				g_assert_null(price);
			else
			{
				expected = minor_text(value, reference.currency);
				assert_money(price, expected);
			}
		}

		/* A sale was estimated between the first two snapshots. */
		g_assert_cmpint(reference.sale_avg, !=, VENTURE_SERIES_NONE);
	}

	/* The region bases, read where the store's recompute left them. */
	memset(&region, 0, sizeof(region));
	g_assert_true(venture_series_store_get_region(store, "eu", "2770", NULL, &region, &error));
	g_assert_no_error(error);
	g_assert_true(region.found);
	{
		struct
		{
			VentureMarketdataBasis	 basis;
			gint64			 value;
		} cases[] = {
			{ VENTURE_MARKETDATA_BASIS_REGION_MEDIAN, 0 },
			{ VENTURE_MARKETDATA_BASIS_REGION_P33, 0 },
			{ VENTURE_MARKETDATA_BASIS_REGION_MARKET_AVG, 0 },
			{ VENTURE_MARKETDATA_BASIS_MARKET, 0 }
		};

		cases[0].value = region.median_min;
		cases[1].value = region.p33;
		cases[2].value = region.market_avg;
		cases[3].value = region.market_avg;

		for (i = 0; i < G_N_ELEMENTS(cases); i++)
		{
			g_autoptr(VentureMoney) price = NULL;
			g_autoptr(VentureMarketdataEvidence) evidence = NULL;
			g_autofree gchar *expected = minor_text(cases[i].value, region.currency);

			question.basis = cases[i].basis;
			question.where = "eu";
			price = ask(fixture, &question, &evidence);
			assert_money(price, expected);
			g_assert_cmpstr(evidence->group_key, ==, "eu");
		}

		/* A venue's region is its group's. */
		{
			g_autoptr(VentureMoney) price = NULL;
			g_autofree gchar *expected = minor_text(region.median_min, region.currency);

			question.basis = VENTURE_MARKETDATA_BASIS_REGION_MEDIAN;
			question.where = "realm-b";
			price = ask(fixture, &question, NULL);
			assert_money(price, expected);
		}
	}

	/* No group named, and the store has two: refused, naming them. */
	{
		g_autoptr(VentureMoney) price = NULL;

		question.basis = VENTURE_MARKETDATA_BASIS_REGION_MEDIAN;
		question.where = NULL;
		g_assert_false(venture_marketdata_reference_price(fixture->context, &question, &price,
		                                                  NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_assert_nonnull(strstr(error->message, "2 groups"));
		g_clear_error(&error);
	}

	/* A price basis asked as a number, and the reverse, are refused. */
	{
		gdouble value;

		question.basis = VENTURE_MARKETDATA_BASIS_MIN;
		question.where = "realm-a";
		g_assert_false(venture_marketdata_reference_number(fixture->context, &question, &value,
		                                                   NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
		g_clear_error(&error);
	}
}

/*
 * The number bases: sale rate and units sold a day from the daily history,
 * units on offer now at a venue and across a group.
 *
 * What breaks if this regresses: a sale rate read as a price, or a
 * group's quantity counting a venue twice or not at all.
 */
static void
test_marketdata_oracle_numbers(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketdataQuestion question;
	VentureSeriesReference reference;
	gdouble value;

	(void)user_data;

	seed_store(fixture);
	store = reader(fixture);

	venture_marketdata_question_init(&question);
	question.organization_id = fixture->org;
	question.instrument_id = promoted(fixture, "2770", 0);

	question.basis = VENTURE_MARKETDATA_BASIS_QUANTITY;
	question.where = "realm-a";
	g_assert_true(venture_marketdata_reference_number(fixture->context, &question, &value,
	                                                  NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpfloat(value, ==, 5.0);

	/* realm-a's 5 and realm-b's 4. */
	question.where = "eu";
	g_assert_true(venture_marketdata_reference_number(fixture->context, &question, &value,
	                                                  NULL, &error));
	g_assert_cmpfloat(value, ==, 9.0);

	memset(&reference, 0, sizeof(reference));
	g_assert_true(venture_series_store_reference(store, "realm-a", NULL, "2770",
	                                             g_get_real_time() / G_USEC_PER_SEC,
	                                             &reference, &error));
	g_assert_no_error(error);

	question.where = "realm-a";
	question.basis = VENTURE_MARKETDATA_BASIS_SALE_RATE;
	g_assert_true(venture_marketdata_reference_number(fixture->context, &question, &value,
	                                                  NULL, &error));
	g_assert_true((isnan(value) && isnan(reference.sale_rate)) ||
	              (value == reference.sale_rate));

	question.basis = VENTURE_MARKETDATA_BASIS_SOLD_PER_DAY;
	g_assert_true(venture_marketdata_reference_number(fixture->context, &question, &value,
	                                                  NULL, &error));
	g_assert_true((isnan(value) && isnan(reference.sold_per_day)) ||
	              (value == reference.sold_per_day));
	g_assert_false(isnan(value));
	g_assert_cmpfloat(value, >, 0.0);

	/* A number basis is refused as a price. */
	{
		g_autoptr(VentureMoney) price = NULL;

		g_assert_false(venture_marketdata_reference_price(fixture->context, &question, &price,
		                                                  NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}
}

/*
 * Never a conversion: a figure in another currency than the one asked for
 * is no answer, and the evidence names both.
 *
 * What breaks if this regresses: a margin in dollars computed from euros,
 * the number being right only by accident.
 */
static void
test_marketdata_oracle_currency(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureMoney) wrong = NULL;
	g_autoptr(VentureMoney) right = NULL;
	g_autoptr(VentureMarketdataEvidence) evidence = NULL;
	VentureMarketdataQuestion question;

	(void)user_data;

	seed_store(fixture);

	venture_marketdata_question_init(&question);
	question.organization_id = fixture->org;
	question.instrument_id = promoted(fixture, "2770", 0);
	question.basis = VENTURE_MARKETDATA_BASIS_MIN;
	question.where = "realm-c";
	question.currency = "USD";
	wrong = ask(fixture, &question, &evidence);
	g_assert_null(wrong);
	g_assert_null(evidence->origin);
	g_assert_nonnull(strstr(evidence->note, "EUR"));
	g_assert_nonnull(strstr(evidence->note, "not USD"));

	question.currency = "eur";
	right = ask(fixture, &question, NULL);
	assert_money(right, "1.00 EUR");
}

/*
 * A product is asked through the instruments that name it; with nothing
 * from the stores, the newest observation answers only when allowed.
 *
 * What breaks if this regresses: a valuation that quietly mixes market
 * data and hand-typed prices, or a product with an instrument priced as if
 * it had none.
 */
static void
test_marketdata_oracle_product_and_fallback(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) observation = NULL;
	g_autoptr(VentureMoney) observed = NULL;
	g_autoptr(GDateTime) when = NULL;
	VentureMarketdataQuestion question;
	gint64 ore;
	gint64 herb;

	(void)user_data;

	seed_store(fixture);
	ore = product(fixture, "Copper Ore");
	herb = product(fixture, "Silverleaf");
	promoted(fixture, "2770", ore);

	venture_marketdata_question_init(&question);
	question.organization_id = fixture->org;
	question.basis = VENTURE_MARKETDATA_BASIS_MIN;
	question.where = "realm-a";

	{
		g_autoptr(VentureMoney) price = NULL;

		question.product_id = ore;
		price = ask(fixture, &question, NULL);
		assert_money(price, "1.20 USD");
	}

	/* A product no instrument names: nothing, then the observation. */
	observed = money_of("3.00 USD");
	when = venture_time_now();
	observation = VENTURE_ENTITY(venture_price_observation_new());
	venture_entity_set_organization_id(observation, fixture->org);
	g_object_set(observation, "product-id", herb, "source", "vendor", "price", observed,
	             "observed-at", when, NULL);
	save(fixture, observation);

	{
		g_autoptr(VentureMoney) none = NULL;
		g_autoptr(VentureMoney) fallback = NULL;
		g_autoptr(VentureMoney) other_source = NULL;
		g_autoptr(VentureMarketdataEvidence) evidence = NULL;

		question.product_id = herb;
		none = ask(fixture, &question, &evidence);
		g_assert_null(none);
		g_assert_nonnull(strstr(evidence->note, "no instrument names"));
		g_clear_pointer(&evidence, venture_marketdata_evidence_free);

		question.allow_fallback = TRUE;
		fallback = ask(fixture, &question, &evidence);
		assert_money(fallback, "3.00 USD");
		g_assert_cmpstr(evidence->origin, ==, "observation");
		g_assert_cmpint(evidence->observation_id, ==, ID(observation));
		g_assert_cmpstr(evidence->observation_source, ==, "vendor");

		/* The fallback's source is matched exactly. */
		question.fallback_source = "vendor (region)";
		other_source = ask(fixture, &question, NULL);
		g_assert_null(other_source);
	}
}

/*
 * The grammar is decided before any lookup: series: with a price basis
 * and an optional place, and nothing else.
 *
 * What breaks if this regresses: "series:min" matched as an observation
 * source that cannot exist, or a typo answered with every product
 * unpriced.
 */
static void
test_marketdata_price_source_grammar(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *where = NULL;
	VentureMarketdataBasis basis;
	gboolean series;

	g_assert_true(venture_marketdata_parse_price_source("market value", &series, &basis,
	                                                    &where, &error));
	g_assert_false(series);
	g_assert_null(where);
	g_assert_true(venture_marketdata_parse_price_source(NULL, &series, NULL, NULL, &error));
	g_assert_false(series);

	g_assert_true(venture_marketdata_parse_price_source("series:min", &series, &basis,
	                                                    &where, &error));
	g_assert_true(series);
	g_assert_cmpint(basis, ==, VENTURE_MARKETDATA_BASIS_MIN);
	g_assert_null(where);

	g_assert_true(venture_marketdata_parse_price_source("series:region_p33@eu", &series,
	                                                    &basis, &where, &error));
	g_assert_cmpint(basis, ==, VENTURE_MARKETDATA_BASIS_REGION_P33);
	g_assert_cmpstr(where, ==, "eu");
	g_clear_pointer(&where, g_free);

	g_assert_false(venture_marketdata_parse_price_source("series:", &series, &basis, &where,
	                                                     &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_false(venture_marketdata_parse_price_source("series:cheapest", &series, &basis,
	                                                     &where, &error));
	g_assert_nonnull(strstr(error->message, "historical_60d"));
	g_clear_error(&error);
	g_assert_false(venture_marketdata_parse_price_source("series:min@", &series, &basis,
	                                                     &where, &error));
	g_clear_error(&error);
	g_assert_false(venture_marketdata_parse_price_source("series:quantity@eu", &series, &basis,
	                                                     &where, &error));
	g_assert_nonnull(strstr(error->message, "not a price"));
	g_assert_null(where);
}

/* --- Reports ---------------------------------------------------------------- */

static VentureReportResult *
run_report(
	Fixture		 *fixture,
	const gchar	 *name,
	JsonObject	 *options,
	GError		**error
){
	VentureReport *report;

	report = venture_report_registry_lookup(venture_context_get_report_registry(fixture->context),
	                                        name);
	g_assert_nonnull(report);

	return venture_report_generate(report, fixture->context, NULL, options, error);
}

static gint
row_with(
	VentureReportResult	*result,
	const gchar		*column,
	const gchar		*text
){
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		const GValue *value = venture_report_result_get_cell(result, i, column);

		if ((NULL != value) && G_VALUE_HOLDS_STRING(value) &&
		    (0 == g_strcmp0(g_value_get_string(value), text)))
			return (gint)i;
	}

	return -1;
}

static gchar *
cell_money(
	VentureReportResult	*result,
	gint			 row,
	const gchar		*column
){
	const GValue *value;

	value = venture_report_result_get_cell(result, (guint)row, column);

	if ((NULL == value) || !G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY) ||
	    (NULL == g_value_get_boxed(value)))
		return NULL;

	return venture_money_to_string(g_value_get_boxed(value));
}

static const gchar *
cell_text(
	VentureReportResult	*result,
	gint			 row,
	const gchar		*column
){
	const GValue *value;

	value = venture_report_result_get_cell(result, (guint)row, column);

	return ((NULL != value) && G_VALUE_HOLDS_STRING(value)) ? g_value_get_string(value) : NULL;
}

/* A recipe making one bar from two ore and one flux. */
static gint64
smelting(
	Fixture	*fixture,
	gint64	 ore,
	gint64	 bar,
	gint64	 flux
){
	g_autoptr(VentureEntity) recipe = NULL;
	gint64 inputs[2];
	gint64 quantities[2] = { 2, 1 };
	guint i;

	recipe = VENTURE_ENTITY(venture_recipe_new());
	venture_entity_set_organization_id(recipe, fixture->org);
	g_object_set(recipe, "name", "Smelt copper", "output-product-id", bar,
	             "output-quantity", (gint64)1, "active", TRUE, NULL);
	save(fixture, recipe);

	inputs[0] = ore;
	inputs[1] = flux;

	for (i = 0; i < G_N_ELEMENTS(inputs); i++)
	{
		g_autoptr(VentureEntity) component = NULL;

		if (inputs[i] <= 0)
			continue;

		component = VENTURE_ENTITY(venture_recipe_component_new());
		venture_entity_set_organization_id(component, fixture->org);
		g_object_set(component, "recipe-id", ID(recipe), "product-id", inputs[i],
		             "quantity", quantities[i], NULL);
		save(fixture, component);
	}

	return ID(recipe);
}

/*
 * recipe_margin prices from feeds through series:, and never falls back:
 * a component the feeds have not priced is named and its cost blank even
 * when an observation of it exists.
 *
 * What breaks if this regresses: a margin computed from market data for
 * some inputs and hand-typed prices for others, with nothing saying so.
 */
static void
test_marketdata_recipe_margin(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) partial = NULL;
	g_autoptr(VentureEntity) observation = NULL;
	g_autoptr(VentureMoney) observed = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cost = NULL;
	g_autofree gchar *value = NULL;
	g_autofree gchar *profit = NULL;
	gint64 ore;
	gint64 bar;
	gint64 flux;
	gint row;

	(void)user_data;

	seed_store(fixture);
	ore = product(fixture, "Copper Ore");
	bar = product(fixture, "Copper Bar");
	promoted(fixture, "2770", ore);
	promoted(fixture, "2840", bar);
	smelting(fixture, ore, bar, 0);

	options = json_object_new();
	json_object_set_string_member(options, "price_source", "series:min@realm-a");
	result = run_report(fixture, "recipe_margin", options, &error);
	g_assert_no_error(error);
	row = row_with(result, "recipe", "Smelt copper");
	g_assert_cmpint(row, >=, 0);
	cost = cell_money(result, row, "cost");
	value = cell_money(result, row, "value");
	profit = cell_money(result, row, "profit");
	g_assert_cmpstr(cost, ==, "2.40 USD");
	g_assert_cmpstr(value, ==, "5.00 USD");
	g_assert_cmpstr(profit, ==, "2.60 USD");
	g_assert_cmpstr(cell_text(result, row, "priced_by"), ==, "series:min@realm-a");

	/* A component with an observation and no instrument: not priced. */
	flux = product(fixture, "Flux");
	{
		g_autoptr(VentureEntity) component = NULL;
		g_autoptr(GPtrArray) recipes = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_RECIPE);

		recipes = venture_database_find(fixture->database, query, NULL);
		component = VENTURE_ENTITY(venture_recipe_component_new());
		venture_entity_set_organization_id(component, fixture->org);
		g_object_set(component, "recipe-id", ID(g_ptr_array_index(recipes, 0)),
		             "product-id", flux, "quantity", (gint64)1, NULL);
		save(fixture, component);
	}
	observed = money_of("0.10 USD");
	when = venture_time_now();
	observation = VENTURE_ENTITY(venture_price_observation_new());
	venture_entity_set_organization_id(observation, fixture->org);
	g_object_set(observation, "product-id", flux, "source", "vendor", "price", observed,
	             "observed-at", when, NULL);
	save(fixture, observation);

	partial = run_report(fixture, "recipe_margin", options, &error);
	g_assert_no_error(error);
	row = row_with(partial, "recipe", "Smelt copper");
	g_assert_null(cell_money(partial, row, "cost"));
	g_assert_nonnull(strstr(cell_text(partial, row, "note"), "no price for Flux"));

	/* The grammar is checked before anything runs. */
	{
		g_autoptr(VentureReportResult) refused = NULL;

		json_object_set_string_member(options, "price_source", "series:nonsense");
		refused = run_report(fixture, "recipe_margin", options, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}
}

/*
 * goal_materials prices its shopping list from feeds through series:.
 *
 * What breaks if this regresses: the shopping list and recipe_margin price
 * the same component differently.
 */
static void
test_marketdata_goal_materials(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) goal = NULL;
	g_autoptr(VentureEntity) step = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *unit = NULL;
	g_autofree gchar *cost = NULL;
	gint64 ore;
	gint64 bar;
	gint row;

	(void)user_data;

	seed_store(fixture);
	ore = product(fixture, "Copper Ore");
	bar = product(fixture, "Copper Bar");
	promoted(fixture, "2770", ore);

	goal = VENTURE_ENTITY(venture_goal_new());
	venture_entity_set_organization_id(goal, fixture->org);
	g_object_set(goal, "name", "Mining 75", "start-value", 0.0, "target-value", 75.0, NULL);
	save(fixture, goal);
	step = VENTURE_ENTITY(venture_goal_step_new());
	venture_entity_set_organization_id(step, fixture->org);
	g_object_set(step, "goal-id", ID(goal), "name", "Smelt", "position", (gint64)1,
	             "recipe-id", smelting(fixture, ore, bar, 0), "repetitions", (gint64)3, NULL);
	save(fixture, step);

	options = json_object_new();
	json_object_set_string_member(options, "price_source", "series:min@realm-a");
	result = run_report(fixture, "goal_materials", options, &error);
	g_assert_no_error(error);
	row = row_with(result, "product", "Copper Ore");
	g_assert_cmpint(row, >=, 0);
	unit = cell_money(result, row, "unit_price");
	cost = cell_money(result, row, "cost");
	g_assert_cmpstr(unit, ==, "1.20 USD");
	g_assert_cmpstr(cost, ==, "7.20 USD");
}

/*
 * Feeds off: the oracle answers "nothing observed" (and an observation
 * when allowed), and a series: price source is refused, naming the
 * module. Marketdata off: the same, naming that one.
 *
 * What breaks if this regresses: a report asked about market data with
 * feeds off reads every product as unpriced and says nothing about why.
 */
static void
test_marketdata_modules_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(GError) error = NULL;
	VentureMarketdataQuestion question;
	gint64 ore;
	gint64 bar;

	(void)user_data;

	seed_store(fixture);
	ore = product(fixture, "Copper Ore");
	bar = product(fixture, "Copper Bar");
	promoted(fixture, "2770", ore);
	smelting(fixture, ore, bar, 0);

	options = json_object_new();
	json_object_set_string_member(options, "price_source", "series:min@realm-a");

	g_object_set(fixture->config, "feeds-enabled", FALSE, NULL);
	g_assert_false(venture_context_module_enabled(fixture->context, "feeds"));

	venture_marketdata_question_init(&question);
	question.organization_id = fixture->org;
	question.product_id = ore;
	question.basis = VENTURE_MARKETDATA_BASIS_MIN;
	question.where = "realm-a";
	{
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureMarketdataEvidence) evidence = NULL;

		price = ask(fixture, &question, &evidence);
		g_assert_null(price);
		g_assert_nonnull(strstr(evidence->note, "feeds module is off"));
	}
	{
		g_autoptr(VentureReportResult) refused = NULL;

		refused = run_report(fixture, "recipe_margin", options, &error);
		g_assert_null(refused);
		g_assert_nonnull(strstr(error->message, "feeds module is off"));
		g_clear_error(&error);
	}

	venture_config_set_module_enabled(fixture->config, "marketdata", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "marketdata"));
	{
		g_autoptr(VentureReportResult) refused = NULL;
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureMarketdataEvidence) evidence = NULL;

		refused = run_report(fixture, "recipe_margin", options, &error);
		g_assert_null(refused);
		g_assert_nonnull(strstr(error->message, "marketdata module is off"));
		g_clear_error(&error);

		price = ask(fixture, &question, &evidence);
		g_assert_null(price);
		g_assert_nonnull(strstr(evidence->note, "marketdata module is off"));
	}
	{
		g_autoptr(GError) promote_error = NULL;

		g_assert_false(venture_marketdata_promote_instrument(fixture->context, fixture->org,
		                                                     fixture->source_id, "2447", NULL,
		                                                     NULL, &promote_error));
		g_assert_nonnull(strstr(promote_error->message, "marketdata module is off"));
	}
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/marketdata/price-source-grammar", test_marketdata_price_source_grammar);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/marketdata/records", test_marketdata_records);
	ADD("/marketdata/listing-venue", test_marketdata_listing_venue);
	ADD("/marketdata/promotion", test_marketdata_promotion);
	ADD("/marketdata/track-known", test_marketdata_track_known);
	ADD("/marketdata/oracle/bases", test_marketdata_oracle_bases);
	ADD("/marketdata/oracle/numbers", test_marketdata_oracle_numbers);
	ADD("/marketdata/oracle/currency", test_marketdata_oracle_currency);
	ADD("/marketdata/oracle/product-and-fallback", test_marketdata_oracle_product_and_fallback);
	ADD("/marketdata/recipe-margin", test_marketdata_recipe_margin);
	ADD("/marketdata/goal-materials", test_marketdata_goal_materials);
	ADD("/marketdata/modules-off", test_marketdata_modules_off);

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

#endif /* VENTURE_HAVE_SQLITE */
