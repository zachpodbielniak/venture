/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Covers src/tax/venture-tax-records.h and src/tax/venture-tax-records.c */
#include <venture.h>

static void
test_tax_records_registered(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "tax_filing"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "contractor_tax_form"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "contractor_tax_pack"), !=, G_TYPE_INVALID);
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tax-records/registered", test_tax_records_registered);
	return g_test_run();
}
