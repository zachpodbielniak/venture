/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_tax_liability_registered(void)
{
	g_autoptr(VentureReportRegistry) registry = venture_report_registry_new();
	venture_report_registry_register_builtins(registry);
	g_assert_nonnull(venture_report_registry_lookup(registry, "tax_liability"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-receivable-reports/tax-liability", test_tax_liability_registered);
	return g_test_run();
}
