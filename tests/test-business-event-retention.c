/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_queue_failure(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebhook) hook = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(VentureQuery) events = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT);
	g_autoptr(VentureQuery) leads = venture_query_new(VENTURE_TYPE_LEAD);
	gint64 organization;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	organization = venture_context_get_default_organization_id(context);
	hook = g_object_new(VENTURE_TYPE_WEBHOOK, "organization-id", organization,
		"name", "Example receiver", "url", "https://receiver.example.test", "active", TRUE,
		"business-events", TRUE, "events", "lead.created", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(hook), NULL, &error));
	g_assert_true(venture_database_execute(database, "CREATE TRIGGER reject_delivery BEFORE INSERT ON business_event_deliveries BEGIN SELECT RAISE(ABORT, 'queue unavailable'); END", NULL, &error));
	g_assert_no_error(error);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Example inquiry", NULL);
	g_assert_false(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_assert_cmpint(venture_database_count(database, events, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpint(venture_database_count(database, leads, &error), ==, 0);
	g_assert_no_error(error);
}

static void
test_history_is_immutable(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(VentureBusinessEvent) invented = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT);
	g_autoptr(JsonNode) replay = NULL;
	gint64 organization;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	organization = venture_context_get_default_organization_id(context);
	invented = g_object_new(VENTURE_TYPE_BUSINESS_EVENT, "organization-id", organization, "event", "invoice.paid", NULL);
	g_assert_false(venture_database_save(database, VENTURE_ENTITY(invented), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Example inquiry", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_no_error(error);
	event = venture_database_find_one(database, query, &error);
	g_assert_no_error(error); g_assert_nonnull(event);
	g_object_set(event, "event", "invoice.paid", NULL);
	g_assert_false(venture_database_save(database, event, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_database_delete(database, event, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_false(venture_database_purge(database, event, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_assert_true(venture_database_delete(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_true(venture_database_purge(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_no_error(error);
	replay = venture_business_events_read(database, organization, 0, 10, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(
		json_object_get_array_member(json_node_get_object(replay), "events"), 0), "event"), ==, "lead.created");
}

static void
test_closed_organization_is_deferred(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureOrganization) organization = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Example organization", "active", TRUE, NULL);
	g_autoptr(VentureWebhook) hook = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), clock = g_date_time_add_hours(now, 1);
	g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *address = g_inet_address_to_string(loopback), *url = g_strdup_printf("http://%s:9", address);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT_DELIVERY);
	g_autoptr(VentureEntity) delivery = NULL;
	g_autoptr(GDateTime) next = NULL;
	gint64 attempts = -1, org;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(organization), NULL, &error));
	g_assert_no_error(error);
	org = venture_entity_get_id(VENTURE_ENTITY(organization));
	hook = g_object_new(VENTURE_TYPE_WEBHOOK, "organization-id", org, "name", "Example receiver", "url", url,
		"business-events", TRUE, "active", TRUE, "events", "lead.created", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(hook), NULL, &error));
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", org, "name", "Example inquiry", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_true(venture_database_delete(database, VENTURE_ENTITY(organization), NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_business_events_dispatch(context, clock, &error));
	g_assert_no_error(error);
	delivery = venture_database_find_one(database, query, &error);
	g_assert_no_error(error); g_assert_nonnull(delivery);
	g_object_get(delivery, "attempts", &attempts, "next-attempt-at", &next, NULL);
	g_assert_cmpint(attempts, ==, 0);
	g_assert_cmpint(g_date_time_compare(next, clock), >, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/business-event-retention/queue-failure", test_queue_failure);
	g_test_add_func("/business-event-retention/immutable-history", test_history_is_immutable);
	g_test_add_func("/business-event-retention/closed-organization", test_closed_organization_is_deferred);
	return g_test_run();
}
