/*
 * test-currency-reports.c - What the reports say about several currencies
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One organization keeps its books in GOLD (four decimals) and also deals
 * in TICKET (no decimals). The operational reports -- pnl, ventures,
 * monthly -- anchor on the book currency and keep one figure per currency,
 * book first, never dropping a currency's records and never adding across
 * two. The ledger statements label each currency's section by the posting
 * rule. The valuing reports price in the book currency wherever a product
 * was seen priced in it, or in the currency asked for. And a bare date is
 * one day, not the month it begins.
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

static void
define_currency(Fixture *f, const gchar *code, gint64 exponent, const gchar *treatment)
{
	g_autoptr(VentureEntity) currency = record(f, "currency");

	g_object_set(currency, "code", code, "name", code, "exponent", exponent, NULL);
	field(currency, "book-treatment", treatment);
	save(f, currency);
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
	organization = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, f->org, &error);
	g_assert_no_error(error);
	g_object_set(organization, "default-currency", "GOLD", NULL);
	save(f, organization);
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

static void
sale(Fixture *f, const gchar *gross, const gchar *when)
{
	g_autoptr(VentureEntity) row = record(f, "sale");

	g_object_set(row, "venture-id", f->venture, NULL);
	field(row, "occurred-at", when);
	field(row, "gross", gross);
	save(f, row);
}

static void
expense(Fixture *f, const gchar *amount, const gchar *when)
{
	g_autoptr(VentureEntity) row = record(f, "expense");

	g_object_set(row, "venture-id", f->venture, "description", "Reagents", NULL);
	field(row, "occurred-at", when);
	field(row, "amount", amount);
	save(f, row);
}

static VentureReportResult *
run(Fixture *f, const gchar *name, const gchar *period_text, JsonObject *options)
{
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(GError) error = NULL;
	VentureReportResult *result;
	VentureReport *report;

	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	g_assert_nonnull(report);
	period = venture_context_parse_period(f->context, period_text, &error);
	g_assert_no_error(error);
	result = venture_report_generate(report, f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static const VentureMoney *
metric(VentureReportResult *result, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(result);
	guint i;

	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);

		if (0 == g_strcmp0(venture_metric_get_key(m), key))
			return venture_metric_get_money(m);
	}
	return NULL;
}

static const gchar *
text(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);

	return (NULL != value && G_VALUE_HOLDS_STRING(value)) ? g_value_get_string(value) : NULL;
}

static const VentureMoney *
money(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);

	return (NULL != value && G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY)) ? g_value_get_boxed(value) : NULL;
}

static gdouble
number(VentureReportResult *result, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(result, row, key);

	return (NULL != value && G_VALUE_HOLDS_DOUBLE(value)) ? g_value_get_double(value) : -1;
}

/* Asserts an amount's code and minor units. */
static void
is(const VentureMoney *amount, const gchar *currency, gint64 minor)
{
	g_assert_nonnull(amount);
	g_assert_cmpstr(venture_money_get_currency(amount), ==, currency);
	g_assert_cmpint(venture_money_get_amount(amount), ==, minor);
}

/* The row whose @key column reads @value (and whose currency is @currency). */
static guint
row_where(VentureReportResult *result, const gchar *key, const gchar *value, const gchar *currency)
{
	guint i;

	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		if (0 == g_strcmp0(text(result, i, key), value) &&
			0 == g_strcmp0(text(result, i, "currency"), currency))
			return i;
	}
	g_assert_not_reached();
	return 0;
}

/* One gold sale among three ticket ones, and one gold expense. */
static void
trade_in_march(Fixture *f)
{
	define_currency(f, "TICKET", 0, "valued");
	sale(f, "2 GOLD", "2026-03-03");
	sale(f, "5 TICKET", "2026-03-04");
	sale(f, "5 TICKET", "2026-03-05");
	sale(f, "5 TICKET", "2026-03-06");
	expense(f, "1 GOLD", "2026-03-07");
}

/* ------------------------------------------------------------------------ */

/*
 * Finding 7, the P&L: it picked the currency most sales were in, so three
 * ticket sales outvoted the gold one, the 2 GOLD sale was dropped and the
 * gold expense could not be subtracted from ticket revenue -- a blank
 * profit. Now GOLD comes first, exact (2 - 1 = 1 GOLD), TICKET is a block
 * of its own (15 TICKET, no expenses, 15 profit) under the _TICKET keys,
 * and nothing anywhere says USD.
 */
static void
test_pnl_per_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autofree gchar *rendered = NULL;

	(void)data;
	trade_in_march(f);
	result = run(f, "pnl", "2026-03", NULL);

	is(metric(result, "revenue"), "GOLD", 20000);
	is(metric(result, "expenses"), "GOLD", 10000);
	is(metric(result, "profit"), "GOLD", 10000);
	is(metric(result, "revenue_TICKET"), "TICKET", 15);
	is(metric(result, "expenses_TICKET"), "TICKET", 0);
	is(metric(result, "profit_TICKET"), "TICKET", 15);

	/* Two blocks of eight lines, the book currency's first. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 16);
	g_assert_cmpstr(text(result, 0, "currency"), ==, "GOLD");
	g_assert_cmpstr(text(result, 8, "currency"), ==, "TICKET");
	is(money(result, row_where(result, "line", "Profit", "GOLD"), "amount"), "GOLD", 10000);

	rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TEXT);
	g_assert_null(strstr(rendered, "USD"));
	g_assert_null(strstr(rendered, "$"));
	g_assert_null(strstr(rendered, "could not be included"));
}

/*
 * Finding 7, the empty side: a gold organization that sold and spent
 * nothing had its empty expense total made in USD, so the profit could
 * not be computed and read as a zero dollar absence. Empty totals are
 * zero in the book currency, and the profit is the gold sold.
 */
static void
test_pnl_gold_only_profit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureReportResult) empty = NULL;

	(void)data;
	sale(f, "5 GOLD", "2026-03-03");
	result = run(f, "pnl", "2026-03", NULL);
	is(metric(result, "expenses"), "GOLD", 0);
	is(metric(result, "profit"), "GOLD", 50000);
	g_assert_null(metric(result, "profit_USD"));

	/* Nothing at all is still gold: 0 GOLD, not $0.00. */
	empty = run(f, "pnl", "2026-05", NULL);
	is(metric(empty, "revenue"), "GOLD", 0);
	is(metric(empty, "profit"), "GOLD", 0);
	g_assert_cmpuint(venture_report_result_get_row_count(empty), ==, 8);
}

/*
 * ventures: one row per venture per currency, the book currency's first,
 * each counting its own sales, and a portfolio revenue per currency.
 * Before, the venture's row carried whichever currency most sales used and
 * the gold sale vanished from it.
 */
static void
test_ventures_per_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	guint gold;
	guint tickets;

	(void)data;
	trade_in_march(f);
	result = run(f, "ventures", "2026-03", NULL);

	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 2);
	g_assert_cmpstr(text(result, 0, "currency"), ==, "GOLD");
	gold = row_where(result, "venture", "Evermoor", "GOLD");
	tickets = row_where(result, "venture", "Evermoor", "TICKET");
	g_assert_cmpfloat(number(result, gold, "sales"), ==, 1);
	g_assert_cmpfloat(number(result, tickets, "sales"), ==, 3);
	is(money(result, gold, "profit"), "GOLD", 10000);
	is(money(result, tickets, "profit"), "TICKET", 15);
	is(metric(result, "revenue"), "GOLD", 20000);
	is(metric(result, "revenue_TICKET"), "TICKET", 15);
}

/*
 * monthly: a row per month per currency. The book currency has a row every
 * month -- April, with one gold sale and no expenses, shows its profit in
 * gold -- and TICKET only in the month it traded.
 */
static void
test_monthly_per_currency(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	guint april;

	(void)data;
	trade_in_march(f);
	sale(f, "3 GOLD", "2026-04-02");
	result = run(f, "monthly", "2026-03-01..2026-04-30", NULL);

	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 3);
	g_assert_cmpstr(text(result, 0, "currency"), ==, "GOLD");
	g_assert_cmpstr(text(result, 1, "currency"), ==, "TICKET");
	is(money(result, 0, "profit"), "GOLD", 10000);
	is(money(result, 1, "revenue"), "TICKET", 15);
	april = 2;
	g_assert_cmpstr(text(result, april, "currency"), ==, "GOLD");
	is(money(result, april, "expenses"), "GOLD", 0);
	is(money(result, april, "profit"), "GOLD", 30000);
}

/*
 * The statements: each currency's section is labelled by the posting rule
 * and the book currency's comes first, where the alphabet put EUR ahead of
 * GOLD and nothing said what the TICKET rows were. Memo never appears,
 * because it never posts.
 */
static void
test_statements_labelled(Fixture *f, gconstpointer data)
{
	static const gchar *const names[] = { "trial_balance", "income_statement", "balance_sheet" };
	guint n;

	(void)data;
	define_currency(f, "TICKET", 0, "separate_book");
	define_currency(f, "BREWFEST", 0, "memo");
	sale(f, "2 GOLD", "2026-03-03");
	sale(f, "5 TICKET", "2026-03-04");
	sale(f, "4 EUR", "2026-03-05");
	sale(f, "9 BREWFEST", "2026-03-06");

	for (n = 0; n < G_N_ELEMENTS(names); n++)
	{
		g_autoptr(VentureReportResult) result = run(f, names[n], "2026-03", NULL);
		g_autofree gchar *rendered = NULL;
		gboolean saw_ticket = FALSE;
		gboolean saw_euro = FALSE;
		guint i;

		g_assert_cmpuint(venture_report_result_get_row_count(result), >, 0);
		g_assert_cmpstr(text(result, 0, "currency"), ==, "GOLD");
		g_assert_cmpstr(text(result, 0, "books"), ==, "Book currency: GOLD");

		for (i = 0; i < venture_report_result_get_row_count(result); i++)
		{
			const gchar *currency = text(result, i, "currency");

			g_assert_cmpstr(currency, !=, "BREWFEST");
			if (0 == g_strcmp0(currency, "TICKET"))
			{
				saw_ticket = TRUE;
				g_assert_cmpstr(text(result, i, "books"), ==, "Separate book: TICKET");
			}
			if (0 == g_strcmp0(currency, "EUR"))
			{
				saw_euro = TRUE;
				g_assert_cmpstr(text(result, i, "books"), ==, "Own book: EUR (no rate to GOLD)");
			}
		}
		g_assert_true(saw_ticket);
		g_assert_true(saw_euro);

		rendered = venture_report_result_render(result, VENTURE_OUTPUT_FORMAT_TEXT);
		g_assert_nonnull(strstr(rendered, "Separate book: TICKET"));
		g_assert_null(strstr(rendered, "BREWFEST"));
	}
}

/*
 * A bare date is that one day. The month form was tried first and sscanf
 * matched "2026-03-14" on its first seven characters, so a P&L for the
 * 14th reported all of March. A date that does not exist is refused, not
 * read as the month it starts in.
 */
static void
test_single_date_period(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureDateRange) day = NULL;
	g_autoptr(VentureDateRange) bad = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;
	sale(f, "2 GOLD", "2026-03-01");
	sale(f, "3 GOLD", "2026-03-14");

	day = venture_date_range_parse("2026-03-14", NULL, 1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_date_range_get_days(day), ==, 1);

	result = run(f, "pnl", "2026-03-14", NULL);
	is(metric(result, "revenue"), "GOLD", 30000);

	bad = venture_date_range_parse("2026-02-31", NULL, 1, &error);
	g_assert_null(bad);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static gint64
product(Fixture *f, const gchar *name)
{
	g_autoptr(VentureEntity) row = record(f, "product");

	g_object_set(row, "name", name, "venture-id", f->venture, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}

static void
observe(Fixture *f, gint64 product_id, const gchar *price, const gchar *when)
{
	g_autoptr(VentureEntity) row = record(f, "price_observation");

	g_object_set(row, "product-id", product_id, "source", "market value", NULL);
	field(row, "price", price);
	field(row, "observed-at", when);
	save(f, row);
}

/*
 * Finding 4, in a report: the herb and the potion were seen in gold, then
 * more recently in tickets. The margin used to be worked out in whichever
 * currency was seen last -- tickets -- in a gold organization. By default
 * it is now in gold wherever gold was seen; asked for tickets, it is in
 * tickets; asked for a code that is not one, it is refused.
 */
static void
test_valuing_prefers_book(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureReportResult) gold = NULL;
	g_autoptr(VentureReportResult) tickets = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonObject) wrong = json_object_new();
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) refused = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) recipe = record(f, "recipe");
	g_autoptr(VentureEntity) component = record(f, "recipe_component");
	gint64 herb;
	gint64 potion;

	(void)data;
	define_currency(f, "TICKET", 0, "valued");
	herb = product(f, "Herb");
	potion = product(f, "Potion");
	g_object_set(recipe, "name", "Brew", "output-product-id", potion, "output-quantity", (gint64)1,
		"active", TRUE, "venture-id", f->venture, NULL);
	save(f, recipe);
	g_object_set(component, "recipe-id", venture_entity_get_id(recipe), "product-id", herb,
		"quantity", (gint64)1, NULL);
	save(f, component);
	observe(f, herb, "1 GOLD", "2026-03-01");
	observe(f, potion, "3 GOLD", "2026-03-01");
	observe(f, herb, "7 TICKET", "2026-03-10");
	observe(f, potion, "9 TICKET", "2026-03-10");

	gold = run(f, "recipe_margin", "all", NULL);
	g_assert_cmpuint(venture_report_result_get_row_count(gold), ==, 1);
	g_assert_cmpstr(text(gold, 0, "currency"), ==, "GOLD");
	is(money(gold, 0, "cost"), "GOLD", 10000);
	is(money(gold, 0, "value"), "GOLD", 30000);

	json_object_set_string_member(options, "currency", "TICKET");
	tickets = run(f, "recipe_margin", "all", options);
	g_assert_cmpstr(text(tickets, 0, "currency"), ==, "TICKET");
	is(money(tickets, 0, "cost"), "TICKET", 7);
	is(money(tickets, 0, "value"), "TICKET", 9);

	json_object_set_string_member(wrong, "currency", "not a code");
	period = venture_context_parse_period(f->context, "all", &error);
	g_assert_no_error(error);
	refused = venture_report_generate(venture_report_registry_lookup(
		venture_context_get_report_registry(f->context), "recipe_margin"),
		f->context, period, wrong, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/currency-reports/pnl-per-currency", Fixture, NULL, setup, test_pnl_per_currency, teardown);
	g_test_add("/currency-reports/pnl-gold-only-profit", Fixture, NULL, setup, test_pnl_gold_only_profit, teardown);
	g_test_add("/currency-reports/ventures-per-currency", Fixture, NULL, setup, test_ventures_per_currency, teardown);
	g_test_add("/currency-reports/monthly-per-currency", Fixture, NULL, setup, test_monthly_per_currency, teardown);
	g_test_add("/currency-reports/statements-labelled", Fixture, NULL, setup, test_statements_labelled, teardown);
	g_test_add("/currency-reports/single-date-period", Fixture, NULL, setup, test_single_date_period, teardown);
	g_test_add("/currency-reports/valuing-prefers-book", Fixture, NULL, setup, test_valuing_prefers_book, teardown);
	return g_test_run();
}
