/*
 * test-external-books.c - An external ledger in the books
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A push source's external ledger (sales after the venue's cut, purchases,
 * other income and expenses, per account) reaches the general journal by
 * one of two doors: a summary journal per account per day (post_ledger,
 * `books: daily`), or each sale matched to its buys as a closed arbitrage
 * trade (record_flips, `books: trades`). These tests push real JSON lines
 * through the feeds worker and read the journals back, holding what a
 * plausible implementation gets wrong: a copper lost to rounding, a day
 * posted twice, a changed day left as it was, a purse taken below zero by
 * gold an alt mailed it, a currency added into another, a sale booked as
 * both a day's sales and a trade, and a posting nobody approved.
 */

#include <venture.h>
#include <string.h>

#include "venture-test-util.h"

#ifdef VENTURE_HAVE_SQLITE

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))
#define GOLD(whole, copper) ((gint64)(whole) * 10000 + (copper))

typedef struct
{
	gchar			*state_dir;
	VentureConfig		*config;
	VentureDatabase		*db;
	VentureContext		*context;
	gint64			 org;
} Fixture;

/* --- Helpers ------------------------------------------------------------------- */

static void
save(
	Fixture		*f,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

static void
field(
	gpointer	 record,
	const gchar	*name,
	const gchar	*value
){
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
reread(
	Fixture	*f,
	GType	 type,
	gint64	 id
){
	g_autoptr(GError) error = NULL;
	VentureEntity *record;

	record = venture_database_get(f->db, type, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(record);

	return record;
}

static gint64
int_of(
	VentureEntity	*record,
	const gchar	*property
){
	gint64 value = 0;

	g_object_get(record, property, &value, NULL);

	return value;
}

static void
define_currency(
	Fixture		*f,
	const gchar	*code,
	gint64		 exponent,
	const gchar	*treatment
){
	g_autoptr(VentureEntity) currency = VENTURE_ENTITY(venture_currency_new());

	venture_entity_set_organization_id(currency, f->org);
	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", treatment);
	save(f, currency);
}

/* Lets runs come back until nothing is pending, bounded. */
static void
settle(Fixture *f)
{
	VentureFeedsService *service;
	gint64 deadline;

	service = venture_context_get_feeds_service(f->context);

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

	/* The hooks defer while a transaction is open; let their timers fire. */
	deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

	while (g_get_monotonic_time() < deadline)
	{
		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}
}

/* A push source in @org, in @currency, with @settings (YAML). */
static VentureEntity *
push_source(
	Fixture		*f,
	gint64		 org,
	const gchar	*name,
	const gchar	*currency,
	const gchar	*settings
){
	VentureEntity *source;

	source = VENTURE_ENTITY(venture_data_source_new());
	venture_entity_set_organization_id(source, org);
	g_object_set(source, "name", name, "provider", "push", "settings", settings,
	             "schedule", "manual", "currency", currency, "instrument-namespace", "wow-item",
	             "venue-namespace", "wow-realm", NULL);
	save(f, source);

	return source;
}

static void
set_settings(
	Fixture		*f,
	VentureEntity	*source,
	const gchar	*settings
){
	g_object_set(source, "settings", settings, NULL);
	save(f, source);
}

/* Pushes @text and waits, bounded, for its run (the hooks run inside). */
static VentureEntity *
push(
	Fixture		*f,
	VentureEntity	*source,
	const gchar	*text
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *push_id = NULL;
	VentureFeedsService *service;
	VentureEntity *run;
	gint64 run_id = 0;
	VentureDataSourceRunStatus status;

	service = venture_context_get_feeds_service(f->context);
	body = g_bytes_new(text, strlen(text));
	g_assert_true(venture_feeds_service_push(service, ID(source), body, &push_id, &error));
	g_assert_no_error(error);
	g_assert_true(venture_feeds_service_wait_push(service, push_id, 60, &run_id));
	g_assert_cmpint(run_id, >, 0);
	settle(f);

	run = reread(f, VENTURE_TYPE_DATA_SOURCE_RUN, run_id);
	g_object_get(run, "status", &status, NULL);

	if (VENTURE_DATA_SOURCE_RUN_STATUS_FAILED == status)
	{
		g_autofree gchar *message = NULL;

		g_object_get(run, "error", &message, NULL);
		g_error("push failed: %s", message);
	}

	return run;
}

/* --- JSON lines -------------------------------------------------------------- */

static void
line_market(GString *body)
{
	g_string_append(body,
		"{\"type\":\"instrument\",\"key\":\"2447\",\"name\":\"Peacebloom\",\"kind\":\"item\"}\n"
		"{\"type\":\"venue\",\"key\":\"thorium\",\"name\":\"Thorium Brotherhood\","
		"\"kind\":\"auction_house\",\"group\":\"us\"}\n"
		"{\"type\":\"venue\",\"key\":\"silvermoon\",\"name\":\"Silvermoon\","
		"\"kind\":\"auction_house\",\"group\":\"us\"}\n");
}

static void
line_account(
	GString		*body,
	const gchar	*key,
	const gchar	*venue
){
	g_string_append_printf(body, "{\"type\":\"account\",\"key\":\"%s\",\"name\":\"%.*s\","
	                       "\"kind\":\"character\",\"group\":\"us\",\"venue\":\"%s\"}\n",
	                       key, (gint)strcspn(key, "-"), key, venue);
}

static void
line_balance(
	GString		*body,
	const gchar	*account,
	const gchar	*currency,
	const gchar	*amount,
	const gchar	*at
){
	g_string_append_printf(body, "{\"type\":\"balance\",\"account\":\"%s\",\"currency\":\"%s\","
	                       "\"amount\":\"%s\",\"at\":\"%s\"}\n", account, currency, amount, at);
}

/* A ledger row; @instrument NULL for income and expenses with no item,
 * @amount NULL for an expiry or a cancellation. */
static void
line_txn(
	GString		*body,
	const gchar	*id,
	const gchar	*account,
	const gchar	*venue,
	const gchar	*kind,
	const gchar	*instrument,
	gint64		 quantity,
	const gchar	*amount,
	const gchar	*at
){
	g_string_append_printf(body, "{\"type\":\"txn\",\"id\":\"%s\",\"account\":\"%s\","
	                       "\"venue\":\"%s\",\"kind\":\"%s\"", id, account, venue, kind);

	if (NULL != instrument)
		g_string_append_printf(body, ",\"instrument\":\"%s\",\"quantity\":%" G_GINT64_FORMAT,
		                       instrument, quantity);

	if (NULL != amount)
		g_string_append_printf(body, ",\"amount\":\"%s\"", amount);

	g_string_append_printf(body, ",\"at\":\"%s\"}\n", at);
}

/* --- The books ----------------------------------------------------------------- */

static GDateTime *
day(const gchar *text)
{
	GDateTime *when = venture_time_from_string(text, NULL);

	g_assert_nonnull(when);

	return when;
}

/* post_ledger over [@from, @until) as the system; the report. */
static JsonObject *
post(
	Fixture		*f,
	VentureEntity	*source,
	const gchar	*from,
	const gchar	*until,
	const gchar	*account,
	gboolean	 dry_run,
	GError		**error
){
	g_autoptr(GDateTime) from_at = (NULL != from) ? day(from) : NULL;
	g_autoptr(GDateTime) until_at = (NULL != until) ? day(until) : NULL;
	g_autoptr(JsonNode) report = NULL;
	VentureBooksPostQuery query;

	memset(&query, 0, sizeof(query));
	query.organization_id = venture_entity_get_organization_id(source);
	query.data_source_id = ID(source);
	query.from = from_at;
	query.until = until_at;
	query.account_key = account;
	query.dry_run = dry_run;

	if (!venture_arbitrage_books_post(f->context, &query, NULL, &report, error))
		return NULL;

	return json_object_ref(json_node_get_object(report));
}

static JsonObject *
post_ok(
	Fixture		*f,
	VentureEntity	*source,
	const gchar	*from,
	const gchar	*until
){
	g_autoptr(GError) error = NULL;
	JsonObject *report;

	report = post(f, source, from, until, NULL, FALSE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);

	return report;
}

static gint64
status_count(
	JsonObject	*report,
	const gchar	*status
){
	return json_object_get_int_member_with_default(
		json_object_get_object_member(report, "days_by_status"), status, -1);
}

static JsonObject *
flips(
	Fixture		*f,
	VentureEntity	*source,
	gboolean	 dry_run,
	GError		**error
){
	g_autoptr(JsonNode) report = NULL;
	VentureBooksFlipsQuery query;

	memset(&query, 0, sizeof(query));
	query.organization_id = venture_entity_get_organization_id(source);
	query.data_source_id = ID(source);
	query.dry_run = dry_run;

	if (!venture_arbitrage_books_record_flips(f->context, &query, NULL, &report, error))
		return NULL;

	return json_object_ref(json_node_get_object(report));
}

/* Every journal naming @source, originals and reversals. */
static GPtrArray *
journals_of(
	Fixture		*f,
	VentureEntity	*source
){
	g_autoptr(GError) error = NULL;
	GPtrArray *journals;

	journals = venture_posting_service_find_source(
		venture_database_get_posting_service(f->db), "data_source", ID(source),
		venture_entity_get_organization_id(source), &error);
	g_assert_no_error(error);
	g_assert_nonnull(journals);

	return journals;
}

static guint
count_journals(
	Fixture		*f,
	VentureEntity	*source,
	gboolean	 reversals
){
	g_autoptr(GPtrArray) journals = journals_of(f, source);
	guint count = 0;
	guint i;

	for (i = 0; i < journals->len; i++)
	{
		gboolean reversal = int_of(g_ptr_array_index(journals, i), "reverses-id") > 0;

		if (reversal == reversals)
			count++;
	}

	return count;
}

/* The holding account at @account_key's place. */
static gint64
holding_of(
	Fixture		*f,
	VentureEntity	*source,
	const gchar	*account_key
){
	g_autoptr(VentureEntity) location = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = venture_marketdata_account_ref(source, account_key);
	gint64 account = 0;

	location = venture_marketdata_find_by_ref(f->db, VENTURE_TYPE_LOCATION,
	                                          venture_entity_get_organization_id(source), ref, &error);
	g_assert_no_error(error);
	g_assert_nonnull(location);
	g_assert_true(venture_holdings_account_for_location(f->db, venture_entity_get_organization_id(source),
	                                                    ID(location), TRUE, NULL, &account, &error));
	g_assert_no_error(error);

	return account;
}

static gint64
held(
	Fixture		*f,
	VentureEntity	*source,
	const gchar	*account_key,
	const gchar	*currency
){
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;

	balance = venture_holdings_balance(f->db, holding_of(f, source, account_key), currency, 0, &error);
	g_assert_no_error(error);

	return venture_money_get_amount(balance);
}

/* A classification's balance in @currency's book, debit positive; 0 when
 * the account was never made. */
static gint64
balance_of(
	Fixture		*f,
	const gchar	*classification,
	const gchar	*currency
){
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) end = day("2030-01-01");
	g_autoptr(GError) error = NULL;
	gint64 account = 0;

	g_assert_true(venture_arbitrage_find_account(f->db, f->org, classification, NULL, &account, &error));
	g_assert_no_error(error);

	if (0 == account)
		return 0;

	balance = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
	                                                  account, f->org, currency, end, &error);
	g_assert_no_error(error);

	return venture_money_get_amount(balance);
}

/* The external_posting record of @ref's end ("Drgold-Thorium:2026-03-01"). */
static VentureEntity *
posting_record(
	Fixture		*f,
	VentureEntity	*source,
	const gchar	*suffix
){
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EXTERNAL_POSTING);
	g_autofree gchar *ref = g_strdup_printf("%s:%s", venture_entity_get_uuid(source), suffix);

	venture_query_set_include_deleted(query, TRUE);
	g_assert_true(venture_query_add_filter_string(query, "ref", VENTURE_FILTER_OP_EQ, ref, NULL));

	return venture_database_find_one(f->db, query, NULL);
}

static guint
count_rows(
	Fixture	*f,
	GType	 type
){
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;

	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(rows);

	return rows->len;
}

/* --- Fixture ---------------------------------------------------------------------- */

static void
fixture_set_up(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) rate = NULL;

	(void)user_data;

	venture_currency_clear_registered();
	f->state_dir = g_dir_make_tmp("venture-external-books-XXXXXX", &error);
	g_assert_no_error(error);

	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "feeds-enabled", TRUE,
	             "feeds-run-window-minutes", (gint64)0, NULL);

	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	/* The books are in dollars. Gold is a game's money kept in a book of
	 * its own; tickets are counted and never posted; euros are valued in
	 * dollars at a recorded rate. */
	define_currency(f, "GOLD", 4, "separate_book");
	define_currency(f, "TICKET", 0, "memo");
	rate = VENTURE_ENTITY(g_object_new(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "exchange_rate"), NULL));
	venture_entity_set_organization_id(rate, f->org);
	g_object_set(rate, "from-currency", "EUR", "to-currency", "USD",
	             "rate-numerator", (gint64)11, "rate-denominator", (gint64)10, NULL);
	field(rate, "effective-at", "2026-01-01");
	save(f, rate);
}

static void
fixture_tear_down(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	settle(f);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);

	if (NULL != f->state_dir)
	{
		venture_test_remove_tree(f->state_dir);
		g_clear_pointer(&f->state_dir, g_free);
	}

	venture_currency_clear_registered();
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

/* The ledger the daily tests share: one character, a balance seen the day
 * before, a day of buying and selling with postage, a day of sales and a
 * gift, and a day of only an expiry. */
static void
drgold_ledger(GString *body)
{
	line_market(body);
	line_account(body, "Drgold-Thorium", "thorium");
	line_balance(body, "Drgold-Thorium", "GOLD", "100.0000", "2026-02-28T12:00:00Z");
	line_txn(body, "b1", "Drgold-Thorium", "thorium", "buy", "2447", 3, "30.0000", "2026-03-01T10:00:00Z");
	line_txn(body, "s1", "Drgold-Thorium", "thorium", "sale", "2447", 1, "25.5000", "2026-03-01T12:00:00Z");
	line_txn(body, "p1", "Drgold-Thorium", "thorium", "expense", NULL, 0, "0.3000", "2026-03-01T13:00:00Z");
	line_txn(body, "x1", "Drgold-Thorium", "thorium", "expired", "2447", 1, NULL, "2026-03-01T14:00:00Z");
	line_txn(body, "s2", "Drgold-Thorium", "thorium", "sale", "2447", 2, "40.0000", "2026-03-02T09:00:00Z");
	line_txn(body, "g1", "Drgold-Thorium", "thorium", "income", NULL, 0, "5.0000", "2026-03-02T10:00:00Z");
	line_txn(body, "x2", "Drgold-Thorium", "thorium", "expired", "2447", 1, NULL, "2026-03-05T14:00:00Z");
}

/* --- Daily ------------------------------------------------------------------------ */

/*
 * The days become journals: an opening from the balance the source saw,
 * then one summary per day with money, in the source's own book (gold is
 * a separate book here), to the copper; a day of only an expiry posts
 * nothing. The trading accounts and the purse agree with the ledger, and
 * every journal is keyed and balanced.
 *
 * What breaks if this regresses: a P&L off by a copper on every day, an
 * expiry booked as money, gold added into the dollar books, or a purse
 * that does not match what the character holds.
 */
static void
test_daily_journals(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonObject) report = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(VentureEntity) opening = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) quiet = NULL;
	g_autofree gchar *prefix = NULL;
	guint i;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\n");
	drgold_ledger(body);
	run = push(f, source, body->str);

	report = post_ok(f, source, NULL, "2026-03-10");
	g_assert_cmpint(status_count(report, "unposted"), ==, 3);
	g_assert_cmpint(json_object_get_int_member(report, "written"), ==, 3);

	/* The opening, then 1 and 2 March; 5 March had only an expiry. */
	g_assert_cmpuint(count_journals(f, source, FALSE), ==, 3);
	g_assert_cmpuint(count_journals(f, source, TRUE), ==, 0);
	quiet = posting_record(f, source, "Drgold-Thorium:2026-03-05");
	g_assert_null(quiet);

	journals = journals_of(f, source);
	prefix = g_strdup_printf("extledger:%s:", venture_entity_get_uuid(source));

	for (i = 0; i < journals->len; i++)
	{
		g_autofree gchar *currency = NULL;
		g_autofree gchar *key = NULL;
		g_autofree gchar *rule = NULL;

		g_object_get(g_ptr_array_index(journals, i), "currency", &currency, "posting-key", &key,
		             "rule-name", &rule, NULL);
		g_assert_cmpstr(currency, ==, "GOLD");
		g_assert_true(g_str_has_prefix(key, prefix));
		g_assert_true(g_str_has_suffix(key, ":1"));
		g_assert_true(g_str_has_prefix(rule, "external_ledger:"));
	}

	/* Opening 100; 1 March 25.5 - 30 - 0.3; 2 March 40 + 5. */
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(140, 2000));
	g_assert_cmpint(balance_of(f, "trading_sales", "GOLD"), ==, -GOLD(65, 5000));
	g_assert_cmpint(balance_of(f, "trading_purchases", "GOLD"), ==, GOLD(30, 0));
	g_assert_cmpint(balance_of(f, "trading_expenses", "GOLD"), ==, GOLD(0, 3000));
	g_assert_cmpint(balance_of(f, "trading_income", "GOLD"), ==, -GOLD(5, 0));
	g_assert_cmpint(balance_of(f, "trading_capital", "GOLD"), ==, -GOLD(100, 0));
	g_assert_cmpint(balance_of(f, "trading_sales", "USD"), ==, 0);

	opening = posting_record(f, source, "Drgold-Thorium:opening");
	g_assert_nonnull(opening);
	g_assert_cmpint(int_of(opening, "revision"), ==, 1);
	g_assert_cmpint(int_of(opening, "journal-id"), >, 0);
	first = posting_record(f, source, "Drgold-Thorium:2026-03-01");
	g_assert_nonnull(first);
	g_assert_cmpint(int_of(first, "location-id"), >, 0);

	/* Who may write the record: only the books. */
	{
		g_autoptr(GError) error = NULL;

		g_object_set(first, "revision", (gint64)7, NULL);
		g_assert_false(venture_database_save(f->db, first, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
		g_assert_false(venture_database_delete(f->db, VENTURE_ENTITY(opening), NULL, &error));
		g_assert_nonnull(error);
		g_assert_nonnull(strstr(error->message, "posted"));
	}
}

/*
 * Posting again changes nothing: the same figures post no journal, write
 * no record. A dry run on a fresh ledger says what it would do and
 * writes nothing at all -- no journal, no record, no account in the chart.
 *
 * What breaks if this regresses: every push doubles a month of sales, or
 * a preview leaves accounts and journals behind.
 */
static void
test_idempotent_and_dry_run(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonObject) dry = NULL;
	g_autoptr(JsonObject) report = NULL;
	g_autoptr(JsonObject) again = NULL;
	g_autoptr(GError) error = NULL;
	guint accounts;
	guint journals;
	guint records;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: none\n");
	drgold_ledger(body);
	run = push(f, source, body->str);
	accounts = count_rows(f, VENTURE_TYPE_ACCOUNT);
	journals = count_rows(f, VENTURE_TYPE_JOURNAL);
	records = count_rows(f, VENTURE_TYPE_EXTERNAL_POSTING);

	/* A dry run is answered whatever the mode. */
	dry = post(f, source, NULL, "2026-03-10", NULL, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(status_count(dry, "unposted"), ==, 3);
	g_assert_cmpint(json_object_get_int_member(dry, "written"), ==, 0);
	g_assert_true(json_object_get_boolean_member(dry, "dry_run"));
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ACCOUNT), ==, accounts);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, journals);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_EXTERNAL_POSTING), ==, records);

	/* Not daily: refused, naming the setting. */
	g_assert_null(post(f, source, NULL, "2026-03-10", NULL, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "books"));
	g_clear_error(&error);

	set_settings(f, source, "books: daily\n");
	report = post_ok(f, source, NULL, "2026-03-10");
	g_assert_cmpint(json_object_get_int_member(report, "written"), ==, 3);
	journals = count_rows(f, VENTURE_TYPE_JOURNAL);

	again = post_ok(f, source, NULL, "2026-03-10");
	g_assert_cmpint(json_object_get_int_member(again, "written"), ==, 0);
	g_assert_cmpint(status_count(again, "posted"), ==, 3);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, journals);
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(140, 2000));
}

/*
 * A day whose rows changed -- a sale the source merged and re-sent larger
 * under the same id -- is reversed (dated as the original) and posted
 * again, and so is every posted day after it, whose capital chain it
 * moves. The opening, unchanged, is left alone. Bringing in an earlier
 * day moves the opening to it.
 *
 * What breaks if this regresses: a merged sale is booked at its first
 * size forever, or booked twice, or a later day keeps capital the
 * corrected purse no longer needs.
 */
static void
test_changed_day(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) grown = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GString) more = g_string_new(NULL);
	g_autoptr(JsonObject) late = NULL;
	g_autoptr(JsonObject) widened = NULL;
	g_autoptr(JsonObject) changed = NULL;
	g_autoptr(VentureEntity) opening = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	guint i;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\n");
	drgold_ledger(body);
	run = push(f, source, body->str);

	/* From 2 March only: the opening is what was seen by then. */
	late = post_ok(f, source, "2026-03-02", "2026-03-10");
	g_assert_cmpint(status_count(late, "unposted"), ==, 2);
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(145, 0));

	/* Widened to 1 March: the opening moves, 2 March follows. */
	widened = post_ok(f, source, "2026-03-01", "2026-03-10");
	g_assert_cmpint(status_count(widened, "unposted"), ==, 1);
	g_assert_cmpint(status_count(widened, "changed"), ==, 1);
	g_assert_cmpint(status_count(widened, "follows"), ==, 1);
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(140, 2000));
	g_assert_cmpuint(count_journals(f, source, TRUE), ==, 2);

	/* The 1 March sale grew to two units for 51 gold. */
	line_txn(more, "s1", "Drgold-Thorium", "thorium", "sale", "2447", 2, "51.0000", "2026-03-01T12:00:00Z");
	grown = push(f, source, more->str);
	changed = post_ok(f, source, NULL, "2026-03-10");
	g_assert_cmpint(status_count(changed, "changed"), ==, 1);
	g_assert_cmpint(status_count(changed, "follows"), ==, 1);
	g_assert_cmpint(status_count(changed, "posted"), ==, 1);

	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(165, 7000));
	g_assert_cmpint(balance_of(f, "trading_sales", "GOLD"), ==, -GOLD(91, 0));
	g_assert_cmpint(balance_of(f, "trading_capital", "GOLD"), ==, -GOLD(100, 0));

	opening = posting_record(f, source, "Drgold-Thorium:opening");
	first = posting_record(f, source, "Drgold-Thorium:2026-03-01");
	second = posting_record(f, source, "Drgold-Thorium:2026-03-02");
	g_assert_cmpint(int_of(opening, "revision"), ==, 2);
	g_assert_cmpint(int_of(first, "revision"), ==, 2);
	g_assert_cmpint(int_of(second, "revision"), ==, 3);

	/* Every reversal is dated as its original. */
	journals = journals_of(f, source);

	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		g_autoptr(VentureEntity) original = NULL;
		g_autoptr(GDateTime) at = NULL;
		g_autoptr(GDateTime) original_at = NULL;

		if (int_of(journal, "reverses-id") <= 0)
			continue;

		original = reread(f, VENTURE_TYPE_JOURNAL, int_of(journal, "reverses-id"));
		g_object_get(journal, "occurred-at", &at, NULL);
		g_object_get(original, "occurred-at", &original_at, NULL);
		g_assert_true(g_date_time_equal(at, original_at));
	}
}

/*
 * A character that spends gold its ledger never showed arriving -- an alt
 * mailed it -- would take its purse below zero, which the floor refuses.
 * The day carries the least capital that keeps it at zero instead. An
 * account that may be overdrawn gets none and shows the shortfall.
 *
 * What breaks if this regresses: the first purchase of every alt refuses
 * the whole pass, or the books invent more capital than was needed.
 */
static void
test_capital(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) place = NULL;
	g_autoptr(VentureEntity) purse = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonObject) report = NULL;
	g_autoptr(VentureEntity) spend = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\n");
	line_market(body);
	line_account(body, "Alt-Thorium", "thorium");
	line_account(body, "Neg-Thorium", "thorium");
	line_txn(body, "a1", "Alt-Thorium", "thorium", "buy", "2447", 5, "50.0000", "2026-03-03T10:00:00Z");
	line_txn(body, "a2", "Alt-Thorium", "thorium", "sale", "2447", 5, "70.0000", "2026-03-04T10:00:00Z");
	line_txn(body, "n1", "Neg-Thorium", "thorium", "buy", "2447", 5, "50.0000", "2026-03-03T10:00:00Z");
	line_txn(body, "n2", "Neg-Thorium", "thorium", "sale", "2447", 1, "20.0000", "2026-03-04T10:00:00Z");
	run = push(f, source, body->str);

	/* One purse may be overdrawn. */
	g_assert_true(venture_marketdata_promote_account(f->context, f->org, ID(source), "Neg-Thorium",
	                                                 NULL, &place, &error));
	g_assert_no_error(error);
	purse = reread(f, VENTURE_TYPE_ACCOUNT, holding_of(f, source, "Neg-Thorium"));
	g_object_set(purse, "allow-negative", TRUE, NULL);
	save(f, purse);

	report = post_ok(f, source, NULL, "2026-03-10");
	g_assert_cmpint(status_count(report, "unposted"), ==, 4);

	g_assert_cmpint(held(f, source, "Alt-Thorium", "GOLD"), ==, GOLD(70, 0));
	g_assert_cmpint(held(f, source, "Neg-Thorium", "GOLD"), ==, -GOLD(30, 0));
	g_assert_cmpint(balance_of(f, "trading_capital", "GOLD"), ==, -GOLD(50, 0));

	{
		g_autoptr(VentureEntity) record = posting_record(f, source, "Alt-Thorium:2026-03-03");
		g_autoptr(VentureMoney) capital = NULL;

		g_object_get(record, "capital", &capital, NULL);
		g_assert_cmpint(venture_money_get_amount(capital), ==, GOLD(50, 0));
	}

	/* The floor still judges everyone else: a spend dated before the
	 * sale that funds it is refused. */
	spend = VENTURE_ENTITY(venture_expense_new());
	venture_entity_set_organization_id(spend, f->org);
	g_object_set(spend, "description", "Too early", "cash-account-id",
	             holding_of(f, source, "Alt-Thorium"), NULL);
	field(spend, "amount", "10.0000 GOLD");
	field(spend, "occurred-at", "2026-03-03T12:00:00Z");
	g_assert_false(venture_database_save(f->db, spend, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "below"));
}

/*
 * The currency rule applies unchanged: a valued currency with a rate is
 * converted into the dollar book (the line keeps its euros), and a memo
 * currency posts no journal at all but moves the purse -- replaced, not
 * added to, when the day changes.
 *
 * What breaks if this regresses: euros added to dollars at par, tickets
 * in the general journal, or a corrected ticket day counted twice.
 */
static void
test_currencies(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) euros = NULL;
	g_autoptr(VentureEntity) tickets = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) grown = NULL;
	g_autoptr(GString) eur = g_string_new(NULL);
	g_autoptr(GString) tix = g_string_new(NULL);
	g_autoptr(GString) more = g_string_new(NULL);
	g_autoptr(JsonObject) report = NULL;
	g_autoptr(JsonObject) memo = NULL;
	g_autoptr(JsonObject) again = NULL;
	g_autoptr(GPtrArray) journals = NULL;

	(void)user_data;

	euros = push_source(f, f->org, "Shop", "EUR", "books: daily\n");
	line_account(eur, "Stall-Main", "main");
	line_txn(eur, "e1", "Stall-Main", "main", "sale", "mug", 1, "10.00", "2026-03-02T10:00:00Z");
	run = push(f, euros, eur->str);
	report = post_ok(f, euros, NULL, "2026-03-10");
	g_assert_cmpint(status_count(report, "unposted"), ==, 1);

	journals = journals_of(f, euros);
	g_assert_cmpuint(journals->len, ==, 1);
	{
		g_autofree gchar *currency = NULL;

		g_object_get(g_ptr_array_index(journals, 0), "currency", &currency, NULL);
		g_assert_cmpstr(currency, ==, "USD");
	}
	g_assert_cmpint(held(f, euros, "Stall-Main", "EUR"), ==, 1000);
	g_assert_cmpint(balance_of(f, "trading_sales", "USD"), ==, -1100);

	tickets = push_source(f, f->org, "Faire", "TICKET", "books: daily\n");
	line_account(tix, "Aria-Faire", "faire");
	line_txn(tix, "t1", "Aria-Faire", "faire", "income", NULL, 0, "5", "2026-03-02T10:00:00Z");
	g_clear_object(&run);
	run = push(f, tickets, tix->str);
	memo = post_ok(f, tickets, NULL, "2026-03-10");
	g_assert_cmpint(status_count(memo, "unposted"), ==, 1);
	g_assert_cmpuint(count_journals(f, tickets, FALSE), ==, 0);
	g_assert_cmpint(held(f, tickets, "Aria-Faire", "TICKET"), ==, 5);

	line_txn(more, "t1", "Aria-Faire", "faire", "income", NULL, 0, "8", "2026-03-02T10:00:00Z");
	grown = push(f, tickets, more->str);
	again = post_ok(f, tickets, NULL, "2026-03-10");
	g_assert_cmpint(status_count(again, "changed"), ==, 1);
	g_assert_cmpint(held(f, tickets, "Aria-Faire", "TICKET"), ==, 8);
	g_assert_cmpuint(count_journals(f, tickets, FALSE), ==, 0);
}

/*
 * The settings are judged when written: a mode that does not exist, an
 * automatic post outside daily, a books_from that is no date. The
 * report shows each day and where it stands -- a dry run of the action,
 * so they agree.
 *
 * What breaks if this regresses: a typo turns the books off without a
 * word, or the report says posted for a day the books never saw.
 */
static void
test_settings_and_report(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonObject) posted = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureDateRange) march = venture_date_range_new_month(2026, 3, NULL);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	static const gchar *const refused[] = {
		"books: weekly\n",
		"books: trades\npost_to_books: true\n",
		"books: daily\nbooks_from: someday\n",
		"books: daily\nbooks_max_writes: 0\n",
		NULL
	};
	VentureReport *report;
	guint i;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\n");

	for (i = 0; NULL != refused[i]; i++)
	{
		g_object_set(source, "settings", refused[i], NULL);
		g_assert_false(venture_database_save(f->db, source, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

	g_clear_object(&source);
	source = push_source(f, f->org, "TSM 2", "GOLD", "books: daily\nbooks_from: 2026-03-02\n");
	drgold_ledger(body);
	run = push(f, source, body->str);

	/* books_from: 1 March is not the books'. */
	posted = post_ok(f, source, NULL, "2026-03-10");
	g_assert_cmpint(status_count(posted, "unposted"), ==, 2);

	json_object_set_int_member(options, "data_source_id", ID(source));
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context),
	                                        "external_books");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, march, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);

	/* The opening (dated 2 March) and 2 March, posted; 1 March is
	 * outside the books and 5 March has no money. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);

	for (i = 0; i < 2; i++)
	{
		const GValue *status = venture_report_result_get_cell(result, i, "status");
		const GValue *when = venture_report_result_get_cell(result, i, "day");

		g_assert_cmpstr(g_value_get_string(status), ==, "posted");
		g_assert_true(g_str_has_prefix(g_value_get_string(when), "2026-03-02"));
	}
}

/* The action as @actor inside @principal's scope (NULL: internal). */
static VentureEntity *
act(
	Fixture			 *f,
	VentureEntity		 *source,
	const gchar		 *name,
	const gchar		 *params_json,
	VentureAuthPrincipal	 *principal,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonNode) body = venture_json_parse(params_json, NULL);
	g_autoptr(GHashTable) params = NULL;
	const gchar *type;

	/* Type-level actions on the books' own types, judged as financial. */
	type = (0 == g_strcmp0(name, "post_ledger")) ? "external_posting" : "arbitrage_trade";
	json_object_set_int_member(json_node_get_object(body), "data_source_id", ID(source));
	json_object_set_int_member(json_node_get_object(body), "organization_id",
	                           venture_entity_get_organization_id(source));

	if (NULL != principal)
		scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), principal);

	params = venture_action_parameters_from_json(body, error);
	g_assert_nonnull(params);

	return venture_action_registry_perform(venture_database_get_action_registry(f->db), type, 0,
		name, params, actor, (NULL != principal) ? principal->role : VENTURE_USER_ROLE_OWNER, error);
}

static void
member(
	Fixture			*f,
	gint			 role,
	const gchar		*username,
	VentureAuthPrincipal	*principal
){
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", username,
		"active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) membership = NULL;

	save(f, user);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", ID(user),
		"organization-id", f->org, "role", role, "active", TRUE, NULL);
	save(f, membership);
	principal->authenticated = TRUE;
	principal->user_id = ID(user);
	principal->token_id = 0;
	principal->role = VENTURE_USER_ROLE_EDITOR;
	principal->name = NULL;
}

/*
 * The action is finance's: an organization's finance member posts, its
 * editor member -- who may sync the source -- does not. With a second-
 * actor rule on posting the first person proposes and a second performs;
 * the automatic pass does not post at all and says why on the run.
 *
 * What breaks if this regresses: anybody who can sync a feed writes the
 * books, or one person alone posts an organization's ledger.
 */
static void
test_permission_and_approval(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureAccountingApprovalRule) rule = venture_accounting_approval_rule_new();
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *answer = NULL;
	VentureAuthPrincipal finance;
	VentureAuthPrincipal editor;
	VentureActor alice;
	VentureActor bob;

	(void)user_data;

	member(f, VENTURE_ORGANIZATION_ROLE_FINANCE, "trader", &finance);
	member(f, VENTURE_ORGANIZATION_ROLE_EDITOR, "editor", &editor);

	/* A second-actor rule, then an automatic source: the pass declines. */
	g_object_set(rule, "organization-id", f->org, "action", "post", "require-second-actor", TRUE, NULL);
	save(f, rule);
	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\npost_to_books: true\n");
	drgold_ledger(body);
	run = push(f, source, body->str);
	g_clear_object(&run);
	g_assert_cmpuint(count_journals(f, source, FALSE), ==, 0);

	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
		g_autoptr(VentureEntity) last = NULL;

		venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
		last = venture_database_find_one(f->db, query, NULL);
		g_object_get(last, "notes", &notes, NULL);
		g_assert_nonnull(strstr(notes, "second-actor"));
	}

	/* The editor member may not; the finance member proposes. */
	result = act(f, source, "post_ledger", "{\"until\":\"2026-03-10\"}", &editor, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	alice.kind = VENTURE_ACTOR_KIND_USER; alice.name = "alice";
	alice.prompt = NULL; alice.request_id = NULL; alice.approved_by = NULL;
	bob.kind = VENTURE_ACTOR_KIND_USER; bob.name = "bob";
	bob.prompt = NULL; bob.request_id = NULL; bob.approved_by = NULL;

	result = act(f, source, "post_ledger", "{\"until\":\"2026-03-10\"}", NULL, &alice, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_cmpuint(count_journals(f, source, FALSE), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_EXTERNAL_POSTING), ==, 0);

	result = act(f, source, "post_ledger", "{\"until\":\"2026-03-10\"}", NULL, &bob, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(count_journals(f, source, FALSE), ==, 3);
	g_object_get(result, "result", &answer, NULL);
	g_assert_nonnull(strstr(answer, "\"written\":3"));
	g_clear_object(&result);

	/* With no rule the finance member posts at once (nothing left to). */
	g_object_set(rule, "require-second-actor", FALSE, NULL);
	save(f, rule);
	result = act(f, source, "post_ledger", "{\"until\":\"2026-03-10\",\"dry_run\":true}", &finance,
	             NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
}

/*
 * post_to_books posts after each push, on the main thread, the days that
 * are over; the run record says so. Pushing the same lines again posts
 * nothing more.
 *
 * What breaks if this regresses: the books lag the ledger until somebody
 * remembers, or every push reposts every day.
 */
static void
test_auto(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autofree gchar *notes = NULL;
	g_autofree gchar *later = NULL;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\npost_to_books: true\n");
	drgold_ledger(body);
	run = push(f, source, body->str);
	g_object_get(run, "notes", &notes, NULL);
	g_assert_nonnull(strstr(notes, "books: 3 day(s) posted"));
	g_assert_cmpuint(count_journals(f, source, FALSE), ==, 3);
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(140, 2000));

	again = push(f, source, body->str);
	g_object_get(again, "notes", &later, NULL);
	g_assert_true((NULL == later) || (NULL == strstr(later, "books:")));
	g_assert_cmpuint(count_journals(f, source, FALSE), ==, 3);
}

/* --- Trades ---------------------------------------------------------------------- */

/* Two characters on two realms: Drgold buys, Alt sells; the last sale
 * has one unit no earlier buy covers. */
static void
flip_ledger(GString *body)
{
	line_market(body);
	line_account(body, "Drgold-Thorium", "thorium");
	line_account(body, "Alt-Silvermoon", "silvermoon");
	line_txn(body, "b1", "Drgold-Thorium", "thorium", "buy", "2447", 3, "10.0000", "2026-03-01T10:00:00Z");
	line_txn(body, "b2", "Drgold-Thorium", "thorium", "buy", "2447", 2, "8.0000", "2026-03-01T11:00:00Z");
	line_txn(body, "s1", "Alt-Silvermoon", "silvermoon", "sale", "2447", 1, "6.0000", "2026-03-02T10:00:00Z");
	line_txn(body, "s2", "Alt-Silvermoon", "silvermoon", "sale", "2447", 3, "21.0000", "2026-03-03T10:00:00Z");
	line_txn(body, "s3", "Drgold-Thorium", "thorium", "sale", "2447", 2, "12.0000", "2026-03-04T10:00:00Z");
}

/*
 * The pairs behind the flips report, one take at a time: they add up to
 * its matched units, cost and proceeds exactly, a lot of three bought for
 * 10.0000 splitting into 3.3333 and 6.6667; with what recorded trades
 * used taken off, only the rest is matched again.
 *
 * What breaks if this regresses: recorded trades that disagree with the
 * page by a copper, or a lot booked twice.
 */
static void
test_flip_pairs(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GPtrArray) pairs = NULL;
	g_autoptr(GPtrArray) rest = NULL;
	g_autoptr(GPtrArray) report = NULL;
	g_autoptr(GHashTable) used = NULL;
	g_autoptr(GError) error = NULL;
	VentureSeriesFlip *flip;
	VentureSeriesFlipUse *use;
	gint64 units = 0;
	gint64 cost = 0;
	gint64 proceeds = 0;
	guint i;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: trades\n");
	flip_ledger(body);
	run = push(f, source, body->str);
	store = venture_feeds_service_open_reader(venture_context_get_feeds_service(f->context), ID(source),
	                                          &error);
	g_assert_no_error(error);

	pairs = venture_series_store_flip_pairs(store, NULL, NULL, &error);
	g_assert_no_error(error);
	report = venture_series_store_flips(store, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(report->len, ==, 1);
	flip = g_ptr_array_index(report, 0);

	/* s1 from b1; s2 from b1 then b2; s3 from b2 (one unit unmatched). */
	g_assert_cmpuint(pairs->len, ==, 4);

	for (i = 0; i < pairs->len; i++)
	{
		VentureSeriesFlipPair *pair = g_ptr_array_index(pairs, i);

		units += pair->units;
		cost += pair->cost;
		proceeds += pair->proceeds;
	}

	g_assert_cmpint(units, ==, flip->matched_units);
	g_assert_cmpint(cost, ==, flip->cost);
	g_assert_cmpint(proceeds, ==, flip->proceeds);
	g_assert_cmpint(cost, ==, GOLD(18, 0));
	g_assert_cmpint(proceeds, ==, GOLD(33, 0));
	g_assert_cmpint(((VentureSeriesFlipPair *)g_ptr_array_index(pairs, 0))->cost, ==, GOLD(3, 3333));
	g_assert_cmpint(((VentureSeriesFlipPair *)g_ptr_array_index(pairs, 1))->cost, ==, GOLD(6, 6667));
	g_assert_cmpstr(((VentureSeriesFlipPair *)g_ptr_array_index(pairs, 0))->buy_account, ==,
	                "Drgold-Thorium");
	g_assert_cmpstr(((VentureSeriesFlipPair *)g_ptr_array_index(pairs, 0))->sale_account, ==,
	                "Alt-Silvermoon");

	/* s1 and b1's unit recorded: the walk starts from what is left. */
	used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	use = g_new0(VentureSeriesFlipUse, 1);
	use->units = 1;
	use->amount = GOLD(6, 0);
	g_hash_table_insert(used, g_strdup("s1"), use);
	use = g_new0(VentureSeriesFlipUse, 1);
	use->units = 1;
	use->amount = GOLD(3, 3333);
	g_hash_table_insert(used, g_strdup("b1"), use);
	rest = venture_series_store_flip_pairs(store, NULL, used, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rest->len, ==, 3);
	g_assert_cmpstr(((VentureSeriesFlipPair *)g_ptr_array_index(rest, 0))->sale_key, ==, "s2");
	g_assert_cmpint(((VentureSeriesFlipPair *)g_ptr_array_index(rest, 0))->cost, ==, GOLD(6, 6667));
}

/*
 * record_flips makes one closed trade per matched sale, its buys at the
 * buyer's purse and its sale at the seller's, and their results add up
 * to the flips report's profit; the gold goes to gains in the gold book
 * and the positions account ends at nothing. The buyer's purse, which
 * the ledger never funded, is funded by trading capital at each buy.
 * Recording again records nothing; a sale that grew later is recorded
 * for the units it gained, once.
 *
 * What breaks if this regresses: a trade per run, a lot's cost counted
 * twice, a refused first buy, or recorded profit that is not the page's.
 */
static void
test_record_flips(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) grown = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GString) more = g_string_new(NULL);
	g_autoptr(JsonObject) dry = NULL;
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) second = NULL;
	g_autoptr(JsonObject) third = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) trades = NULL;
	g_autoptr(GError) error = NULL;
	gint64 drgold_place = 0;
	gint64 alt_place = 0;
	guint i;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: trades\n");
	flip_ledger(body);
	run = push(f, source, body->str);

	dry = flips(f, source, TRUE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(dry, "flips"), ==, 3);
	g_assert_cmpint(json_object_get_int_member(dry, "recorded"), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, 0);

	first = flips(f, source, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(first, "recorded"), ==, 3);
	g_assert_cmpint(json_object_get_int_member(first, "units"), ==, 5);

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_TRADE);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	trades = venture_database_find(f->db, query, NULL);
	g_assert_cmpuint(trades->len, ==, 3);

	{
		g_autoptr(VentureEntity) place = NULL;
		g_autofree gchar *ref = venture_marketdata_account_ref(source, "Drgold-Thorium");
		g_autofree gchar *alt_ref = venture_marketdata_account_ref(source, "Alt-Silvermoon");

		place = venture_marketdata_find_by_ref(f->db, VENTURE_TYPE_LOCATION, f->org, ref, NULL);
		drgold_place = ID(place);
		g_clear_object(&place);
		place = venture_marketdata_find_by_ref(f->db, VENTURE_TYPE_LOCATION, f->org, alt_ref, NULL);
		alt_place = ID(place);
	}

	for (i = 0; i < trades->len; i++)
	{
		VentureEntity *trade = g_ptr_array_index(trades, i);
		g_autoptr(VentureQuery) legs_query = venture_query_new(VENTURE_TYPE_ARBITRAGE_LEG);
		g_autoptr(GPtrArray) legs = NULL;
		g_autofree gchar *ref = NULL;
		g_autofree gchar *strategy = NULL;
		gint status = 0;
		guint j;

		g_object_get(trade, "status", &status, "external-ref", &ref, "strategy", &strategy, NULL);
		g_assert_cmpint(status, ==, VENTURE_ARBITRAGE_TRADE_STATUS_CLOSED);
		g_assert_cmpstr(strategy, ==, "flip");
		g_assert_true(g_str_has_prefix(ref, venture_entity_get_uuid(source)));
		g_assert_true(g_str_has_suffix(ref, ":1"));
		g_assert_cmpint(int_of(trade, "data-source-id"), ==, ID(source));

		venture_query_add_filter_int(legs_query, "trade-id", VENTURE_FILTER_OP_EQ, ID(trade), NULL);
		legs = venture_database_find(f->db, legs_query, NULL);

		for (j = 0; j < legs->len; j++)
		{
			VentureEntity *leg = g_ptr_array_index(legs, j);
			gint kind = 0;
			gint leg_status = 0;

			g_object_get(leg, "kind", &kind, "status", &leg_status, NULL);
			g_assert_cmpint(leg_status, ==, VENTURE_ARBITRAGE_LEG_STATUS_EXECUTED);

			/* s3 is Drgold's own sale; the others are Alt's. */
			if (VENTURE_ARBITRAGE_LEG_KIND_BUY == kind)
				g_assert_cmpint(int_of(leg, "location-id"), ==, drgold_place);
			else if (2 != i)
				g_assert_cmpint(int_of(leg, "location-id"), ==, alt_place);
		}
	}

	/* The flips report's profit, to the copper, in the gold book. */
	g_assert_cmpint(balance_of(f, "arbitrage_gains", "GOLD"), ==, -GOLD(15, 0));
	g_assert_cmpint(balance_of(f, "arbitrage_positions", "GOLD"), ==, 0);
	g_assert_cmpint(balance_of(f, "trading_capital", "GOLD"), ==, -GOLD(18, 0));
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(6, 0));
	g_assert_cmpint(held(f, source, "Alt-Silvermoon", "GOLD"), ==, GOLD(27, 0));

	second = flips(f, source, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(second, "recorded"), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, 3);

	/* s3 grew to three units for 18 gold, and a buy before it covers one
	 * more: one trade for that unit, numbered 2. */
	line_txn(more, "b3", "Drgold-Thorium", "thorium", "buy", "2447", 1, "5.0000", "2026-03-03T12:00:00Z");
	line_txn(more, "s3", "Drgold-Thorium", "thorium", "sale", "2447", 3, "18.0000", "2026-03-04T10:00:00Z");
	grown = push(f, source, more->str);
	third = flips(f, source, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(third, "recorded"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(third, "units"), ==, 1);

	{
		JsonArray *rows = json_object_get_array_member(third, "trades");
		JsonObject *row = json_array_get_object_element(rows, 0);

		g_assert_true(g_str_has_suffix(venture_json_object_get_string(row, "external_ref", ""), ":s3:2"));
	}

	/* 15 before, and this unit sold for 6 against 5. */
	g_assert_cmpint(balance_of(f, "arbitrage_gains", "GOLD"), ==, -GOLD(16, 0));

	/* A hand edit of where a flip came from is refused. */
	{
		g_autoptr(VentureEntity) trade = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE,
		                                        ID(g_ptr_array_index(trades, 0)));

		g_object_set(trade, "external-ref", "mine", NULL);
		g_assert_false(venture_database_save(f->db, trade, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/*
 * A source books one way, and the data agrees whatever the setting says
 * now: flips touching a day already posted a day at a time are left out
 * of record_flips, and days whose flips are recorded are left out of
 * post_ledger. Each action refuses the other mode.
 *
 * What breaks if this regresses: switching modes books a month of sales
 * twice -- once as days, once as trades.
 */
static void
test_one_booking(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) later = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GString) more = g_string_new(NULL);
	g_autoptr(JsonObject) recorded = NULL;
	g_autoptr(JsonObject) daily = NULL;
	g_autoptr(JsonObject) none = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: trades\n");
	flip_ledger(body);
	run = push(f, source, body->str);

	/* Trades mode refuses post_ledger. */
	g_assert_null(post(f, source, NULL, "2026-03-10", NULL, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);

	recorded = flips(f, source, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(recorded, "recorded"), ==, 3);

	/* Switched to daily, with a later day of postage. */
	set_settings(f, source, "books: daily\n");
	line_txn(more, "p9", "Alt-Silvermoon", "silvermoon", "expense", NULL, 0, "1.0000", "2026-03-06T10:00:00Z");
	later = push(f, source, more->str);
	daily = post_ok(f, source, NULL, "2026-03-10");

	/* Drgold 1 March (buys), Alt 2 and 3 March, Drgold 4 March: all
	 * booked by the trades. Alt's 6 March posts (with its opening). */
	g_assert_cmpint(status_count(daily, "left_out"), ==, 4);
	g_assert_cmpint(status_count(daily, "unposted"), ==, 1);
	g_assert_cmpint(balance_of(f, "trading_sales", "GOLD"), ==, 0);
	g_assert_cmpint(balance_of(f, "trading_expenses", "GOLD"), ==, GOLD(1, 0));

	/* Daily mode refuses record_flips; and, switched back, a flip
	 * touching the posted day is left out. */
	g_assert_null(flips(f, source, FALSE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_clear_error(&error);

	g_clear_object(&source);
	source = push_source(f, f->org, "TSM daily", "GOLD", "books: daily\n");
	g_clear_object(&run);
	run = push(f, source, body->str);
	g_clear_pointer(&daily, json_object_unref);
	daily = post_ok(f, source, NULL, "2026-03-10");
	set_settings(f, source, "books: trades\n");
	none = flips(f, source, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(none, "recorded"), ==, 0);
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(none, "skipped"),
	                                           "posted_days"), ==, 3);
}

/*
 * A source is its organization's: another organization's books cannot
 * post it or record its flips, and an editor member cannot record flips.
 *
 * What breaks if this regresses: one organization writes another's books.
 */
static void
test_organizations(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;
	VentureBooksPostQuery post_query;
	VentureBooksFlipsQuery flips_query;
	VentureAuthPrincipal editor;

	(void)user_data;

	organization = VENTURE_ENTITY(venture_organization_new());
	g_object_set(organization, "name", "Evermoor", "slug", "evermoor", NULL);
	save(f, organization);
	source = push_source(f, ID(organization), "Theirs", "GOLD", "books: trades\n");
	flip_ledger(body);
	run = push(f, source, body->str);

	memset(&post_query, 0, sizeof(post_query));
	post_query.organization_id = f->org;
	post_query.data_source_id = ID(source);
	post_query.dry_run = TRUE;
	g_assert_false(venture_arbitrage_books_post(f->context, &post_query, NULL, &report, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	memset(&flips_query, 0, sizeof(flips_query));
	flips_query.organization_id = f->org;
	flips_query.data_source_id = ID(source);
	g_assert_false(venture_arbitrage_books_record_flips(f->context, &flips_query, NULL, &report, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	g_clear_object(&source);
	g_clear_object(&run);
	source = push_source(f, f->org, "Ours", "GOLD", "books: trades\n");
	run = push(f, source, body->str);
	member(f, VENTURE_ORGANIZATION_ROLE_EDITOR, "editor", &editor);
	result = act(f, source, "record_flips", "{\"dry_run\":true}", &editor, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, 0);
}

/*
 * A recorded flip's snapshot is the ledger rows it took, and the next run
 * subtracts it before matching again. A hand edit that drops or shrinks
 * it is refused like an edit of its external reference; the same
 * snapshot posted back re-indented is not a change.
 *
 * What breaks if this regresses: editing a flip's Expected box frees the
 * rows it named, and the next Record flips books the same sale a second
 * time, with a second set of journals.
 */
static void
test_flip_snapshot_held(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonObject) first = NULL;
	g_autoptr(JsonObject) again = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) trades = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	g_autofree gchar *snapshot = NULL;
	g_autofree gchar *pretty = NULL;
	guint journals;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: trades\n");
	flip_ledger(body);
	run = push(f, source, body->str);

	first = flips(f, source, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(first, "recorded"), ==, 3);
	journals = count_rows(f, VENTURE_TYPE_JOURNAL);

	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_TRADE);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	trades = venture_database_find(f->db, query, NULL);
	g_assert_cmpuint(trades->len, ==, 3);

	/* The snapshot without its source: refused. */
	trade = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, ID(g_ptr_array_index(trades, 0)));
	g_object_get(trade, "expected", &snapshot, NULL);
	g_object_set(trade, "expected", "{\"profit\":[\"1.0000 GOLD\"]}", NULL);
	g_assert_false(venture_database_save(f->db, trade, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Emptied: refused too. */
	g_clear_object(&trade);
	trade = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, ID(g_ptr_array_index(trades, 0)));
	g_object_set(trade, "expected", "", NULL);
	g_assert_false(venture_database_save(f->db, trade, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* The same snapshot, re-indented, beside an edited note: saved. */
	g_clear_object(&trade);
	trade = reread(f, VENTURE_TYPE_ARBITRAGE_TRADE, ID(g_ptr_array_index(trades, 0)));
	parsed = json_from_string(snapshot, &error);
	g_assert_no_error(error);
	pretty = json_to_string(parsed, TRUE);
	g_assert_cmpstr(pretty, !=, snapshot);
	g_object_set(trade, "expected", pretty, "notes", "Checked against the ledger", NULL);
	g_assert_true(venture_database_save(f->db, trade, NULL, &error));
	g_assert_no_error(error);

	/* Nothing was freed, so nothing is recorded again. */
	again = flips(f, source, FALSE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(again, "recorded"), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, 3);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, journals);
}

/* "2026-03-01T12:00:00Z" for @when. */
static gchar *
iso(gint64 when)
{
	g_autoptr(GDateTime) at = g_date_time_new_from_unix_utc(when);

	return g_date_time_format(at, "%Y-%m-%dT%H:%M:%SZ");
}

/*
 * Retention purges balance points by their own time and keeps only an
 * account's newest before its horizon. An opening posted for the
 * horizon's own day took its capital from a point the day before, which
 * the purge removes while the day itself stays inside the horizon. The
 * opening is kept as posted; nothing is reversed.
 *
 * What breaks if this regresses: once, on the day retention reaches each
 * account's opening, the opening reads as zero, and it and every day the
 * account ever posted are reversed and posted again.
 */
static void
test_opening_after_purge(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureSeriesStore) store = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(JsonObject) posted = NULL;
	g_autoptr(JsonObject) kept = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *store_dir = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *bought = NULL;
	g_autofree gchar *sold = NULL;
	g_autofree gchar *later = NULL;
	VentureSeriesPurgeResult purged;
	JsonArray *days;
	gint64 now;
	gint64 opening_day;
	gboolean found;
	guint i;

	(void)user_data;

	/* Kept for 11 days, so the horizon is ten days back: the opening's. */
	now = g_get_real_time() / G_USEC_PER_SEC;
	opening_day = (now / 86400) * 86400 - 10 * 86400;
	seen = iso(opening_day - 12 * 3600);
	bought = iso(opening_day + 10 * 3600);
	sold = iso(opening_day + 12 * 3600);
	later = iso(opening_day + 2 * 86400);

	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\n");
	line_market(body);
	line_account(body, "Drgold-Thorium", "thorium");
	line_balance(body, "Drgold-Thorium", "GOLD", "100.0000", seen);
	line_txn(body, "b1", "Drgold-Thorium", "thorium", "buy", "2447", 3, "30.0000", bought);
	line_txn(body, "s1", "Drgold-Thorium", "thorium", "sale", "2447", 1, "25.5000", sold);
	line_balance(body, "Drgold-Thorium", "GOLD", "95.5000", later);
	run = push(f, source, body->str);

	posted = post_ok(f, source, NULL, NULL);
	g_assert_cmpint(status_count(posted, "unposted"), ==, 2);
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(95, 5000));

	/* The store's own retention, as the worker runs it. */
	g_object_set(f->config, "series-daily-days", (gint64)11, NULL);
	store_dir = venture_feeds_store_dir(f->config, venture_entity_get_uuid(source));
	store = venture_series_store_open(store_dir, &error);
	g_assert_no_error(error);
	memset(&purged, 0, sizeof(purged));
	g_assert_true(venture_series_store_purge(store, now, 0, 11, &purged, &error));
	g_assert_no_error(error);
	g_assert_cmpint(purged.balances, ==, 1);
	g_assert_cmpint(purged.txns, ==, 0);
	g_clear_object(&store);

	kept = post_ok(f, source, NULL, NULL);
	g_assert_cmpint(json_object_get_int_member(kept, "written"), ==, 0);
	g_assert_cmpint(status_count(kept, "changed"), ==, 0);
	g_assert_cmpint(status_count(kept, "follows"), ==, 0);
	g_assert_cmpuint(count_journals(f, source, TRUE), ==, 0);
	g_assert_cmpint(held(f, source, "Drgold-Thorium", "GOLD"), ==, GOLD(95, 5000));

	days = json_object_get_array_member(kept, "days");
	found = FALSE;

	for (i = 0; i < json_array_get_length(days); i++)
	{
		JsonObject *row = json_array_get_object_element(days, i);

		if (0 != g_strcmp0(venture_json_object_get_string(row, "kind", NULL), "opening"))
			continue;

		g_assert_cmpstr(venture_json_object_get_string(row, "status", NULL), ==, "kept");
		found = TRUE;
	}

	g_assert_true(found);
}

/* Refuses every date from 2 March 2026 on: a later period closed while an
 * earlier one is open. */
static GError *
closed_from_march_second(
	VenturePostingService	*posting,
	gint64			 org,
	GDateTime		*when,
	gpointer		 data
){
	g_autoptr(GDateTime) bound = day("2026-03-02");

	(void)posting;
	(void)org;
	(void)data;

	if (g_date_time_compare(when, bound) < 0)
		return NULL;

	return g_error_new_literal(VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Period 'March, later' is closed");
}

/*
 * A day that changes makes every posted day after it post again, only so
 * that the reversals come first; what those days say is unchanged. One in
 * a closed period is kept as posted: its reversal would be refused.
 *
 * What breaks if this regresses: reopening an earlier period to correct
 * it fails every pass on the later, closed period's reversal, and the
 * correction can never be posted.
 */
static void
test_follower_in_closed_period(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) grown = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GString) more = g_string_new(NULL);
	g_autoptr(JsonObject) posted = NULL;
	g_autoptr(JsonObject) changed = NULL;
	JsonArray *notes;
	gboolean noted;
	gulong handler;
	guint i;

	(void)user_data;

	source = push_source(f, f->org, "TSM", "GOLD", "books: daily\n");
	drgold_ledger(body);
	run = push(f, source, body->str);
	posted = post_ok(f, source, NULL, "2026-03-10");
	g_assert_cmpint(status_count(posted, "unposted"), ==, 3);

	/* 1 March's sale grew; 2 March is now in a closed period. */
	line_txn(more, "s1", "Drgold-Thorium", "thorium", "sale", "2447", 2, "51.0000", "2026-03-01T12:00:00Z");
	grown = push(f, source, more->str);
	handler = g_signal_connect(venture_database_get_posting_service(f->db), "date-postable",
	                           G_CALLBACK(closed_from_march_second), NULL);

	changed = post_ok(f, source, NULL, "2026-03-10");
	g_signal_handler_disconnect(venture_database_get_posting_service(f->db), handler);

	g_assert_cmpint(status_count(changed, "changed"), ==, 1);
	g_assert_cmpint(status_count(changed, "follows"), ==, 0);
	g_assert_cmpint(status_count(changed, "kept"), ==, 1);
	g_assert_cmpuint(count_journals(f, source, TRUE), ==, 1);
	g_assert_cmpint(balance_of(f, "trading_sales", "GOLD"), ==, -GOLD(91, 0));

	second = posting_record(f, source, "Drgold-Thorium:2026-03-02");
	g_assert_cmpint(int_of(second, "revision"), ==, 1);

	notes = json_object_get_array_member(changed, "notes");
	noted = FALSE;

	for (i = 0; i < json_array_get_length(notes); i++)
		noted = noted || (NULL != strstr(json_array_get_string_element(notes, i), "closed period"));

	g_assert_true(noted);
}

/*
 * The capital a buy needs is what keeps every later moment of the purse at
 * zero or more once it is spent, because the floor refuses a spend that
 * ends a moment below zero and lower than it was -- which a spend over a
 * dip already below zero always does. With a -50 dip after it, a spend of
 * 30 needs 80, not 30: with 30 the dip would end at -50, deeper than the
 * -20 the capital left it at.
 *
 * What breaks if this regresses: Record flips funds the buy too little,
 * and the leg is refused by the floor it was funded to pass.
 */
static void
test_shortfall_over_a_dip(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) place = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureEntity) movement = NULL;
	g_autoptr(VentureMoney) spend = NULL;
	g_autoptr(VentureMoney) shortfall = NULL;
	g_autoptr(GDateTime) at = day("2026-03-05");
	g_autoptr(GError) error = NULL;
	gint64 holding = 0;

	(void)user_data;

	place = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_LOCATION, NULL));
	venture_entity_set_organization_id(place, f->org);
	g_object_set(place, "name", "Drgold", NULL);
	save(f, place);
	g_assert_true(venture_holdings_account_for_location(f->db, f->org, ID(place), TRUE, NULL,
	                                                    &holding, &error));
	g_assert_no_error(error);

	/* A dip left behind while overdrafts were allowed. */
	account = reread(f, VENTURE_TYPE_ACCOUNT, holding);
	g_object_set(account, "allow-negative", TRUE, NULL);
	save(f, account);
	movement = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_HOLDING_TXN, NULL));
	venture_entity_set_organization_id(movement, f->org);
	g_object_set(movement, "account-id", holding, NULL);
	field(movement, "amount", "-50 TICKET");
	field(movement, "occurred-at", "2026-03-10");
	save(f, movement);
	g_clear_object(&account);
	account = reread(f, VENTURE_TYPE_ACCOUNT, holding);
	g_object_set(account, "allow-negative", FALSE, NULL);
	save(f, account);

	spend = venture_money_new(-30, "TICKET", 0);
	g_assert_nonnull(spend);
	g_assert_true(venture_holdings_floor_shortfall(f->db, holding, spend, at, &shortfall, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(shortfall), ==, 80);

	/* One less is not enough; exactly that is. */
	g_clear_object(&movement);
	movement = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_HOLDING_TXN, NULL));
	venture_entity_set_organization_id(movement, f->org);
	g_object_set(movement, "account-id", holding, NULL);
	field(movement, "amount", "79 TICKET");
	field(movement, "occurred-at", "2026-03-05");
	save(f, movement);

	g_clear_object(&movement);
	movement = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_HOLDING_TXN, NULL));
	venture_entity_set_organization_id(movement, f->org);
	g_object_set(movement, "account-id", holding, NULL);
	field(movement, "amount", "-30 TICKET");
	field(movement, "occurred-at", "2026-03-05");
	g_assert_false(venture_database_save(f->db, movement, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_clear_object(&movement);
	movement = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_HOLDING_TXN, NULL));
	venture_entity_set_organization_id(movement, f->org);
	g_object_set(movement, "account-id", holding, NULL);
	field(movement, "amount", "1 TICKET");
	field(movement, "occurred-at", "2026-03-05");
	save(f, movement);

	g_clear_object(&movement);
	movement = VENTURE_ENTITY(g_object_new(VENTURE_TYPE_HOLDING_TXN, NULL));
	venture_entity_set_organization_id(movement, f->org);
	g_object_set(movement, "account-id", holding, NULL);
	field(movement, "amount", "-30 TICKET");
	field(movement, "occurred-at", "2026-03-05");
	save(f, movement);
}

/*
 * A sale's cost is the sum of the buys it drew on, and a call's totals the
 * sum of its sales. Past 64 bits either is refused, before anything is
 * written, rather than wrapped into a negative cost and a profit nobody
 * made.
 *
 * What breaks if this regresses: two enormous buys record a trade costing
 * less than nothing, posting a gain of about 18 quintillion coppers.
 */
static void
test_flip_overflow(
	Fixture		*f,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) one_sale = NULL;
	g_autoptr(VentureEntity) two_sales = NULL;
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(VentureEntity) other_run = NULL;
	g_autoptr(GString) body = g_string_new(NULL);
	g_autoptr(GString) other = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	JsonObject *report;

	(void)user_data;

	/* Two buys of just over a quarter of the range each... */
	one_sale = push_source(f, f->org, "One sale", "GOLD", "books: trades\n");
	line_market(body);
	line_account(body, "Drgold-Thorium", "thorium");
	line_txn(body, "b1", "Drgold-Thorium", "thorium", "buy", "2447", 1, "461168601842739.0000",
	         "2026-03-01T10:00:00Z");
	line_txn(body, "b2", "Drgold-Thorium", "thorium", "buy", "2447", 1, "461168601842739.0000",
	         "2026-03-01T11:00:00Z");
	line_txn(body, "s1", "Drgold-Thorium", "thorium", "sale", "2447", 2, "1.0000", "2026-03-02T10:00:00Z");
	run = push(f, one_sale, body->str);

	/* ...drawn on by one sale: its cost does not fit. */
	report = flips(f, one_sale, TRUE, &error);
	g_assert_null(report);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	report = flips(f, one_sale, FALSE, &error);
	g_assert_null(report);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Drawn on by two sales: each fits, their total does not. */
	two_sales = push_source(f, f->org, "Two sales", "GOLD", "books: trades\n");
	line_market(other);
	line_account(other, "Drgold-Thorium", "thorium");
	line_txn(other, "b1", "Drgold-Thorium", "thorium", "buy", "2447", 1, "461168601842739.0000",
	         "2026-03-01T10:00:00Z");
	line_txn(other, "b2", "Drgold-Thorium", "thorium", "buy", "2447", 1, "461168601842739.0000",
	         "2026-03-01T11:00:00Z");
	line_txn(other, "s1", "Drgold-Thorium", "thorium", "sale", "2447", 1, "1.0000", "2026-03-02T10:00:00Z");
	line_txn(other, "s2", "Drgold-Thorium", "thorium", "sale", "2447", 1, "1.0000", "2026-03-02T11:00:00Z");
	other_run = push(f, two_sales, other->str);

	report = flips(f, two_sales, TRUE, &error);
	g_assert_null(report);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	report = flips(f, two_sales, FALSE, &error);
	g_assert_null(report);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_ARBITRAGE_TRADE), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, 0);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/external-books/daily-journals", test_daily_journals);
	ADD("/external-books/idempotent-and-dry-run", test_idempotent_and_dry_run);
	ADD("/external-books/changed-day", test_changed_day);
	ADD("/external-books/capital", test_capital);
	ADD("/external-books/currencies", test_currencies);
	ADD("/external-books/settings-and-report", test_settings_and_report);
	ADD("/external-books/permission-and-approval", test_permission_and_approval);
	ADD("/external-books/auto", test_auto);
	ADD("/external-books/flip-pairs", test_flip_pairs);
	ADD("/external-books/record-flips", test_record_flips);
	ADD("/external-books/one-booking", test_one_booking);
	ADD("/external-books/organizations", test_organizations);
	ADD("/external-books/flip-snapshot-held", test_flip_snapshot_held);
	ADD("/external-books/opening-after-purge", test_opening_after_purge);
	ADD("/external-books/follower-in-closed-period", test_follower_in_closed_period);
	ADD("/external-books/shortfall-over-a-dip", test_shortfall_over_a_dip);
	ADD("/external-books/flip-overflow", test_flip_overflow);

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
