/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_rate_table_type(void)
{
	g_assert_true(g_type_is_a(VENTURE_TYPE_RATE_TABLE_POLICY, G_TYPE_OBJECT));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-posting-rule/rate-table-type", test_rate_table_type);
	return g_test_run();
}
