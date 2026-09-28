/*
 * test-sessions.c - Sessions, their yields, posting and performance
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A session is worth recording only if three things hold. Its length must
 * follow its times, because the report divides by it. Posting must put a
 * yield's units on the shelf exactly once -- twice is stock that never
 * existed, a half-finished post is stock that vanished -- at no cost,
 * because nothing was paid for them. And the performance report must keep
 * to the books' rules: money per currency, a missing value named and not
 * read as zero, rates over finished hours only. These tests hold all
 * three.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
	gint64		 venture_id;
} Fixture;

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

/* Saves, expecting success; the refusal's reason is the failure. */
static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/* Saves, expecting a refusal whose message holds @fragment. */
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

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_context_get_default_organization_id(fixture->context);

	/* A game's gold: four minor digits, never to be added to dollars. */
	g_assert_true(venture_currency_register("GOLD", 4, NULL, FALSE, NULL, &error));
	g_assert_no_error(error);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Workshop", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), fixture->organization_id);
	save(fixture, venture);
	fixture->venture_id = ID(venture);
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
	venture_currency_clear_registered();

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

static VentureMoney *
money_of(const gchar *text)
{
	VentureMoney *money;

	money = venture_money_from_string(text, NULL, NULL);
	g_assert_nonnull(money);

	return money;
}

static GDateTime *
time_of(const gchar *text)
{
	GDateTime *when;

	when = venture_time_from_string(text, NULL);
	g_assert_nonnull(when);

	return when;
}

/* --- Records --------------------------------------------------------------- */

static gint64
organization(
	Fixture		*fixture,
	const gchar	*slug
){
	g_autoptr(VentureEntity) other = NULL;

	other = VENTURE_ENTITY(venture_organization_new());
	g_object_set(other, "name", slug, "slug", slug, NULL);
	save(fixture, other);

	return ID(other);
}

static gint64
product_priced(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*list_price
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) list = NULL;

	list = (NULL != list_price) ? money_of(list_price) : NULL;
	record = VENTURE_ENTITY(venture_product_new());
	g_object_set(record, "name", name, "list-price", list, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static gint64
product(
	Fixture		*fixture,
	const gchar	*name
){
	return product_priced(fixture, name, NULL);
}

static gint64
location(
	Fixture		*fixture,
	const gchar	*name
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_location_new());
	g_object_set(record, "name", name, "active", TRUE, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

/* A saved inventory item for @product_id at @location_id (0: none). */
static gint64
item_at(
	Fixture		*fixture,
	gint64		 product_id,
	gint64		 location_id
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_inventory_item_new());
	g_object_set(record, "product-id", product_id, "location-id", location_id, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static gint64
on_hand(
	Fixture		*fixture,
	gint64		 item_id
){
	g_autoptr(GError) error = NULL;
	gint64 units;

	units = venture_inventory_service_on_hand(
		venture_inventory_service_get(fixture->database), item_id, NULL, &error);
	g_assert_no_error(error);

	return units;
}

static guint
count_rows(
	Fixture	*fixture,
	GType	 type
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(rows);

	return rows->len;
}

/* An unsaved session starting at @started (ISO), ending at @ended (NULL:
 * open), in the default organization. */
static VentureEntity *
session_new(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*activity,
	const gchar	*started,
	const gchar	*ended
){
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	VentureEntity *record;

	start = time_of(started);
	end = (NULL != ended) ? time_of(ended) : NULL;
	record = VENTURE_ENTITY(venture_session_new());
	g_object_set(record, "name", name, "activity", activity, "started-at", start,
	             "ended-at", end, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);

	return record;
}

static gint64
session(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*activity,
	const gchar	*started,
	const gchar	*ended,
	gint64		 location_id,
	const gchar	*cost
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) spent = NULL;

	spent = (NULL != cost) ? money_of(cost) : NULL;
	record = session_new(fixture, name, activity, started, ended);
	g_object_set(record, "location-id", location_id, "cost", spent, NULL);
	save(fixture, record);

	return ID(record);
}

static VentureEntity *
yield_new(
	Fixture		*fixture,
	gint64		 session_id
){
	VentureEntity *record;

	record = VENTURE_ENTITY(venture_session_yield_new());
	g_object_set(record, "session-id", session_id, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);

	return record;
}

/* A saved goods yield; @unit_value and @item_id optional. */
static gint64
goods(
	Fixture		*fixture,
	gint64		 session_id,
	gint64		 product_id,
	gint64		 quantity,
	const gchar	*unit_value,
	gint64		 item_id
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) value = NULL;

	value = (NULL != unit_value) ? money_of(unit_value) : NULL;
	record = yield_new(fixture, session_id);
	g_object_set(record, "product-id", product_id, "quantity", quantity,
	             "unit-value", value, "inventory-item-id", item_id, NULL);
	save(fixture, record);

	return ID(record);
}

/* A saved money yield. */
static gint64
money_yield(
	Fixture		*fixture,
	gint64		 session_id,
	const gchar	*amount
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) value = NULL;

	value = money_of(amount);
	record = yield_new(fixture, session_id);
	g_object_set(record, "amount", value, NULL);
	save(fixture, record);

	return ID(record);
}

static VentureEntity *
reread(
	Fixture	*fixture,
	GType	 type,
	gint64	 id
){
	VentureEntity *record;

	record = venture_database_get(fixture->database, type, id, NULL);
	g_assert_nonnull(record);

	return record;
}

static gint64
int_of(
	Fixture		*fixture,
	GType		 type,
	gint64		 id,
	const gchar	*property
){
	g_autoptr(VentureEntity) record = NULL;
	gint64 value;

	record = reread(fixture, type, id);
	value = 0;
	g_object_get(record, property, &value, NULL);

	return value;
}

/* Posts, expecting success; returns how many yields were posted. */
static guint
post(
	Fixture	*fixture,
	gint64	 session_id
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) after = NULL;
	guint posted;

	posted = 0;

	if (!venture_sessions_post(fixture->database, session_id, NULL, &posted, &after,
	                           &error))
		g_error("post refused: %s", error->message);

	g_assert_nonnull(after);
	g_assert_cmpint(ID(after), ==, session_id);

	return posted;
}

static void
post_refused(
	Fixture		*fixture,
	gint64		 session_id,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) after = NULL;
	guint posted;

	posted = 99;
	g_assert_false(venture_sessions_post(fixture->database, session_id, NULL, &posted,
	                                     &after, &error));
	g_assert_nonnull(error);
	g_assert_null(after);
	g_assert_cmpuint(posted, ==, 0);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

/* ==========================================================================
 * Validators
 * ========================================================================== */

/*
 * Minutes follow the times: derived when both are set, an end filled in
 * when only minutes are given, zero while open. An end before the start,
 * a minutes figure that disagrees with the times, a negative one and a
 * run longer than a year are refused. posted-at written by hand is
 * ignored. What breaks: a per-hour rate divided by a stale or typed-in
 * duration, or a session that looks posted when nothing is in stock.
 */
static void
test_session_minutes(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) backwards = NULL;
	g_autoptr(VentureEntity) disagrees = NULL;
	g_autoptr(VentureEntity) negative = NULL;
	g_autoptr(VentureEntity) forever = NULL;
	g_autoptr(VentureEntity) by_minutes = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) reopened = NULL;
	g_autoptr(VentureEntity) stamped = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GDateTime) posted_at = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *end_text = NULL;
	gint64 closed;
	gint64 open;

	(void)user_data;

	closed = session(fixture, "Route", "herbing", "2026-03-01T10:00:00Z",
	                 "2026-03-01T11:30:20Z", 0, NULL);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION, closed, "minutes"), ==, 90);

	open = session(fixture, "Still going", "herbing", "2026-03-01T10:00:00Z", NULL, 0, NULL);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION, open, "minutes"), ==, 0);

	backwards = session_new(fixture, "Backwards", "herbing", "2026-03-01T10:00:00Z",
	                        "2026-03-01T09:00:00Z");
	save_refused(fixture, backwards, "before it started");

	disagrees = session_new(fixture, "Wrong", "herbing", "2026-03-01T10:00:00Z",
	                        "2026-03-01T11:00:00Z");
	g_object_set(disagrees, "minutes", (gint64)45, NULL);
	save_refused(fixture, disagrees, "the times give 60");

	negative = session_new(fixture, "Negative", "herbing", "2026-03-01T10:00:00Z", NULL);
	g_object_set(negative, "minutes", (gint64)-5, NULL);
	save_refused(fixture, negative, "cannot be negative");

	forever = session_new(fixture, "Forever", "herbing", "2026-03-01T10:00:00Z",
	                      "2027-06-01T10:00:00Z");
	save_refused(fixture, forever, "more than a year");

	/* Only minutes: a finished session, its end filled in. */
	by_minutes = session_new(fixture, "Ninety", "herbing", "2026-03-02T08:00:00Z", NULL);
	g_object_set(by_minutes, "minutes", (gint64)90, NULL);
	save(fixture, by_minutes);
	{
		g_autoptr(VentureEntity) stored = reread(fixture, VENTURE_TYPE_SESSION,
		                                         ID(by_minutes));

		g_object_get(stored, "ended-at", &end, NULL);
	}
	g_assert_nonnull(end);
	end_text = g_date_time_format_iso8601(end);
	g_assert_true(g_str_has_prefix(end_text, "2026-03-02T09:30:00"));

	/* Moving the end recomputes; the stored minutes a form posts back
	 * with it are not a disagreement. */
	edited = reread(fixture, VENTURE_TYPE_SESSION, closed);
	{
		g_autoptr(GDateTime) later = time_of("2026-03-01T12:00:00Z");

		g_object_set(edited, "ended-at", later, NULL);
	}
	save(fixture, edited);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION, closed, "minutes"), ==, 120);

	/* Clearing the end reopens it. */
	reopened = reread(fixture, VENTURE_TYPE_SESSION, closed);
	g_object_set(reopened, "ended-at", NULL, NULL);
	save(fixture, reopened);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION, closed, "minutes"), ==, 0);

	/* posted-at is the post's; written by hand it does not stick. */
	stamped = reread(fixture, VENTURE_TYPE_SESSION, open);
	now = venture_time_now();
	g_object_set(stamped, "posted-at", now, "notes", "touched", NULL);
	save(fixture, stamped);
	g_clear_object(&stamped);
	stamped = reread(fixture, VENTURE_TYPE_SESSION, open);
	g_object_get(stamped, "posted-at", &posted_at, NULL);
	g_assert_null(posted_at);
}

/*
 * A yield is goods or money, exactly one: a product with at least one
 * unit, or a positive amount. Unit value and stock belong to goods; the
 * stock must hold the product and live in the yield's organization; the
 * session must be the yield's organization's; and the stock movement is
 * the post's to write. What breaks: a yield counted twice by the report,
 * ore posted into the herb bag, or a yield made to look posted by hand.
 */
static void
test_yield_form(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) neither = NULL;
	g_autoptr(VentureEntity) both = NULL;
	g_autoptr(VentureEntity) no_units = NULL;
	g_autoptr(VentureEntity) zero = NULL;
	g_autoptr(VentureEntity) money_units = NULL;
	g_autoptr(VentureEntity) money_value = NULL;
	g_autoptr(VentureEntity) wrong_shelf = NULL;
	g_autoptr(VentureEntity) forged = NULL;
	g_autoptr(VentureEntity) foreign = NULL;
	g_autoptr(VentureMoney) five = NULL;
	g_autoptr(VentureMoney) nothing = NULL;
	g_autoptr(GError) error = NULL;
	gint64 run;
	gint64 herb;
	gint64 ore;
	gint64 ore_item;
	gint64 elsewhere;
	gint64 txn_id;

	(void)user_data;

	run = session(fixture, "Route", "herbing", "2026-03-01T10:00:00Z",
	              "2026-03-01T11:00:00Z", 0, NULL);
	herb = product(fixture, "Peacebloom");
	ore = product(fixture, "Copper Ore");
	ore_item = item_at(fixture, ore, 0);
	five = money_of("5.0000 GOLD");
	nothing = money_of("0.0000 GOLD");

	neither = yield_new(fixture, run);
	save_refused(fixture, neither, "needs goods");

	both = yield_new(fixture, run);
	g_object_set(both, "product-id", herb, "quantity", (gint64)1, "amount", five, NULL);
	save_refused(fixture, both, "not both");

	no_units = yield_new(fixture, run);
	g_object_set(no_units, "product-id", herb, NULL);
	save_refused(fixture, no_units, "at least 1");

	zero = yield_new(fixture, run);
	g_object_set(zero, "amount", nothing, NULL);
	save_refused(fixture, zero, "more than zero");

	money_units = yield_new(fixture, run);
	g_object_set(money_units, "amount", five, "quantity", (gint64)3, NULL);
	save_refused(fixture, money_units, "for goods");

	money_value = yield_new(fixture, run);
	g_object_set(money_value, "amount", five, "unit-value", five, NULL);
	save_refused(fixture, money_value, "for goods");

	wrong_shelf = yield_new(fixture, run);
	g_object_set(wrong_shelf, "product-id", herb, "quantity", (gint64)2,
	             "inventory-item-id", ore_item, NULL);
	save_refused(fixture, wrong_shelf, "holds Copper Ore, not Peacebloom");

	/* Forging a stock movement: a real one, on a yield never posted. */
	g_assert_true(venture_inventory_service_produce(
		venture_inventory_service_get(fixture->database), NULL, 0, ore_item, 1, NULL,
		"seed", NULL, &forged, NULL, &error));
	g_assert_no_error(error);
	txn_id = ID(forged);
	g_clear_object(&forged);
	forged = yield_new(fixture, run);
	g_object_set(forged, "product-id", ore, "quantity", (gint64)1,
	             "inventory-txn-id", txn_id, NULL);
	save_refused(fixture, forged, "set by posting");

	/* A yield in another organization than its session. */
	elsewhere = organization(fixture, "elsewhere");
	foreign = VENTURE_ENTITY(venture_session_yield_new());
	g_object_set(foreign, "session-id", run, "amount", five, NULL);
	venture_entity_set_organization_id(foreign, elsewhere);
	save_refused(fixture, foreign, "another organization");

	/* And the two good forms save. */
	goods(fixture, run, herb, 3, "1.0000 GOLD", 0);
	money_yield(fixture, run, "5.0000 GOLD");
}

/* ==========================================================================
 * Posting
 * ========================================================================== */

typedef struct
{
	gint64	bank;
	gint64	herb;
	gint64	ore;
	gint64	herb_item;
	gint64	ore_item;
	gint64	session;
	gint64	herb_yield;
	gint64	ore_yield;
	gint64	coin_yield;
} Farm;

/* A finished run at the bank: ten herbs valued at a gold each, three ore
 * into a named shelf, five gold looted. */
static void
farm(
	Fixture	*fixture,
	Farm	*out
){
	out->bank = location(fixture, "Bank");
	out->herb = product(fixture, "Peacebloom");
	out->ore = product(fixture, "Copper Ore");
	out->herb_item = item_at(fixture, out->herb, out->bank);
	out->ore_item = item_at(fixture, out->ore, out->bank);
	out->session = session(fixture, "Elwynn loop", "herbing", "2026-03-01T10:00:00Z",
	                       "2026-03-01T12:00:00Z", out->bank, "1.00 USD");
	out->herb_yield = goods(fixture, out->session, out->herb, 10, "1.0000 GOLD", 0);
	out->ore_yield = goods(fixture, out->session, out->ore, 3, NULL, out->ore_item);
	out->coin_yield = money_yield(fixture, out->session, "5.0000 GOLD");
}

/* The cost layers on @item_id: how many, units left, and their total. */
static void
layers_of(
	Fixture	*fixture,
	gint64	 item_id,
	guint	*out_count,
	gint64	*out_units,
	gint64	*out_amount
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_INVENTORY_COST_LAYER);
	venture_query_set_limit(query, 0);
	g_assert_true(venture_query_add_filter_int(query, "inventory-item-id",
		VENTURE_FILTER_OP_EQ, item_id, NULL));
	rows = venture_database_find(fixture->database, query, NULL);
	g_assert_nonnull(rows);
	*out_count = rows->len;
	*out_units = 0;
	*out_amount = 0;

	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureMoney) unit = NULL;
		gint64 remaining;

		g_object_get(g_ptr_array_index(rows, i), "remaining-qty", &remaining,
		             "unit-cost", &unit, NULL);
		*out_units += remaining;

		if (NULL != unit)
			*out_amount += venture_money_get_amount(unit) * remaining;
	}
}

/*
 * A post puts each goods yield into stock as a PRODUCTION movement named
 * "session:<id>", into the yield's own shelf or the one at the session's
 * location, with a zero cost layer -- the herbs were picked, not bought,
 * and their unit value is a valuation, not a cost. Each yield is stamped
 * with its movement and where it landed, the session with the time, and
 * the money yield is left alone. What breaks: a market valuation booked
 * as cost (profit when picked, none when sold), units with no layer that
 * the sale path then refuses, or a yield that does not say it is posted.
 */
static void
test_post_success(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) herb_yield = NULL;
	g_autoptr(VentureEntity) txn = NULL;
	g_autoptr(VentureEntity) before = NULL;
	g_autoptr(VentureEntity) after = NULL;
	g_autoptr(GDateTime) posted_at = NULL;
	g_autoptr(GDateTime) occurred = NULL;
	g_autoptr(GDateTime) ended = NULL;
	g_autofree gchar *reference = NULL;
	g_autofree gchar *expected = NULL;
	Farm farm_run;
	gint64 txn_id;
	gint64 units;
	gint64 amount;
	guint layers;
	gint kind;

	(void)user_data;

	farm(fixture, &farm_run);
	before = reread(fixture, VENTURE_TYPE_SESSION, farm_run.session);

	/* Two goods yields into stock, and the coins into the bank's holding. */
	g_assert_cmpuint(post(fixture, farm_run.session), ==, 3);

	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 10);
	g_assert_cmpint(on_hand(fixture, farm_run.ore_item), ==, 3);

	herb_yield = reread(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.herb_yield);
	g_object_get(herb_yield, "inventory-txn-id", &txn_id, NULL);
	g_assert_cmpint(txn_id, >, 0);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.herb_yield,
	                       "inventory-item-id"), ==, farm_run.herb_item);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.ore_yield,
	                       "inventory-txn-id"), >, 0);
	/* Money never touches stock: it lands in the location's holding, as a
	 * journal because GOLD posts (a book of its own; no rate to USD). */
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.coin_yield,
	                       "inventory-txn-id"), ==, 0);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.coin_yield,
	                       "journal-id"), >, 0);

	txn = reread(fixture, VENTURE_TYPE_INVENTORY_TXN, txn_id);
	g_object_get(txn, "kind", &kind, "reference", &reference,
	             "occurred-at", &occurred, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INVENTORY_TXN_KIND_PRODUCTION);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_INVENTORY_TXN, txn_id, "quantity"), ==, 10);
	expected = g_strdup_printf("session:%" G_GINT64_FORMAT, farm_run.session);
	g_assert_cmpstr(reference, ==, expected);
	g_assert_cmpint(venture_entity_get_organization_id(txn), ==, fixture->organization_id);

	/* Dated when the run ended: the units were in hand then. */
	g_object_get(before, "ended-at", &ended, NULL);
	g_assert_cmpint(g_date_time_compare(occurred, ended), ==, 0);

	/* One layer of ten, at nothing. */
	layers_of(fixture, farm_run.herb_item, &layers, &units, &amount);
	g_assert_cmpuint(layers, ==, 1);
	g_assert_cmpint(units, ==, 10);
	g_assert_cmpint(amount, ==, 0);

	/* The session is stamped, and the save bumped its version. */
	after = reread(fixture, VENTURE_TYPE_SESSION, farm_run.session);
	g_object_get(after, "posted-at", &posted_at, NULL);
	g_assert_nonnull(posted_at);
	g_assert_cmpint(venture_entity_get_version(after), >,
	                venture_entity_get_version(before));
}

/*
 * Posting twice posts nothing the second time and writes nothing at all;
 * a yield added after the first post is the only one the next post
 * touches. What breaks: a retried request doubling the shelf, or a new
 * yield that waits for a post that never looks at it.
 */
static void
test_post_idempotent(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	Farm farm_run;
	guint txns;
	gint64 extra;

	(void)user_data;

	farm(fixture, &farm_run);
	g_assert_cmpuint(post(fixture, farm_run.session), ==, 3);
	txns = count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN);
	first = reread(fixture, VENTURE_TYPE_SESSION, farm_run.session);

	g_assert_cmpuint(post(fixture, farm_run.session), ==, 0);
	g_assert_cmpuint(count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN), ==, txns);
	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 10);
	second = reread(fixture, VENTURE_TYPE_SESSION, farm_run.session);
	g_assert_cmpint(venture_entity_get_version(second), ==,
	                venture_entity_get_version(first));

	extra = goods(fixture, farm_run.session, farm_run.herb, 2, NULL, 0);
	g_assert_cmpuint(post(fixture, farm_run.session), ==, 1);
	g_assert_cmpuint(count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN), ==, txns + 1);
	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 12);
	g_assert_cmpint(on_hand(fixture, farm_run.ore_item), ==, 3);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION_YIELD, extra,
	                       "inventory-txn-id"), >, 0);
}

/*
 * A posted yield's goods are frozen and it cannot be deleted, nor can its
 * session; its value and notes stay editable, and an unposted yield
 * deletes as any record does. What breaks: a yield that says 11 while the
 * shelf got 10, or stock left on the shelf with nothing saying where it
 * came from.
 */
static void
test_post_immutable(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) changed = NULL;
	g_autoptr(VentureEntity) moved = NULL;
	g_autoptr(VentureEntity) revalued = NULL;
	g_autoptr(VentureEntity) doomed = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) loose = NULL;
	g_autoptr(VentureMoney) two = NULL;
	g_autoptr(GError) error = NULL;
	Farm farm_run;
	gint64 other;
	gint64 unposted;

	(void)user_data;

	farm(fixture, &farm_run);
	post(fixture, farm_run.session);

	changed = reread(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.herb_yield);
	g_object_set(changed, "quantity", (gint64)11, NULL);
	save_refused(fixture, changed, "is posted");

	other = session(fixture, "Other", "herbing", "2026-03-02T10:00:00Z",
	                "2026-03-02T11:00:00Z", 0, NULL);
	moved = reread(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.herb_yield);
	g_object_set(moved, "session-id", other, NULL);
	save_refused(fixture, moved, "is posted");

	/* A value is a valuation: it moves nothing, so it may change. */
	revalued = reread(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.herb_yield);
	two = money_of("2.0000 GOLD");
	g_object_set(revalued, "unit-value", two, "notes", "repriced", NULL);
	save(fixture, revalued);

	doomed = reread(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.herb_yield);
	g_assert_false(venture_database_delete(fixture->database, doomed, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "is posted"));
	g_clear_error(&error);

	run = reread(fixture, VENTURE_TYPE_SESSION, farm_run.session);
	g_assert_false(venture_database_delete(fixture->database, run, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "posted yields"));
	g_clear_error(&error);
	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 10);

	/* Never posted: deleted like anything else, and so is its session. */
	unposted = goods(fixture, other, farm_run.herb, 1, NULL, 0);
	loose = reread(fixture, VENTURE_TYPE_SESSION_YIELD, unposted);
	g_assert_true(venture_database_delete(fixture->database, loose, NULL, &error));
	g_assert_no_error(error);
	g_clear_object(&run);
	run = reread(fixture, VENTURE_TYPE_SESSION, other);
	g_assert_true(venture_database_delete(fixture->database, run, NULL, &error));
	g_assert_no_error(error);
}

/* Refuses the @user_data-th positive PRODUCTION movement once armed. */
typedef struct
{
	gboolean	armed;
	guint		seen;
	guint		refuse_at;
} Tripwire;

static gboolean
refuse_nth(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	Tripwire *wire;
	gint64 quantity;
	gint kind;

	(void)database;
	(void)previous;

	wire = user_data;
	g_object_get(entity, "quantity", &quantity, "kind", &kind, NULL);

	if (!wire->armed || (quantity <= 0) || (VENTURE_INVENTORY_TXN_KIND_PRODUCTION != kind))
		return TRUE;

	wire->seen++;

	if (wire->seen == wire->refuse_at)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "the shelf is full");
		return FALSE;
	}

	return TRUE;
}

/*
 * A failure on the second yield -- after the first was received, layered
 * and stamped -- rolls all of it back: no movement, no layer, no stamp,
 * no posted-at. And the database is usable afterwards. What breaks: half
 * a post, whose first yield is stamped and so is never posted again
 * although its units were taken back off the shelf.
 */
static void
test_post_rollback(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GDateTime) posted_at = NULL;
	Tripwire wire;
	Farm farm_run;
	guint txns;
	guint layers;

	(void)user_data;

	farm(fixture, &farm_run);
	wire.armed = FALSE;
	wire.seen = 0;
	wire.refuse_at = 2;
	venture_database_add_save_validator(fixture->database, VENTURE_TYPE_INVENTORY_TXN,
	                                    refuse_nth, &wire, NULL);
	wire.armed = TRUE;
	txns = count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN);
	layers = count_rows(fixture, VENTURE_TYPE_INVENTORY_COST_LAYER);

	post_refused(fixture, farm_run.session, "the shelf is full");

	g_assert_cmpuint(count_rows(fixture, VENTURE_TYPE_INVENTORY_TXN), ==, txns);
	g_assert_cmpuint(count_rows(fixture, VENTURE_TYPE_INVENTORY_COST_LAYER), ==, layers);
	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 0);
	g_assert_cmpint(int_of(fixture, VENTURE_TYPE_SESSION_YIELD, farm_run.herb_yield,
	                       "inventory-txn-id"), ==, 0);
	run = reread(fixture, VENTURE_TYPE_SESSION, farm_run.session);
	g_object_get(run, "posted-at", &posted_at, NULL);
	g_assert_null(posted_at);

	wire.armed = FALSE;
	g_assert_cmpuint(post(fixture, farm_run.session), ==, 3);
	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 10);
}

/*
 * Where a yield with no shelf of its own goes: the one inventory item for
 * the product at the session's location. Kept in two places with no
 * location named is refused, naming both and saying how to choose; kept
 * nowhere is refused, naming what to create. What breaks: a post that
 * guesses which bag the herbs went into.
 */
static void
test_post_stock(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) placed = NULL;
	gint64 bank;
	gint64 bag;
	gint64 herb;
	gint64 bank_item;
	gint64 bag_item;
	gint64 nowhere;
	gint64 anywhere;
	gint64 at_bag;
	gint64 lonely;

	(void)user_data;

	bank = location(fixture, "Bank");
	bag = location(fixture, "Bag");
	herb = product(fixture, "Peacebloom");
	bank_item = item_at(fixture, herb, bank);
	bag_item = item_at(fixture, herb, bag);

	anywhere = session(fixture, "Unplaced", "herbing", "2026-03-01T10:00:00Z",
	                   "2026-03-01T11:00:00Z", 0, NULL);
	goods(fixture, anywhere, herb, 4, NULL, 0);
	post_refused(fixture, anywhere, "2 places");
	post_refused(fixture, anywhere, "Set the session's location");

	/* Naming the location on the session answers it. */
	placed = reread(fixture, VENTURE_TYPE_SESSION, anywhere);
	g_object_set(placed, "location-id", bag, NULL);
	save(fixture, placed);
	g_assert_cmpuint(post(fixture, anywhere), ==, 1);
	g_assert_cmpint(on_hand(fixture, bag_item), ==, 4);
	g_assert_cmpint(on_hand(fixture, bank_item), ==, 0);

	at_bag = session(fixture, "Bagged", "herbing", "2026-03-02T10:00:00Z",
	                 "2026-03-02T11:00:00Z", bag, NULL);
	goods(fixture, at_bag, herb, 1, NULL, bank_item);
	g_assert_cmpuint(post(fixture, at_bag), ==, 1);
	g_assert_cmpint(on_hand(fixture, bank_item), ==, 1);

	nowhere = session(fixture, "Nowhere", "mining", "2026-03-03T10:00:00Z",
	                  "2026-03-03T11:00:00Z", 0, NULL);
	lonely = product(fixture, "Tin Ore");
	goods(fixture, nowhere, lonely, 2, NULL, 0);
	post_refused(fixture, nowhere, "No stock of Tin Ore");
}

/*
 * With the sales module off there are no products and no stock: a goods
 * yield is refused with a message that says so and what to do instead,
 * money yields still work, and posting is refused -- and not offered.
 * What breaks: a module that claims to need only core but cannot save a
 * cash-only session, or a Post button that can only fail.
 */
static void
test_sales_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) goods_yield = NULL;
	g_autoptr(VentureEntity) subject = NULL;
	VentureActionRegistry *registry;
	VentureAction *action;
	gint64 herb;
	gint64 stall;

	(void)user_data;

	herb = product(fixture, "Peacebloom");

	venture_config_set_module_enabled(fixture->config, "sales", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "sales"));
	g_assert_true(venture_context_module_enabled(fixture->context, "sessions"));

	stall = session(fixture, "Market day", "stall", "2026-03-01T08:00:00Z",
	                "2026-03-01T16:00:00Z", 0, "12.00 USD");
	money_yield(fixture, stall, "140.00 USD");

	goods_yield = yield_new(fixture, stall);
	g_object_set(goods_yield, "product-id", herb, "quantity", (gint64)1, NULL);
	save_refused(fixture, goods_yield, "sales module");

	post_refused(fixture, stall, "sales module");

	registry = venture_database_get_action_registry(fixture->database);
	action = venture_action_registry_lookup(registry, "session", "post");
	g_assert_nonnull(action);
	subject = reread(fixture, VENTURE_TYPE_SESSION, stall);
	g_assert_false(venture_action_registry_allowed(registry, action, subject, NULL,
	                                               VENTURE_USER_ROLE_EDITOR, NULL));
}

/*
 * The action is how every door posts: the page's button, the API,
 * `venturectl act`, the assistant. It is a record action on the session,
 * stageable, and returns the session as it stands after the post (its
 * version bumped by the stamp). It runs in the session's organization.
 * What breaks: a post run in the default organization while the session
 * belongs to another, or a returned object a version behind.
 */
static void
test_post_action(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureActionRegistry *registry;
	VentureAction *action;
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureEntity) theirs_result = NULL;
	g_autoptr(VentureEntity) their_session = NULL;
	g_autoptr(VentureEntity) their_product = NULL;
	g_autoptr(VentureEntity) their_item = NULL;
	g_autoptr(VentureEntity) their_yield = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = NULL;
	Farm farm_run;
	gint64 elsewhere;
	gboolean stageable;
	gboolean type_level;

	(void)user_data;

	farm(fixture, &farm_run);
	registry = venture_database_get_action_registry(fixture->database);
	action = venture_action_registry_lookup(registry, "session", "post");
	g_assert_nonnull(action);
	g_object_get(action, "stageable", &stageable, "type-level", &type_level, NULL);
	g_assert_true(stageable);
	g_assert_false(type_level);

	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               (GDestroyNotify)json_node_unref);
	result = venture_action_registry_perform(registry, "session", farm_run.session, "post",
	                                         params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(VENTURE_IS_SESSION(result));
	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 10);
	{
		g_autoptr(VentureEntity) stored = reread(fixture, VENTURE_TYPE_SESSION,
		                                         farm_run.session);

		g_assert_cmpint(venture_entity_get_version(result), ==,
		                venture_entity_get_version(stored));
	}

	/* Another organization's session posts into that organization's
	 * stock. */
	elsewhere = organization(fixture, "elsewhere");
	their_product = VENTURE_ENTITY(venture_product_new());
	g_object_set(their_product, "name", "Peacebloom", NULL);
	venture_entity_set_organization_id(their_product, elsewhere);
	save(fixture, their_product);
	their_item = VENTURE_ENTITY(venture_inventory_item_new());
	g_object_set(their_item, "product-id", ID(their_product), NULL);
	venture_entity_set_organization_id(their_item, elsewhere);
	save(fixture, their_item);
	start = time_of("2026-03-05T10:00:00Z");
	their_session = VENTURE_ENTITY(venture_session_new());
	g_object_set(their_session, "name", "Theirs", "started-at", start,
	             "minutes", (gint64)30, NULL);
	venture_entity_set_organization_id(their_session, elsewhere);
	save(fixture, their_session);
	their_yield = VENTURE_ENTITY(venture_session_yield_new());
	g_object_set(their_yield, "session-id", ID(their_session),
	             "product-id", ID(their_product), "quantity", (gint64)6, NULL);
	venture_entity_set_organization_id(their_yield, elsewhere);
	save(fixture, their_yield);

	theirs_result = venture_action_registry_perform(registry, "session", ID(their_session),
	                                                "post", params, NULL,
	                                                VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error);
	g_assert_nonnull(theirs_result);
	g_assert_cmpint(on_hand(fixture, ID(their_item)), ==, 6);
	g_assert_cmpint(on_hand(fixture, farm_run.herb_item), ==, 10);
}

/* ==========================================================================
 * session_performance
 * ========================================================================== */

static VentureReportResult *
performance(
	Fixture		 *fixture,
	const gchar	 *group_by,
	gint		  depth,
	const gchar	 *price_source,
	const gchar	 *as_of,
	GError		**error
){
	g_autoptr(JsonObject) options = NULL;
	VentureReport *report;

	options = json_object_new();

	if (NULL != group_by)
		json_object_set_string_member(options, "group_by", group_by);

	if (depth >= 0)
		json_object_set_int_member(options, "category_depth", depth);

	if (NULL != price_source)
		json_object_set_string_member(options, "price_source", price_source);

	if (NULL != as_of)
		json_object_set_string_member(options, "as_of", as_of);

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "session_performance");
	g_assert_nonnull(report);

	return venture_report_generate(report, fixture->context, NULL, options, error);
}

static const gchar *
cell_text(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);

	if ((NULL == value) || !G_VALUE_HOLDS_STRING(value))
		return NULL;

	return g_value_get_string(value);
}

/* The row for @group in @currency ("" for the row with none). */
static guint
row_of(
	VentureReportResult	*result,
	const gchar		*group,
	const gchar		*currency
){
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		const gchar *currency_cell;

		currency_cell = cell_text(result, i, "currency");

		if ((0 == g_strcmp0(cell_text(result, i, "group"), group)) &&
		    (0 == g_strcmp0((NULL != currency_cell) ? currency_cell : "", currency)))
			return i;
	}

	g_error("no row for %s in %s", group, currency);
	return 0;
}

static gchar *
cell_money(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);

	if ((NULL == value) || !G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY) ||
	    (NULL == g_value_get_boxed(value)))
		return NULL;

	return venture_money_to_string(g_value_get_boxed(value));
}

static gdouble
cell_number(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS_DOUBLE(value));

	return g_value_get_double(value);
}

#define ASSERT_CELL_MONEY(result, row, key, expected) G_STMT_START { \
	g_autofree gchar *cell_money_text = cell_money(result, row, key); \
	g_assert_cmpstr(cell_money_text, ==, expected); \
} G_STMT_END

static void
observe(
	Fixture		*fixture,
	gint64		 product_id,
	const gchar	*source,
	const gchar	*price,
	const gchar	*when
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) at = NULL;

	amount = money_of(price);
	at = time_of(when);
	record = VENTURE_ENTITY(venture_price_observation_new());
	g_object_set(record, "product-id", product_id, "source", source,
	             "price", amount, "observed-at", at, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);
	save(fixture, record);
}

typedef struct
{
	gint64	herb;
	gint64	ore;
	gint64	first;
	gint64	second;
	gint64	open;
} Herbing;

/*
 * Three herbing runs. The first: two hours, ten herbs valued at 1 gold
 * each, 5 gold looted, 1.00 USD of cost. The second: one hour, four ore
 * with no recorded value, observed at 2.5 gold ("market value") before
 * the run ended and at 7 gold after; a vendor quotes 9. The third is
 * still going: one herb at 1 gold.
 */
static void
herbing(
	Fixture	*fixture,
	Herbing	*out
){
	out->herb = product(fixture, "Peacebloom");
	out->ore = product(fixture, "Copper Ore");
	out->first = session(fixture, "Loop one", "herbing", "2026-03-01T10:00:00Z",
	                     "2026-03-01T12:00:00Z", 0, "1.00 USD");
	goods(fixture, out->first, out->herb, 10, "1.0000 GOLD", 0);
	money_yield(fixture, out->first, "5.0000 GOLD");
	out->second = session(fixture, "Loop two", "herbing", "2026-03-02T10:00:00Z",
	                      "2026-03-02T11:00:00Z", 0, NULL);
	goods(fixture, out->second, out->ore, 4, NULL, 0);
	out->open = session(fixture, "Loop three", "herbing", "2026-03-03T10:00:00Z",
	                    NULL, 0, NULL);
	goods(fixture, out->open, out->herb, 1, "1.0000 GOLD", 0);

	observe(fixture, out->ore, "market value", "2.5000 GOLD", "2026-03-01T00:00:00Z");
	observe(fixture, out->ore, "market value", "7.0000 GOLD", "2026-03-05T00:00:00Z");
	observe(fixture, out->ore, "vendor", "9.0000 GOLD", "2026-03-01T00:00:00Z");
}

/*
 * The arithmetic, in two currencies. Gold: 21 of goods (10 + 4 x 2.5 at
 * the run's end, not the later 7 + 1 from the open run), 5 looted, net
 * 26; per hour over the three finished hours, 25 gold (the open run's
 * herb is in the totals but not the rate) -- 8.3333. Dollars: 1.00 of
 * cost, net -1.00, -0.33 an hour. Sessions, open and hours repeat on both
 * rows. With as_of every yield is valued at that date instead, and a
 * different source gives a different answer. What breaks: gold added to
 * dollars, an open run inflating an hour it has not finished, or a price
 * read from after the run.
 */
static void
test_performance_math(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) later = NULL;
	g_autoptr(VentureReportResult) vendor = NULL;
	g_autoptr(GError) error = NULL;
	Herbing runs;
	guint gold;
	guint usd;

	(void)user_data;

	herbing(fixture, &runs);

	result = performance(fixture, NULL, -1, "market value", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);

	gold = row_of(result, "herbing", "GOLD");
	g_assert_cmpfloat(cell_number(result, gold, "sessions"), ==, 3.0);
	g_assert_cmpfloat(cell_number(result, gold, "open"), ==, 1.0);
	g_assert_cmpfloat(cell_number(result, gold, "hours"), ==, 3.0);
	g_assert_cmpfloat(cell_number(result, gold, "units"), ==, 15.0);
	ASSERT_CELL_MONEY(result, gold, "value", "21.0000 GOLD");
	ASSERT_CELL_MONEY(result, gold, "amount", "5.0000 GOLD");
	ASSERT_CELL_MONEY(result, gold, "cost", NULL);
	ASSERT_CELL_MONEY(result, gold, "net", "26.0000 GOLD");
	ASSERT_CELL_MONEY(result, gold, "value_per_hour", "8.3333 GOLD");
	ASSERT_CELL_MONEY(result, gold, "net_per_hour", "8.3333 GOLD");
	g_assert_cmpstr(cell_text(result, gold, "priced_by"), ==, "unit value, market value");
	g_assert_null(cell_text(result, gold, "note"));

	usd = row_of(result, "herbing", "USD");
	g_assert_cmpfloat(cell_number(result, usd, "sessions"), ==, 3.0);
	g_assert_cmpfloat(cell_number(result, usd, "hours"), ==, 3.0);
	ASSERT_CELL_MONEY(result, usd, "value", NULL);
	ASSERT_CELL_MONEY(result, usd, "cost", "1.00 USD");
	ASSERT_CELL_MONEY(result, usd, "net", "-1.00 USD");
	ASSERT_CELL_MONEY(result, usd, "value_per_hour", NULL);
	ASSERT_CELL_MONEY(result, usd, "net_per_hour", "-0.33 USD");

	/* Valued at one later date: the ore at 7. */
	later = performance(fixture, NULL, -1, "market value", "2026-03-10", &error);
	g_assert_no_error(error);
	ASSERT_CELL_MONEY(later, row_of(later, "herbing", "GOLD"), "value", "39.0000 GOLD");

	vendor = performance(fixture, "activity", -1, "vendor", NULL, &error);
	g_assert_no_error(error);
	gold = row_of(vendor, "herbing", "GOLD");
	ASSERT_CELL_MONEY(vendor, gold, "value", "47.0000 GOLD");
	g_assert_cmpstr(cell_text(vendor, gold, "priced_by"), ==, "unit value, vendor");
}

/*
 * A product with no recorded value and no price is named, and every
 * figure it would change -- goods value, net, both rates -- is blank,
 * never zero; what does not depend on it (money yielded, cost, counts)
 * stands. With the market module off, goods are valued at list price and
 * a price_source is refused rather than ignored. What breaks: a run that
 * reads as worthless because nobody priced what it found, or a question
 * about a price source answered from somewhere else.
 */
static void
test_performance_unpriced_and_market_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) listed = NULL;
	g_autoptr(VentureReportResult) refused = NULL;
	g_autoptr(GError) error = NULL;
	gint64 mystery;
	gint64 herb;
	gint64 run;
	gint64 stall;
	guint row;

	(void)user_data;

	mystery = product(fixture, "Mystery Box");
	run = session(fixture, "Dig", "digging", "2026-03-01T10:00:00Z",
	              "2026-03-01T11:00:00Z", 0, NULL);
	goods(fixture, run, mystery, 1, NULL, 0);
	money_yield(fixture, run, "3.0000 GOLD");

	result = performance(fixture, NULL, -1, NULL, NULL, &error);
	g_assert_no_error(error);
	row = row_of(result, "digging", "GOLD");
	g_assert_nonnull(strstr(cell_text(result, row, "note"), "no value for Mystery Box"));
	ASSERT_CELL_MONEY(result, row, "amount", "3.0000 GOLD");
	ASSERT_CELL_MONEY(result, row, "value", NULL);
	ASSERT_CELL_MONEY(result, row, "net", NULL);
	ASSERT_CELL_MONEY(result, row, "value_per_hour", NULL);
	ASSERT_CELL_MONEY(result, row, "net_per_hour", NULL);
	g_assert_cmpfloat(cell_number(result, row, "units"), ==, 1.0);

	/* Market off: list price. */
	herb = product_priced(fixture, "Silverleaf", "0.50 USD");
	stall = session(fixture, "Pick", "picking", "2026-03-02T10:00:00Z",
	                "2026-03-02T12:00:00Z", 0, NULL);
	goods(fixture, stall, herb, 10, NULL, 0);

	venture_config_set_module_enabled(fixture->config, "market", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "market"));

	listed = performance(fixture, NULL, -1, NULL, NULL, &error);
	g_assert_no_error(error);
	row = row_of(listed, "picking", "USD");
	ASSERT_CELL_MONEY(listed, row, "value", "5.00 USD");
	ASSERT_CELL_MONEY(listed, row, "value_per_hour", "2.50 USD");
	g_assert_cmpstr(cell_text(listed, row, "priced_by"), ==, "list price");

	refused = performance(fixture, NULL, -1, "market value", NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * Grouping by category reads each session's own category by its computed
 * path, and category_depth rolls children up into their ancestor; a
 * session filed nowhere is "Uncategorised"; venture groups by name. A
 * depth on an activity grouping and an unknown grouping are refused, and
 * grouping by location is refused while sales is off. What breaks: two
 * herb routes that are one question split in two, or an option silently
 * ignored.
 */
static void
test_performance_groups(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) leaves = NULL;
	g_autoptr(VentureReportResult) tops = NULL;
	g_autoptr(VentureReportResult) ventures = NULL;
	g_autoptr(VentureReportResult) refused = NULL;
	g_autoptr(VentureEntity) farming = NULL;
	g_autoptr(VentureEntity) herbs = NULL;
	g_autoptr(VentureEntity) ores = NULL;
	g_autoptr(VentureEntity) filed = NULL;
	g_autoptr(GError) error = NULL;
	gint64 one;
	gint64 two;
	guint row;

	(void)user_data;

	farming = VENTURE_ENTITY(venture_category_new());
	g_object_set(farming, "name", "Farming", "applies-to", "session", NULL);
	venture_entity_set_organization_id(farming, fixture->organization_id);
	save(fixture, farming);
	herbs = VENTURE_ENTITY(venture_category_new());
	g_object_set(herbs, "name", "Herbs", "applies-to", "session", "parent-id", ID(farming), NULL);
	venture_entity_set_organization_id(herbs, fixture->organization_id);
	save(fixture, herbs);
	ores = VENTURE_ENTITY(venture_category_new());
	g_object_set(ores, "name", "Ore", "applies-to", "session", "parent-id", ID(farming), NULL);
	venture_entity_set_organization_id(ores, fixture->organization_id);
	save(fixture, ores);

	one = session(fixture, "Herb loop", "herbing", "2026-03-01T10:00:00Z",
	              "2026-03-01T11:00:00Z", 0, NULL);
	two = session(fixture, "Mine loop", "mining", "2026-03-02T10:00:00Z",
	              "2026-03-02T12:00:00Z", 0, NULL);
	session(fixture, "Loose", "resting", "2026-03-03T10:00:00Z",
	        "2026-03-03T10:30:00Z", 0, NULL);

	filed = reread(fixture, VENTURE_TYPE_SESSION, one);
	g_object_set(filed, "category-id", ID(herbs), "venture-id", fixture->venture_id, NULL);
	save(fixture, filed);
	g_clear_object(&filed);
	filed = reread(fixture, VENTURE_TYPE_SESSION, two);
	g_object_set(filed, "category-id", ID(ores), NULL);
	save(fixture, filed);

	leaves = performance(fixture, "category", -1, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(leaves), ==, 3);
	row = row_of(leaves, "Farming / Herbs", "");
	g_assert_cmpfloat(cell_number(leaves, row, "hours"), ==, 1.0);
	row = row_of(leaves, "Farming / Ore", "");
	g_assert_cmpfloat(cell_number(leaves, row, "hours"), ==, 2.0);
	row_of(leaves, "Uncategorised", "");

	tops = performance(fixture, "category", 0, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(tops), ==, 2);
	row = row_of(tops, "Farming", "");
	g_assert_cmpfloat(cell_number(tops, row, "sessions"), ==, 2.0);
	g_assert_cmpfloat(cell_number(tops, row, "hours"), ==, 3.0);

	ventures = performance(fixture, "venture", -1, NULL, NULL, &error);
	g_assert_no_error(error);
	row = row_of(ventures, "Workshop", "");
	g_assert_cmpfloat(cell_number(ventures, row, "sessions"), ==, 1.0);
	row = row_of(ventures, "No venture", "");
	g_assert_cmpfloat(cell_number(ventures, row, "sessions"), ==, 2.0);

	refused = performance(fixture, "activity", 0, NULL, NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	refused = performance(fixture, "weather", -1, NULL, NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	venture_config_set_module_enabled(fixture->config, "sales", FALSE);
	refused = performance(fixture, "location", -1, NULL, NULL, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "sales module"));
}

/* ==========================================================================
 * The module
 * ========================================================================== */

/*
 * The module needs only core and suggests sales and market. Switching it
 * off hides its types, its report and its action; switching it back
 * restores all three with nothing re-registered; and sales off leaves it
 * on. What breaks: a Post button on an install that turned sessions off,
 * a report that vanishes for good, or a cash-only install that cannot
 * have sessions because it sells no products.
 */
static void
test_module_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureEntityRegistry *types;
	VentureReportRegistry *reports;
	VentureActionRegistry *actions;
	VentureModule *sessions;

	(void)user_data;

	sessions = venture_module_registry_lookup(venture_context_get_modules(fixture->context),
	                                          "sessions");
	g_assert_nonnull(sessions);
	g_assert_true(g_strv_contains(venture_module_get_requires(sessions), "core"));
	g_assert_false(g_strv_contains(venture_module_get_requires(sessions), "sales"));
	g_assert_true(g_strv_contains(venture_module_get_suggests(sessions), "sales"));
	g_assert_true(g_strv_contains(venture_module_get_suggests(sessions), "market"));
	g_assert_true(g_strv_contains(venture_module_get_reports(sessions), "session_performance"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(sessions), "session"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(sessions), "session_yield"));

	types = venture_context_get_entity_registry(fixture->context);
	reports = venture_context_get_report_registry(fixture->context);
	actions = venture_database_get_action_registry(fixture->database);
	g_assert_nonnull(venture_action_registry_lookup(actions, "session", "post"));

	venture_config_set_module_enabled(fixture->config, "sessions", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "sessions"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "session"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "session_yield"));
	g_assert_null(venture_report_registry_lookup(reports, "session_performance"));
	g_assert_null(venture_action_registry_lookup(actions, "session", "post"));

	venture_config_set_module_enabled(fixture->config, "sessions", TRUE);
	g_assert_nonnull(venture_report_registry_lookup(reports, "session_performance"));
	g_assert_nonnull(venture_action_registry_lookup(actions, "session", "post"));

	venture_config_set_module_enabled(fixture->config, "sales", FALSE);
	g_assert_true(venture_context_module_enabled(fixture->context, "sessions"));
	g_assert_true(VENTURE_TYPE_SESSION == venture_entity_registry_lookup(types, "session"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "session_performance"));
}

/* ==========================================================================
 * HTTP
 * ========================================================================== */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gchar		**out_body
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != fixture->cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "sessions-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-sessions-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 9241) % 20000));

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };

		url = g_strdup_printf("http://127.0.0.1:%u/login", fixture->port);
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1",
		                           strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(fixture->session, message,
		                                 G_PRIORITY_DEFAULT, NULL,
		                                 request_done, &outcome);

		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);

		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);

		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}

	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/* Saves @record under the default organization, expecting success. */
static void
server_save(
	ServerFixture	*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/*
 * Every door reaches sessions: the API forwards group_by and
 * price_source to the report (an option dropped on the way would answer
 * a different question), the report page offers them, the session page
 * offers Post, the action endpoint posts from an empty JSON body, the
 * record API applies the yield validator, and a posted yield cannot be
 * deleted through it. What breaks: an option the page and the API never
 * pass, a session page with no way to post, or a door around the rules.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) herb = NULL;
	g_autoptr(VentureEntity) herb_item = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) picked = NULL;
	g_autoptr(VentureEntity) seen = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *request = NULL;
	JsonArray *rows;
	guint status;

	(void)user_data;

	herb = VENTURE_ENTITY(venture_product_new());
	g_object_set(herb, "name", "Peacebloom", NULL);
	server_save(fixture, herb);
	herb_item = VENTURE_ENTITY(venture_inventory_item_new());
	g_object_set(herb_item, "product-id", ID(herb), NULL);
	server_save(fixture, herb_item);

	start = g_date_time_new_utc(2026, 3, 2, 10, 0, 0);
	end = g_date_time_new_utc(2026, 3, 2, 12, 0, 0);
	run = VENTURE_ENTITY(venture_session_new());
	g_object_set(run, "name", "Loop", "activity", "herbing", "started-at", start,
	             "ended-at", end, NULL);
	server_save(fixture, run);
	picked = VENTURE_ENTITY(venture_session_yield_new());
	g_object_set(picked, "session-id", ID(run), "product-id", ID(herb),
	             "quantity", (gint64)8, NULL);
	server_save(fixture, picked);

	price = venture_money_new(250, "USD", 2);
	when = g_date_time_new_utc(2026, 3, 1, 12, 0, 0);
	seen = VENTURE_ENTITY(venture_price_observation_new());
	g_object_set(seen, "product-id", ID(herb), "source", "vendor",
	             "price", price, "observed-at", when, NULL);
	server_save(fixture, seen);

	g_assert_cmpuint(server_request(fixture, "GET",
		"/api/v1/reports/session_performance?period=all&group_by=activity&price_source=vendor",
		NULL, NULL, &body), ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	rows = json_object_get_array_member(json_node_get_object(node), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "priced_by"), ==, "vendor");
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "group"), ==, "herbing");

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "GET",
		"/reports/session_performance?period=all&price_source=vendor", NULL, NULL, &body),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "name=\"group_by\""));
	g_assert_nonnull(strstr(body, "name=\"price_source\""));
	g_assert_nonnull(strstr(body, "Net per hour"));

	/* The session page: its yields among the related records, and a
	 * Post form posting to the action. */
	g_clear_pointer(&body, g_free);
	path = g_strdup_printf("/e/session/%" G_GINT64_FORMAT, ID(run));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body),
		==, SOUP_STATUS_OK);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/session/%" G_GINT64_FORMAT "/actions/post", ID(run));
	g_assert_nonnull(strstr(body, path));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "POST", path, "application/json", "{}",
		&body), ==, SOUP_STATUS_OK);
	g_assert_cmpint(venture_inventory_service_on_hand(
		venture_inventory_service_get(fixture->database), ID(herb_item), NULL, NULL), ==, 8);

	/* Again: nothing new, still a success, nothing moved. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "POST", path, "application/json", "{}",
		&body), ==, SOUP_STATUS_OK);
	g_assert_cmpint(venture_inventory_service_on_hand(
		venture_inventory_service_get(fixture->database), ID(herb_item), NULL, NULL), ==, 8);

	/* The record API is the same validator: a product with no quantity. */
	g_clear_pointer(&body, g_free);
	request = g_strdup_printf("{\"session_id\":%" G_GINT64_FORMAT ",\"product_id\":%"
	                          G_GINT64_FORMAT "}", ID(run), ID(herb));
	status = server_request(fixture, "POST", "/api/v1/session_yield", "application/json",
	                        request, &body);
	g_assert_cmpuint(status, ==, 422);
	g_assert_nonnull(strstr(body, "at least 1"));

	/* And the posted yield stays. */
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/session_yield/%" G_GINT64_FORMAT, ID(picked));
	status = server_request(fixture, "DELETE", path, NULL, NULL, &body);
	g_assert_cmpuint(status, >=, 400);
	g_assert_nonnull(strstr(body, "is posted"));
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/sessions/validators/minutes", test_session_minutes);
	ADD("/sessions/validators/yield-form", test_yield_form);
	ADD("/sessions/post/success", test_post_success);
	ADD("/sessions/post/idempotent", test_post_idempotent);
	ADD("/sessions/post/immutable", test_post_immutable);
	ADD("/sessions/post/rollback", test_post_rollback);
	ADD("/sessions/post/stock", test_post_stock);
	ADD("/sessions/post/sales-off", test_sales_off);
	ADD("/sessions/post/action", test_post_action);
	ADD("/sessions/performance/math", test_performance_math);
	ADD("/sessions/performance/unpriced-and-market-off",
	    test_performance_unpriced_and_market_off);
	ADD("/sessions/performance/groups", test_performance_groups);
	ADD("/sessions/module-off", test_module_off);
	g_test_add("/sessions/http", ServerFixture, NULL, server_fixture_set_up,
	           test_http, server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
