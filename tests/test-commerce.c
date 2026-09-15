/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Covers src/commerce: Shopify-like connectors import orders as invoices. */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 company;
} Fixture;

typedef struct { GObject parent; GPtrArray *orders; } FakeConnector;
typedef struct { GObjectClass parent; } FakeConnectorClass;
GType fake_connector_get_type(void);
static void fake_connector_iface(VentureCommerceConnectorInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeConnector, fake_connector, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_COMMERCE_CONNECTOR, fake_connector_iface))
static const gchar *fake_connector_name(VentureCommerceConnector *self)
{
	(void)self;
	return "fake";
}
static GPtrArray *
fake_connector_fetch(VentureCommerceConnector *connector, GDateTime *from, GDateTime *to, GError **error)
{
	FakeConnector *self = (FakeConnector *)connector;
	GPtrArray *copy = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
	guint i;
	(void)from; (void)to; (void)error;
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
	G_OBJECT_CLASS(fake_connector_parent_class)->finalize(object);
}
static void fake_connector_class_init(FakeConnectorClass *klass) { G_OBJECT_CLASS(klass)->finalize = fake_connector_finalize; }
static void fake_connector_init(FakeConnector *self)
{
	self->orders = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
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
	g_setenv("VENTURE_COMMERCE_SHOPIFY_TOKEN", "test-token", TRUE);
	g_setenv("VENTURE_COMMERCE_SHOPIFY_SHOP", "shop.myshopify.com", TRUE);
	f->config = venture_config_new();
	g_object_set(f->config, "commerce-enabled", TRUE, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
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

	g_unsetenv("VENTURE_COMMERCE_SHOPIFY_TOKEN");
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	service = venture_commerce_service_new(db, 1, NULL, &error);
	g_assert_nonnull(service);
	g_assert_no_error(error);
	g_assert_null(venture_commerce_connector_registry_lookup(venture_commerce_service_get_registry(service), "shopify"));
	g_setenv("VENTURE_COMMERCE_SHOPIFY_TOKEN", "test-token", TRUE);
	g_setenv("VENTURE_COMMERCE_SHOPIFY_SHOP", "shop.myshopify.com", TRUE);
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
	venture_commerce_connector_registry_add(venture_commerce_service_get_registry(service),
		VENTURE_COMMERCE_CONNECTOR(connector));
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
	venture_commerce_connector_registry_add(venture_commerce_service_get_registry(service),
		VENTURE_COMMERCE_CONNECTOR(connector));
	imported = venture_commerce_service_import(service, "fake", NULL, NULL, NULL, &error);
	g_assert_cmpint(imported, <, 0);
	g_assert_nonnull(error);
	g_assert_cmpuint(count_type(f, VENTURE_TYPE_INVOICE), ==, 0);
}

typedef struct { GObject parent; gchar *body; gchar *next; gchar *url; guint calls; } FakeTransport;
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
	(void)authorization;
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
		g_string_append_printf(body, "%s{\"id\":%u,\"currency\":\"USD\",\"line_items\":[]}", i > 1 ? "," : "", i);
	g_string_append(body, "]}");
	transport->body = g_strdup(body->str);
	transport->next = g_strdup("{\"orders\":[{\"id\":251,\"currency\":\"USD\",\"line_items\":[]}]}");
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
	g_object_set(customer, "name", "Other customer", "organization-id", org, NULL);
	save(f, VENTURE_ENTITY(customer));
	service = venture_commerce_service_new(f->db, f->org, NULL, &error);
	g_assert_no_error(error);
	spec = order(f, "scoped:1", "Service", "10 USD", FALSE);
	json_object_set_int_member(spec, "company_id", venture_entity_get_id(VENTURE_ENTITY(customer)));
	json_object_set_boolean_member(spec, "send", FALSE);
	g_ptr_array_add(connector->orders, spec);
	venture_commerce_connector_registry_add(venture_commerce_service_get_registry(service), VENTURE_COMMERCE_CONNECTOR(connector));
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
	g_object_unref(transport);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/commerce/missing-key", test_missing_key);
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
