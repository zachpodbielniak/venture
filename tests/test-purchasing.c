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
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) product = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) venture = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
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
	product = record(f, "product");
	g_object_set(product, "name", "Widget", "venture-id", venture_entity_get_id(venture), NULL);
	field(product, "list-price", "10 USD");
	field(product, "cost", "4 USD");
	save(f, product);
	f->product = venture_entity_get_id(product);
	item = record(f, "inventory_item");
	g_object_set(item, "product-id", f->product, "sku", "W-1", "location", "WH", "reorder-point", (gint64)2, NULL);
	field(item, "unit-cost", "4 USD");
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
test_records(void)
{
	static const gchar *const names[] = {
		"purchase_order", "purchase_order_line", "goods_receipt", "goods_receipt_line"
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(
			venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}

static VentureEntity *
purchase_order(Fixture *f, const gchar *number, gint64 qty)
{
	VentureEntity *po = record(f, "purchase_order");
	g_autoptr(VentureEntity) line = NULL;
	g_object_set(po, "number", number, "vendor-id", f->vendor, "currency", "USD",
		"status", "draft", "match-tolerance-percent", (gint64)2, NULL);
	field(po, "ordered-at", "2026-03-01");
	save(f, po);
	line = record(f, "purchase_order_line");
	g_object_set(line, "purchase-order-id", venture_entity_get_id(po),
		"product-id", f->product, "inventory-item-id", f->item,
		"description", "Widget", "quantity", qty, "position", (gint64)1, NULL);
	field(line, "unit-price", "4 USD");
	save(f, line);
	return po;
}

static VentureEntity *
fresh(Fixture *f, const gchar *name, gint64 id)
{
	return venture_database_get(f->db, type(name), id, NULL);
}

static gint64
first_line_id(Fixture *f, gint64 po_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(type("purchase_order_line"));
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, f->org);
	venture_query_add_filter_int(query, "purchase-order-id", VENTURE_FILTER_OP_EQ, po_id, NULL);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, >, 0);
	return venture_entity_get_id(g_ptr_array_index(rows, 0));
}

static gboolean
reject_match_once(VentureDatabase *db, VentureEntity *record, VentureEntity *previous,
	gpointer data, GError **error)
{
	(void)record;
	(void)data;
	/* Inject a final-header failure after the order and bill lines were already saved. */
	if (previous != NULL && g_object_get_data(G_OBJECT(db), "reject-match") != NULL)
	{
		g_object_set_data(G_OBJECT(db), "reject-match", NULL);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Rejected matching bill");
		return FALSE;
	}
	return TRUE;
}

static void
test_receive_and_three_way(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) po = purchase_order(f, "PO-1", 10);
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) bill_line = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-03-02", NULL);
	gint64 po_id = venture_entity_get_id(po);
	gint64 line_id = first_line_id(f, po_id);
	gint64 on_hand;
	(void)unused;

	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line_id, 6, date, NULL, &error));
	g_assert_no_error(error);
	on_hand = venture_inventory_service_on_hand(venture_inventory_service_get(f->db), f->item, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(on_hand, ==, 6);

	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line_id, 4, date, NULL, &error));
	g_assert_no_error(error);
	on_hand = venture_inventory_service_on_hand(venture_inventory_service_get(f->db), f->item, NULL, &error);
	g_assert_cmpint(on_hand, ==, 10);

	bill = record(f, "vendor_bill");
	g_object_set(bill, "number", "B-1", "company-id", f->vendor, "currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-03-03");
	save(f, bill);
	bill_line = record(f, "vendor_bill_line");
	g_object_set(bill_line, "bill-id", venture_entity_get_id(bill),
		"description", "Widget", "quantity", "10", "purchase-order-line-id", line_id, NULL);
	field(bill_line, "unit-price", "4 USD");
	save(f, bill_line);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_VENDOR_BILL, reject_match_once, NULL, NULL);
	g_object_set_data(G_OBJECT(f->db), "reject-match", GINT_TO_POINTER(1));
	g_assert_false(venture_purchasing_service_match(venture_purchasing_service_get(f->db),
		po_id, venture_entity_get_id(bill), FALSE, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	{
		g_autoptr(VentureEntity) stored = fresh(f, "purchase_order", po_id);
		gint64 linked = 0;
		g_object_get(stored, "vendor-bill-id", &linked, NULL);
		g_assert_cmpint(linked, ==, 0);
	}
	g_assert_true(venture_purchasing_service_match(venture_purchasing_service_get(f->db),
		po_id, venture_entity_get_id(bill), FALSE, NULL, &error));
	g_assert_no_error(error);
	event = record(f, "vendor_bill_event");
	g_object_set(event, "bill-id", venture_entity_get_id(bill), "vendor-id", f->vendor,
		"kind", "approve", "state", "approved", NULL);
	field(event, "date", "2026-03-03");
	save(f, event);
	{
		g_autoptr(VentureEntity) stored = fresh(f, "vendor_bill", venture_entity_get_id(bill));
		g_autofree gchar *status = NULL;
		g_object_get(stored, "status", &status, NULL);
		g_assert_cmpstr(status, ==, "approved");
	}
}

static void
test_mismatch_refuses_bill(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) po = purchase_order(f, "PO-2", 10);
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) bill_line = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-03-02", NULL);
	gint64 po_id = venture_entity_get_id(po);
	gint64 line_id = first_line_id(f, po_id);
	(void)unused;

	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line_id, 10, date, NULL, &error));
	bill = record(f, "vendor_bill");
	g_object_set(bill, "number", "B-2", "company-id", f->vendor, "currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-03-03");
	save(f, bill);
	bill_line = record(f, "vendor_bill_line");
	g_object_set(bill_line, "bill-id", venture_entity_get_id(bill),
		"description", "Widget", "quantity", "10", "purchase-order-line-id", line_id, NULL);
	field(bill_line, "unit-price", "9 USD");
	save(f, bill_line);
	g_assert_true(venture_purchasing_service_match(venture_purchasing_service_get(f->db),
		po_id, venture_entity_get_id(bill), FALSE, NULL, &error));
	g_assert_no_error(error);
	event = record(f, "vendor_bill_event");
	g_object_set(event, "bill-id", venture_entity_get_id(bill), "vendor-id", f->vendor,
		"kind", "approve", "state", "approved", NULL);
	field(event, "date", "2026-03-03");
	g_assert_false(venture_database_save(f->db, event, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "three-way"));
	g_clear_error(&error);

	g_assert_true(venture_purchasing_service_match(venture_purchasing_service_get(f->db),
		po_id, venture_entity_get_id(bill), TRUE, NULL, &error));
	g_assert_no_error(error);
	g_object_set(event, "kind", "approve", NULL);
	field(event, "date", "2026-03-03");
	g_assert_true(venture_database_save(f->db, event, NULL, &error));
	g_assert_no_error(error);
}

static void
test_cancel_and_return(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) open = purchase_order(f, "PO-3", 5);
	g_autoptr(VentureEntity) received = purchase_order(f, "PO-4", 5);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-03-02", NULL);
	gint64 open_id = venture_entity_get_id(open);
	gint64 received_id = venture_entity_get_id(received);
	gint64 line_id;
	(void)unused;

	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db), open_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_cancel(venture_purchasing_service_get(f->db), open_id, date, NULL, &error));
	g_assert_no_error(error);

	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db), received_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db), received_id, date, NULL, &error));
	line_id = first_line_id(f, received_id);
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line_id, 5, date, NULL, &error));
	g_assert_true(venture_purchasing_service_return_line(venture_purchasing_service_get(f->db),
		line_id, 5, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_inventory_service_on_hand(venture_inventory_service_get(f->db), f->item, NULL, &error), ==, 0);
	/* A complete return reopens the order for replacements and subsequent cancellation. */
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line_id, 5, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_purchasing_service_return_line(venture_purchasing_service_get(f->db),
		line_id, 5, date, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_purchasing_service_cancel(venture_purchasing_service_get(f->db), received_id, date, NULL, &error));
	g_assert_no_error(error);
}

static void
test_committed_spend(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) po = purchase_order(f, "PO-5", 8);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-03-02", NULL);
	VentureReport *report;
	g_autoptr(VentureReportResult) result = NULL;
	gint64 po_id = venture_entity_get_id(po);
	(void)unused;

	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "committed_spend");
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(venture_report_result_get_row_count(result), >=, 1);
}

static void
test_owned_rows(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) po = purchase_order(f, "PO-6", 1);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-03-02", NULL);
	(void)unused;
	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db),
		venture_entity_get_id(po), date, NULL, &error));
	g_object_set(po, "status", "cancelled", NULL);
	g_assert_false(venture_database_save(f->db, po, NULL, &error));
	g_assert_nonnull(strstr(error->message, "VenturePurchasingService"));
}

static void
test_unmatched_bill_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) po = purchase_order(f, "PO-6", 10);
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) bill_line = NULL;
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-03-02", NULL);
	gint64 po_id = venture_entity_get_id(po);
	gint64 line_id = first_line_id(f, po_id);
	(void)unused;

	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line_id, 10, date, NULL, &error));
	bill = record(f, "vendor_bill");
	g_object_set(bill, "number", "B-6", "company-id", f->vendor, "currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-03-03");
	save(f, bill);
	bill_line = record(f, "vendor_bill_line");
	g_object_set(bill_line, "bill-id", venture_entity_get_id(bill),
		"description", "Widget", "quantity", "10", "purchase-order-line-id", line_id, NULL);
	field(bill_line, "unit-price", "4 USD");
	save(f, bill_line);
	event = record(f, "vendor_bill_event");
	g_object_set(event, "bill-id", venture_entity_get_id(bill), "vendor-id", f->vendor,
		"kind", "approve", "state", "approved", NULL);
	field(event, "date", "2026-03-03");
	g_assert_false(venture_database_save(f->db, event, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "match"));
}

static void
test_decimal_billed_qty_mismatch(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) po = purchase_order(f, "PO-7", 10);
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) bill_line = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) date = venture_time_from_string("2026-03-02", NULL);
	gint64 po_id = venture_entity_get_id(po);
	gint64 line_id = first_line_id(f, po_id);
	g_autofree gchar *status = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	(void)unused;

	g_assert_true(venture_purchasing_service_approve(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_send(venture_purchasing_service_get(f->db), po_id, date, NULL, &error));
	g_assert_true(venture_purchasing_service_receive_line(venture_purchasing_service_get(f->db),
		line_id, 10, date, NULL, &error));
	bill = record(f, "vendor_bill");
	g_object_set(bill, "number", "B-7", "company-id", f->vendor, "currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-03-03");
	save(f, bill);
	bill_line = record(f, "vendor_bill_line");
	g_object_set(bill_line, "bill-id", venture_entity_get_id(bill),
		"description", "Widget", "quantity", "10.01", "purchase-order-line-id", line_id, NULL);
	field(bill_line, "unit-price", "4 USD");
	save(f, bill_line);
	g_assert_true(venture_purchasing_service_match(venture_purchasing_service_get(f->db),
		po_id, venture_entity_get_id(bill), FALSE, NULL, &error));
	g_assert_no_error(error);
	stored = fresh(f, "purchase_order", po_id);
	g_object_get(stored, "match-status", &status, NULL);
	g_assert_cmpstr(status, ==, "mismatch");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/purchasing/records", test_records);
	g_test_add("/purchasing/receive-three-way", Fixture, NULL, setup, test_receive_and_three_way, teardown);
	g_test_add("/purchasing/mismatch-exception", Fixture, NULL, setup, test_mismatch_refuses_bill, teardown);
	g_test_add("/purchasing/cancel-return", Fixture, NULL, setup, test_cancel_and_return, teardown);
	g_test_add("/purchasing/committed-spend", Fixture, NULL, setup, test_committed_spend, teardown);
	g_test_add("/purchasing/owned-rows", Fixture, NULL, setup, test_owned_rows, teardown);
	g_test_add("/purchasing/unmatched-bill", Fixture, NULL, setup, test_unmatched_bill_refused, teardown);
	g_test_add("/purchasing/decimal-qty", Fixture, NULL, setup, test_decimal_billed_qty_mismatch, teardown);
	return g_test_run();
}
