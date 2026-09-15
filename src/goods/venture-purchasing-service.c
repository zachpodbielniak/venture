/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <stdlib.h>

struct _VenturePurchasingService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VenturePurchasingService, venture_purchasing_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VenturePurchasingService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_PURCHASING_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VenturePurchasingService *self = VENTURE_PURCHASING_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VenturePurchasingService *self = VENTURE_PURCHASING_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_purchasing_service_parent_class)->finalize(object);
}

static void
venture_purchasing_service_class_init(VenturePurchasingServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_purchasing_service_init(VenturePurchasingService *self)
{
	(void)self;
}

VenturePurchasingService *
venture_purchasing_service_get(VentureDatabase *database)
{
	VenturePurchasingService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-purchasing-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_PURCHASING_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-purchasing-service", self, g_object_unref);
	}
	return self;
}

static gint64
get_id(VentureEntity *record, const gchar *field)
{
	gint64 id = 0;
	g_object_get(record, field, &id, NULL);
	return id;
}

static gint64
get_int(VentureEntity *record, const gchar *field)
{
	gint64 value = 0;
	g_object_get(record, field, &value, NULL);
	return value;
}

static gboolean
save_owned(VenturePurchasingService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	gboolean ok;
	self->writing = record;
	g_object_set_data(G_OBJECT(self->database), "venture-purchasing-writing", record);
	ok = venture_database_save(self->database, record, actor, error);
	g_object_set_data(G_OBJECT(self->database), "venture-purchasing-writing", previous);
	self->writing = previous;
	return ok;
}

static GPtrArray *
find_rows(VenturePurchasingService *self, GType type, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static VentureEntity *
load_po(VenturePurchasingService *self, gint64 id, GError **error)
{
	VentureEntity *po = venture_database_get(self->database, VENTURE_TYPE_PURCHASE_ORDER, id, error);
	if (po == NULL)
		refuse(error, "purchase order not found");
	return po;
}

static gchar *
status_of(VentureEntity *record)
{
	gchar *status = NULL;
	g_object_get(record, "status", &status, NULL);
	return status;
}

static gboolean
set_status(VenturePurchasingService *self, VentureEntity *po, const gchar *status,
	const VentureActor *actor, GError **error)
{
	g_object_set(po, "status", status, NULL);
	return save_owned(self, po, actor, error);
}

static gint64
received_for_line(VenturePurchasingService *self, gint64 po_line_id, GError **error)
{
	g_autoptr(GPtrArray) rows = find_rows(self, VENTURE_TYPE_GOODS_RECEIPT_LINE,
		"purchase-order-line-id", po_line_id, error);
	gint64 total = 0;
	guint i;
	if (rows == NULL)
		return 0;
	for (i = 0; i < rows->len; i++)
	{
		g_autoptr(VentureEntity) receipt = NULL;
		g_autofree gchar *status = NULL;
		receipt = venture_database_get(self->database, VENTURE_TYPE_GOODS_RECEIPT,
			get_id(g_ptr_array_index(rows, i), "goods-receipt-id"), error);
		if (receipt == NULL)
			return 0;
		g_object_get(receipt, "status", &status, NULL);
		if (g_strcmp0(status, "cancelled") == 0)
			continue;
		total += get_int(g_ptr_array_index(rows, i), "quantity") -
			get_int(g_ptr_array_index(rows, i), "returned-qty");
	}
	return total;
}

static gboolean
refresh_po_status(VenturePurchasingService *self, VentureEntity *po, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	gint64 ordered = 0, received = 0;
	guint i;
	lines = find_rows(self, VENTURE_TYPE_PURCHASE_ORDER_LINE, "purchase-order-id",
		venture_entity_get_id(po), error);
	if (lines == NULL)
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		gint64 qty = get_int(g_ptr_array_index(lines, i), "quantity");
		gint64 got = received_for_line(self, venture_entity_get_id(g_ptr_array_index(lines, i)), error);
		if (error != NULL && *error != NULL)
			return FALSE;
		ordered += qty;
		received += got;
	}
	if (received <= 0)
		return set_status(self, po, "sent", actor, error);
	return set_status(self, po, received >= ordered ? "received" : "partial", actor, error);
}

static gchar *
next_number(VenturePurchasingService *self, gint64 org, GType type, const gchar *prefix)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	gint64 count;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	count = venture_database_count(self->database, query, NULL);
	return g_strdup_printf("%s%" G_GINT64_FORMAT, prefix, count + 1);
}

gboolean
venture_purchasing_service_approve(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) po = NULL;
	g_autofree gchar *status = NULL;
	g_return_val_if_fail(VENTURE_IS_PURCHASING_SERVICE(self), FALSE);
	(void)date;
	po = load_po(self, purchase_order_id, error);
	if (po == NULL)
		return FALSE;
	status = status_of(po);
	if (g_strcmp0(status, "draft") != 0)
		return refuse(error, "only a draft purchase order can be approved");
	return set_status(self, po, "approved", actor, error);
}

gboolean
venture_purchasing_service_send(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) po = NULL;
	g_autofree gchar *status = NULL;
	g_return_val_if_fail(VENTURE_IS_PURCHASING_SERVICE(self), FALSE);
	(void)date;
	po = load_po(self, purchase_order_id, error);
	if (po == NULL)
		return FALSE;
	status = status_of(po);
	if (g_strcmp0(status, "approved") != 0 && g_strcmp0(status, "sent") != 0)
		return refuse(error, "approve the purchase order before sending it");
	return set_status(self, po, "sent", actor, error);
}

static gboolean
venture_purchasing_service_receive_line_impl(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) po = NULL;
	g_autoptr(VentureGoodsReceipt) receipt = NULL;
	g_autoptr(VentureGoodsReceiptLine) receipt_line = NULL;
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *number = NULL;
	gint64 ordered, already, item_id, org;
	g_return_val_if_fail(VENTURE_IS_PURCHASING_SERVICE(self), FALSE);
	if (quantity <= 0)
		return refuse(error, "receive a positive quantity");
	line = venture_database_get(self->database, VENTURE_TYPE_PURCHASE_ORDER_LINE, purchase_order_line_id, error);
	if (line == NULL)
		return FALSE;
	po = load_po(self, get_id(line, "purchase-order-id"), error);
	if (po == NULL)
		return FALSE;
	status = status_of(po);
	if (g_strcmp0(status, "sent") != 0 && g_strcmp0(status, "partial") != 0 && g_strcmp0(status, "approved") != 0)
		return refuse(error, "receive against a sent purchase order");
	ordered = get_int(line, "quantity");
	already = received_for_line(self, purchase_order_line_id, error);
	if (error != NULL && *error != NULL)
		return FALSE;
	if (already + quantity > ordered)
		return refuse(error, "cannot receive more than the ordered quantity");
	g_object_get(line, "unit-price", &unit, NULL);
	item_id = get_id(line, "inventory-item-id");
	if (item_id <= 0 || unit == NULL)
		return refuse(error, "a receipt line needs an inventory item and unit price");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	org = venture_entity_get_organization_id(po);
	if (!venture_database_begin(self->database, error))
		return FALSE;
	receipt = venture_goods_receipt_new();
	number = next_number(self, org, VENTURE_TYPE_GOODS_RECEIPT, "GR-");
	g_object_set(receipt, "number", number, "purchase-order-id", venture_entity_get_id(po),
		"received-at", when, "status", "posted", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(receipt), org);
	if (!save_owned(self, VENTURE_ENTITY(receipt), actor, error))
		goto fail;
	receipt_line = venture_goods_receipt_line_new();
	g_object_set(receipt_line, "goods-receipt-id", venture_entity_get_id(VENTURE_ENTITY(receipt)),
		"purchase-order-line-id", purchase_order_line_id, "inventory-item-id", item_id,
		"quantity", quantity, "unit-cost", unit, "returned-qty", (gint64)0, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(receipt_line), org);
	if (!save_owned(self, VENTURE_ENTITY(receipt_line), actor, error))
		goto fail;
	if (!venture_inventory_service_receive(venture_inventory_service_get(self->database), item_id, quantity,
		unit, when, venture_entity_get_id(VENTURE_ENTITY(receipt_line)),
		number, actor, error))
		goto fail;
	if (!refresh_po_status(self, po, actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gint64
abs_amount(const VentureMoney *amount)
{
	gint64 value = venture_money_get_amount(amount);
	return value < 0 ? -value : value;
}

static gboolean
within_tolerance(const VentureMoney *expected, const VentureMoney *actual, gint64 percent, GError **error)
{
	g_autoptr(VentureMoney) diff = NULL;
	g_autoptr(VentureMoney) allowed = NULL;
	gint64 gap, limit;
	if (expected == NULL || actual == NULL)
		return refuse(error, "three-way match needs amounts");
	diff = venture_money_subtract(expected, actual, error);
	if (diff == NULL)
		return FALSE;
	allowed = venture_money_multiply_rational(expected, percent, 100, error);
	if (allowed == NULL)
		return FALSE;
	gap = abs_amount(diff);
	limit = abs_amount(allowed);
	return gap <= limit;
}

static gboolean
match_result(VenturePurchasingService *self, VentureEntity *po, VentureEntity *bill,
	gboolean *matched, GError **error)
{
	g_autoptr(GPtrArray) po_lines = NULL;
	g_autoptr(GPtrArray) bill_lines = NULL;
	g_autoptr(VentureMoney) received_value = NULL;
	g_autoptr(VentureMoney) billed_value = NULL;
	gint64 tolerance;
	guint i;
	*matched = FALSE;
	g_object_get(po, "match-tolerance-percent", &tolerance, NULL);
	if (tolerance < 0)
		tolerance = 0;
	po_lines = find_rows(self, VENTURE_TYPE_PURCHASE_ORDER_LINE, "purchase-order-id",
		venture_entity_get_id(po), error);
	bill_lines = find_rows(self, VENTURE_TYPE_VENDOR_BILL_LINE, "bill-id",
		venture_entity_get_id(bill), error);
	if (po_lines == NULL || bill_lines == NULL)
		return FALSE;
	for (i = 0; i < po_lines->len; i++)
	{
		g_autoptr(VentureMoney) unit = NULL;
		g_autoptr(VentureMoney) slice = NULL;
		gint64 received = received_for_line(self, venture_entity_get_id(g_ptr_array_index(po_lines, i)), error);
		if (error != NULL && *error != NULL)
			return FALSE;
		g_object_get(g_ptr_array_index(po_lines, i), "unit-price", &unit, NULL);
		if (unit == NULL || received < 0)
			return refuse(error, "three-way match needs received quantity and price");
		slice = venture_money_multiply_int(unit, received, error);
		if (slice == NULL)
			return FALSE;
		if (received_value == NULL)
			received_value = venture_money_copy(slice);
		else
		{
			VentureMoney *next = venture_money_add(received_value, slice, error);
			if (next == NULL)
				return FALSE;
			venture_money_free(received_value);
			received_value = next;
		}
	}
	for (i = 0; i < bill_lines->len; i++)
	{
		g_autoptr(VentureMoney) amount = venture_vendor_bill_line_get_amount(
			g_ptr_array_index(bill_lines, i), error);
		gint64 po_line_id = 0;
		if (amount == NULL)
			return FALSE;
		if (g_object_class_find_property(G_OBJECT_GET_CLASS(g_ptr_array_index(bill_lines, i)),
			"purchase-order-line-id") != NULL)
			po_line_id = get_id(g_ptr_array_index(bill_lines, i), "purchase-order-line-id");
		if (po_line_id != 0)
		{
			gint64 received = received_for_line(self, po_line_id, error);
			g_autofree gchar *qty = NULL;
			gint64 billed_milli = 0;
			const gchar *p;
			gint64 numerator = 0;
			gint64 denominator = 1;
			guint decimals = 0;
			gboolean point = FALSE;
			gboolean digit = FALSE;
			g_object_get(g_ptr_array_index(bill_lines, i), "quantity", &qty, NULL);
			for (p = qty != NULL ? qty : ""; *p != '\0'; p++)
			{
				if (*p == '.' && !point)
				{
					point = TRUE;
					continue;
				}
				if (!g_ascii_isdigit(*p) || numerator > (G_MAXINT64 - (*p - '0')) / 10)
				{
					digit = FALSE;
					break;
				}
				digit = TRUE;
				numerator = numerator * 10 + (*p - '0');
				if (point)
				{
					if (++decimals > 3)
					{
						digit = FALSE;
						break;
					}
					denominator *= 10;
				}
			}
			if (digit && denominator > 0 && 1000 % denominator == 0)
				billed_milli = numerator * (1000 / denominator);
			if (billed_milli > received * 1000)
				return TRUE;
		}
		if (billed_value == NULL)
			billed_value = g_steal_pointer(&amount);
		else
		{
			VentureMoney *next = venture_money_add(billed_value, amount, error);
			if (next == NULL)
				return FALSE;
			venture_money_free(billed_value);
			billed_value = next;
		}
	}
	if (received_value == NULL || billed_value == NULL)
		return TRUE;
	if (error != NULL && *error != NULL)
		return FALSE;
	if (!within_tolerance(received_value, billed_value, tolerance, error))
	{
		if (error != NULL && *error != NULL)
			return FALSE;
		*matched = FALSE;
		return TRUE;
	}
	*matched = TRUE;
	return TRUE;
}

gboolean
venture_purchasing_service_match(VenturePurchasingService *self, gint64 purchase_order_id,
	gint64 vendor_bill_id, gboolean exception, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) po = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(GPtrArray) bill_lines = NULL;
	gboolean matched = FALSE;
	gint64 grni = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_PURCHASING_SERVICE(self), FALSE);
	po = load_po(self, purchase_order_id, error);
	bill = venture_database_get(self->database, VENTURE_TYPE_VENDOR_BILL, vendor_bill_id, error);
	if (po == NULL || bill == NULL)
		return FALSE;
	if (get_id(po, "vendor-id") != get_id(bill, "company-id"))
		return refuse(error, "the bill vendor must match the purchase order");
	if (!match_result(self, po, bill, &matched, error))
		return FALSE;
	if (!matched && !exception)
		g_object_set(po, "match-status", "mismatch", "match-exception", FALSE,
			"vendor-bill-id", vendor_bill_id, NULL);
	else if (!matched && exception)
		g_object_set(po, "match-status", "exception", "match-exception", TRUE,
			"vendor-bill-id", vendor_bill_id, NULL);
	else
		g_object_set(po, "match-status", "matched", "match-exception", FALSE,
			"vendor-bill-id", vendor_bill_id, NULL);
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(bill), "purchase-order-id") != NULL)
		g_object_set(bill, "purchase-order-id", purchase_order_id, NULL);
	/* Linking the order, allocating GRNI and linking the bill are one match result. */
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!save_owned(self, po, actor, error))
		goto fail;
	{
		g_autoptr(GError) ignored = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) found = NULL;
		g_autofree gchar *scoped = g_strdup_printf("%" G_GINT64_FORMAT ":2010",
			venture_entity_get_organization_id(po));
		grni = venture_setup_resolve_account(self->database, venture_entity_get_organization_id(po),
			"grni", "organization", 0, NULL, &ignored);
		if (grni == 0)
		{
			query = venture_query_new(VENTURE_TYPE_ACCOUNT);
			venture_query_set_organization(query, venture_entity_get_organization_id(po));
			venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "2010", NULL);
			found = venture_database_find_one(self->database, query, NULL);
			if (found == NULL)
			{
				g_clear_object(&query);
				query = venture_query_new(VENTURE_TYPE_ACCOUNT);
				venture_query_set_organization(query, venture_entity_get_organization_id(po));
				venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped, NULL);
				found = venture_database_find_one(self->database, query, NULL);
			}
			if (found != NULL)
				grni = venture_entity_get_id(found);
		}
		if (grni == 0)
		{
			refuse(error, "the goods-received-not-invoiced control account is missing");
			goto fail;
		}
	}
	bill_lines = find_rows(self, VENTURE_TYPE_VENDOR_BILL_LINE, "bill-id", vendor_bill_id, error);
	if (bill_lines == NULL)
		goto fail;
	for (i = 0; i < bill_lines->len; i++)
	{
		if (grni != 0)
			g_object_set(g_ptr_array_index(bill_lines, i), "account-id", grni, NULL);
		if (!save_owned(self, g_ptr_array_index(bill_lines, i), actor, error))
			goto fail;
	}
	if (!save_owned(self, bill, actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_purchasing_check_bill_approval(VentureDatabase *database, VentureEntity *bill, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) po = NULL;
	gint64 po_id = 0;
	g_autofree gchar *match = NULL;
	gboolean exception = FALSE;
	if (bill == NULL || database == NULL)
		return TRUE;
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(bill), "purchase-order-id") != NULL)
		g_object_get(bill, "purchase-order-id", &po_id, NULL);
	if (po_id <= 0)
	{
		query = venture_query_new(VENTURE_TYPE_PURCHASE_ORDER);
		venture_query_set_organization(query, venture_entity_get_organization_id(bill));
		if (!venture_query_add_filter_int(query, "vendor-bill-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(bill), error))
			return FALSE;
		po = venture_database_find_one(database, query, error);
	}
	else
		po = venture_database_get(database, VENTURE_TYPE_PURCHASE_ORDER, po_id, error);
	if (po == NULL && (error == NULL || *error == NULL))
	{
		g_autoptr(VentureQuery) lines = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		venture_query_set_limit(lines, 0);
		if (!venture_query_add_filter_int(lines, "bill-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(bill), error))
			return FALSE;
		rows = venture_database_find(database, lines, error);
		if (rows == NULL)
			return FALSE;
		for (i = 0; i < rows->len && po == NULL; i++)
		{
			gint64 line_id = 0;
			g_autoptr(VentureEntity) po_line = NULL;
			if (g_object_class_find_property(G_OBJECT_GET_CLASS(g_ptr_array_index(rows, i)),
				"purchase-order-line-id") == NULL)
				continue;
			g_object_get(g_ptr_array_index(rows, i), "purchase-order-line-id", &line_id, NULL);
			if (line_id <= 0)
				continue;
			po_line = venture_database_get(database, VENTURE_TYPE_PURCHASE_ORDER_LINE, line_id, error);
			if (po_line == NULL)
			{
				if (error != NULL && *error != NULL)
					return FALSE;
				continue;
			}
			po_id = 0;
			g_object_get(po_line, "purchase-order-id", &po_id, NULL);
			if (po_id > 0)
				po = venture_database_get(database, VENTURE_TYPE_PURCHASE_ORDER, po_id, error);
		}
	}
	if (po == NULL)
		return error != NULL && *error != NULL ? FALSE : TRUE;
	g_object_get(po, "match-status", &match, "match-exception", &exception, NULL);
	if (g_strcmp0(match, "matched") == 0)
		return TRUE;
	if (g_strcmp0(match, "exception") == 0 && exception)
		return TRUE;
	if (g_strcmp0(match, "mismatch") == 0)
		return refuse(error, "three-way match failed; approve a match exception first");
	return refuse(error, "match the supplier bill to the purchase order before approval");
}

gboolean
venture_purchasing_service_cancel(VenturePurchasingService *self, gint64 purchase_order_id,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) po = NULL;
	g_autofree gchar *status = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_PURCHASING_SERVICE(self), FALSE);
	(void)date;
	po = load_po(self, purchase_order_id, error);
	if (po == NULL)
		return FALSE;
	status = status_of(po);
	if (g_strcmp0(status, "cancelled") == 0 || g_strcmp0(status, "received") == 0)
		return refuse(error, "this purchase order cannot be cancelled");
	lines = find_rows(self, VENTURE_TYPE_PURCHASE_ORDER_LINE, "purchase-order-id", purchase_order_id, error);
	if (lines == NULL)
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		if (received_for_line(self, venture_entity_get_id(g_ptr_array_index(lines, i)), error) > 0)
			return refuse(error, "return received quantity before cancelling");
		if (error != NULL && *error != NULL)
			return FALSE;
	}
	return set_status(self, po, "cancelled", actor, error);
}

static gboolean
venture_purchasing_service_return_line_impl(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) when = NULL;
	gint64 remaining = quantity;
	guint i;
	g_return_val_if_fail(VENTURE_IS_PURCHASING_SERVICE(self), FALSE);
	if (quantity <= 0)
		return refuse(error, "return a positive quantity");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	rows = find_rows(self, VENTURE_TYPE_GOODS_RECEIPT_LINE, "purchase-order-line-id",
		purchase_order_line_id, error);
	if (rows == NULL)
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	for (i = 0; i < rows->len && remaining > 0; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) unit = NULL;
		gint64 open = get_int(row, "quantity") - get_int(row, "returned-qty");
		gint64 take;
		if (open <= 0)
			continue;
		take = open < remaining ? open : remaining;
		g_object_get(row, "unit-cost", &unit, NULL);
		g_object_set(row, "returned-qty", get_int(row, "returned-qty") + take, NULL);
		if (!save_owned(self, row, actor, error) ||
			!venture_inventory_service_restore(venture_inventory_service_get(self->database),
				get_id(row, "inventory-item-id"), take, unit, when,
				venture_entity_get_id(row), "return", actor, error))
		{
			venture_database_rollback(self->database);
			return FALSE;
		}
		remaining -= take;
	}
	if (remaining > 0)
	{
		venture_database_rollback(self->database);
		return refuse(error, "cannot return more than was received");
	}
	{
		g_autoptr(VentureEntity) line = venture_database_get(self->database, VENTURE_TYPE_PURCHASE_ORDER_LINE,
			purchase_order_line_id, error);
		g_autoptr(VentureEntity) po = NULL;
		if (line == NULL)
		{
			venture_database_rollback(self->database);
			return FALSE;
		}
		po = load_po(self, get_id(line, "purchase-order-id"), error);
		if (po == NULL || !refresh_po_status(self, po, actor, error))
		{
			venture_database_rollback(self->database);
			return FALSE;
		}
	}
	return venture_database_commit(self->database, error);
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_purchasing_service_receive_line(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	gboolean result;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_PURCHASE_ORDER_LINE, purchase_order_line_id, error);
	if (subject == NULL)
		return FALSE;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "purchase_order_line_id", g_variant_new_int64((gint64)purchase_order_line_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64((gint64)quantity));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	operation = venture_accounting_operation_begin(db, "purchasing-receive-line", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_purchasing_service_receive_line_impl(self, purchase_order_line_id, quantity, date, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
gboolean
venture_purchasing_service_return_line(VenturePurchasingService *self, gint64 purchase_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	gboolean result;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return FALSE;
	}
	subject = venture_database_get(db, VENTURE_TYPE_PURCHASE_ORDER_LINE, purchase_order_line_id, error);
	if (subject == NULL)
		return FALSE;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "purchase_order_line_id", g_variant_new_int64((gint64)purchase_order_line_id));
	g_variant_builder_add(&arguments, "{sv}", "quantity", g_variant_new_int64((gint64)quantity));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	operation = venture_accounting_operation_begin(db, "purchasing-return-line", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return FALSE;
	result = venture_purchasing_service_return_line_impl(self, purchase_order_line_id, quantity, date, actor, error);
	if (!result)
		return FALSE;
	if (!venture_accounting_operation_finish(operation, error))
		return FALSE;
	return result;
}
