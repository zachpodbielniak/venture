/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

static void
test_mileage_amount(void)
{
	g_autoptr(VentureExpenseClaimLine) line = venture_expense_claim_line_new();
	g_autoptr(VentureMoney) rate = venture_money_new_for_currency(67, "USD");
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(line, "kind", "mileage", "description", "Site visit",
		"miles", "100", "mileage-rate", rate, NULL);
	amount = venture_expense_claim_line_get_amount(line, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(amount), ==, 6700);
}

static void
test_names(void)
{
	g_assert_cmpuint(VENTURE_TYPE_EXPENSE_CLAIM, !=, G_TYPE_INVALID);
	g_assert_cmpuint(VENTURE_TYPE_EXPENSE_CLAIM_LINE, !=, G_TYPE_INVALID);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/claim-records/names", test_names);
	g_test_add_func("/claim-records/mileage", test_mileage_amount);
	return g_test_run();
}
