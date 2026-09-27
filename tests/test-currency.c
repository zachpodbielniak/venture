/*
 * test-currency.c - User-defined currency records and the registry
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A currency record is a promise about every amount stored in it: what its
 * code is, how many decimals it has and how it reads. These tests hold the
 * record to the rules the money type can honour, and hold the registry --
 * what every formatter actually consults -- to the table: a save shows up
 * at once, a rollback takes it back out, and a delete changes nothing
 * about how the amounts already in it display.
 */

#include <venture.h>

#include <string.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
} Fixture;

static const gchar *const gold_denominations =
	"[{\"suffix\":\"g\",\"units\":10000},"
	"{\"suffix\":\"s\",\"units\":100},"
	"{\"suffix\":\"c\",\"units\":1}]";

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	venture_currency_clear_registered();

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_context_get_default_organization_id(fixture->context);
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

	/* The registry is process-wide; the next test must not inherit it. */
	venture_currency_clear_registered();
}

/* A currency record filed under the default organization, unsaved. */
static VentureEntity *
currency_new(
	Fixture		*fixture,
	const gchar	*code,
	gint64		 exponent,
	const gchar	*denominations
){
	VentureEntity *currency;

	currency = VENTURE_ENTITY(venture_currency_new());
	g_object_set(currency, "code", code, "name", code, "exponent", exponent,
	             "denominations", denominations, NULL);
	venture_entity_set_organization_id(currency, fixture->organization_id);

	return currency;
}

/* Saves a currency and expects the save to be refused as a validation. */
static void
assert_refused(
	Fixture		*fixture,
	VentureEntity	*currency,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;

	g_assert_false(venture_database_save(fixture->database, currency, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

/*
 * Each rule the validator enforces, on the ordinary save path every writer
 * shares. What breaks if one regresses: a row saves that the registry then
 * refuses to load, and its amounts quietly display as a two-decimal code.
 */
static void
test_record_refusals(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	{
		g_autoptr(VentureEntity) c = currency_new(fixture, "G", 2, NULL);
		assert_refused(fixture, c, "2 to 15 characters");
	}
	{
		g_autoptr(VentureEntity) c = currency_new(fixture, "9LIVES", 2, NULL);
		assert_refused(fixture, c, "2 to 15 characters");
	}
	{
		g_autoptr(VentureEntity) c = currency_new(fixture, "ABCDEFGHIJKLMNOP", 2, NULL);
		assert_refused(fixture, c, "2 to 15 characters");
	}
	{
		/* Redefining a dollar would restate every figure in the books. */
		g_autoptr(VentureEntity) c = currency_new(fixture, "usd", 2, NULL);
		assert_refused(fixture, c, "built-in ISO 4217");
	}
	{
		g_autoptr(VentureEntity) c = currency_new(fixture, "GEMS", 7, NULL);
		assert_refused(fixture, c, "0 to 6");
	}
	{
		g_autoptr(VentureEntity) c = currency_new(fixture, "GEMS", -1, NULL);
		assert_refused(fixture, c, "0 to 6");
	}
	{
		/* 100 does not divide 150: a gold could not be written in
		 * silver exactly. */
		g_autoptr(VentureEntity) c = currency_new(fixture, "GEMS", 4,
			"[{\"suffix\":\"g\",\"units\":150},{\"suffix\":\"s\",\"units\":100},"
			"{\"suffix\":\"c\",\"units\":1}]");
		assert_refused(fixture, c, "divide");
	}
	{
		g_autoptr(VentureEntity) c = currency_new(fixture, "GEMS", 2, "not json");
		assert_refused(fixture, c, "JSON");
	}
	{
		g_autoptr(VentureEntity) c = currency_new(fixture, "GEMS", 2, NULL);
		g_object_set(c, "symbol", "a symbol that is far too long", NULL);
		assert_refused(fixture, c, "symbol");
	}

	/* None of them reached the table or the registry. */
	g_assert_false(venture_currency_is_registered("GEMS"));
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CURRENCY);
		g_assert_cmpint(venture_database_count(fixture->database, query, NULL), ==, 0);
	}
}

/*
 * The code is stored in the spelling amounts use and is unique across the
 * install; code and exponent are fixed once saved. What breaks if this
 * regresses: "gold" and "GOLD" become two currencies, or an exponent edit
 * makes the same typed "5" mean a different amount tomorrow.
 */
static void
test_record_identity(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) gold = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autofree gchar *code = NULL;

	(void)user_data;

	gold = currency_new(fixture, " gold ", 4, gold_denominations);
	g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);
	g_object_get(gold, "code", &code, NULL);
	g_assert_cmpstr(code, ==, "GOLD");

	/* Unique across the install, whatever the spelling. */
	again = currency_new(fixture, "Gold", 2, NULL);
	g_assert_false(venture_database_save(fixture->database, again, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_object_set(gold, "exponent", (gint64)2, NULL);
	assert_refused(fixture, gold, "decimal places cannot change");
	g_object_set(gold, "exponent", (gint64)4, "code", "SILVER", NULL);
	assert_refused(fixture, gold, "code cannot change");

	/* Everything else may be edited. */
	g_object_set(gold, "code", "GOLD", "symbol", "gp", "description", "The good stuff", NULL);
	g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);
}

/*
 * A saved currency is in the registry before the save returns, and an
 * amount in it reads in its own coins -- also when it came in through the
 * generic field parser every form and API write uses.
 */
static void
test_registry_follows_saves(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) gold = NULL;
	g_autoptr(VentureEntity) points = NULL;
	g_autoptr(VentureEntity) venture = NULL;
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureMoney) gross = NULL;
	g_autofree gchar *display = NULL;

	(void)user_data;

	g_assert_false(venture_currency_is_registered("GOLD"));

	gold = currency_new(fixture, "GOLD", 4, gold_denominations);
	g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);

	g_assert_true(venture_currency_is_registered("GOLD"));
	g_assert_cmpuint(venture_currency_get_exponent("GOLD"), ==, 4);

	/* A sale priced in coins, through the parser forms and the API use. */
	venture = VENTURE_ENTITY(venture_venture_new());
	g_object_set(venture, "name", "Auction house", NULL);
	venture_entity_set_organization_id(venture, fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database, venture, NULL, &error));
	g_assert_no_error(error);

	sale = VENTURE_ENTITY(venture_sale_new());
	g_object_set(sale, "venture-id", venture_entity_get_id(venture), NULL);
	venture_entity_set_organization_id(sale, fixture->organization_id);
	g_assert_true(venture_entity_set_field_from_string(sale, "gross", "12g 34s 56c", &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database, sale, NULL, &error));
	g_assert_no_error(error);

	stored = venture_database_get(fixture->database, VENTURE_TYPE_SALE,
	                              venture_entity_get_id(sale), &error);
	g_assert_no_error(error);
	g_object_get(stored, "gross", &gross, NULL);
	g_assert_cmpstr(venture_money_get_currency(gross), ==, "GOLD");
	g_assert_cmpint(venture_money_get_amount(gross), ==, 123456);
	display = venture_money_to_display_string(gross, TRUE);
	g_assert_cmpstr(display, ==, "12g 34s 56c");

	/* An edit to the symbol is live at once too. */
	points = currency_new(fixture, "POINTS", 0, NULL);
	g_object_set(points, "symbol", "pts", "symbol-position",
	             VENTURE_SYMBOL_POSITION_SUFFIX, "kind", VENTURE_CURRENCY_KIND_POINTS, NULL);
	g_assert_true(venture_database_save(fixture->database, points, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureMoney) amount = venture_money_new(1500, "POINTS", 0);
		g_autofree gchar *text = venture_money_to_display_string(amount, TRUE);
		g_assert_cmpstr(text, ==, "1,500 pts");
	}
	g_object_set(points, "symbol", "P", "symbol-position", VENTURE_SYMBOL_POSITION_PREFIX, NULL);
	g_assert_true(venture_database_save(fixture->database, points, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureMoney) amount = venture_money_new(1500, "POINTS", 0);
		g_autofree gchar *text = venture_money_to_display_string(amount, TRUE);
		g_assert_cmpstr(text, ==, "P1,500");
	}
}

/*
 * Deleting a currency record must not change how the amounts stored in it
 * display: the registry loads soft-deleted rows as well. What breaks if
 * this regresses: deleting GOLD turns every historical sale from
 * "12g 34s 56c" into "12.3456 GOLD" -- and a restart would do the same.
 */
static void
test_deleted_currency_still_formats(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) gold = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *reloaded = NULL;

	(void)user_data;

	gold = currency_new(fixture, "GOLD", 4, gold_denominations);
	g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);

	amount = venture_money_new(123456, "GOLD", 4);
	before = venture_money_to_display_string(amount, FALSE);

	g_assert_true(venture_database_delete(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_currency_is_registered("GOLD"));
	after = venture_money_to_display_string(amount, FALSE);
	g_assert_cmpstr(after, ==, before);

	/* And from cold, as a restart would load it. */
	venture_currency_clear_registered();
	g_assert_true(venture_currency_load_registry(fixture->database, &error));
	g_assert_no_error(error);
	reloaded = venture_money_to_display_string(amount, FALSE);
	g_assert_cmpstr(reloaded, ==, "12g 34s 56c");
}

/*
 * A currency saved inside a transaction that rolls back is taken back out
 * of the registry; one that commits stays. What breaks if this regresses:
 * a refused import leaves a currency formatting amounts that were never
 * written.
 */
static void
test_rollback_unregisters(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) gems = NULL;
	g_autoptr(VentureEntity) kept = NULL;

	(void)user_data;

	g_assert_true(venture_database_begin(fixture->database, &error));
	gems = currency_new(fixture, "GEMS", 2, NULL);
	g_assert_true(venture_database_save(fixture->database, gems, NULL, &error));
	g_assert_no_error(error);
	/* Visible to the rest of the transaction... */
	g_assert_true(venture_currency_is_registered("GEMS"));
	venture_database_rollback(fixture->database);
	/* ...and gone with it. */
	g_assert_false(venture_currency_is_registered("GEMS"));

	g_assert_true(venture_database_begin(fixture->database, &error));
	kept = currency_new(fixture, "RUBIES", 0, NULL);
	g_assert_true(venture_database_save(fixture->database, kept, NULL, &error));
	g_assert_true(venture_database_commit(fixture->database, &error));
	g_assert_no_error(error);
	g_assert_true(venture_currency_is_registered("RUBIES"));
}

/*
 * The registry is loaded once the schema exists. A server builds its
 * context before it migrates, so the context's own load finds no table on
 * a fresh install; the migration loads it. A scratch database migrated to
 * check a backup has no context and must leave the live registry alone.
 */
static void
test_loaded_after_migration(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) gold = NULL;
	g_autoptr(VentureDatabase) scratch = NULL;

	(void)user_data;

	gold = currency_new(fixture, "GOLD", 4, gold_denominations);
	g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);

	venture_currency_clear_registered();
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_currency_is_registered("GOLD"));

	scratch = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(scratch,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_true(venture_currency_is_registered("GOLD"));
}

/*
 * Exchange rates already work between any valid codes, so valuing gold in
 * dollars needs nothing new: a rate row and the conversion. Pinned so a
 * later "ISO only" check in the rate table would fail here.
 */
static void
test_exchange_rate_into_usd(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) gold = NULL;
	g_autoptr(VentureMoney) hoard = NULL;
	g_autoptr(VentureMoney) dollars = NULL;

	(void)user_data;

	gold = currency_new(fixture, "GOLD", 4, gold_denominations);
	g_assert_true(venture_database_save(fixture->database, gold, NULL, &error));
	g_assert_no_error(error);

	/* 1000 gold (10,000,000 copper) at $15 per 1000 gold. */
	hoard = venture_money_from_string("1000g", NULL, &error);
	g_assert_no_error(error);
	dollars = venture_money_convert_at_rate(hoard, 15, 1000, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_money_get_currency(dollars), ==, "USD");
	g_assert_cmpint(venture_money_get_amount(dollars), ==, 1500);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/currency/record-refusals", Fixture, NULL,
	           fixture_set_up, test_record_refusals, fixture_tear_down);
	g_test_add("/currency/record-identity", Fixture, NULL,
	           fixture_set_up, test_record_identity, fixture_tear_down);
	g_test_add("/currency/registry-follows-saves", Fixture, NULL,
	           fixture_set_up, test_registry_follows_saves, fixture_tear_down);
	g_test_add("/currency/deleted-still-formats", Fixture, NULL,
	           fixture_set_up, test_deleted_currency_still_formats, fixture_tear_down);
	g_test_add("/currency/rollback-unregisters", Fixture, NULL,
	           fixture_set_up, test_rollback_unregisters, fixture_tear_down);
	g_test_add("/currency/loaded-after-migration", Fixture, NULL,
	           fixture_set_up, test_loaded_after_migration, fixture_tear_down);
	g_test_add("/currency/exchange-rate-into-usd", Fixture, NULL,
	           fixture_set_up, test_exchange_rate_into_usd, fixture_tear_down);

	return g_test_run();
}
