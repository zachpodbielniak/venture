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
	gint64 vendor;
	gint64 product;
	gint64 item;
	gint64 item_b;
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

static gint64
account_id(Fixture *f, const gchar *code)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) found = NULL;
	g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", f->org, code);
	guint i;
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	found = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(found);
	for (i = 0; i < found->len; i++)
	{
		g_autofree gchar *actual = NULL;
		g_object_get(g_ptr_array_index(found, i), "code", &actual, NULL);
		if (g_strcmp0(actual, code) == 0 || g_strcmp0(actual, scoped) == 0)
			return venture_entity_get_id(g_ptr_array_index(found, i));
	}
	return 0;
}

static gint64
balance(Fixture *f, const gchar *code)
{
	g_autoptr(GDateTime) as_of = g_date_time_new_now_utc();
	g_autoptr(VentureMoney) amount = NULL;
	gint64 id = account_id(f, code);
	g_assert_cmpint(id, !=, 0);
	amount = venture_posting_service_account_balance(venture_database_get_posting_service(f->db),
		id, f->org, "USD", as_of, NULL);
	g_assert_nonnull(amount);
	return venture_money_get_amount(amount);
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) item_b = NULL;
	g_autoptr(VentureEntity) venture = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
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
	field(product, "cost", "4 USD");
	save(f, product);
	f->product = venture_entity_get_id(product);
	item = record(f, "inventory_item");
	g_object_set(item, "product-id", f->product, "sku", "W-1", "location", "WH", "reorder-point", (gint64)5, NULL);
	save(f, item);
	f->item = venture_entity_get_id(item);
	item_b = record(f, "inventory_item");
	g_object_set(item_b, "product-id", f->product, "sku", "W-2", "location", "SHOP", "reorder-point", (gint64)0, NULL);
	save(f, item_b);
	f->item_b = venture_entity_get_id(item_b);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static gint64
po_line(Fixture *f, gint64 qty, const gchar *number, const gchar *price)
{
	g_autoptr(VentureEntity) po = record(f, "purchase_order");
	g_autoptr(VentureEntity) line = record(f, "purchase_order_line");
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-04-01", NULL);
	gint64 line_id;
	g_object_set(po, "number", number, "vendor-id", f->vendor, "currency", "USD", "status", "draft", NULL);
	field(po, "ordered-at", "2026-04-01");
	save(f, po);
	g_object_set(line, "purchase-order-id", venture_entity_get_id(po),
		"product-id", f->product, "inventory-item-id", f->item,
		"description", "Widget", "quantity", qty, NULL);
	field(line, "unit-price", price);
	save(f, line);
	line_id = venture_entity_get_id(line);
	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db),
		venture_entity_get_id(po), date, NULL, &error));
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db),
		venture_entity_get_id(po), date, NULL, &error));
	return line_id;
}

static void
test_fifo_and_gl(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-04-02", NULL);
	g_autoptr(VentureMoney) valuation = NULL;
	gint64 first = po_line(f, 2, "PO-A", "10 USD");
	gint64 second = po_line(f, 3, "PO-B", "12 USD");
	g_autoptr(VentureMoney) cogs = NULL;
	(void)unused;

	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		first, 2, date, NULL, &error));
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		second, 3, date, NULL, &error));
	g_assert_cmpint(venture_inventory_service_on_hand(venture_inventory_service_get(f->db), f->item, NULL, &error), ==, 5);
	valuation = venture_inventory_service_valuation(venture_inventory_service_get(f->db), f->org, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(valuation), ==, 2 * 1000 + 3 * 1200);
	g_assert_cmpint(balance(f, "1200"), ==, venture_money_get_amount(valuation));

	g_assert_true(venture_inventory_service_issue(venture_inventory_service_get(f->db),
		f->item, 3, date, "inventory_txn", 0, NULL, &cogs, &error));
	g_assert_no_error(error);
	/* FIFO: 2 @ 10.00 then 1 @ 12.00 */
	g_assert_cmpint(venture_money_get_amount(cogs), ==, 2000 + 1200);
	g_assert_cmpint(balance(f, "5000"), ==, 3200);
	valuation = venture_inventory_service_valuation(venture_inventory_service_get(f->db), f->org, NULL, &error);
	g_assert_cmpint(venture_money_get_amount(valuation), ==, 2 * 1200);
	g_assert_cmpint(balance(f, "1200"), ==, venture_money_get_amount(valuation));
}

static void
test_negative_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-04-02", NULL);
	g_autoptr(VentureEntity) txn = record(f, "inventory_txn");
	(void)unused;
	g_object_set(txn, "inventory-item-id", f->item, "kind", VENTURE_INVENTORY_TXN_KIND_SALE,
		"quantity", (gint64)-1, NULL);
	field(txn, "occurred-at", "2026-04-02");
	g_assert_false(venture_database_save(f->db, txn, NULL, &error));
	g_assert_nonnull(strstr(error->message, "negative"));
	g_clear_error(&error);
	g_assert_false(venture_inventory_service_issue(venture_inventory_service_get(f->db),
		f->item, 1, date, "inventory_txn", 0, NULL, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_transfer_and_reorder(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-04-02", NULL);
	gint64 line = po_line(f, 4, "PO-C", "5 USD");
	VentureReport *report;
	g_autoptr(VentureReportResult) result = NULL;
	(void)unused;
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line, 4, date, NULL, &error));
	g_assert_true(venture_inventory_service_transfer(venture_inventory_service_get(f->db),
		f->item, f->item_b, 2, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_inventory_service_on_hand(venture_inventory_service_get(f->db), f->item, NULL, &error), ==, 2);
	g_assert_cmpint(venture_inventory_service_on_hand(venture_inventory_service_get(f->db), f->item_b, NULL, &error), ==, 2);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVENTORY_TXN);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		gboolean found = FALSE;
		venture_query_set_organization(query, f->org);
		venture_query_add_filter_int(query, "inventory-item-id", VENTURE_FILTER_OP_EQ, f->item_b, NULL);
		rows = venture_database_find(f->db, query, NULL);
		g_assert_nonnull(rows);
		for (i = 0; i < rows->len; i++)
		{
			gint kind = 0;
			g_autoptr(VentureMoney) unit = NULL;
			g_object_get(g_ptr_array_index(rows, i), "kind", &kind, "unit-cost", &unit, NULL);
			if (kind != VENTURE_INVENTORY_TXN_KIND_TRANSFER)
				continue;
			g_assert_nonnull(unit);
			g_assert_cmpint(venture_money_get_amount(unit), ==, 500);
			found = TRUE;
		}
		g_assert_true(found);
	}
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "reorder_worklist");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_report_result_get_row_count(result), >=, 1);
}


static void
test_restore_without_receipt(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-04-02", NULL);
	g_autoptr(VentureMoney) unit = venture_money_new_for_currency(500, "USD");
	gint64 line = po_line(f, 2, "PO-E", "5 USD");
	(void)unused;
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line, 2, date, NULL, &error));
	g_assert_true(venture_inventory_service_restore(venture_inventory_service_get(f->db),
		f->item, 1, unit, date, 0, "return", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_inventory_service_on_hand(venture_inventory_service_get(f->db), f->item, NULL, &error), ==, 1);
}

static void
test_autojournal_no_double(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-04-02", NULL);
	g_autoptr(VentureEntity) sale = record(f, "sale");
	gint64 line = po_line(f, 2, "PO-D", "4 USD");
	(void)unused;
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line, 2, date, NULL, &error));
	g_object_set(sale, "venture-id", f->venture, "product-id", f->product, "quantity", (gint64)1, NULL);
	field(sale, "occurred-at", "2026-04-03");
	field(sale, "gross", "10 USD");
	save(f, sale);
	g_assert_cmpint(balance(f, "5000"), ==, 400);
	g_assert_cmpint(balance(f, "1200"), ==, 400);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/inventory-ledger/fifo-gl", Fixture, NULL, setup, test_fifo_and_gl, teardown);
	g_test_add("/inventory-ledger/negative", Fixture, NULL, setup, test_negative_refused, teardown);
	g_test_add("/inventory-ledger/transfer-reorder", Fixture, NULL, setup, test_transfer_and_reorder, teardown);
	g_test_add("/inventory-ledger/autojournal", Fixture, NULL, setup, test_autojournal_no_double, teardown);
	g_test_add("/inventory-ledger/restore-without-receipt", Fixture, NULL, setup, test_restore_without_receipt, teardown);
	return g_test_run();
}
