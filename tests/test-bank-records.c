/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Field-table coverage for src/banking/venture-bank-records.c */
#include <venture.h>

static void
test_match_window_field(void)
{
	g_autoptr(VentureBankAccount) account = venture_bank_account_new();
	GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(account), "match-window-days");
	g_assert_nonnull(spec);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bank-records/match-window", test_match_window_field);
	return g_test_run();
}
