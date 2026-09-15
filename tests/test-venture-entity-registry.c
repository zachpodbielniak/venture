/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_tax_and_rate_types(void)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "tax_code"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "exchange_rate"), !=, G_TYPE_INVALID);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-entity-registry/tax-and-rate-types", test_tax_and_rate_types);
	return g_test_run();
}
