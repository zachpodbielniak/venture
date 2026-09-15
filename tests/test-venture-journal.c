/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_exchange_rate_type(void)
{
	g_assert_true(g_type_is_a(VENTURE_TYPE_EXCHANGE_RATE, VENTURE_TYPE_ENTITY));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-journal/exchange-rate-type", test_exchange_rate_type);
	return g_test_run();
}
