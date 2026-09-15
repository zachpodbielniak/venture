/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 customer;
	gint64 vendor;
	gint64 product;
	gint64 item;
	gint64 venture;
} Fixture;

static GType
type(const gchar *name)
{
	GType result = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(result, !=, G_TYPE_INVALID);
	return result;
}

static VentureEntity *
record(Fixture *f, const gchar *name)
{
	VentureEntity *r = g_object_new(type(name), NULL);
	venture_entity_set_organization_id(r, f->org);
	return r;
}

static void
save(Fixture *f, VentureEntity *r)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, r, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
field(VentureEntity *r, const gchar *key, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(r, key, value, &error));
	g_assert_no_error(error);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) venture = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	customer = record(f, "company");
	field(customer, "name", "Buyer");
	save(f, customer);
	f->customer = venture_entity_get_id(customer);
	vendor = record(f, "company");
	field(vendor, "name", "Supplier");
	field(vendor, "kind", "supplier");
	save(f, vendor);
	f->vendor = venture_entity_get_id(vendor);
	venture = record(f, "venture");
	field(venture, "name", "Shop");
	save(f, venture);
	f->venture = venture_entity_get_id(venture);
	product = record(f, "product");
	g_object_set(product, "name", "Widget", "venture-id", f->venture, NULL);
	field(product, "list-price", "20 USD");
	save(f, product);
	f->product = venture_entity_get_id(product);
	item = record(f, "inventory_item");
	g_object_set(item, "product-id", f->product, "sku", "W-1", "location", "WH", NULL);
	save(f, item);
	f->item = venture_entity_get_id(item);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
stock(Fixture *f, gint64 qty)
{
	g_autoptr(VentureEntity) po = record(f, "purchase_order");
	g_autoptr(VentureEntity) line = record(f, "purchase_order_line");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-05-01", NULL);
	g_object_set(po, "number", "PO-SO", "vendor-id", f->vendor, "currency", "USD", "status", "draft", NULL);
	field(po, "ordered-at", "2026-05-01");
	save(f, po);
	g_object_set(line, "purchase-order-id", venture_entity_get_id(po),
		"product-id", f->product, "inventory-item-id", f->item,
		"description", "Widget", "quantity", qty, NULL);
	field(line, "unit-price", "5 USD");
	save(f, line);
	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db),
		venture_entity_get_id(po), date, NULL, &error));
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db),
		venture_entity_get_id(po), date, NULL, &error));
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		venture_entity_get_id(line), qty, date, NULL, &error));
}

static gint64
so_line(Fixture *f, VentureEntity *order, gint64 qty, gboolean service)
{
	g_autoptr(VentureEntity) line = record(f, "sales_order_line");
	g_object_set(line, "sales-order-id", venture_entity_get_id(order),
		"product-id", f->product, "inventory-item-id", service ? (gint64)0 : f->item,
		"description", service ? "Support" : "Widget", "quantity", qty,
		"is-service", service, NULL);
	field(line, "unit-price", "20 USD");
	save(f, line);
	return venture_entity_get_id(line);
}

static void
test_records(void)
{
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "sales_order"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "sales_order_line"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "fulfillment"), !=, G_TYPE_INVALID);
}

static void
test_partial_fulfill_invoice(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) order = record(f, "sales_order");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-05-02", NULL);
	gint64 line_id;
	gint64 order_id;
	(void)unused;
	stock(f, 10);
	g_object_set(order, "number", "SO-1", "company-id", f->customer, "currency", "USD", "status", "draft", NULL);
	field(order, "ordered-at", "2026-05-02");
	save(f, order);
	order_id = venture_entity_get_id(order);
	line_id = so_line(f, order, 6, FALSE);
	g_assert_true(venture_sales_order_service_allocate(venture_sales_order_service_get(f->db),
		order_id, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_sales_order_service_ship_line(venture_sales_order_service_get(f->db),
		line_id, 4, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_sales_order_service_invoice(venture_sales_order_service_get(f->db),
		order_id, date, NULL, &error));
	g_clear_error(&error);
	g_assert_true(venture_sales_order_service_invoice_fulfilled(venture_sales_order_service_get(f->db),
		order_id, date, NULL, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->db, type("sales_order_line"), line_id, NULL);
		gint64 invoiced = 0, fulfilled = 0;
		g_object_get(stored, "invoiced-qty", &invoiced, "fulfilled-qty", &fulfilled, NULL);
		g_assert_cmpint(fulfilled, ==, 4);
		g_assert_cmpint(invoiced, ==, 4);
	}
	g_assert_false(venture_sales_order_service_invoice_fulfilled(venture_sales_order_service_get(f->db),
		order_id, date, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_service_without_fulfillment(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) order = record(f, "sales_order");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-05-02", NULL);
	gint64 order_id;
	(void)unused;
	g_object_set(order, "number", "SO-2", "company-id", f->customer, "currency", "USD", "status", "draft", NULL);
	field(order, "ordered-at", "2026-05-02");
	save(f, order);
	order_id = venture_entity_get_id(order);
	so_line(f, order, 1, TRUE);
	g_assert_true(venture_sales_order_service_invoice(venture_sales_order_service_get(f->db),
		order_id, date, NULL, &error));
	g_assert_no_error(error);
}

static void
test_cannot_invoice_unfulfilled(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) order = record(f, "sales_order");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-05-02", NULL);
	(void)unused;
	stock(f, 2);
	g_object_set(order, "number", "SO-3", "company-id", f->customer, "currency", "USD", "status", "draft", NULL);
	field(order, "ordered-at", "2026-05-02");
	save(f, order);
	so_line(f, order, 2, FALSE);
	g_assert_true(venture_sales_order_service_allocate(venture_sales_order_service_get(f->db),
		venture_entity_get_id(order), date, NULL, &error));
	g_assert_false(venture_sales_order_service_invoice(venture_sales_order_service_get(f->db),
		venture_entity_get_id(order), date, NULL, &error));
	g_assert_nonnull(strstr(error->message, "fulfilled"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sales-orders/records", test_records);
	g_test_add("/sales-orders/partial-invoice", Fixture, NULL, setup, test_partial_fulfill_invoice, teardown);
	g_test_add("/sales-orders/service-line", Fixture, NULL, setup, test_service_without_fulfillment, teardown);
	g_test_add("/sales-orders/unfulfilled", Fixture, NULL, setup, test_cannot_invoice_unfulfilled, teardown);
	return g_test_run();
}
