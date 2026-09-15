/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_COMMERCE_H
#define VENTURE_COMMERCE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_COMMERCE_CONNECTOR (venture_commerce_connector_get_type())
G_DECLARE_INTERFACE(VentureCommerceConnector, venture_commerce_connector, VENTURE, COMMERCE_CONNECTOR, GObject)
/**
 * VentureCommerceConnectorInterface:
 * @get_name: registry key
 * @fetch_orders: orders as JSON objects with external_id, company_id, lines
 */
struct _VentureCommerceConnectorInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VentureCommerceConnector *self);
	GPtrArray *(*fetch_orders)(VentureCommerceConnector *self, GDateTime *from, GDateTime *to, GError **error);
};
const gchar *venture_commerce_connector_get_name(VentureCommerceConnector *self);
GPtrArray *venture_commerce_connector_fetch_orders(VentureCommerceConnector *self,
	GDateTime *from, GDateTime *to, GError **error);

#define VENTURE_TYPE_COMMERCE_CONNECTOR_REGISTRY (venture_commerce_connector_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureCommerceConnectorRegistry, venture_commerce_connector_registry,
	VENTURE, COMMERCE_CONNECTOR_REGISTRY, GObject)
VentureCommerceConnectorRegistry *venture_commerce_connector_registry_new(void);
void venture_commerce_connector_registry_add(VentureCommerceConnectorRegistry *self, VentureCommerceConnector *connector);
VentureCommerceConnector *venture_commerce_connector_registry_lookup(VentureCommerceConnectorRegistry *self, const gchar *name);
gboolean venture_commerce_connector_registry_remove(VentureCommerceConnectorRegistry *self, const gchar *name);
GPtrArray *venture_commerce_connector_registry_list(VentureCommerceConnectorRegistry *self);

#define VENTURE_TYPE_SHOPIFY_CONNECTOR (venture_shopify_connector_get_type())
G_DECLARE_FINAL_TYPE(VentureShopifyConnector, venture_shopify_connector, VENTURE, SHOPIFY_CONNECTOR, GObject)
VentureCommerceConnector *venture_shopify_connector_new(const gchar *shop, const gchar *token,
	VentureBankFeedTransport *transport);

#define VENTURE_TYPE_COMMERCE_SERVICE (venture_commerce_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCommerceService, venture_commerce_service, VENTURE, COMMERCE_SERVICE, GObject)
VentureCommerceService *venture_commerce_service_new(VentureDatabase *database, gint64 organization_id,
	VentureBankFeedTransport *transport, GError **error);
VentureCommerceConnectorRegistry *venture_commerce_service_get_registry(VentureCommerceService *self);
gint venture_commerce_service_import(VentureCommerceService *self, const gchar *connector_name,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error);

G_END_DECLS
#endif
