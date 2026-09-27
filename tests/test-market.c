/*
 * test-market.c - Price observations, listings and their reports
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The market module keeps what a sale record cannot: prices somebody saw,
 * and offers that ended without selling. Its figures are only worth
 * reading if the records agree with themselves -- a listing marked sold
 * with two of five units counted is a sale rate nobody can trust -- and if
 * the reports keep to the books' rules: money per currency and never
 * across, an average of money rounded half to even once, a bucket a UTC
 * bucket, another organization's rows invisible. These tests hold both.
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

/* Saves, expecting a validation refusal whose message holds @fragment. */
static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
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

	/* A user-defined currency with four minor digits, as a game's gold,
	 * silver and copper are. It must never be added to dollars. */
	g_assert_true(venture_currency_register("GOLD", 4, NULL, FALSE, NULL, &error));
	g_assert_no_error(error);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Stall", NULL);
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

/* A saved category under @parent_id (0 for a top level). */
static gint64
category(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 parent_id
){
	g_autoptr(VentureEntity) node = NULL;

	node = VENTURE_ENTITY(venture_category_new());
	g_object_set(node, "name", name, "parent-id", parent_id,
	             "applies-to", "product", NULL);
	venture_entity_set_organization_id(node, fixture->organization_id);
	save(fixture, node);

	return ID(node);
}

/* A saved product in @organization_id (0 for the default). */
static gint64
product_in(
	Fixture		*fixture,
	gint64		 organization_id,
	gint64		 venture_id,
	const gchar	*name,
	gint64		 category_id
){
	g_autoptr(VentureEntity) record = NULL;

	record = VENTURE_ENTITY(venture_product_new());
	g_object_set(record, "name", name, "venture-id", venture_id,
	             "category-id", category_id, NULL);
	venture_entity_set_organization_id(record, (0 != organization_id)
		? organization_id : fixture->organization_id);
	save(fixture, record);

	return ID(record);
}

static gint64
product(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 category_id
){
	return product_in(fixture, 0, fixture->venture_id, name, category_id);
}

/* An unsaved observation. */
static VentureEntity *
observation_new(
	Fixture		*fixture,
	gint64		 organization_id,
	gint64		 product_id,
	const gchar	*source,
	const gchar	*price,
	const gchar	*when,
	gint64		 volume
){
	VentureEntity *record;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) at = NULL;

	record = VENTURE_ENTITY(venture_price_observation_new());
	amount = (NULL != price) ? money_of(price) : NULL;
	at = time_of(when);
	g_object_set(record, "product-id", product_id, "source", source,
	             "price", amount, "observed-at", at, "volume", volume, NULL);
	venture_entity_set_organization_id(record, (0 != organization_id)
		? organization_id : fixture->organization_id);

	return record;
}

static gint64
observe(
	Fixture		*fixture,
	gint64		 product_id,
	const gchar	*source,
	const gchar	*price,
	const gchar	*when,
	gint64		 volume
){
	g_autoptr(VentureEntity) record = NULL;

	record = observation_new(fixture, 0, product_id, source, price, when, volume);
	save(fixture, record);

	return ID(record);
}

/* An unsaved listing, open, with no deposit or fees. */
static VentureEntity *
listing_new(
	Fixture		*fixture,
	gint64		 product_id,
	const gchar	*channel,
	gint64		 quantity,
	const gchar	*unit_price,
	const gchar	*listed_at
){
	VentureEntity *record;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(GDateTime) at = NULL;

	record = VENTURE_ENTITY(venture_listing_new());
	price = money_of(unit_price);
	at = time_of(listed_at);
	g_object_set(record, "product-id", product_id, "channel", channel,
	             "quantity", quantity, "unit-price", price, "listed-at", at, NULL);
	venture_entity_set_organization_id(record, fixture->organization_id);

	return record;
}

/*
 * A saved listing that ended with @outcome, @sold units counted, closing
 * at @closed_at (NULL to let the validator derive it), with an optional
 * deposit and fees.
 */
static gint64
listing(
	Fixture			*fixture,
	gint64			 product_id,
	const gchar		*channel,
	gint64			 quantity,
	const gchar		*unit_price,
	const gchar		*listed_at,
	VentureListingOutcome	 outcome,
	gint64			 sold,
	const gchar		*closed_at,
	const gchar		*deposit,
	const gchar		*fees
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureMoney) deposit_money = NULL;
	g_autoptr(VentureMoney) fees_money = NULL;
	g_autoptr(GDateTime) closed = NULL;

	record = listing_new(fixture, product_id, channel, quantity, unit_price, listed_at);
	deposit_money = (NULL != deposit) ? money_of(deposit) : NULL;
	fees_money = (NULL != fees) ? money_of(fees) : NULL;
	closed = (NULL != closed_at) ? time_of(closed_at) : NULL;
	g_object_set(record, "outcome", outcome, "quantity-sold", sold,
	             "closed-at", closed, "deposit", deposit_money,
	             "fees", fees_money, NULL);
	save(fixture, record);

	return ID(record);
}

/* --- Report helpers -------------------------------------------------------- */

/*
 * Runs report @name with key/value string options (integers for the
 * options that are integers, as the web layer sends them).
 */
static VentureReportResult *
run(
	Fixture		 *fixture,
	const gchar	 *name,
	const gchar	 *period,
	GError		**error,
	...
){
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	VentureReport *report;
	const gchar *key;
	va_list args;

	options = json_object_new();
	va_start(args, error);

	while (NULL != (key = va_arg(args, const gchar *)))
	{
		const gchar *value;

		value = va_arg(args, const gchar *);

		if ((0 == g_strcmp0(key, "category_depth")) ||
		    (0 == g_strcmp0(key, "organization_id")) ||
		    (0 == g_strcmp0(key, "venture_id")) ||
		    (0 == g_strcmp0(key, "product_id")))
			json_object_set_int_member(options, key, g_ascii_strtoll(value, NULL, 10));
		else
			json_object_set_string_member(options, key, value);
	}

	va_end(args);

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), name);
	g_assert_nonnull(report);
	range = venture_context_parse_period(fixture->context, period, NULL);
	g_assert_nonnull(range);

	return venture_report_generate(report, fixture->context, range, options, error);
}

#define RUN(fixture, name, period, ...) ({ \
	g_autoptr(GError) run_error = NULL; \
	VentureReportResult *run_result = run(fixture, name, period, &run_error, __VA_ARGS__, NULL); \
	if (NULL == run_result) \
		g_error("%s refused: %s", name, run_error->message); \
	run_result; })

static const gchar *
text(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS_STRING(value));

	return g_value_get_string(value);
}

static gdouble
number(
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

static gchar *
money_text(
	VentureReportResult	*result,
	guint			 row,
	const gchar		*key
){
	const GValue *value;

	value = venture_report_result_get_cell(result, row, key);
	g_assert_nonnull(value);
	g_assert_true(G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY));

	return venture_money_to_string(g_value_get_boxed(value));
}

#define ASSERT_MONEY(result, row, key, expected) G_STMT_START { \
	g_autofree gchar *assert_money_text = money_text(result, row, key); \
	g_assert_cmpstr(assert_money_text, ==, expected); \
} G_STMT_END

/* The row whose @key column reads @value, failing when there is none. */
static guint
row_where(
	VentureReportResult	*result,
	const gchar		*key,
	const gchar		*value
){
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		if (0 == g_strcmp0(text(result, i, key), value))
			return i;
	}

	g_error("no row with %s = %s", key, value);
	return 0;
}

/* ==========================================================================
 * Observations
 * ========================================================================== */

/*
 * An observation needs a product and a price that is not negative. What
 * breaks if this regresses: an observation of nothing, or a negative
 * market value, becomes the price every valuing report reads.
 */
static void
test_observation_validators(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) no_product = NULL;
	g_autoptr(VentureEntity) no_price = NULL;
	g_autoptr(VentureEntity) negative = NULL;
	g_autoptr(VentureEntity) bad_volume = NULL;
	gint64 herb;

	(void)user_data;

	herb = product(fixture, "Herb", 0);

	no_product = observation_new(fixture, 0, 0, "market value", "1.00 USD",
	                             "2026-03-01T00:00:00Z", 0);
	save_refused(fixture, no_product, "Product");

	no_price = observation_new(fixture, 0, herb, "market value", NULL,
	                           "2026-03-01T00:00:00Z", 0);
	save_refused(fixture, no_price, "Price");

	negative = observation_new(fixture, 0, herb, "market value", "-1.00 USD",
	                           "2026-03-01T00:00:00Z", 0);
	save_refused(fixture, negative, "negative");

	bad_volume = observation_new(fixture, 0, herb, "market value", "1.00 USD",
	                             "2026-03-01T00:00:00Z", -3);
	save_refused(fixture, bad_volume, "Volume");

	/* Zero is a price: a thing can be seen to fetch nothing. */
	observe(fixture, herb, "market value", "0.00 USD", "2026-03-01T00:00:00Z", 0);
}

/* ==========================================================================
 * Listings
 * ========================================================================== */

/*
 * At least one unit, sold between none and all. What breaks: a listing
 * of nothing, or eleven sold of ten, and every rate over it.
 */
static void
test_listing_quantities(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) empty = NULL;
	g_autoptr(VentureEntity) oversold = NULL;
	g_autoptr(VentureEntity) negative = NULL;
	g_autoptr(VentureEntity) no_product = NULL;
	gint64 herb;

	(void)user_data;

	herb = product(fixture, "Herb", 0);

	empty = listing_new(fixture, herb, "auction", 0, "1.00 USD", "2026-03-01T00:00:00Z");
	save_refused(fixture, empty, "Quantity");

	oversold = listing_new(fixture, herb, "auction", 10, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(oversold, "quantity-sold", (gint64)11, NULL);
	save_refused(fixture, oversold, "between 0 and the quantity");

	negative = listing_new(fixture, herb, "auction", 10, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(negative, "quantity-sold", (gint64)-1, NULL);
	save_refused(fixture, negative, "between 0 and the quantity");

	no_product = listing_new(fixture, 0, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z");
	save_refused(fixture, no_product, "Product");

	/* Open and part sold is allowed: the rest is still on offer. */
	listing(fixture, herb, "auction", 10, "1.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_OPEN, 4, NULL, NULL, NULL);
}

/*
 * A listing's and an observation's display name show a denominated price
 * the way a person reads it. What breaks if it regresses: every picker,
 * related list and activity line reads "1 x 7.0720 GEMS" for what the
 * price field itself shows as "7g 7s 20c".
 */
static void
test_display_names_denominated(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) offer = NULL;
	g_autoptr(VentureEntity) seen = NULL;
	g_autofree gchar *offer_name = NULL;
	g_autofree gchar *seen_name = NULL;
	gint64 herb;

	(void)user_data;

	g_assert_true(venture_currency_register("GEMS", 4, NULL, FALSE,
		"[{\"suffix\":\"g\",\"units\":10000},"
		"{\"suffix\":\"s\",\"units\":100},"
		"{\"suffix\":\"c\",\"units\":1}]", &error));
	g_assert_no_error(error);

	herb = product(fixture, "Herb", 0);

	offer = listing_new(fixture, herb, "auction house", 1, "7.0720 GEMS",
	                    "2026-03-01T00:00:00Z");
	offer_name = venture_entity_get_display_name(offer);
	g_assert_cmpstr(offer_name, ==, "auction house: 1 x 7g 7s 20c");

	seen = observation_new(fixture, fixture->organization_id, herb, "vendor",
	                       "7.0720 GEMS", "2026-03-01T00:00:00Z", 0);
	seen_name = venture_entity_get_display_name(seen);
	g_assert_cmpstr(seen_name, ==, "vendor: 7g 7s 20c");
}

/*
 * The outcome agrees with the count. Sold with nothing counted is filled
 * to the quantity; sold with some counted is a contradiction; partial is
 * some and not all; expired and cancelled are none. What breaks: a sale
 * rate over listings whose outcome and count disagree, which is two
 * answers to one question.
 */
static void
test_listing_outcome_and_count(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) filled = NULL;
	g_autoptr(VentureEntity) contradiction = NULL;
	g_autoptr(VentureEntity) partial_none = NULL;
	g_autoptr(VentureEntity) partial_all = NULL;
	g_autoptr(VentureEntity) expired_some = NULL;
	gint64 herb;
	gint64 sold;
	gint64 id;

	(void)user_data;

	herb = product(fixture, "Herb", 0);

	id = listing(fixture, herb, "auction", 5, "1.00 USD", "2026-03-01T00:00:00Z",
	             VENTURE_LISTING_OUTCOME_SOLD, 0, NULL, NULL, NULL);
	filled = venture_database_get(fixture->database, VENTURE_TYPE_LISTING, id, NULL);
	g_object_get(filled, "quantity-sold", &sold, NULL);
	g_assert_cmpint(sold, ==, 5);

	contradiction = listing_new(fixture, herb, "auction", 5, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(contradiction, "outcome", VENTURE_LISTING_OUTCOME_SOLD,
	             "quantity-sold", (gint64)2, NULL);
	save_refused(fixture, contradiction, "mark it partial");

	partial_none = listing_new(fixture, herb, "auction", 5, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(partial_none, "outcome", VENTURE_LISTING_OUTCOME_PARTIAL, NULL);
	save_refused(fixture, partial_none, "some but not all");

	partial_all = listing_new(fixture, herb, "auction", 5, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(partial_all, "outcome", VENTURE_LISTING_OUTCOME_PARTIAL,
	             "quantity-sold", (gint64)5, NULL);
	save_refused(fixture, partial_all, "some but not all");

	expired_some = listing_new(fixture, herb, "auction", 5, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(expired_some, "outcome", VENTURE_LISTING_OUTCOME_EXPIRED,
	             "quantity-sold", (gint64)1, NULL);
	save_refused(fixture, expired_some, "nothing sold");
}

/*
 * The closing time follows the outcome: derived when an ended listing has
 * none, kept when given, cleared on reopening, refused on an open listing
 * and before the listing opened. What breaks: an ended listing with no
 * closing time has no time to sell, and a reopened one keeps an end it no
 * longer has.
 */
static void
test_listing_closed_at(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) derived = NULL;
	g_autoptr(VentureEntity) kept = NULL;
	g_autoptr(VentureEntity) reopened = NULL;
	g_autoptr(VentureEntity) open_closed = NULL;
	g_autoptr(VentureEntity) backwards = NULL;
	g_autoptr(GDateTime) closed_at = NULL;
	g_autoptr(GDateTime) given = NULL;
	g_autoptr(GDateTime) before = NULL;
	gint64 herb;
	gint64 id;

	(void)user_data;

	herb = product(fixture, "Herb", 0);
	before = venture_time_now();

	/* Derived: expired with no closing time is given now. */
	id = listing(fixture, herb, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z",
	             VENTURE_LISTING_OUTCOME_EXPIRED, 0, NULL, NULL, NULL);
	derived = venture_database_get(fixture->database, VENTURE_TYPE_LISTING, id, NULL);
	g_object_get(derived, "closed-at", &closed_at, NULL);
	g_assert_nonnull(closed_at);
	g_assert_cmpint(g_date_time_compare(closed_at, before), >=, 0);
	g_clear_pointer(&closed_at, g_date_time_unref);

	/* Kept: a given closing time is never overwritten. */
	id = listing(fixture, herb, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z",
	             VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-03T12:00:00Z", NULL, NULL);
	kept = venture_database_get(fixture->database, VENTURE_TYPE_LISTING, id, NULL);
	g_object_get(kept, "closed-at", &closed_at, NULL);
	given = time_of("2026-03-03T12:00:00Z");
	g_assert_true(venture_time_equal(closed_at, given));
	g_clear_pointer(&closed_at, g_date_time_unref);

	/* Reopened: back to open clears it. */
	g_object_set(kept, "outcome", VENTURE_LISTING_OUTCOME_OPEN,
	             "quantity-sold", (gint64)0, NULL);
	save(fixture, kept);
	reopened = venture_database_get(fixture->database, VENTURE_TYPE_LISTING, id, NULL);
	g_object_get(reopened, "closed-at", &closed_at, NULL);
	g_assert_null(closed_at);

	/* An open listing that was never closed may not carry one. */
	open_closed = listing_new(fixture, herb, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(open_closed, "closed-at", given, NULL);
	save_refused(fixture, open_closed, "has not closed");

	/* Closing before it opened. */
	backwards = listing_new(fixture, herb, "auction", 1, "1.00 USD", "2026-03-05T00:00:00Z");
	g_object_set(backwards, "outcome", VENTURE_LISTING_OUTCOME_EXPIRED,
	             "closed-at", given, NULL);
	save_refused(fixture, backwards, "before it was listed");
}

/*
 * A listing's amounts share one currency, and none is negative. What
 * breaks: deposits lost summed across gold and dollars.
 */
static void
test_listing_one_currency(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) mixed_deposit = NULL;
	g_autoptr(VentureEntity) mixed_fees = NULL;
	g_autoptr(VentureEntity) negative_fees = NULL;
	g_autoptr(VentureMoney) gold = NULL;
	g_autoptr(VentureMoney) minus = NULL;
	gint64 herb;

	(void)user_data;

	herb = product(fixture, "Herb", 0);
	gold = money_of("0.5000 GOLD");
	minus = money_of("-1.00 USD");

	mixed_deposit = listing_new(fixture, herb, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(mixed_deposit, "deposit", gold, NULL);
	save_refused(fixture, mixed_deposit, "share one currency");

	mixed_fees = listing_new(fixture, herb, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(mixed_fees, "fees", gold, NULL);
	save_refused(fixture, mixed_fees, "share one currency");

	negative_fees = listing_new(fixture, herb, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z");
	g_object_set(negative_fees, "fees", minus, NULL);
	save_refused(fixture, negative_fees, "negative");

	listing(fixture, herb, "auction", 1, "1.2345 GOLD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_EXPIRED, 0, NULL, "0.0100 GOLD", "0.0050 GOLD");
}

/* ==========================================================================
 * The latest price
 * ========================================================================== */

/*
 * The newest observation at or before the moment, from the source asked
 * for, in this organization, not deleted. What breaks: a valuation dated
 * March reading April's price, one source's price passed off as
 * another's, a deleted mistake still valuing stock, or another
 * organization's market setting this one's prices.
 */
static void
test_latest_price(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(VentureEntity) observation = NULL;
	g_autoptr(VentureEntity) mistake = NULL;
	g_autoptr(VentureEntity) foreign = NULL;
	g_autoptr(VentureOrganization) other = NULL;
	g_autoptr(GDateTime) march = NULL;
	g_autoptr(GDateTime) early = NULL;
	g_autofree gchar *shown = NULL;
	gint64 herb;
	gint64 mistake_id;
	gint64 tied;

	(void)user_data;

	herb = product(fixture, "Herb", 0);
	observe(fixture, herb, "market value", "1.00 USD", "2026-03-01T00:00:00Z", 0);
	observe(fixture, herb, "market value", "2.00 USD", "2026-03-10T00:00:00Z", 0);
	observe(fixture, herb, "region average", "3.00 USD", "2026-03-12T00:00:00Z", 0);
	observe(fixture, herb, "market value", "9.00 USD", "2026-04-01T00:00:00Z", 0);

#define LATEST(source, at, expected) G_STMT_START { \
	g_clear_pointer(&price, venture_money_free); \
	g_clear_pointer(&shown, g_free); \
	g_assert_true(venture_market_latest_price(fixture->database, \
		fixture->organization_id, herb, source, at, &price, NULL, &error)); \
	g_assert_no_error(error); \
	g_assert_nonnull(price); \
	shown = venture_money_to_string(price); \
	g_assert_cmpstr(shown, ==, expected); \
} G_STMT_END

	march = time_of("2026-03-15T00:00:00Z");
	LATEST("market value", march, "2.00 USD");
	LATEST(NULL, march, "3.00 USD");
	LATEST("", march, "3.00 USD");
	LATEST(NULL, NULL, "9.00 USD");

	/* Nothing seen yet is an answer, not an error. */
	early = time_of("2026-02-01T00:00:00Z");
	g_clear_pointer(&price, venture_money_free);
	g_assert_true(venture_market_latest_price(fixture->database,
		fixture->organization_id, herb, NULL, early, &price, &observation, &error));
	g_assert_no_error(error);
	g_assert_null(price);
	g_assert_null(observation);

	/* A deleted observation values nothing. */
	mistake_id = observe(fixture, herb, "market value", "500.00 USD",
	                     "2026-03-14T00:00:00Z", 0);
	LATEST("market value", march, "500.00 USD");
	mistake = venture_database_get(fixture->database, VENTURE_TYPE_PRICE_OBSERVATION,
	                               mistake_id, NULL);
	g_assert_true(venture_database_delete(fixture->database, mistake, NULL, &error));
	g_assert_no_error(error);
	LATEST("market value", march, "2.00 USD");

	/* Another organization's observation is not this one's price. */
	other = venture_organization_new();
	g_object_set(other, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, other);
	foreign = observation_new(fixture, ID(other), herb, "market value",
	                          "700.00 USD", "2026-03-14T00:00:00Z", 0);
	save(fixture, foreign);
	LATEST("market value", march, "2.00 USD");

	/* A tie on the time goes to the one recorded last, every time. */
	tied = observe(fixture, herb, "market value", "4.00 USD", "2026-03-10T00:00:00Z", 0);
	LATEST("market value", march, "4.00 USD");
	g_assert_true(venture_market_latest_price(fixture->database,
		fixture->organization_id, herb, "market value", march, NULL,
		&observation, &error));
	g_assert_nonnull(observation);
	g_assert_cmpint(venture_entity_get_id(observation), ==, tied);

#undef LATEST
}

/* ==========================================================================
 * Listing performance
 * ========================================================================== */

/*
 * Sale rate is units sold on closed listings over units on closed
 * listings: open ones wait, a partial one counts its sold units as sold
 * and the rest as unsold, a cancelled one as unsold. What breaks: an open
 * listing counted as a failure makes a busy week look like a bad one, and
 * a partial one counted whole makes it look like a good one.
 */
static void
test_listing_performance_sale_rate(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	gint64 herb;

	(void)user_data;

	herb = product(fixture, "Herb", 0);

	/* Sold 1 of 1 in two days. */
	listing(fixture, herb, "auction", 1, "10.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-03T00:00:00Z", NULL, "0.50 USD");
	/* Sold 3 of 10, the rest unsold; its deposit came back. */
	listing(fixture, herb, "auction", 10, "4.00 USD", "2026-03-02T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_PARTIAL, 3, "2026-03-05T00:00:00Z", "1.00 USD", "0.25 USD");
	/* Nothing sold, deposit lost. */
	listing(fixture, herb, "auction", 5, "4.00 USD", "2026-03-02T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_EXPIRED, 0, "2026-03-04T00:00:00Z", "0.40 USD", NULL);
	/* Withdrawn, deposit lost. */
	listing(fixture, herb, "auction", 2, "4.00 USD", "2026-03-02T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_CANCELLED, 0, "2026-03-03T00:00:00Z", "0.10 USD", NULL);
	/* Still on offer, one sold so far: out of the rate. */
	listing(fixture, herb, "auction", 4, "5.00 USD", "2026-03-06T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_OPEN, 1, NULL, NULL, NULL);
	/* Opened outside the period: not in it at all. */
	listing(fixture, herb, "auction", 7, "5.00 USD", "2026-04-06T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_EXPIRED, 0, NULL, NULL, NULL);

	result = RUN(fixture, "listing_performance", "2026-03", "group_by", "product");
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);
	g_assert_cmpstr(text(result, 0, "group"), ==, "Herb");
	g_assert_cmpstr(text(result, 0, "currency"), ==, "USD");
	g_assert_cmpfloat(number(result, 0, "listings"), ==, 5);
	g_assert_cmpfloat(number(result, 0, "units_listed"), ==, 22);
	g_assert_cmpfloat(number(result, 0, "units_sold"), ==, 5);
	g_assert_cmpfloat(number(result, 0, "units_closed"), ==, 18);
	g_assert_cmpfloat(number(result, 0, "sold"), ==, 1);
	g_assert_cmpfloat(number(result, 0, "partial"), ==, 1);
	g_assert_cmpfloat(number(result, 0, "expired"), ==, 1);
	g_assert_cmpfloat(number(result, 0, "cancelled"), ==, 1);
	g_assert_cmpfloat(number(result, 0, "open"), ==, 1);

	/* (1 + 3) sold of (1 + 10 + 5 + 2) closed. */
	g_assert_cmpfloat_with_epsilon(number(result, 0, "sale_rate"), 4.0 / 18.0, 1e-9);

	/* Only the listing that sold out: two days. */
	g_assert_cmpfloat_with_epsilon(number(result, 0, "avg_days_to_sell"), 2.0, 1e-9);

	/* 10.00 + 3 x 4.00 + 5.00 = 27.00 over 5 units = 5.40. */
	ASSERT_MONEY(result, 0, "sold_value", "27.00 USD");
	ASSERT_MONEY(result, 0, "avg_unit_price", "5.40 USD");
	ASSERT_MONEY(result, 0, "deposits_lost", "0.50 USD");
	ASSERT_MONEY(result, 0, "fees", "0.75 USD");
}

/*
 * The average sold unit price is one division rounded half to even, and
 * a group with no closed listings has no rate rather than 0%. What
 * breaks: a price that drifts by a cent depending on the order rows were
 * added, and "0% sold" on listings that simply have not ended.
 */
static void
test_listing_performance_rounding_and_empty_rate(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	gint64 ore;
	gint64 herb;
	guint row;

	(void)user_data;

	ore = product(fixture, "Ore", 0);
	herb = product(fixture, "Herb", 0);

	/* 0.01 + 0.02 = 0.03 over 2 units = 0.015, half to even: 0.02. */
	listing(fixture, ore, "auction", 1, "0.01 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-02T00:00:00Z", NULL, NULL);
	listing(fixture, ore, "auction", 1, "0.02 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-02T00:00:00Z", NULL, NULL);
	listing(fixture, herb, "auction", 3, "1.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_OPEN, 0, NULL, NULL, NULL);

	result = RUN(fixture, "listing_performance", "2026-03", "group_by", "product");
	row = row_where(result, "group", "Ore");
	ASSERT_MONEY(result, row, "avg_unit_price", "0.02 USD");

	row = row_where(result, "group", "Herb");
	g_assert_null(venture_report_result_get_cell(result, row, "sale_rate"));
	g_assert_null(venture_report_result_get_cell(result, row, "avg_unit_price"));
}

/*
 * Gold and dollars are two rows of one group, and a group's counts are
 * its currency's. What breaks: a sold value in neither currency.
 */
static void
test_listing_performance_currencies(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) result = NULL;
	gint64 herb;
	guint gold;
	guint usd;

	(void)user_data;

	herb = product(fixture, "Herb", 0);

	listing(fixture, herb, "auction", 2, "1.5000 GOLD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-02T00:00:00Z", NULL, NULL);
	listing(fixture, herb, "shop", 1, "3.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_EXPIRED, 0, "2026-03-02T00:00:00Z", "0.20 USD", NULL);

	result = RUN(fixture, "listing_performance", "2026-03", "group_by", "product");
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);

	gold = row_where(result, "currency", "GOLD");
	usd = row_where(result, "currency", "USD");
	g_assert_cmpstr(text(result, gold, "group"), ==, "Herb");
	g_assert_cmpstr(text(result, usd, "group"), ==, "Herb");
	ASSERT_MONEY(result, gold, "sold_value", "3.0000 GOLD");
	g_assert_cmpfloat_with_epsilon(number(result, gold, "sale_rate"), 1.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(number(result, usd, "sale_rate"), 0.0, 1e-9);
	ASSERT_MONEY(result, usd, "deposits_lost", "0.20 USD");
	g_assert_null(venture_report_result_get_cell(result, gold, "deposits_lost"));

	/* By channel, the two currencies were two channels anyway. */
	g_clear_pointer(&result, g_object_unref);
	result = RUN(fixture, "listing_performance", "2026-03", "group_by", "channel");
	g_assert_cmpstr(text(result, row_where(result, "group", "auction"), "currency"), ==, "GOLD");
	g_assert_cmpstr(text(result, row_where(result, "group", "shop"), "currency"), ==, "USD");
}

/*
 * Category groups are the product's category by path, rolled up by
 * category_depth; venture_id narrows by the product's venture. What
 * breaks: herbs and ore reported apart when the question was materials,
 * or another venture's listings in this one's rate.
 */
static void
test_listing_performance_category(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) leaves = NULL;
	g_autoptr(VentureReportResult) top = NULL;
	g_autoptr(VentureReportResult) narrowed = NULL;
	g_autoptr(VentureVenture) second = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) refused = NULL;
	g_autofree gchar *venture_text = NULL;
	gint64 materials;
	gint64 herbs;
	gint64 ore;
	gint64 herb;
	gint64 iron;
	gint64 loose;
	gint64 other;
	guint row;

	(void)user_data;

	materials = category(fixture, "Materials", 0);
	herbs = category(fixture, "Herbs", materials);
	ore = category(fixture, "Ore", materials);
	herb = product(fixture, "Peacebloom", herbs);
	iron = product(fixture, "Iron", ore);
	loose = product(fixture, "Trinket", 0);

	second = venture_venture_new();
	g_object_set(second, "name", "Other stall", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(second), fixture->organization_id);
	save(fixture, second);
	other = product_in(fixture, 0, ID(second), "Silverleaf", herbs);

	listing(fixture, herb, "auction", 2, "1.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-02T00:00:00Z", NULL, NULL);
	listing(fixture, iron, "auction", 2, "1.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_EXPIRED, 0, "2026-03-02T00:00:00Z", NULL, NULL);
	listing(fixture, loose, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-02T00:00:00Z", NULL, NULL);
	listing(fixture, other, "auction", 4, "1.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-02T00:00:00Z", NULL, NULL);

	leaves = RUN(fixture, "listing_performance", "2026-03", "group_by", "category");
	g_assert_cmpuint(venture_report_result_get_row_count(leaves), ==, 3);
	row = row_where(leaves, "group", "Materials / Herbs");
	g_assert_cmpfloat(number(leaves, row, "units_listed"), ==, 6);
	row = row_where(leaves, "group", "Materials / Ore");
	g_assert_cmpfloat(number(leaves, row, "units_listed"), ==, 2);
	row_where(leaves, "group", "Uncategorised");

	top = RUN(fixture, "listing_performance", "2026-03", "group_by", "category",
	          "category_depth", "0");
	g_assert_cmpuint(venture_report_result_get_row_count(top), ==, 2);
	row = row_where(top, "group", "Materials");
	g_assert_cmpfloat(number(top, row, "units_listed"), ==, 8);
	g_assert_cmpfloat_with_epsilon(number(top, row, "sale_rate"), 6.0 / 8.0, 1e-9);

	venture_text = g_strdup_printf("%" G_GINT64_FORMAT, fixture->venture_id);
	narrowed = RUN(fixture, "listing_performance", "2026-03", "group_by", "category",
	               "category_depth", "0", "venture_id", venture_text);
	row = row_where(narrowed, "group", "Materials");
	g_assert_cmpfloat(number(narrowed, row, "units_listed"), ==, 4);

	/* A depth with nothing to roll up is refused, not ignored. */
	refused = run(fixture, "listing_performance", "2026-03", &error,
	              "group_by", "channel", "category_depth", "0", NULL);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	refused = run(fixture, "listing_performance", "2026-03", &error,
	              "group_by", "colour", NULL);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * Another organization's listings are not this one's. What breaks: two
 * businesses on one install reading each other's sale rate.
 */
static void
test_listing_performance_organization(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) mine = NULL;
	g_autoptr(VentureReportResult) theirs = NULL;
	g_autoptr(VentureOrganization) other = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autoptr(VentureEntity) foreign = NULL;
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(GDateTime) at = NULL;
	g_autofree gchar *other_id = NULL;
	gint64 herb;
	gint64 theirs_product;

	(void)user_data;

	herb = product(fixture, "Herb", 0);
	listing(fixture, herb, "auction", 1, "1.00 USD", "2026-03-01T00:00:00Z",
	        VENTURE_LISTING_OUTCOME_SOLD, 0, "2026-03-02T00:00:00Z", NULL, NULL);

	other = venture_organization_new();
	g_object_set(other, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, other);
	venture = venture_venture_new();
	g_object_set(venture, "name", "Their stall", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), ID(other));
	save(fixture, venture);
	theirs_product = product_in(fixture, ID(other), ID(venture), "Their herb", 0);

	foreign = VENTURE_ENTITY(venture_listing_new());
	price = money_of("9.00 USD");
	at = time_of("2026-03-01T00:00:00Z");
	g_object_set(foreign, "product-id", theirs_product, "channel", "auction",
	             "quantity", (gint64)9, "unit-price", price, "listed-at", at, NULL);
	venture_entity_set_organization_id(foreign, ID(other));
	save(fixture, foreign);

	mine = RUN(fixture, "listing_performance", "2026-03", "group_by", "product");
	g_assert_cmpuint(venture_report_result_get_row_count(mine), ==, 1);
	g_assert_cmpfloat(number(mine, 0, "units_listed"), ==, 1);

	other_id = g_strdup_printf("%" G_GINT64_FORMAT, ID(other));
	theirs = RUN(fixture, "listing_performance", "2026-03", "organization_id", other_id);
	g_assert_cmpuint(venture_report_result_get_row_count(theirs), ==, 1);
	g_assert_cmpfloat(number(theirs, 0, "units_listed"), ==, 9);
}

/* ==========================================================================
 * Price history
 * ========================================================================== */

/*
 * Prices per UTC bucket and source: lowest, average half to even,
 * highest, volume and count, one row per currency. What breaks: a week
 * that starts on Sunday in one zone and Monday in another, an average
 * that drifts by a unit, or gold quoted in a dollar row.
 */
static void
test_price_history_buckets(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureReportResult) days = NULL;
	g_autoptr(VentureReportResult) weeks = NULL;
	g_autoptr(VentureReportResult) months = NULL;
	g_autoptr(VentureReportResult) one_source = NULL;
	g_autofree gchar *herb_text = NULL;
	gint64 herb;
	guint row;

	(void)user_data;

	herb = product(fixture, "Herb", 0);
	herb_text = g_strdup_printf("%" G_GINT64_FORMAT, herb);

	/* Monday 2 March and Tuesday 3 March 2026: one ISO week. */
	observe(fixture, herb, "market value", "1.00 USD", "2026-03-02T01:00:00Z", 10);
	observe(fixture, herb, "market value", "2.00 USD", "2026-03-02T23:00:00Z", 5);
	observe(fixture, herb, "market value", "1.02 USD", "2026-03-03T08:00:00Z", 0);
	/* Sunday 8 March, still the same week; Monday 9 March, the next. */
	observe(fixture, herb, "market value", "3.00 USD", "2026-03-08T12:00:00Z", 1);
	observe(fixture, herb, "market value", "4.00 USD", "2026-03-09T00:00:00Z", 1);
	/* Another source, and gold. */
	observe(fixture, herb, "region average", "5.00 USD", "2026-03-02T12:00:00Z", 0);
	observe(fixture, herb, "market value", "0.1234 GOLD", "2026-03-02T12:00:00Z", 2);

	days = RUN(fixture, "price_history", "2026-03", "product_id", herb_text,
	           "bucket", "day");
	/* 2 Mar: market value USD, market value GOLD, region average USD;
	 * then 3, 8 and 9 March. */
	g_assert_cmpuint(venture_report_result_get_row_count(days), ==, 6);
	g_assert_cmpstr(text(days, 0, "bucket"), ==, "2026-03-02");
	g_assert_cmpstr(text(days, 0, "source"), ==, "market value");
	g_assert_cmpstr(text(days, 0, "currency"), ==, "GOLD");
	g_assert_cmpstr(text(days, 1, "currency"), ==, "USD");
	ASSERT_MONEY(days, 1, "min", "1.00 USD");
	ASSERT_MONEY(days, 1, "max", "2.00 USD");
	ASSERT_MONEY(days, 1, "avg", "1.50 USD");
	g_assert_cmpfloat(number(days, 1, "volume"), ==, 15);
	g_assert_cmpfloat(number(days, 1, "observations"), ==, 2);
	g_assert_cmpstr(text(days, 2, "source"), ==, "region average");

	weeks = RUN(fixture, "price_history", "2026-03", "product_id", herb_text,
	            "bucket", "week", "source", "market value");
	g_assert_cmpuint(venture_report_result_get_row_count(weeks), ==, 3);
	row = 1;
	g_assert_cmpstr(text(weeks, row, "bucket"), ==, "2026-W10");
	g_assert_cmpstr(text(weeks, row, "starts_on"), ==, "2026-03-02");
	g_assert_cmpstr(text(weeks, row, "currency"), ==, "USD");
	/* 1.00 + 2.00 + 1.02 + 3.00 = 7.02 over 4 = 1.755, half to even 1.76. */
	ASSERT_MONEY(weeks, row, "avg", "1.76 USD");
	g_assert_cmpfloat(number(weeks, row, "observations"), ==, 4);
	g_assert_cmpstr(text(weeks, 2, "bucket"), ==, "2026-W11");

	months = RUN(fixture, "price_history", "2026-03", "product_id", herb_text,
	             "bucket", "month");
	row = row_where(months, "source", "region average");
	g_assert_cmpstr(text(months, row, "bucket"), ==, "2026-03");

	/* A source matches exactly; the other source is not in it. */
	one_source = RUN(fixture, "price_history", "2026-03", "product_id", herb_text,
	                 "bucket", "month", "source", "region average");
	g_assert_cmpuint(venture_report_result_get_row_count(one_source), ==, 1);
	ASSERT_MONEY(one_source, 0, "avg", "5.00 USD");
}

/*
 * price_history needs a product, one of this organization's. What
 * breaks: the history of every product at once, or another
 * organization's product confirmed to exist.
 */
static void
test_price_history_refusals(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureOrganization) other = NULL;
	g_autoptr(VentureVenture) venture = NULL;
	g_autofree gchar *theirs_text = NULL;
	g_autofree gchar *herb_text = NULL;
	gint64 theirs;
	gint64 herb;

	(void)user_data;

	result = run(fixture, "price_history", "all", &error, NULL);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "product_id"));
	g_clear_error(&error);

	other = venture_organization_new();
	g_object_set(other, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(fixture, other);
	venture = venture_venture_new();
	g_object_set(venture, "name", "Their stall", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), ID(other));
	save(fixture, venture);
	theirs = product_in(fixture, ID(other), ID(venture), "Their herb", 0);
	theirs_text = g_strdup_printf("%" G_GINT64_FORMAT, theirs);

	result = run(fixture, "price_history", "all", &error, "product_id", theirs_text, NULL);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	herb = product(fixture, "Herb", 0);
	herb_text = g_strdup_printf("%" G_GINT64_FORMAT, herb);
	result = run(fixture, "price_history", "all", &error, "product_id", herb_text,
	             "bucket", "fortnight", NULL);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* ==========================================================================
 * The module
 * ========================================================================== */

/*
 * Switching market off hides its types and its reports; switching it back
 * restores both with nothing re-registered. What breaks: listings still
 * offered on an install that turned the module off, or a report that
 * vanishes for good.
 */
static void
test_module_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureEntityRegistry *types;
	VentureReportRegistry *reports;
	VentureModule *market;

	(void)user_data;

	market = venture_module_registry_lookup(venture_context_get_modules(fixture->context),
	                                        "market");
	g_assert_nonnull(market);
	g_assert_true(g_strv_contains(venture_module_get_requires(market), "sales"));
	g_assert_true(g_strv_contains(venture_module_get_reports(market), "listing_performance"));
	g_assert_true(g_strv_contains(venture_module_get_reports(market), "price_history"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(market), "listing"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(market), "price_observation"));

	types = venture_context_get_entity_registry(fixture->context);
	reports = venture_context_get_report_registry(fixture->context);

	venture_config_set_module_enabled(fixture->config, "market", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "market"));
	g_assert_true(venture_context_module_enabled(fixture->context, "sales"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "listing"));
	g_assert_true(G_TYPE_INVALID == venture_entity_registry_lookup(types, "price_observation"));
	g_assert_null(venture_report_registry_lookup(reports, "listing_performance"));
	g_assert_null(venture_report_registry_lookup(reports, "price_history"));

	/* Sales off takes market with it: it requires sales. */
	venture_config_set_module_enabled(fixture->config, "market", TRUE);
	g_assert_true(venture_context_module_enabled(fixture->context, "market"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "listing_performance"));
	venture_config_set_module_enabled(fixture->config, "sales", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "market"));
	g_assert_null(venture_report_registry_lookup(reports, "price_history"));

	venture_config_set_module_enabled(fixture->config, "sales", TRUE);
	g_assert_true(venture_context_module_enabled(fixture->context, "market"));
	g_assert_true(VENTURE_TYPE_LISTING == venture_entity_registry_lookup(types, "listing"));
	g_assert_nonnull(venture_report_registry_lookup(reports, "price_history"));
}

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
server_request_full(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gboolean	  with_cookie,
	gchar		**out_body,
	gchar		**out_location
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (with_cookie && (NULL != fixture->cookie))
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

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static guint
server_get(
	ServerFixture	 *fixture,
	const gchar	 *path,
	gchar		**out_body
){
	return server_request_full(fixture, "GET", path, NULL, NULL, TRUE,
	                           out_body, NULL);
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

	g_setenv("VENTURE_TEST_SESSION_SECRET", "market-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-market-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 7937) % 20000));

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


/*
 * Every door forwards the market options. The API is where the CLI, the
 * MCP tool and the page all arrive; product_id or source missing from its
 * lists would be silently absent, and price_history would refuse (no
 * product) or answer for every source. The page, asked with no product,
 * offers the picker above the refusal rather than a dead end.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) herb = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonArray *rows;
	gint64 organization_id;
	guint i;

	(void)user_data;

	organization_id = venture_context_get_default_organization_id(fixture->context);
	herb = VENTURE_ENTITY(venture_product_new());
	g_object_set(herb, "name", "Peacebloom", NULL);
	venture_entity_set_organization_id(herb, organization_id);
	g_assert_true(venture_database_save(fixture->database, herb, NULL, NULL));

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureEntity) seen = NULL;
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(GDateTime) when = NULL;

		price = venture_money_new(100 * (i + 1), "USD", 2);
		when = g_date_time_new_utc(2026, 3, 2 + i, 12, 0, 0);
		seen = VENTURE_ENTITY(venture_price_observation_new());
		g_object_set(seen, "product-id", ID(herb),
		             "source", (i < 2) ? "market value" : "supplier",
		             "price", price, "observed-at", when, NULL);
		venture_entity_set_organization_id(seen, organization_id);
		g_assert_true(venture_database_save(fixture->database, seen, NULL, NULL));
	}

	path = g_strdup_printf("/api/v1/reports/price_history?period=2026-03"
	                       "&product_id=%" G_GINT64_FORMAT
	                       "&source=market%%20value&bucket=month", ID(herb));
	g_assert_cmpuint(server_get(fixture, path, &body), ==, SOUP_STATUS_OK);
	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	rows = json_object_get_array_member(json_node_get_object(node), "rows");
	g_assert_cmpuint(json_array_get_length(rows), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "source"), ==, "market value");

	/* Without a product: a 400 naming it, and on the page, the picker. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture, "/api/v1/reports/price_history?period=all",
	                            &body), ==, 400);
	g_assert_nonnull(strstr(body, "product_id"));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture, "/reports/price_history", &body), ==, 400);
	g_assert_nonnull(strstr(body, "name=\"product_id\""));
	g_assert_nonnull(strstr(body, "Peacebloom"));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_get(fixture,
		"/reports/listing_performance?period=all&group_by=channel", &body),
		==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "name=\"group_by\""));
	g_assert_nonnull(strstr(body, "Sale rate"));
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/market/observation/validators", test_observation_validators);
	ADD("/market/listing/quantities", test_listing_quantities);
	ADD("/market/display-names/denominated", test_display_names_denominated);
	ADD("/market/listing/outcome-and-count", test_listing_outcome_and_count);
	ADD("/market/listing/closed-at", test_listing_closed_at);
	ADD("/market/listing/one-currency", test_listing_one_currency);
	ADD("/market/latest-price", test_latest_price);
	ADD("/market/listing-performance/sale-rate", test_listing_performance_sale_rate);
	ADD("/market/listing-performance/rounding-and-empty-rate",
	    test_listing_performance_rounding_and_empty_rate);
	ADD("/market/listing-performance/currencies", test_listing_performance_currencies);
	ADD("/market/listing-performance/category", test_listing_performance_category);
	ADD("/market/listing-performance/organization", test_listing_performance_organization);
	ADD("/market/price-history/buckets", test_price_history_buckets);
	ADD("/market/price-history/refusals", test_price_history_refusals);
	ADD("/market/module-off", test_module_off);
	g_test_add("/market/http", ServerFixture, NULL, server_fixture_set_up,
	           test_http, server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
