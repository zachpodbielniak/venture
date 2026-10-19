/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* 1. Tags. One model: the comma-separated tags field contacts and
 * companies already have. Matching ignores capitals and spacing, and a
 * tag is a whole word between commas -- "vip" is not in "vipers". */
static void test_tag_model(void)
{
	g_auto(GStrv) parsed = venture_tags_parse(" VIP, early ,, vip,Early Bird ");
	g_auto(GStrv) added = venture_tags_added("vip, early", "Early, VIP, renewal, gold");
	g_auto(GStrv) none = venture_tags_added("vip", "VIP ");
	g_autofree gchar *with = venture_tags_add("vip, early", "Renewal");
	g_autofree gchar *again = venture_tags_add("vip, early", "VIP");
	g_autofree gchar *without = venture_tags_remove("vip, Early, renewal", "early");
	g_assert_nonnull(parsed);
	g_assert_cmpuint(g_strv_length(parsed), ==, 3);
	g_assert_cmpstr(parsed[0], ==, "VIP");
	g_assert_cmpstr(parsed[1], ==, "early");
	g_assert_cmpstr(parsed[2], ==, "Early Bird");
	g_assert_true(venture_tags_has("vipers, VIP", "vip"));
	g_assert_false(venture_tags_has("vipers, gold", "vip"));
	g_assert_false(venture_tags_has(NULL, "vip"));
	g_assert_nonnull(added);
	g_assert_cmpuint(g_strv_length(added), ==, 2);
	g_assert_cmpstr(added[0], ==, "renewal");
	g_assert_cmpstr(added[1], ==, "gold");
	g_assert_nonnull(none);
	g_assert_cmpuint(g_strv_length(none), ==, 0);
	g_assert_cmpstr(with, ==, "vip, early, Renewal");
	g_assert_cmpstr(again, ==, "vip, early");
	g_assert_cmpstr(without, ==, "vip, renewal");
	g_assert_true(venture_tags_valid("Early bird"));
	g_assert_false(venture_tags_valid("a,b"));
	g_assert_false(venture_tags_valid("  "));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tags/model", test_tag_model);
	return g_test_run();
}
