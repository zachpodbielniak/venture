/*
 * venture-webhook.c - Telling something outside that a record changed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <libsoup/soup.h>
#include <string.h>

/* How much of an answer is worth keeping: enough to read an error page's
 * first line, not enough to store somebody's whole HTML. */
#define VENTURE_WEBHOOK_EXCERPT (600)

/* A delivery that has not answered in this long is a delivery that is not
 * coming. Short, because these fire on the write path's heels and a slow
 * endpoint should not pile requests up behind it. */
#define VENTURE_WEBHOOK_TIMEOUT (15)

/*
 * The types no webhook hears about.
 *
 * A delivery record about a delivery would fire a webhook about a
 * webhook, which is the obvious loop; the rest are bookkeeping whose
 * changes say nothing an outside system could act on, and two of them
 * -- chat and the audit log -- are things nobody outside should be
 * handed wholesale.
 */
static gboolean
venture_webhook_type_is_quiet(const gchar *type_name)
{
	static const gchar *const quiet[] = {
		"webhook", "webhook_delivery", "notification", "watch",
		"audit_entry", "chat_thread", "chat_message", "kb_chunk",
		"kb_link", "federation_replica", "federation_peer", "federation_grant", "api_token", "user", NULL
	};
	gsize i;

	if (NULL == type_name)
		return TRUE;

	for (i = 0; NULL != quiet[i]; i++)
	{
		if (0 == g_strcmp0(quiet[i], type_name))
			return TRUE;
	}

	return FALSE;
}

gchar *
venture_webhook_event_name(
	const gchar		*target_type,
	VentureAuditAction	 action
){
	const gchar *verb;

	if (venture_string_is_empty(target_type))
		return NULL;

	switch (action)
	{
	case VENTURE_AUDIT_ACTION_CREATE: verb = "created"; break;
	case VENTURE_AUDIT_ACTION_UPDATE: verb = "updated"; break;
	case VENTURE_AUDIT_ACTION_DELETE: verb = "deleted"; break;
	default:                          return NULL;
	}

	return g_strdup_printf("%s.%s", target_type, verb);
}

gboolean
venture_webhook_matches(
	const gchar	*events,
	const gchar	*event
){
	g_auto(GStrv) patterns = NULL;
	gsize i;

	if (venture_string_is_empty(event))
		return FALSE;

	/* Nothing named is a webhook nobody has narrowed yet. */
	if (venture_string_is_empty(events))
		return TRUE;

	patterns = g_strsplit(events, ",", -1);

	for (i = 0; NULL != patterns[i]; i++)
	{
		g_autofree gchar *pattern = NULL;

		pattern = g_strstrip(g_strdup(patterns[i]));

		if (venture_string_is_empty(pattern))
			continue;

		if (0 == g_strcmp0(pattern, "*"))
			return TRUE;

		if (0 == g_ascii_strcasecmp(pattern, event))
			return TRUE;

		/* "ticket.*" wants every action on a ticket. */
		if (g_str_has_suffix(pattern, ".*"))
		{
			g_autofree gchar *prefix = NULL;

			prefix = g_strndup(pattern, strlen(pattern) - 1);

			if (g_str_has_prefix(event, prefix))
				return TRUE;
		}
	}

	return FALSE;
}

/* --- The body -------------------------------------------------------------- */

/*
 * What goes over the wire. The envelope is always the same shape -- the
 * event, a delivery id the far end can deduplicate on, when, which
 * webhook, and which record -- so a receiver can route on it without
 * knowing VENTURE's types. The record itself rides along only when the
 * webhook asks for it, because a body is a copy of business data leaving
 * the building and that should be a decision rather than a default.
 */
static gchar *
venture_webhook_build_body(
	VentureContext	*context,
	VentureEntity	*webhook,
	const gchar	*event,
	const gchar	*delivery_id,
	const gchar	*target_type,
	gint64		 target_id,
	const gchar	*target_label,
	const gchar	*actor
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *when = NULL;
	g_autofree gchar *name = NULL;
	gboolean include = FALSE;

	g_object_get(webhook, "name", &name, "include-record", &include, NULL);
	now = venture_time_now();
	when = venture_time_to_string(now);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "event");
	json_builder_add_string_value(builder, event);
	json_builder_set_member_name(builder, "delivery");
	json_builder_add_string_value(builder, delivery_id);
	json_builder_set_member_name(builder, "occurred_at");
	json_builder_add_string_value(builder, when);
	json_builder_set_member_name(builder, "actor");

	if (!venture_string_is_empty(actor))
		json_builder_add_string_value(builder, actor);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "webhook");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, venture_entity_get_id(webhook));
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_end_object(builder);

	json_builder_set_member_name(builder, "record");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, target_type);
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, target_id);
	json_builder_set_member_name(builder, "label");

	if (!venture_string_is_empty(target_label))
		json_builder_add_string_value(builder, target_label);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "url");

	if (!venture_string_is_empty(target_type) && (0 != target_id))
	{
		g_autofree gchar *url = NULL;

		url = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, target_type,
		                      target_id);
		json_builder_add_string_value(builder, url);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_end_object(builder);

	json_builder_set_member_name(builder, "data");

	if (include && !venture_string_is_empty(target_type) && (0 != target_id))
	{
		g_autoptr(VentureEntity) record = NULL;
		GType entity_type;

		entity_type = venture_entity_registry_lookup_any(
			venture_context_get_entity_registry(context), target_type);

		if (G_TYPE_INVALID != entity_type)
			record = venture_database_get(
				venture_context_get_database(context), entity_type, target_id,
				NULL);

		/*
		 * Never with sensitive fields. The flag exists so a password
		 * hash and a forge token stay out of a response; a body posted
		 * to somebody else's server is the last place to make an
		 * exception.
		 */
		if (NULL != record)
			json_builder_add_value(builder,
				venture_serializable_to_json(VENTURE_SERIALIZABLE(record),
				                             FALSE));
		else
			json_builder_add_null_value(builder);
	}
	else
	{
		json_builder_add_null_value(builder);
	}

	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_json_to_string(node, TRUE);
}

/* --- Sending --------------------------------------------------------------- */

typedef struct
{
	VentureContext	*context;
	SoupSession	*session;
	SoupMessage	*message;
	gint64		 webhook_id;
	gchar		*event;
	gchar		*delivery_id;
	gchar		*target_type;
	gint64		 target_id;
	gchar		*target_label;
	gchar		*body;
	gint64		 started;
} VentureWebhookJob;

static void
venture_webhook_job_free(VentureWebhookJob *job)
{
	if (NULL == job)
		return;

	g_clear_object(&job->context);
	g_clear_object(&job->session);
	g_clear_object(&job->message);
	g_free(job->event);
	g_free(job->delivery_id);
	g_free(job->target_type);
	g_free(job->target_label);
	g_free(job->body);
	g_free(job);
}

/*
 * Writes what happened, and moves the webhook's failure count. A tenth
 * consecutive failure switches the webhook off and says so in the
 * delivery, which is the only place anybody will look.
 */
static VentureEntity *
venture_webhook_record_delivery(
	VentureContext	*context,
	gint64		 webhook_id,
	const gchar	*event,
	const gchar	*target_type,
	gint64		 target_id,
	const gchar	*target_label,
	const gchar	*body,
	guint		 status,
	const gchar	*response,
	const gchar	*failure,
	gint64		 duration_ms
){
	VentureDatabase *database;
	g_autoptr(VentureWebhookDelivery) delivery = NULL;
	g_autoptr(VentureEntity) webhook = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *excerpt = NULL;
	VentureActor actor;
	gboolean succeeded;

	database = venture_context_get_database(context);
	succeeded = (status >= 200) && (status < 300);
	now = venture_time_now();
	excerpt = venture_truncate(response, VENTURE_WEBHOOK_EXCERPT);

	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "webhook";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;

	delivery = venture_webhook_delivery_new();
	g_object_set(delivery,
	             "webhook-id", webhook_id,
	             "event", event,
	             "state", succeeded ? VENTURE_DELIVERY_STATE_SUCCEEDED
	                                : VENTURE_DELIVERY_STATE_FAILED,
	             "target-type", target_type,
	             "target-id", target_id,
	             "target-label", target_label,
	             "status-code", (gint64)status,
	             "duration-ms", duration_ms,
	             "attempted-at", now,
	             "request-body", body,
	             "response-excerpt", excerpt,
	             "failure-reason", failure,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(delivery),
		venture_context_get_default_organization_id(context));

	if (!venture_database_save(database, VENTURE_ENTITY(delivery), &actor,
	                           NULL))
		return NULL;

	webhook = venture_database_get(database, VENTURE_TYPE_WEBHOOK, webhook_id,
	                               NULL);

	if (NULL != webhook)
	{
		gint64 failures = 0;

		g_object_get(webhook, "failure-count", &failures, NULL);
		failures = succeeded ? 0 : failures + 1;
		g_object_set(webhook, "failure-count", failures,
		             "last-delivery-at", now, NULL);

		if (failures >= VENTURE_WEBHOOK_FAILURE_LIMIT)
		{
			g_autofree gchar *name = NULL;
			g_autofree gchar *title = NULL;

			g_object_get(webhook, "name", &name, NULL);
			g_object_set(webhook, "active", FALSE, NULL);
			title = g_strdup_printf("Webhook switched off: %s", name);
			venture_notify_broadcast(context, VENTURE_USER_ROLE_ADMIN,
				VENTURE_NOTIFICATION_KIND_SYSTEM, title,
				"Ten deliveries in a row failed. Fix the endpoint and "
				"switch it back on.", "webhook", webhook_id, name, NULL,
				NULL);
		}

		venture_database_save(database, webhook, &actor, NULL);
	}

	return VENTURE_ENTITY(g_steal_pointer(&delivery));
}

static void
venture_webhook_sent(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	VentureWebhookJob *job = user_data;
	g_autoptr(GBytes) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	gint64 duration;
	guint status = 0;

	response = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
	                                             &error);
	duration = (g_get_monotonic_time() - job->started) / 1000;

	if (NULL != response)
	{
		gsize length = 0;
		const gchar *data;

		status = soup_message_get_status(job->message);
		data = g_bytes_get_data(response, &length);

		/* The answer is somebody else's bytes: it may be binary, and it
		 * may not be terminated. Copy the first stretch and let the
		 * truncation in the recorder decide how much to keep. */
		if ((NULL != data) && (length > 0))
			text = g_strndup(data, MIN(length,
			                           (gsize)VENTURE_WEBHOOK_EXCERPT * 2));
	}

	venture_webhook_record_delivery(job->context, job->webhook_id, job->event,
		job->target_type, job->target_id, job->target_label, job->body,
		status, text,
		(NULL != error) ? error->message : NULL, duration);

	venture_webhook_job_free(job);
}

/*
 * Signs and posts. The signature is an HMAC-SHA256 over the exact bytes
 * of the body, hex, prefixed `sha256=` -- the same shape VENTURE
 * verifies on the way in, so an operator who has wired one up already
 * knows how to check the other.
 */
static void
venture_webhook_send(
	VentureContext	*context,
	VentureEntity	*webhook,
	const gchar	*event,
	const gchar	*target_type,
	gint64		 target_id,
	const gchar	*target_label,
	const gchar	*actor
){
	VentureWebhookJob *job;
	g_autofree gchar *url = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *signature = NULL;
	g_autoptr(GBytes) bytes = NULL;
	SoupMessageHeaders *headers;

	g_object_get(webhook, "url", &url, "secret", &secret, NULL);

	if (venture_string_is_empty(url))
		return;

	job = g_new0(VentureWebhookJob, 1);
	job->context = g_object_ref(context);
	job->webhook_id = venture_entity_get_id(webhook);
	job->event = g_strdup(event);
	job->delivery_id = g_uuid_string_random();
	job->target_type = g_strdup(target_type);
	job->target_id = target_id;
	job->target_label = g_strdup(target_label);
	job->body = venture_webhook_build_body(context, webhook, event,
	                                       job->delivery_id, target_type,
	                                       target_id, target_label, actor);

	job->message = soup_message_new(SOUP_METHOD_POST, url);

	if (NULL == job->message)
	{
		venture_webhook_record_delivery(context, job->webhook_id, event,
			target_type, target_id, target_label, job->body, 0, NULL,
			"That is not a URL anything can be posted to", 0);
		venture_webhook_job_free(job);
		return;
	}

	bytes = g_bytes_new(job->body, strlen(job->body));
	soup_message_set_request_body_from_bytes(job->message, "application/json",
	                                         bytes);

	headers = soup_message_get_request_headers(job->message);
	soup_message_headers_append(headers, "X-Venture-Event", event);
	soup_message_headers_append(headers, "X-Venture-Delivery",
	                            job->delivery_id);

	if (!venture_string_is_empty(secret))
	{
		g_autofree gchar *hex = NULL;

		hex = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
		                              (const guchar *)secret, strlen(secret),
		                              (const guchar *)job->body,
		                              strlen(job->body));
		signature = g_strconcat("sha256=", hex, NULL);
		soup_message_headers_append(headers, "X-Venture-Signature", signature);
	}

	job->session = soup_session_new();
	soup_session_set_timeout(job->session, VENTURE_WEBHOOK_TIMEOUT);
	soup_session_set_user_agent(job->session, "VENTURE/" VENTURE_VERSION_S " ");
	job->started = g_get_monotonic_time();

	/*
	 * Asynchronous, on the default main context. This runs from inside a
	 * database write, and a blocking POST there would make every save in
	 * the program wait on somebody else's server.
	 */
	soup_session_send_and_read_async(job->session, job->message,
	                                 G_PRIORITY_DEFAULT, NULL,
	                                 venture_webhook_sent, job);
}

/* --- The hook -------------------------------------------------------------- */

static GPtrArray *
venture_webhook_active(VentureContext *context)
{
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_WEBHOOK);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ,
	                                "true", NULL);
	venture_query_set_limit(query, 0);

	return venture_database_find(venture_context_get_database(context), query,
	                             NULL);
}

static void
venture_webhook_on_audit(
	VentureDatabase	*database,
	VentureEntity	*entry,
	gpointer	 user_data
){
	VentureContext *context;
	g_autoptr(GPtrArray) webhooks = NULL;
	g_autofree gchar *target_type = NULL;
	g_autofree gchar *target_label = NULL;
	g_autofree gchar *actor = NULL;
	g_autofree gchar *event = NULL;
	VentureAuditAction action;
	gint64 target_id = 0;
	guint i;

	(void)database;
	context = user_data;

	if (!venture_context_module_enabled(context, "webhooks"))
		return;

	g_object_get(entry, "action", &action, "actor", &actor,
	             "target-type", &target_type, "target-id", &target_id,
	             "target-label", &target_label, NULL);

	if (venture_webhook_type_is_quiet(target_type) || (0 == target_id))
		return;

	event = venture_webhook_event_name(target_type, action);

	if (NULL == event)
		return;

	webhooks = venture_webhook_active(context);

	for (i = 0; (NULL != webhooks) && (i < webhooks->len); i++)
	{
		VentureEntity *webhook;
		g_autofree gchar *events = NULL;

		webhook = g_ptr_array_index(webhooks, i);
		g_object_get(webhook, "events", &events, NULL);

		if (!venture_webhook_matches(events, event))
			continue;

		venture_webhook_send(context, webhook, event, target_type, target_id,
		                     target_label, actor);
	}
}

void
venture_webhook_install(VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	/* No reference, for the same reason the validators take none: the
	 * context holds the database, so the database cannot outlive it. */
	g_signal_connect(venture_context_get_database(context), "audit",
	                 G_CALLBACK(venture_webhook_on_audit), context);
}

/* --- Operator actions ------------------------------------------------------ */

gchar *
venture_webhook_set_secret(
	VentureContext		 *context,
	VentureEntity		 *webhook,
	const gchar		 *secret,
	const VentureActor	 *actor,
	GError			**error
){
	g_autofree gchar *generated = NULL;
	g_autoptr(GDateTime) now = NULL;
	const gchar *value;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_WEBHOOK(webhook), NULL);

	if (venture_string_is_empty(secret))
	{
		generated = venture_generate_token(32);
		value = generated;
	}
	else
	{
		value = secret;
	}

	now = venture_time_now();
	g_object_set(webhook, "secret", value, "secret-set-at", now, NULL);

	if (!venture_database_save(venture_context_get_database(context), webhook,
	                           actor, error))
		return NULL;

	return g_strdup(value);
}

typedef struct
{
	GMainLoop	*loop;
	GBytes		*response;
	GError		*error;
	gboolean	 finished;
} VentureWebhookWait;

static void
venture_webhook_test_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	VentureWebhookWait *wait = user_data;

	wait->response = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                   result, &wait->error);
	wait->finished = TRUE;

	if (g_main_loop_is_running(wait->loop))
		g_main_loop_quit(wait->loop);
}

static gboolean
venture_webhook_test_timeout(gpointer user_data)
{
	VentureWebhookWait *wait = user_data;

	if (!wait->finished && g_main_loop_is_running(wait->loop))
		g_main_loop_quit(wait->loop);

	return G_SOURCE_REMOVE;
}

VentureEntity *
venture_webhook_test(
	VentureContext	 *context,
	VentureEntity	 *webhook,
	GError		**error
){
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *delivery_id = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *label = NULL;
	g_autoptr(GBytes) bytes = NULL;
	VentureWebhookWait wait;
	SoupMessageHeaders *headers;
	VentureEntity *delivery;
	gint64 started;
	guint status = 0;
	guint timeout_id;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_WEBHOOK(webhook), NULL);

	g_object_get(webhook, "url", &url, "secret", &secret, NULL);

	if (venture_string_is_empty(url))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "That webhook names no URL");
		return NULL;
	}

	label = venture_entity_get_display_name(webhook);
	delivery_id = g_uuid_string_random();
	body = venture_webhook_build_body(context, webhook, "webhook.test",
	                                  delivery_id, "webhook",
	                                  venture_entity_get_id(webhook), label,
	                                  NULL);

	message = soup_message_new(SOUP_METHOD_POST, url);

	if (NULL == message)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "\"%s\" is not a URL anything can be posted to", url);
		return NULL;
	}

	bytes = g_bytes_new(body, strlen(body));
	soup_message_set_request_body_from_bytes(message, "application/json",
	                                         bytes);
	headers = soup_message_get_request_headers(message);
	soup_message_headers_append(headers, "X-Venture-Event", "webhook.test");
	soup_message_headers_append(headers, "X-Venture-Delivery", delivery_id);

	if (!venture_string_is_empty(secret))
	{
		g_autofree gchar *hex = NULL;
		g_autofree gchar *signature = NULL;

		hex = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
		                              (const guchar *)secret, strlen(secret),
		                              (const guchar *)body, strlen(body));
		signature = g_strconcat("sha256=", hex, NULL);
		soup_message_headers_append(headers, "X-Venture-Signature", signature);
	}

	session = soup_session_new();
	soup_session_set_timeout(session, VENTURE_WEBHOOK_TIMEOUT);
	soup_session_set_user_agent(session, "VENTURE/" VENTURE_VERSION_S " ");
	started = g_get_monotonic_time();

	/*
	 * Sent asynchronously and waited for in a nested main loop, rather
	 * than with the blocking call this obviously wants to be.
	 *
	 * Somebody pressed Test and is waiting to be told whether the far
	 * end is there, so the call has to have an answer before it
	 * returns -- but blocking the main loop to get one stops every
	 * source in the process, including whatever is serving the far end
	 * when the far end happens to be this program. That is not a
	 * hypothetical: it is exactly the case an operator tries first,
	 * pointing a webhook at their own install to see what the body
	 * looks like, and it deadlocked until the timeout.
	 *
	 * This is the ordinary GLib idiom for a synchronous wrapper around
	 * an asynchronous call, and it is safe here for the reason the
	 * nested loops in the automation engine are not: this one runs on
	 * the main thread with no handler of ours on a worker underneath
	 * it.
	 */
	loop = g_main_loop_new(NULL, FALSE);
	wait.loop = loop;
	wait.response = NULL;
	wait.error = NULL;
	wait.finished = FALSE;

	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, venture_webhook_test_done, &wait);

	/* A guard, in case the session's own timeout never fires. */
	timeout_id = g_timeout_add_seconds(VENTURE_WEBHOOK_TIMEOUT + 5,
	                                   venture_webhook_test_timeout, &wait);
	g_main_loop_run(loop);
	g_source_remove(timeout_id);

	if (NULL != wait.response)
	{
		gsize length = 0;
		const gchar *data;

		status = soup_message_get_status(message);
		data = g_bytes_get_data(wait.response, &length);

		if ((NULL != data) && (length > 0))
			text = g_strndup(data, MIN(length,
			                           (gsize)VENTURE_WEBHOOK_EXCERPT * 2));
	}

	delivery = venture_webhook_record_delivery(context,
		venture_entity_get_id(webhook), "webhook.test", "webhook",
		venture_entity_get_id(webhook), label, body, status, text,
		(NULL != wait.error) ? wait.error->message
		                     : (wait.finished ? NULL : "It did not answer"),
		(g_get_monotonic_time() - started) / 1000);

	g_clear_pointer(&wait.response, g_bytes_unref);
	g_clear_error(&wait.error);

	return delivery;
}

JsonNode *
venture_webhook_describe(
	VentureContext	 *context,
	GError		**error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) webhooks = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	if (!venture_context_module_enabled(context, "webhooks"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "The webhooks module is disabled on this install "
		                    "(modules.webhooks.enabled)");
		return NULL;
	}

	query = venture_query_new(VENTURE_TYPE_WEBHOOK);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	webhooks = venture_database_find(venture_context_get_database(context),
	                                 query, error);

	if (NULL == webhooks)
		return NULL;

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < webhooks->len; i++)
	{
		VentureEntity *webhook;
		g_autoptr(VentureQuery) deliveries_query = NULL;
		g_autoptr(GPtrArray) deliveries = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *url = NULL;
		g_autofree gchar *events = NULL;
		g_autofree gchar *secret = NULL;
		g_autoptr(GDateTime) last = NULL;
		gint64 failures = 0;
		gboolean active = FALSE;
		gboolean include = FALSE;
		guint j;

		webhook = g_ptr_array_index(webhooks, i);
		g_object_get(webhook, "name", &name, "url", &url, "events", &events,
		             "secret", &secret, "active", &active,
		             "include-record", &include, "failure-count", &failures,
		             "last-delivery-at", &last, NULL);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, venture_entity_get_id(webhook));
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, name);
		json_builder_set_member_name(builder, "url");
		json_builder_add_string_value(builder, url);
		json_builder_set_member_name(builder, "events");
		json_builder_add_string_value(builder,
			venture_string_is_empty(events) ? "*" : events);
		json_builder_set_member_name(builder, "active");
		json_builder_add_boolean_value(builder, active);
		json_builder_set_member_name(builder, "include_record");
		json_builder_add_boolean_value(builder, include);
		json_builder_set_member_name(builder, "failure_count");
		json_builder_add_int_value(builder, failures);

		/* Whether it is signed, never the secret itself. */
		json_builder_set_member_name(builder, "signed");
		json_builder_add_boolean_value(builder,
		                               !venture_string_is_empty(secret));

		json_builder_set_member_name(builder, "last_delivery_at");

		if (NULL != last)
		{
			g_autofree gchar *when = NULL;

			when = venture_time_to_string(last);
			json_builder_add_string_value(builder, when);
		}
		else
		{
			json_builder_add_null_value(builder);
		}

		deliveries_query = venture_query_new(VENTURE_TYPE_WEBHOOK_DELIVERY);
		venture_query_add_filter_int(deliveries_query, "webhook-id",
		                             VENTURE_FILTER_OP_EQ,
		                             venture_entity_get_id(webhook), NULL);
		venture_query_add_order(deliveries_query, "id", VENTURE_SORT_DESCENDING,
		                        NULL);
		venture_query_set_limit(deliveries_query, 10);
		deliveries = venture_database_find(
			venture_context_get_database(context), deliveries_query, NULL);

		json_builder_set_member_name(builder, "deliveries");
		json_builder_begin_array(builder);

		for (j = 0; (NULL != deliveries) && (j < deliveries->len); j++)
		{
			VentureEntity *delivery;
			g_autofree gchar *event = NULL;
			g_autofree gchar *failure = NULL;
			g_autoptr(GDateTime) attempted = NULL;
			VentureDeliveryState state;
			gint64 status_code = 0;
			gint64 duration = 0;

			delivery = g_ptr_array_index(deliveries, j);
			g_object_get(delivery, "event", &event, "state", &state,
			             "status-code", &status_code, "duration-ms", &duration,
			             "failure-reason", &failure,
			             "attempted-at", &attempted, NULL);

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "id");
			json_builder_add_int_value(builder,
			                           venture_entity_get_id(delivery));
			json_builder_set_member_name(builder, "event");
			json_builder_add_string_value(builder, event);
			json_builder_set_member_name(builder, "state");
			json_builder_add_string_value(builder,
				venture_enum_to_nick(VENTURE_TYPE_DELIVERY_STATE,
				                     (gint)state));
			json_builder_set_member_name(builder, "status_code");
			json_builder_add_int_value(builder, status_code);
			json_builder_set_member_name(builder, "duration_ms");
			json_builder_add_int_value(builder, duration);
			json_builder_set_member_name(builder, "failure_reason");

			if (!venture_string_is_empty(failure))
				json_builder_add_string_value(builder, failure);
			else
				json_builder_add_null_value(builder);

			json_builder_set_member_name(builder, "attempted_at");

			if (NULL != attempted)
			{
				g_autofree gchar *when = NULL;

				when = venture_time_to_string(attempted);
				json_builder_add_string_value(builder, when);
			}
			else
			{
				json_builder_add_null_value(builder);
			}

			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
