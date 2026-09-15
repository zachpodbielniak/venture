/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BANKFEED_H
#define VENTURE_BANKFEED_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_BANK_FEED_TRANSPORT (venture_bank_feed_transport_get_type())
G_DECLARE_INTERFACE(VentureBankFeedTransport, venture_bank_feed_transport, VENTURE, BANK_FEED_TRANSPORT, GObject)
/**
 * VentureBankFeedTransportInterface:
 * @get: GET a URL; tests inject a fake, production uses HTTP
 */
struct _VentureBankFeedTransportInterface
{
	GTypeInterface parent_iface;
	gchar *(*get)(VentureBankFeedTransport *self, const gchar *url, const gchar *authorization, GError **error);
	void (*get_async)(VentureBankFeedTransport *self, const gchar *url, const gchar *authorization,
		GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
};
/**
 * venture_bank_feed_transport_get:
 * @self: the transport
 * @url: absolute URL
 * @authorization: (nullable): Authorization value, or "Header: value"
 * @error: (out) (optional): network failure
 *
 * Synchronous GET. Production HTTP uses the async form from the server path.
 *
 * Returns: (transfer full) (nullable): response body
 */
gchar *venture_bank_feed_transport_get(VentureBankFeedTransport *self, const gchar *url,
	const gchar *authorization, GError **error);
/**
 * venture_bank_feed_transport_get_async: (finish-func venture_bank_feed_transport_get_finish)
 * @self: the transport
 * @url: absolute URL
 * @authorization: (nullable): Authorization value, or "Header: value"
 * @cancellable: (nullable): cancel the in-flight GET
 * @callback: (scope async) (closure user_data): completion on the thread-default context
 * @user_data: (nullable): callback data
 *
 * Network I/O must not run on a worker that also writes the database.
 */
void venture_bank_feed_transport_get_async(VentureBankFeedTransport *self, const gchar *url,
	const gchar *authorization, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
/**
 * venture_bank_feed_transport_get_finish:
 * @self: the transport
 * @result: the async result
 * @error: (out) (optional): network or cancellation
 *
 * Returns: (transfer full) (nullable): response body
 */
gchar *venture_bank_feed_transport_get_finish(VentureBankFeedTransport *self, GAsyncResult *result, GError **error);
/**
 * venture_bank_feed_transport_new_http:
 *
 * Returns: (transfer full): owned result
 */
VentureBankFeedTransport *venture_bank_feed_transport_new_http(void);

#define VENTURE_TYPE_BANK_FEED (venture_bank_feed_get_type())
G_DECLARE_INTERFACE(VentureBankFeed, venture_bank_feed, VENTURE, BANK_FEED, GObject)
/**
 * VentureBankFeedInterface:
 * @get_name: registry key
 * @fetch: provider transactions as JSON objects with id, date, amount, description
 * @fetch_async: nonblocking fetch, returning a GTask with owned JSON objects
 */
struct _VentureBankFeedInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VentureBankFeed *self);
	GPtrArray *(*fetch)(VentureBankFeed *self, const gchar *account_id, GDateTime *from,
		GDateTime *to, const gchar *currency, GError **error);
	void (*fetch_async)(VentureBankFeed *self, const gchar *account_id, GDateTime *from,
		GDateTime *to, const gchar *currency, GCancellable *cancellable,
		GAsyncReadyCallback callback, gpointer user_data);
};
/**
 * venture_bank_feed_get_name:
 * @self: the service or registry instance
 *
 * Returns: (transfer none): provider registry key
 */
const gchar *venture_bank_feed_get_name(VentureBankFeed *self);
/**
 * venture_bank_feed_fetch:
 * @self: the feed
 * @account_id: provider account
 * @from: inclusive start
 * @to: exclusive end
 * @currency: (nullable): book currency for imported rows
 * @error: (out) (optional)
 *
 * Returns: (transfer full) (element-type JsonObject) (nullable): posted transactions
 */
GPtrArray *venture_bank_feed_fetch(VentureBankFeed *self, const gchar *account_id,
	GDateTime *from, GDateTime *to, const gchar *currency, GError **error);

/**
 * venture_bank_feed_fetch_async: (finish-func venture_bank_feed_fetch_finish)
 * @self: provider
 * @account_id: account identifier
 * @from: inclusive start
 * @to: exclusive end
 * @currency: (nullable): book currency
 * @cancellable: (nullable): cancellation
 * @callback: (scope async) (closure user_data): completion on the thread-default main context
 * @user_data: (nullable): callback data
 *
 * Providers implement fetch_async with a GTask returning an owned GPtrArray
 * of owned JsonObject values. Implementations must copy borrowed arguments
 * before returning. A synchronous-only plugin is refused by async callers.
 */
void venture_bank_feed_fetch_async(VentureBankFeed *self, const gchar *account_id,
	GDateTime *from, GDateTime *to, const gchar *currency, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data);
/**
 * venture_bank_feed_fetch_finish:
 * @self: provider
 * @result: async result
 * @error: (out) (optional): failure
 *
 * Returns: (transfer full) (element-type JsonObject) (nullable): transactions
 */
GPtrArray *venture_bank_feed_fetch_finish(VentureBankFeed *self, GAsyncResult *result, GError **error);

#define VENTURE_TYPE_BANK_FEED_REGISTRY (venture_bank_feed_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureBankFeedRegistry, venture_bank_feed_registry, VENTURE, BANK_FEED_REGISTRY, GObject)
/**
 * venture_bank_feed_registry_new:
 *
 * Returns: (transfer full): owned result
 */
VentureBankFeedRegistry *venture_bank_feed_registry_new(void);
/**
 * venture_bank_feed_registry_add:
 * @self: the registry
 * @feed: (transfer full): the registry takes ownership
 */
void venture_bank_feed_registry_add(VentureBankFeedRegistry *self, VentureBankFeed *feed);
/**
 * venture_bank_feed_registry_lookup:
 * @self: the registry
 * @name: provider key
 *
 * Returns: (transfer none) (nullable): borrowed feed
 */
VentureBankFeed *venture_bank_feed_registry_lookup(VentureBankFeedRegistry *self, const gchar *name);
/**
 * venture_bank_feed_registry_remove:
 * @self: the service or registry instance
 * @name: name or registry key
 *
 * Returns: TRUE if an entry was removed
 */
gboolean venture_bank_feed_registry_remove(VentureBankFeedRegistry *self, const gchar *name);
/**
 * venture_bank_feed_registry_list:
 * @self: the registry
 *
 * Returns: (transfer container) (element-type VentureBankFeed): borrowed feeds
 */
GPtrArray *venture_bank_feed_registry_list(VentureBankFeedRegistry *self);

#define VENTURE_TYPE_TELLER_FEED (venture_teller_feed_get_type())
G_DECLARE_FINAL_TYPE(VentureTellerFeed, venture_teller_feed, VENTURE, TELLER_FEED, GObject)
/**
 * venture_teller_feed_new:
 * @access_token: provider credential, copied by the constructor
 * @transport: (nullable): injected transport; NULL selects HTTP
 *
 * Returns: (transfer full): owned result
 */
VentureBankFeed *venture_teller_feed_new(const gchar *access_token, VentureBankFeedTransport *transport);

#define VENTURE_TYPE_BANKFEED_SERVICE (venture_bankfeed_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBankFeedService, venture_bankfeed_service, VENTURE, BANKFEED_SERVICE, GObject)
/**
 * venture_bankfeed_service_new:
 * @database: database owning the records
 * @organization_id: target legal entity ID
 * @transport: (nullable): injected transport; NULL selects HTTP
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureBankFeedService *venture_bankfeed_service_new(VentureDatabase *database, gint64 organization_id,
	VentureBankFeedTransport *transport, GError **error);
/**
 * venture_bankfeed_service_get_registry:
 * @self: the service or registry instance
 *
 * Returns: (transfer none): borrowed result
 */
VentureBankFeedRegistry *venture_bankfeed_service_get_registry(VentureBankFeedService *self);
/**
 * venture_bankfeed_service_sync:
 * @self: the service
 * @connection_id: a bank_connection in this organization
 * @from: (nullable): default 30 days ago
 * @to: (nullable): default now
 * @actor: (nullable): audit actor
 * @error: (out) (optional)
 *
 * Fetches then writes bank_transaction on this thread. Prefer the async form
 * from a server request so health stays responsive.
 *
 * Returns: imported row count, or -1
 */
gint venture_bankfeed_service_sync(VentureBankFeedService *self, gint64 connection_id,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error);
/**
 * venture_bankfeed_service_sync_async:
 * @self: the service
 * @connection_id: a bank_connection in this organization
 * @from: (nullable)
 * @to: (nullable)
 * @actor: (nullable)
 * @cancellable: (nullable)
 * @callback: (scope async) (closure user_data)
 * @user_data: (nullable): callback data
 *
 * Looks up the connection on the caller thread, fetches on the transport's
 * async path, and applies database changes on the completion context.
 */
void venture_bankfeed_service_sync_async(VentureBankFeedService *self, gint64 connection_id,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GCancellable *cancellable,
	GAsyncReadyCallback callback, gpointer user_data);
/**
 * venture_bankfeed_service_sync_finish:
 * @self: the service
 * @result: the async result
 * @error: (out) (optional)
 *
 * Returns: imported row count, or -1
 */
gint venture_bankfeed_service_sync_finish(VentureBankFeedService *self, GAsyncResult *result, GError **error);
/**
 * venture_bankfeed_service_sync_due:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Syncs connections belonging to the requested organization through the shared asynchronous import path.
 *
 * Returns: number processed, or -1 on failure
 */
gint venture_bankfeed_service_sync_due(VentureBankFeedService *self, gint64 organization_id,
	const VentureActor *actor, GError **error);

G_END_DECLS
#endif
