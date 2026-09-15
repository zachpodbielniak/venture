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
	g_assert_null(service);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_COMMERCE_SHOPIFY_TOKEN"));
	g_setenv("VENTURE_COMMERCE_SHOPIFY_TOKEN", "test-token", TRUE);
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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/commerce/missing-key", test_missing_key);
	g_test_add("/commerce/idempotent-import", Fixture, NULL, setup, test_idempotent_import, teardown);
	g_test_add("/commerce/failure-rollback", Fixture, NULL, setup, test_failure_rolls_back, teardown);
	return g_test_run();
}
