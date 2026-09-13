/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_records(void)
{
	const gchar *names[] = { "bank_account", "bank_statement", "bank_transaction", "bank_match", "reconciliation" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/banking/records", test_records);
	return g_test_run();
}
