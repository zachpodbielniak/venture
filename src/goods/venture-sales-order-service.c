/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureSalesOrderService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureSalesOrderService, venture_sales_order_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureSalesOrderService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_SALES_ORDER_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureSalesOrderService *self = VENTURE_SALES_ORDER_SERVICE(object);
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
	VentureSalesOrderService *self = VENTURE_SALES_ORDER_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_sales_order_service_parent_class)->finalize(object);
}

static void
venture_sales_order_service_class_init(VentureSalesOrderServiceClass *klass)
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
venture_sales_order_service_init(VentureSalesOrderService *self)
{
	(void)self;
}

VentureSalesOrderService *
venture_sales_order_service_get(VentureDatabase *database)
{
	VentureSalesOrderService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-sales-order-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_SALES_ORDER_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-sales-order-service", self, g_object_unref);
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
save_owned(VentureSalesOrderService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	gboolean ok;
	self->writing = record;
	g_object_set_data(G_OBJECT(self->database), "venture-sales-order-writing", record);
	ok = venture_database_save(self->database, record, actor, error);
	g_object_set_data(G_OBJECT(self->database), "venture-sales-order-writing", previous);
	self->writing = previous;
	return ok;
}

static GPtrArray *
find_rows(VentureSalesOrderService *self, GType type, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static gboolean
refresh_status(VentureSalesOrderService *self, VentureEntity *order, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	gint64 ordered = 0, allocated = 0, fulfilled = 0, invoiced = 0;
	guint i;
	const gchar *status = "draft";
	lines = find_rows(self, VENTURE_TYPE_SALES_ORDER_LINE, "sales-order-id",
		venture_entity_get_id(order), error);
	if (lines == NULL)
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		ordered += get_int(g_ptr_array_index(lines, i), "quantity");
		allocated += get_int(g_ptr_array_index(lines, i), "allocated-qty");
		fulfilled += get_int(g_ptr_array_index(lines, i), "fulfilled-qty");
		invoiced += get_int(g_ptr_array_index(lines, i), "invoiced-qty");
	}
	if (invoiced >= ordered && ordered > 0)
		status = "invoiced";
	else if (fulfilled >= ordered && ordered > 0)
		status = "fulfilled";
	else if (fulfilled > 0)
		status = "partial";
	else if (allocated > 0)
		status = "allocated";
	g_object_set(order, "status", status, NULL);
	return save_owned(self, order, actor, error);
}

gboolean
venture_sales_order_service_allocate(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) order = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_SALES_ORDER_SERVICE(self), FALSE);
	(void)date;
	order = venture_database_get(self->database, VENTURE_TYPE_SALES_ORDER, sales_order_id, error);
	if (order == NULL)
		return FALSE;
	lines = find_rows(self, VENTURE_TYPE_SALES_ORDER_LINE, "sales-order-id", sales_order_id, error);
	if (lines == NULL)
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		gboolean service = FALSE;
		gint64 item_id, qty, on_hand;
		g_object_get(line, "is-service", &service, NULL);
		if (service)
		{
			g_object_set(line, "allocated-qty", get_int(line, "quantity"), NULL);
			if (!save_owned(self, line, actor, error))
				goto fail;
			continue;
		}
		item_id = get_id(line, "inventory-item-id");
		qty = get_int(line, "quantity");
		if (item_id <= 0)
			goto fail_msg;
		on_hand = venture_inventory_service_on_hand(venture_inventory_service_get(self->database),
			item_id, NULL, error);
		if (error != NULL && *error != NULL)
			goto fail;
		if (on_hand < qty)
		{
			refuse(error, "not enough stock to allocate");
			goto fail;
		}
		g_object_set(line, "allocated-qty", qty, NULL);
		if (!save_owned(self, line, actor, error))
			goto fail;
	}
	if (!refresh_status(self, order, actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail_msg:
	refuse(error, "stocked lines need an inventory item");
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_sales_order_service_ship_line(VentureSalesOrderService *self, gint64 sales_order_line_id,
	gint64 quantity, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) order = NULL;
	g_autoptr(VentureFulfillment) fulfillment = NULL;
	g_autoptr(GDateTime) when = NULL;
	gboolean service = FALSE;
	gint64 allocated, fulfilled, open, item_id;
	g_return_val_if_fail(VENTURE_IS_SALES_ORDER_SERVICE(self), FALSE);
	if (quantity <= 0)
		return refuse(error, "ship a positive quantity");
	line = venture_database_get(self->database, VENTURE_TYPE_SALES_ORDER_LINE, sales_order_line_id, error);
	if (line == NULL)
		return FALSE;
	g_object_get(line, "is-service", &service, NULL);
	if (service)
		return refuse(error, "service lines are not shipped");
	allocated = get_int(line, "allocated-qty");
	fulfilled = get_int(line, "fulfilled-qty");
	open = allocated - fulfilled;
	if (quantity > open)
		return refuse(error, "cannot ship more than the allocated remainder");
	item_id = get_id(line, "inventory-item-id");
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	order = venture_database_get(self->database, VENTURE_TYPE_SALES_ORDER, get_id(line, "sales-order-id"), error);
	if (order == NULL)
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!venture_inventory_service_issue(venture_inventory_service_get(self->database), item_id, quantity, when,
		"fulfillment", sales_order_line_id, actor, NULL, error))
		goto fail;
	fulfillment = venture_fulfillment_new();
	g_object_set(fulfillment, "sales-order-id", venture_entity_get_id(order),
		"sales-order-line-id", sales_order_line_id, "quantity", quantity,
		"shipped-at", when, "status", "shipped", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(fulfillment), venture_entity_get_organization_id(order));
	if (!save_owned(self, VENTURE_ENTITY(fulfillment), actor, error))
		goto fail;
	g_object_set(line, "fulfilled-qty", fulfilled + quantity, NULL);
	if (!save_owned(self, line, actor, error) || !refresh_status(self, order, actor, error) ||
		!venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean
invoice_qty(VentureSalesOrderService *self, gint64 sales_order_id, gboolean fulfilled_only,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) order = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) spec = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GDateTime) when = NULL;
	gboolean any = FALSE;
	guint i;
	order = venture_database_get(self->database, VENTURE_TYPE_SALES_ORDER, sales_order_id, error);
	if (order == NULL)
		return FALSE;
	lines = find_rows(self, VENTURE_TYPE_SALES_ORDER_LINE, "sales-order-id", sales_order_id, error);
	if (lines == NULL)
		return FALSE;
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "company_id");
	json_builder_add_int_value(builder, get_id(order, "company-id"));
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autofree gchar *description = NULL;
		g_autoptr(VentureMoney) price = NULL;
		g_autofree gchar *price_text = NULL;
		gboolean service = FALSE;
		gint64 billable, invoiced, remaining;
		g_object_get(line, "is-service", &service, "description", &description, "unit-price", &price, NULL);
		invoiced = get_int(line, "invoiced-qty");
		billable = service ? get_int(line, "quantity") : get_int(line, "fulfilled-qty");
		if (!service && !fulfilled_only)
			billable = get_int(line, "quantity");
		remaining = billable - invoiced;
		if (remaining <= 0)
			continue;
		if (!service && get_int(line, "fulfilled-qty") < remaining && !service)
		{
			if (!fulfilled_only)
				return refuse(error, "cannot invoice more than the fulfilled quantity");
		}
		if (!service && fulfilled_only == FALSE && get_int(line, "fulfilled-qty") < get_int(line, "quantity") - invoiced)
			return refuse(error, "cannot invoice more than the fulfilled quantity");
		any = TRUE;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder, description != NULL ? description : "Item");
		json_builder_set_member_name(builder, "quantity");
		json_builder_add_int_value(builder, remaining);
		json_builder_set_member_name(builder, "product_id");
		json_builder_add_int_value(builder, get_id(line, "product-id"));
		if (price != NULL)
		{
			price_text = venture_money_to_string(price);
			json_builder_set_member_name(builder, "unit_price");
			json_builder_add_string_value(builder, price_text);
		}
		json_builder_end_object(builder);
		g_object_set(line, "invoiced-qty", invoiced + remaining, NULL);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "send");
	json_builder_add_boolean_value(builder, FALSE);
	json_builder_end_object(builder);
	if (!any)
		return refuse(error, "nothing remains to invoice");
	if (!fulfilled_only)
	{
		for (i = 0; i < lines->len; i++)
		{
			gboolean service = FALSE;
			g_object_get(g_ptr_array_index(lines, i), "is-service", &service, NULL);
			if (!service && get_int(g_ptr_array_index(lines, i), "fulfilled-qty") <
				get_int(g_ptr_array_index(lines, i), "quantity"))
				return refuse(error, "cannot invoice more than the fulfilled quantity");
		}
	}
	spec = json_builder_get_root(builder);
	when = date != NULL ? g_date_time_ref(date) : venture_time_now();
	(void)when;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	invoice = venture_document_service_compose_invoice(venture_document_service_get(self->database),
		venture_entity_get_organization_id(order), json_node_get_object(spec), actor, error);
	if (invoice == NULL)
		goto fail;
	for (i = 0; i < lines->len; i++)
	{
		if (!save_owned(self, g_ptr_array_index(lines, i), actor, error))
			goto fail;
	}
	g_object_set(order, "invoice-id", venture_entity_get_id(invoice), NULL);
	if (!save_owned(self, order, actor, error))
		goto fail;
	if (!venture_settlement_service_transition(venture_settlement_service_get(self->database),
		VENTURE_INVOICE(invoice), "sent", date, actor, error))
		goto fail;
	if (!refresh_status(self, order, actor, error) || !venture_database_commit(self->database, error))
		goto fail;
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_sales_order_service_invoice(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_SALES_ORDER_SERVICE(self), FALSE);
	return invoice_qty(self, sales_order_id, FALSE, date, actor, error);
}

gboolean
venture_sales_order_service_invoice_fulfilled(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_SALES_ORDER_SERVICE(self), FALSE);
	return invoice_qty(self, sales_order_id, TRUE, date, actor, error);
}

gboolean
venture_sales_order_service_cancel(VentureSalesOrderService *self, gint64 sales_order_id,
	GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) order = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_SALES_ORDER_SERVICE(self), FALSE);
	(void)date;
	order = venture_database_get(self->database, VENTURE_TYPE_SALES_ORDER, sales_order_id, error);
	if (order == NULL)
		return FALSE;
	lines = find_rows(self, VENTURE_TYPE_SALES_ORDER_LINE, "sales-order-id", sales_order_id, error);
	if (lines == NULL)
		return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		if (get_int(g_ptr_array_index(lines, i), "fulfilled-qty") > 0)
			return refuse(error, "fulfilled sales orders cannot be cancelled");
	}
	g_object_set(order, "status", "cancelled", NULL);
	return save_owned(self, order, actor, error);
}
