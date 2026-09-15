/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_quote_rate_parts(void)
{
	g_autoptr(VentureMoney) subtotal = venture_money_new_for_currency(8000, "USD");
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_quote_rate_parts(subtotal, 0, 8875, 100000,
		NULL, NULL, &tax, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(tax), ==, 710);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-quote-records/rate-parts", test_quote_rate_parts);
	return g_test_run();
}
