/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Every surface must discover the same five durable record types. */
static void
test_records(void)
{
	const gchar *names[] = { "sequence", "sequence_step", "sequence_enrollment",
		"sequence_delivery", "suppression" };
	guint i;
	VentureEntityRegistry *registry = venture_entity_registry_get_default();

	venture_entity_registry_register_builtins(registry);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]), !=, G_TYPE_INVALID);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sequences/records", test_records);
	return g_test_run();
}
