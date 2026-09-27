/*
 * test-money.c - Exact monetary arithmetic
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These tests exist because every one of them corresponds to a way a naive
 * money implementation loses real money: binary floating point that cannot
 * hold a dime, rounding that biases upward over a year of tax lines, a split
 * that drops a cent, a zero-decimal currency inflated by a factor of a
 * hundred, and silent overflow.
 */

#include <venture.h>

#include <string.h>

/* --- Construction and accessors ------------------------------------------ */

static void
test_money_new(void)
{
	g_autoptr(VentureMoney) money = NULL;

	money = venture_money_new(123, "USD", 2);

	g_assert_nonnull(money);
	g_assert_cmpint(venture_money_get_amount(money), ==, 123);
	g_assert_cmpstr(venture_money_get_currency(money), ==, "USD");
	g_assert_cmpuint(venture_money_get_exponent(money), ==, 2);
}

static void
test_money_currency_normalised(void)
{
	g_autoptr(VentureMoney) lower = NULL;

	/* A currency code arriving in lowercase from a CSV import must not
	 * become a different currency from the same code in uppercase. */
	lower = venture_money_new(100, "usd", 2);

	g_assert_cmpstr(venture_money_get_currency(lower), ==, "USD");
}

static void
test_money_zero_decimal_currency(void)
{
	g_autoptr(VentureMoney) yen = NULL;

	/* JPY has no minor unit. Treating it as two-decimal would report
	 * every yen figure as a hundredth of its real value. */
	yen = venture_money_new_for_currency(100, "JPY");

	g_assert_cmpuint(venture_money_get_exponent(yen), ==, 0);
	g_assert_cmpint(venture_money_get_amount(yen), ==, 100);
}

static void
test_money_three_decimal_currency(void)
{
	g_autoptr(VentureMoney) dinar = NULL;

	dinar = venture_money_new_for_currency(1500, "KWD");

	g_assert_cmpuint(venture_money_get_exponent(dinar), ==, 3);
}

/* --- Addition and subtraction -------------------------------------------- */

static void
test_money_add(void)
{
	g_autoptr(VentureMoney) a = NULL;
	g_autoptr(VentureMoney) b = NULL;
	g_autoptr(VentureMoney) sum = NULL;
	g_autoptr(GError) error = NULL;

	/* The canonical floating-point failure: 0.10 + 0.20 must be exactly
	 * 0.30, not 0.30000000000000004. */
	a = venture_money_new(10, "USD", 2);
	b = venture_money_new(20, "USD", 2);
	sum = venture_money_add(a, b, &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(sum), ==, 30);
}

static void
test_money_add_differing_exponents(void)
{
	g_autoptr(VentureMoney) cents = NULL;
	g_autoptr(VentureMoney) mills = NULL;
	g_autoptr(VentureMoney) sum = NULL;
	g_autoptr(GError) error = NULL;

	/* Adding a two-decimal amount to a four-decimal one must keep the
	 * finer precision rather than truncating it away. */
	cents = venture_money_new(100, "USD", 2);
	mills = venture_money_new(12345, "USD", 4);
	sum = venture_money_add(cents, mills, &error);

	/* $1.00 rescaled from two decimals to four is 10000, not 1000000. */
	g_assert_no_error(error);
	g_assert_cmpuint(venture_money_get_exponent(sum), ==, 4);
	g_assert_cmpint(venture_money_get_amount(sum), ==, 10000 + 12345);
}

static void
test_money_add_mixed_currency_fails(void)
{
	g_autoptr(VentureMoney) usd = NULL;
	g_autoptr(VentureMoney) eur = NULL;
	g_autoptr(VentureMoney) sum = NULL;
	g_autoptr(GError) error = NULL;

	/* There is no exchange rate in this type. Guessing one silently would
	 * be far worse than failing. */
	usd = venture_money_new(100, "USD", 2);
	eur = venture_money_new(100, "EUR", 2);
	sum = venture_money_add(usd, eur, &error);

	g_assert_null(sum);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static void
test_money_subtract(void)
{
	g_autoptr(VentureMoney) a = NULL;
	g_autoptr(VentureMoney) b = NULL;
	g_autoptr(VentureMoney) difference = NULL;
	g_autoptr(GError) error = NULL;

	a = venture_money_new(100, "USD", 2);
	b = venture_money_new(250, "USD", 2);
	difference = venture_money_subtract(a, b, &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(difference), ==, -150);
	g_assert_true(venture_money_is_negative(difference));
}

static void
test_money_add_overflow_detected(void)
{
	g_autoptr(VentureMoney) big = NULL;
	g_autoptr(VentureMoney) sum = NULL;
	g_autoptr(GError) error = NULL;

	/* Signed overflow must be reported, not wrapped into a negative
	 * balance that looks like a legitimate figure. */
	big = venture_money_new(G_MAXINT64, "USD", 2);
	sum = venture_money_add(big, big, &error);

	g_assert_null(sum);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- Multiplication and rounding ----------------------------------------- */

static void
test_money_multiply_int(void)
{
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;

	unit = venture_money_new(1999, "USD", 2);
	total = venture_money_multiply_int(unit, 3, &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(total), ==, 5997);
}

static void
test_money_multiply_percent(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(GError) error = NULL;

	/* 7.25% of $100.00 is exactly $7.25. Computed as 10000 * 725 / 10000
	 * with a single rounding, so it cannot drift. */
	amount = venture_money_new(10000, "USD", 2);
	tax = venture_money_multiply_percent(amount, 725, &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 725);
}

static void
test_money_rounding_half_to_even(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) two_point_five = NULL;
	g_autoptr(VentureMoney) three_point_five = NULL;
	g_autoptr(VentureMoney) rounded_down = NULL;
	g_autoptr(VentureMoney) rounded_up = NULL;

	/* Exact halves round to the even neighbour. 2.5 units rounds to 2 and
	 * 3.5 rounds to 4, so a long series of roundings does not drift
	 * upward the way round-half-away-from-zero does. */
	two_point_five = venture_money_new(5, "USD", 2);
	rounded_down = venture_money_multiply_rational(two_point_five, 1, 2, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(rounded_down), ==, 2);

	three_point_five = venture_money_new(7, "USD", 2);
	rounded_up = venture_money_multiply_rational(three_point_five, 1, 2, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(rounded_up), ==, 4);
}

static void
test_money_rounding_negative(void)
{
	g_autoptr(VentureMoney) negative = NULL;
	g_autoptr(VentureMoney) result = NULL;
	g_autoptr(GError) error = NULL;

	/* Rounding must be symmetric about zero: -2.5 rounds to -2 just as
	 * 2.5 rounds to 2. C's truncating division does not do this for
	 * negatives by itself. */
	negative = venture_money_new(-5, "USD", 2);
	result = venture_money_multiply_rational(negative, 1, 2, &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(result), ==, -2);
}

static void
test_money_multiply_zero_denominator_fails(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) result = NULL;
	g_autoptr(GError) error = NULL;

	amount = venture_money_new(100, "USD", 2);
	result = venture_money_multiply_rational(amount, 1, 0, &error);

	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- Allocation ---------------------------------------------------------- */

static void
test_money_allocate_evenly_preserves_total(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;

	/* Five cents split three ways cannot be equal. What it must be is
	 * three parts that add back to exactly five cents -- 2, 2, 1 -- not
	 * three parts of 1 with two cents quietly lost. */
	amount = venture_money_new(5, "USD", 2);
	parts = venture_money_allocate_evenly(amount, 3, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(parts->len, ==, 3);

	g_assert_cmpint(venture_money_get_amount(g_ptr_array_index(parts, 0)), ==, 2);
	g_assert_cmpint(venture_money_get_amount(g_ptr_array_index(parts, 1)), ==, 2);
	g_assert_cmpint(venture_money_get_amount(g_ptr_array_index(parts, 2)), ==, 1);

	total = venture_money_sum(parts, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(total), ==, 5);
}

static void
test_money_allocate_by_ratio(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;
	gint64 ratios[3];

	/* Apportioning a shared expense 1:1:1 across three ventures, where
	 * the amount does not divide evenly. */
	ratios[0] = 3;
	ratios[1] = 2;
	ratios[2] = 1;

	amount = venture_money_new(10000, "USD", 2);
	parts = venture_money_allocate(amount, ratios, 3, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(parts->len, ==, 3);

	total = venture_money_sum(parts, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(total), ==, 10000);

	/* The exact shares are 5000, 3333 and 1666, which sum to 9999. The
	 * leftover minor unit goes to the largest ratio, so the first part is
	 * 5001 rather than 5000 and the total is preserved exactly. */
	g_assert_cmpint(venture_money_get_amount(g_ptr_array_index(parts, 0)), ==, 5001);
	g_assert_cmpint(venture_money_get_amount(g_ptr_array_index(parts, 1)), ==, 3333);
	g_assert_cmpint(venture_money_get_amount(g_ptr_array_index(parts, 2)), ==, 1666);
}

static void
test_money_allocate_negative_preserves_total(void)
{
	g_autoptr(VentureMoney) refund = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;

	/* A refund split across line items is a negative allocation. The
	 * remainder distribution has to work in that direction too. */
	refund = venture_money_new(-5, "USD", 2);
	parts = venture_money_allocate_evenly(refund, 3, &error);

	g_assert_no_error(error);

	total = venture_money_sum(parts, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(total), ==, -5);
}

static void
test_money_allocate_rejects_zero_parts(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GPtrArray) parts = NULL;
	g_autoptr(GError) error = NULL;

	amount = venture_money_new(100, "USD", 2);
	parts = venture_money_allocate_evenly(amount, 0, &error);

	g_assert_null(parts);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static void
test_money_sum_empty_is_zero(void)
{
	g_autoptr(GPtrArray) empty = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;

	/* A report for a month with no sales prints 0.00; it does not fail. */
	empty = g_ptr_array_new();
	total = venture_money_sum(empty, "EUR", &error);

	g_assert_no_error(error);
	g_assert_true(venture_money_is_zero(total));
	g_assert_cmpstr(venture_money_get_currency(total), ==, "EUR");
}

/* --- Comparison ---------------------------------------------------------- */

static void
test_money_compare_across_exponents(void)
{
	g_autoptr(VentureMoney) coarse = NULL;
	g_autoptr(VentureMoney) fine = NULL;

	/* 1.50 and 1.5000 are the same amount written two ways. */
	coarse = venture_money_new(150, "USD", 2);
	fine = venture_money_new(15000, "USD", 4);

	g_assert_cmpint(venture_money_compare(coarse, fine), ==, 0);
	g_assert_true(venture_money_equal(coarse, fine));
	g_assert_cmpuint(venture_money_hash(coarse), ==, venture_money_hash(fine));
}

/* --- Text round trip ----------------------------------------------------- */

static void
test_money_to_string_round_trip(void)
{
	g_autoptr(VentureMoney) original = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(GError) error = NULL;

	original = venture_money_new(-123456, "USD", 2);
	text = venture_money_to_string(original);

	g_assert_cmpstr(text, ==, "-1234.56 USD");

	parsed = venture_money_from_string(text, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_money_equal(original, parsed));
}

static void
test_money_parse_tolerant_forms(void)
{
	struct
	{
		const gchar	*text;
		gint64		 expected;
		const gchar	*currency;
	} cases[] = {
		{ "12.34",          1234,   "USD" },
		{ "$12.34",         1234,   "USD" },
		{ "12.34 USD",      1234,   "USD" },
		{ "USD 12.34",      1234,   "USD" },
		{ "1,234.56",       123456, "USD" },
		{ "(12.34)",        -1234,  "USD" },
		{ "-12.34",         -1234,  "USD" },
		{ "+12.34",         1234,   "USD" },
		{ "  12.34  ",      1234,   "USD" },
		/* A whole-unit amount must be scaled to the currency's natural
		 * precision so it adds cleanly to a fractional one. */
		{ "5",              500,    "USD" },
		{ "12.34 EUR",      1234,   "EUR" },
		{ "1,234,567.89",   123456789, "USD" },
		{ "US$12.34",       1234,   "USD" },
		{ "1,000",          100000, "USD" },
		/* A symbol stuck to a whole number, as spreadsheet and
		 * marketplace exports write it, is decoration -- not a coin
		 * of some denominated currency. */
		{ "12\xe2\x82\xac",       1200,   "USD" },
		{ "100$",           10000,  "USD" }
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureMoney) parsed = NULL;
		g_autoptr(GError) error = NULL;

		parsed = venture_money_from_string(cases[i].text, "USD", &error);

		g_assert_no_error(error);
		g_assert_nonnull(parsed);
		g_assert_cmpint(venture_money_get_amount(parsed), ==,
		                cases[i].expected);
		g_assert_cmpstr(venture_money_get_currency(parsed), ==,
		                cases[i].currency);
	}
}

static void
test_money_parse_rejects_garbage(void)
{
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(GError) error = NULL;

	parsed = venture_money_from_string("not money", "USD", &error);

	g_assert_null(parsed);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * Forms that were once read as a different number without a word: a
 * decimal comma, a second point, a credit suffix, letters inside the
 * digits. Each must now be refused, because an import that turns 1,50 EUR
 * into 150.00 EUR posts a hundred times the amount.
 */
static void
test_money_parse_refuses_ambiguous_forms(void)
{
	static const gchar *const cases[] = {
		"1.2.3", "1,50", "1,50 EUR", "1.234,56 EUR", "12,34", "1,2345.00",
		"1234,567", "1.2,3", "100.00 CR", "12abc34", ",100", "1,00,000"
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureMoney) parsed = NULL;
		g_autoptr(GError) error = NULL;

		parsed = venture_money_from_string(cases[i], "USD", &error);

		if (NULL != parsed)
			g_error("\"%s\" parsed as %" G_GINT64_FORMAT " instead of being refused",
			        cases[i], venture_money_get_amount(parsed));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}
}

static void
test_money_parse_rejects_excess_precision(void)
{
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(GError) error = NULL;

	/* More decimal places than the type can hold must fail rather than
	 * silently discard digits. Six is the most (VENTURE_MONEY_MAX_EXPONENT);
	 * seven is refused. */
	parsed = venture_money_from_string("1.2345678", "USD", &error);

	g_assert_null(parsed);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

static void
test_money_display_string(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *display = NULL;

	amount = venture_money_new(123456789, "USD", 2);
	display = venture_money_to_display_string(amount, TRUE);

	g_assert_cmpstr(display, ==, "$1,234,567.89");
}

static void
test_money_display_negative_sign_placement(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *display = NULL;

	amount = venture_money_new(-500, "USD", 2);
	display = venture_money_to_display_string(amount, FALSE);

	/* "-$5.00" reads correctly; "$-5.00" does not. */
	g_assert_cmpstr(display, ==, "-$5.00");
}

/* --- JSON round trip ----------------------------------------------------- */

static void
test_money_json_round_trip(void)
{
	g_autoptr(VentureMoney) original = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *object;

	original = venture_money_new(98765, "JPY", 0);
	node = venture_money_to_json(original);

	g_assert_nonnull(node);
	object = json_node_get_object(node);

	/* The formatted member exists so a consumer that does not understand
	 * minor units -- an AI reading a tool result -- still sees a correct
	 * figure rather than reading 98765 as a fractional amount. */
	g_assert_true(json_object_has_member(object, "formatted"));
	g_assert_cmpstr(json_object_get_string_member(object, "formatted"),
	                ==, "98765 JPY");

	parsed = venture_money_from_json(node, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_money_equal(original, parsed));
}

static void
test_money_json_accepts_bare_string(void)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(GError) error = NULL;

	node = json_node_init_string(json_node_alloc(), "42.50");
	parsed = venture_money_from_json(node, "USD", &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(parsed), ==, 4250);
}

/* --- Rescaling ----------------------------------------------------------- */

static void
test_money_rescale(void)
{
	g_autoptr(VentureMoney) fine = NULL;
	g_autoptr(VentureMoney) coarse = NULL;
	g_autoptr(GError) error = NULL;

	/* Dropping precision rounds half to even: 1.2350 becomes 1.24 by the
	 * even rule applied to the final digit. */
	fine = venture_money_new(12350, "USD", 4);
	coarse = venture_money_rescale(fine, 2, &error);

	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(coarse), ==, 124);
}

static void
test_money_rescale_rejects_excess_exponent(void)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) rescaled = NULL;
	g_autoptr(GError) error = NULL;

	amount = venture_money_new(100, "USD", 2);
	rescaled = venture_money_rescale(amount, 9, &error);

	g_assert_null(rescaled);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/* --- Currency metadata --------------------------------------------------- */

static void
test_currency_validity(void)
{
	/* The grammar is [A-Z][A-Z0-9_]{1,14}, case-insensitive on input:
	 * ISO codes and the codes an operator defines alike. */
	g_assert_true(venture_currency_is_valid("USD"));
	g_assert_true(venture_currency_is_valid("eur"));
	g_assert_true(venture_currency_is_valid("GP"));
	g_assert_true(venture_currency_is_valid("GOLD"));
	g_assert_true(venture_currency_is_valid("U5D"));
	g_assert_true(venture_currency_is_valid("LOYALTY_PTS"));
	g_assert_true(venture_currency_is_valid("ABCDEFGHIJKLMNO"));
	g_assert_false(venture_currency_is_valid("ABCDEFGHIJKLMNOP"));
	g_assert_false(venture_currency_is_valid("U"));
	g_assert_false(venture_currency_is_valid("5USD"));
	g_assert_false(venture_currency_is_valid("_USD"));
	g_assert_false(venture_currency_is_valid("US D"));
	g_assert_false(venture_currency_is_valid("US-D"));
	g_assert_false(venture_currency_is_valid(""));
	g_assert_false(venture_currency_is_valid(NULL));

	/* Normalised is the stored spelling, for byte-for-byte comparisons. */
	g_assert_true(venture_currency_is_normalised("GOLD_2"));
	g_assert_false(venture_currency_is_normalised("Gold"));
}

static void
test_currency_default(void)
{
	const gchar *original;

	original = venture_money_get_default_currency();
	g_assert_cmpstr(original, ==, "USD");

	venture_money_set_default_currency("GBP");
	g_assert_cmpstr(venture_money_get_default_currency(), ==, "GBP");

	/* Restore so test ordering cannot affect other cases. */
	venture_money_set_default_currency(original);
}

/* --- User-defined currencies ------------------------------------------------ */

static const gchar *const test_gold_denominations =
	"[{\"suffix\":\"g\",\"units\":10000},"
	"{\"suffix\":\"s\",\"units\":100},"
	"{\"suffix\":\"c\",\"units\":1}]";

/* Registers GOLD (exponent 4, g/s/c) for one test; the caller clears. */
static void
register_gold(void)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_currency_register("GOLD", 4, NULL, FALSE,
	                                        test_gold_denominations, &error));
	g_assert_no_error(error);
}

/*
 * A code longer than three letters must survive intact. With the old
 * four-byte buffer "GOLD" became "GOL" -- a different currency, silently --
 * and an over-long code must now be refused rather than cut to fit.
 */
static void
test_currency_long_codes(void)
{
	g_autoptr(VentureMoney) gold = NULL;
	g_autoptr(VentureMoney) longest = NULL;
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(GError) error = NULL;

	gold = venture_money_new(12, "gold", 2);
	g_assert_cmpstr(venture_money_get_currency(gold), ==, "GOLD");

	longest = venture_money_new(1, "ABCDEFGHIJKLMNO", 0);
	g_assert_cmpstr(venture_money_get_currency(longest), ==, "ABCDEFGHIJKLMNO");

	/* From input, a code that cannot be stored is an error, not a
	 * truncation. */
	parsed = venture_money_from_string("12", "ABCDEFGHIJKLMNOPQ", &error);
	g_assert_null(parsed);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	{
		g_autoptr(JsonNode) node = NULL;
		g_autoptr(VentureMoney) from_json = NULL;

		node = json_from_string("{\"amount\":1,\"currency\":\"ABCDEFGHIJKLMNOPQ\"}", NULL);
		from_json = venture_money_from_json(node, NULL, &error);
		g_assert_null(from_json);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION);
	}

	/* A programmer passing one is a critical, and no amount. */
	if (g_test_undefined())
	{
		g_autoptr(VentureMoney) refused = NULL;

		g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, "*store_currency*");
		refused = venture_money_new(1, "ABCDEFGHIJKLMNOPQ", 0);
		g_test_assert_expected_messages();
		g_assert_null(refused);
	}
}

/*
 * A registered code is read beside a number; an unregistered short word is
 * still refused. What breaks if this regresses: "100.00 CR" -- a credit,
 * which means the opposite sign -- becomes a hundred units of "CR".
 */
static void
test_currency_registered_code_parses(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) trailing = NULL;
	g_autoptr(VentureMoney) leading = NULL;
	g_autoptr(VentureMoney) credit = NULL;

	g_assert_true(venture_currency_register("POINTS", 0, "pts", TRUE, NULL, &error));
	g_assert_no_error(error);

	trailing = venture_money_from_string("150 POINTS", "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_money_get_currency(trailing), ==, "POINTS");
	g_assert_cmpint(venture_money_get_amount(trailing), ==, 150);
	g_assert_cmpuint(venture_money_get_exponent(trailing), ==, 0);

	leading = venture_money_from_string("points 7", "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_money_get_currency(leading), ==, "POINTS");
	g_assert_cmpint(venture_money_get_amount(leading), ==, 7);

	credit = venture_money_from_string("100.00 CR", "USD", &error);
	g_assert_null(credit);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);

	venture_currency_clear_registered();
}

/*
 * The registry answers before the built-in table, and a built-in code can
 * never be registered: redefining USD would change every figure in the
 * books without touching a row.
 */
static void
test_currency_registry(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) zero = NULL;
	const gchar *const keep[] = { "GOLD", NULL };

	g_assert_false(venture_currency_is_registered("GOLD"));
	g_assert_cmpuint(venture_currency_get_exponent("GOLD"), ==, 2);

	register_gold();
	g_assert_true(venture_currency_is_registered("gold"));
	g_assert_cmpuint(venture_currency_get_exponent("GOLD"), ==, 4);
	zero = venture_money_new_zero("GOLD");
	g_assert_cmpuint(venture_money_get_exponent(zero), ==, 4);

	g_assert_false(venture_currency_register("USD", 4, "U", FALSE, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_cmpuint(venture_currency_get_exponent("USD"), ==, 2);

	g_assert_false(venture_currency_register("GEMS", 7, NULL, FALSE, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	g_assert_false(venture_currency_register("9LIVES", 0, NULL, FALSE, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* A reload keeps what it is told to and drops the rest. */
	g_assert_true(venture_currency_register("GEMS", 0, NULL, FALSE, NULL, NULL));
	venture_currency_retain_registered(keep);
	g_assert_true(venture_currency_is_registered("GOLD"));
	g_assert_false(venture_currency_is_registered("GEMS"));

	venture_currency_clear_registered();
	g_assert_false(venture_currency_is_registered("GOLD"));
	g_assert_cmpuint(venture_currency_get_exponent("GOLD"), ==, 2);
}

/*
 * The doors to the outside -- a card processor, a tax return, a storefront
 * -- ask whether a code can be ISO money. Three letters that nobody
 * registered can (THB is not in the built-in table and Stripe still takes
 * it); a registered code of any shape cannot.
 */
static void
test_currency_is_iso(void)
{
	g_assert_true(venture_currency_is_iso("USD"));
	g_assert_true(venture_currency_is_iso("THB"));
	g_assert_false(venture_currency_is_iso("GOLD"));
	g_assert_false(venture_currency_is_iso("U5D"));
	g_assert_false(venture_currency_is_iso(NULL));

	g_assert_true(venture_currency_register("GLD", 2, NULL, FALSE, NULL, NULL));
	g_assert_false(venture_currency_is_iso("GLD"));
	g_assert_false(venture_currency_is_builtin("GLD"));
	g_assert_true(venture_currency_is_builtin("JPY"));
	venture_currency_clear_registered();
	g_assert_true(venture_currency_is_iso("GLD"));
}

/*
 * Denominations display largest first with zero parts left out, "0c" for
 * nothing, and the sign outside. What breaks if this regresses: a game's
 * books read "123456.0000 GOLD" instead of the "12g 34s 56c" everybody who
 * plays it would write.
 */
static void
test_currency_denominations_display(void)
{
	static const struct
	{
		gint64		 amount;
		guint8		 exponent;
		const gchar	*expected;
	} cases[] = {
		{ 123456, 4, "12g 34s 56c" },
		{ 120056, 4, "12g 56c" },
		{ 100000, 4, "10g" },
		{ 0, 4, "0c" },
		{ -3456, 4, "-34s 56c" },
		{ 5, 4, "5c" },
		/* A different exponent is rescaled when that is exact... */
		{ 12, 2, "12s" },
		{ 12345600, 6, "12g 34s 56c" },
		/* ...and shown in decimals when it is not, never rounded. */
		{ 1234501, 6, "1.234501 GOLD" }
	};
	gsize i;

	register_gold();

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *display = NULL;

		amount = venture_money_new(cases[i].amount, "GOLD", cases[i].exponent);
		display = venture_money_to_display_string(amount, TRUE);
		g_assert_cmpstr(display, ==, cases[i].expected);
	}

	/* The canonical form is unchanged and still round-trips. */
	{
		g_autoptr(VentureMoney) amount = venture_money_new(123456, "GOLD", 4);
		g_autofree gchar *text = venture_money_to_string(amount);
		g_autoptr(VentureMoney) back = venture_money_from_string(text, NULL, NULL);

		g_assert_cmpstr(text, ==, "12.3456 GOLD");
		g_assert_true(venture_money_equal(amount, back));
	}

	venture_currency_clear_registered();
}

/* A registered symbol goes where the currency says; no symbol shows the
 * code after the figure. Built-in currencies are untouched. */
static void
test_currency_symbol_position(void)
{
	g_autoptr(VentureMoney) points = NULL;
	g_autoptr(VentureMoney) tokens = NULL;
	g_autoptr(VentureMoney) prefix = NULL;
	g_autoptr(VentureMoney) dollars = NULL;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;

	g_assert_true(venture_currency_register("POINTS", 0, "pts", TRUE, NULL, NULL));
	g_assert_true(venture_currency_register("TOKENS", 2, NULL, FALSE, NULL, NULL));
	g_assert_true(venture_currency_register("STARS", 1, "*", FALSE, NULL, NULL));

	points = venture_money_new(-1500, "POINTS", 0);
	tokens = venture_money_new(1250, "TOKENS", 2);
	prefix = venture_money_new(15, "STARS", 1);
	dollars = venture_money_new(500, "USD", 2);

	a = venture_money_to_display_string(points, TRUE);
	b = venture_money_to_display_string(tokens, FALSE);
	c = venture_money_to_display_string(prefix, FALSE);
	d = venture_money_to_display_string(dollars, FALSE);

	g_assert_cmpstr(a, ==, "-1,500 pts");
	g_assert_cmpstr(b, ==, "12.50 TOKENS");
	g_assert_cmpstr(c, ==, "*1.5");
	g_assert_cmpstr(d, ==, "$5.00");
	g_assert_cmpstr(venture_currency_get_symbol("POINTS"), ==, "pts");

	venture_currency_clear_registered();
}

/*
 * The display form parses back: with or without the code, in any order of
 * coins, negative, with the default currency breaking a tie. Everything
 * that could be read two ways is refused.
 */
static void
test_currency_denominations_parse(void)
{
	static const struct
	{
		const gchar	*text;
		const gchar	*fallback;
		gint64		 expected;
	} accepted[] = {
		{ "12g 34s 56c", NULL, 123456 },
		{ "12g 34s 56c GOLD", NULL, 123456 },
		{ "GOLD 12g 56c", NULL, 120056 },
		{ "56c 12g", NULL, 120056 },
		{ "12G 34S", NULL, 123400 },
		{ "-5s", NULL, -500 },
		{ "(1g)", NULL, -10000 },
		{ "0c", NULL, 0 },
		{ "1g", "GOLD", 10000 }
	};
	static const gchar *const refused[] = {
		"12g 12g",	/* the same coin twice */
		"12g 5",	/* a bare number among coins */
		"12x",		/* nobody's coin */
		"12g USD",	/* USD has no coins */
		"1.5g",	/* a coin count is whole */
		"99999999999999999999c", /* out of range */
		"922337203685478g"	/* overflows once multiplied */
	};
	gsize i;

	register_gold();

	for (i = 0; i < G_N_ELEMENTS(accepted); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureMoney) parsed = NULL;

		parsed = venture_money_from_string(accepted[i].text,
			(NULL != accepted[i].fallback) ? accepted[i].fallback : "USD",
			&error);

		if (NULL != error)
			g_error("\"%s\": %s", accepted[i].text, error->message);
		g_assert_cmpstr(venture_money_get_currency(parsed), ==, "GOLD");
		g_assert_cmpuint(venture_money_get_exponent(parsed), ==, 4);
		g_assert_cmpint(venture_money_get_amount(parsed), ==, accepted[i].expected);
	}

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureMoney) parsed = NULL;

		parsed = venture_money_from_string(refused[i], "USD", &error);

		if (NULL != parsed)
			g_error("\"%s\" parsed instead of being refused", refused[i]);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}

	/* With coins registered, a symbol stuck to a number is still a
	 * decimal in the default currency. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureMoney) parsed = NULL;

		parsed = venture_money_from_string("100$", "USD", &error);
		g_assert_no_error(error);
		g_assert_cmpstr(venture_money_get_currency(parsed), ==, "USD");
		g_assert_cmpint(venture_money_get_amount(parsed), ==, 10000);
	}

	venture_currency_clear_registered();
}

/*
 * Two currencies sharing a coin: without a code the text is ambiguous and
 * refused, with one it is not, and a default currency whose coins fit
 * settles it. What breaks if this regresses: "5g" posts to whichever
 * currency the hash table happened to list first.
 */
static void
test_currency_denominations_ambiguous(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) guessed = NULL;
	g_autoptr(VentureMoney) named = NULL;
	g_autoptr(VentureMoney) defaulted = NULL;
	g_autoptr(VentureMoney) unique = NULL;

	register_gold();
	g_assert_true(venture_currency_register("GEMS", 2, NULL, FALSE,
		"[{\"suffix\":\"g\",\"units\":100},{\"suffix\":\"p\",\"units\":1}]",
		&error));
	g_assert_no_error(error);

	guessed = venture_money_from_string("5g", "USD", &error);
	g_assert_null(guessed);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_nonnull(strstr(error->message, "name the currency"));
	g_clear_error(&error);

	named = venture_money_from_string("5g GEMS", "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_money_get_currency(named), ==, "GEMS");
	g_assert_cmpint(venture_money_get_amount(named), ==, 500);

	defaulted = venture_money_from_string("5g", "GOLD", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_money_get_currency(defaulted), ==, "GOLD");

	/* A coin only one currency has is not ambiguous. */
	unique = venture_money_from_string("5g 3p", "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_money_get_currency(unique), ==, "GEMS");
	g_assert_cmpint(venture_money_get_amount(unique), ==, 503);

	venture_currency_clear_registered();
}

/* Every malformed list is refused, each for the reason a person can act
 * on; a well-formed one and "none" are accepted. */
static void
test_currency_denominations_validation(void)
{
	static const gchar *const bad[] = {
		"not json",
		"{\"suffix\":\"g\"}",
		"[1]",
		"[{\"units\":1}]",
		"[{\"suffix\":\"\",\"units\":1}]",
		"[{\"suffix\":\"g1\",\"units\":1}]",
		"[{\"suffix\":\"g s\",\"units\":1}]",
		"[{\"suffix\":\"g.\",\"units\":1}]",
		/* a coin spelled like a currency symbol would claim "100$" */
		"[{\"suffix\":\"$\",\"units\":1}]",
		"[{\"suffix\":\"\xe2\x82\xac\",\"units\":1}]",
		"[{\"suffix\":\"g\",\"units\":0}]",
		"[{\"suffix\":\"g\",\"units\":-1}]",
		"[{\"suffix\":\"g\",\"units\":1.5}]",
		/* not descending */
		"[{\"suffix\":\"c\",\"units\":1},{\"suffix\":\"g\",\"units\":100}]",
		/* does not divide */
		"[{\"suffix\":\"g\",\"units\":150},{\"suffix\":\"s\",\"units\":100},{\"suffix\":\"c\",\"units\":1}]",
		/* smallest is not one minor unit */
		"[{\"suffix\":\"g\",\"units\":100},{\"suffix\":\"s\",\"units\":10}]",
		/* a suffix twice, ignoring case */
		"[{\"suffix\":\"g\",\"units\":100},{\"suffix\":\"G\",\"units\":1}]",
		/* too many */
		"[{\"suffix\":\"a\",\"units\":256},{\"suffix\":\"b\",\"units\":128},"
		"{\"suffix\":\"c\",\"units\":64},{\"suffix\":\"d\",\"units\":32},"
		"{\"suffix\":\"e\",\"units\":16},{\"suffix\":\"f\",\"units\":8},"
		"{\"suffix\":\"h\",\"units\":4},{\"suffix\":\"i\",\"units\":2},"
		"{\"suffix\":\"j\",\"units\":1}]"
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{
		g_autoptr(GError) error = NULL;

		if (venture_currency_check_denominations(bad[i], &error))
			g_error("%s was accepted", bad[i]);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}

	g_assert_true(venture_currency_check_denominations(test_gold_denominations, NULL));
	g_assert_true(venture_currency_check_denominations(NULL, NULL));
	g_assert_true(venture_currency_check_denominations("", NULL));
	g_assert_true(venture_currency_check_denominations("[]", NULL));
	g_assert_true(venture_currency_check_denominations("null", NULL));
}

/*
 * Six decimal places is now the limit, and the arithmetic that scales to
 * it must still detect overflow rather than wrap.
 */
static void
test_money_exponent_six(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) fine = NULL;
	g_autoptr(VentureMoney) coarse = NULL;
	g_autoptr(VentureMoney) sum = NULL;
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(VentureMoney) huge = NULL;
	g_autoptr(VentureMoney) overflow = NULL;

	parsed = venture_money_from_string("1.234567 USD", NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(parsed), ==, 1234567);
	g_assert_cmpuint(venture_money_get_exponent(parsed), ==, 6);

	fine = venture_money_new(1, "USD", 6);
	coarse = venture_money_new(100, "USD", 2);
	sum = venture_money_add(fine, coarse, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(sum), ==, 1000001);

	/* Widening a huge two-decimal amount to six overflows, and says so. */
	huge = venture_money_new(G_MAXINT64 / 1000, "USD", 2);
	overflow = venture_money_add(huge, fine, &error);
	g_assert_null(overflow);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * Converting into a six-decimal currency scales by up to a million on top
 * of the rate; the wide intermediate must catch a result that no longer
 * fits rather than wrap into a plausible wrong figure.
 */
static void
test_money_convert_to_exponent_six(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) dollars = NULL;
	g_autoptr(VentureMoney) micro = NULL;
	g_autoptr(VentureMoney) back = NULL;
	g_autoptr(VentureMoney) too_big = NULL;
	g_autoptr(VentureMoney) overflow = NULL;

	g_assert_true(venture_currency_register("MICRO", 6, NULL, FALSE, NULL, NULL));

	/* $1.00 at 3 MICRO per dollar is 3.000000 MICRO. */
	dollars = venture_money_new(100, "USD", 2);
	micro = venture_money_convert_at_rate(dollars, 3, 1, "MICRO", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(micro), ==, 3000000);
	g_assert_cmpuint(venture_money_get_exponent(micro), ==, 6);

	/* And back: 1/3 of a dollar per MICRO, rounded once, half to even. */
	back = venture_money_convert_at_rate(micro, 1, 3, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(back), ==, 100);

	too_big = venture_money_new(G_MAXINT64 / 100, "USD", 2);
	overflow = venture_money_convert_at_rate(too_big, 1000, 1, "MICRO", &error);
	g_assert_null(overflow);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);

	venture_currency_clear_registered();
}

/* FX must round once, after both the rate and decimal scaling. */
static void
test_money_fx_precision(void)
{
	static const struct { gint64 minor, num, den, expected; const gchar *from, *to; } cases[] = {
		{ 50, 150, 1, 75, "USD", "JPY" },
		{ -50, 150, 1, -75, "USD", "JPY" },
		{ 100, 1, 150, 67, "JPY", "USD" },
		{ 1, 50, 1, 0, "USD", "JPY" },
		{ 3, 50, 1, 2, "USD", "JPY" },
		{ G_MAXINT64, G_MAXINT64, G_MAXINT64, G_MAXINT64, "USD", "EUR" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureMoney) source = venture_money_new_for_currency(cases[i].minor, cases[i].from);
		g_autoptr(VentureMoney) result = venture_money_convert_at_rate(source,
			cases[i].num, cases[i].den, cases[i].to, &error);
		g_assert_no_error(error);
		g_assert_nonnull(result);
		g_assert_cmpint(result->amount, ==, cases[i].expected);
	}
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureMoney) source = venture_money_new_for_currency(G_MAXINT64, "JPY");
		g_autoptr(VentureMoney) result = venture_money_convert_at_rate(source, 1, 1, "USD", &error);
		g_assert_null(result);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	}
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/money/new", test_money_new);
	g_test_add_func("/money/currency-normalised", test_money_currency_normalised);
	g_test_add_func("/money/zero-decimal-currency", test_money_zero_decimal_currency);
	g_test_add_func("/money/three-decimal-currency", test_money_three_decimal_currency);

	g_test_add_func("/money/add", test_money_add);
	g_test_add_func("/money/add-differing-exponents", test_money_add_differing_exponents);
	g_test_add_func("/money/add-mixed-currency-fails", test_money_add_mixed_currency_fails);
	g_test_add_func("/money/subtract", test_money_subtract);
	g_test_add_func("/money/add-overflow-detected", test_money_add_overflow_detected);

	g_test_add_func("/money/multiply-int", test_money_multiply_int);
	g_test_add_func("/money/multiply-percent", test_money_multiply_percent);
	g_test_add_func("/money/rounding-half-to-even", test_money_rounding_half_to_even);
	g_test_add_func("/money/rounding-negative", test_money_rounding_negative);
	g_test_add_func("/money/multiply-zero-denominator-fails",
	                test_money_multiply_zero_denominator_fails);

	g_test_add_func("/money/allocate-evenly-preserves-total",
	                test_money_allocate_evenly_preserves_total);
	g_test_add_func("/money/allocate-by-ratio", test_money_allocate_by_ratio);
	g_test_add_func("/money/allocate-negative-preserves-total",
	                test_money_allocate_negative_preserves_total);
	g_test_add_func("/money/allocate-rejects-zero-parts",
	                test_money_allocate_rejects_zero_parts);
	g_test_add_func("/money/sum-empty-is-zero", test_money_sum_empty_is_zero);

	g_test_add_func("/money/compare-across-exponents", test_money_compare_across_exponents);

	g_test_add_func("/money/to-string-round-trip", test_money_to_string_round_trip);
	g_test_add_func("/money/parse-tolerant-forms", test_money_parse_tolerant_forms);
	g_test_add_func("/money/parse-rejects-garbage", test_money_parse_rejects_garbage);
	g_test_add_func("/money/parse-refuses-ambiguous-forms",
	                test_money_parse_refuses_ambiguous_forms);
	g_test_add_func("/money/parse-rejects-excess-precision",
	                test_money_parse_rejects_excess_precision);
	g_test_add_func("/money/display-string", test_money_display_string);
	g_test_add_func("/money/display-negative-sign-placement",
	                test_money_display_negative_sign_placement);

	g_test_add_func("/money/json-round-trip", test_money_json_round_trip);
	g_test_add_func("/money/json-accepts-bare-string", test_money_json_accepts_bare_string);

	g_test_add_func("/money/rescale", test_money_rescale);
	g_test_add_func("/money/rescale-rejects-excess-exponent",
	                test_money_rescale_rejects_excess_exponent);

	g_test_add_func("/currency/validity", test_currency_validity);
	g_test_add_func("/currency/default", test_currency_default);

	g_test_add_func("/money/fx-precision", test_money_fx_precision);
	g_test_add_func("/money/exponent-six", test_money_exponent_six);
	g_test_add_func("/money/convert-to-exponent-six", test_money_convert_to_exponent_six);
	g_test_add_func("/currency/long-codes", test_currency_long_codes);
	g_test_add_func("/currency/registered-code-parses", test_currency_registered_code_parses);
	g_test_add_func("/currency/registry", test_currency_registry);
	g_test_add_func("/currency/is-iso", test_currency_is_iso);
	g_test_add_func("/currency/denominations-display", test_currency_denominations_display);
	g_test_add_func("/currency/symbol-position", test_currency_symbol_position);
	g_test_add_func("/currency/denominations-parse", test_currency_denominations_parse);
	g_test_add_func("/currency/denominations-ambiguous", test_currency_denominations_ambiguous);
	g_test_add_func("/currency/denominations-validation", test_currency_denominations_validation);
	return g_test_run();
}
