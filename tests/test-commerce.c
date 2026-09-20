/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Covers src/commerce: Shopify-like connectors import orders as invoices. */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
} Fixture;

typedef struct { GObject parent; GPtrArray *orders; gboolean async_probe; VentureBankFeedTransport *transport; } FakeConnector;
typedef struct { GObjectClass parent; } FakeConnectorClass;
GType fake_connector_get_type(void);
static void fake_connector_iface(VentureCommerceConnectorInterface *iface);
static void fake_factory_iface(VentureCommerceConnectorFactoryInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeConnector, fake_connector, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_COMMERCE_CONNECTOR, fake_connector_iface)
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_COMMERCE_CONNECTOR_FACTORY, fake_factory_iface))
static const gchar *fake_connector_name(VentureCommerceConnector *self)
{
	(void)self;
	return "fake";
}
typedef struct { gboolean done; gchar *body; GError *error; } FakeAsyncWait;
static void fake_async_done(GObject *source, GAsyncResult *result, gpointer data)
{
	FakeAsyncWait *wait = data;
	wait->body = venture_bank_feed_transport_get_finish(VENTURE_BANK_FEED_TRANSPORT(source), result, &wait->error);
	wait->done = TRUE;
}
static GPtrArray *
fake_connector_fetch(VentureCommerceConnector *connector, GDateTime *from, GDateTime *to, GError **error)
{
	FakeConnector *self = (FakeConnector *)connector;
	GPtrArray *copy = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
	guint i;
	(void)from; (void)to;
	if (self->async_probe)
	{
		FakeAsyncWait wait = { FALSE, NULL, NULL };
		venture_bank_feed_transport_get_async(self->transport, "https://fixture.invalid/orders", "synthetic", NULL, fake_async_done, &wait);
		while (!wait.done) g_main_context_iteration(NULL, TRUE);
		g_free(wait.body);
		if (wait.error != NULL)
		{
			g_propagate_error(error, wait.error);
			g_ptr_array_unref(copy);
			return NULL;
		}
	}
	for (i = 0; i < self->orders->len; i++)
		g_ptr_array_add(copy, json_object_ref(g_ptr_array_index(self->orders, i)));
	return copy;
}
static void fake_connector_iface(VentureCommerceConnectorInterface *iface)
{
	iface->get_name = fake_connector_name;
	iface->fetch_orders = fake_connector_fetch;
}
static void fake_connector_finalize(GObject *object)
{
	g_clear_pointer(&((FakeConnector *)object)->orders, g_ptr_array_unref);
	g_clear_object(&((FakeConnector *)object)->transport);
	G_OBJECT_CLASS(fake_connector_parent_class)->finalize(object);
}
static void fake_connector_class_init(FakeConnectorClass *klass) { G_OBJECT_CLASS(klass)->finalize = fake_connector_finalize; }
static void fake_connector_init(FakeConnector *self)
{
	self->orders = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
}

static const gchar *fake_factory_name(VentureCommerceConnectorFactory *self) { (void)self; return "fake"; }
static VentureCommerceConnector *fake_factory_create(VentureCommerceConnectorFactory *factory, const gchar *account,
	JsonNode *settings, VentureBankFeedTransport *transport, GError **error)
{
	FakeConnector *source = (FakeConnector *)factory;
	FakeConnector *client = g_object_new(fake_connector_get_type(), NULL);
	guint i;
	(void)account; (void)settings; (void)error;
	client->async_probe = source->async_probe;
	client->transport = g_object_ref(transport);
	for (i = 0; i < source->orders->len; i++)
	{
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT), copy = NULL;
		g_autofree gchar *wire = NULL;
		json_node_set_object(node, g_ptr_array_index(source->orders, i));
		wire = venture_json_to_string(node, FALSE);
		copy = venture_json_parse(wire, NULL);
		g_ptr_array_add(client->orders, json_object_ref(json_node_get_object(copy)));
	}
	return VENTURE_COMMERCE_CONNECTOR(client);
}
static void fake_factory_iface(VentureCommerceConnectorFactoryInterface *iface)
{
	iface->get_name = fake_factory_name;
	iface->create = fake_factory_create;
}
static void configure_fake(Fixture *f, gint64 org)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) settings = venture_json_parse("{}", NULL);
	g_autoptr(VentureIntegrationConnection) binding = venture_integration_service_configure(
		venture_integration_service_get(f->db), org, "commerce.fake", "fake-account", "test", settings, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(binding);
}

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, record, NULL, &error));
	g_assert_no_error(error);
}

static JsonObject *
order(Fixture *f, const gchar *id, const gchar *description, const gchar *price, gboolean paid)
{
	JsonObject *object = json_object_new();
	JsonArray *lines = json_array_new();
	JsonObject *line = json_object_new();
	json_object_set_string_member(object, "external_id", id);
	json_object_set_int_member(object, "company_id", f->company);
	json_object_set_boolean_member(object, "paid", paid);
	json_object_set_string_member(line, "description", description);
	json_object_set_int_member(line, "quantity", 1);
	json_object_set_string_member(line, "unit_price", price);
	json_array_add_object_element(lines, line);
	json_object_set_array_member(object, "lines", lines);
	return object;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCompany) company = NULL;
	(void)data;
	f->config = venture_config_new();
	g_object_set(f->config, "commerce-enabled", TRUE, NULL);
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	{
		g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
		g_autoptr(VentureCommerceService) service = venture_commerce_service_new(f->db, f->org, NULL, &error);
		g_autoptr(VentureIntegrationConnection) binding = NULL;
		g_assert_true(venture_integration_service_set_key(venture_integration_service_get(f->db), key, &error));
		binding = venture_commerce_service_configure_shopify(service, f->org, "shop.myshopify.com", "test-token", 0, 0, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(binding);
	}
	configure_fake(f, f->org);
	company = venture_company_new();
	g_object_set(company, "name", "Shop customer", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(company), f->org);
	save(f, VENTURE_ENTITY(company));
	f->company = venture_entity_get_id(VENTURE_ENTITY(company));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static guint
count_type(Fixture *f, GType type)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, f->org);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows->len;
}

static void
test_missing_key(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureCommerceService) service = NULL;

	/* Ambient secrets must never select an organization account. */
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	service = venture_commerce_service_new(db, 1, NULL, &error);
	g_assert_nonnull(service);
	g_assert_no_error(error);
	g_assert_null(venture_commerce_connector_registry_lookup(venture_commerce_service_get_registry(service), "shopify"));
}

static void
test_idempotent_import(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	FakeConnector *connector;
	gint imported;
	(void)data;
	service = venture_commerce_service_new(f->db, f->org, NULL, &error);
	g_assert_no_error(error);
	connector = g_object_new(fake_connector_get_type(), NULL);
	g_ptr_array_add(connector->orders, order(f, "shopify:1001", "Hat", "25 USD", TRUE));
	venture_commerce_connector_registry_add_factory(venture_commerce_service_get_registry(service),
		VENTURE_COMMERCE_CONNECTOR_FACTORY(connector));
	imported = venture_commerce_service_import(service, "fake", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_SALE), >=, 1);
	imported = venture_commerce_service_import(service, "fake", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 1);
}

static void
test_failure_rolls_back(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	FakeConnector *connector;
	gint imported;
	(void)data;
	service = venture_commerce_service_new(f->db, f->org, NULL, &error);
	g_assert_no_error(error);
	connector = g_object_new(fake_connector_get_type(), NULL);
	g_ptr_array_add(connector->orders, order(f, "shopify:1002", "Mug", "10 USD", FALSE));
	{
		JsonObject *bad = json_object_new();
		json_object_set_string_member(bad, "external_id", "shopify:bad");
		json_object_set_int_member(bad, "company_id", f->company);
		g_ptr_array_add(connector->orders, bad);
	}
	venture_commerce_connector_registry_add_factory(venture_commerce_service_get_registry(service),
		VENTURE_COMMERCE_CONNECTOR_FACTORY(connector));
	imported = venture_commerce_service_import(service, "fake", NULL, NULL, NULL, &error);
	g_assert_cmpint(imported, <, 0);
	g_assert_nonnull(error);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMMERCE_IMPORT_LINK), ==, 0);
}

typedef struct { GObject parent; gchar *body; gchar *next; gchar *url; gchar *authorization; guint calls; void (*during_fetch)(gpointer); gpointer callback_data; } FakeTransport;
typedef struct { GObjectClass parent; } FakeTransportClass;
GType fake_transport_get_type(void);
static void fake_transport_iface(VentureBankFeedTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeTransport, fake_transport, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_BANK_FEED_TRANSPORT, fake_transport_iface))
static gchar *
fake_transport_get(VentureBankFeedTransport *transport, const gchar *url,
	const gchar *authorization, GError **error)
{
	FakeTransport *self = (FakeTransport *)transport;
	g_free(self->url);
	self->url = g_strdup(url);
	self->calls++;
	g_free(self->authorization);
	self->authorization = g_strdup(authorization);
	if (self->during_fetch != NULL) self->during_fetch(self->callback_data);
	(void)error;
	return g_strdup(self->calls > 1 && self->next != NULL ? self->next :
		self->body != NULL ? self->body : "{\"orders\":[]}");
}
static void fake_transport_iface(VentureBankFeedTransportInterface *iface)
{
	iface->get = fake_transport_get;
}
static void fake_transport_finalize(GObject *object)
{
	g_free(((FakeTransport *)object)->body);
	g_free(((FakeTransport *)object)->next);
	g_free(((FakeTransport *)object)->url);
	g_free(((FakeTransport *)object)->authorization);
	G_OBJECT_CLASS(fake_transport_parent_class)->finalize(object);
}
static void fake_transport_class_init(FakeTransportClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = fake_transport_finalize;
}
static void fake_transport_init(FakeTransport *self) { (void)self; }

static void
test_shopify_currency_and_cancelled(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	FakeTransport *transport;
	gint imported;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) companies = NULL;
	(void)data;
	transport = g_object_new(fake_transport_get_type(), NULL);
	transport->body = g_strdup("{\"orders\":["
		"{\"id\":2001,\"currency\":\"EUR\",\"financial_status\":\"paid\","
		"\"customer\":{\"id\":55,\"email\":\"buyer@example.com\",\"first_name\":\"Ada\",\"last_name\":\"Lovelace\"},"
		"\"line_items\":[{\"title\":\"Hat\",\"quantity\":1,\"price\":\"25.00\"}]},"
		"{\"id\":2002,\"currency\":\"EUR\",\"financial_status\":\"voided\",\"cancelled_at\":\"2026-08-01T00:00:00Z\","
		"\"customer\":{\"id\":56,\"email\":\"skip@example.com\"},"
		"\"line_items\":[{\"title\":\"Skip\",\"quantity\":1,\"price\":\"9.00\"}]}"
		"]}");
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	imported = venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 1);
	query = venture_query_new(VENTURE_TYPE_INVOICE);
	venture_query_set_organization(query, f->org);
	invoices = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(invoices->len, ==, 1);
	{
		gint64 company_id = 0;
		g_object_get(g_ptr_array_index(invoices, 0), "company-id", &company_id, NULL);
		g_assert_cmpint(company_id, >, 0);
		g_assert_cmpint(company_id, !=, f->company);
	}
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_COMPANY);
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_EQ, "buyer@example.com", NULL);
	companies = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(companies->len, ==, 1);
	g_object_unref(transport);
}

static void
test_shopify_refuses_bare_usd(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceConnector) connector = NULL;
	g_autoptr(GPtrArray) orders = NULL;
	FakeTransport *transport;
	(void)data;
	transport = g_object_new(fake_transport_get_type(), NULL);
	transport->body = g_strdup("{\"orders\":[{\"id\":9,\"financial_status\":\"paid\","
		"\"line_items\":[{\"title\":\"Hat\",\"quantity\":1,\"price\":\"25.00\"}]}]}");
	connector = venture_shopify_connector_new("shop.myshopify.com", "tok",
		VENTURE_BANK_FEED_TRANSPORT(transport));
	orders = venture_commerce_connector_fetch_orders(connector, NULL, NULL, &error);
	g_assert_null(orders);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "currency"));
	g_object_unref(transport);
}

/* A full first page must not silently truncate a window's orders. */
static void
test_shopify_pages(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceConnector) connector = NULL;
	g_autoptr(GPtrArray) orders = NULL;
	g_autoptr(GString) body = g_string_new("{\"orders\":[");
	g_autoptr(GDateTime) from = g_date_time_new_from_iso8601("2026-08-01T12:30:00Z", NULL);
	g_autoptr(GDateTime) to = g_date_time_new_from_iso8601("2026-08-02T13:45:00Z", NULL);
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	guint i;
	for (i = 1; i <= 250; i++)
		g_string_append_printf(body, "%s{\"id\":%u,\"currency\":\"USD\",\"line_items\":[{\"title\":\"Service\",\"quantity\":1,\"price\":\"25.00\"}]}", i > 1 ? "," : "", i);
	g_string_append(body, "]}");
	transport->body = g_strdup(body->str);
	transport->next = g_strdup("{\"orders\":[{\"id\":251,\"currency\":\"USD\",\"line_items\":[{\"title\":\"Service\",\"quantity\":1,\"price\":\"25.00\"}]}]}");
	connector = venture_shopify_connector_new("shop.myshopify.com", "synthetic-token", VENTURE_BANK_FEED_TRANSPORT(transport));
	orders = venture_commerce_connector_fetch_orders(connector, from, to, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(orders->len, ==, 251);
	g_assert_cmpuint(transport->calls, ==, 2);
	g_assert_nonnull(strstr(transport->url, "since_id=250"));
	g_assert_nonnull(strstr(transport->url, "created_at_min=2026-08-01T12%3A30%3A00Z"));
	g_assert_nonnull(strstr(transport->url, "created_at_max=2026-08-02T13%3A45%3A00Z"));
	g_object_unref(transport);
}

static void
test_scoped_plugin_import(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureCompany) customer = venture_company_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
	g_autoptr(GPtrArray) rows = NULL;
	FakeConnector *connector = g_object_new(fake_connector_get_type(), NULL);
	JsonObject *spec;
	gint64 org;
	(void)data;
	g_object_set(other, "name", "Other shop", "legal-name", "Other shop", "default-currency", "USD", NULL);
	save(f, VENTURE_ENTITY(other));
	org = venture_entity_get_id(VENTURE_ENTITY(other));
	configure_fake(f, org);
	g_object_set(customer, "name", "Other customer", "organization-id", org, NULL);
	save(f, VENTURE_ENTITY(customer));
	service = venture_commerce_service_new(f->db, f->org, NULL, &error);
	g_assert_no_error(error);
	spec = order(f, "scoped:1", "Service", "10 USD", FALSE);
	json_object_set_int_member(spec, "company_id", venture_entity_get_id(VENTURE_ENTITY(customer)));
	json_object_set_boolean_member(spec, "send", FALSE);
	g_ptr_array_add(connector->orders, spec);
	venture_commerce_connector_registry_add_factory(venture_commerce_service_get_registry(service), VENTURE_COMMERCE_CONNECTOR_FACTORY(connector));
	g_assert_cmpint(venture_commerce_service_import_for_organization(service, org,
		venture_commerce_connector_get_name(VENTURE_COMMERCE_CONNECTOR(connector)), NULL, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	venture_query_set_organization(query, org);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpint(venture_entity_get_organization_id(g_ptr_array_index(rows, 0)), ==, org);
}

/* Tax and payment state cannot be discarded when creating accounting entries. */
static void
test_shopify_unsupported_amounts(void)
{
	static const gchar *const extra[] = {
		"\"total_tax\":\"2.50\"", "\"total_discounts\":\"5.00\"",
		"\"shipping_lines\":[{\"price\":\"3.00\"}]",
		"\"financial_status\":\"partially_paid\"",
		"\"financial_status\":\"partially_refunded\""
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(extra); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureCommerceConnector) connector = NULL;
		g_autoptr(GPtrArray) orders = NULL;
		FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
		transport->body = g_strdup_printf("{\"orders\":[{\"id\":1,\"currency\":\"USD\",%s,"
			"\"line_items\":[{\"title\":\"Hat\",\"quantity\":1,\"price\":\"25.00\"}]}]}", extra[i]);
		connector = venture_shopify_connector_new("shop.myshopify.com", "tok", VENTURE_BANK_FEED_TRANSPORT(transport));
		orders = venture_commerce_connector_fetch_orders(connector, NULL, NULL, &error);
		g_assert_null(orders);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED);
		g_object_unref(transport);
	}
}

/* A mismatched provider total must roll back both the invoice and customer. */
static void
test_shopify_total_mismatch(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	(void)data;
	transport->body = g_strdup("{\"orders\":[{\"id\":1,\"currency\":\"USD\",\"total_price\":\"30.00\","
		"\"financial_status\":\"paid\",\"customer\":{\"id\":90,\"email\":\"new@example.org\"},"
		"\"line_items\":[{\"title\":\"Hat\",\"quantity\":1,\"price\":\"25.00\"}]}]}");
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMPANY), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMMERCE_IMPORT_LINK), ==, 0);
	g_object_unref(transport);
}

static const gchar *commerce_test_order = "{\"orders\":[{\"id\":9001,\"currency\":\"USD\","
	"\"financial_status\":\"paid\",\"customer\":{\"id\":55,\"email\":\"shared@example.test\"},"
	"\"line_items\":[{\"title\":\"Service\",\"quantity\":1,\"price\":\"25.00\"}]}]}";

static VentureIntegrationConnection *
shop_binding(Fixture *f, gint64 org)
{
	g_autoptr(GError) error = NULL;
	VentureIntegrationConnection *binding = venture_integration_service_find(
		venture_integration_service_get(f->db), org, "commerce.shopify", &error);
	g_assert_no_error(error);
	g_assert_nonnull(binding);
	return binding;
}
static void
connect_shop(Fixture *f, VentureCommerceService *service, gint64 org, const gchar *shop, const gchar *token)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureIntegrationConnection) binding = venture_commerce_service_configure_shopify(
		service, org, shop, token, 0, 0, NULL, &error);
	(void)f;
	g_assert_no_error(error);
	g_assert_nonnull(binding);
}
static void
disconnect_shop(Fixture *f, VentureCommerceService *service, gint64 org)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureIntegrationConnection) binding = shop_binding(f, org);
	g_assert_true(venture_commerce_service_disconnect(service, org, venture_entity_get_id(VENTURE_ENTITY(binding)),
		venture_entity_get_version(VENTURE_ENTITY(binding)), NULL, &error));
	g_assert_no_error(error);
}

/* Equal provider IDs and emails never join accounts, even within one legal
 * organization. Reconnecting the original account still recognizes history. */
static void
test_account_identity(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureCommerceService) service = NULL;
	g_autoptr(VentureIntegrationConnection) old = shop_binding(f, f->org), rotated = NULL;
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	gint64 org;
	(void)data;
	transport->body = g_strdup(commerce_test_order);
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	g_object_set(other, "name", "Second account business", "default-currency", "USD", NULL);
	save(f, VENTURE_ENTITY(other));
	org = venture_entity_get_id(VENTURE_ENTITY(other));
	{
		static const gchar *const codes[] = { "1000", "1100", "4000" };
		guint i;
		for (i = 0; i < G_N_ELEMENTS(codes); i++)
		{
			g_autoptr(VentureEntity) account = VENTURE_ENTITY(venture_account_new());
			g_object_set(account, "organization-id", org, "code", codes[i], "name", codes[i],
				"kind", i == 2 ? VENTURE_ACCOUNT_KIND_INCOME : VENTURE_ACCOUNT_KIND_ASSET, "active", TRUE, NULL);
			save(f, account);
		}
	}
	g_assert_cmpint(venture_commerce_service_import_for_organization(service, org, "shopify", NULL, NULL, NULL, &error), ==, -1);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpuint(transport->calls, ==, 0);
	connect_shop(f, service, org, "other.myshopify.com", "other-token");
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpstr(transport->authorization, ==, "X-Shopify-Access-Token: test-token");
	{ gint imported = venture_commerce_service_import_for_organization(service, org, "shopify", NULL, NULL, NULL, &error); g_assert_no_error(error); g_assert_cmpint(imported, ==, 1); }
	g_assert_no_error(error);
	g_assert_cmpstr(transport->authorization, ==, "X-Shopify-Access-Token: other-token");
	g_assert_true(g_str_has_prefix(transport->url, "https://other.myshopify.com/"));
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMPANY), ==, 2);
	disconnect_shop(f, service, f->org);
	connect_shop(f, service, f->org, "replacement.myshopify.com", "replacement-token");
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 2);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMPANY), ==, 3);
	disconnect_shop(f, service, f->org);
	connect_shop(f, service, f->org, "shop.myshopify.com", "reconnected-token");
	/* The old displayed version must not rotate a newly connected row. */
	rotated = venture_commerce_service_configure_shopify(service, f->org, "shop.myshopify.com", "stale-token",
		venture_entity_get_id(VENTURE_ENTITY(old)), venture_entity_get_version(VENTURE_ENTITY(old)), NULL, &error);
	g_assert_null(rotated);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpstr(transport->authorization, ==, "X-Shopify-Access-Token: reconnected-token");
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 2);
	g_object_unref(transport);
}

typedef struct { Fixture *fixture; VentureCommerceService *service; gint operation; } DuringFetch;
static void
change_during_fetch(gpointer data)
{
	DuringFetch *change = data;
	Fixture *f = change->fixture;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureIntegrationConnection) binding = shop_binding(f, f->org), replacement = NULL;
	if (change->operation == 2) g_object_set(f->config, "commerce-enabled", FALSE, NULL);
	else if (change->operation == 1) disconnect_shop(f, change->service, f->org);
	else
	{
		replacement = venture_commerce_service_configure_shopify(change->service, f->org, "shop.myshopify.com", "rotated-token",
			venture_entity_get_id(VENTURE_ENTITY(binding)), venture_entity_get_version(VENTURE_ENTITY(binding)), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(replacement);
	}
}
/* A nested network completion can rotate or revoke the binding. No stale
 * response may create a customer, invoice, identity link or ledger entry. */
static void
test_inflight_revocation(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	DuringFetch change;
	transport->body = g_strdup(commerce_test_order);
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	change.fixture = f; change.service = service; change.operation = GPOINTER_TO_INT(data);
	transport->during_fetch = change_during_fetch;
	transport->callback_data = &change;
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, -1);
	g_assert_nonnull(error);
	g_assert_cmpuint(transport->calls, ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMPANY), ==, 1);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMMERCE_IMPORT_LINK), ==, 0);
	transport->during_fetch = NULL;
	g_clear_error(&error);
	if (change.operation != 0)
	{
		g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, -1);
		g_assert_nonnull(error);
		g_assert_cmpuint(transport->calls, ==, 1);
	}
	else
	{
		g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, 1);
		g_assert_no_error(error);
		g_assert_cmpstr(transport->authorization, ==, "X-Shopify-Access-Token: rotated-token");
	}
	if (change.operation == 2) g_object_set(f->config, "commerce-enabled", TRUE, NULL);
	g_object_unref(transport);
}

static void
test_explicit_legacy_adoption(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	g_autoptr(VentureIntegrationConnection) binding = shop_binding(f, f->org);
	g_autoptr(JsonObject) spec = order(f, "shopify:9001", "Service", "25 USD", FALSE);
	g_autoptr(VentureEntity) invoice = NULL, retained = NULL, link = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMMERCE_IMPORT_LINK);
	gint64 version;
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	(void)data;
	json_object_set_boolean_member(spec, "send", FALSE);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(f->db), f->org, spec, NULL, &error);
	g_assert_no_error(error);
	version = venture_entity_get_version(invoice);
	/* Upgrade adds identity storage without inventing an account for retained
	 * records. Both backends must preserve the original invoice revision. */
	g_assert_true(venture_database_execute(f->db, "DROP TABLE commerce_import_links", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	transport->body = g_strdup(commerce_test_order);
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	g_assert_true(venture_commerce_service_adopt_invoice(service, f->org, venture_entity_get_id(VENTURE_ENTITY(binding)),
		venture_entity_get_version(VENTURE_ENTITY(binding)), venture_entity_get_id(invoice), "Reviewed original shop order evidence", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, 0);
	g_assert_no_error(error);
	retained = venture_database_get(f->db, VENTURE_TYPE_INVOICE, venture_entity_get_id(invoice), &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_version(retained), ==, version);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 1);
	/* A bound historical invoice belongs to its adopted account, not every
	 * shop that subsequently reuses the same remote order number. */
	disconnect_shop(f, service, f->org);
	connect_shop(f, service, f->org, "second-legacy-shop.myshopify.com", "second-shop-token");
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 2);
	venture_query_set_organization(query, f->org);
	link = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	g_assert_false(venture_database_delete(f->db, link, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	{
		g_autoptr(VentureEntity) forged = VENTURE_ENTITY(venture_commerce_import_link_new());
		g_object_set(forged, "organization-id", f->org, "name", "Forged mapping", NULL);
		g_assert_false(venture_database_save(f->db, forged, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
		g_clear_error(&error);
	}
	g_object_set(link, "remote-id", "forged:identity", NULL);
	g_assert_false(venture_database_save(f->db, link, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_object_unref(transport);
}

static void
test_shop_validation_and_test(Fixture *f, gconstpointer data)
{
	static const gchar *const invalid[] = { "http://shop.myshopify.com", "shop.myshopify.com@evil.example", "shop.myshopify.com.evil", "../shop.myshopify.com", "SHOP.myshopify.com", "shop.myshopify.com:443", "-bad.myshopify.com" };
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	guint i;
	(void)data;
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	for (i = 0; i < G_N_ELEMENTS(invalid); i++)
	{
		g_autoptr(VentureCommerceConnector) client = venture_shopify_connector_new(invalid[i], "test-token", VENTURE_BANK_FEED_TRANSPORT(transport));
		g_autoptr(GPtrArray) orders = venture_commerce_connector_fetch_orders(client, NULL, NULL, &error);
		g_assert_null(orders);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}
	g_assert_cmpuint(transport->calls, ==, 0);
	transport->body = g_strdup("{\"shop\":{\"id\":55,\"myshopify_domain\":\"different.myshopify.com\"}}");
	g_assert_false(venture_commerce_service_test_shopify(service, f->org, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_free(transport->body);
	transport->body = g_strdup("{\"shop\":{\"id\":55,\"myshopify_domain\":\"shop.myshopify.com\"}}");
	g_assert_true(venture_commerce_service_test_shopify(service, f->org, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(transport->url, ==, "https://shop.myshopify.com/admin/api/2025-10/shop.json");
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_object_unref(transport);
}

/* Exercise request authority before outbound work, rather than relying on a
 * later invoice save to reject an already contacted provider. */
static void
test_request_authority(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "commerce-admin", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_autoptr(VentureEntity) membership = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureAuthPrincipal principal;
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	(void)data;
	save(f, user);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(user),
		"organization-id", f->org, "role", VENTURE_ORGANIZATION_ROLE_ADMIN, "active", TRUE, NULL);
	save(f, membership);
	principal.authenticated = TRUE; principal.user_id = venture_entity_get_id(user);
	principal.token_id = 0; principal.role = VENTURE_USER_ROLE_EDITOR; principal.name = NULL;
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	{ gint imported = venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error); g_assert_no_error(error); g_assert_cmpint(imported, ==, 0); }
	g_assert_cmpuint(transport->calls, ==, 1);
	g_clear_object(&scope);
	g_object_set(membership, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, NULL);
	save(f, membership);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	g_assert_cmpint(venture_commerce_service_import(service, "shopify", NULL, NULL, NULL, &error), ==, -1);
	g_assert_nonnull(error);
	g_assert_cmpuint(transport->calls, ==, 1);
	g_clear_object(&scope);
	g_object_unref(transport);
}
static void
test_legacy_client_refusal(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = venture_commerce_service_new(f->db, f->org, NULL, &error);
	FakeConnector *client = g_object_new(fake_connector_get_type(), NULL);
	(void)data;
	g_ptr_array_add(client->orders, order(f, "fake:1", "Forbidden live client", "25 USD", TRUE));
	venture_commerce_connector_registry_add(venture_commerce_service_get_registry(service), VENTURE_COMMERCE_CONNECTOR(client));
	g_assert_cmpint(venture_commerce_service_import(service, "fake", NULL, NULL, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_COMMERCE_IMPORT_LINK), ==, 0);
}

static void test_factory_async_transport(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureCommerceService) service = NULL;
	FakeConnector *factory = g_object_new(fake_connector_get_type(), NULL);
	FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
	gint imported;
	(void)data;
	factory->async_probe = TRUE;
	service = venture_commerce_service_new(f->db, f->org, VENTURE_BANK_FEED_TRANSPORT(transport), &error);
	venture_commerce_connector_registry_add_factory(venture_commerce_service_get_registry(service), VENTURE_COMMERCE_CONNECTOR_FACTORY(factory));
	imported = venture_commerce_service_import(service, "fake", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(imported, ==, 0);
	g_assert_cmpuint(transport->calls, ==, 1);
	g_object_unref(transport);
}

static void test_malformed_provider_response(void)
{
	static const gchar *const bodies[] = {
		"{\"orders\":[null]}", "{\"orders\":[1]}",
		"{\"orders\":[{\"id\":true,\"currency\":\"USD\",\"line_items\":[{\"title\":\"Service\",\"quantity\":1,\"price\":\"25.00\"}]}]}",
		"{\"orders\":[{\"id\":1,\"currency\":\"USD\",\"line_items\":[1]}]}",
		"{\"orders\":[{\"id\":1,\"currency\":\"USD\",\"line_items\":[{\"price\":\"25.00\"}]}]}",
		"{\"orders\":[{\"id\":1,\"currency\":\"USD\",\"line_items\":[{\"price\":\"25.00\",\"quantity\":1.2}]}]}"
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(bodies); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureCommerceConnector) client = NULL;
		g_autoptr(GPtrArray) orders = NULL;
		FakeTransport *transport = g_object_new(fake_transport_get_type(), NULL);
		transport->body = g_strdup(bodies[i]);
		client = venture_shopify_connector_new("shop.myshopify.com", "test-token", VENTURE_BANK_FEED_TRANSPORT(transport));
		orders = venture_commerce_connector_fetch_orders(client, NULL, NULL, &error);
		g_assert_null(orders);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_object_unref(transport);
	}
}

int
main(int argc, char **argv)
{
	g_setenv("VENTURE_COMMERCE_SHOPIFY_TOKEN", "ambient-secret", TRUE);
	g_setenv("VENTURE_COMMERCE_SHOPIFY_SHOP", "ambient.myshopify.com", TRUE);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/commerce/missing-key", test_missing_key);
	g_test_add_func("/commerce/malformed-provider-response", test_malformed_provider_response);
	g_test_add("/commerce/factory-async-transport", Fixture, NULL, setup, test_factory_async_transport, teardown);
	g_test_add("/commerce/request-authority", Fixture, NULL, setup, test_request_authority, teardown);
	g_test_add("/commerce/legacy-client-refusal", Fixture, NULL, setup, test_legacy_client_refusal, teardown);
	g_test_add("/commerce/account-identity", Fixture, NULL, setup, test_account_identity, teardown);
	g_test_add("/commerce/rotation-during-fetch", Fixture, NULL, setup, test_inflight_revocation, teardown);
	g_test_add("/commerce/disconnect-during-fetch", Fixture, GINT_TO_POINTER(1), setup, test_inflight_revocation, teardown);
	g_test_add("/commerce/module-disabled-during-fetch", Fixture, GINT_TO_POINTER(2), setup, test_inflight_revocation, teardown);
	g_test_add("/commerce/legacy-adoption", Fixture, NULL, setup, test_explicit_legacy_adoption, teardown);
	g_test_add("/commerce/account-validation-test", Fixture, NULL, setup, test_shop_validation_and_test, teardown);
	g_test_add("/commerce/idempotent-import", Fixture, NULL, setup, test_idempotent_import, teardown);
	g_test_add("/commerce/failure-rollback", Fixture, NULL, setup, test_failure_rolls_back, teardown);
	g_test_add("/commerce/shopify-currency-cancelled", Fixture, NULL, setup, test_shopify_currency_and_cancelled, teardown);
	g_test_add("/commerce/shopify-refuses-usd-default", Fixture, NULL, setup, test_shopify_refuses_bare_usd, teardown);
	g_test_add_func("/commerce/pagination-window", test_shopify_pages);
	g_test_add_func("/commerce/unsupported-amounts", test_shopify_unsupported_amounts);
	g_test_add("/commerce/total-mismatch", Fixture, NULL, setup, test_shopify_total_mismatch, teardown);
	g_test_add("/commerce/scoped-plugin", Fixture, NULL, setup, test_scoped_plugin_import, teardown);
	return g_test_run();
}
