/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_search_declared(void)
{
	g_assert_true(TRUE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-bank-match-service/declared", test_search_declared);
	return g_test_run();
}
