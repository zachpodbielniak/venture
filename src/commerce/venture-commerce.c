/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

G_DEFINE_INTERFACE(VentureCommerceConnector, venture_commerce_connector, G_TYPE_OBJECT)
static void
venture_commerce_connector_default_init(VentureCommerceConnectorInterface *iface)
{
	(void)iface;
}
const gchar *
venture_commerce_connector_get_name(VentureCommerceConnector *self)
{
	g_return_val_if_fail(VENTURE_IS_COMMERCE_CONNECTOR(self), NULL);
	return VENTURE_COMMERCE_CONNECTOR_GET_IFACE(self)->get_name(self);
}
GPtrArray *
venture_commerce_connector_fetch_orders(VentureCommerceConnector *self, GDateTime *from, GDateTime *to, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_COMMERCE_CONNECTOR(self), NULL);
	if (NULL == VENTURE_COMMERCE_CONNECTOR_GET_IFACE(self)->fetch_orders)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED, "The commerce connector cannot fetch orders");
		return NULL;
	}
	return VENTURE_COMMERCE_CONNECTOR_GET_IFACE(self)->fetch_orders(self, from, to, error);
}

struct _VentureCommerceConnectorRegistry
{
	GObject parent_instance;
	GHashTable *connectors;
};
G_DEFINE_FINAL_TYPE(VentureCommerceConnectorRegistry, venture_commerce_connector_registry, G_TYPE_OBJECT)
static void
venture_commerce_connector_registry_finalize(GObject *object)
{
	g_hash_table_unref(VENTURE_COMMERCE_CONNECTOR_REGISTRY(object)->connectors);
	G_OBJECT_CLASS(venture_commerce_connector_registry_parent_class)->finalize(object);
}
static void
venture_commerce_connector_registry_class_init(VentureCommerceConnectorRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_commerce_connector_registry_finalize;
}
static void
venture_commerce_connector_registry_init(VentureCommerceConnectorRegistry *self)
{
	self->connectors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}
VentureCommerceConnectorRegistry *
venture_commerce_connector_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_COMMERCE_CONNECTOR_REGISTRY, NULL);
}
void
venture_commerce_connector_registry_add(VentureCommerceConnectorRegistry *self, VentureCommerceConnector *connector)
{
	const gchar *name;
	g_return_if_fail(VENTURE_IS_COMMERCE_CONNECTOR_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_COMMERCE_CONNECTOR(connector));
	name = venture_commerce_connector_get_name(connector);
	g_return_if_fail(NULL != name && '\0' != *name);
	g_hash_table_replace(self->connectors, g_strdup(name), connector);
}
VentureCommerceConnector *
venture_commerce_connector_registry_lookup(VentureCommerceConnectorRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_COMMERCE_CONNECTOR_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);
	return g_hash_table_lookup(self->connectors, name);
}
gboolean
venture_commerce_connector_registry_remove(VentureCommerceConnectorRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_COMMERCE_CONNECTOR_REGISTRY(self), FALSE);
	return g_hash_table_remove(self->connectors, name);
}
static gint
compare_connectors(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(venture_commerce_connector_get_name(*(VentureCommerceConnector *const *)a),
		venture_commerce_connector_get_name(*(VentureCommerceConnector *const *)b));
}
GPtrArray *
venture_commerce_connector_registry_list(VentureCommerceConnectorRegistry *self)
{
	GPtrArray *result = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, self->connectors);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(result, value);
	g_ptr_array_sort(result, compare_connectors);
	return result;
}

struct _VentureShopifyConnector
{
	GObject parent_instance;
	gchar *shop;
	gchar *token;
	VentureBankFeedTransport *transport;
};
static void shopify_iface(VentureCommerceConnectorInterface *iface);
G_DEFINE_TYPE_WITH_CODE(VentureShopifyConnector, venture_shopify_connector, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_COMMERCE_CONNECTOR, shopify_iface))

static gboolean
iso_currency(const gchar *code)
{
	return code != NULL && strlen(code) == 3 && g_ascii_isalpha(code[0]) &&
		g_ascii_isalpha(code[1]) && g_ascii_isalpha(code[2]);
}

static gboolean
shopify_cancelled(JsonObject *raw)
{
	JsonNode *node;
	const gchar *financial;
	if (json_object_has_member(raw, "cancelled_at"))
	{
		node = json_object_get_member(raw, "cancelled_at");
		if (node != NULL && !JSON_NODE_HOLDS_NULL(node))
		{
			const gchar *when = JSON_NODE_HOLDS_VALUE(node) ? json_node_get_string(node) : NULL;
			if (when != NULL && when[0] != '\0')
				return TRUE;
		}
	}
	financial = venture_json_object_get_string(raw, "financial_status", "");
	return g_strcmp0(financial, "voided") == 0 || g_strcmp0(financial, "refunded") == 0;
}

static const gchar *shopify_name(VentureCommerceConnector *self) { (void)self; return "shopify"; }
static GPtrArray *
shopify_fetch(VentureCommerceConnector *connector, GDateTime *from, GDateTime *to, GError **error)
{
	VentureShopifyConnector *self = VENTURE_SHOPIFY_CONNECTOR(connector);
	g_autofree gchar *start = NULL, *url = NULL, *auth = NULL, *body = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root, *orders_node;
	JsonArray *orders;
	GPtrArray *items;
	guint i;
	if (self->transport == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
			"Shopify connector requires an injected transport");
		return NULL;
	}
	start = from != NULL ? g_date_time_format(from, "%Y-%m-%d") : g_strdup("");
	url = g_strdup_printf("https://%s/admin/api/2024-01/orders.json?status=any&created_at_min=%s",
		self->shop != NULL && *self->shop ? self->shop : "example.myshopify.com", start);
	auth = g_strdup_printf("X-Shopify-Access-Token: %s", self->token);
	(void)to;
	body = venture_bank_feed_transport_get(self->transport, url, auth, error);
	if (body == NULL) return NULL;
	if (!json_parser_load_from_data(parser, body, -1, error)) return NULL;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Shopify response must be a JSON object");
		return NULL;
	}
	orders_node = json_object_get_member(json_node_get_object(root), "orders");
	if (orders_node == NULL || !JSON_NODE_HOLDS_ARRAY(orders_node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Shopify response needs orders");
		return NULL;
	}
	orders = json_node_get_array(orders_node);
	items = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
	for (i = 0; i < json_array_get_length(orders); i++)
	{
		JsonObject *raw = json_array_get_object_element(orders, i);
		JsonObject *item;
		JsonArray *lines, *raw_lines;
		const gchar *id, *financial, *currency;
		guint l;
		gboolean send;
		if (raw == NULL) continue;
		if (shopify_cancelled(raw))
			continue;
		currency = venture_json_object_get_string(raw, "currency", NULL);
		if (!iso_currency(currency))
			currency = venture_json_object_get_string(raw, "presentment_currency", NULL);
		if (!iso_currency(currency))
		{
			g_ptr_array_unref(items);
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Shopify order is missing a currency");
			return NULL;
		}
		id = venture_json_object_get_string(raw, "id", NULL);
		item = json_object_new();
		if (id == NULL)
		{
			gint64 number = json_object_get_int_member(raw, "id");
			g_autofree gchar *external = g_strdup_printf("shopify:%" G_GINT64_FORMAT, number);
			json_object_set_string_member(item, "external_id", external);
		}
		else
		{
			g_autofree gchar *external = g_strdup_printf("shopify:%s", id);
			json_object_set_string_member(item, "external_id", external);
		}
		financial = venture_json_object_get_string(raw, "financial_status", "");
		json_object_set_boolean_member(item, "paid", g_strcmp0(financial, "paid") == 0);
		send = g_strcmp0(financial, "paid") == 0 || g_strcmp0(financial, "pending") == 0 ||
			g_strcmp0(financial, "authorized") == 0 || g_strcmp0(financial, "partially_paid") == 0 ||
			g_strcmp0(financial, "unpaid") == 0 || financial[0] == '\0';
		json_object_set_boolean_member(item, "send", send);
		if (json_object_has_member(raw, "customer") &&
			JSON_NODE_HOLDS_OBJECT(json_object_get_member(raw, "customer")))
		{
			JsonObject *customer = json_object_get_object_member(raw, "customer");
			const gchar *email = venture_json_object_get_string(customer, "email", NULL);
			const gchar *first = venture_json_object_get_string(customer, "first_name", "");
			const gchar *last = venture_json_object_get_string(customer, "last_name", "");
			const gchar *cid = venture_json_object_get_string(customer, "id", NULL);
			g_autofree gchar *name = NULL;
			g_autofree gchar *external = NULL;
			if (cid == NULL && json_object_has_member(customer, "id"))
			{
				gint64 number = json_object_get_int_member(customer, "id");
				external = g_strdup_printf("shopify:customer:%" G_GINT64_FORMAT, number);
			}
			else if (cid != NULL)
				external = g_strdup_printf("shopify:customer:%s", cid);
			if (first[0] != '\0' || last[0] != '\0')
				name = g_strconcat(first, first[0] && last[0] ? " " : "", last, NULL);
			if (external != NULL)
				json_object_set_string_member(item, "customer_external_id", external);
			if (email != NULL && email[0] != '\0')
				json_object_set_string_member(item, "customer_email", email);
			if (name != NULL && name[0] != '\0')
				json_object_set_string_member(item, "customer_name", name);
			else if (email != NULL)
				json_object_set_string_member(item, "customer_name", email);
		}
		lines = json_array_new();
		raw_lines = json_object_has_member(raw, "line_items") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(raw, "line_items"))
			? json_object_get_array_member(raw, "line_items") : NULL;
		if (raw_lines != NULL)
		{
			for (l = 0; l < json_array_get_length(raw_lines); l++)
			{
				JsonObject *src = json_array_get_object_element(raw_lines, l);
				JsonObject *line = json_object_new();
				const gchar *title = venture_json_object_get_string(src, "title", "Order");
				const gchar *price = venture_json_object_get_string(src, "price", "0");
				gint64 quantity = json_object_get_int_member(src, "quantity");
				g_autofree gchar *unit = NULL;
				json_object_set_string_member(line, "description", title);
				json_object_set_int_member(line, "quantity", quantity > 0 ? quantity : 1);
				unit = strstr(price, " ") ? g_strdup(price) : g_strdup_printf("%s %s", price, currency);
				json_object_set_string_member(line, "unit_price", unit);
				json_array_add_object_element(lines, line);
			}
		}
		json_object_set_array_member(item, "lines", lines);
		g_ptr_array_add(items, item);
	}
	return items;
}
static void shopify_iface(VentureCommerceConnectorInterface *iface)
{
	iface->get_name = shopify_name;
	iface->fetch_orders = shopify_fetch;
}
static void
venture_shopify_connector_finalize(GObject *object)
{
	VentureShopifyConnector *self = VENTURE_SHOPIFY_CONNECTOR(object);
	g_free(self->shop);
	if (self->token != NULL)
	{
		memset(self->token, 0, strlen(self->token));
		g_clear_pointer(&self->token, g_free);
	}
	g_clear_object(&self->transport);
	G_OBJECT_CLASS(venture_shopify_connector_parent_class)->finalize(object);
}
static void
venture_shopify_connector_class_init(VentureShopifyConnectorClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_shopify_connector_finalize;
}
static void venture_shopify_connector_init(VentureShopifyConnector *self) { (void)self; }
VentureCommerceConnector *
venture_shopify_connector_new(const gchar *shop, const gchar *token, VentureBankFeedTransport *transport)
{
	VentureShopifyConnector *self = g_object_new(VENTURE_TYPE_SHOPIFY_CONNECTOR, NULL);
	self->shop = g_strdup(shop);
	self->token = g_strdup(token);
	self->transport = transport != NULL ? g_object_ref(transport) : NULL;
	return VENTURE_COMMERCE_CONNECTOR(self);
}

struct _VentureCommerceService
{
	GObject parent_instance;
	VentureDatabase *database;
	gint64 organization_id;
	VentureCommerceConnectorRegistry *registry;
};
G_DEFINE_TYPE(VentureCommerceService, venture_commerce_service, G_TYPE_OBJECT)
static void
venture_commerce_service_finalize(GObject *object)
{
	VentureCommerceService *self = VENTURE_COMMERCE_SERVICE(object);
	g_clear_object(&self->registry);
	g_clear_object(&self->database);
	G_OBJECT_CLASS(venture_commerce_service_parent_class)->finalize(object);
}
static void
venture_commerce_service_class_init(VentureCommerceServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_commerce_service_finalize;
}
static void venture_commerce_service_init(VentureCommerceService *self) { (void)self; }

VentureCommerceService *
venture_commerce_service_new(VentureDatabase *database, gint64 organization_id,
	VentureBankFeedTransport *transport, GError **error)
{
	g_autoptr(VentureCommerceService) self = NULL;
	const gchar *token = g_getenv("VENTURE_COMMERCE_SHOPIFY_TOKEN");
	const gchar *shop = g_getenv("VENTURE_COMMERCE_SHOPIFY_SHOP");
	if (token == NULL || *token == '\0')
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			"Commerce module requires VENTURE_COMMERCE_SHOPIFY_TOKEN");
		return NULL;
	}
	self = g_object_new(VENTURE_TYPE_COMMERCE_SERVICE, NULL);
	self->database = g_object_ref(database);
	self->organization_id = organization_id;
	self->registry = venture_commerce_connector_registry_new();
	venture_commerce_connector_registry_add(self->registry,
		venture_shopify_connector_new(shop, token, transport));
	return g_steal_pointer(&self);
}

VentureCommerceConnectorRegistry *
venture_commerce_service_get_registry(VentureCommerceService *self)
{
	g_return_val_if_fail(VENTURE_IS_COMMERCE_SERVICE(self), NULL);
	return self->registry;
}


static gint64
find_company_by(VentureCommerceService *self, const gchar *field, const gchar *value, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) row = NULL;
	if (venture_string_is_empty(value))
		return 0;
	query = venture_query_new(VENTURE_TYPE_COMPANY);
	venture_query_set_organization(query, self->organization_id);
	if (!venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, error))
		return 0;
	row = venture_database_find_one(self->database, query, error);
	if (row == NULL)
		return error != NULL && *error != NULL ? -1 : 0;
	return venture_entity_get_id(row);
}

static gboolean
ensure_company(VentureCommerceService *self, JsonObject *spec, GError **error)
{
	gint64 company_id = venture_json_object_get_int(spec, "company_id", 0);
	const gchar *external = venture_json_object_get_string(spec, "customer_external_id", NULL);
	const gchar *email = venture_json_object_get_string(spec, "customer_email", NULL);
	const gchar *name = venture_json_object_get_string(spec, "customer_name", NULL);
	g_autoptr(VentureEntity) company = NULL;
	if (company_id > 0)
		return TRUE;
	company_id = find_company_by(self, "external-id", external, error);
	if (company_id < 0)
		return FALSE;
	if (company_id == 0)
		company_id = find_company_by(self, "email", email, error);
	if (company_id < 0)
		return FALSE;
	if (company_id > 0)
	{
		json_object_set_int_member(spec, "company_id", company_id);
		return TRUE;
	}
	if (venture_string_is_empty(external) && venture_string_is_empty(email))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Commerce import needs a customer or company_id");
		return FALSE;
	}
	company = VENTURE_ENTITY(venture_company_new());
	venture_entity_set_organization_id(company, self->organization_id);
	g_object_set(company, "name", name != NULL && name[0] != '\0' ? name : (email != NULL ? email : external),
		"kind", VENTURE_COMPANY_KIND_CUSTOMER, "email", email, "external-id", external,
		"source", "shopify", "active", TRUE, NULL);
	if (!venture_database_save(self->database, company, NULL, error))
		return FALSE;
	json_object_set_int_member(spec, "company_id", venture_entity_get_id(company));
	return TRUE;
}

static VentureEntity *
find_invoice(VentureCommerceService *self, const gchar *external_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) rows = NULL;
	if (venture_string_is_empty(external_id)) return NULL;
	venture_query_set_organization(query, self->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, external_id, error))
		return NULL;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL) return NULL;
	if (rows->len == 0) return NULL;
	return g_object_ref(g_ptr_array_index(rows, 0));
}

gint
venture_commerce_service_import(VentureCommerceService *self, const gchar *connector_name,
	GDateTime *from, GDateTime *to, const VentureActor *actor, GError **error)
{
	VentureCommerceConnector *connector;
	g_autoptr(GPtrArray) orders = NULL;
	guint i;
	gint imported = 0;
	g_return_val_if_fail(VENTURE_IS_COMMERCE_SERVICE(self), -1);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "invoice") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Commerce module is disabled (commerce.enabled)");
		return -1;
	}
	connector = venture_commerce_connector_registry_lookup(self->registry,
		connector_name != NULL ? connector_name : "shopify");
	if (connector == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No commerce connector named %s",
			connector_name != NULL ? connector_name : "shopify");
		return -1;
	}
	orders = venture_commerce_connector_fetch_orders(connector, from, to, error);
	if (orders == NULL) return -1;
	if (!venture_database_begin(self->database, error)) return -1;
	for (i = 0; i < orders->len; i++)
	{
		JsonObject *spec = g_ptr_array_index(orders, i);
		const gchar *external = venture_json_object_get_string(spec, "external_id", NULL);
		g_autoptr(VentureEntity) existing = NULL;
		g_autoptr(VentureEntity) invoice = NULL;
		existing = find_invoice(self, external, error);
		if (error && *error) goto fail;
		if (existing != NULL) continue;
		if (!ensure_company(self, spec, error))
			goto fail;
		if (!json_object_has_member(spec, "send"))
			json_object_set_boolean_member(spec, "send", TRUE);
		invoice = venture_document_service_compose_invoice(venture_document_service_get(self->database),
			self->organization_id, spec, actor, error);
		if (invoice == NULL) goto fail;
		if (venture_json_object_get_bool(spec, "paid", FALSE))
		{
			g_autoptr(GDateTime) now = venture_time_now();
			if (!venture_settlement_service_settle_invoice(venture_settlement_service_get(self->database),
				venture_entity_get_id(invoice), now, actor, error))
				goto fail;
		}
		imported++;
	}
	if (!venture_database_commit(self->database, error)) return -1;
	return imported;
fail:
	venture_database_rollback(self->database);
	return -1;
}
