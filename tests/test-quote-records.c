/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_parts(void)
{
	g_autoptr(VentureMoney) subtotal = venture_money_new_for_currency(10000, "USD");
	g_autoptr(VentureMoney) discount = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_quote_percentage_parts(subtotal, 10, 5, &discount, &net, &tax, &total, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(discount), ==, 1000);
	g_assert_cmpint(venture_money_get_amount(net), ==, 9000);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 450);
	g_assert_cmpint(venture_money_get_amount(total), ==, 9450);
}

static void
test_rate_parts(void)
{
	g_autoptr(VentureMoney) subtotal = venture_money_new_for_currency(8000, "USD");
	g_autoptr(VentureMoney) discount = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GError) error = NULL;

	/* 8.875% is 8875/100000, not a truncated integer percent. */
	g_assert_true(venture_quote_rate_parts(subtotal, 0, 8875, 100000,
		&discount, &net, &tax, &total, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(net), ==, 8000);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 710);
	g_assert_cmpint(venture_money_get_amount(total), ==, 8710);
	g_assert_false(venture_quote_rate_parts(subtotal, 0, 1, 0, NULL, NULL, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

static void
test_tax_code_levy(void)
{
	g_autoptr(VentureTaxCode) code = venture_tax_code_new();
	g_autoptr(VentureMoney) net = venture_money_new_for_currency(8000, "USD");
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(GError) error = NULL;
	gint64 numerator = 0;
	gint64 denominator = 0;

	g_object_set(code, "code", "NY-8875", "name", "NY combined",
		"jurisdiction", "US-NY", "rate-numerator", (gint64)8875,
		"rate-denominator", (gint64)100000, "recoverable", FALSE, NULL);
	g_assert_true(venture_tax_code_get_rate(code, &numerator, &denominator, &error));
	g_assert_cmpint(numerator, ==, 8875);
	g_assert_cmpint(denominator, ==, 100000);
	tax = venture_tax_code_levy(code, net, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 710);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/quote-records/percentage-parts", test_parts);
	g_test_add_func("/quote-records/rate-parts", test_rate_parts);
	g_test_add_func("/quote-records/tax-code-levy", test_tax_code_levy);
	return g_test_run();
}
