/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included after the service definition: account clients are operation-owned,
 * while the public registry owns only reusable, credential-free factories. */
static gboolean
commerce_shopify_settings(const gchar *shop, const gchar *token, GError **error)
{
	const gchar *suffix = ".myshopify.com";
	gsize length, i;
	gboolean valid = shop != NULL && token != NULL;
	length = shop ? strlen(shop) : 0;
	if (valid)
		for (i = 0; token[i] != '\0'; i++) valid = valid && g_ascii_isgraph(token[i]);
	valid = valid && length > strlen(suffix) && length <= 63 + strlen(suffix) &&
		g_str_has_suffix(shop, suffix) && strlen(token) > 0 && strlen(token) <= 4096 &&
		strchr(token, '\r') == NULL && strchr(token, '\n') == NULL;
	if (valid)
	{
		gsize label = length - strlen(suffix);
		valid = g_ascii_isalnum(shop[0]) && g_ascii_isalnum(shop[label - 1]);
		for (i = 0; i < label; i++)
			valid = valid && ((shop[i] >= 'a' && shop[i] <= 'z') || g_ascii_isdigit(shop[i]) || shop[i] == '-');
	}
	if (!valid)
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Commerce requires a canonical myshopify.com shop hostname and a bounded, single-line access token");
	return valid;
}

typedef struct { GObject parent; } CommerceShopifyFactory;
typedef struct { GObjectClass parent; } CommerceShopifyFactoryClass;
GType commerce_shopify_factory_get_type(void);
static void commerce_shopify_factory_iface(VentureCommerceConnectorFactoryInterface *iface);
G_DEFINE_TYPE_WITH_CODE(CommerceShopifyFactory, commerce_shopify_factory, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_COMMERCE_CONNECTOR_FACTORY, commerce_shopify_factory_iface))
static void commerce_shopify_factory_init(CommerceShopifyFactory *self) { (void)self; }
static void commerce_shopify_factory_class_init(CommerceShopifyFactoryClass *klass) { (void)klass; }
static const gchar *commerce_shopify_factory_name(VentureCommerceConnectorFactory *self) { (void)self; return "shopify"; }
static VentureCommerceConnector *
commerce_shopify_factory_create(VentureCommerceConnectorFactory *self, const gchar *account,
	JsonNode *settings, VentureBankFeedTransport *transport, GError **error)
{
	JsonObject *object;
	const gchar *shop, *token;
	(void)self;
	if (settings == NULL || !JSON_NODE_HOLDS_OBJECT(settings))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Commerce account settings are unavailable");
		return NULL;
	}
	object = json_node_get_object(settings);
	shop = venture_json_object_get_string(object, "shop", NULL);
	token = venture_json_object_get_string(object, "access_token", NULL);
	if (!commerce_shopify_settings(shop, token, error)) return NULL;
	if (g_strcmp0(account, shop) != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Commerce credentials do not match the selected account");
		return NULL;
	}
	return venture_shopify_connector_new(shop, token, transport);
}
static void commerce_shopify_factory_iface(VentureCommerceConnectorFactoryInterface *iface)
{
	iface->get_name = commerce_shopify_factory_name;
	iface->create = commerce_shopify_factory_create;
}
static VentureCommerceConnectorFactory *commerce_shopify_factory_new(void)
{
	return g_object_new(commerce_shopify_factory_get_type(), NULL);
}

typedef struct
{
	GObject parent;
	VentureDatabase *database;
	VentureBankFeedTransport *transport;
	GThread *owner;
	VentureAuthPrincipal *principal;
	gint64 organization_id, connection_id, version;
} CommerceGuardTransport;
typedef struct { GObjectClass parent; } CommerceGuardTransportClass;
GType commerce_guard_transport_get_type(void);
static void commerce_guard_transport_iface(VentureBankFeedTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(CommerceGuardTransport, commerce_guard_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_BANK_FEED_TRANSPORT, commerce_guard_transport_iface))
static gboolean
commerce_guard_check(CommerceGuardTransport *self, GError **error)
{
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	if (g_thread_self() != self->owner)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "Commerce account checks require the originating main thread");
		return FALSE;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "commerce_import_link") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Commerce module was disabled");
		return FALSE;
	}
	scope = venture_access_policy_enter_organization(venture_database_get_access_policy(self->database),
		self->principal, self->organization_id);
	settings = venture_integration_service_resolve_version(
		venture_integration_service_get(self->database), self->organization_id,
		self->connection_id, self->version, FALSE, error);
	return settings != NULL;
}
static gchar *
commerce_guard_get(VentureBankFeedTransport *transport, const gchar *url, const gchar *authorization, GError **error)
{
	CommerceGuardTransport *self = (CommerceGuardTransport *)transport;
	g_autofree gchar *response = NULL;
	if (!commerce_guard_check(self, error)) return NULL;
	response = venture_bank_feed_transport_get(self->transport, url, authorization, error);
	if (response == NULL || !commerce_guard_check(self, error)) return NULL;
	return g_steal_pointer(&response);
}
static void
commerce_guard_async_done(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr(GTask) task = data;
	CommerceGuardTransport *self = g_task_get_source_object(task);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = venture_bank_feed_transport_get_finish(VENTURE_BANK_FEED_TRANSPORT(source), result, &error);
	if (g_task_return_error_if_cancelled(task)) return;
	if (body != NULL && commerce_guard_check(self, &error))
		g_task_return_pointer(task, g_steal_pointer(&body), g_free);
	else
	{
		if (error == NULL) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Commerce transport returned no response");
		g_task_return_error(task, g_steal_pointer(&error));
	}
}
static void
commerce_guard_get_async(VentureBankFeedTransport *transport, const gchar *url, const gchar *authorization,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	CommerceGuardTransport *self = (CommerceGuardTransport *)transport;
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	g_autoptr(GError) error = NULL;
	if (g_task_return_error_if_cancelled(task)) return;
	if (!commerce_guard_check(self, &error))
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	/* The underlying transport may move byte I/O to a worker. The vault
	 * checks stay before dispatch and on this context's completion callback. */
	venture_bank_feed_transport_get_async(self->transport, url, authorization, cancellable,
		commerce_guard_async_done, g_steal_pointer(&task));
}
static void commerce_guard_transport_iface(VentureBankFeedTransportInterface *iface)
{
	iface->get = commerce_guard_get;
	iface->get_async = commerce_guard_get_async;
}
static void commerce_guard_transport_init(CommerceGuardTransport *self) { (void)self; }
static void commerce_guard_transport_finalize(GObject *object)
{
	CommerceGuardTransport *self = (CommerceGuardTransport *)object;
	g_clear_object(&self->database);
	g_clear_object(&self->transport);
	g_clear_pointer(&self->owner, g_thread_unref);
	g_clear_pointer(&self->principal, venture_auth_principal_free);
	G_OBJECT_CLASS(commerce_guard_transport_parent_class)->finalize(object);
}
static void commerce_guard_transport_class_init(CommerceGuardTransportClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = commerce_guard_transport_finalize;
}
static VentureBankFeedTransport *
commerce_guard_new(VentureCommerceService *self, VentureIntegrationConnection *binding)
{
	CommerceGuardTransport *guard = g_object_new(commerce_guard_transport_get_type(), NULL);
	guard->database = g_object_ref(self->database);
	guard->owner = g_thread_ref(g_thread_self());
	guard->principal = venture_auth_principal_copy(venture_access_policy_get_actor(venture_database_get_access_policy(self->database)));
	guard->transport = g_object_ref(self->transport);
	guard->organization_id = venture_entity_get_organization_id(VENTURE_ENTITY(binding));
	guard->connection_id = venture_entity_get_id(VENTURE_ENTITY(binding));
	guard->version = venture_entity_get_version(VENTURE_ENTITY(binding));
	return VENTURE_BANK_FEED_TRANSPORT(guard);
}
static VentureIntegrationConnection *
commerce_binding(VentureCommerceService *self, gint64 organization_id, const gchar *name, GError **error)
{
	g_autofree gchar *provider = g_strconcat("commerce.", name, NULL);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "commerce_import_link") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Commerce module is disabled");
		return NULL;
	}
	return venture_integration_service_find(venture_integration_service_get(self->database), organization_id, provider, error);
}
VentureIntegrationConnection *
venture_commerce_service_configure_shopify(VentureCommerceService *self, gint64 organization_id,
	const gchar *shop, const gchar *token, gint64 expected_connection, gint64 expected_version, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonNode) settings = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	g_autoptr(VentureIntegrationConnection) current = NULL, result = NULL;
	json_node_take_object(settings, object);
	g_return_val_if_fail(VENTURE_IS_COMMERCE_SERVICE(self), NULL);
	if (!commerce_shopify_settings(shop, token, error)) return NULL;
	if (expected_connection < 0 || expected_version < 0 || ((expected_connection == 0) != (expected_version == 0)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Connect requires zero identity and version; rotation requires both positive values");
		return NULL;
	}
	json_object_set_string_member(object, "shop", shop);
	json_object_set_string_member(object, "access_token", token);
	if (!venture_database_begin(self->database, error)) return NULL;
	if (expected_connection > 0 || expected_version > 0)
	{
		current = commerce_binding(self, organization_id, "shopify", error);
		if (current == NULL) goto fail;
		if (venture_entity_get_id(VENTURE_ENTITY(current)) != expected_connection ||
			venture_entity_get_version(VENTURE_ENTITY(current)) != expected_version)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Commerce account changed; reload settings before rotating");
			goto fail;
		}
	}
	result = venture_integration_service_configure(venture_integration_service_get(self->database),
		organization_id, "commerce.shopify", shop, "live", settings, expected_version, actor, error);
	if (result == NULL) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&result);
fail:
	venture_database_rollback(self->database);
	return NULL;
}
gboolean
venture_commerce_service_disconnect(VentureCommerceService *self, gint64 organization_id,
	gint64 connection_id, gint64 expected_version, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) binding = venture_database_get(self->database, VENTURE_TYPE_INTEGRATION_CONNECTION, connection_id, error);
	g_autofree gchar *provider = NULL;
	if (binding == NULL) return FALSE;
	g_object_get(binding, "provider", &provider, NULL);
	if (venture_entity_get_organization_id(binding) != organization_id || !g_str_has_prefix(provider ? provider : "", "commerce."))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Commerce account is unavailable");
		return FALSE;
	}
	return venture_integration_service_disable(venture_integration_service_get(self->database),
		organization_id, connection_id, expected_version, actor, error);
}
gboolean
venture_commerce_service_test_shopify(VentureCommerceService *self, gint64 organization_id, GError **error)
{
	g_autoptr(VentureIntegrationConnection) binding = commerce_binding(self, organization_id, "shopify", error);
	g_autoptr(JsonNode) settings = NULL, response = NULL;
	g_autoptr(VentureBankFeedTransport) guard = NULL;
	g_autofree gchar *account = NULL, *url = NULL, *auth = NULL, *body = NULL;
	JsonObject *object, *shop;
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy = venture_database_get_access_policy(self->database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	if (binding == NULL) return FALSE;
	if (principal && !venture_access_policy_has_organization_role(policy, principal, organization_id, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization integration administration is required to test an account");
		return FALSE;
	}
	settings = venture_integration_service_resolve_version(venture_integration_service_get(self->database), organization_id,
		venture_entity_get_id(VENTURE_ENTITY(binding)), venture_entity_get_version(VENTURE_ENTITY(binding)), FALSE, error);
	if (settings == NULL) return FALSE;
	object = json_node_get_object(settings);
	g_object_get(binding, "account-id", &account, NULL);
	if (!commerce_shopify_settings(account, venture_json_object_get_string(object, "access_token", NULL), error)) return FALSE;
	guard = commerce_guard_new(self, binding);
	url = g_strdup_printf("https://%s/admin/api/2025-10/shop.json", account);
	auth = g_strdup_printf("X-Shopify-Access-Token: %s", venture_json_object_get_string(object, "access_token", ""));
	body = venture_bank_feed_transport_get(guard, url, auth, error);
	if (body == NULL) return FALSE;
	response = venture_json_parse(body, NULL);
	if (response == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Shopify returned invalid shop JSON");
		return FALSE;
	}
	object = JSON_NODE_HOLDS_OBJECT(response) ? json_node_get_object(response) : NULL;
	shop = object && json_object_has_member(object, "shop") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "shop")) ?
		json_object_get_object_member(object, "shop") : NULL;
	if (shop == NULL || venture_json_object_get_int(shop, "id", 0) <= 0 ||
		g_strcmp0(venture_json_object_get_string(shop, "myshopify_domain", NULL), account) != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Shopify returned a different or invalid account identity");
		return FALSE;
	}
	return TRUE;
}
