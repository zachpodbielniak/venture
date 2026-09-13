/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_records(void)
{
	static const gchar *const names[] = {
		"stripe_price_link", "stripe_customer_link", "stripe_checkout", "stripe_event"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(
			venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/stripe/records", test_records);
	return g_test_run();
}
