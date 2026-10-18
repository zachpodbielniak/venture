/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"

static void
test_lead_atomic_event(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *kind = NULL;
	g_autoptr(JsonNode) uncommitted = NULL;
	GType event_type;
	gint64 organization;

	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	organization = venture_context_get_default_organization_id(context);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization,
		"name", "Example inquiry", "email", "person+inquiry@example.test", NULL);
	g_assert_true(venture_database_begin(database, &error));
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_no_error(error);
	event_type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), "business_event");
	g_assert_cmpuint(event_type, !=, G_TYPE_INVALID);
	query = venture_query_new(event_type);
	rows = venture_database_find(database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_object_get(g_ptr_array_index(rows, 0), "event", &kind, NULL);
	g_assert_cmpstr(kind, ==, "lead.created");
	uncommitted = venture_business_events_read(database, organization, 0, 10, &error);
	g_assert_null(uncommitted);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpint(venture_entity_get_organization_id(g_ptr_array_index(rows, 0)), ==, organization);
	venture_database_rollback(database);
	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = venture_database_find(database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 0);
}

static void
test_outbox_failure_rolls_back(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	gint64 organization;
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	organization = venture_context_get_default_organization_id(context);
	g_assert_true(venture_database_execute(database,
		"CREATE TRIGGER refuse_event BEFORE INSERT ON business_events BEGIN SELECT RAISE(ABORT, 'event store unavailable'); END", NULL, &error));
	g_assert_no_error(error);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Example inquiry", NULL);
	g_assert_true(venture_database_begin(database, &error));
	g_assert_false(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* An ignored child refusal must not let the enclosing operation commit. */
	g_assert_false(venture_database_commit(database, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	saved = venture_database_get(database, VENTURE_TYPE_LEAD, venture_entity_get_id(VENTURE_ENTITY(lead)), &error);
	g_assert_no_error(error);
	g_assert_null(saved);
}

static void
test_replay_restart(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-business-events-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/events.db", directory);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new(uri, &error);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureLead) first = NULL, second = NULL;
	g_autoptr(JsonNode) page = NULL, again = NULL;
	g_autofree gchar *identity = NULL, *wire = NULL, *replayed = NULL;
	JsonArray *events;
	gint64 organization, cursor;
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	organization = venture_context_get_default_organization_id(context);
	first = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Private inquiry", "email", "person+one@example.test", NULL);
	second = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Another inquiry", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(first), NULL, &error));
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(second), NULL, &error));
	g_assert_no_error(error);
	page = venture_business_events_read(database, organization, 0, 1, &error);
	g_assert_no_error(error);
	g_assert_nonnull(page);
	events = json_object_get_array_member(json_node_get_object(page), "events");
	g_assert_cmpuint(json_array_get_length(events), ==, 1);
	identity = g_strdup(json_object_get_string_member(json_array_get_object_element(events, 0), "id"));
	g_assert_true(g_uuid_string_is_valid(identity));
	cursor = json_object_get_int_member(json_node_get_object(page), "next_cursor");
	wire = venture_json_to_string(page, FALSE);
	g_assert_null(strstr(wire, "Private inquiry"));
	g_assert_null(strstr(wire, "person+one"));
	g_clear_object(&context);
	g_clear_object(&database);
	database = venture_database_new(uri, &error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	again = venture_business_events_read(database, organization, 0, 1, &error);
	g_assert_no_error(error);
	replayed = venture_json_to_string(again, FALSE);
	g_assert_cmpstr(wire, ==, replayed);
	g_clear_pointer(&again, json_node_unref);
	again = venture_business_events_read(database, organization, cursor, 1, &error);
	g_assert_no_error(error);
	events = json_object_get_array_member(json_node_get_object(again), "events");
	g_assert_cmpuint(json_array_get_length(events), ==, 1);
	g_assert_cmpstr(identity, !=, json_object_get_string_member(json_array_get_object_element(events, 0), "id"));
	cursor = json_object_get_int_member(json_node_get_object(again), "next_cursor");
	g_clear_pointer(&again, json_node_unref);
	again = venture_business_events_read(database, organization, cursor, 1, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(again), "events")), ==, 0);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(again), "next_cursor"), ==, cursor);
	g_clear_object(&context);
	g_clear_object(&database);
	venture_test_remove_tree(directory);
}


typedef struct {
	guint received;
	guint status;
	gchar *body;
	gchar *delivery;
	const gchar *secret;
} Receiver;

static void
receive_event(SoupServer *server, SoupServerMessage *message, const gchar *path,
	GHashTable *query, gpointer data)
{
	Receiver *receiver = data;
	SoupMessageBody *body = soup_server_message_get_request_body(message);
	const gchar *delivery = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "X-Venture-Delivery");
	const gchar *signature = soup_message_headers_get_one(soup_server_message_get_request_headers(message), "X-Venture-Signature");
	(void)server; (void)path; (void)query;
	receiver->received++;
	if (receiver->body == NULL)
	{
		receiver->body = g_strndup(body->data, (gsize)body->length);
		receiver->delivery = g_strdup(delivery);
	}
	else
	{
		g_assert_cmpstr(receiver->body, ==, body->data);
		g_assert_cmpstr(receiver->delivery, ==, delivery);
	}
	g_assert_true(g_uuid_string_is_valid(delivery));
	if (receiver->secret != NULL)
	{
		const gchar *secret = receiver->secret;
		g_autofree gchar *digest = g_compute_hmac_for_string(G_CHECKSUM_SHA256,
			(const guchar *)secret, strlen(secret), body->data, body->length);
		g_autofree gchar *expected = g_strconcat("sha256=", digest, NULL);
		g_assert_nonnull(signature);
		g_assert_cmpstr(signature, ==, expected);
	}
	soup_server_message_set_status(message, receiver->status, NULL);
}

static void
test_retry_keeps_delivery(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-event-retry-XXXXXX", NULL);
	g_autofree gchar *uri = g_strdup_printf("sqlite://%s/events.db", directory);
	g_autofree gchar *signing_secret = g_uuid_string_random();
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new(uri, &error);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebhook) hook = NULL;
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(SoupServer) endpoint = soup_server_new(NULL, NULL);
	g_autofree gchar *url = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	GType delivery_type;
	GSList *uris;
	Receiver receiver = { 0, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL, NULL, signing_secret };
	guint attempt;
	gint64 organization;
	g_assert_no_error(error);
	context = venture_context_new(config, database);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	organization = venture_context_get_default_organization_id(context);
	soup_server_add_handler(endpoint, "/", receive_event, &receiver, NULL);
	g_assert_true(soup_server_listen_local(endpoint, 0, 0, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(endpoint);
	url = g_uri_to_string(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	hook = g_object_new(VENTURE_TYPE_WEBHOOK, "organization-id", organization,
		"name", "Example receiver", "url", url, "active", TRUE,
		"business-events", TRUE, "events", "lead.created", "secret", signing_secret, NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(hook), NULL, &error));
	/* Disabling transport must retain obligations for existing subscriptions. */
	venture_config_set_module_enabled(config, "webhooks", FALSE);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Private inquiry", NULL);
	g_assert_true(venture_database_begin(database, &error));
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_false(venture_business_events_dispatch(context, now, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpuint(receiver.received, ==, 0);
	g_assert_true(venture_database_commit(database, &error));
	g_assert_no_error(error);
	delivery_type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), "business_event_delivery");
	g_assert_cmpuint(delivery_type, !=, G_TYPE_INVALID);
	venture_config_set_module_enabled(config, "webhooks", FALSE);
	{
		g_autoptr(GDateTime) clock = g_date_time_add_hours(now, 1);
		g_assert_true(venture_business_events_dispatch(context, clock, &error));
		g_assert_no_error(error);
		while (g_main_context_iteration(NULL, FALSE)) { }
		g_assert_cmpuint(receiver.received, ==, 0);
	}
	venture_config_set_module_enabled(config, "webhooks", TRUE);
	for (attempt = 0; attempt < 12; attempt++)
	{
		g_autoptr(GDateTime) clock = g_date_time_add_hours(now, attempt + 1);
		g_autoptr(VentureQuery) query = venture_query_new(delivery_type);
		g_autoptr(VentureEntity) delivery = NULL;
		g_autoptr(VentureEntity) current_hook = NULL;
		gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
		gint64 status = 0;
		gboolean delivered = FALSE, active = FALSE;
		if (attempt == 11)
		{
			g_clear_object(&context); g_clear_object(&database);
			database = venture_database_new(uri, &error);
			g_assert_no_error(error);
			context = venture_context_new(config, database);
		}
		receiver.status = attempt == 11 ? SOUP_STATUS_OK : SOUP_STATUS_SERVICE_UNAVAILABLE;
		g_assert_true(venture_business_events_dispatch(context, clock, &error));
		g_assert_no_error(error);
		do
		{
			while (g_main_context_iteration(NULL, FALSE)) { }
			g_clear_object(&delivery);
			delivery = venture_database_find_one(database, query, &error);
			g_assert_no_error(error);
			g_assert_nonnull(delivery);
			g_object_get(delivery, "status-code", &status, NULL);
			if (receiver.received > attempt && status == (gint64)receiver.status) break;
			g_usleep(1000);
		} while (g_get_monotonic_time() < deadline);
		g_assert_cmpuint(receiver.received, ==, attempt + 1);
		g_assert_cmpint(status, ==, receiver.status);
		g_object_get(delivery, "delivered", &delivered, NULL);
		g_assert_cmpint(delivered, ==, attempt == 11);
		current_hook = venture_database_get(database, VENTURE_TYPE_WEBHOOK, venture_entity_get_id(VENTURE_ENTITY(hook)), &error);
		g_assert_no_error(error);
		g_object_get(current_hook, "active", &active, NULL);
		g_assert_true(active);
		/* Repeating the same clock must respect backoff, or accepted state. */
		g_assert_true(venture_business_events_dispatch(context, clock, &error));
		g_assert_no_error(error);
		while (g_main_context_iteration(NULL, FALSE)) { }
		g_assert_cmpuint(receiver.received, ==, attempt + 1);
	}
	g_assert_null(strstr(receiver.body, "Private inquiry"));
	g_free(receiver.body);
	g_free(receiver.delivery);
	soup_server_disconnect(endpoint);
	g_clear_object(&context); g_clear_object(&database);
	venture_test_remove_tree(directory);
}


typedef struct { gboolean done; GBytes *body; GError *error; } HttpReply;
static void
http_received(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpReply *reply = data;
	reply->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static guint
http_get(SoupSession *session, VentureWebServer *server, const gchar *path, gchar **body)
{
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	HttpReply reply = { FALSE, NULL, NULL };
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_received, &reply);
	while (!reply.done && g_get_monotonic_time() < deadline)
	{
		while (g_main_context_iteration(NULL, FALSE)) { }
		if (!reply.done) g_usleep(1000);
	}
	g_assert_true(reply.done);
	g_assert_no_error(reply.error);
	if (body != NULL) *body = g_strndup(g_bytes_get_data(reply.body, NULL), g_bytes_get_size(reply.body));
	g_bytes_unref(reply.body);
	return soup_message_get_status(message);
}

static void
test_http_replay(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("venture-events-http-XXXXXX", NULL);
	g_autoptr(GInetAddress) address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autofree gchar *bind = g_inet_address_to_string(address);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(SoupSession) session = soup_session_new();
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(JsonNode) page = NULL;
	g_autofree gchar *body = NULL, *path = NULL;
	gint64 organization, cursor;
	g_object_set(config, "state-dir", directory, "server-bind-address", bind,
		"server-port", (gint64)0, "security-require-auth", FALSE, NULL);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, database);
	organization = venture_context_get_default_organization_id(context);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Private inquiry", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	path = g_strdup_printf("/api/v1/business-events?organization_id=%" G_GINT64_FORMAT "&after=0&limit=1", organization);
	g_assert_cmpuint(http_get(session, server, path, &body), ==, SOUP_STATUS_OK);
	page = venture_json_parse(body, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(page), "events")), ==, 1);
	cursor = json_object_get_int_member(json_node_get_object(page), "next_cursor");
	g_clear_pointer(&path, g_free); g_clear_pointer(&body, g_free); g_clear_pointer(&page, json_node_unref);
	path = g_strdup_printf("/api/v1/business-events?organization_id=%" G_GINT64_FORMAT "&after=%" G_GINT64_FORMAT, organization, cursor);
	g_assert_cmpuint(http_get(session, server, path, &body), ==, SOUP_STATUS_OK);
	page = venture_json_parse(body, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(page), "events")), ==, 0);
	g_assert_cmpuint(http_get(session, server, "/api/v1/business-events?after=-1", NULL), ==, SOUP_STATUS_BAD_REQUEST);
	g_assert_cmpuint(http_get(session, server, "/api/v1/business-events?limit=0", NULL), ==, SOUP_STATUS_BAD_REQUEST);
	g_object_set(config, "security-require-auth", TRUE, NULL);
	venture_web_server_stop(server);
	g_clear_object(&server);
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(http_get(session, server, path, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	venture_web_server_stop(server);
	g_clear_object(&server); g_clear_object(&context); g_clear_object(&database);
	venture_test_remove_tree(directory);
}


static void
test_organization_access(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) other = NULL, member = NULL, user = NULL, lead = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonNode) page = NULL;
	VentureAuthPrincipal principal;
	gint64 organization;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, database);
	organization = venture_context_get_default_organization_id(context);
	other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other organization", "slug", "other", "active", TRUE, NULL);
	user = g_object_new(VENTURE_TYPE_USER, "username", "event-reader", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Example inquiry", NULL);
	g_assert_true(venture_database_save(database, other, NULL, &error));
	g_assert_true(venture_database_save(database, user, NULL, &error));
	g_assert_true(venture_database_save(database, lead, NULL, &error));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", organization,
		"user-id", venture_entity_get_id(user), "role", VENTURE_ORGANIZATION_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_database_save(database, member, NULL, &error));
	g_assert_no_error(error);
	principal.authenticated = TRUE;
	principal.user_id = venture_entity_get_id(user);
	principal.token_id = 0;
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.name = NULL;
	principal.provider_token = FALSE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), &principal);
	page = venture_business_events_read(database, organization, 0, 10, &error);
	g_assert_no_error(error);
	g_assert_nonnull(page);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(page), "events")), ==, 1);
	g_clear_pointer(&page, json_node_unref);
	page = venture_business_events_read(database, venture_entity_get_id(other), 0, 10, &error);
	g_assert_null(page);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_object(&scope);
	g_object_set(member, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, NULL);
	g_assert_true(venture_database_save(database, member, NULL, &error));
	g_assert_no_error(error);
	scope = venture_access_policy_enter(venture_database_get_access_policy(database), &principal);
	page = venture_business_events_read(database, organization, 0, 10, &error);
	g_assert_null(page);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_clear_object(&scope);
}


static void
test_paused_hooks_do_not_starve(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", &error);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GPtrArray) hooks = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureLead) lead = NULL;
	g_autoptr(SoupServer) endpoint = soup_server_new(NULL, NULL);
	g_autoptr(GDateTime) now = venture_time_now(), clock = g_date_time_add_hours(now, 1);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT_DELIVERY);
	g_autofree gchar *url = NULL;
	Receiver receiver = { 0, SOUP_STATUS_OK, NULL, NULL, NULL };
	GSList *uris;
	gint64 organization, deadline;
	guint i;
	gboolean delivered = FALSE;
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, database);
	organization = venture_context_get_default_organization_id(context);
	soup_server_add_handler(endpoint, "/", receive_event, &receiver, NULL);
	g_assert_true(soup_server_listen_local(endpoint, 0, 0, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(endpoint); url = g_uri_to_string(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	for (i = 0; i < 17; i++)
	{
		VentureWebhook *hook = g_object_new(VENTURE_TYPE_WEBHOOK, "organization-id", organization,
			"name", "Example receiver", "url", url, "active", TRUE, "business-events", TRUE, "events", "lead.created", NULL);
		g_assert_true(venture_database_save(database, VENTURE_ENTITY(hook), NULL, &error));
		g_assert_no_error(error); g_ptr_array_add(hooks, hook);
	}
	lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", organization, "name", "Example inquiry", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(lead), NULL, &error));
	g_assert_no_error(error);
	for (i = 0; i < 16; i++)
	{
		VentureEntity *hook = g_ptr_array_index(hooks, i);
		g_object_set(hook, "active", FALSE, NULL);
		g_assert_true(venture_database_save(database, hook, NULL, &error));
		g_assert_no_error(error);
	}
	g_assert_cmpint(venture_database_count(database, query, &error), ==, 17);
	g_assert_no_error(error);
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE business_event_deliveries SET next_attempt_at = CASE WHEN webhook_id = %" G_GINT64_FORMAT " THEN '2001-01-02T00:00:00Z' ELSE '2001-01-01T00:00:00Z' END",
			venture_entity_get_id(g_ptr_array_index(hooks, 16)));
		g_assert_true(venture_database_execute(database, sql, NULL, &error));
		g_assert_no_error(error);
	}
	venture_query_add_filter_int(query, "webhook-id", VENTURE_FILTER_OP_EQ,
		venture_entity_get_id(g_ptr_array_index(hooks, 16)), NULL);
	g_assert_true(venture_business_events_dispatch(context, clock, &error));
	g_assert_true(venture_business_events_dispatch(context, clock, &error));
	g_assert_no_error(error);
	deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	do
	{
		g_autoptr(VentureEntity) delivery = NULL;
		while (g_main_context_iteration(NULL, FALSE)) { }
		delivery = venture_database_find_one(database, query, &error);
		g_assert_no_error(error); g_assert_nonnull(delivery);
		g_object_get(delivery, "delivered", &delivered, NULL);
		if (delivered) break;
		g_usleep(1000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_true(delivered);
	g_assert_cmpuint(receiver.received, ==, 1);
	g_free(receiver.body); g_free(receiver.delivery);
	soup_server_disconnect(endpoint);
}


int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/business-events/lead-atomic", test_lead_atomic_event);
	g_test_add_func("/business-events/outbox-failure", test_outbox_failure_rolls_back);
	g_test_add_func("/business-events/replay-restart", test_replay_restart);
	g_test_add_func("/business-events/retry-keeps-delivery", test_retry_keeps_delivery);
	g_test_add_func("/business-events/http-replay", test_http_replay);
	g_test_add_func("/business-events/organization-access", test_organization_access);
	g_test_add_func("/business-events/paused-hooks", test_paused_hooks_do_not_starve);
	return g_test_run();
}
