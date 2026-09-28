/*
 * test-currency-books.c - How each currency reaches the general ledger
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One organization keeps its books in GOLD (a user-defined currency with
 * four decimals) and also deals in TICKET (no decimals), which may or may
 * not have a rate to GOLD. The posting service decides, in one place, what
 * a TICKET amount does: valued with a rate, it is converted into the GOLD
 * journal and keeps its original amount; valued with no rate, or kept as a
 * separate book, it posts a balanced TICKET journal; memo, it never posts.
 * A document whose lines land in two journals balances each through the
 * currency clearing account. These tests pin that rule through every door
 * that posts: an automatic sale or expense, a manual journal, a saved
 * draft, and the call the inventory service uses.
 */

#include <venture.h>

#include <string.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*db;
	VentureContext	*context;
	gint64		 org;
	gint64		 venture;
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

static void
field(gpointer record, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, value, &error));
	g_assert_no_error(error);
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *entity;

	entity = venture_entity_registry_create(venture_entity_registry_get_default(), name, NULL);
	g_assert_nonnull(entity);
	venture_entity_set_organization_id(entity, f->org);
	return entity;
}

/* A currency record with the given treatment, loaded into the registry by
 * the save's audit signal like any other. */
static void
define_currency(Fixture *f, const gchar *code, gint64 exponent, const gchar *treatment)
{
	g_autoptr(VentureEntity) currency = record(f, "currency");

	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", treatment);
	save(f, currency);
}

static void
set_treatment(Fixture *f, const gchar *code, const gchar *treatment)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CURRENCY);
	g_autoptr(VentureEntity) currency = NULL;
	g_autoptr(GError) error = NULL;

	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	currency = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(currency);
	field(currency, "book-treatment", treatment);
	save(f, currency);
}

/* Records that one @from is worth @numerator/@denominator of @to, from
 * 1 January 2026, in the organization's rate table. */
static void
rate(Fixture *f, const gchar *from, const gchar *to, gint64 numerator, gint64 denominator)
{
	g_autoptr(VentureEntity) row = record(f, "exchange_rate");

	g_object_set(row, "from-currency", from, "to-currency", to,
		"rate-numerator", numerator, "rate-denominator", denominator, NULL);
	field(row, "effective-at", "2026-01-01");
	save(f, row);
}

static void
set_book_currency(Fixture *f, const gchar *code)
{
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GError) error = NULL;

	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, &error);
	g_assert_no_error(error);
	g_object_set(organization, "default-currency", code, NULL);
	save(f, organization);
}

static void
setup_books(Fixture *f, const gchar *book)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();

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
	if (g_strcmp0(book, "GOLD") == 0)
		define_currency(f, "GOLD", 4, "valued");
	set_book_currency(f, book);
}

static void
setup_gold(Fixture *f, gconstpointer data)
{
	(void)data;
	setup_books(f, "GOLD");
}

static void
setup_usd(Fixture *f, gconstpointer data)
{
	(void)data;
	setup_books(f, "USD");
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	/* The registry is process-wide; the next test must not inherit it. */
	venture_currency_clear_registered();
}

static GPtrArray *
journals_for(Fixture *f, const gchar *type, gint64 id)
{
	g_autoptr(GError) error = NULL;
	GPtrArray *journals;

	journals = venture_posting_service_find_source(venture_database_get_posting_service(f->db),
		type, id, f->org, &error);
	g_assert_no_error(error);
	g_assert_nonnull(journals);
	return journals;
}

static GPtrArray *
lines_of(Fixture *f, gpointer journal)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(VENTURE_ENTITY(journal)), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static gchar *
currency_of(gpointer journal)
{
	gchar *currency = NULL;

	g_object_get(journal, "currency", &currency, NULL);
	return currency;
}

/* Asserts @line's original and book amounts, as display-independent
 * minor units and codes. */
static void
line_amounts(gpointer line, gint64 amount, const gchar *currency, gint64 book, const gchar *book_currency)
{
	g_autoptr(VentureMoney) original = NULL;
	g_autoptr(VentureMoney) valued = NULL;

	g_object_get(line, "amount", &original, "book-amount", &valued, NULL);
	g_assert_cmpstr(venture_money_get_currency(original), ==, currency);
	g_assert_cmpint(venture_money_get_amount(original), ==, amount);
	g_assert_cmpstr(venture_money_get_currency(valued), ==, book_currency);
	g_assert_cmpint(venture_money_get_amount(valued), ==, book);
}

static gint64
account_id(Fixture *f, gpointer line)
{
	gint64 id = 0;

	(void)f;
	g_object_get(line, "account-id", &id, NULL);
	return id;
}

static gchar *
account_name(Fixture *f, gpointer line)
{
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GError) error = NULL;
	gchar *name = NULL;

	account = venture_database_get(f->db, VENTURE_TYPE_ACCOUNT, account_id(f, line), &error);
	g_assert_no_error(error);
	g_object_get(account, "name", &name, NULL);
	return name;
}

static VentureEntity *
ticket_sale(Fixture *f, const gchar *gross)
{
	VentureEntity *sale = record(f, "sale");

	g_object_set(sale, "venture-id", f->venture, NULL);
	field(sale, "occurred-at", "2026-03-01");
	field(sale, "gross", gross);
	return sale;
}

/* ------------------------------------------------------------------------ */

/*
 * The treatment is read from the record and survives a delete, like the
 * rest of the registry. Every ISO code, and a code nobody defined, is
 * valued. If this regresses, the posting rule reads a stale or default
 * treatment and a separate-book currency is converted after all.
 */
static void
test_treatment_registry(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CURRENCY);
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	g_assert_cmpint(venture_currency_get_book_treatment("USD"), ==, VENTURE_BOOK_TREATMENT_VALUED);
	g_assert_cmpint(venture_currency_get_book_treatment("NOPE"), ==, VENTURE_BOOK_TREATMENT_VALUED);
	g_assert_cmpint(venture_currency_get_book_treatment("GOLD"), ==, VENTURE_BOOK_TREATMENT_VALUED);
	define_currency(f, "TICKET", 0, "memo");
	g_assert_cmpint(venture_currency_get_book_treatment("TICKET"), ==, VENTURE_BOOK_TREATMENT_MEMO);
	set_treatment(f, "TICKET", "separate_book");
	g_assert_cmpint(venture_currency_get_book_treatment("TICKET"), ==, VENTURE_BOOK_TREATMENT_SEPARATE_BOOK);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "TICKET", NULL);
	ticket = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(f->db, ticket, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_currency_get_book_treatment("TICKET"), ==, VENTURE_BOOK_TREATMENT_SEPARATE_BOOK);
}

/*
 * The helper the later phases build on answers the same rule the posting
 * does: book, converted with a rate, separate without one or when kept
 * apart, memo never.
 */
static void
test_route(Fixture *f, gconstpointer data)
{
	VenturePostingService *posting = venture_database_get_posting_service(f->db);
	g_autoptr(GDateTime) when = g_date_time_new_utc(2026, 3, 1, 0, 0, 0);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *book = NULL;
	VentureBookRoute route = VENTURE_BOOK_ROUTE_MEMO;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	define_currency(f, "BREWFEST", 0, "memo");
	g_assert_true(venture_posting_service_route_currency(posting, f->org, "GOLD", when, &route, &book, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(book, ==, "GOLD");
	g_assert_cmpint(route, ==, VENTURE_BOOK_ROUTE_BOOK);
	g_assert_true(venture_posting_service_route_currency(posting, f->org, "TICKET", when, &route, NULL, &error));
	g_assert_cmpint(route, ==, VENTURE_BOOK_ROUTE_SEPARATE);
	rate(f, "TICKET", "GOLD", 1, 2);
	g_assert_true(venture_posting_service_route_currency(posting, f->org, "TICKET", when, &route, NULL, &error));
	g_assert_cmpint(route, ==, VENTURE_BOOK_ROUTE_CONVERTED);
	/* A rate from before it existed does not reach back. */
	{
		g_autoptr(GDateTime) earlier = g_date_time_new_utc(2025, 12, 1, 0, 0, 0);

		g_assert_true(venture_posting_service_route_currency(posting, f->org, "TICKET", earlier, &route, NULL, &error));
		g_assert_cmpint(route, ==, VENTURE_BOOK_ROUTE_SEPARATE);
	}
	set_treatment(f, "TICKET", "separate_book");
	g_assert_true(venture_posting_service_route_currency(posting, f->org, "TICKET", when, &route, NULL, &error));
	g_assert_cmpint(route, ==, VENTURE_BOOK_ROUTE_SEPARATE);
	g_assert_true(venture_posting_service_route_currency(posting, f->org, "BREWFEST", when, &route, NULL, &error));
	g_assert_cmpint(route, ==, VENTURE_BOOK_ROUTE_MEMO);
	g_assert_no_error(error);
	g_assert_false(venture_posting_service_route_currency(posting, f->org, "no such code", when, &route, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * Finding 2a, the part that stays: a valued currency with no rate keeps
 * posting its own balanced journal exactly as before -- no rate is ever
 * invented, and nothing is recorded as valued by a policy.
 */
static void
test_valued_without_rate(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *policy = NULL;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	sale = ticket_sale(f, "50 TICKET");
	save(f, sale);
	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(journals->len, ==, 1);
	currency = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(currency, ==, "TICKET");
	g_object_get(g_ptr_array_index(journals, 0), "exchange-policy", &policy, NULL);
	g_assert_null(policy);
	rows = lines_of(f, g_ptr_array_index(journals, 0));
	g_assert_cmpuint(rows->len, ==, 2);
	line_amounts(g_ptr_array_index(rows, 0), 50, "TICKET", 50, "TICKET");
}

/*
 * Finding 2a, the fix: with a TICKET-to-GOLD rate the same sale posts one
 * GOLD journal. Each line keeps the 50 TICKET it was and carries 25 GOLD
 * as its book amount, and the journal names the rate table that valued it.
 * Before the fix the sale kept posting a TICKET journal, rate or no rate.
 */
static void
test_valued_with_rate(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *policy = NULL;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	rate(f, "TICKET", "GOLD", 1, 2);
	sale = ticket_sale(f, "50 TICKET");
	save(f, sale);
	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(journals->len, ==, 1);
	currency = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(currency, ==, "GOLD");
	g_object_get(g_ptr_array_index(journals, 0), "exchange-policy", &policy, NULL);
	g_assert_cmpstr(policy, ==, "exchange_rate");
	rows = lines_of(f, g_ptr_array_index(journals, 0));
	g_assert_cmpuint(rows->len, ==, 2);
	/* 25 GOLD at four decimals. */
	line_amounts(g_ptr_array_index(rows, 0), 50, "TICKET", 250000, "GOLD");
	line_amounts(g_ptr_array_index(rows, 1), 50, "TICKET", 250000, "GOLD");
}

/*
 * A separate-book currency is never converted, even with a rate on file,
 * and not through the core service either: a caller handing it a policy
 * is refused rather than obeyed.
 */
static void
test_separate_never_converts(Fixture *f, gconstpointer data)
{
	VenturePostingService *posting = venture_database_get_posting_service(f->db);
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureExchangePolicy) policy = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *currency = NULL;
	guint i;

	(void)data;
	define_currency(f, "TICKET", 0, "separate_book");
	rate(f, "TICKET", "GOLD", 1, 2);
	sale = ticket_sale(f, "50 TICKET");
	save(f, sale);
	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(journals->len, ==, 1);
	currency = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(currency, ==, "TICKET");
	rows = lines_of(f, g_ptr_array_index(journals, 0));
	line_amounts(g_ptr_array_index(rows, 0), 50, "TICKET", 50, "TICKET");

	/* The same lines under a GOLD header, with the policy handed over. */
	g_object_set(header, "organization-id", f->org, "source-type", "organization", "source-id", f->org,
		"currency", "GOLD", NULL);
	field(header, "occurred-at", "2026-03-01");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *copy = venture_entity_duplicate(g_ptr_array_index(rows, i));

		g_object_set(copy, "journal-id", (gint64)0, NULL);
		g_ptr_array_add(lines, copy);
	}
	policy = venture_rate_table_policy_new(f->db, f->org);
	posted = venture_posting_service_post(posting, header, lines, policy, NULL, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "separate book"));
}

/*
 * A memo currency never reaches the ledger: a sale or expense in it saves
 * with no journal, is never listed as waiting for one, and the core
 * service refuses a line in it outright.
 */
static void
test_memo_never_posts(Fixture *f, gconstpointer data)
{
	VenturePostingService *posting = venture_database_get_posting_service(f->db);
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(VentureEntity) expense = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) waiting = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	define_currency(f, "BREWFEST", 0, "memo");
	/* A rate changes nothing for a memo currency. */
	rate(f, "BREWFEST", "GOLD", 1, 1);
	sale = ticket_sale(f, "40 BREWFEST");
	save(f, sale);
	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(journals->len, ==, 0);
	g_clear_pointer(&journals, g_ptr_array_unref);
	expense = record(f, "expense");
	g_object_set(expense, "venture-id", f->venture, "description", "Faire costs", NULL);
	field(expense, "occurred-at", "2026-03-02");
	field(expense, "amount", "12 BREWFEST");
	save(f, expense);
	journals = journals_for(f, "expense", venture_entity_get_id(expense));
	g_assert_cmpuint(journals->len, ==, 0);
	waiting = venture_autojournal_service_unposted(venture_database_get_autojournal_service(f->db), f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(waiting->len, ==, 0);

	rows = g_ptr_array_new_with_free_func(g_object_unref);
	{
		g_autoptr(VentureAccount) account = venture_account_new();
		guint i;

		g_object_set(account, "organization-id", f->org, "code", "MEMO-1", "name", "Memo holdings",
			"kind", VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
		save(f, account);
		for (i = 0; i < 2; i++)
		{
			VentureJournalLine *line = venture_journal_line_new();

			g_object_set(line, "account-id", venture_entity_get_id(VENTURE_ENTITY(account)),
				"side", i == 0 ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT, NULL);
			field(line, "amount", "5 BREWFEST");
			g_ptr_array_add(rows, line);
		}
	}
	g_object_set(header, "organization-id", f->org, "source-type", "organization", "source-id", f->org,
		"currency", "BREWFEST", NULL);
	field(header, "occurred-at", "2026-03-01");
	posted = venture_posting_service_post(posting, header, rows, NULL, NULL, &error);
	g_assert_null(posted);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "memo currency"));
}

/*
 * Finding 2c: a GOLD sale of a product whose recorded cost is in TICKET.
 * The whole sale used to be refused ("Cannot combine GOLD and TICKET")
 * because the cost was added into the gold cash total to check its
 * currency. Now the gold legs post in GOLD and the ticket cost posts by
 * TICKET's own treatment: its own journal with no rate, valued into the
 * GOLD journal with one.
 */
static void
test_gold_sale_ticket_cost(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) gold = NULL;
	g_autoptr(GPtrArray) ticket = NULL;
	g_autofree gchar *first_currency = NULL;
	g_autofree gchar *second_currency = NULL;
	guint i;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	product = record(f, "product");
	g_object_set(product, "name", "Darkmoon prize", "venture-id", f->venture, NULL);
	field(product, "cost", "5 TICKET");
	save(f, product);
	sale = ticket_sale(f, "15 GOLD");
	g_object_set(sale, "product-id", venture_entity_get_id(product), "quantity", (gint64)2, NULL);
	save(f, sale);

	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(journals->len, ==, 2);
	first_currency = currency_of(g_ptr_array_index(journals, 0));
	second_currency = currency_of(g_ptr_array_index(journals, 1));
	g_assert_cmpstr(first_currency, ==, "GOLD");
	g_assert_cmpstr(second_currency, ==, "TICKET");
	gold = lines_of(f, g_ptr_array_index(journals, 0));
	ticket = lines_of(f, g_ptr_array_index(journals, 1));
	/* Gold cash and sales, and nothing else: no cost in gold was invented. */
	g_assert_cmpuint(gold->len, ==, 2);
	for (i = 0; i < gold->len; i++)
		line_amounts(g_ptr_array_index(gold, i), 150000, "GOLD", 150000, "GOLD");
	/* Each journal balances by itself, so there is nothing to clear. */
	g_assert_cmpuint(ticket->len, ==, 2);
	for (i = 0; i < ticket->len; i++)
	{
		g_autofree gchar *name = account_name(f, g_ptr_array_index(ticket, i));

		line_amounts(g_ptr_array_index(ticket, i), 10, "TICKET", 10, "TICKET");
		g_assert_true(g_strcmp0(name, "Cost of goods sold") == 0 || g_strcmp0(name, "Inventory") == 0);
	}

	/* With a rate, the next such sale is one GOLD journal. */
	rate(f, "TICKET", "GOLD", 1, 2);
	second = ticket_sale(f, "15 GOLD");
	g_object_set(second, "product-id", venture_entity_get_id(product), "quantity", (gint64)2, NULL);
	save(f, second);
	g_clear_pointer(&journals, g_ptr_array_unref);
	g_clear_pointer(&gold, g_ptr_array_unref);
	journals = journals_for(f, "sale", venture_entity_get_id(second));
	g_assert_cmpuint(journals->len, ==, 1);
	gold = lines_of(f, g_ptr_array_index(journals, 0));
	g_assert_cmpuint(gold->len, ==, 4);
	line_amounts(g_ptr_array_index(gold, 2), 10, "TICKET", 50000, "GOLD");
	line_amounts(g_ptr_array_index(gold, 3), 10, "TICKET", 50000, "GOLD");
}

/*
 * Finding 2d: an absent amount is zero of the organization's book
 * currency, not of USD. The zero-evidence lines a sale with no money
 * builds are how it shows: in a GOLD organization they were dollar zeros.
 */
static void
test_missing_amount_is_book_currency(Fixture *f, gconstpointer data)
{
	VenturePostingRuleRegistry *rules;
	VenturePostingRule *rule;
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = NULL;

	(void)data;
	venture_database_get_autojournal_service(f->db);
	rules = venture_posting_service_get_rules(venture_database_get_posting_service(f->db));
	rule = venture_posting_rule_registry_lookup(rules, "sale");
	g_assert_nonnull(rule);
	sale = record(f, "sale");
	g_object_set(sale, "venture-id", f->venture, NULL);
	rows = venture_posting_rule_build_lines(rule, f->db, sale, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 2);
	g_object_get(g_ptr_array_index(rows, 0), "amount", &amount, NULL);
	g_assert_cmpstr(venture_money_get_currency(amount), ==, "GOLD");
}

/*
 * Finding 3a: an expense in TICKET with no rate still posts, to a TICKET
 * journal, and the TICKET cash it credits goes negative in the TICKET book.
 * Refusing a negative holding is a later phase; this pins that nothing
 * here refuses it yet, and that the GOLD book is untouched.
 */
static void
test_ticket_expense(Fixture *f, gconstpointer data)
{
	VenturePostingService *posting = venture_database_get_posting_service(f->db);
	g_autoptr(VentureEntity) expense = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(GDateTime) as_of = g_date_time_new_utc(2026, 12, 31, 0, 0, 0);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *currency = NULL;
	gint64 cash = 0;
	guint i;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	expense = record(f, "expense");
	g_object_set(expense, "venture-id", f->venture, "description", "Faire costs", NULL);
	field(expense, "occurred-at", "2026-03-02");
	field(expense, "amount", "50 TICKET");
	save(f, expense);
	journals = journals_for(f, "expense", venture_entity_get_id(expense));
	g_assert_cmpuint(journals->len, ==, 1);
	currency = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(currency, ==, "TICKET");
	rows = lines_of(f, g_ptr_array_index(journals, 0));
	for (i = 0; i < rows->len; i++)
	{
		VentureLedgerSide side;

		g_object_get(g_ptr_array_index(rows, i), "side", &side, NULL);
		if (side == VENTURE_LEDGER_SIDE_CREDIT)
			cash = account_id(f, g_ptr_array_index(rows, i));
	}
	g_assert_cmpint(cash, >, 0);
	balance = venture_posting_service_account_balance(posting, cash, f->org, "TICKET", as_of, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, -50);
	g_clear_pointer(&balance, venture_money_free);
	balance = venture_posting_service_account_balance(posting, cash, f->org, "GOLD", as_of, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(balance), ==, 0);

	/* The trial balance over all time shows the TICKET book as its own
	 * balanced section. "all" has no end, and the report used to hand the
	 * balance query a NULL cutoff and fail for the whole organization. */
	{
		VentureReport *report = venture_report_registry_lookup(
			venture_context_get_report_registry(f->context), "trial_balance");
		g_autoptr(VentureDateRange) all = venture_date_range_new_all_time();
		g_autoptr(VentureReportResult) result = NULL;

		result = venture_report_generate(report, f->context, all, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
		g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	}
}

/*
 * A save that changes nothing financial does not reverse and repost a
 * document merely because a rate was recorded since: the TICKET journal a
 * sale posted before the rate stays the evidence. A real change reverses
 * it and posts under today's rule. If this regresses, recording one rate
 * rewrites every past period's figures the next time a note is edited.
 */
static void
test_resave_keeps_history(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) sale = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autofree gchar *last = NULL;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	sale = ticket_sale(f, "50 TICKET");
	save(f, sale);
	rate(f, "TICKET", "GOLD", 1, 2);
	g_object_set(sale, "notes", "Metadata only", NULL);
	save(f, sale);
	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	g_assert_cmpuint(journals->len, ==, 1);
	g_clear_pointer(&journals, g_ptr_array_unref);

	field(sale, "gross", "60 TICKET");
	save(f, sale);
	journals = journals_for(f, "sale", venture_entity_get_id(sale));
	/* The original, its reversal, and the replacement valued in GOLD. */
	g_assert_cmpuint(journals->len, ==, 3);
	last = currency_of(g_ptr_array_index(journals, 2));
	g_assert_cmpstr(last, ==, "GOLD");
}

/*
 * Finding 3b: a line takes its document's currency. A TICKET line on a
 * GOLD purchase order was accepted end to end; a vendor bill caught it only
 * at issue. And a document cannot be moved to a currency its lines are
 * not in. The line saved against order 0 also used to say "Unknown error".
 */
static void
test_line_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) order = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) orphan = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) bill_line = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	vendor = record(f, "company");
	field(vendor, "name", "Darkmoon vendor");
	field(vendor, "kind", "supplier");
	save(f, vendor);

	order = record(f, "purchase_order");
	g_object_set(order, "number", "PO-1", "vendor-id", venture_entity_get_id(vendor), "status", "draft",
		"currency", "GOLD", NULL);
	save(f, order);
	line = record(f, "purchase_order_line");
	g_object_set(line, "purchase-order-id", venture_entity_get_id(order), "description", "Prize",
		"quantity", (gint64)1, NULL);
	field(line, "unit-price", "5 TICKET");
	g_assert_false(venture_database_save(f->db, line, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "TICKET"));
	g_clear_error(&error);
	field(line, "unit-price", "5 GOLD");
	save(f, line);
	g_object_set(order, "currency", "TICKET", NULL);
	g_assert_false(venture_database_save(f->db, order, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	orphan = record(f, "purchase_order_line");
	g_object_set(orphan, "description", "Nowhere", "quantity", (gint64)1, NULL);
	field(orphan, "unit-price", "5 GOLD");
	g_assert_false(venture_database_save(f->db, orphan, NULL, &error));
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, "Unknown error"));
	g_clear_error(&error);

	bill = record(f, "vendor_bill");
	g_object_set(bill, "number", "B-1", "company-id", venture_entity_get_id(vendor),
		"currency", "GOLD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-03-01");
	field(bill, "due-date", "2026-03-31");
	save(f, bill);
	bill_line = record(f, "vendor_bill_line");
	g_object_set(bill_line, "bill-id", venture_entity_get_id(bill), "description", "Prize",
		"quantity", "1", "category", "supplies", NULL);
	field(bill_line, "unit-price", "5 TICKET");
	g_assert_false(venture_database_save(f->db, bill_line, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	field(bill_line, "unit-price", "5 GOLD");
	field(bill_line, "tax-amount", "1 TICKET");
	g_assert_false(venture_database_save(f->db, bill_line, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	field(bill_line, "tax-amount", "1 GOLD");
	save(f, bill_line);
}

static gint64
make_account(Fixture *f, const gchar *code, const gchar *name, VentureAccountKind kind)
{
	g_autoptr(VentureAccount) account = venture_account_new();

	g_object_set(account, "organization-id", f->org, "code", code, "name", name,
		"kind", kind, "active", TRUE, NULL);
	save(f, account);
	return venture_entity_get_id(VENTURE_ENTITY(account));
}

static GHashTable *
journal_params(Fixture *f, const gchar *lines)
{
	g_autofree gchar *body = NULL;
	GHashTable *params;

	body = g_strdup_printf("{\"organization_id\":%" G_GINT64_FORMAT ",\"source_type\":\"organization\","
		"\"source_id\":%" G_GINT64_FORMAT ",\"currency\":\"GOLD\",\"occurred_at\":\"2026-03-01\","
		"\"lines\":[%s]}", f->org, f->org, lines);
	params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
	{
		JsonNode *node = json_node_new(JSON_NODE_VALUE);

		json_node_set_string(node, body);
		g_hash_table_insert(params, g_strdup("journal"), node);
	}
	return params;
}

/*
 * Finding 7: a manual journal mixing GOLD and TICKET was refused ("Mixed
 * currencies require an explicit exchange policy") even with a rate.
 * create_and_post now applies the rule: with no rate, a GOLD journal and a
 * TICKET journal, each balanced through currency clearing; with a rate,
 * one GOLD journal valued by it. A saved draft is one journal, so it
 * converts with a rate and refuses without one, saying why.
 */
static void
test_manual_mixing(Fixture *f, gconstpointer data)
{
	VentureActionRegistry *actions = venture_database_get_action_registry(f->db);
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) gold = NULL;
	g_autoptr(GPtrArray) ticket = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *lines = NULL;
	g_autofree gchar *clearing = NULL;
	gint64 holdings, cash;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	holdings = make_account(f, "1500", "Ticket holdings", VENTURE_ACCOUNT_KIND_ASSET);
	cash = make_account(f, "1010", "Gold purse", VENTURE_ACCOUNT_KIND_ASSET);
	lines = g_strdup_printf(
		"{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"debit\",\"amount\":\"50 TICKET\"},"
		"{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"credit\",\"amount\":\"10 GOLD\"}", holdings, cash);

	params = journal_params(f, lines);
	result = venture_action_registry_perform(actions, "journal", 0, "create_and_post", params,
		NULL, VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	journals = journals_for(f, "organization", f->org);
	g_assert_cmpuint(journals->len, ==, 2);
	gold = lines_of(f, g_ptr_array_index(journals, 0));
	ticket = lines_of(f, g_ptr_array_index(journals, 1));
	g_assert_cmpuint(gold->len, ==, 2);
	g_assert_cmpuint(ticket->len, ==, 2);
	clearing = account_name(f, g_ptr_array_index(gold, 1));
	g_assert_cmpstr(clearing, ==, "Currency clearing");
	line_amounts(g_ptr_array_index(gold, 1), 100000, "GOLD", 100000, "GOLD");
	line_amounts(g_ptr_array_index(ticket, 1), 50, "TICKET", 50, "TICKET");
	g_assert_cmpint(account_id(f, g_ptr_array_index(gold, 1)), ==,
		account_id(f, g_ptr_array_index(ticket, 1)));

	/* With 1 TICKET = 0.2 GOLD the same journal is one GOLD journal. */
	rate(f, "TICKET", "GOLD", 1, 5);
	g_clear_object(&result);
	g_clear_pointer(&params, g_hash_table_unref);
	params = journal_params(f, lines);
	result = venture_action_registry_perform(actions, "journal", 0, "create_and_post", params,
		NULL, VENTURE_USER_ROLE_OWNER, &error);
	g_assert_no_error(error);
	g_clear_pointer(&journals, g_ptr_array_unref);
	journals = journals_for(f, "organization", f->org);
	g_assert_cmpuint(journals->len, ==, 3);
	g_clear_pointer(&gold, g_ptr_array_unref);
	gold = lines_of(f, g_ptr_array_index(journals, 2));
	g_assert_cmpuint(gold->len, ==, 2);
	line_amounts(g_ptr_array_index(gold, 0), 50, "TICKET", 100000, "GOLD");

	/* A single-currency imbalance is still an imbalance. */
	g_clear_object(&result);
	g_clear_pointer(&params, g_hash_table_unref);
	g_clear_pointer(&lines, g_free);
	lines = g_strdup_printf(
		"{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"debit\",\"amount\":\"11 GOLD\"},"
		"{\"account_id\":%" G_GINT64_FORMAT ",\"side\":\"credit\",\"amount\":\"10 GOLD\"}", holdings, cash);
	params = journal_params(f, lines);
	result = venture_action_registry_perform(actions, "journal", 0, "create_and_post", params,
		NULL, VENTURE_USER_ROLE_OWNER, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_BALANCE);
	g_clear_error(&error);

	/* A saved draft in GOLD with a TICKET line: valued with the rate. */
	{
		g_autoptr(VentureJournal) draft = venture_journal_new();
		g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
		g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
		g_autoptr(VentureEntity) posted = NULL;
		g_autoptr(GHashTable) none = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)json_node_unref);

		g_object_set(draft, "organization-id", f->org, "source-type", "organization", "source-id", f->org,
			"currency", "GOLD", NULL);
		field(draft, "occurred-at", "2026-03-01");
		save(f, draft);
		g_object_set(debit, "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)), "account-id", holdings,
			"side", VENTURE_LEDGER_SIDE_DEBIT, "organization-id", f->org, NULL);
		field(debit, "amount", "50 TICKET");
		save(f, debit);
		g_object_set(credit, "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)), "account-id", cash,
			"side", VENTURE_LEDGER_SIDE_CREDIT, "organization-id", f->org, NULL);
		field(credit, "amount", "10 GOLD");
		save(f, credit);
		posted = venture_action_registry_perform(actions, "journal", venture_entity_get_id(VENTURE_ENTITY(draft)),
			"post", none, NULL, VENTURE_USER_ROLE_OWNER, &error);
		g_assert_no_error(error);
		g_assert_nonnull(posted);
	}

	/* Kept apart, the same draft is refused with the reason. */
	set_treatment(f, "TICKET", "separate_book");
	{
		g_autoptr(VentureJournal) draft = venture_journal_new();
		g_autoptr(VentureJournalLine) debit = venture_journal_line_new();
		g_autoptr(VentureJournalLine) credit = venture_journal_line_new();
		g_autoptr(VentureEntity) posted = NULL;
		g_autoptr(GHashTable) none = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
			(GDestroyNotify)json_node_unref);

		g_object_set(draft, "organization-id", f->org, "source-type", "organization", "source-id", f->org,
			"currency", "GOLD", NULL);
		field(draft, "occurred-at", "2026-03-01");
		save(f, draft);
		g_object_set(debit, "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)), "account-id", holdings,
			"side", VENTURE_LEDGER_SIDE_DEBIT, "organization-id", f->org, NULL);
		field(debit, "amount", "50 TICKET");
		save(f, debit);
		g_object_set(credit, "journal-id", venture_entity_get_id(VENTURE_ENTITY(draft)), "account-id", cash,
			"side", VENTURE_LEDGER_SIDE_CREDIT, "organization-id", f->org, NULL);
		field(credit, "amount", "10 GOLD");
		save(f, credit);
		posted = venture_action_registry_perform(actions, "journal", venture_entity_get_id(VENTURE_ENTITY(draft)),
			"post", none, NULL, VENTURE_USER_ROLE_OWNER, &error);
		g_assert_null(posted);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "create_and_post"));
	}
}

/*
 * The hook the inventory service posts through: an issue whose FIFO
 * layers cost GOLD and TICKET hands both pairs to one call and gets one
 * journal per book, each balanced by itself -- no clearing, no refusal.
 */
static void
test_pairs_per_currency(Fixture *f, gconstpointer data)
{
	VenturePostingService *posting = venture_database_get_posting_service(f->db);
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) posted = NULL;
	g_autoptr(GError) error = NULL;
	gint64 cogs, inventory;
	const gchar *const amounts[] = { "3 GOLD", "7 TICKET" };
	guint i, j;

	(void)data;
	define_currency(f, "TICKET", 0, "separate_book");
	cogs = make_account(f, "5010", "Cost of prizes", VENTURE_ACCOUNT_KIND_EXPENSE);
	inventory = make_account(f, "1210", "Prize stock", VENTURE_ACCOUNT_KIND_ASSET);
	g_object_set(header, "organization-id", f->org, "source-type", "organization", "source-id", f->org,
		"memo", "Inventory issue", NULL);
	field(header, "occurred-at", "2026-03-01");
	for (i = 0; i < G_N_ELEMENTS(amounts); i++)
		for (j = 0; j < 2; j++)
		{
			VentureJournalLine *line = venture_journal_line_new();

			g_object_set(line, "account-id", j == 0 ? cogs : inventory, "organization-id", f->org,
				"side", j == 0 ? VENTURE_LEDGER_SIDE_DEBIT : VENTURE_LEDGER_SIDE_CREDIT, NULL);
			field(line, "amount", amounts[i]);
			g_ptr_array_add(lines, line);
		}
	posted = venture_posting_service_post_by_currency(posting, header, lines, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
	g_assert_cmpuint(posted->len, ==, 2);
	for (i = 0; i < posted->len; i++)
	{
		g_autoptr(GPtrArray) rows = lines_of(f, g_ptr_array_index(posted, i));
		g_autofree gchar *currency = currency_of(g_ptr_array_index(posted, i));

		g_assert_cmpstr(currency, ==, i == 0 ? "GOLD" : "TICKET");
		g_assert_cmpuint(rows->len, ==, 2);
	}
}

/*
 * The behaviour change for fiat books: a EUR expense in a USD organization
 * with a EUR-to-USD rate on file is now valued into a USD journal
 * (functional-currency accounting), keeping its 80 EUR as the original
 * amount. Without the rate it still posts a EUR journal.
 */
static void
test_fiat_converts(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) before = NULL;
	g_autoptr(VentureEntity) after = NULL;
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	(void)data;
	before = record(f, "expense");
	g_object_set(before, "venture-id", f->venture, "description", "Faire costs", NULL);
	field(before, "occurred-at", "2026-03-02");
	field(before, "amount", "80 EUR");
	save(f, before);
	journals = journals_for(f, "expense", venture_entity_get_id(before));
	first = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(first, ==, "EUR");
	g_clear_pointer(&journals, g_ptr_array_unref);

	rate(f, "EUR", "USD", 11, 10);
	after = record(f, "expense");
	g_object_set(after, "venture-id", f->venture, "description", "Faire costs", NULL);
	field(after, "occurred-at", "2026-03-02");
	field(after, "amount", "80 EUR");
	save(f, after);
	journals = journals_for(f, "expense", venture_entity_get_id(after));
	g_assert_cmpuint(journals->len, ==, 1);
	second = currency_of(g_ptr_array_index(journals, 0));
	g_assert_cmpstr(second, ==, "USD");
	rows = lines_of(f, g_ptr_array_index(journals, 0));
	line_amounts(g_ptr_array_index(rows, 0), 8000, "EUR", 8800, "USD");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/currency-books/treatment-registry", Fixture, NULL, setup_gold, test_treatment_registry, teardown);
	g_test_add("/currency-books/route", Fixture, NULL, setup_gold, test_route, teardown);
	g_test_add("/currency-books/valued-without-rate", Fixture, NULL, setup_gold, test_valued_without_rate, teardown);
	g_test_add("/currency-books/valued-with-rate", Fixture, NULL, setup_gold, test_valued_with_rate, teardown);
	g_test_add("/currency-books/separate-never-converts", Fixture, NULL, setup_gold, test_separate_never_converts, teardown);
	g_test_add("/currency-books/memo-never-posts", Fixture, NULL, setup_gold, test_memo_never_posts, teardown);
	g_test_add("/currency-books/gold-sale-ticket-cost", Fixture, NULL, setup_gold, test_gold_sale_ticket_cost, teardown);
	g_test_add("/currency-books/missing-amount-is-book-currency", Fixture, NULL, setup_gold, test_missing_amount_is_book_currency, teardown);
	g_test_add("/currency-books/ticket-expense", Fixture, NULL, setup_gold, test_ticket_expense, teardown);
	g_test_add("/currency-books/resave-keeps-history", Fixture, NULL, setup_gold, test_resave_keeps_history, teardown);
	g_test_add("/currency-books/line-currency", Fixture, NULL, setup_gold, test_line_currency, teardown);
	g_test_add("/currency-books/manual-mixing", Fixture, NULL, setup_gold, test_manual_mixing, teardown);
	g_test_add("/currency-books/pairs-per-currency", Fixture, NULL, setup_gold, test_pairs_per_currency, teardown);
	g_test_add("/currency-books/fiat-converts", Fixture, NULL, setup_usd, test_fiat_converts, teardown);
	return g_test_run();
}
