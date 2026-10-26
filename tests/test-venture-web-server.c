/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Outbound webhooks are a business's destinations. Hosted mode refuses
 * platform records, so the type itself has to be tenant data. */
static void
test_webhook_records_belong_to_the_business(void)
{
	/* Classification is applied when the built-in registry is built. */
	g_assert_nonnull(venture_entity_registry_get_default());
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_WEBHOOK),
		==, VENTURE_DATA_CLASS_TENANT);
	g_assert_cmpint(venture_data_class_for_type(VENTURE_TYPE_WEBHOOK_DELIVERY),
		==, VENTURE_DATA_CLASS_TENANT);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/web-server/webhook-records-are-tenant",
		test_webhook_records_belong_to_the_business);
	return g_test_run();
}
