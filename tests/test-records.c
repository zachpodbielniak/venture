/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_tax_code_rate(void)
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
	g_assert_no_error(error);
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
	g_test_add_func("/records/tax-code-rate", test_tax_code_rate);
	return g_test_run();
}
