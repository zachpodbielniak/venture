/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <libsoup/soup.h>

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
static gchar *
soup_transport_get(VentureBankFeedTransport *transport, const gchar *url, const gchar *authorization, GError **error)
{
	SoupBankFeedTransport *self = (SoupBankFeedTransport *)transport;
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	g_autoptr(GBytes) bytes = NULL;
	gsize size = 0;
	const guint8 *data;
	if (message == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "Invalid bank feed URL");
		return NULL;
	}
	if (!venture_string_is_empty(authorization))
		soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", authorization);
	bytes = soup_session_send_and_read(self->session, message, NULL, error);
	if (bytes == NULL) return NULL;
	if (soup_message_get_status(message) < 200 || soup_message_get_status(message) >= 300)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "Teller returned HTTP %u",
			soup_message_get_status(message));
		return NULL;
	}
	data = g_bytes_get_data(bytes, &size);
	return g_strndup((const gchar *)data, size);
}
static void soup_transport_iface(VentureBankFeedTransportInterface *iface) { iface->get = soup_transport_get; }
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
	self->session = soup_session_new();
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
teller_fetch(VentureBankFeed *feed, const gchar *account_id, GDateTime *from, GDateTime *to,
	const gchar *currency, GError **error)
{
	VentureTellerFeed *self = VENTURE_TELLER_FEED(feed);
	g_autofree gchar *start = NULL, *end = NULL, *url = NULL, *auth = NULL, *body = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonArray *array;
	GPtrArray *items;
	guint i;
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
static void teller_iface(VentureBankFeedInterface *iface)
{
	iface->get_name = teller_name;
	iface->fetch = teller_fetch;
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

static gboolean
require_key(GError **error)
{
	const gchar *key = g_getenv("VENTURE_BANKFEED_TELLER_KEY");
	if (key == NULL || *key == '\0')
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			"Bank feed module requires VENTURE_BANKFEED_TELLER_KEY");
		return FALSE;
	}
	return TRUE;
}

VentureBankFeedService *
venture_bankfeed_service_new(VentureDatabase *database, gint64 organization_id,
	VentureBankFeedTransport *transport, GError **error)
{
	g_autoptr(VentureBankFeedService) self = NULL;
	const gchar *key;
	if (!require_key(error)) return NULL;
	key = g_getenv("VENTURE_BANKFEED_TELLER_KEY");
	self = g_object_new(VENTURE_TYPE_BANKFEED_SERVICE, NULL);
	self->database = g_object_ref(database);
	self->organization_id = organization_id;
	self->registry = venture_bank_feed_registry_new();
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

gint
venture_bankfeed_service_sync(VentureBankFeedService *self, gint64 connection_id,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) connection = NULL;
	g_autoptr(VentureEntity) bank = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(JsonObject) args = json_object_new();
	JsonArray *transactions = json_array_new();
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GDateTime) window_from = NULL, window_to = NULL, now = NULL;
	g_autofree gchar *provider = NULL, *account = NULL, *currency = NULL, *start = NULL, *end = NULL;
	VentureBankFeed *feed;
	gint64 bank_id = 0, before, after;
	guint i;
	g_return_val_if_fail(VENTURE_IS_BANKFEED_SERVICE(self), -1);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_connection") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Bank feed module is disabled (bankfeed.enabled)");
		return -1;
	}
	window_from = from != NULL ? g_date_time_ref(from) : g_date_time_add_days(now = venture_time_now(), -30);
	if (now == NULL) now = venture_time_now();
	window_to = to != NULL ? g_date_time_ref(to) : g_date_time_ref(now);
	connection = venture_database_get(self->database, VENTURE_TYPE_BANK_CONNECTION, connection_id, error);
	if (connection == NULL) return -1;
	if (venture_entity_get_organization_id(connection) != self->organization_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Bank connection belongs to another organization");
		return -1;
	}
	g_object_get(connection, "provider", &provider, "provider-account-id", &account, "bank-account-id", &bank_id, NULL);
	feed = venture_bank_feed_registry_lookup(self->registry, provider);
	if (feed == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No bank feed named %s", provider);
		return -1;
	}
	bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, bank_id, error);
	if (bank == NULL) return -1;
	g_object_get(bank, "currency", &currency, NULL);
	items = venture_bank_feed_fetch(feed, account, window_from, window_to, currency, error);
	if (items == NULL) return -1;
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
	start = g_date_time_format(window_from, "%Y-%m-%d");
	end = g_date_time_format(window_to, "%Y-%m-%d");
	json_object_set_string_member(args, "period_start", start);
	json_object_set_string_member(args, "period_end", end);
	json_object_set_array_member(args, "transactions", transactions);
	before = count_filter(self->database, VENTURE_TYPE_BANK_TRANSACTION, self->organization_id, "bank-account-id", bank_id, error);
	if (before < 0) return -1;
	if (!venture_database_begin(self->database, error)) return -1;
	result = venture_bank_match_service_execute(venture_database_get_bank_match_service(self->database),
		"feed", bank_id, args, actor, error);
	if (result == NULL)
	{
		venture_database_rollback(self->database);
		return -1;
	}
	after = count_filter(self->database, VENTURE_TYPE_BANK_TRANSACTION, self->organization_id, "bank-account-id", bank_id, error);
	if (after < 0)
	{
		venture_database_rollback(self->database);
		return -1;
	}
	g_object_set(connection, "last-synced-at", now, "last-imported", (gint64)(after - before), "status", "linked", NULL);
	if (!venture_database_save(self->database, connection, actor, error))
	{
		venture_database_rollback(self->database);
		return -1;
	}
	if (!venture_database_commit(self->database, error)) return -1;
	return (gint)(after - before);
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
