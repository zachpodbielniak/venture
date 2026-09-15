/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_write_off_declared(void)
{
	g_assert_true(g_type_is_a(VENTURE_TYPE_SETTLEMENT_SERVICE, G_TYPE_OBJECT));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-settlement-service/declared", test_write_off_declared);
	return g_test_run();
}
#include <venture.h>
int main(int argc, char **argv) { g_test_init(&argc, &argv, NULL); return g_test_run(); }
