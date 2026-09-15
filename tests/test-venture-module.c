/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_finance_owns_tax_code(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	VentureModule *finance;
	VentureModule *ledger;

	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, database);
	finance = venture_module_registry_lookup(venture_context_get_modules(context), "finance");
	ledger = venture_module_registry_lookup(venture_context_get_modules(context), "ledger");
	g_assert_true(g_strv_contains(venture_module_get_entity_names(finance), "tax_code"));
	g_assert_true(g_strv_contains(venture_module_get_entity_names(ledger), "exchange_rate"));
	g_assert_true(g_strv_contains(venture_module_get_reports(finance), "tax_liability"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-module/tax-code", test_finance_owns_tax_code);
	return g_test_run();
}
#include <venture.h>
int main(int argc, char **argv) { g_test_init(&argc, &argv, NULL); return g_test_run(); }
