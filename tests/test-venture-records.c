/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_tax_code_fields(void)
{
	g_autoptr(VentureTaxCode) code = venture_tax_code_new();
	g_autoptr(GError) error = NULL;
	gint64 numerator = 0;
	gint64 denominator = 0;

	g_object_set(code, "code", "VAT", "jurisdiction", "EU",
		"rate-numerator", (gint64)20, "rate-denominator", (gint64)100, NULL);
	g_assert_true(venture_tax_code_get_rate(code, &numerator, &denominator, &error));
	g_assert_cmpint(numerator, ==, 20);
	g_assert_cmpint(denominator, ==, 100);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-records/tax-code-fields", test_tax_code_fields);
	return g_test_run();
}
#include <venture.h>
int main(int argc, char **argv) { g_test_init(&argc, &argv, NULL); return g_test_run(); }
