/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_write_off_action_exists(void)
{
	g_assert_cmpstr("write-off", ==, "write-off");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-web-server/write-off-action", test_write_off_action_exists);
	return g_test_run();
}
