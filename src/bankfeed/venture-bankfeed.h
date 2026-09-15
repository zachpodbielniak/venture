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
};
gchar *venture_bank_feed_transport_get(VentureBankFeedTransport *self, const gchar *url,
	const gchar *authorization, GError **error);

#define VENTURE_TYPE_BANK_FEED (venture_bank_feed_get_type())
G_DECLARE_INTERFACE(VentureBankFeed, venture_bank_feed, VENTURE, BANK_FEED, GObject)
/**
 * VentureBankFeedInterface:
 * @get_name: registry key
 * @fetch: provider transactions as JSON objects with id, date, amount, description
 */
struct _VentureBankFeedInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VentureBankFeed *self);
	GPtrArray *(*fetch)(VentureBankFeed *self, const gchar *account_id, GDateTime *from,
		GDateTime *to, const gchar *currency, GError **error);
};
const gchar *venture_bank_feed_get_name(VentureBankFeed *self);
GPtrArray *venture_bank_feed_fetch(VentureBankFeed *self, const gchar *account_id,
	GDateTime *from, GDateTime *to, const gchar *currency, GError **error);

#define VENTURE_TYPE_BANK_FEED_REGISTRY (venture_bank_feed_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureBankFeedRegistry, venture_bank_feed_registry, VENTURE, BANK_FEED_REGISTRY, GObject)
VentureBankFeedRegistry *venture_bank_feed_registry_new(void);
void venture_bank_feed_registry_add(VentureBankFeedRegistry *self, VentureBankFeed *feed);
VentureBankFeed *venture_bank_feed_registry_lookup(VentureBankFeedRegistry *self, const gchar *name);
gboolean venture_bank_feed_registry_remove(VentureBankFeedRegistry *self, const gchar *name);
GPtrArray *venture_bank_feed_registry_list(VentureBankFeedRegistry *self);

#define VENTURE_TYPE_TELLER_FEED (venture_teller_feed_get_type())
G_DECLARE_FINAL_TYPE(VentureTellerFeed, venture_teller_feed, VENTURE, TELLER_FEED, GObject)
VentureBankFeed *venture_teller_feed_new(const gchar *access_token, VentureBankFeedTransport *transport);

#define VENTURE_TYPE_BANKFEED_SERVICE (venture_bankfeed_service_get_type())
G_DECLARE_FINAL_TYPE(VentureBankFeedService, venture_bankfeed_service, VENTURE, BANKFEED_SERVICE, GObject)
VentureBankFeedService *venture_bankfeed_service_new(VentureDatabase *database, gint64 organization_id,
	VentureBankFeedTransport *transport, GError **error);
VentureBankFeedRegistry *venture_bankfeed_service_get_registry(VentureBankFeedService *self);
gint venture_bankfeed_service_sync(VentureBankFeedService *self, gint64 connection_id,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error);
gint venture_bankfeed_service_sync_due(VentureBankFeedService *self, gint64 organization_id,
	const VentureActor *actor, GError **error);

G_END_DECLS
#endif
