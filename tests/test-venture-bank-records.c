/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_match_window_field(void)
{
	g_autoptr(VentureBankAccount) account = venture_bank_account_new();
	g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(account), "match-window-days"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-bank-records/match-window", test_match_window_field);
	return g_test_run();
}
