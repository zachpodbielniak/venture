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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/quote-records/percentage-parts", test_parts);
	return g_test_run();
}
