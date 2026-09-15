/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_bill_line_tax_code(void)
{
	g_autoptr(VentureVendorBillLine) line = venture_vendor_bill_line_new();
	gint64 tax_code_id = 0;

	g_object_set(line, "tax-code-id", (gint64)3, NULL);
	g_object_get(line, "tax-code-id", &tax_code_id, NULL);
	g_assert_cmpint(tax_code_id, ==, 3);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-payable-records/tax-code", test_bill_line_tax_code);
	return g_test_run();
}
