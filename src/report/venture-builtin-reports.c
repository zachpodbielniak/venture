/*
 * venture-builtin-reports.c - The reports VENTURE ships with
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each report is a function taking a period and options and returning a
 * result. They aggregate in C rather than in SQL wherever money is involved,
 * because the arithmetic has to be exact and currency-aware, and a SUM over
 * mixed currencies would produce a confidently wrong number.
 */

#include "venture.h"

#include <string.h>

/*
 * Builds a query scoped the way every report wants: the caller's period on
 * the record type's own date field, and the organisation from the options or
 * the context's default.
 */
static VentureQuery *
venture_report_scoped_query(
	VentureContext		 *context,
	GType			  entity_type,
	const gchar		 *date_field,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;
	gint64 organization_id;
	gint64 venture_id;

	query = venture_query_new(entity_type);

	organization_id = (NULL != options)
		? venture_json_object_get_int(options, "organization_id", 0) : 0;

	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(context);

	venture_query_set_organization(query, organization_id);

	if ((NULL != period) && (NULL != date_field))
	{
		if (!venture_query_set_date_range(query, date_field, period, error))
			return NULL;
	}

	/* Most reports are read per venture as often as across the whole
	 * portfolio, so the filter belongs here rather than in each of them. */
	venture_id = (NULL != options)
		? venture_json_object_get_int(options, "venture_id", 0) : 0;

	if (0 != venture_id)
	{
		if (!venture_query_add_filter_int(query, "venture-id",
		                                  VENTURE_FILTER_OP_EQ, venture_id,
		                                  error))
			return NULL;
	}

	return g_steal_pointer(&query);
}

/*
 * Totals a money property across a set of records, skipping any in a
 * different currency from the first and reporting how many were skipped.
 *
 * Silently adding across currencies is the failure mode this exists to
 * prevent: the result would look authoritative and be wrong by whatever the
 * exchange rate happens to be.
 */
static VentureMoney *
venture_report_total(
	GPtrArray	 *records,
	const gchar	 *property,
	guint		 *out_skipped
){
	g_autoptr(VentureMoney) total = NULL;
	const gchar *currency = NULL;
	guint skipped;
	guint i;

	skipped = 0;

	for (i = 0; i < records->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) next = NULL;

		g_object_get(g_ptr_array_index(records, i), property, &amount, NULL);

		if (NULL == amount)
			continue;

		if (NULL == total)
		{
			currency = venture_money_get_currency(amount);
			total = venture_money_copy(amount);
			continue;
		}

		if (0 != g_strcmp0(currency, venture_money_get_currency(amount)))
		{
			skipped++;
			continue;
		}

		next = venture_money_add(total, amount, NULL);

		if (NULL == next)
		{
			skipped++;
			continue;
		}

		g_clear_pointer(&total, venture_money_free);
		total = g_steal_pointer(&next);
	}

	if (NULL != out_skipped)
		*out_skipped = skipped;

	if (NULL == total)
		return venture_money_new_zero(NULL);

	return g_steal_pointer(&total);
}

/*
 * Totals the net proceeds of a set of sales, which is what the P&L counts as
 * revenue -- not the gross, which includes money that was never yours.
 */
static VentureMoney *
venture_report_total_net(GPtrArray *sales)
{
	g_autoptr(VentureMoney) total = NULL;
	guint i;

	total = venture_money_new_zero(NULL);

	for (i = 0; i < sales->len; i++)
	{
		g_autoptr(VentureMoney) net = NULL;
		g_autoptr(VentureMoney) next = NULL;

		net = venture_sale_get_net(g_ptr_array_index(sales, i), NULL);

		if (NULL == net)
			continue;

		next = venture_money_add(total, net, NULL);

		if (NULL == next)
			continue;

		g_clear_pointer(&total, venture_money_free);
		total = g_steal_pointer(&next);
	}

	return g_steal_pointer(&total);
}

/*
 * Fetches every record matching a query, without the paging limit reports
 * must not inherit.
 */
static GPtrArray *
venture_report_fetch_all(
	VentureContext	 *context,
	VentureQuery	 *query,
	GError		**error
){
	venture_query_set_limit(query, 0);

	return venture_database_find(venture_context_get_database(context),
	                             query, error);
}

/* ==========================================================================
 * Profit and loss
 * ========================================================================== */

static VentureReportResult *
venture_report_pnl(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) sales_query = NULL;
	g_autoptr(VentureQuery) expenses_query = NULL;
	g_autoptr(GPtrArray) sales = NULL;
	g_autoptr(GPtrArray) expenses = NULL;
	g_autoptr(VentureMoney) revenue = NULL;
	g_autoptr(VentureMoney) gross = NULL;
	g_autoptr(VentureMoney) fees = NULL;
	g_autoptr(VentureMoney) shipping = NULL;
	g_autoptr(VentureMoney) refunds = NULL;
	g_autoptr(VentureMoney) expense_total = NULL;
	g_autoptr(VentureMoney) deductible = NULL;
	g_autoptr(VentureMoney) profit = NULL;
	g_autofree gchar *title = NULL;
	guint i;

	sales_query = venture_report_scoped_query(context, VENTURE_TYPE_SALE,
	                                          "occurred-at", period, options,
	                                          error);

	if (NULL == sales_query)
		return NULL;

	expenses_query = venture_report_scoped_query(context, VENTURE_TYPE_EXPENSE,
	                                             "occurred-at", period, options,
	                                             error);

	if (NULL == expenses_query)
		return NULL;

	sales = venture_report_fetch_all(context, sales_query, error);

	if (NULL == sales)
		return NULL;

	expenses = venture_report_fetch_all(context, expenses_query, error);

	if (NULL == expenses)
		return NULL;

	revenue = venture_report_total_net(sales);
	gross = venture_report_total(sales, "gross", NULL);
	fees = venture_report_total(sales, "fees", NULL);
	shipping = venture_report_total(sales, "shipping-cost", NULL);
	refunds = venture_report_total(sales, "refunded", NULL);
	expense_total = venture_report_total(expenses, "amount", NULL);

	/* The deductible total is tracked separately from the cash total,
	 * because they are different questions: what left the account, and
	 * what may be claimed. */
	deductible = venture_money_new_zero(
		venture_money_get_currency(expense_total));

	for (i = 0; i < expenses->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) next = NULL;

		amount = venture_expense_get_deductible_amount(
			g_ptr_array_index(expenses, i), NULL);

		if (NULL == amount)
			continue;

		next = venture_money_add(deductible, amount, NULL);

		if (NULL == next)
			continue;

		g_clear_pointer(&deductible, venture_money_free);
		deductible = g_steal_pointer(&next);
	}

	profit = venture_money_subtract(revenue, expense_total, NULL);

	title = g_strdup_printf("Profit and loss");
	result = venture_report_result_new(title, period);

	venture_report_result_add_metric(result,
		venture_metric_new_money("revenue", "Net revenue", revenue));
	venture_report_result_add_metric(result,
		venture_metric_new_money("expenses", "Expenses", expense_total));
	venture_report_result_add_metric(result,
		venture_metric_new_money("profit", "Profit", profit));
	venture_report_result_add_metric(result,
		venture_metric_new_count("sales", "Sales", (gint64)sales->len));

	venture_report_result_add_column(result, "line", "Line",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "amount", "Amount",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line", "Gross sales");
	venture_report_result_set_money(result, "amount", gross);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line", "Less refunds");
	venture_report_result_set_money(result, "amount", refunds);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line", "Less platform fees");
	venture_report_result_set_money(result, "amount", fees);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line", "Less shipping cost");
	venture_report_result_set_money(result, "amount", shipping);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line", "Net revenue");
	venture_report_result_set_money(result, "amount", revenue);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line", "Expenses");
	venture_report_result_set_money(result, "amount", expense_total);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line",
	                               "Of which deductible");
	venture_report_result_set_money(result, "amount", deductible);

	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "line", "Profit");
	venture_report_result_set_money(result, "amount", profit);

	if (!venture_money_equal(expense_total, deductible))
	{
		venture_report_result_set_note(result,
			"The deductible total excludes capitalised costs and anything "
			"still marked for review. Review those before relying on this "
			"for tax.");
	}

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Venture performance
 * ========================================================================== */

static VentureReportResult *
venture_report_ventures(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) ventures_query = NULL;
	g_autoptr(GPtrArray) ventures = NULL;
	g_autoptr(VentureMoney) portfolio_revenue = NULL;
	guint i;

	ventures_query = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_organization(ventures_query,
		venture_context_get_default_organization_id(context));

	ventures = venture_report_fetch_all(context, ventures_query, error);

	if (NULL == ventures)
		return NULL;

	result = venture_report_result_new("Venture performance", period);
	portfolio_revenue = venture_money_new_zero(NULL);

	venture_report_result_add_column(result, "venture", "Venture",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "type", "Type",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sales", "Sales",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "revenue", "Net revenue",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "expenses", "Expenses",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "profit", "Profit",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < ventures->len; i++)
	{
		g_autoptr(VentureQuery) sales_query = NULL;
		g_autoptr(VentureQuery) expenses_query = NULL;
		g_autoptr(GPtrArray) sales = NULL;
		g_autoptr(GPtrArray) expenses = NULL;
		g_autoptr(VentureMoney) revenue = NULL;
		g_autoptr(VentureMoney) spent = NULL;
		g_autoptr(VentureMoney) profit = NULL;
		g_autoptr(VentureMoney) running = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *type = NULL;
		VentureEntity *venture;
		VentureVentureStatus status;
		gint64 venture_id;

		venture = g_ptr_array_index(ventures, i);
		venture_id = venture_entity_get_id(venture);

		g_object_get(venture, "name", &name, "venture-type", &type,
		             "status", &status, NULL);

		sales_query = venture_query_new(VENTURE_TYPE_SALE);

		if (!venture_query_add_filter_int(sales_query, "venture-id",
		                                  VENTURE_FILTER_OP_EQ, venture_id,
		                                  error))
			return NULL;

		if (!venture_query_set_date_range(sales_query, "occurred-at", period,
		                                  error))
			return NULL;

		expenses_query = venture_query_new(VENTURE_TYPE_EXPENSE);

		if (!venture_query_add_filter_int(expenses_query, "venture-id",
		                                  VENTURE_FILTER_OP_EQ, venture_id,
		                                  error))
			return NULL;

		if (!venture_query_set_date_range(expenses_query, "occurred-at",
		                                  period, error))
			return NULL;

		sales = venture_report_fetch_all(context, sales_query, error);
		expenses = venture_report_fetch_all(context, expenses_query, error);

		if ((NULL == sales) || (NULL == expenses))
			return NULL;

		revenue = venture_report_total_net(sales);
		spent = venture_report_total(expenses, "amount", NULL);
		profit = venture_money_subtract(revenue, spent, NULL);

		running = venture_money_add(portfolio_revenue, revenue, NULL);

		if (NULL != running)
		{
			g_clear_pointer(&portfolio_revenue, venture_money_free);
			portfolio_revenue = g_steal_pointer(&running);
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "venture", name);
		venture_report_result_set_text(result, "type", type);
		venture_report_result_set_text(result, "status",
			venture_enum_to_nick(VENTURE_TYPE_VENTURE_STATUS, (gint)status));
		venture_report_result_set_number(result, "sales", (gdouble)sales->len);
		venture_report_result_set_money(result, "revenue", revenue);
		venture_report_result_set_money(result, "expenses", spent);
		venture_report_result_set_money(result, "profit", profit);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("ventures", "Ventures",
		                         (gint64)ventures->len));
	venture_report_result_add_metric(result,
		venture_metric_new_money("revenue", "Portfolio revenue",
		                         portfolio_revenue));

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Category and genre performance
 * ========================================================================== */

/*
 * Groups sales by a product attribute -- genre by default, or any other
 * grouping field the caller names.
 *
 * Genre is the default because it is the question a book venture actually
 * asks: not "how did that title do" but "is this genre worth writing in".
 */
static VentureReportResult *
venture_report_categories(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) sales_query = NULL;
	g_autoptr(GPtrArray) sales = NULL;
	g_autoptr(GHashTable) products = NULL;
	g_autoptr(GHashTable) groups = NULL;
	g_autoptr(GList) keys = NULL;
	g_autofree gchar *title = NULL;
	const gchar *group_by;
	GList *iter;
	guint i;

	group_by = (NULL != options)
		? venture_json_object_get_string(options, "group_by", "genre")
		: "genre";

	sales_query = venture_report_scoped_query(context, VENTURE_TYPE_SALE,
	                                          "occurred-at", period, options,
	                                          error);

	if (NULL == sales_query)
		return NULL;

	sales = venture_report_fetch_all(context, sales_query, error);

	if (NULL == sales)
		return NULL;

	/* Products are cached as they are needed rather than loaded up front:
	 * a period's sales usually touch a small fraction of the catalogue. */
	products = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                 g_object_unref);
	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < sales->len; i++)
	{
		g_autoptr(VentureMoney) net = NULL;
		VentureEntity *sale;
		VentureEntity *product;
		GPtrArray *bucket;
		g_autofree gchar *group_value = NULL;
		gint64 product_id;

		sale = g_ptr_array_index(sales, i);
		g_object_get(sale, "product-id", &product_id, NULL);

		if (0 == product_id)
			continue;

		product = g_hash_table_lookup(products, &product_id);

		if (NULL == product)
		{
			gint64 *key;

			product = venture_database_get(
				venture_context_get_database(context),
				VENTURE_TYPE_PRODUCT, product_id, NULL);

			if (NULL == product)
				continue;

			key = g_new0(gint64, 1);
			*key = product_id;
			g_hash_table_insert(products, key, product);
		}

		{
			g_autofree gchar *property = NULL;

			property = venture_entity_column_to_property(group_by);

			if (NULL == g_object_class_find_property(
				G_OBJECT_GET_CLASS(product), property))
			{
				g_set_error(error, VENTURE_ERROR,
				            VENTURE_ERROR_INVALID_ARGUMENT,
				            "Products have no \"%s\" to group by", group_by);
				return NULL;
			}

			g_object_get(product, property, &group_value, NULL);
		}

		if (venture_string_is_empty(group_value))
		{
			g_free(group_value);
			group_value = g_strdup("(none)");
		}

		bucket = g_hash_table_lookup(groups, group_value);

		if (NULL == bucket)
		{
			bucket = g_ptr_array_new();
			g_hash_table_insert(groups, g_strdup(group_value), bucket);
		}

		g_ptr_array_add(bucket, sale);
		(void)net;
	}

	title = g_strdup_printf("Performance by %s", group_by);
	result = venture_report_result_new(title, period);

	venture_report_result_add_column(result, "group", group_by,
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sales", "Sales",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "units", "Units",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "revenue", "Net revenue",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "average", "Average sale",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	keys = g_hash_table_get_keys(groups);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);

	for (iter = keys; NULL != iter; iter = iter->next)
	{
		g_autoptr(VentureMoney) revenue = NULL;
		g_autoptr(GPtrArray) parts = NULL;
		GPtrArray *bucket;
		gint64 units;
		guint j;

		bucket = g_hash_table_lookup(groups, iter->data);
		revenue = venture_report_total_net(bucket);
		units = 0;

		for (j = 0; j < bucket->len; j++)
		{
			gint64 quantity;

			g_object_get(g_ptr_array_index(bucket, j), "quantity",
			             &quantity, NULL);
			units += quantity;
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "group", iter->data);
		venture_report_result_set_number(result, "sales", (gdouble)bucket->len);
		venture_report_result_set_number(result, "units", (gdouble)units);
		venture_report_result_set_money(result, "revenue", revenue);

		if (bucket->len > 0)
		{
			/* Splitting rather than dividing keeps the average exact
			 * and the parts summing back to the total. */
			parts = venture_money_allocate_evenly(revenue, bucket->len, NULL);

			if (NULL != parts)
			{
				venture_report_result_set_money(result, "average",
					g_ptr_array_index(parts, 0));
			}
		}
	}

	{
		GHashTableIter bucket_iter;
		gpointer value;

		g_hash_table_iter_init(&bucket_iter, groups);

		while (g_hash_table_iter_next(&bucket_iter, NULL, &value))
			g_ptr_array_unref(value);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("groups", "Distinct values",
		                         (gint64)g_list_length(keys)));

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Inventory
 * ========================================================================== */

static VentureReportResult *
venture_report_inventory(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) items_query = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(VentureMoney) total_value = NULL;
	gint64 below_reorder;
	guint i;

	items_query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_organization(items_query,
		venture_context_get_default_organization_id(context));

	items = venture_report_fetch_all(context, items_query, error);

	if (NULL == items)
		return NULL;

	result = venture_report_result_new("Inventory", period);
	total_value = venture_money_new_zero(NULL);
	below_reorder = 0;

	venture_report_result_add_column(result, "sku", "SKU",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "location", "Location",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "on_hand", "On hand",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "reorder_point", "Reorder at",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "unit_cost", "Unit cost",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "value", "Value",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < items->len; i++)
	{
		g_autoptr(VentureQuery) txn_query = NULL;
		g_autoptr(GPtrArray) transactions = NULL;
		g_autoptr(VentureMoney) unit_cost = NULL;
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(VentureMoney) running = NULL;
		g_autofree gchar *sku = NULL;
		g_autofree gchar *location = NULL;
		VentureEntity *item;
		gint64 on_hand;
		gint64 reorder_point;
		guint j;

		item = g_ptr_array_index(items, i);
		g_object_get(item, "sku", &sku, "location", &location,
		             "unit-cost", &unit_cost, "reorder-point", &reorder_point,
		             NULL);

		/*
		 * Quantity on hand is the sum of the signed transactions, never
		 * a stored field, so the figure is auditable and can be
		 * reconstructed for any past date.
		 */
		txn_query = venture_query_new(VENTURE_TYPE_INVENTORY_TXN);

		if (!venture_query_add_filter_int(txn_query, "inventory-item-id",
		                                  VENTURE_FILTER_OP_EQ,
		                                  venture_entity_get_id(item), error))
			return NULL;

		/* Bounded by the period's end but not its start: stock on hand
		 * is a running balance, not a movement within the window. */
		if ((NULL != period) && (NULL != venture_date_range_get_end(period)))
		{
			g_autofree gchar *text = NULL;

			text = venture_time_to_string(venture_date_range_get_end(period));

			if (!venture_query_add_filter_string(txn_query, "occurred-at",
			                                     VENTURE_FILTER_OP_LT, text,
			                                     error))
				return NULL;
		}

		transactions = venture_report_fetch_all(context, txn_query, error);

		if (NULL == transactions)
			return NULL;

		on_hand = 0;

		for (j = 0; j < transactions->len; j++)
		{
			gint64 quantity;

			g_object_get(g_ptr_array_index(transactions, j), "quantity",
			             &quantity, NULL);
			on_hand += quantity;
		}

		if (NULL != unit_cost)
			value = venture_money_multiply_int(unit_cost, on_hand, NULL);

		if (NULL != value)
		{
			running = venture_money_add(total_value, value, NULL);

			if (NULL != running)
			{
				g_clear_pointer(&total_value, venture_money_free);
				total_value = g_steal_pointer(&running);
			}
		}

		if ((reorder_point > 0) && (on_hand <= reorder_point))
			below_reorder++;

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "sku", sku);
		venture_report_result_set_text(result, "location", location);
		venture_report_result_set_number(result, "on_hand", (gdouble)on_hand);
		venture_report_result_set_number(result, "reorder_point",
		                                 (gdouble)reorder_point);
		venture_report_result_set_money(result, "unit_cost", unit_cost);
		venture_report_result_set_money(result, "value", value);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("items", "Tracked items",
		                         (gint64)items->len));
	venture_report_result_add_metric(result,
		venture_metric_new_money("value", "Inventory value", total_value));

	{
		VentureMetric *metric;

		metric = venture_metric_new_count("below_reorder", "Need reordering",
		                                  below_reorder);
		/* More items below their reorder point is bad news, so the UI
		 * must not colour a rise green. */
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Tax
 * ========================================================================== */

static VentureReportResult *
venture_report_tax(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) expenses_query = NULL;
	g_autoptr(GPtrArray) expenses = NULL;
	g_autoptr(GHashTable) buckets = NULL;
	g_autoptr(GList) keys = NULL;
	g_autoptr(VentureMoney) deductible_total = NULL;
	g_autoptr(VentureMoney) review_total = NULL;
	GList *iter;
	gint64 review_count;
	guint i;

	expenses_query = venture_report_scoped_query(context, VENTURE_TYPE_EXPENSE,
	                                             "occurred-at", period, options,
	                                             error);

	if (NULL == expenses_query)
		return NULL;

	expenses = venture_report_fetch_all(context, expenses_query, error);

	if (NULL == expenses)
		return NULL;

	result = venture_report_result_new("Deductions and write-offs", period);
	buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	deductible_total = venture_money_new_zero(NULL);
	review_total = venture_money_new_zero(NULL);
	review_count = 0;

	venture_report_result_add_column(result, "category", "Category",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "count", "Items",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "spent", "Spent",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "deductible", "Deductible",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	for (i = 0; i < expenses->len; i++)
	{
		g_autofree gchar *category = NULL;
		VentureEntity *expense;
		VentureDeductibility deductibility;
		GPtrArray *bucket;

		expense = g_ptr_array_index(expenses, i);
		g_object_get(expense, "category", &category,
		             "deductibility", &deductibility, NULL);

		if (venture_string_is_empty(category))
		{
			g_free(category);
			category = g_strdup("(uncategorised)");
		}

		bucket = g_hash_table_lookup(buckets, category);

		if (NULL == bucket)
		{
			bucket = g_ptr_array_new();
			g_hash_table_insert(buckets, g_strdup(category), bucket);
		}

		g_ptr_array_add(bucket, expense);

		if (VENTURE_DEDUCTIBILITY_REVIEW == deductibility)
		{
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureMoney) running = NULL;

			review_count++;
			g_object_get(expense, "amount", &amount, NULL);

			if (NULL != amount)
			{
				running = venture_money_add(review_total, amount, NULL);

				if (NULL != running)
				{
					g_clear_pointer(&review_total, venture_money_free);
					review_total = g_steal_pointer(&running);
				}
			}
		}
	}

	keys = g_hash_table_get_keys(buckets);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);

	for (iter = keys; NULL != iter; iter = iter->next)
	{
		g_autoptr(VentureMoney) spent = NULL;
		g_autoptr(VentureMoney) deductible = NULL;
		g_autoptr(VentureMoney) running = NULL;
		GPtrArray *bucket;
		guint j;

		bucket = g_hash_table_lookup(buckets, iter->data);
		spent = venture_report_total(bucket, "amount", NULL);
		deductible = venture_money_new_zero(venture_money_get_currency(spent));

		for (j = 0; j < bucket->len; j++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureMoney) next = NULL;

			amount = venture_expense_get_deductible_amount(
				g_ptr_array_index(bucket, j), NULL);

			if (NULL == amount)
				continue;

			next = venture_money_add(deductible, amount, NULL);

			if (NULL == next)
				continue;

			g_clear_pointer(&deductible, venture_money_free);
			deductible = g_steal_pointer(&next);
		}

		running = venture_money_add(deductible_total, deductible, NULL);

		if (NULL != running)
		{
			g_clear_pointer(&deductible_total, venture_money_free);
			deductible_total = g_steal_pointer(&running);
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "category", iter->data);
		venture_report_result_set_number(result, "count", (gdouble)bucket->len);
		venture_report_result_set_money(result, "spent", spent);
		venture_report_result_set_money(result, "deductible", deductible);
	}

	{
		GHashTableIter bucket_iter;
		gpointer value;

		g_hash_table_iter_init(&bucket_iter, buckets);

		while (g_hash_table_iter_next(&bucket_iter, NULL, &value))
			g_ptr_array_unref(value);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_money("deductible", "Total deductible",
		                         deductible_total));

	{
		VentureMetric *metric;

		metric = venture_metric_new_count("review", "Awaiting review",
		                                  review_count);
		venture_metric_set_higher_is_better(metric, FALSE);
		venture_report_result_add_metric(result, metric);
	}

	if (review_count > 0)
	{
		g_autofree gchar *amount_text = NULL;
		g_autofree gchar *note = NULL;

		amount_text = venture_money_to_display_string(review_total, TRUE);
		note = g_strdup_printf(
			"%" G_GINT64_FORMAT " expenses totalling %s are still marked "
			"for review and count as zero here. Classify them before "
			"filing.", review_count, amount_text);

		venture_report_result_set_note(result, note);
	}

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Campaigns and pipeline
 * ========================================================================== */

static VentureReportResult *
venture_report_campaigns(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) campaigns = NULL;
	g_autoptr(VentureMoney) total_spend = NULL;
	g_autoptr(VentureMoney) total_revenue = NULL;
	guint i;

	query = venture_report_scoped_query(context, VENTURE_TYPE_CAMPAIGN,
	                                    "started-at", period, options, error);

	if (NULL == query)
		return NULL;

	campaigns = venture_report_fetch_all(context, query, error);

	if (NULL == campaigns)
		return NULL;

	result = venture_report_result_new("Campaign performance", period);
	total_spend = venture_report_total(campaigns, "spend", NULL);
	total_revenue = venture_report_total(campaigns, "revenue", NULL);

	venture_report_result_add_column(result, "campaign", "Campaign",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "channel", "Channel",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "spend", "Spend",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "revenue", "Revenue",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "roi", "Return",
	                                 VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "conversions", "Conversions",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < campaigns->len; i++)
	{
		g_autoptr(VentureMoney) spend = NULL;
		g_autoptr(VentureMoney) revenue = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *channel = NULL;
		VentureEntity *campaign;
		gint64 conversions;

		campaign = g_ptr_array_index(campaigns, i);
		g_object_get(campaign, "name", &name, "channel", &channel,
		             "spend", &spend, "revenue", &revenue,
		             "conversions", &conversions, NULL);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "campaign", name);
		venture_report_result_set_text(result, "channel", channel);
		venture_report_result_set_money(result, "spend", spend);
		venture_report_result_set_money(result, "revenue", revenue);
		venture_report_result_set_number(result, "roi",
			venture_campaign_get_roi(VENTURE_CAMPAIGN(campaign)));
		venture_report_result_set_number(result, "conversions",
		                                 (gdouble)conversions);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_money("spend", "Total spend", total_spend));
	venture_report_result_add_metric(result,
		venture_metric_new_money("revenue", "Attributed revenue",
		                         total_revenue));

	return g_steal_pointer(&result);
}

static VentureReportResult *
venture_report_pipeline(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) deals = NULL;
	g_autoptr(VentureMoney) open_value = NULL;
	g_autoptr(VentureMoney) weighted_value = NULL;
	g_auto(GStrv) stages = NULL;
	gsize s;
	guint i;

	query = venture_query_new(VENTURE_TYPE_DEAL);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));

	deals = venture_report_fetch_all(context, query, error);

	if (NULL == deals)
		return NULL;

	result = venture_report_result_new("Deal pipeline", period);
	open_value = venture_money_new_zero(NULL);
	weighted_value = venture_money_new_zero(NULL);

	venture_report_result_add_column(result, "stage", "Stage",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "count", "Deals",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "value", "Value",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "weighted", "Weighted",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	/* Iterating the enum rather than the data means empty stages still
	 * appear, which is what makes a pipeline readable as a funnel. */
	stages = venture_enum_list_nicks(VENTURE_TYPE_DEAL_STAGE);

	for (s = 0; NULL != stages[s]; s++)
	{
		g_autoptr(VentureMoney) stage_value = NULL;
		g_autoptr(VentureMoney) stage_weighted = NULL;
		gint stage_enum;
		gint64 count;

		if (!venture_enum_from_nick(VENTURE_TYPE_DEAL_STAGE, stages[s],
		                            &stage_enum))
			continue;

		stage_value = venture_money_new_zero(NULL);
		stage_weighted = venture_money_new_zero(NULL);
		count = 0;

		for (i = 0; i < deals->len; i++)
		{
			g_autoptr(VentureMoney) value = NULL;
			g_autoptr(VentureMoney) weighted = NULL;
			g_autoptr(VentureMoney) next = NULL;
			VentureEntity *deal;
			VentureDealStage stage;

			deal = g_ptr_array_index(deals, i);
			g_object_get(deal, "stage", &stage, "value", &value, NULL);

			if ((gint)stage != stage_enum)
				continue;

			count++;

			if (NULL != value)
			{
				next = venture_money_add(stage_value, value, NULL);

				if (NULL != next)
				{
					g_clear_pointer(&stage_value, venture_money_free);
					stage_value = g_steal_pointer(&next);
				}
			}

			weighted = venture_deal_get_weighted_value(VENTURE_DEAL(deal),
			                                           NULL);

			if (NULL != weighted)
			{
				g_autoptr(VentureMoney) accumulated = NULL;

				accumulated = venture_money_add(stage_weighted, weighted,
				                                NULL);

				if (NULL != accumulated)
				{
					g_clear_pointer(&stage_weighted, venture_money_free);
					stage_weighted = g_steal_pointer(&accumulated);
				}
			}
		}

		if (!venture_deal_stage_is_closed((VentureDealStage)stage_enum))
		{
			g_autoptr(VentureMoney) running = NULL;

			running = venture_money_add(open_value, stage_value, NULL);

			if (NULL != running)
			{
				g_clear_pointer(&open_value, venture_money_free);
				open_value = g_steal_pointer(&running);
			}

			running = venture_money_add(weighted_value, stage_weighted, NULL);

			if (NULL != running)
			{
				g_clear_pointer(&weighted_value, venture_money_free);
				weighted_value = g_steal_pointer(&running);
			}
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "stage", stages[s]);
		venture_report_result_set_number(result, "count", (gdouble)count);
		venture_report_result_set_money(result, "value", stage_value);
		venture_report_result_set_money(result, "weighted", stage_weighted);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_money("open", "Open pipeline", open_value));
	venture_report_result_add_metric(result,
		venture_metric_new_money("weighted", "Weighted pipeline",
		                         weighted_value));

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Monthly series
 * ========================================================================== */

static VentureReportResult *
venture_report_monthly(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) months = NULL;
	guint i;

	result = venture_report_result_new("Monthly revenue and expenses", period);

	venture_report_result_add_column(result, "month", "Month",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sales", "Sales",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "revenue", "Net revenue",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "expenses", "Expenses",
	                                 VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "profit", "Profit",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	if (NULL == period)
		return g_steal_pointer(&result);

	months = venture_date_range_split_by_month(period);

	for (i = 0; i < months->len; i++)
	{
		g_autoptr(VentureQuery) sales_query = NULL;
		g_autoptr(VentureQuery) expenses_query = NULL;
		g_autoptr(GPtrArray) sales = NULL;
		g_autoptr(GPtrArray) expenses = NULL;
		g_autoptr(VentureMoney) revenue = NULL;
		g_autoptr(VentureMoney) spent = NULL;
		g_autoptr(VentureMoney) profit = NULL;
		VentureDateRange *month;

		month = g_ptr_array_index(months, i);

		sales_query = venture_report_scoped_query(context, VENTURE_TYPE_SALE,
		                                          "occurred-at", month,
		                                          options, error);

		if (NULL == sales_query)
			return NULL;

		expenses_query = venture_report_scoped_query(context,
			VENTURE_TYPE_EXPENSE, "occurred-at", month, options, error);

		if (NULL == expenses_query)
			return NULL;

		sales = venture_report_fetch_all(context, sales_query, error);
		expenses = venture_report_fetch_all(context, expenses_query, error);

		if ((NULL == sales) || (NULL == expenses))
			return NULL;

		revenue = venture_report_total_net(sales);
		spent = venture_report_total(expenses, "amount", NULL);
		profit = venture_money_subtract(revenue, spent, NULL);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "month",
		                               venture_date_range_get_label(month));
		venture_report_result_set_number(result, "sales", (gdouble)sales->len);
		venture_report_result_set_money(result, "revenue", revenue);
		venture_report_result_set_money(result, "expenses", spent);
		venture_report_result_set_money(result, "profit", profit);
	}

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Ideas
 * ========================================================================== */

static VentureReportResult *
venture_report_ideas(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) ideas = NULL;
	guint i;

	query = venture_query_new(VENTURE_TYPE_IDEA);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));

	ideas = venture_report_fetch_all(context, query, error);

	if (NULL == ideas)
		return NULL;

	result = venture_report_result_new("Idea pipeline", period);

	venture_report_result_add_column(result, "title", "Idea",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "score", "Score",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "opportunity", "Opportunity",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "confidence", "Confidence",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "effort", "Effort",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "estimated_revenue",
	                                 "Est. revenue",
	                                 VENTURE_REPORT_COLUMN_MONEY);

	/* Highest score first: the point of the report is to answer "what
	 * should I actually do next", which a creation-ordered list does not.
	 * The ordering is done by the selection pass inside the loop below,
	 * because the score is computed rather than stored and a plain sort
	 * comparator would recompute it O(n log n) times. */
	for (i = 0; i < ideas->len; i++)
	{
		g_autoptr(VentureMoney) estimated = NULL;
		g_autofree gchar *title = NULL;
		VentureEntity *idea;
		VentureIdeaStatus status;
		gint64 opportunity;
		gint64 confidence;
		gint64 effort;
		guint best;
		guint j;

		/* Selection sort by score, done in place so the array stays the
		 * caller's to free. The idea list is small enough that this
		 * costs nothing worth optimising. */
		best = i;

		for (j = i + 1; j < ideas->len; j++)
		{
			if (venture_idea_get_score(g_ptr_array_index(ideas, j)) >
			    venture_idea_get_score(g_ptr_array_index(ideas, best)))
				best = j;
		}

		if (best != i)
		{
			gpointer swap;

			swap = g_ptr_array_index(ideas, i);
			ideas->pdata[i] = g_ptr_array_index(ideas, best);
			ideas->pdata[best] = swap;
		}

		idea = g_ptr_array_index(ideas, i);

		g_object_get(idea, "title", &title, "status", &status,
		             "opportunity", &opportunity, "confidence", &confidence,
		             "effort", &effort, "estimated-revenue", &estimated,
		             NULL);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "title", title);
		venture_report_result_set_text(result, "status",
			venture_enum_to_nick(VENTURE_TYPE_IDEA_STATUS, (gint)status));
		venture_report_result_set_number(result, "score",
			venture_idea_get_score(VENTURE_IDEA(idea)));
		venture_report_result_set_number(result, "opportunity",
		                                 (gdouble)opportunity);
		venture_report_result_set_number(result, "confidence",
		                                 (gdouble)confidence);
		venture_report_result_set_number(result, "effort", (gdouble)effort);
		venture_report_result_set_money(result, "estimated_revenue", estimated);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("ideas", "Ideas", (gint64)ideas->len));

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Registration
 * ========================================================================== */

void
venture_report_registry_register_builtins(VentureReportRegistry *self)
{
	static const struct
	{
		const gchar		*name;
		const gchar		*title;
		const gchar		*description;
		VentureReportFunc	 func;
	} builtins[] = {
		{ "pnl", "Profit and loss",
		  "Revenue, expenses and profit for a period, with the deductible "
		  "portion of expenses shown separately",
		  venture_report_pnl },
		{ "ventures", "Venture performance",
		  "Sales, revenue, expenses and profit for each venture",
		  venture_report_ventures },
		{ "categories", "Performance by category",
		  "Sales grouped by a product attribute; defaults to genre, and "
		  "accepts group_by for category, format or any other product field",
		  venture_report_categories },
		{ "inventory", "Inventory",
		  "Quantity on hand and carrying value per item, with items at or "
		  "below their reorder point flagged",
		  venture_report_inventory },
		{ "tax", "Deductions and write-offs",
		  "Expenses by category with the deductible amount, and a count of "
		  "anything still awaiting review",
		  venture_report_tax },
		{ "campaigns", "Campaign performance",
		  "Spend, attributed revenue and return for each campaign",
		  venture_report_campaigns },
		{ "pipeline", "Deal pipeline",
		  "Open deals by stage, with probability-weighted value",
		  venture_report_pipeline },
		{ "monthly", "Monthly series",
		  "Revenue, expenses and profit month by month across the period",
		  venture_report_monthly },
		{ "ideas", "Idea pipeline",
		  "Ideas ranked by opportunity and confidence against effort",
		  venture_report_ideas }
	};
	gsize i;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(self));

	for (i = 0; i < G_N_ELEMENTS(builtins); i++)
	{
		venture_report_registry_add(self,
			VENTURE_REPORT(venture_func_report_new(builtins[i].name,
			                                       builtins[i].title,
			                                       builtins[i].description,
			                                       builtins[i].func)));
	}
}
