/*
 * test-holdings.c - What each wallet, till or character holds
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One organization keeps its books in GOLD and also deals in TICKET (memo:
 * counted, never posted) and BREWFEST (a separate book). Two characters,
 * Aria and Bram, are locations under "Characters". A holding is an account
 * with a location: posted currencies move it through journal lines, memo
 * currencies through holding movements, and the holdings report adds the
 * two per location and currency. These tests pin every door that moves a
 * holding -- a session's money yield, a sale paid into one, an expense paid
 * out of one, a transfer, a hand adjustment -- and the floor that stops a
 * holding being spent below zero.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*db;
	VentureContext	*context;
	gint64		 org;
	gint64		 venture;
	gint64		 characters;
	gint64		 aria;
	gint64		 bram;
} Fixture;

static void
save(Fixture *f, gpointer record)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

/* Saves @record expecting a refusal that mentions @fragment. */
static void
save_refused(Fixture *f, gpointer record, const gchar *fragment)
{
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = venture_database_save(f->db, VENTURE_ENTITY(record), NULL, &error);
	g_assert_false(ok);
	g_assert_nonnull(error);

	if (NULL == strstr(error->message, fragment))
		g_error("expected \"%s\" in: %s", fragment, error->message);
}

static void
field(gpointer record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record_in(Fixture *f, gint64 org, const gchar *name)
{
	VentureEntity *entity;

	(void)f;
	entity = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(entity);
	venture_entity_set_organization_id(entity, org);
	return entity;
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	return record_in(f, f->org, name);
}

static void
define_currency(Fixture *f, const gchar *code, gint64 exponent, const gchar *treatment)
{
	g_autoptr(VentureEntity) currency = record(f, "currency");

	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", treatment);
	save(f, currency);
}

static gint64
location_in(Fixture *f, gint64 org, const gchar *name, gint64 parent)
{
	g_autoptr(VentureEntity) place = record_in(f, org, "location");

	g_object_set(place, "name", name, "parent-id", parent, "kind", "character", NULL);
	save(f, place);
	return venture_entity_get_id(place);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();
	g_autoptr(VentureEntity) organization = NULL;

	(void)data;
	venture_currency_clear_registered();
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(venture, "name", "Evermoor", "venture-type", "books", "organization-id", f->org, NULL);
	save(f, venture);
	f->venture = venture_entity_get_id(VENTURE_ENTITY(venture));

	define_currency(f, "GOLD", 4, "valued");
	define_currency(f, "TICKET", 0, "memo");
	define_currency(f, "BREWFEST", 0, "separate_book");
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, &error);
	g_assert_no_error(error);
	g_object_set(organization, "default-currency", "GOLD", NULL);
	save(f, organization);

	f->characters = location_in(f, f->org, "Characters", 0);
	f->aria = location_in(f, f->org, "Aria", f->characters);
	f->bram = location_in(f, f->org, "Bram", f->characters);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_currency_clear_registered();

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

static gint64
int_of(Fixture *f, GType type, gint64 id, const gchar *property)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	gint64 value = 0;

	row = venture_database_get(f->db, type, id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_object_get(row, property, &value, NULL);
	return value;
}

static guint
count_rows(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;

	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows->len;
}

/* The location's holding account, made on first use. */
static gint64
holding(Fixture *f, gint64 location)
{
	g_autoptr(GError) error = NULL;
	gint64 account = 0;

	g_assert_true(venture_holdings_account_for_location(f->db, f->org, location, TRUE,
	                                                    NULL, &account, &error));
	g_assert_no_error(error);
	g_assert_cmpint(account, >, 0);
	return account;
}

/* What @location's holding holds in @currency, in minor units. */
static gint64
held(Fixture *f, gint64 location, const gchar *currency)
{
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GError) error = NULL;

	balance = venture_holdings_balance(f->db, holding(f, location), currency, 0, &error);
	g_assert_no_error(error);
	g_assert_nonnull(balance);
	g_assert_cmpstr(venture_money_get_currency(balance), ==, currency);
	return venture_money_get_amount(balance);
}

/* A finished session at @location with one money yield; returns the yield. */
static gint64
earn_in_session(Fixture *f, gint64 location, const gchar *amount, gint64 *out_session)
{
	g_autoptr(VentureEntity) session = record(f, "session");
	g_autoptr(VentureEntity) yield = record(f, "session_yield");

	g_object_set(session, "name", "Faire loop", "activity", "faire",
	             "location-id", location, NULL);
	field(session, "started-at", "2026-03-01T10:00:00Z");
	field(session, "ended-at", "2026-03-01T11:00:00Z");
	save(f, session);
	g_object_set(yield, "session-id", venture_entity_get_id(session), NULL);
	field(yield, "amount", amount);
	save(f, yield);

	if (NULL != out_session)
		*out_session = venture_entity_get_id(session);

	return venture_entity_get_id(yield);
}

static guint
post_session(Fixture *f, gint64 session)
{
	g_autoptr(GError) error = NULL;
	guint posted = 0;

	g_assert_true(venture_sessions_post(f->db, session, NULL, &posted, NULL, &error));
	g_assert_no_error(error);
	return posted;
}

static VentureEntity *
expense_from(Fixture *f, gint64 location, const gchar *amount)
{
	VentureEntity *expense = record(f, "expense");

	g_object_set(expense, "description", "Prize from the faire", "venture-id", f->venture,
	             "cash-account-id", holding(f, location), NULL);
	field(expense, "occurred-at", "2026-03-02");
	field(expense, "amount", amount);
	return expense;
}

static VentureEntity *
sale_into(Fixture *f, gint64 location, const gchar *gross)
{
	VentureEntity *sale = record(f, "sale");

	g_object_set(sale, "venture-id", f->venture, "cash-account-id", holding(f, location), NULL);
	field(sale, "occurred-at", "2026-03-03");
	field(sale, "gross", gross);
	return sale;
}

static VentureReportResult *
run_holdings(Fixture *f, gint64 org, gint64 location, const gchar *currency)
{
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;

	period = venture_context_parse_period(f->context, "all", &error);
	g_assert_no_error(error);
	json_object_set_int_member(options, "organization_id", org);

	if (location > 0)
		json_object_set_int_member(options, "location_id", location);

	if (NULL != currency)
		json_object_set_string_member(options, "currency", currency);

	result = venture_holdings_report(f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

/* The row for @location_path and @currency, or -1. */
static gint
row_of(VentureReportResult *result, const gchar *location_path, const gchar *currency)
{
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		const GValue *where = venture_report_result_get_cell(result, i, "location");
		const GValue *code = venture_report_result_get_cell(result, i, "currency");

		if ((0 == g_strcmp0(g_value_get_string(where), location_path)) &&
		    (0 == g_strcmp0(g_value_get_string(code), currency)))
			return (gint)i;
	}

	return -1;
}

/* A money cell's minor units, asserting its currency. */
static gint64
cell_amount(VentureReportResult *result, gint row, const gchar *key, const gchar *currency)
{
	const GValue *value = venture_report_result_get_cell(result, (guint)row, key);
	const VentureMoney *money;

	g_assert_nonnull(value);
	money = g_value_get_boxed(value);
	g_assert_nonnull(money);
	g_assert_cmpstr(venture_money_get_currency(money), ==, currency);
	return venture_money_get_amount(money);
}

/* ------------------------------------------------------------------------ */

/*
 * A memo currency earned in a session lands in the session location's
 * holding as a movement -- no journal, since memo never posts -- and the
 * report shows it for that character with the treatment named. Posting
 * again posts nothing. If this regresses, tickets won at the faire are
 * held by nobody, or held twice after a retried post.
 */
static void
test_session_memo(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *books = NULL;
	gint64 session;
	gint64 yield;
	gint row;

	(void)data;
	yield = earn_in_session(f, f->aria, "12 TICKET", &session);
	g_assert_cmpuint(post_session(f, session), ==, 1);

	g_assert_cmpint(int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "holding-txn-id"), >, 0);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "journal-id"), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, 0);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 12);
	g_assert_cmpint(held(f, f->bram, "TICKET"), ==, 0);

	/* Idempotent: the stamp is the whole of it. */
	g_assert_cmpuint(post_session(f, session), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_HOLDING_TXN), ==, 1);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 12);

	result = run_holdings(f, f->org, 0, NULL);
	row = row_of(result, "Characters / Aria", "TICKET");
	g_assert_cmpint(row, >=, 0);
	g_assert_cmpint(cell_amount(result, row, "earned", "TICKET"), ==, 12);
	g_assert_cmpint(cell_amount(result, row, "spent", "TICKET"), ==, 0);
	g_assert_cmpint(cell_amount(result, row, "balance", "TICKET"), ==, 12);
	books = venture_report_result_format_cell(result, (guint)row, "books");
	g_assert_nonnull(strstr(books, "Memo: TICKET"));
}

/*
 * A separate-book currency earned in a session posts its own balanced
 * journal -- holding debited, session income credited, in BREWFEST -- and
 * the yield carries it. If this regresses, tokens are either counted with
 * no evidence in their book or mixed into the gold one.
 */
static void
test_session_separate_book(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) journal = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *books = NULL;
	gint64 session;
	gint64 yield;
	gint row;

	(void)data;
	yield = earn_in_session(f, f->aria, "7 BREWFEST", &session);
	g_assert_cmpuint(post_session(f, session), ==, 1);

	g_assert_cmpint(int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "journal-id"), >, 0);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "holding-txn-id"), ==, 0);
	journal = venture_database_get(f->db, VENTURE_TYPE_JOURNAL,
		int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "journal-id"), NULL);
	g_assert_nonnull(journal);
	g_object_get(journal, "currency", &currency, NULL);
	g_assert_cmpstr(currency, ==, "BREWFEST");
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_HOLDING_TXN), ==, 0);
	g_assert_cmpint(held(f, f->aria, "BREWFEST"), ==, 7);

	g_assert_cmpuint(post_session(f, session), ==, 0);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, 1);

	result = run_holdings(f, f->org, f->aria, NULL);
	row = row_of(result, "Characters / Aria", "BREWFEST");
	g_assert_cmpint(row, >=, 0);
	g_assert_cmpint(cell_amount(result, row, "balance", "BREWFEST"), ==, 7);
	books = venture_report_result_format_cell(result, (guint)row, "books");
	g_assert_nonnull(strstr(books, "Separate book: BREWFEST"));
}

/*
 * A gold sale paid into a character's purse: the sale's cash account is
 * honoured the way an expense's always was, so the gold lands in Aria's
 * holding and not the organization's cash. Gold comes first among Aria's
 * rows. If this regresses, every sale lands in 1000 Cash and nobody's
 * purse ever fills.
 */
static void
test_sale_into_holding(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	gint64 session;
	gint row_gold;
	gint row_ticket;

	(void)data;
	earn_in_session(f, f->aria, "3 TICKET", &session);
	post_session(f, session);
	sale = sale_into(f, f->aria, "30 GOLD");
	save(f, sale);

	g_assert_cmpint(held(f, f->aria, "GOLD"), ==, 300000);
	g_assert_cmpint(held(f, f->bram, "GOLD"), ==, 0);

	/* A memo sale into the purse is a movement, earned. */
	g_clear_object(&sale);
	sale = sale_into(f, f->aria, "2 TICKET");
	save(f, sale);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 5);

	result = run_holdings(f, f->org, 0, NULL);
	row_gold = row_of(result, "Characters / Aria", "GOLD");
	row_ticket = row_of(result, "Characters / Aria", "TICKET");
	g_assert_cmpint(row_gold, >=, 0);
	g_assert_cmpint(row_gold, <, row_ticket);
	g_assert_cmpint(cell_amount(result, row_gold, "earned", "GOLD"), ==, 300000);
}

/*
 * Spending tickets on a purchase paid from a character's purse takes them
 * out of the holding; editing the expense replaces its movement rather
 * than adding a second; spending more than is held is refused and writes
 * nothing. If this regresses, a re-saved expense spends twice, or a
 * character buys a prize with tickets it never had.
 */
static void
test_spend_from_holding(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) expense = NULL;
	g_autoptr(VentureEntity) greedy = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	gint64 session;
	guint expenses;
	gint row;

	(void)data;
	earn_in_session(f, f->aria, "12 TICKET", &session);
	post_session(f, session);

	expense = expense_from(f, f->aria, "5 TICKET");
	save(f, expense);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 7);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_HOLDING_TXN), ==, 2);

	/* A note changes nothing; a new amount replaces the movement. */
	g_object_set(expense, "notes", "the ribbon", NULL);
	save(f, expense);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_HOLDING_TXN), ==, 2);
	field(expense, "amount", "6 TICKET");
	save(f, expense);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 6);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_HOLDING_TXN), ==, 2);

	expenses = count_rows(f, VENTURE_TYPE_EXPENSE);
	greedy = expense_from(f, f->aria, "50 TICKET");
	save_refused(f, greedy, "below zero");
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_EXPENSE), ==, expenses);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 6);

	result = run_holdings(f, f->org, 0, "TICKET");
	row = row_of(result, "Characters / Aria", "TICKET");
	g_assert_cmpint(cell_amount(result, row, "earned", "TICKET"), ==, 12);
	g_assert_cmpint(cell_amount(result, row, "spent", "TICKET"), ==, 6);
	g_assert_cmpint(cell_amount(result, row, "balance", "TICKET"), ==, 6);
	g_assert_cmpint(cell_amount(result, row, "net", "TICKET"), ==, 6);
}

/*
 * The floor holds for posted currencies too, through the posting guard,
 * and a ticked Allow negative lifts it. An ordinary account with no
 * location is never judged -- an overdraft is a real balance. By hand, a
 * person writes memo adjustments only, never below zero, and never a
 * posted currency. If this regresses, BREWFEST is spent that was never
 * earned, or an ordinary cash account starts refusing payments.
 */
static void
test_floor(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) spend = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureEntity) overdraft = NULL;
	g_autoptr(VentureEntity) adjust = NULL;
	g_autoptr(VentureEntity) gold = NULL;
	gint64 session;

	(void)data;
	earn_in_session(f, f->bram, "7 BREWFEST", &session);
	post_session(f, session);

	spend = expense_from(f, f->bram, "20 BREWFEST");
	save_refused(f, spend, "below zero");
	g_assert_cmpint(held(f, f->bram, "BREWFEST"), ==, 7);

	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, holding(f, f->bram), NULL);
	g_object_set(account, "allow-negative", TRUE, NULL);
	save(f, account);
	g_clear_object(&spend);
	spend = expense_from(f, f->bram, "20 BREWFEST");
	save(f, spend);
	g_assert_cmpint(held(f, f->bram, "BREWFEST"), ==, -13);

	/* No location: an expense from 1000 Cash goes negative as ever. */
	overdraft = record(f, "expense");
	g_object_set(overdraft, "description", "Repairs", "venture-id", f->venture, NULL);
	field(overdraft, "occurred-at", "2026-03-02");
	field(overdraft, "amount", "500 GOLD");
	save(f, overdraft);

	/* By hand: a memo adjustment may not take Aria below zero... */
	adjust = record(f, "holding_txn");
	g_object_set(adjust, "account-id", holding(f, f->aria), NULL);
	field(adjust, "amount", "-3 TICKET");
	save_refused(f, adjust, "below zero");
	field(adjust, "amount", "3 TICKET");
	save(f, adjust);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 3);

	/* ...and a posted currency is the journal's, not a movement's. */
	gold = record(f, "holding_txn");
	g_object_set(gold, "account-id", holding(f, f->aria), NULL);
	field(gold, "amount", "5 GOLD");
	save_refused(f, gold, "which the ledger posts");

	/* Nor may a person write one side of a transfer. */
	g_clear_object(&gold);
	gold = record(f, "holding_txn");
	g_object_set(gold, "account-id", holding(f, f->aria), "kind",
	             VENTURE_HOLDING_KIND_TRANSFER, NULL);
	field(gold, "amount", "1 TICKET");
	save_refused(f, gold, "Transfer action");
}

/* A hand adjustment of @amount to @location's holding on @date, saved. */
static gint64
adjust_on(Fixture *f, gint64 location, const gchar *amount, const gchar *date)
{
	g_autoptr(VentureEntity) movement = record(f, "holding_txn");

	g_object_set(movement, "account-id", holding(f, location), NULL);
	field(movement, "amount", amount);
	field(movement, "occurred-at", date);
	save(f, movement);
	return venture_entity_get_id(movement);
}

/* The same, expecting the floor to refuse it. */
static void
adjust_refused(Fixture *f, gint64 location, const gchar *amount, const gchar *date)
{
	g_autoptr(VentureEntity) movement = record(f, "holding_txn");

	g_object_set(movement, "account-id", holding(f, location), NULL);
	field(movement, "amount", amount);
	field(movement, "occurred-at", date);
	save_refused(f, movement, "below zero");
}

/*
 * The floor is the running balance, not today's. Aria wins 12 tickets on
 * 1 March; a spend dated in February is refused although she holds 12
 * now, because in February she held none -- through an expense (a memo
 * movement), through a posted currency's journal, by hand, by moving a
 * spend earlier, and by deleting an earlier earning that later spends
 * leaned on. A moment already below zero -- left by a period the account
 * allowed it -- does not block a change that leaves it as it was, but
 * one that deepens it is refused. If this regresses, a back-dated
 * purchase spends tickets a character did not have yet, and the history
 * shows a purse below zero that nothing ever allowed.
 */
static void
test_floor_back_dated(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) early = NULL;
	g_autoptr(VentureEntity) late = NULL;
	g_autoptr(VentureEntity) moved = NULL;
	g_autoptr(VentureEntity) earning = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GError) error = NULL;
	gint64 session;
	gint64 spend;
	gint64 lent;

	(void)data;
	earn_in_session(f, f->aria, "12 TICKET", &session);
	post_session(f, session);

	/* A memo expense dated before the takings: refused, then accepted a
	 * day after them. */
	early = expense_from(f, f->aria, "5 TICKET");
	field(early, "occurred-at", "2026-02-15");
	save_refused(f, early, "below zero");
	field(early, "occurred-at", "2026-03-02");
	save(f, early);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 7);

	/* A posted currency, through the posting guard. */
	earn_in_session(f, f->bram, "7 BREWFEST", &session);
	post_session(f, session);
	late = expense_from(f, f->bram, "3 BREWFEST");
	field(late, "occurred-at", "2026-02-20");
	save_refused(f, late, "below zero");
	field(late, "occurred-at", "2026-03-02");
	save(f, late);
	g_assert_cmpint(held(f, f->bram, "BREWFEST"), ==, 4);

	/* By hand, and by moving a spend earlier than the takings. */
	adjust_refused(f, f->aria, "-3 TICKET", "2026-02-01");
	spend = adjust_on(f, f->aria, "-2 TICKET", "2026-03-05");
	moved = venture_database_get(f->db, VENTURE_TYPE_HOLDING_TXN, spend, NULL);
	field(moved, "occurred-at", "2026-02-01");
	save_refused(f, moved, "below zero");
	g_clear_object(&moved);
	moved = venture_database_get(f->db, VENTURE_TYPE_HOLDING_TXN, spend, NULL);
	g_object_set(moved, "notes", "the goldfish", NULL);
	save(f, moved);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 5);

	/* Deleting an earning a later spend leaned on: 10 lent on 1 January
	 * and 8 spent on the 2nd. Today's balance would survive losing the
	 * 10 (the faire's 12 came later), but 2 January would not. */
	lent = adjust_on(f, f->aria, "10 TICKET", "2026-01-01");
	adjust_on(f, f->aria, "-8 TICKET", "2026-01-02");
	earning = venture_database_get(f->db, VENTURE_TYPE_HOLDING_TXN, lent, NULL);
	g_assert_false(venture_database_delete(f->db, earning, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "below zero"));
	g_clear_error(&error);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 7);

	/* A dip the account once allowed: Bram overspends in January while
	 * overdrafts were allowed, then they are not. A spend in April, when
	 * he holds plenty, still goes through; one that deepens January is
	 * refused. */
	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, holding(f, f->bram), NULL);
	g_object_set(account, "allow-negative", TRUE, NULL);
	save(f, account);
	g_clear_object(&late);
	late = expense_from(f, f->bram, "1 BREWFEST");
	field(late, "occurred-at", "2026-01-05");
	save(f, late);
	g_object_set(account, "allow-negative", FALSE, NULL);
	save(f, account);
	g_clear_object(&late);
	late = expense_from(f, f->bram, "1 BREWFEST");
	field(late, "occurred-at", "2026-04-01");
	save(f, late);
	g_clear_object(&late);
	late = expense_from(f, f->bram, "1 BREWFEST");
	field(late, "occurred-at", "2026-01-06");
	save_refused(f, late, "below zero");
	g_assert_cmpint(held(f, f->bram, "BREWFEST"), ==, 2);
}

/*
 * A movement the ledger derived from a document is the document's: its
 * amount cannot be edited nor the row deleted by hand; its notes can. If
 * this regresses, a character's tickets can be rewritten with no sale or
 * session saying so.
 */
static void
test_derived_frozen(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) movement = NULL;
	g_autoptr(GError) error = NULL;
	gint64 session;
	gint64 yield;

	(void)data;
	yield = earn_in_session(f, f->aria, "12 TICKET", &session);
	post_session(f, session);
	movement = venture_database_get(f->db, VENTURE_TYPE_HOLDING_TXN,
		int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "holding-txn-id"), NULL);
	g_assert_nonnull(movement);

	field(movement, "amount", "99 TICKET");
	save_refused(f, movement, "came from session");
	g_clear_object(&movement);
	movement = venture_database_get(f->db, VENTURE_TYPE_HOLDING_TXN,
		int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "holding-txn-id"), NULL);
	g_object_set(movement, "notes", "a good day", NULL);
	save(f, movement);

	g_assert_false(venture_database_delete(f->db, movement, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "came from session"));
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 12);

	/* And the posted money yield cannot be deleted from under it. */
	g_clear_error(&error);
	{
		g_autoptr(VentureEntity) posted = venture_database_get(f->db,
			VENTURE_TYPE_SESSION_YIELD, yield, NULL);

		g_assert_false(venture_database_delete(f->db, posted, NULL, &error));
		g_assert_nonnull(error);
		g_assert_nonnull(strstr(error->message, "holding"));
	}
}

/* Whether any note on @result contains @fragment. */
static gboolean
has_note(VentureReportResult *result, const gchar *fragment)
{
	g_autoptr(JsonNode) node = venture_report_result_to_json(result);
	g_autofree gchar *text = venture_json_to_string(node, FALSE);

	return NULL != strstr(text, fragment);
}

/*
 * Deleting a document keeps what it moved, the rule a posted sale's
 * journal has always followed: deletion is not a financial correction. A
 * memo sale into Aria's purse, deleted, still counts in her tickets, and
 * so does a separate-book sale's journal -- and the holdings report says
 * which deleted documents it is counting, so a reader looking for them
 * does not take the balance for wrong. The movement's own refusal no
 * longer claims deleting the document removes it. If this regresses, the
 * report shows a balance nobody can trace, or the refusal sends a person
 * to delete the sale expecting the tickets to go.
 */
static void
test_deleted_document(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(VentureEntity) tokens = NULL;
	g_autoptr(VentureEntity) movement = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sale_label = NULL;
	g_autofree gchar *tokens_label = NULL;

	(void)data;
	sale = sale_into(f, f->aria, "2 TICKET");
	save(f, sale);
	tokens = sale_into(f, f->aria, "3 BREWFEST");
	save(f, tokens);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 2);
	g_assert_cmpint(held(f, f->aria, "BREWFEST"), ==, 3);

	result = run_holdings(f, f->org, 0, NULL);
	g_assert_false(has_note(result, "deleted"));
	g_clear_object(&result);

	g_assert_true(venture_database_delete(f->db, sale, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(f->db, tokens, NULL, &error));
	g_assert_no_error(error);

	/* Kept, as the journal is. */
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 2);
	g_assert_cmpint(held(f, f->aria, "BREWFEST"), ==, 3);

	result = run_holdings(f, f->org, 0, NULL);
	sale_label = g_strdup_printf("sale #%" G_GINT64_FORMAT, venture_entity_get_id(sale));
	tokens_label = g_strdup_printf("sale #%" G_GINT64_FORMAT, venture_entity_get_id(tokens));
	g_assert_true(has_note(result, "2 deleted documents still count here"));
	g_assert_true(has_note(result, sale_label));
	g_assert_true(has_note(result, tokens_label));

	/* The movement is still the sale's, and says how to take it back. */
	query = venture_query_new(VENTURE_TYPE_HOLDING_TXN);
	venture_query_add_filter_string(query, "source-type", VENTURE_FILTER_OP_EQ, "sale", NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	movement = g_object_ref(g_ptr_array_index(rows, 0));
	g_assert_false(venture_database_delete(f->db, movement, NULL, &error));
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, "the ledger removes it"));
	g_assert_nonnull(strstr(error->message, "adjustment"));
}

/*
 * A transfer moves a memo currency as a pair of transfer movements and a
 * posted one as one journal under the transfer rule; the report counts
 * both as transferred, not earned or spent. The action is how every door
 * does it, and it refuses to leave the source below zero, writing
 * nothing. If this regresses, a move between characters reads as income
 * for one and spending for the other, or drains a purse past empty.
 */
static void
test_transfer(Fixture *f, gconstpointer data)
{
	VentureActionRegistry *registry;
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(VentureMoney) four = NULL;
	g_autoptr(GError) error = NULL;
	gint64 session;
	guint journals;
	guint movements;
	gint row;

	(void)data;
	earn_in_session(f, f->aria, "12 TICKET", &session);
	post_session(f, session);
	earn_in_session(f, f->aria, "7 BREWFEST", &session);
	post_session(f, session);

	four = venture_money_from_string("4 TICKET", NULL, NULL);
	g_assert_true(venture_holdings_transfer(f->db, f->aria, f->bram, four, NULL, "for the ride",
	                                        NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 8);
	g_assert_cmpint(held(f, f->bram, "TICKET"), ==, 4);

	/* Through the action, as the page and venturectl act do. */
	registry = venture_database_get_action_registry(f->db);
	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	g_hash_table_insert(params, g_strdup("to_location_id"),
	                    json_node_init_int(json_node_alloc(), f->bram));
	g_hash_table_insert(params, g_strdup("amount"),
	                    json_node_init_string(json_node_alloc(), "3 BREWFEST"));
	result = venture_action_registry_perform(registry, "location", f->aria, "transfer",
	                                         params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(held(f, f->aria, "BREWFEST"), ==, 4);
	g_assert_cmpint(held(f, f->bram, "BREWFEST"), ==, 3);

	/* More than Aria holds: refused, nothing written. */
	journals = count_rows(f, VENTURE_TYPE_JOURNAL);
	movements = count_rows(f, VENTURE_TYPE_HOLDING_TXN);
	g_hash_table_insert(params, g_strdup("amount"),
	                    json_node_init_string(json_node_alloc(), "40 BREWFEST"));
	g_clear_object(&result);
	result = venture_action_registry_perform(registry, "location", f->aria, "transfer",
	                                         params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "below zero"));
	g_clear_error(&error);
	g_hash_table_insert(params, g_strdup("amount"),
	                    json_node_init_string(json_node_alloc(), "40 TICKET"));
	result = venture_action_registry_perform(registry, "location", f->aria, "transfer",
	                                         params, NULL, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_JOURNAL), ==, journals);
	g_assert_cmpuint(count_rows(f, VENTURE_TYPE_HOLDING_TXN), ==, movements);

	report = run_holdings(f, f->org, 0, NULL);
	row = row_of(report, "Characters / Aria", "TICKET");
	g_assert_cmpint(cell_amount(report, row, "earned", "TICKET"), ==, 12);
	g_assert_cmpint(cell_amount(report, row, "spent", "TICKET"), ==, 0);
	g_assert_cmpint(cell_amount(report, row, "transfers", "TICKET"), ==, -4);
	row = row_of(report, "Characters / Bram", "TICKET");
	g_assert_cmpint(cell_amount(report, row, "earned", "TICKET"), ==, 0);
	g_assert_cmpint(cell_amount(report, row, "transfers", "TICKET"), ==, 4);
	row = row_of(report, "Characters / Bram", "BREWFEST");
	g_assert_cmpint(cell_amount(report, row, "transfers", "BREWFEST"), ==, 3);
	g_assert_cmpint(cell_amount(report, row, "balance", "BREWFEST"), ==, 3);

	/* The subtree: Characters holds both, Bram only his own. */
	g_clear_object(&report);
	report = run_holdings(f, f->org, f->bram, NULL);
	g_assert_cmpint(row_of(report, "Characters / Aria", "TICKET"), <, 0);
	g_assert_cmpint(row_of(report, "Characters / Bram", "TICKET"), >=, 0);
	g_clear_object(&report);
	report = run_holdings(f, f->org, f->characters, NULL);
	g_assert_cmpint(row_of(report, "Characters / Aria", "TICKET"), >=, 0);
	g_assert_cmpint(row_of(report, "Characters / Bram", "TICKET"), >=, 0);
}

/*
 * Holdings are an organization's own: the report reads only its accounts,
 * a location of another organization is not found, and neither a
 * movement nor a transfer may reach across. If this regresses, one
 * business's report shows another's purses.
 */
static void
test_organization_scope(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureReportResult) report = NULL;
	g_autoptr(VentureEntity) foreign = NULL;
	g_autoptr(VentureMoney) one = NULL;
	g_autoptr(GError) error = NULL;
	gint64 theirs;
	gint64 their_account = 0;
	gint64 session;

	(void)data;
	g_object_set(other, "name", "Elsewhere", "slug", "elsewhere", NULL);
	save(f, other);
	theirs = location_in(f, venture_entity_get_id(VENTURE_ENTITY(other)), "Stranger", 0);
	g_assert_true(venture_holdings_account_for_location(f->db,
		venture_entity_get_id(VENTURE_ENTITY(other)), theirs, TRUE, NULL, &their_account, &error));
	g_assert_no_error(error);

	earn_in_session(f, f->aria, "12 TICKET", &session);
	post_session(f, session);

	report = run_holdings(f, f->org, 0, NULL);
	g_assert_cmpint(row_of(report, "Stranger", "TICKET"), <, 0);

	{
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(VentureReportResult) refused = NULL;

		json_object_set_int_member(options, "organization_id", f->org);
		json_object_set_int_member(options, "location_id", theirs);
		refused = venture_holdings_report(f->context, NULL, options, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_clear_error(&error);
	}

	/* Not a holding of ours: the account-for-location refuses it. */
	g_assert_false(venture_holdings_account_for_location(f->db, f->org, theirs, TRUE, NULL,
	                                                     &their_account, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	one = venture_money_from_string("1 TICKET", NULL, NULL);
	g_assert_false(venture_holdings_transfer(f->db, f->aria, theirs, one, NULL, NULL, NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 12);

	foreign = record(f, "holding_txn");
	g_assert_true(venture_holdings_account_for_location(f->db,
		venture_entity_get_id(VENTURE_ENTITY(other)), theirs, FALSE, NULL, &their_account, &error));
	g_object_set(foreign, "account-id", their_account, NULL);
	field(foreign, "amount", "5 TICKET");
	save_refused(f, foreign, "another organization");
}

/*
 * With the ledger off there are no holdings: the report is hidden and
 * refuses, and a session's goods still post while its money yield stays
 * an unposted record, as it was before holdings existed. If this
 * regresses, switching the ledger off breaks posting sessions, or writes
 * to tables nobody can see.
 */
static void
test_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) refused = NULL;
	gint64 session;
	gint64 yield;

	(void)data;
	yield = earn_in_session(f, f->aria, "12 TICKET", &session);
	venture_config_set_module_enabled(f->config, "ledger", FALSE);
	g_assert_false(venture_context_module_enabled(f->context, "ledger"));

	g_assert_null(venture_report_registry_lookup(venture_context_get_report_registry(f->context),
	                                             "holdings"));
	refused = venture_holdings_report(f->context, NULL, NULL, &error);
	g_assert_null(refused);
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_assert_cmpuint(post_session(f, session), ==, 0);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "holding-txn-id"), ==, 0);
	g_assert_cmpint(int_of(f, VENTURE_TYPE_SESSION_YIELD, yield, "journal-id"), ==, 0);

	/* On again: the same yield posts now, once. */
	venture_config_set_module_enabled(f->config, "ledger", TRUE);
	g_assert_cmpuint(post_session(f, session), ==, 1);
	g_assert_cmpint(held(f, f->aria, "TICKET"), ==, 12);
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

	g_setenv("VENTURE_TEST_SESSION_SECRET", "holdings-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-holdings-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 13901) % 20000));

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
 * Every door reaches holdings: the report API forwards location_id (an
 * option dropped on the way answers for every character instead of one),
 * the report page offers it, and the transfer action runs through the
 * generic action route. What breaks: the fifth door that quietly never
 * passes the question.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) currency = NULL;
	g_autoptr(VentureEntity) session = NULL;
	g_autoptr(VentureEntity) yield = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *request = NULL;
	gint64 org;
	gint64 ids[3];
	guint posted;
	guint i;
	static const gchar *const names[] = { "Characters", "Aria", "Bram" };

	(void)user_data;
	org = venture_context_get_default_organization_id(fixture->context);
	currency = venture_entity_registry_create(venture_entity_registry_get_default(), "currency", NULL);
	venture_entity_set_organization_id(currency, org);
	g_object_set(currency, "code", "TICKET", "name", "Ticket", "exponent", (gint64)0,
	             "book-treatment", VENTURE_BOOK_TREATMENT_MEMO, NULL);
	g_assert_true(venture_database_save(fixture->database, currency, NULL, &error));
	g_assert_no_error(error);

	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autoptr(VentureEntity) place = venture_entity_registry_create(
			venture_entity_registry_get_default(), "location", NULL);

		venture_entity_set_organization_id(place, org);
		g_object_set(place, "name", names[i], "parent-id", (i == 0) ? (gint64)0 : ids[0], NULL);
		g_assert_true(venture_database_save(fixture->database, place, NULL, &error));
		g_assert_no_error(error);
		ids[i] = venture_entity_get_id(place);
	}

	session = venture_entity_registry_create(venture_entity_registry_get_default(), "session", NULL);
	venture_entity_set_organization_id(session, org);
	g_object_set(session, "name", "Faire", "location-id", ids[1], NULL);
	g_assert_true(venture_entity_set_field_from_string(session, "started-at", "2026-03-01T10:00:00Z", NULL));
	g_assert_true(venture_entity_set_field_from_string(session, "ended-at", "2026-03-01T11:00:00Z", NULL));
	g_assert_true(venture_database_save(fixture->database, session, NULL, &error));
	g_assert_no_error(error);
	yield = venture_entity_registry_create(venture_entity_registry_get_default(), "session_yield", NULL);
	venture_entity_set_organization_id(yield, org);
	g_object_set(yield, "session-id", venture_entity_get_id(session), NULL);
	g_assert_true(venture_entity_set_field_from_string(yield, "amount", "12 TICKET", NULL));
	g_assert_true(venture_database_save(fixture->database, yield, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_sessions_post(fixture->database, venture_entity_get_id(session),
	                                    NULL, &posted, NULL, &error));
	g_assert_no_error(error);

	/* The action through the generic route: Aria gives Bram 5. */
	path = g_strdup_printf("/api/v1/location/%" G_GINT64_FORMAT "/actions/transfer", ids[1]);
	request = g_strdup_printf("{\"to_location_id\": %" G_GINT64_FORMAT ", \"amount\": \"5 TICKET\"}",
	                          ids[2]);
	g_assert_cmpuint(server_request(fixture, "POST", path, "application/json", request, &body),
		==, SOUP_STATUS_OK);

	/* location_id reaches the report: Bram's subtree has no Aria. */
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/reports/holdings?period=all&location_id=%" G_GINT64_FORMAT, ids[2]);
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &body), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "Characters / Bram"));
	g_assert_null(strstr(body, "Characters / Aria"));

	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "GET", "/api/v1/reports/holdings?period=all",
		NULL, NULL, &body), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "Characters / Aria"));

	/* The page offers the question. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(server_request(fixture, "GET", "/reports/holdings?period=all",
		NULL, NULL, &body), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "name=\"location_id\""));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/holdings/session-memo", Fixture, NULL, setup, test_session_memo, teardown);
	g_test_add("/holdings/session-separate-book", Fixture, NULL, setup,
	           test_session_separate_book, teardown);
	g_test_add("/holdings/sale-into-holding", Fixture, NULL, setup, test_sale_into_holding, teardown);
	g_test_add("/holdings/spend-from-holding", Fixture, NULL, setup, test_spend_from_holding, teardown);
	g_test_add("/holdings/floor", Fixture, NULL, setup, test_floor, teardown);
	g_test_add("/holdings/floor-back-dated", Fixture, NULL, setup, test_floor_back_dated,
	           teardown);
	g_test_add("/holdings/derived-frozen", Fixture, NULL, setup, test_derived_frozen, teardown);
	g_test_add("/holdings/deleted-document", Fixture, NULL, setup, test_deleted_document,
	           teardown);
	g_test_add("/holdings/transfer", Fixture, NULL, setup, test_transfer, teardown);
	g_test_add("/holdings/organization-scope", Fixture, NULL, setup, test_organization_scope, teardown);
	g_test_add("/holdings/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/holdings/http", ServerFixture, NULL, server_fixture_set_up, test_http,
	           server_fixture_tear_down);

	return g_test_run();
}
