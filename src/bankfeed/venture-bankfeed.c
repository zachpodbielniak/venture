/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <libsoup/soup.h>
#include <gio/gio.h>

G_DEFINE_INTERFACE(VentureBankFeedTransport, venture_bank_feed_transport, G_TYPE_OBJECT)
static void
venture_bank_feed_transport_default_init(VentureBankFeedTransportInterface *iface)
{
	(void)iface;
}
gchar *
venture_bank_feed_transport_get(VentureBankFeedTransport *self, const gchar *url,
	const gchar *authorization, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_BANK_FEED_TRANSPORT(self), NULL);
	if (NULL == VENTURE_BANK_FEED_TRANSPORT_GET_IFACE(self)->get)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "The bank feed transport cannot GET");
		return NULL;
	}
	return VENTURE_BANK_FEED_TRANSPORT_GET_IFACE(self)->get(self, url, authorization, error);
}

typedef struct
{
	gchar *url;
	gchar *authorization;
} TransportGetData;

static void
transport_get_data_free(gpointer data)
{
	TransportGetData *req = data;
	g_free(req->url);
	g_free(req->authorization);
	g_free(req);
}

static void
transport_get_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
	VentureBankFeedTransport *self = source;
	TransportGetData *req = task_data;
	g_autoptr(GError) error = NULL;
	gchar *body;
	if (g_task_return_error_if_cancelled(task))
		return;
	(void)cancellable;
	body = VENTURE_BANK_FEED_TRANSPORT_GET_IFACE(self)->get(self, req->url, req->authorization, &error);
	if (body == NULL)
		g_task_return_error(task, g_steal_pointer(&error));
	else
		g_task_return_pointer(task, body, g_free);
}

void
venture_bank_feed_transport_get_async(VentureBankFeedTransport *self, const gchar *url,
	const gchar *authorization, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;
	g_return_if_fail(VENTURE_IS_BANK_FEED_TRANSPORT(self));
	if (VENTURE_BANK_FEED_TRANSPORT_GET_IFACE(self)->get_async != NULL)
	{
		VENTURE_BANK_FEED_TRANSPORT_GET_IFACE(self)->get_async(self, url, authorization,
			cancellable, callback, user_data);
		return;
	}
	task = g_task_new(self, cancellable, callback, user_data);
	if (VENTURE_BANK_FEED_TRANSPORT_GET_IFACE(self)->get == NULL)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"The bank feed transport cannot GET");
		g_object_unref(task);
		return;
	}
	{
		TransportGetData *req = g_new0(TransportGetData, 1);
		req->url = g_strdup(url);
		req->authorization = g_strdup(authorization);
		g_task_set_task_data(task, req, transport_get_data_free);
	}
	g_task_run_in_thread(task, transport_get_thread);
	g_object_unref(task);
}

gchar *
venture_bank_feed_transport_get_finish(VentureBankFeedTransport *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_BANK_FEED_TRANSPORT(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}

G_DEFINE_INTERFACE(VentureBankFeed, venture_bank_feed, G_TYPE_OBJECT)
static void
venture_bank_feed_default_init(VentureBankFeedInterface *iface)
{
	(void)iface;
}
const gchar *
venture_bank_feed_get_name(VentureBankFeed *self)
{
	g_return_val_if_fail(VENTURE_IS_BANK_FEED(self), NULL);
	return VENTURE_BANK_FEED_GET_IFACE(self)->get_name(self);
}
GPtrArray *
venture_bank_feed_fetch(VentureBankFeed *self, const gchar *account_id, GDateTime *from,
	GDateTime *to, const gchar *currency, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_BANK_FEED(self), NULL);
	if (NULL == VENTURE_BANK_FEED_GET_IFACE(self)->fetch)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "The bank feed cannot fetch");
		return NULL;
	}
	return VENTURE_BANK_FEED_GET_IFACE(self)->fetch(self, account_id, from, to, currency, error);
}

void
venture_bank_feed_fetch_async(VentureBankFeed *self, const gchar *account_id,
	GDateTime *from, GDateTime *to, const gchar *currency, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	g_return_if_fail(VENTURE_IS_BANK_FEED(self));
	if (VENTURE_BANK_FEED_GET_IFACE(self)->fetch_async != NULL)
		VENTURE_BANK_FEED_GET_IFACE(self)->fetch_async(self, account_id, from, to,
			currency, cancellable, callback, user_data);
	else
	{
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"The bank feed must implement nonblocking fetch_async");
	}
}

GPtrArray *
venture_bank_feed_fetch_finish(VentureBankFeed *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}

struct _VentureBankFeedRegistry
{
	GObject parent_instance;
	GHashTable *feeds;
};
G_DEFINE_FINAL_TYPE(VentureBankFeedRegistry, venture_bank_feed_registry, G_TYPE_OBJECT)
static void
venture_bank_feed_registry_finalize(GObject *object)
{
	g_hash_table_unref(VENTURE_BANK_FEED_REGISTRY(object)->feeds);
	G_OBJECT_CLASS(venture_bank_feed_registry_parent_class)->finalize(object);
}
static void
venture_bank_feed_registry_class_init(VentureBankFeedRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_bank_feed_registry_finalize;
}
static void
venture_bank_feed_registry_init(VentureBankFeedRegistry *self)
{
	self->feeds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}
VentureBankFeedRegistry *
venture_bank_feed_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_BANK_FEED_REGISTRY, NULL);
}
void
venture_bank_feed_registry_add(VentureBankFeedRegistry *self, VentureBankFeed *feed)
{
	const gchar *name;
	g_return_if_fail(VENTURE_IS_BANK_FEED_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_BANK_FEED(feed));
	name = venture_bank_feed_get_name(feed);
	g_return_if_fail(NULL != name && '\0' != *name);
	g_hash_table_replace(self->feeds, g_strdup(name), feed);
}
VentureBankFeed *
venture_bank_feed_registry_lookup(VentureBankFeedRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_BANK_FEED_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);
	return g_hash_table_lookup(self->feeds, name);
}
gboolean
venture_bank_feed_registry_remove(VentureBankFeedRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_BANK_FEED_REGISTRY(self), FALSE);
	return g_hash_table_remove(self->feeds, name);
}
static gint
compare_feeds(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(venture_bank_feed_get_name(*(VentureBankFeed *const *)a),
		venture_bank_feed_get_name(*(VentureBankFeed *const *)b));
}
GPtrArray *
venture_bank_feed_registry_list(VentureBankFeedRegistry *self)
{
	GPtrArray *result = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, self->feeds);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(result, value);
	g_ptr_array_sort(result, compare_feeds);
	return result;
}

typedef struct { GObject parent_instance; SoupSession *session; } SoupBankFeedTransport;
GType soup_bank_feed_transport_get_type(void);
typedef struct { GObjectClass parent_class; } SoupBankFeedTransportClass;
static void soup_transport_iface(VentureBankFeedTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(SoupBankFeedTransport, soup_bank_feed_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_BANK_FEED_TRANSPORT, soup_transport_iface))
static void soup_transport_get_async(VentureBankFeedTransport *transport, const gchar *url,
	const gchar *authorization, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);

typedef struct
{
	gboolean done;
	gchar *body;
	GError *error;
} HttpWait;

static void
http_wait_done(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpWait *wait = data;
	wait->body = venture_bank_feed_transport_get_finish(VENTURE_BANK_FEED_TRANSPORT(source), result, &wait->error);
	wait->done = TRUE;
}

static gchar *
soup_transport_get(VentureBankFeedTransport *transport, const gchar *url, const gchar *authorization, GError **error)
{
	HttpWait wait;
	GMainContext *context = g_main_context_get_thread_default();

	memset(&wait, 0, sizeof(wait));
	/* Synchronous service callers still dispatch sockets and cancellation.
	 * The async implementation owns authentication and redirect policy. */
	soup_transport_get_async(transport, url, authorization, NULL, http_wait_done, &wait);
	while (!wait.done)
		g_main_context_iteration(context, TRUE);
	if (wait.error != NULL)
		g_propagate_error(error, wait.error);
	return wait.body;
}
static void
soup_got(GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = user_data;
	SoupMessage *message = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	gsize size = 0;
	const guint8 *data;
	if (bytes == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		g_object_unref(task);
		return;
	}
	if (soup_message_get_status(message) < 200 || soup_message_get_status(message) >= 300)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "Provider returned HTTP %u",
			soup_message_get_status(message));
		g_object_unref(task);
		return;
	}
	data = g_bytes_get_data(bytes, &size);
	g_task_return_pointer(task, g_strndup((const gchar *)data, size), g_free);
	g_object_unref(task);
}
static void
soup_apply_auth(SoupMessage *message, const gchar *authorization)
{
	if (venture_string_is_empty(authorization))
		return;
	{
		const gchar *colon = strstr(authorization, ": ");
		if (colon != NULL && !g_str_has_prefix(authorization, "Basic ") &&
			!g_str_has_prefix(authorization, "Bearer "))
		{
			g_autofree gchar *name = g_strndup(authorization, (gsize)(colon - authorization));
			soup_message_headers_append(soup_message_get_request_headers(message),
				name, colon + 2);
		}
		else
			soup_message_headers_append(soup_message_get_request_headers(message),
				"Authorization", authorization);
	}
}
static void
soup_transport_get_async(VentureBankFeedTransport *transport, const gchar *url, const gchar *authorization,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	SoupBankFeedTransport *self = (SoupBankFeedTransport *)transport;
	GTask *task = g_task_new(transport, cancellable, callback, user_data);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	if (message == NULL)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "Invalid bank feed URL");
		g_object_unref(task);
		return;
	}
	soup_message_set_flags(message, soup_message_get_flags(message) | SOUP_MESSAGE_NO_REDIRECT);
	soup_apply_auth(message, authorization);
	g_task_set_task_data(task, g_object_ref(message), g_object_unref);
	soup_session_send_and_read_async(self->session, message, G_PRIORITY_DEFAULT, cancellable, soup_got, task);
}
static void soup_transport_iface(VentureBankFeedTransportInterface *iface)
{
	iface->get = soup_transport_get;
	iface->get_async = soup_transport_get_async;
}
static void
soup_bank_feed_transport_finalize(GObject *object)
{
	g_clear_object(&((SoupBankFeedTransport *)object)->session);
	G_OBJECT_CLASS(soup_bank_feed_transport_parent_class)->finalize(object);
}
static void
soup_bank_feed_transport_class_init(SoupBankFeedTransportClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = soup_bank_feed_transport_finalize;
}
static void
soup_bank_feed_transport_init(SoupBankFeedTransport *self)
{
	self->session = soup_session_new_with_options("timeout", 30, NULL);
}

VentureBankFeedTransport *
venture_bank_feed_transport_new_http(void)
{
	return g_object_new(soup_bank_feed_transport_get_type(), NULL);
}

struct _VentureTellerFeed
{
	GObject parent_instance;
	gchar *token;
	VentureBankFeedTransport *transport;
};
static void teller_iface(VentureBankFeedInterface *iface);
G_DEFINE_TYPE_WITH_CODE(VentureTellerFeed, venture_teller_feed, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_BANK_FEED, teller_iface))
static const gchar *teller_name(VentureBankFeed *self) { (void)self; return "teller"; }
static gchar *
teller_basic(const gchar *token)
{
	g_autofree gchar *pair = g_strdup_printf("%s:", token);
	g_autofree gchar *encoded = g_base64_encode((const guchar *)pair, strlen(pair));
	return g_strdup_printf("Basic %s", encoded);
}
static GPtrArray *
teller_parse_body(const gchar *body, const gchar *currency, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonArray *array;
	GPtrArray *items;
	guint i;
	if (!json_parser_load_from_data(parser, body, -1, error)) return NULL;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Teller response must be a JSON array");
		return NULL;
	}
	array = json_node_get_array(root);
	items = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *raw = json_array_get_object_element(array, i);
		JsonObject *item;
		const gchar *id, *date, *amount, *description, *status;
		if (raw == NULL) continue;
		status = venture_json_object_get_string(raw, "status", "posted");
		if (g_strcmp0(status, "posted") != 0) continue;
		id = venture_json_object_get_string(raw, "id", NULL);
		date = venture_json_object_get_string(raw, "date", NULL);
		amount = venture_json_object_get_string(raw, "amount", NULL);
		description = venture_json_object_get_string(raw, "description", "Bank transaction");
		if (venture_string_is_empty(id) || venture_string_is_empty(date) || venture_string_is_empty(amount))
			continue;
		item = json_object_new();
		json_object_set_string_member(item, "id", id);
		json_object_set_string_member(item, "date", date);
		json_object_set_string_member(item, "amount", amount);
		json_object_set_string_member(item, "description", description);
		if (currency != NULL)
			json_object_set_string_member(item, "currency", currency);
		g_ptr_array_add(items, item);
	}
	return items;
}
static GPtrArray *
teller_fetch(VentureBankFeed *feed, const gchar *account_id, GDateTime *from, GDateTime *to,
	const gchar *currency, GError **error)
{
	VentureTellerFeed *self = VENTURE_TELLER_FEED(feed);
	g_autofree gchar *start = NULL, *end = NULL, *url = NULL, *auth = NULL, *body = NULL;
	if (venture_string_is_empty(account_id) || from == NULL || to == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Teller fetch needs account and window");
		return NULL;
	}
	start = g_date_time_format(from, "%Y-%m-%d");
	end = g_date_time_format(to, "%Y-%m-%d");
	url = g_strdup_printf("https://api.teller.io/accounts/%s/transactions?from=%s&to=%s", account_id, start, end);
	auth = teller_basic(self->token);
	body = venture_bank_feed_transport_get(self->transport, url, auth, error);
	if (body == NULL) return NULL;
	return teller_parse_body(body, currency, error);
}
static void
teller_body_ready(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr(GTask) task = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = venture_bank_feed_transport_get_finish(VENTURE_BANK_FEED_TRANSPORT(source), result, &error);
	GPtrArray *items;
	if (body == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	items = teller_parse_body(body, g_task_get_task_data(task), &error);
	if (items == NULL)
		g_task_return_error(task, g_steal_pointer(&error));
	else
		g_task_return_pointer(task, items, (GDestroyNotify)g_ptr_array_unref);
}

static void
teller_fetch_async(VentureBankFeed *feed, const gchar *account_id, GDateTime *from,
	GDateTime *to, const gchar *currency, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	VentureTellerFeed *self = VENTURE_TELLER_FEED(feed);
	g_autoptr(GTask) task = g_task_new(feed, cancellable, callback, user_data);
	g_autofree gchar *start = NULL, *end = NULL, *url = NULL, *auth = NULL, *account = NULL;
	if (venture_string_is_empty(account_id) || from == NULL || to == NULL)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Teller fetch needs account and window");
		return;
	}
	start = g_date_time_format(from, "%Y-%m-%d");
	end = g_date_time_format(to, "%Y-%m-%d");
	account = g_uri_escape_string(account_id, NULL, FALSE);
	url = g_strdup_printf("https://api.teller.io/accounts/%s/transactions?from=%s&to=%s", account, start, end);
	auth = teller_basic(self->token);
	g_task_set_task_data(task, g_strdup(currency), g_free);
	venture_bank_feed_transport_get_async(self->transport, url, auth, cancellable,
		teller_body_ready, g_steal_pointer(&task));
}

static void teller_iface(VentureBankFeedInterface *iface)
{
	iface->get_name = teller_name;
	iface->fetch = teller_fetch;
	iface->fetch_async = teller_fetch_async;
}
static void
venture_teller_feed_finalize(GObject *object)
{
	VentureTellerFeed *self = VENTURE_TELLER_FEED(object);
	if (self->token != NULL)
	{
		memset(self->token, 0, strlen(self->token));
		g_clear_pointer(&self->token, g_free);
	}
	g_clear_object(&self->transport);
	G_OBJECT_CLASS(venture_teller_feed_parent_class)->finalize(object);
}
static void
venture_teller_feed_class_init(VentureTellerFeedClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_teller_feed_finalize;
}
static void venture_teller_feed_init(VentureTellerFeed *self) { (void)self; }
VentureBankFeed *
venture_teller_feed_new(const gchar *access_token, VentureBankFeedTransport *transport)
{
	VentureTellerFeed *self = g_object_new(VENTURE_TYPE_TELLER_FEED, NULL);
	self->token = g_strdup(access_token);
	self->transport = transport != NULL ? g_object_ref(transport) : g_object_new(soup_bank_feed_transport_get_type(), NULL);
	return VENTURE_BANK_FEED(self);
}

struct _VentureBankFeedService
{
	GObject parent_instance;
	VentureDatabase *database;
	gint64 organization_id;
	VentureBankFeedRegistry *registry;
};
G_DEFINE_TYPE(VentureBankFeedService, venture_bankfeed_service, G_TYPE_OBJECT)
static void
venture_bankfeed_service_finalize(GObject *object)
{
	VentureBankFeedService *self = VENTURE_BANKFEED_SERVICE(object);
	g_clear_object(&self->registry);
	g_clear_object(&self->database);
	G_OBJECT_CLASS(venture_bankfeed_service_parent_class)->finalize(object);
}
static void
venture_bankfeed_service_class_init(VentureBankFeedServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_bankfeed_service_finalize;
}
static void venture_bankfeed_service_init(VentureBankFeedService *self) { (void)self; }

VentureBankFeedService *
venture_bankfeed_service_new(VentureDatabase *database, gint64 organization_id,
	VentureBankFeedTransport *transport, GError **error)
{
	g_autoptr(VentureBankFeedService) self = NULL;
	const gchar *key;
	(void)error;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_new(VENTURE_TYPE_BANKFEED_SERVICE, NULL);
	self->database = g_object_ref(database);
	self->organization_id = organization_id;
	self->registry = venture_bank_feed_registry_new();
	key = g_getenv("VENTURE_BANKFEED_TELLER_KEY");
	if (key != NULL && *key != '\0')
		venture_bank_feed_registry_add(self->registry, venture_teller_feed_new(key, transport));
	return g_steal_pointer(&self);
}

VentureBankFeedRegistry *
venture_bankfeed_service_get_registry(VentureBankFeedService *self)
{
	g_return_val_if_fail(VENTURE_IS_BANKFEED_SERVICE(self), NULL);
	return self->registry;
}

static gint
count_filter(VentureDatabase *db, GType type, gint64 org, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	gint64 n;
	venture_query_set_organization(query, org);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return -1;
	n = venture_database_count(db, query, error);
	return n < 0 ? -1 : (gint)n;
}

typedef struct
{
	gboolean done;
	gint count;
	GError *error;
} SyncWait;

static void
sync_wait_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SyncWait *wait = data;
	wait->count = venture_bankfeed_service_sync_finish(VENTURE_BANKFEED_SERVICE(source), result, &wait->error);
	wait->done = TRUE;
}

gint
venture_bankfeed_service_sync(VentureBankFeedService *self, gint64 connection_id,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error)
{
	SyncWait wait;
	GMainContext *context = g_main_context_get_thread_default();
	memset(&wait, 0, sizeof(wait));
	venture_bankfeed_service_sync_async(self, connection_id, from, to, actor, NULL, sync_wait_done, &wait);
	while (!wait.done)
		g_main_context_iteration(context, TRUE);
	if (wait.error != NULL)
		g_propagate_error(error, wait.error);
	return wait.count;
}

typedef struct
{
	gint64 bank_id;
	gint64 organization_id;
	gchar *currency;
	gchar *account;
	GDateTime *from;
	GDateTime *to;
	GDateTime *now;
	VentureEntity *connection;
	VentureBankFeed *feed;
	gboolean has_actor;
	VentureActor actor;
	gchar *actor_name;
	gchar *actor_prompt;
	gchar *actor_request_id;
	gchar *actor_approved_by;
} BankfeedSyncJob;

static void
bankfeed_sync_job_free(gpointer data)
{
	BankfeedSyncJob *job = data;
	g_free(job->currency);
	g_free(job->account);
	g_clear_pointer(&job->from, g_date_time_unref);
	g_clear_pointer(&job->to, g_date_time_unref);
	g_clear_pointer(&job->now, g_date_time_unref);
	g_clear_object(&job->connection);
	g_clear_object(&job->feed);
	g_free(job->actor_name);
	g_free(job->actor_prompt);
	g_free(job->actor_request_id);
	g_free(job->actor_approved_by);
	g_free(job);
}

static gint
bankfeed_import_items(VentureBankFeedService *self, BankfeedSyncJob *job, GPtrArray *items, GError **error)
{
	g_autoptr(JsonObject) args = json_object_new();
	g_autoptr(JsonArray) transactions = json_array_new();
	g_autoptr(VentureEntity) result = NULL;
	g_autofree gchar *start = NULL, *end = NULL;
	gint64 before, after;
	guint i;
	const VentureActor *actor = job->has_actor ? &job->actor : NULL;
	for (i = 0; i < items->len; i++)
	{
		JsonObject *item = g_ptr_array_index(items, i);
		JsonObject *row = json_object_new();
		const gchar *id = venture_json_object_get_string(item, "id", NULL);
		const gchar *date = venture_json_object_get_string(item, "date", NULL);
		const gchar *amount = venture_json_object_get_string(item, "amount", NULL);
		const gchar *description = venture_json_object_get_string(item, "description", "Bank transaction");
		json_object_set_string_member(row, "date", date);
		json_object_set_string_member(row, "amount", amount);
		json_object_set_string_member(row, "description", description);
		json_object_set_string_member(row, "external_id", id);
		json_array_add_object_element(transactions, row);
	}
	start = g_date_time_format(job->from, "%Y-%m-%d");
	end = g_date_time_format(job->to, "%Y-%m-%d");
	json_object_set_string_member(args, "period_start", start);
	json_object_set_string_member(args, "period_end", end);
	json_object_set_array_member(args, "transactions", g_steal_pointer(&transactions));
	before = count_filter(self->database, VENTURE_TYPE_BANK_TRANSACTION, job->organization_id, "bank-account-id", job->bank_id, error);
	if (before < 0) return -1;
	if (!venture_database_begin(self->database, error)) return -1;
	result = venture_bank_match_service_execute(venture_database_get_bank_match_service(self->database),
		"feed", job->bank_id, args, actor, error);
	if (result == NULL)
	{
		venture_database_rollback(self->database);
		return -1;
	}
	after = count_filter(self->database, VENTURE_TYPE_BANK_TRANSACTION, job->organization_id, "bank-account-id", job->bank_id, error);
	if (after < 0)
	{
		venture_database_rollback(self->database);
		return -1;
	}
	g_object_set(job->connection, "last-synced-at", job->now, "last-imported", (gint64)(after - before), "status", "linked", NULL);
	if (!venture_database_save(self->database, job->connection, actor, error))
	{
		venture_database_rollback(self->database);
		return -1;
	}
	if (!venture_database_commit(self->database, error)) return -1;
	return (gint)(after - before);
}

static BankfeedSyncJob *
bankfeed_sync_prepare(VentureBankFeedService *self, gint64 connection_id,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error)
{
	BankfeedSyncJob *job;
	g_autoptr(VentureEntity) bank = NULL;
	g_autofree gchar *provider = NULL;
	VentureBankFeed *feed;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_connection") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Bank feed module is disabled (bankfeed.enabled)");
		return NULL;
	}
	job = g_new0(BankfeedSyncJob, 1);
	job->now = venture_time_now();
	job->from = from != NULL ? g_date_time_ref(from) : g_date_time_add_days(job->now, -30);
	job->to = to != NULL ? g_date_time_ref(to) : g_date_time_ref(job->now);
	job->connection = venture_database_get(self->database, VENTURE_TYPE_BANK_CONNECTION, connection_id, error);
	if (job->connection == NULL)
	{
		bankfeed_sync_job_free(job);
		return NULL;
	}
	job->organization_id = venture_entity_get_organization_id(job->connection);
	if (job->organization_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Bank connection needs an organization");
		bankfeed_sync_job_free(job);
		return NULL;
	}
	g_object_get(job->connection, "provider", &provider, "provider-account-id", &job->account,
		"bank-account-id", &job->bank_id, NULL);
	feed = venture_bank_feed_registry_lookup(self->registry, provider);
	if (feed == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No bank feed named %s", provider);
		bankfeed_sync_job_free(job);
		return NULL;
	}
	job->feed = g_object_ref(feed);
	bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, job->bank_id, error);
	if (bank == NULL)
	{
		bankfeed_sync_job_free(job);
		return NULL;
	}
	if (venture_entity_get_organization_id(bank) != job->organization_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Bank account belongs to another organization");
		bankfeed_sync_job_free(job);
		return NULL;
	}
	g_object_get(bank, "currency", &job->currency, NULL);
	if (actor != NULL)
	{
		job->has_actor = TRUE;
		job->actor = *actor;
		job->actor_name = g_strdup(actor->name);
		job->actor.name = job->actor_name;
		job->actor.prompt = job->actor_prompt = g_strdup(actor->prompt);
		job->actor.request_id = job->actor_request_id = g_strdup(actor->request_id);
		job->actor.approved_by = job->actor_approved_by = g_strdup(actor->approved_by);
	}
	return job;
}

static void
bankfeed_body_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GTask) task = user_data;
	VentureBankFeedService *self = g_task_get_source_object(task);
	BankfeedSyncJob *job = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) items = venture_bank_feed_fetch_finish(VENTURE_BANK_FEED(source), result, &error);
	gint imported;
	if (g_task_return_error_if_cancelled(task))
		return;
	if (items == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	imported = bankfeed_import_items(self, job, items, &error);
	if (imported < 0)
		g_task_return_error(task, g_steal_pointer(&error));
	else
		g_task_return_int(task, imported);
}

void
venture_bankfeed_service_sync_async(VentureBankFeedService *self, gint64 connection_id,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data)
{
	GTask *task;
	BankfeedSyncJob *job;
	g_autoptr(GError) error = NULL;
	g_return_if_fail(VENTURE_IS_BANKFEED_SERVICE(self));
	task = g_task_new(self, cancellable, callback, user_data);
	job = bankfeed_sync_prepare(self, connection_id, from, to, actor, &error);
	if (job == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		g_object_unref(task);
		return;
	}
	g_task_set_task_data(task, job, bankfeed_sync_job_free);
	venture_bank_feed_fetch_async(job->feed, job->account, job->from, job->to,
		job->currency, cancellable, bankfeed_body_ready, task);
}

gint
venture_bankfeed_service_sync_finish(VentureBankFeedService *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_BANKFEED_SERVICE(self), -1);
	g_return_val_if_fail(g_task_is_valid(result, self), -1);
	if (g_task_had_error(G_TASK(result)))
	{
		g_task_propagate_int(G_TASK(result), error);
		return -1;
	}
	return g_task_propagate_int(G_TASK(result), error);
}

gint
venture_bankfeed_service_sync_due(VentureBankFeedService *self, gint64 organization_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_CONNECTION);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) from = NULL, to = NULL;
	gint total = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_BANKFEED_SERVICE(self), -1);
	to = venture_time_now();
	from = g_date_time_add_days(to, -30);
	venture_query_set_organization(query, organization_id);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL) return -1;
	for (i = 0; i < rows->len; i++)
	{
		gint imported = venture_bankfeed_service_sync(self,
			venture_entity_get_id(g_ptr_array_index(rows, i)), from, to, actor, error);
		if (imported < 0) return -1;
		total += imported;
	}
	return total;
}
