/* SPDX-License-Identifier: AGPL-3.0-or-later
 * API coverage for src/banking/venture-bank-match-service.c */
#include <venture.h>

static void
test_search_symbol(void)
{
	g_assert_nonnull(venture_bank_transaction_candidates_search);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bank-match-service/search-symbol", test_search_symbol);
	return g_test_run();
}
