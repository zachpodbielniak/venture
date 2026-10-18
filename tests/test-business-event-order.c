/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-accounting.h"
#include "venture-test-util.h"

static void
test_commit_order(void)
{
	const gchar *external = g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI");
	g_autofree gchar *directory = g_dir_make_tmp("venture-event-order-XXXXXX", NULL);
	g_autofree gchar *uri = external ? g_strdup(external) : g_strdup_printf("sqlite://%s/events.db", directory);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) first = external ? venture_test_accounting_database(&error) : venture_database_new(uri, &error);
	g_autoptr(VentureDatabase) second = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL, other = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(VentureSubscriber) subscriber = NULL;
	g_autoptr(VentureNewsletter) newsletter = NULL;
	g_autoptr(JsonNode) page = NULL, next = NULL;
	const gchar *schema;
	gint64 organization, cursor;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(first, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, first);
	organization = venture_context_get_default_organization_id(context);
	second = venture_database_new(uri, &error);
	g_assert_no_error(error);
	schema = g_object_get_data(G_OBJECT(first), "accounting-test-schema");
	if (schema != NULL)
	{
		g_autofree gchar *sql = g_strdup_printf("SET search_path TO %s; SET lock_timeout TO '100ms'", schema);
		g_assert_true(venture_database_execute(second, sql, NULL, &error));
	}
	else
		g_assert_true(venture_database_execute(second, "PRAGMA busy_timeout=100", NULL, &error));
	g_assert_no_error(error);
	other = venture_context_new(config, second);
	newsletter = g_object_new(VENTURE_TYPE_NEWSLETTER, "organization-id", organization, "name", "Example updates", NULL);
	g_assert_true(venture_database_save(first, VENTURE_ENTITY(newsletter), NULL, &error));
	g_assert_no_error(error);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "First inquiry", NULL);
	subscriber = g_object_new(VENTURE_TYPE_SUBSCRIBER, "organization-id", organization,
		"newsletter-id", venture_entity_get_id(VENTURE_ENTITY(newsletter)), "email", "reader@example.test", NULL);
	g_assert_true(venture_database_begin(first, &error));
	g_assert_true(venture_database_save(first, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_no_error(error);
	/* A later writer must not commit a higher cursor ahead of an earlier
	 * uncommitted event. A bounded lock timeout proves it waits. */
	g_assert_false(venture_database_save(second, VENTURE_ENTITY(subscriber), NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_assert_true(venture_database_commit(first, &error));
	g_assert_no_error(error);
	page = venture_business_events_read(first, organization, 0, 10, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(page), "events")), ==, 1);
	cursor = json_object_get_int_member(json_node_get_object(page), "next_cursor");
	g_clear_object(&subscriber);
	subscriber = g_object_new(VENTURE_TYPE_SUBSCRIBER, "organization-id", organization,
		"newsletter-id", venture_entity_get_id(VENTURE_ENTITY(newsletter)), "email", "reader@example.test", NULL);
	g_assert_true(venture_database_save(second, VENTURE_ENTITY(subscriber), NULL, &error));
	g_assert_no_error(error);
	next = venture_business_events_read(first, organization, cursor, 10, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(next), "events")), ==, 1);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(next), "next_cursor"), >, cursor);
	g_clear_object(&other); g_clear_object(&second); g_clear_object(&context);
	venture_test_accounting_database_cleanup(first); g_clear_object(&first);
	venture_test_remove_tree(directory);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/business-event-order/commit-order", test_commit_order);
	return g_test_run();
}
