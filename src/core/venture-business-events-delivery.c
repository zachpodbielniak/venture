/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-business-events-private.h"
#include <libsoup/soup.h>
#include <string.h>

#define DRIVER_KEY "venture-business-events-driver"

typedef struct {
	VentureContext *context; /* Weak: the context owns the timer. */
	GHashTable *inflight;
	guint timer;
} EventDriver;

typedef struct {
	VentureContext *context;
	SoupSession *session;
	SoupMessage *message;
	gint64 delivery_id;
} EventRequest;

static void
driver_free(gpointer data)
{
	EventDriver *driver = data;
	if (driver->timer != 0) g_source_remove(driver->timer);
	g_hash_table_unref(driver->inflight);
	g_free(driver);
}

static void
request_free(EventRequest *request)
{
	EventDriver *driver = g_object_get_data(G_OBJECT(request->context), DRIVER_KEY);
	g_hash_table_remove(driver->inflight, &request->delivery_id);
	g_clear_object(&request->message);
	g_clear_object(&request->session);
	g_clear_object(&request->context);
	g_free(request);
}

static void
sent(GObject *source, GAsyncResult *result, gpointer data)
{
	EventRequest *request = data;
	VentureDatabase *database = venture_context_get_database(request->context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GInputStream) stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	g_autoptr(VentureEntity) delivery = NULL;
	guint status = stream != NULL ? soup_message_get_status(request->message) : 0;
	/* Only the status matters. Never retain or read an unbounded response body. */
	if (stream != NULL) g_input_stream_close(stream, NULL, NULL);
	g_clear_error(&error);
	/* A nested main loop can finish HTTP during an unrelated transaction.
	 * Keep the already-retained retry rather than writing into that operation. */
	if (!venture_database_has_transaction(database))
	{
		delivery = venture_database_get(database, VENTURE_TYPE_BUSINESS_EVENT_DELIVERY, request->delivery_id, &error);
		if (delivery != NULL)
		{
			g_object_set(delivery, "status-code", (gint64)status,
				"delivered", status >= 200 && status < 300, NULL);
			if (!venture_business_events_store(database, delivery, &error))
				g_debug("Business-event delivery result was not retained; retry remains pending");
		}
	}
	request_free(request);
}

static EventDriver *
driver_get(VentureContext *context)
{
	EventDriver *driver = g_object_get_data(G_OBJECT(context), DRIVER_KEY);
	if (driver == NULL)
	{
		driver = g_new0(EventDriver, 1);
		driver->context = context;
		driver->inflight = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
		g_object_set_data_full(G_OBJECT(context), DRIVER_KEY, driver, driver_free);
	}
	return driver;
}

static gboolean
defer_delivery(VentureDatabase *database, VentureEntity *delivery, GDateTime *now, GError **error)
{
	g_autoptr(GDateTime) next = g_date_time_add_seconds(now, 60);
	/* Keep paused obligations without letting them monopolize the due page. */
	g_object_set(delivery, "next-attempt-at", next, NULL);
	return venture_business_events_store(database, delivery, error);
}

static gboolean
start_delivery(VentureContext *context, VentureEntity *delivery, GDateTime *now, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	EventDriver *driver = driver_get(context);
	g_autoptr(VentureEntity) hook = NULL;
	g_autoptr(JsonNode) page = NULL;
	g_autoptr(GError) page_error = NULL;
	g_autoptr(GDateTime) next = NULL;
	g_autofree gchar *url = NULL, *secret = NULL, *body = NULL, *signature = NULL;
	g_autoptr(GUri) uri = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) bytes = NULL;
	gint64 hook_id = 0, event_id = 0, attempts = 0;
	gint64 id = venture_entity_get_id(delivery);
	gboolean active = FALSE, business = FALSE;
	gint format = 0;
	JsonArray *events;
	JsonNode *event;
	EventRequest *request;
	gint64 *key;
	guint delay;
	if (g_hash_table_contains(driver->inflight, &id)) return TRUE;
	g_object_get(delivery, "webhook-id", &hook_id, "event-id", &event_id, "attempts", &attempts, NULL);
	hook = venture_database_get(database, VENTURE_TYPE_WEBHOOK, hook_id, error);
	if (hook == NULL)
		return (error == NULL || *error == NULL) && defer_delivery(database, delivery, now, error);
	g_object_get(hook, "active", &active, "business-events", &business, "format", &format, "url", &url, "secret", &secret, NULL);
	if (!active || !business || venture_entity_is_deleted(hook) || format != VENTURE_WEBHOOK_FORMAT_VENTURE ||
		venture_entity_get_organization_id(hook) != venture_entity_get_organization_id(delivery))
		return defer_delivery(database, delivery, now, error);
	if (!venture_tenant_service_check_resource(venture_tenant_service_get(database), G_OBJECT(hook), TRUE, NULL))
		return defer_delivery(database, delivery, now, error);
	page = venture_business_events_read(database, venture_entity_get_organization_id(delivery), event_id - 1, 1, &page_error);
	if (page == NULL)
	{
		if (g_error_matches(page_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND) ||
			g_error_matches(page_error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED))
			return defer_delivery(database, delivery, now, error);
		g_propagate_error(error, g_steal_pointer(&page_error));
		return FALSE;
	}
	events = json_object_get_array_member(json_node_get_object(page), "events");
	if (json_array_get_length(events) != 1) return defer_delivery(database, delivery, now, error);
	event = json_array_get_element(events, 0);
	if (json_object_get_int_member(json_node_get_object(event), "cursor") != event_id)
		return defer_delivery(database, delivery, now, error);
	body = venture_json_to_string(event, FALSE);
	/* Retain a retry before any transport, including malformed configuration.
	 * Saturation prevents a long-lived refusal from overflowing its counter. */
	delay = 60u << (guint)MIN(MAX(attempts, 0), 6);
	delay = MIN(delay, 3600u);
	next = g_date_time_add_seconds(now, delay);
	g_object_set(delivery, "attempts", attempts < G_MAXINT64 ? attempts + 1 : attempts,
		"next-attempt-at", next, "status-code", (gint64)0, NULL);
	if (!venture_business_events_store(database, delivery, error)) return FALSE;
	uri = url != NULL ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
	if (uri == NULL || g_uri_get_userinfo(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
		(g_strcmp0(g_uri_get_scheme(uri), "https") != 0 && g_strcmp0(g_uri_get_scheme(uri), "http") != 0)) return TRUE;
	message = soup_message_new("POST", url);
	if (message == NULL) return TRUE;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	bytes = g_bytes_new(body, strlen(body));
	soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	soup_message_headers_replace(soup_message_get_request_headers(message), "X-Venture-Delivery", venture_entity_get_uuid(delivery));
	soup_message_headers_replace(soup_message_get_request_headers(message), "X-Venture-Event",
		json_object_get_string_member(json_node_get_object(event), "event"));
	if (!venture_string_is_empty(secret))
	{
		g_autofree gchar *digest = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret), body, -1);
		signature = g_strconcat("sha256=", digest, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(message), "X-Venture-Signature", signature);
	}
	key = g_new(gint64, 1); *key = id;
	g_hash_table_add(driver->inflight, key);
	request = g_new0(EventRequest, 1);
	request->context = g_object_ref(context);
	request->session = soup_session_new();
	soup_session_set_timeout(request->session, 15);
	request->message = g_steal_pointer(&message);
	request->delivery_id = id;
	soup_session_send_async(request->session, request->message, G_PRIORITY_DEFAULT, NULL, sent, request);
	return TRUE;
}

gboolean
venture_business_events_dispatch(VentureContext *context, GDateTime *now, GError **error)
{
	VentureDatabase *database;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) deliveries = NULL;
	g_autoptr(GDateTime) clock = now != NULL ? g_date_time_ref(now) : venture_time_now();
	g_autofree gchar *at = venture_time_to_string(clock);
	guint i;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	database = venture_context_get_database(context);
	if (venture_database_has_transaction(database))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Deliver business events only after commit");
		return FALSE;
	}
	if (!venture_context_module_enabled(context, "webhooks")) return TRUE;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT_DELIVERY);
	venture_query_add_filter_string(query, "delivered", VENTURE_FILTER_OP_EQ, "false", NULL);
	venture_query_add_filter_string(query, "next-attempt-at", VENTURE_FILTER_OP_LTE, at, NULL);
	venture_query_add_order(query, "next-attempt-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 16);
	deliveries = venture_database_find(database, query, error);
	if (deliveries == NULL) return FALSE;
	for (i = 0; i < deliveries->len; i++)
		if (!start_delivery(context, g_ptr_array_index(deliveries, i), clock, error)) return FALSE;
	return TRUE;
}

static gboolean
tick(gpointer data)
{
	EventDriver *driver = data;
	if (!venture_database_has_transaction(venture_context_get_database(driver->context)))
		venture_business_events_dispatch(driver->context, NULL, NULL);
	return G_SOURCE_CONTINUE;
}

void
venture_business_events_install(VentureContext *context)
{
	EventDriver *driver;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	driver = driver_get(context);
	if (driver->timer == 0) driver->timer = g_timeout_add_seconds(5, tick, driver);
}
