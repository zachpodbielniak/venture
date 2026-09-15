/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_exchange_rate_record(void)
{
	g_autoptr(VentureExchangeRate) rate = venture_exchange_rate_new();
	g_autofree gchar *from = NULL;

	g_object_set(rate, "from-currency", "EUR", "to-currency", "USD",
		"rate-numerator", (gint64)110, "rate-denominator", (gint64)100,
		"source", "manual", NULL);
	g_object_get(rate, "from-currency", &from, NULL);
	g_assert_cmpstr(from, ==, "EUR");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/journal/exchange-rate-record", test_exchange_rate_record);
	return g_test_run();
}
