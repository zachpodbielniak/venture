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
		{ "12.34 EUR",      1234,   "EUR" }
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

static void
test_money_parse_rejects_excess_precision(void)
{
	g_autoptr(VentureMoney) parsed = NULL;
	g_autoptr(GError) error = NULL;

	/* More decimal places than the type can hold must fail rather than
	 * silently discard digits. */
	parsed = venture_money_from_string("1.234567", "USD", &error);

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
	g_assert_true(venture_currency_is_valid("USD"));
	g_assert_true(venture_currency_is_valid("eur"));
	g_assert_false(venture_currency_is_valid("US"));
	g_assert_false(venture_currency_is_valid("USDD"));
	g_assert_false(venture_currency_is_valid("U5D"));
	g_assert_false(venture_currency_is_valid(NULL));
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

	return g_test_run();
}
