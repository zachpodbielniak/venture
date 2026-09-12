/*
 * test-webhook.c - Webhooks out, routing rules, satisfaction and triage
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The promises here are the ones an integration is trusted on: the right
 * events reach the right endpoint, the body is signed with something the
 * far end can check, a delivery that fails is recorded and eventually
 * switches a dead endpoint off, and nothing about a webhook fires a
 * webhook. Beside them, the desk's two smaller promises: a new ticket
 * lands on somebody, and a rating turns into a score.
 */

#include <venture.h>

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;

	/* The far end: a real HTTP server this test owns. */
	SoupServer	*endpoint;
	gchar		*endpoint_url;
	guint		 received;
	gchar		*last_body;
	gchar		*last_signature;
	gchar		*last_event;
	guint		 answer_with;
} Fixture;

static void
file_under_default(
	Fixture		*fixture,
	gpointer	 record
){
	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));
}

static void
endpoint_handler(
	SoupServer		*server,
	SoupServerMessage	*message,
	const gchar		*path,
	GHashTable		*query,
	gpointer		 user_data
){
	Fixture *fixture = user_data;
	SoupMessageHeaders *headers;
	SoupMessageBody *body;

	(void)server;
	(void)path;
	(void)query;

	headers = soup_server_message_get_request_headers(message);
	body = soup_server_message_get_request_body(message);

	fixture->received++;
	g_free(fixture->last_signature);
	fixture->last_signature = g_strdup(
		soup_message_headers_get_one(headers, "X-Venture-Signature"));
	g_free(fixture->last_event);
	fixture->last_event = g_strdup(
		soup_message_headers_get_one(headers, "X-Venture-Event"));
	g_free(fixture->last_body);
	fixture->last_body = g_strndup(body->data, (gsize)body->length);

	soup_server_message_set_status(message, fixture->answer_with, NULL);
	soup_server_message_set_response(message, "text/plain", SOUP_MEMORY_COPY,
	                                 "ok", 2);
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GSList) uris = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->answer_with = SOUP_STATUS_OK;
	fixture->endpoint = soup_server_new(NULL, NULL);
	soup_server_add_handler(fixture->endpoint, "/", endpoint_handler, fixture,
	                        NULL);
	g_assert_true(soup_server_listen_local(fixture->endpoint, 0,
	                                       SOUP_SERVER_LISTEN_IPV4_ONLY,
	                                       &error));
	g_assert_no_error(error);

	uris = soup_server_get_uris(fixture->endpoint);
	g_assert_nonnull(uris);
	fixture->endpoint_url = g_uri_to_string(uris->data);
	g_slist_free_full(g_steal_pointer(&uris), (GDestroyNotify)g_uri_unref);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->endpoint)
		soup_server_disconnect(fixture->endpoint);

	g_clear_object(&fixture->endpoint);
	g_clear_pointer(&fixture->endpoint_url, g_free);
	g_clear_pointer(&fixture->last_body, g_free);
	g_clear_pointer(&fixture->last_signature, g_free);
	g_clear_pointer(&fixture->last_event, g_free);

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

static gint64
count_of(
	Fixture	*fixture,
	GType	 type
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(type);

	return venture_database_count(fixture->database, query, NULL);
}

/*
 * Deliveries go out asynchronously, so a test that asserts straight
 * after a save asserts on nothing. Spin the loop until as many
 * deliveries as expected have been recorded, bounded -- a test that can
 * hang is worse than one that fails.
 */
static void
settle(
	Fixture	*fixture,
	gint64	 expected
){
	gint64 waited;

	for (waited = 0; waited < 5000; waited += 10)
	{
		if (count_of(fixture, VENTURE_TYPE_WEBHOOK_DELIVERY) >= expected)
			break;

		g_main_context_iteration(NULL, FALSE);
		g_usleep(10 * 1000);
	}

	/* One more pass, so a delivery that arrived on the last iteration
	 * has had its record written. */
	while (g_main_context_iteration(NULL, FALSE))
		;
}

static VentureEntity *
create_webhook(
	Fixture		*fixture,
	const gchar	*events,
	const gchar	*secret
){
	VentureWebhook *webhook;

	webhook = venture_webhook_new();
	g_object_set(webhook, "name", "Ops", "url", fixture->endpoint_url,
	             "events", events, "secret", secret, "active", TRUE, NULL);
	file_under_default(fixture, webhook);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(webhook), NULL, NULL));

	return VENTURE_ENTITY(webhook);
}

static VentureEntity *
create_ticket(
	Fixture		*fixture,
	const gchar	*title
){
	VentureTicket *ticket;

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", title, "status", VENTURE_TICKET_STATUS_TODO,
	             NULL);
	file_under_default(fixture, ticket);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, NULL));

	return VENTURE_ENTITY(ticket);
}

/* --- Naming and matching --------------------------------------------------- */

/*
 * An event is the record type and what happened to it, and the patterns
 * a webhook subscribes to are exact, per-type or everything.
 */
static void
test_webhook_events_are_named_and_matched(void)
{
	g_autofree gchar *created = NULL;
	g_autofree gchar *updated = NULL;
	g_autofree gchar *deleted = NULL;

	created = venture_webhook_event_name("ticket",
	                                     VENTURE_AUDIT_ACTION_CREATE);
	updated = venture_webhook_event_name("invoice",
	                                     VENTURE_AUDIT_ACTION_UPDATE);
	deleted = venture_webhook_event_name("sale", VENTURE_AUDIT_ACTION_DELETE);

	g_assert_cmpstr(created, ==, "ticket.created");
	g_assert_cmpstr(updated, ==, "invoice.updated");
	g_assert_cmpstr(deleted, ==, "sale.deleted");

	/* A login is not a record change and is published as nothing. */
	g_assert_null(venture_webhook_event_name("user",
	                                         VENTURE_AUDIT_ACTION_LOGIN));
	g_assert_null(venture_webhook_event_name(NULL,
	                                         VENTURE_AUDIT_ACTION_CREATE));

	g_assert_true(venture_webhook_matches("*", "ticket.created"));
	g_assert_true(venture_webhook_matches("ticket.*", "ticket.created"));
	g_assert_true(venture_webhook_matches("ticket.*", "ticket.deleted"));
	g_assert_true(venture_webhook_matches("invoice.paid, ticket.created",
	                                      "ticket.created"));
	g_assert_true(venture_webhook_matches(" ticket.created ",
	                                      "ticket.created"));

	g_assert_false(venture_webhook_matches("ticket.*", "invoice.created"));
	g_assert_false(venture_webhook_matches("ticket.created",
	                                       "ticket.updated"));

	/* A prefix is not a type: ticket.* must not swallow ticket_link. */
	g_assert_false(venture_webhook_matches("ticket.*", "ticket_link.created"));

	/* Nothing named is not yet narrowed, so everything matches. */
	g_assert_true(venture_webhook_matches("", "anything.created"));
	g_assert_true(venture_webhook_matches(NULL, "anything.created"));
	g_assert_false(venture_webhook_matches("*", NULL));
}

/* --- Delivery -------------------------------------------------------------- */

/*
 * A record changes, the matching webhook receives a signed body naming
 * it, and what happened is recorded.
 *
 * What breaks if this regresses: an integration that silently stops, or
 * one that cannot tell VENTURE's deliveries from anybody else's POST.
 */
static void
test_webhook_delivers_signed(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autofree gchar *expected = NULL;
	JsonObject *object;
	JsonObject *record;

	(void)user_data;

	webhook = create_webhook(fixture, "ticket.*", "a-shared-secret");
	ticket = create_ticket(fixture, "Printer on fire");
	settle(fixture, 1);

	g_assert_cmpuint(fixture->received, ==, 1);
	g_assert_cmpstr(fixture->last_event, ==, "ticket.created");

	/* The signature is an HMAC over exactly the bytes that arrived, so
	 * computing it again here is what the far end would do. */
	expected = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
		(const guchar *)"a-shared-secret", strlen("a-shared-secret"),
		(const guchar *)fixture->last_body, strlen(fixture->last_body));
	g_assert_nonnull(fixture->last_signature);
	g_assert_true(g_str_has_prefix(fixture->last_signature, "sha256="));
	g_assert_cmpstr(fixture->last_signature + strlen("sha256="), ==, expected);

	body = venture_json_parse(fixture->last_body, NULL);
	g_assert_nonnull(body);
	object = json_node_get_object(body);
	g_assert_cmpstr(venture_json_object_get_string(object, "event", ""), ==,
	                "ticket.created");
	g_assert_nonnull(venture_json_object_get_string(object, "delivery", NULL));

	record = json_object_get_object_member(object, "record");
	g_assert_cmpstr(venture_json_object_get_string(record, "type", ""), ==,
	                "ticket");
	g_assert_cmpint(venture_json_object_get_int(record, "id", 0), ==,
	                venture_entity_get_id(ticket));
	g_assert_cmpstr(venture_json_object_get_string(record, "label", ""), ==,
	                "Printer on fire");

	/* The record itself only when asked for. */
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(object, "data")));

	/* And the delivery was kept. */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) delivery = NULL;
		VentureDeliveryState state;
		gint64 status = 0;

		query = venture_query_new(VENTURE_TYPE_WEBHOOK_DELIVERY);
		venture_query_set_limit(query, 1);
		delivery = venture_database_find_one(fixture->database, query, NULL);
		g_assert_nonnull(delivery);
		g_object_get(delivery, "state", &state, "status-code", &status, NULL);
		g_assert_cmpint(state, ==, VENTURE_DELIVERY_STATE_SUCCEEDED);
		g_assert_cmpint(status, ==, 200);
	}
}

/*
 * A webhook asking for the record gets it, without sensitive fields, and
 * one subscribed to something else gets nothing at all.
 */
static void
test_webhook_scope_and_payload(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonNode) body = NULL;
	JsonObject *data;

	(void)user_data;

	webhook = create_webhook(fixture, "invoice.*", NULL);
	ticket = create_ticket(fixture, "Not for you");
	settle(fixture, 1);

	/* Nothing matched, so nothing went out and nothing was recorded. */
	g_assert_cmpuint(fixture->received, ==, 0);
	g_assert_cmpint(count_of(fixture, VENTURE_TYPE_WEBHOOK_DELIVERY), ==, 0);

	g_object_set(webhook, "events", "ticket.created", "include-record", TRUE,
	             NULL);
	g_assert_true(venture_database_save(fixture->database, webhook, NULL,
	                                    NULL));

	{
		g_autoptr(VentureEntity) second = NULL;

		second = create_ticket(fixture, "For you");
		settle(fixture, 1);
	}

	g_assert_cmpuint(fixture->received, ==, 1);
	body = venture_json_parse(fixture->last_body, NULL);
	data = json_object_get_object_member(json_node_get_object(body), "data");
	g_assert_nonnull(data);
	g_assert_cmpstr(venture_json_object_get_string(data, "title", ""), ==,
	                "For you");

	/* Saving the webhook itself must not have fired anything: a webhook
	 * about a webhook is the loop this list exists to prevent. */
	g_assert_cmpuint(fixture->received, ==, 1);
}

/*
 * An endpoint that refuses is recorded as failed, counted, and switched
 * off once it has refused often enough.
 */
static void
test_webhook_failures_switch_it_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) reread = NULL;
	gint64 failures = 0;
	gboolean active = TRUE;
	guint i;

	(void)user_data;

	fixture->answer_with = SOUP_STATUS_INTERNAL_SERVER_ERROR;
	webhook = create_webhook(fixture, "*", NULL);

	for (i = 0; i < VENTURE_WEBHOOK_FAILURE_LIMIT; i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;
		g_autofree gchar *title = NULL;

		title = g_strdup_printf("Attempt %u", i);
		ticket = create_ticket(fixture, title);
		settle(fixture, (gint64)i + 1);
	}

	reread = venture_database_get(fixture->database, VENTURE_TYPE_WEBHOOK,
	                              venture_entity_get_id(webhook), NULL);
	g_object_get(reread, "failure-count", &failures, "active", &active, NULL);

	g_assert_cmpint(failures, ==, VENTURE_WEBHOOK_FAILURE_LIMIT);
	g_assert_false(active);

	/* Switched off, it delivers nothing more. */
	{
		g_autoptr(VentureEntity) ticket = NULL;
		guint was = fixture->received;

		ticket = create_ticket(fixture, "After it gave up");
		settle(fixture, VENTURE_WEBHOOK_FAILURE_LIMIT + 1);
		g_assert_cmpuint(fixture->received, ==, was);
	}
}

/*
 * Test sends a ping and waits, and a fresh secret replaces the old one.
 */
static void
test_webhook_test_and_secret(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) delivery = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *event = NULL;
	VentureDeliveryState state;

	(void)user_data;

	webhook = create_webhook(fixture, "*", NULL);

	secret = venture_webhook_set_secret(fixture->context, webhook, NULL, NULL,
	                                    &error);
	g_assert_no_error(error);
	g_assert_nonnull(secret);
	g_assert_cmpuint(strlen(secret), >, 16);

	again = venture_webhook_set_secret(fixture->context, webhook, NULL, NULL,
	                                   &error);
	g_assert_no_error(error);
	g_assert_cmpstr(again, !=, secret);

	delivery = venture_webhook_test(fixture->context, webhook, &error);
	g_assert_no_error(error);
	g_assert_nonnull(delivery);
	g_object_get(delivery, "state", &state, "event", &event, NULL);
	g_assert_cmpint(state, ==, VENTURE_DELIVERY_STATE_SUCCEEDED);
	g_assert_cmpstr(event, ==, "webhook.test");
	g_assert_cmpstr(fixture->last_event, ==, "webhook.test");

	/* Signed with the newest secret, not the one it replaced. */
	{
		g_autofree gchar *expected = NULL;

		expected = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
			(const guchar *)again, strlen(again),
			(const guchar *)fixture->last_body, strlen(fixture->last_body));
		g_assert_cmpstr(fixture->last_signature + strlen("sha256="), ==,
		                expected);
	}

	/*
	 * A URL nothing can be posted to fails without pretending. Re-read
	 * first: recording the delivery above stamped the webhook's
	 * last-delivery-at, so the copy in hand is a version behind.
	 */
	{
		g_autoptr(VentureEntity) refused = NULL;
		g_autoptr(VentureEntity) fresh = NULL;

		fresh = venture_database_get(fixture->database, VENTURE_TYPE_WEBHOOK,
		                             venture_entity_get_id(webhook), NULL);
		g_object_set(fresh, "url", "not-a-url", NULL);
		g_assert_true(venture_database_save(fixture->database, fresh, NULL,
		                                    NULL));
		refused = venture_webhook_test(fixture->context, fresh, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}
}

/*
 * The module off means nothing goes out, whatever is configured.
 */
static void
test_webhook_module_off_sends_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	webhook = create_webhook(fixture, "*", NULL);
	venture_config_set_module_enabled(fixture->config, "webhooks", FALSE);

	ticket = create_ticket(fixture, "Quiet please");
	settle(fixture, 1);

	g_assert_cmpuint(fixture->received, ==, 0);
	g_assert_null(venture_webhook_describe(fixture->context, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* --- Routing --------------------------------------------------------------- */

static VentureEntity *
create_rule(
	Fixture			*fixture,
	const gchar		*name,
	const gchar		*assignees,
	VentureRoutingStrategy	 strategy,
	const gchar		*tag
){
	VentureRoutingRule *rule;

	rule = venture_routing_rule_new();
	g_object_set(rule, "name", name, "assignees", assignees,
	             "strategy", strategy, "tag", tag, "all-kinds", TRUE,
	             "all-priorities", TRUE, "active", TRUE, NULL);
	file_under_default(fixture, rule);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(rule), NULL, NULL));

	return VENTURE_ENTITY(rule);
}

/*
 * A new ticket with nobody on it lands on somebody, in turn; a ticket
 * raised for a named person keeps that person; and the narrowest rule
 * wins.
 *
 * What breaks if this regresses: a queue nobody is holding, or a rota
 * that gives every ticket to the same person.
 */
static void
test_routing_assigns_new_tickets(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) rota = NULL;
	g_autoptr(VentureEntity) billing = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *third = NULL;
	g_autofree gchar *named = NULL;
	g_autofree gchar *tagged = NULL;

	(void)user_data;

	rota = create_rule(fixture, "Support rota", "alice, bob",
	                   VENTURE_ROUTING_STRATEGY_ROUND_ROBIN, NULL);

	{
		g_autoptr(VentureEntity) one = NULL;
		g_autoptr(VentureEntity) two = NULL;
		g_autoptr(VentureEntity) three = NULL;

		one = create_ticket(fixture, "One");
		two = create_ticket(fixture, "Two");
		three = create_ticket(fixture, "Three");

		g_object_get(one, "assignee", &first, NULL);
		g_object_get(two, "assignee", &second, NULL);
		g_object_get(three, "assignee", &third, NULL);
	}

	g_assert_cmpstr(first, ==, "alice");
	g_assert_cmpstr(second, ==, "bob");
	g_assert_cmpstr(third, ==, "alice");

	/* A ticket raised for somebody keeps them. */
	{
		g_autoptr(VentureTicket) ticket = NULL;

		ticket = venture_ticket_new();
		g_object_set(ticket, "title", "For carol", "assignee", "carol", NULL);
		file_under_default(fixture, ticket);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(ticket), NULL,
		                                    NULL));
		g_object_get(ticket, "assignee", &named, NULL);
	}

	g_assert_cmpstr(named, ==, "carol");

	/* A rule naming a tag is narrower than the catch-all rota. */
	billing = create_rule(fixture, "Billing", "dave",
	                      VENTURE_ROUTING_STRATEGY_FIRST, "billing");

	{
		g_autoptr(VentureTicket) ticket = NULL;

		ticket = venture_ticket_new();
		g_object_set(ticket, "title", "Invoice is wrong", "tags",
		             "urgent, Billing", NULL);
		file_under_default(fixture, ticket);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(ticket), NULL,
		                                    NULL));
		g_object_get(ticket, "assignee", &tagged, NULL);
	}

	g_assert_cmpstr(tagged, ==, "dave");
}

/*
 * Least busy counts open tickets, and a rule naming nobody refuses
 * rather than assigning an empty name.
 */
static void
test_routing_least_busy_and_empty(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(VentureEntity) empty = NULL;
	g_autofree gchar *chosen = NULL;
	g_autofree gchar *nobody = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	(void)user_data;

	/* Alice is holding two; bob one. */
	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureTicket) ticket = NULL;

		ticket = venture_ticket_new();
		g_object_set(ticket, "title", "Existing",
		             "assignee", (i < 2) ? "alice" : "bob",
		             "status", VENTURE_TICKET_STATUS_IN_PROGRESS, NULL);
		file_under_default(fixture, ticket);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(ticket), NULL,
		                                    NULL));
	}

	rule = create_rule(fixture, "Least busy", "alice, bob",
	                   VENTURE_ROUTING_STRATEGY_LEAST_BUSY, NULL);
	chosen = venture_routing_choose(fixture->context, rule, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(chosen, ==, "bob");

	empty = create_rule(fixture, "Nobody", "  ",
	                    VENTURE_ROUTING_STRATEGY_ROUND_ROBIN, NULL);
	nobody = venture_routing_choose(fixture->context, empty, &error);
	g_assert_null(nobody);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* --- Satisfaction and triage ----------------------------------------------- */

/*
 * The support report counts what the desk did, and scores satisfaction
 * over the ratings given rather than over every ticket.
 */
static void
test_support_report_scores_ratings(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	JsonArray *metrics;
	gboolean saw_csat = FALSE;
	guint i;

	(void)user_data;

	{
		static const VentureSatisfaction ratings[] = {
			VENTURE_SATISFACTION_GOOD, VENTURE_SATISFACTION_GOOD,
			VENTURE_SATISFACTION_BAD, VENTURE_SATISFACTION_UNRATED
		};
		guint j;

		for (j = 0; j < G_N_ELEMENTS(ratings); j++)
		{
			g_autoptr(VentureTicket) ticket = NULL;

			ticket = venture_ticket_new();
			g_object_set(ticket, "title", "Rated", "assignee", "alice",
			             "satisfaction", ratings[j],
			             "status", VENTURE_TICKET_STATUS_DONE, NULL);
			file_under_default(fixture, ticket);
			g_assert_true(venture_database_save(fixture->database,
			                                    VENTURE_ENTITY(ticket), NULL,
			                                    NULL));
		}
	}

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "support");
	g_assert_nonnull(report);
	period = venture_date_range_new_all_time();
	result = venture_report_generate(report, fixture->context, period, NULL,
	                                 &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);

	/* One row: every ticket is alice's. */
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);

	node = venture_report_result_to_json(result);
	metrics = json_object_get_array_member(json_node_get_object(node),
	                                       "metrics");

	for (i = 0; i < json_array_get_length(metrics); i++)
	{
		JsonObject *metric;
		const gchar *key;

		metric = json_array_get_object_element(metrics, i);
		key = venture_json_object_get_string(metric, "key", "");

		if (0 == g_strcmp0(key, "csat"))
		{
			/* Two good out of three rated, not out of four raised. */
			g_assert_cmpfloat(json_object_get_double_member(metric, "value"),
			                  >, 0.66);
			g_assert_cmpfloat(json_object_get_double_member(metric, "value"),
			                  <, 0.67);
			saw_csat = TRUE;
		}
		else if (0 == g_strcmp0(key, "rated"))
		{
			g_assert_cmpint(venture_json_object_get_int(metric, "value", 0),
			                ==, 3);
		}
		else if (0 == g_strcmp0(key, "tickets"))
		{
			g_assert_cmpint(venture_json_object_get_int(metric, "value", 0),
			                ==, 4);
		}
	}

	g_assert_true(saw_csat);

	/*
	 * And with nothing rated the score is absent rather than zero: 0%
	 * satisfaction is a claim that everyone who answered was unhappy,
	 * which is not what "nobody has said" means.
	 */
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) all = NULL;
		g_autoptr(VentureReportResult) empty = NULL;
		g_autoptr(JsonNode) node_empty = NULL;
		JsonArray *metrics_empty;
		guint j;

		query = venture_query_new(VENTURE_TYPE_TICKET);
		venture_query_set_limit(query, 0);
		all = venture_database_find(fixture->database, query, NULL);

		for (j = 0; j < all->len; j++)
		{
			g_object_set(g_ptr_array_index(all, j), "satisfaction",
			             VENTURE_SATISFACTION_UNRATED, NULL);
			g_assert_true(venture_database_save(fixture->database,
			                                    g_ptr_array_index(all, j),
			                                    NULL, NULL));
		}

		empty = venture_report_generate(report, fixture->context, period, NULL,
		                                &error);
		g_assert_no_error(error);
		node_empty = venture_report_result_to_json(empty);
		metrics_empty = json_object_get_array_member(
			json_node_get_object(node_empty), "metrics");

		for (j = 0; j < json_array_get_length(metrics_empty); j++)
		{
			g_assert_cmpstr(venture_json_object_get_string(
				json_array_get_object_element(metrics_empty, j), "key", ""),
				!=, "csat");
		}
	}
}

/*
 * Applying a triage takes what it understands and leaves the rest: an
 * invented priority changes nothing, and proposed tags are added to the
 * ones somebody put on by hand rather than replacing them.
 *
 * What breaks if this regresses: a model's guess overwriting a person's
 * classification.
 */
static void
test_triage_applies_only_what_it_understands(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(JsonNode) good = NULL;
	g_autoptr(JsonNode) nonsense = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *tags = NULL;
	VenturePriority priority;
	VentureIssueType issue_type;

	(void)user_data;

	ticket = create_ticket(fixture, "Checkout is broken");
	g_object_set(ticket, "tags", "checkout", NULL);
	g_assert_true(venture_database_save(fixture->database, ticket, NULL, NULL));

	good = venture_json_parse(
		"{\"priority\": \"urgent\", \"issue_type\": \"bug\", "
		"\"tags\": [\"payments\", \"Checkout\"]}", NULL);
	g_assert_true(venture_ai_assist_apply_triage(fixture->context, ticket,
	                                             good, &error));
	g_assert_no_error(error);

	g_object_get(ticket, "priority", &priority, "issue-type", &issue_type,
	             "tags", &tags, NULL);
	g_assert_cmpint(priority, ==, VENTURE_PRIORITY_URGENT);
	g_assert_cmpint(issue_type, ==, VENTURE_ISSUE_TYPE_BUG);

	/* The hand-written tag kept, the new one added, the duplicate not
	 * added twice whatever its case. */
	g_assert_cmpstr(tags, ==, "checkout, payments");

	nonsense = venture_json_parse(
		"{\"priority\": \"catastrophic\", \"issue_type\": \"\"}", NULL);
	g_assert_false(venture_ai_assist_apply_triage(fixture->context, ticket,
	                                              nonsense, &error));

	g_free(tags);
	tags = NULL;
	g_object_get(ticket, "priority", &priority, "tags", &tags, NULL);
	g_assert_cmpint(priority, ==, VENTURE_PRIORITY_URGENT);
	g_assert_cmpstr(tags, ==, "checkout, payments");

	/* Anything that is not an object is refused rather than ignored. */
	g_assert_false(venture_ai_assist_apply_triage(fixture->context, ticket,
	                                              NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/webhook/events-are-named-and-matched",
	                test_webhook_events_are_named_and_matched);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/webhook/delivers-signed", test_webhook_delivers_signed);
	ADD("/webhook/scope-and-payload", test_webhook_scope_and_payload);
	ADD("/webhook/failures-switch-it-off",
	    test_webhook_failures_switch_it_off);
	ADD("/webhook/test-and-secret", test_webhook_test_and_secret);
	ADD("/webhook/module-off-sends-nothing",
	    test_webhook_module_off_sends_nothing);
	ADD("/routing/assigns-new-tickets", test_routing_assigns_new_tickets);
	ADD("/routing/least-busy-and-empty", test_routing_least_busy_and_empty);
	ADD("/support/report-scores-ratings", test_support_report_scores_ratings);
	ADD("/triage/applies-only-what-it-understands",
	    test_triage_applies_only_what_it_understands);

#undef ADD

	return g_test_run();
}
