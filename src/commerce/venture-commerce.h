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
/**
 * venture_commerce_connector_get_name:
 * @self: the service or registry instance
 *
 * Returns: (transfer none): provider registry key
 */
const gchar *venture_commerce_connector_get_name(VentureCommerceConnector *self);
/**
 * venture_commerce_connector_fetch_orders:
 * @self: the service or registry instance
 * @from: (nullable): start of the requested window
 * @to: (nullable): end of the requested window
 * @error: (out) (optional): return location for an error
 *
 * Fetches normalized order objects. Providers must preserve currency and external identity; the service applies them through document and settlement services.
 *
 * Returns: (transfer full) (element-type JsonObject) (nullable): owned result
 */
GPtrArray *venture_commerce_connector_fetch_orders(VentureCommerceConnector *self,
	GDateTime *from, GDateTime *to, GError **error);

#define VENTURE_TYPE_COMMERCE_CONNECTOR_FACTORY (venture_commerce_connector_factory_get_type())
G_DECLARE_INTERFACE(VentureCommerceConnectorFactory, venture_commerce_connector_factory,
	VENTURE, COMMERCE_CONNECTOR_FACTORY, GObject)
/**
 * VentureCommerceConnectorFactoryInterface:
 * @get_name: stable provider key, without the commerce. namespace
 * @create: construct a fresh client from one verified binding snapshot
 *
 * Factories hold no organization credentials. Clients must use the supplied
 * guarded transport; fetches run on the database-owning main thread.
 */
struct _VentureCommerceConnectorFactoryInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VentureCommerceConnectorFactory *self);
	VentureCommerceConnector *(*create)(VentureCommerceConnectorFactory *self,
		const gchar *account_id, JsonNode *settings, VentureBankFeedTransport *transport, GError **error);
};
/**
 * venture_commerce_connector_factory_get_name:
 * @self: stateless factory
 * Returns: (transfer none): stable provider key
 */
const gchar *venture_commerce_connector_factory_get_name(VentureCommerceConnectorFactory *self);
/**
 * venture_commerce_connector_factory_create:
 * @self: stateless factory
 * @account_id: immutable verified binding identity
 * @settings: decrypted snapshot; never retained by the factory
 * @transport: main-thread binding-guarded transport
 * @error: (out) (optional): safe validation failure
 * Returns: (transfer full) (nullable): fresh account client
 */
VentureCommerceConnector *venture_commerce_connector_factory_create(VentureCommerceConnectorFactory *self,
	const gchar *account_id, JsonNode *settings, VentureBankFeedTransport *transport, GError **error);

#define VENTURE_TYPE_COMMERCE_CONNECTOR_REGISTRY (venture_commerce_connector_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureCommerceConnectorRegistry, venture_commerce_connector_registry,
	VENTURE, COMMERCE_CONNECTOR_REGISTRY, GObject)
/**
 * venture_commerce_connector_registry_new:
 *
 * Returns: (transfer full): owned result
 */
VentureCommerceConnectorRegistry *venture_commerce_connector_registry_new(void);
/**
 * venture_commerce_connector_registry_add:
 * @self: the registry
 * @connector: (transfer full): the registry takes ownership
 */
void venture_commerce_connector_registry_add(VentureCommerceConnectorRegistry *self, VentureCommerceConnector *connector);
/**
 * venture_commerce_connector_registry_lookup:
 * @self: the registry
 * @name: connector key
 *
 * Returns: (transfer none) (nullable): borrowed connector
 */
VentureCommerceConnector *venture_commerce_connector_registry_lookup(VentureCommerceConnectorRegistry *self, const gchar *name);
/**
 * venture_commerce_connector_registry_remove:
 * @self: the service or registry instance
 * @name: name or registry key
 *
 * Returns: TRUE if an entry was removed
 */
gboolean venture_commerce_connector_registry_remove(VentureCommerceConnectorRegistry *self, const gchar *name);
/**
 * venture_commerce_connector_registry_list:
 * @self: the registry
 *
 * Returns: (transfer container) (element-type VentureCommerceConnector): borrowed connectors
 */
GPtrArray *venture_commerce_connector_registry_list(VentureCommerceConnectorRegistry *self);

/**
 * venture_commerce_connector_registry_add_factory:
 * @self: registry
 * @factory: (transfer full): stateless factory owned by the registry
 *
 * Replaces only a factory of the same name. Legacy live clients remain visible
 * through lookup(), but imports require factories and explicit vault bindings.
 */
void venture_commerce_connector_registry_add_factory(VentureCommerceConnectorRegistry *self,
	VentureCommerceConnectorFactory *factory);
/**
 * venture_commerce_connector_registry_lookup_factory:
 * @self: registry
 * @name: provider key
 * Returns: (transfer none) (nullable): registered stateless factory
 */
VentureCommerceConnectorFactory *venture_commerce_connector_registry_lookup_factory(
	VentureCommerceConnectorRegistry *self, const gchar *name);

#define VENTURE_TYPE_SHOPIFY_CONNECTOR (venture_shopify_connector_get_type())
G_DECLARE_FINAL_TYPE(VentureShopifyConnector, venture_shopify_connector, VENTURE, SHOPIFY_CONNECTOR, GObject)
/**
 * venture_shopify_connector_new:
 * @shop: Shopify shop hostname
 * @token: access token
 * @transport: (nullable): injected transport; NULL selects HTTP
 *
 * Returns: (transfer full): owned result
 */
VentureCommerceConnector *venture_shopify_connector_new(const gchar *shop, const gchar *token,
	VentureBankFeedTransport *transport);

#define VENTURE_TYPE_COMMERCE_SERVICE (venture_commerce_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCommerceService, venture_commerce_service, VENTURE, COMMERCE_SERVICE, GObject)
/**
 * venture_commerce_service_new:
 * @database: database owning the records
 * @organization_id: target legal entity ID
 * @transport: (nullable): injected transport; NULL selects HTTP
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureCommerceService *venture_commerce_service_new(VentureDatabase *database, gint64 organization_id,
	VentureBankFeedTransport *transport, GError **error);
/**
 * venture_commerce_service_get_registry:
 * @self: the service or registry instance
 *
 * Returns: (transfer none): borrowed result
 */
VentureCommerceConnectorRegistry *venture_commerce_service_get_registry(VentureCommerceService *self);
/**
 * venture_commerce_service_import:
 * @self: the service or registry instance
 * @connector_name: (nullable): connector key; NULL selects Shopify
 * @from: (nullable): start of the requested window
 * @to: (nullable): end of the requested window
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: number processed, or -1 on failure
 */
gint venture_commerce_service_import(VentureCommerceService *self, const gchar *connector_name,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error);

/**
 * venture_commerce_service_import_for_organization:
 * @self: service with the stateless connector factory registry
 * @organization_id: target legal entity
 * @connector_name: (nullable): defaults to Shopify
 * @from: (nullable): start of the import window
 * @to: (nullable): end of the import window
 * @actor: (nullable): audit actor
 * @error: (out) (optional): validation, provider or persistence failure
 *
 * Constructs an account client from a registered factory and the selected
 * organization binding, then reuses the import transaction.
 * Returns: number imported, or -1 on failure
 */
gint venture_commerce_service_import_for_organization(VentureCommerceService *self,
	gint64 organization_id, const gchar *connector_name, GDateTime *from, GDateTime *to,
	const VentureActor *actor, GError **error);
/**
 * venture_commerce_service_configure_shopify:
 * @self: service
 * @organization_id: owning organization
 * @shop: canonical myshopify.com hostname
 * @token: write-only access token
 * @expected_connection: zero for connect, exact binding ID for rotation
 * @expected_version: zero for connect, current version for rotation
 * @actor: (nullable): audit actor
 * @error: (out) (optional): redacted failure
 * Returns: (transfer full) (nullable): encrypted account binding
 */
VentureIntegrationConnection *venture_commerce_service_configure_shopify(VentureCommerceService *self,
	gint64 organization_id, const gchar *shop, const gchar *token, gint64 expected_connection, gint64 expected_version,
	const VentureActor *actor, GError **error);
/**
 * venture_commerce_service_test_shopify:
 * @self: service
 * @organization_id: owning organization
 * @error: (out) (optional): redacted failure
 *
 * Fetches shop identity only, verifies the pinned account, and never imports.
 * Returns: whether the exact current binding authenticated to its account
 */
gboolean venture_commerce_service_test_shopify(VentureCommerceService *self,
	gint64 organization_id, GError **error);
/**
 * venture_commerce_service_disconnect:
 * @self: service
 * @organization_id: owning organization
 * @connection_id: exact binding
 * @expected_version: displayed version
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 * Returns: whether the account was disconnected
 */
gboolean venture_commerce_service_disconnect(VentureCommerceService *self,
	gint64 organization_id, gint64 connection_id, gint64 expected_version,
	const VentureActor *actor, GError **error);
/**
 * venture_commerce_service_adopt_invoice:
 * @self: service
 * @organization_id: owning organization
 * @connection_id: exact active account selected for adoption
 * @expected_version: selected binding version
 * @invoice_id: retained unbound legacy invoice
 * @reason: required audit reason
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Explicitly associates historical identity without modifying the invoice,
 * settlement, customer or ledger. Conflicting account claims are refused.
 * Returns: whether the historical identity was adopted
 */
gboolean venture_commerce_service_adopt_invoice(VentureCommerceService *self,
	gint64 organization_id, gint64 connection_id, gint64 expected_version, gint64 invoice_id,
	const gchar *reason, const VentureActor *actor, GError **error);
/**
 * venture_commerce_check_write:
 * @database: repository
 * @entity: proposed record
 * @removal: deletion or restore operation
 * @error: (out) (optional): refusal
 * Returns: whether the repository may apply this service-owned identity write
 */
gboolean venture_commerce_check_write(VentureDatabase *database, VentureEntity *entity,
	gboolean removal, GError **error);
/**
 * venture_commerce_actions_register:
 * @database: action registry owner
 *
 * Registers generic, audited account adoption without a separate write route.
 */
void venture_commerce_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
