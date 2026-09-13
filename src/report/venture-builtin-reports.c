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
#include "ledger/venture-ledger-private.h"

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
 * Adds an amount into a running total, counting rather than hiding the
 * additions that fail. A NULL total adopts the first amount's currency, the
 * same anchoring every total in this file uses; a failed addition -- another
 * currency, or overflow -- leaves the total alone and increments the
 * caller's counter.
 *
 * Silently adding across currencies is the failure mode this exists to
 * prevent: the result would look authoritative and be wrong by whatever the
 * exchange rate happens to be. But silently *dropping* the row is only half
 * a fix -- the count is what lets the report admit it.
 */
static void
venture_report_accumulate(
	VentureMoney		**total,
	const VentureMoney	 *amount,
	guint			 *inout_skipped
){
	VentureMoney *next;

	if (NULL == amount)
		return;

	if (NULL == *total)
	{
		*total = venture_money_copy(amount);
		return;
	}

	next = venture_money_add(*total, amount, NULL);

	if (NULL == next)
	{
		if (NULL != inout_skipped)
			(*inout_skipped)++;
		return;
	}

	venture_money_free(*total);
	*total = next;
}

/*
 * Notes on the result how many amounts its totals had to leave out. Called
 * once per report, after every total is in, so a clean report carries no
 * note and a lossy one says exactly how lossy it was. Appended rather than
 * set, because a report can have other caveats too.
 */
static void
venture_report_flag_skipped(
	VentureReportResult	*result,
	guint			 skipped
){
	g_autofree gchar *note = NULL;

	if (0 == skipped)
		return;

	note = g_strdup_printf(
		"%u amount%s could not be included in these totals -- a different "
		"currency from the rest, or arithmetic that would overflow. "
		"Cross-currency totals are refused rather than guessed; filter by "
		"venture or currency for exact figures.",
		skipped, (1 == skipped) ? "" : "s");

	venture_report_result_append_note(result, note);
}

/*
 * Picks the currency a total should be carried in: the one most of the
 * amounts are denominated in, with ties broken toward the process default
 * and then the alphabetically first code. Anchoring on "the first record"
 * instead would hand the choice to the sort order, and a stray EUR row
 * sorted first must not decide the currency of a USD ledger's total.
 *
 * Returns NULL when nothing carries an amount at all.
 */
static gchar *
venture_report_majority_currency(GPtrArray *amounts)
{
	g_autoptr(GHashTable) counts = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	const gchar *best;
	guint best_count;
	guint i;

	counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < amounts->len; i++)
	{
		const VentureMoney *amount;
		gchar *currency;
		guint count;

		amount = g_ptr_array_index(amounts, i);

		if (NULL == amount)
			continue;

		currency = g_ascii_strup(venture_money_get_currency(amount), -1);
		count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, currency)) + 1;
		g_hash_table_replace(counts, currency, GUINT_TO_POINTER(count));
	}

	best = NULL;
	best_count = 0;

	g_hash_table_iter_init(&iter, counts);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		const gchar *currency;
		guint count;

		currency = key;
		count = GPOINTER_TO_UINT(value);

		if ((NULL == best) || (count > best_count))
		{
			best = currency;
			best_count = count;
			continue;
		}

		if (count == best_count)
		{
			const gchar *fallback;
			gboolean best_is_default;
			gboolean this_is_default;

			fallback = venture_money_get_default_currency();
			best_is_default =
				(0 == g_ascii_strcasecmp(best, fallback));
			this_is_default =
				(0 == g_ascii_strcasecmp(currency, fallback));

			if (this_is_default && !best_is_default)
				best = currency;
			else if ((this_is_default == best_is_default) &&
			         (g_strcmp0(currency, best) < 0))
				best = currency;
		}
	}

	return g_strdup(best);
}

/*
 * Sums a collected array of amounts in their majority currency, counting
 * whatever cannot join into @inout_skipped, which accumulates across calls
 * so one counter can watch every total a report builds.
 */
static VentureMoney *
venture_report_sum_amounts(
	GPtrArray	 *amounts,
	guint		 *inout_skipped
){
	g_autoptr(VentureMoney) total = NULL;
	g_autofree gchar *currency = NULL;
	guint i;

	currency = venture_report_majority_currency(amounts);

	if (NULL == currency)
		return venture_money_new_zero(NULL);

	total = venture_money_new_zero(currency);

	for (i = 0; i < amounts->len; i++)
		venture_report_accumulate(&total, g_ptr_array_index(amounts, i),
		                          inout_skipped);

	return g_steal_pointer(&total);
}

/*
 * Totals a money property across a set of records, in the currency most of
 * them are kept in.
 */
static VentureMoney *
venture_report_total(
	GPtrArray	 *records,
	const gchar	 *property,
	guint		 *inout_skipped
){
	g_autoptr(GPtrArray) amounts = NULL;
	guint i;

	amounts = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_money_free);

	for (i = 0; i < records->len; i++)
	{
		VentureMoney *amount = NULL;

		g_object_get(g_ptr_array_index(records, i), property, &amount, NULL);

		if (NULL != amount)
			g_ptr_array_add(amounts, amount);
	}

	return venture_report_sum_amounts(amounts, inout_skipped);
}

/*
 * Totals the net proceeds of a set of sales, which is what the P&L counts as
 * revenue -- not the gross, which includes money that was never yours.
 *
 * Totalling in the sales' own majority currency rather than the process
 * default matters here: a portfolio kept entirely in EUR must total in EUR,
 * not report zero because every sale failed to add into a USD zero. A sale
 * whose own fields disagree about currency yields no net at all, and that
 * is counted too.
 */
static VentureMoney *
venture_report_total_net(
	GPtrArray	 *sales,
	guint		 *inout_skipped
){
	g_autoptr(GPtrArray) nets = NULL;
	guint i;

	nets = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);

	for (i = 0; i < sales->len; i++)
	{
		VentureMoney *net;

		net = venture_sale_get_net(g_ptr_array_index(sales, i), NULL);

		if (NULL == net)
		{
			if (NULL != inout_skipped)
				(*inout_skipped)++;
			continue;
		}

		g_ptr_array_add(nets, net);
	}

	return venture_report_sum_amounts(nets, inout_skipped);
}

/*
 * Fetches every record matching a query, without the paging limit reports
 * must not inherit.
 */
static GPtrArray *
venture_report_fetch_all(
	VentureContext	 *context,
	VentureQuery	 *query,
	JsonObject	 *options,
	GError		**error
){
	if (!venture_period_report_scope(query, options, error))
		return NULL;
	if ((NULL != options) && json_object_has_member(options, "organization_id") &&
		(VENTURE_TYPE_INVOICE_LINE != venture_query_get_entity_type(query)) &&
		(VENTURE_TYPE_INVENTORY_TXN != venture_query_get_entity_type(query)))
		venture_query_set_organization(query, venture_json_object_get_int(options, "organization_id", 0));
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
	guint skipped;
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

	sales = venture_report_fetch_all(context, sales_query, options, error);

	if (NULL == sales)
		return NULL;

	expenses = venture_report_fetch_all(context, expenses_query, options, error);

	if (NULL == expenses)
		return NULL;

	skipped = 0;
	revenue = venture_report_total_net(sales, &skipped);
	gross = venture_report_total(sales, "gross", &skipped);
	fees = venture_report_total(sales, "fees", &skipped);
	shipping = venture_report_total(sales, "shipping-cost", &skipped);
	refunds = venture_report_total(sales, "refunded", &skipped);
	expense_total = venture_report_total(expenses, "amount", &skipped);

	/* The deductible total is tracked separately from the cash total,
	 * because they are different questions: what left the account, and
	 * what may be claimed. */
	deductible = venture_money_new_zero(
		venture_money_get_currency(expense_total));

	for (i = 0; i < expenses->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;

		amount = venture_expense_get_deductible_amount(
			g_ptr_array_index(expenses, i), NULL);

		venture_report_accumulate(&deductible, amount, &skipped);
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

	/* Revenue in one currency and expenses in another has no profit line
	 * at all, and the metric machinery renders an absent amount as zero.
	 * A zero that means "no answer" must not be left looking like an
	 * answer of zero. */
	if (NULL == profit)
	{
		venture_report_result_append_note(result,
			"Revenue and expenses are in different currencies, so no "
			"profit could be computed; the profit line's zero is an "
			"absence, not a result.");
	}

	venture_report_flag_skipped(result, skipped);

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
	guint skipped;
	guint i;

	ventures_query = venture_query_new(VENTURE_TYPE_VENTURE);
	venture_query_set_organization(ventures_query,
		venture_context_get_default_organization_id(context));

	ventures = venture_report_fetch_all(context, ventures_query, options, error);

	if (NULL == ventures)
		return NULL;

	result = venture_report_result_new("Venture performance", period);
	skipped = 0;

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

		sales = venture_report_fetch_all(context, sales_query, options, error);
		expenses = venture_report_fetch_all(context, expenses_query, options, error);

		if ((NULL == sales) || (NULL == expenses))
			return NULL;

		revenue = venture_report_total_net(sales, &skipped);
		spent = venture_report_total(expenses, "amount", &skipped);
		profit = venture_money_subtract(revenue, spent, NULL);

		venture_report_accumulate(&portfolio_revenue, revenue, &skipped);

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

	venture_report_flag_skipped(result, skipped);

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
	guint skipped;
	guint i;

	skipped = 0;
	group_by = (NULL != options)
		? venture_json_object_get_string(options, "group_by", "genre")
		: "genre";

	sales_query = venture_report_scoped_query(context, VENTURE_TYPE_SALE,
	                                          "occurred-at", period, options,
	                                          error);

	if (NULL == sales_query)
		return NULL;

	sales = venture_report_fetch_all(context, sales_query, options, error);

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
		revenue = venture_report_total_net(bucket, &skipped);
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

	venture_report_flag_skipped(result, skipped);

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
	guint skipped;
	guint i;

	items_query = venture_query_new(VENTURE_TYPE_INVENTORY_ITEM);
	venture_query_set_organization(items_query,
		venture_context_get_default_organization_id(context));

	items = venture_report_fetch_all(context, items_query, options, error);

	if (NULL == items)
		return NULL;

	result = venture_report_result_new("Inventory", period);
	skipped = 0;
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

		transactions = venture_report_fetch_all(context, txn_query, options, error);

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
		{
			value = venture_money_multiply_int(unit_cost, on_hand, NULL);

			/* A costed item whose value could not be computed is an
			 * exclusion too, not merely an empty cell. */
			if (NULL == value)
				skipped++;
		}

		venture_report_accumulate(&total_value, value, &skipped);

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

	venture_report_flag_skipped(result, skipped);

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
	guint skipped;
	guint i;

	expenses_query = venture_report_scoped_query(context, VENTURE_TYPE_EXPENSE,
	                                             "occurred-at", period, options,
	                                             error);

	if (NULL == expenses_query)
		return NULL;

	expenses = venture_report_fetch_all(context, expenses_query, options, error);

	if (NULL == expenses)
		return NULL;

	result = venture_report_result_new("Deductions and write-offs", period);
	buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	review_count = 0;
	skipped = 0;

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

			review_count++;
			g_object_get(expense, "amount", &amount, NULL);

			venture_report_accumulate(&review_total, amount, &skipped);
		}
	}

	keys = g_hash_table_get_keys(buckets);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);

	for (iter = keys; NULL != iter; iter = iter->next)
	{
		g_autoptr(VentureMoney) spent = NULL;
		g_autoptr(VentureMoney) deductible = NULL;
		GPtrArray *bucket;
		guint j;

		bucket = g_hash_table_lookup(buckets, iter->data);
		spent = venture_report_total(bucket, "amount", &skipped);
		deductible = venture_money_new_zero(venture_money_get_currency(spent));

		for (j = 0; j < bucket->len; j++)
		{
			g_autoptr(VentureMoney) amount = NULL;

			amount = venture_expense_get_deductible_amount(
				g_ptr_array_index(bucket, j), NULL);

			venture_report_accumulate(&deductible, amount, &skipped);
		}

		venture_report_accumulate(&deductible_total, deductible, &skipped);

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

		if (NULL == review_total)
			review_total = venture_money_new_zero(NULL);

		amount_text = venture_money_to_display_string(review_total, TRUE);
		note = g_strdup_printf(
			"%" G_GINT64_FORMAT " expenses totalling %s are still marked "
			"for review and count as zero here. Classify them before "
			"filing.", review_count, amount_text);

		venture_report_result_set_note(result, note);
	}

	venture_report_flag_skipped(result, skipped);

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
	guint skipped;
	guint i;

	query = venture_report_scoped_query(context, VENTURE_TYPE_CAMPAIGN,
	                                    "started-at", period, options, error);

	if (NULL == query)
		return NULL;

	campaigns = venture_report_fetch_all(context, query, options, error);

	if (NULL == campaigns)
		return NULL;

	result = venture_report_result_new("Campaign performance", period);
	skipped = 0;
	total_spend = venture_report_total(campaigns, "spend", &skipped);
	total_revenue = venture_report_total(campaigns, "revenue", &skipped);

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

	venture_report_flag_skipped(result, skipped);

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
	guint skipped;
	guint i;

	query = venture_query_new(VENTURE_TYPE_DEAL);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));

	deals = venture_report_fetch_all(context, query, options, error);

	if (NULL == deals)
		return NULL;

	result = venture_report_result_new("Deal pipeline", period);
	skipped = 0;

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

		count = 0;

		for (i = 0; i < deals->len; i++)
		{
			g_autoptr(VentureMoney) value = NULL;
			g_autoptr(VentureMoney) weighted = NULL;
			VentureEntity *deal;
			VentureDealStage stage;

			deal = g_ptr_array_index(deals, i);
			g_object_get(deal, "stage", &stage, "value", &value, NULL);

			if ((gint)stage != stage_enum)
				continue;

			count++;

			venture_report_accumulate(&stage_value, value, &skipped);

			weighted = venture_deal_get_weighted_value(VENTURE_DEAL(deal),
			                                           NULL);
			venture_report_accumulate(&stage_weighted, weighted, &skipped);
		}

		if (!venture_deal_stage_is_closed((VentureDealStage)stage_enum))
		{
			venture_report_accumulate(&open_value, stage_value, &skipped);
			venture_report_accumulate(&weighted_value, stage_weighted,
			                          &skipped);
		}

		/* An empty stage still shows a zero rather than a blank, which
		 * is what keeps the funnel readable as a funnel. */
		if (NULL == stage_value)
			stage_value = venture_money_new_zero(NULL);

		if (NULL == stage_weighted)
			stage_weighted = venture_money_new_zero(NULL);

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

	venture_report_flag_skipped(result, skipped);

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
	guint skipped;
	guint i;

	skipped = 0;
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

		sales = venture_report_fetch_all(context, sales_query, options, error);
		expenses = venture_report_fetch_all(context, expenses_query, options, error);

		if ((NULL == sales) || (NULL == expenses))
			return NULL;

		revenue = venture_report_total_net(sales, &skipped);
		spent = venture_report_total(expenses, "amount", &skipped);
		profit = venture_money_subtract(revenue, spent, NULL);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "month",
		                               venture_date_range_get_label(month));
		venture_report_result_set_number(result, "sales", (gdouble)sales->len);
		venture_report_result_set_money(result, "revenue", revenue);
		venture_report_result_set_money(result, "expenses", spent);
		venture_report_result_set_money(result, "profit", profit);
	}

	venture_report_flag_skipped(result, skipped);

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

	ideas = venture_report_fetch_all(context, query, options, error);

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

/* ==========================================================================
 * The software factory
 * ========================================================================== */

/*
 * A record's product name, or a dash. Products belong to the sales module,
 * which the factory suggests rather than requires, so the lookup may find
 * nothing and the report still runs.
 */
static gchar *
venture_report_product_label(
	VentureContext	*context,
	gint64		 product_id
){
	g_autoptr(VentureEntity) product = NULL;
	GType product_type;

	if (0 == product_id)
		return g_strdup("\xe2\x80\x94");

	product_type = venture_entity_registry_lookup(
		venture_context_get_entity_registry(context), "product");

	if (G_TYPE_INVALID != product_type)
		product = venture_database_get(venture_context_get_database(context),
		                               product_type, product_id, NULL);

	if (NULL == product)
		return g_strdup_printf("product #%" G_GINT64_FORMAT, product_id);

	return venture_entity_get_display_name(product);
}

/*
 * How many rows of @type point at @field = @id, and optionally how many of
 * those have @status_field equal to @status_nick.
 */
static gint64
venture_report_count_pointing_at(
	VentureContext	*context,
	GType		 type,
	const gchar	*field,
	gint64		 id,
	const gchar	*status_field,
	const gchar	*status_nick
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(type);

	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id,
	                                  NULL))
		return 0;

	if ((NULL != status_field) &&
	    !venture_query_add_filter_string(query, status_field,
	                                     VENTURE_FILTER_OP_EQ, status_nick,
	                                     NULL))
		return 0;

	return MAX(venture_database_count(venture_context_get_database(context),
	                                  query, NULL), 0);
}

/*
 * Releases released in the period: what each carried and how it was built
 * and deployed.
 */
static VentureReportResult *
venture_report_releases(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	gint64 shipped_total;
	gint64 failed_total;
	guint i;

	query = venture_query_new(VENTURE_TYPE_RELEASE);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));

	if (!venture_query_set_date_range(query, "released-at", period, error))
		return NULL;

	if (!venture_query_add_order(query, "released-at", VENTURE_SORT_DESCENDING,
	                             error))
		return NULL;

	releases = venture_report_fetch_all(context, query, options, error);

	if (NULL == releases)
		return NULL;

	result = venture_report_result_new("Releases", period);
	shipped_total = 0;
	failed_total = 0;

	venture_report_result_add_column(result, "version", "Version",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "product", "Product",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "released", "Released",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "tickets", "Tickets",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "builds_ok", "Builds green",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "builds_failed", "Builds red",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "deployments", "Deployed",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < releases->len; i++)
	{
		g_autofree gchar *version = NULL;
		g_autofree gchar *product = NULL;
		g_autofree gchar *released = NULL;
		g_autoptr(GDateTime) released_at = NULL;
		VentureEntity *release;
		VentureReleaseStatus status;
		gint64 product_id = 0;
		gint64 id;
		gint64 tickets;
		gint64 ok;
		gint64 failed;
		gint64 deployed;

		release = g_ptr_array_index(releases, i);
		id = venture_entity_get_id(release);
		g_object_get(release, "number", &version, "status", &status,
		             "released-at", &released_at, "product-id", &product_id,
		             NULL);

		product = venture_report_product_label(context, product_id);
		released = (NULL != released_at)
			? venture_time_to_date_string(released_at, NULL) : g_strdup("");

		tickets = venture_report_count_pointing_at(context, VENTURE_TYPE_TICKET,
		                                           "release-id", id, NULL, NULL);
		ok = venture_report_count_pointing_at(context, VENTURE_TYPE_BUILD,
		                                      "release-id", id, "status",
		                                      "succeeded");
		failed = venture_report_count_pointing_at(context, VENTURE_TYPE_BUILD,
		                                          "release-id", id, "status",
		                                          "failed");
		deployed = venture_report_count_pointing_at(context,
		                                            VENTURE_TYPE_DEPLOYMENT,
		                                            "release-id", id, "status",
		                                            "succeeded");

		shipped_total += tickets;
		failed_total += failed;

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "version", version);
		venture_report_result_set_text(result, "product", product);
		venture_report_result_set_text(result, "status",
			venture_enum_to_nick(VENTURE_TYPE_RELEASE_STATUS, (gint)status));
		venture_report_result_set_text(result, "released", released);
		venture_report_result_set_number(result, "tickets", (gdouble)tickets);
		venture_report_result_set_number(result, "builds_ok", (gdouble)ok);
		venture_report_result_set_number(result, "builds_failed",
		                                 (gdouble)failed);
		venture_report_result_set_number(result, "deployments",
		                                 (gdouble)deployed);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("releases", "Releases", (gint64)releases->len));
	venture_report_result_add_metric(result,
		venture_metric_new_count("tickets", "Tickets shipped", shipped_total));
	venture_report_result_add_metric(result,
		venture_metric_new_count("failed_builds", "Failed builds",
		                         failed_total));

	return g_steal_pointer(&result);
}

/*
 * Lead time: from a ticket being created to the release that carried it
 * going out. One row per release in the period, averaged over its tickets,
 * with the median across every ticket as the headline.
 *
 * Measured to the release rather than to "done", because done is a column
 * on a board and released is a fact a customer can observe. A ticket
 * finished in March and released in June took until June.
 */
static gint
venture_report_compare_double(
	gconstpointer	a,
	gconstpointer	b
){
	gdouble left;
	gdouble right;

	left = *(const gdouble *)a;
	right = *(const gdouble *)b;

	return (left < right) ? -1 : (left > right) ? 1 : 0;
}

static VentureReportResult *
venture_report_lead_time(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	g_autoptr(GArray) all_days = NULL;
	gdouble total_days;
	guint counted;
	guint i;

	query = venture_query_new(VENTURE_TYPE_RELEASE);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));

	if (!venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ,
	                                     "released", error))
		return NULL;

	if (!venture_query_set_date_range(query, "released-at", period, error))
		return NULL;

	if (!venture_query_add_order(query, "released-at", VENTURE_SORT_ASCENDING,
	                             error))
		return NULL;

	releases = venture_report_fetch_all(context, query, options, error);

	if (NULL == releases)
		return NULL;

	result = venture_report_result_new("Lead time", period);
	all_days = g_array_new(FALSE, FALSE, sizeof(gdouble));
	total_days = 0.0;
	counted = 0;

	venture_report_result_add_column(result, "version", "Release",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "released", "Released",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "tickets", "Tickets",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "average", "Average days",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "longest", "Longest days",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < releases->len; i++)
	{
		g_autoptr(VentureQuery) tickets_query = NULL;
		g_autoptr(GPtrArray) tickets = NULL;
		g_autoptr(GDateTime) released_at = NULL;
		g_autofree gchar *version = NULL;
		g_autofree gchar *released = NULL;
		VentureEntity *release;
		gdouble sum;
		gdouble longest;
		guint n;
		guint j;

		release = g_ptr_array_index(releases, i);
		g_object_get(release, "number", &version, "released-at",
		             &released_at, NULL);

		if (NULL == released_at)
			continue;

		tickets_query = venture_query_new(VENTURE_TYPE_TICKET);

		if (!venture_query_add_filter_int(tickets_query, "release-id",
		                                  VENTURE_FILTER_OP_EQ,
		                                  venture_entity_get_id(release), error))
			return NULL;

		tickets = venture_report_fetch_all(context, tickets_query, options, error);

		if (NULL == tickets)
			return NULL;

		sum = 0.0;
		longest = 0.0;
		n = 0;

		for (j = 0; j < tickets->len; j++)
		{
			VentureEntity *ticket;
			GDateTime *created;
			gdouble days;

			ticket = g_ptr_array_index(tickets, j);
			created = venture_entity_get_created_at(ticket);

			if (NULL == created)
				continue;

			days = (gdouble)g_date_time_difference(released_at, created)
			       / (gdouble)G_TIME_SPAN_DAY;

			/* A ticket created after its release is a data-entry
			 * order, not a negative lead time. */
			if (days < 0.0)
				days = 0.0;

			sum += days;
			longest = MAX(longest, days);
			n++;
			g_array_append_val(all_days, days);
		}

		released = venture_time_to_date_string(released_at, NULL);

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "version", version);
		venture_report_result_set_text(result, "released", released);
		venture_report_result_set_number(result, "tickets", (gdouble)n);
		venture_report_result_set_number(result, "average",
		                                 (n > 0) ? sum / n : 0.0);
		venture_report_result_set_number(result, "longest", longest);

		total_days += sum;
		counted += n;
	}

	if (all_days->len > 0)
	{
		gdouble median;

		g_array_sort(all_days, venture_report_compare_double);

		if (0 == (all_days->len % 2))
			median = (g_array_index(all_days, gdouble, all_days->len / 2 - 1) +
			          g_array_index(all_days, gdouble, all_days->len / 2)) / 2.0;
		else
			median = g_array_index(all_days, gdouble, all_days->len / 2);

		venture_report_result_add_metric(result,
			venture_metric_new_number("median", "Median lead time (days)",
			                          median));
	}

	venture_report_result_add_metric(result,
		venture_metric_new_number("average", "Average lead time (days)",
		                          (counted > 0) ? total_days / counted : 0.0));
	venture_report_result_add_metric(result,
		venture_metric_new_count("tickets", "Tickets measured",
		                         (gint64)counted));
	venture_report_result_add_metric(result,
		venture_metric_new_count("releases", "Releases",
		                         (gint64)releases->len));

	if (0 == counted)
		venture_report_result_set_note(result,
			"No ticket in the period is marked as fixed in a released "
			"release. Set a ticket's Fixed in field to measure it.");

	return g_steal_pointer(&result);
}

/*
 * Incidents that started in the period, with how long each took to resolve
 * and the mean across those that were.
 */
static VentureReportResult *
venture_report_incidents(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) incidents = NULL;
	gdouble total_hours;
	guint resolved;
	guint open;
	guint i;

	query = venture_query_new(VENTURE_TYPE_INCIDENT);
	venture_query_set_organization(query,
		venture_context_get_default_organization_id(context));

	if (!venture_query_set_date_range(query, "started-at", period, error))
		return NULL;

	if (!venture_query_add_order(query, "started-at", VENTURE_SORT_DESCENDING,
	                             error))
		return NULL;

	incidents = venture_report_fetch_all(context, query, options, error);

	if (NULL == incidents)
		return NULL;

	result = venture_report_result_new("Incidents", period);
	total_hours = 0.0;
	resolved = 0;
	open = 0;

	venture_report_result_add_column(result, "title", "Incident",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "severity", "Severity",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "started", "Started",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "hours", "Hours to resolve",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < incidents->len; i++)
	{
		g_autofree gchar *title = NULL;
		g_autofree gchar *started = NULL;
		g_autoptr(GDateTime) started_at = NULL;
		g_autoptr(GDateTime) resolved_at = NULL;
		VentureEntity *incident;
		VentureIncidentSeverity severity;
		VentureIncidentStatus status;
		gdouble hours;

		incident = g_ptr_array_index(incidents, i);
		g_object_get(incident, "title", &title, "severity", &severity,
		             "status", &status, "started-at", &started_at,
		             "resolved-at", &resolved_at, NULL);

		hours = 0.0;

		if ((NULL != started_at) && (NULL != resolved_at))
		{
			hours = (gdouble)g_date_time_difference(resolved_at, started_at)
			        / (gdouble)G_TIME_SPAN_HOUR;

			if (hours < 0.0)
				hours = 0.0;

			total_hours += hours;
			resolved++;
		}
		else if ((VENTURE_INCIDENT_STATUS_OPEN == status) ||
		         (VENTURE_INCIDENT_STATUS_MITIGATED == status))
		{
			open++;
		}

		started = (NULL != started_at)
			? venture_time_to_date_string(started_at, NULL) : g_strdup("");

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "title", title);
		venture_report_result_set_text(result, "severity",
			venture_enum_to_nick(VENTURE_TYPE_INCIDENT_SEVERITY,
			                     (gint)severity));
		venture_report_result_set_text(result, "status",
			venture_enum_to_nick(VENTURE_TYPE_INCIDENT_STATUS, (gint)status));
		venture_report_result_set_text(result, "started", started);
		venture_report_result_set_number(result, "hours", hours);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("incidents", "Incidents",
		                         (gint64)incidents->len));
	venture_report_result_add_metric(result,
		venture_metric_new_count("open", "Still open", (gint64)open));
	venture_report_result_add_metric(result,
		venture_metric_new_number("mttr", "Mean hours to resolve",
		                          (resolved > 0) ? total_hours / resolved : 0.0));

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Delivery: the four keys, and what the agents cost to get there
 *
 * DORA's four -- deployment frequency, lead time for changes, change
 * failure rate, time to restore -- measured from the factory's own
 * records: a change is a ticket, a deploy is a deployment into a
 * production environment, a failure is an incident that names the
 * deployment, a restore is the incident resolving. Beside them, what
 * the coding runs did and cost in the same period, because the question
 * in 2026 is not "how fast" but "how fast, at what price, and how much
 * of it did a model write".
 * ========================================================================== */

static VentureReportResult *
venture_report_delivery(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) env_query = NULL;
	g_autoptr(GPtrArray) environments = NULL;
	g_autoptr(VentureQuery) deploy_query = NULL;
	g_autoptr(GPtrArray) deployments = NULL;
	g_autoptr(VentureQuery) incident_query = NULL;
	g_autoptr(GPtrArray) incidents = NULL;
	g_autoptr(VentureQuery) run_query = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GArray) lead_days = NULL;
	g_autoptr(GHashTable) production = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(VentureMoney) cost_total = NULL;
	gint64 days;
	guint prod_deploys;
	guint failed_deploys;
	guint failures;
	guint restored;
	gdouble restore_hours;
	guint run_succeeded;
	guint run_failed;
	guint run_prs;
	gint64 tokens;
	guint i;

	(void)options;

	/* Which environments are production: the ones the four keys are
	 * about. A staging deploy is practice. */
	env_query = venture_query_new(VENTURE_TYPE_ENVIRONMENT);
	venture_query_set_organization(env_query,
		venture_context_get_default_organization_id(context));
	environments = venture_report_fetch_all(context, env_query, options, error);

	if (NULL == environments)
		return NULL;

	production = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; i < environments->len; i++)
	{
		VentureEntity *environment;
		VentureEnvironmentKind kind;

		environment = g_ptr_array_index(environments, i);
		g_object_get(environment, "kind", &kind, NULL);

		if (VENTURE_ENVIRONMENT_KIND_PRODUCTION == kind)
			g_hash_table_add(production,
				GINT_TO_POINTER((gint)venture_entity_get_id(environment)));
	}

	deploy_query = venture_query_new(VENTURE_TYPE_DEPLOYMENT);
	venture_query_set_organization(deploy_query,
		venture_context_get_default_organization_id(context));

	if (!venture_query_set_date_range(deploy_query, "deployed-at", period,
	                                  error))
		return NULL;

	venture_query_add_order(deploy_query, "deployed-at", VENTURE_SORT_ASCENDING,
	                        NULL);
	deployments = venture_report_fetch_all(context, deploy_query, options, error);

	if (NULL == deployments)
		return NULL;

	incident_query = venture_query_new(VENTURE_TYPE_INCIDENT);
	venture_query_set_organization(incident_query,
		venture_context_get_default_organization_id(context));

	if (!venture_query_set_date_range(incident_query, "started-at", period,
	                                  error))
		return NULL;

	incidents = venture_report_fetch_all(context, incident_query, options, error);

	if (NULL == incidents)
		return NULL;

	result = venture_report_result_new("Delivery", period);
	lead_days = g_array_new(FALSE, FALSE, sizeof(gdouble));
	prod_deploys = failed_deploys = failures = restored = 0;
	restore_hours = 0.0;

	venture_report_result_add_column(result, "environment", "Environment",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "kind", "Kind",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "deployments", "Deployments",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "failed", "Failed deploys",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "incidents", "Incidents",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "lead", "Median lead days",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	/* One row per environment, with the production ones feeding the
	 * headline figures. */
	for (i = 0; i < environments->len; i++)
	{
		VentureEntity *environment;
		g_autofree gchar *name = NULL;
		g_autoptr(GArray) env_lead = NULL;
		VentureEnvironmentKind kind;
		gint64 env_id;
		guint env_deploys;
		guint env_failed;
		guint env_incidents;
		guint j;
		gdouble median;

		environment = g_ptr_array_index(environments, i);
		env_id = venture_entity_get_id(environment);
		g_object_get(environment, "name", &name, "kind", &kind, NULL);
		env_lead = g_array_new(FALSE, FALSE, sizeof(gdouble));
		env_deploys = env_failed = env_incidents = 0;

		for (j = 0; j < deployments->len; j++)
		{
			VentureEntity *deployment;
			g_autoptr(GDateTime) deployed_at = NULL;
			VentureDeploymentStatus status;
			gint64 environment_id = 0;
			gint64 release_id = 0;

			deployment = g_ptr_array_index(deployments, j);
			g_object_get(deployment, "environment-id", &environment_id,
			             "release-id", &release_id, "status", &status,
			             "deployed-at", &deployed_at, NULL);

			if (environment_id != env_id)
				continue;

			if (VENTURE_DEPLOYMENT_STATUS_FAILED == status)
			{
				env_failed++;
				continue;
			}

			if (VENTURE_DEPLOYMENT_STATUS_SUCCEEDED != status)
				continue;

			env_deploys++;

			/* Lead time for changes: from each ticket the release
			 * carried being raised to this deployment landing. */
			if ((0 != release_id) && (NULL != deployed_at))
			{
				g_autoptr(GPtrArray) tickets = NULL;
				guint k;

				tickets = venture_factory_release_tickets(
					venture_context_get_database(context), release_id);

				for (k = 0; (NULL != tickets) && (k < tickets->len); k++)
				{
					GDateTime *created;
					gdouble lead;

					created = venture_entity_get_created_at(
						g_ptr_array_index(tickets, k));

					if (NULL == created)
						continue;

					lead = (gdouble)g_date_time_difference(deployed_at,
					                                       created)
					       / (gdouble)G_TIME_SPAN_DAY;

					if (lead < 0.0)
						lead = 0.0;

					g_array_append_val(env_lead, lead);

					if (VENTURE_ENVIRONMENT_KIND_PRODUCTION == kind)
						g_array_append_val(lead_days, lead);
				}
			}
		}

		for (j = 0; j < incidents->len; j++)
		{
			gint64 environment_id = 0;

			g_object_get(g_ptr_array_index(incidents, j),
			             "environment-id", &environment_id, NULL);

			if (environment_id == env_id)
				env_incidents++;
		}

		if (VENTURE_ENVIRONMENT_KIND_PRODUCTION == kind)
		{
			prod_deploys += env_deploys;
			failed_deploys += env_failed;
		}

		median = 0.0;

		if (env_lead->len > 0)
		{
			g_array_sort(env_lead, venture_report_compare_double);
			median = (0 == (env_lead->len % 2))
				? (g_array_index(env_lead, gdouble, env_lead->len / 2 - 1) +
				   g_array_index(env_lead, gdouble, env_lead->len / 2)) / 2.0
				: g_array_index(env_lead, gdouble, env_lead->len / 2);
		}

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "environment", name);
		venture_report_result_set_text(result, "kind",
			venture_enum_to_nick(VENTURE_TYPE_ENVIRONMENT_KIND, (gint)kind));
		venture_report_result_set_number(result, "deployments",
		                                 (gdouble)env_deploys);
		venture_report_result_set_number(result, "failed", (gdouble)env_failed);
		venture_report_result_set_number(result, "incidents",
		                                 (gdouble)env_incidents);
		venture_report_result_set_number(result, "lead", median);
	}

	/* Change failure rate counts incidents that name a deployment into
	 * production; time to restore is measured over every incident that
	 * resolved in the period. */
	for (i = 0; i < incidents->len; i++)
	{
		VentureEntity *incident;
		g_autoptr(GDateTime) started_at = NULL;
		g_autoptr(GDateTime) resolved_at = NULL;
		gint64 deployment_id = 0;
		gint64 environment_id = 0;

		incident = g_ptr_array_index(incidents, i);
		g_object_get(incident, "deployment-id", &deployment_id,
		             "environment-id", &environment_id,
		             "started-at", &started_at, "resolved-at", &resolved_at,
		             NULL);

		if ((0 != deployment_id) &&
		    g_hash_table_contains(production,
		                          GINT_TO_POINTER((gint)environment_id)))
			failures++;

		if ((NULL != started_at) && (NULL != resolved_at))
		{
			gdouble hours;

			hours = (gdouble)g_date_time_difference(resolved_at, started_at)
			        / (gdouble)G_TIME_SPAN_HOUR;
			restore_hours += MAX(hours, 0.0);
			restored++;
		}
	}

	days = venture_date_range_get_days(period);

	if (days < 1)
		days = 1;

	venture_report_result_add_metric(result,
		venture_metric_new_count("deployments", "Production deployments",
		                         (gint64)prod_deploys));
	venture_report_result_add_metric(result,
		venture_metric_new_number("frequency", "Deployments per week",
		                          ((gdouble)prod_deploys * 7.0) / (gdouble)days));

	if (lead_days->len > 0)
	{
		gdouble median;

		g_array_sort(lead_days, venture_report_compare_double);
		median = (0 == (lead_days->len % 2))
			? (g_array_index(lead_days, gdouble, lead_days->len / 2 - 1) +
			   g_array_index(lead_days, gdouble, lead_days->len / 2)) / 2.0
			: g_array_index(lead_days, gdouble, lead_days->len / 2);
		venture_report_result_add_metric(result,
			venture_metric_new_number("lead_time",
			                          "Median lead time for changes (days)",
			                          median));
	}

	venture_report_result_add_metric(result,
		venture_metric_new_ratio("change_failure_rate", "Change failure rate",
		                         (prod_deploys > 0)
		                         	? (gdouble)failures / (gdouble)prod_deploys
		                         	: 0.0));
	venture_report_result_add_metric(result,
		venture_metric_new_number("time_to_restore",
		                          "Mean time to restore (hours)",
		                          (restored > 0) ? restore_hours / restored
		                                         : 0.0));

	/* And the agents' side of the same period. The forge module may be
	 * off, in which case the runs table is not offered and the figures
	 * are simply absent rather than zero. */
	if (venture_context_module_enabled(context, "forge"))
	{
		run_query = venture_query_new(VENTURE_TYPE_FORGE_RUN);
		venture_query_set_organization(run_query,
			venture_context_get_default_organization_id(context));

		if (!venture_query_set_date_range(run_query, "started-at", period,
		                                  error))
			return NULL;

		runs = venture_report_fetch_all(context, run_query, options, error);

		if (NULL == runs)
			return NULL;

		costs = g_ptr_array_new_with_free_func(
			(GDestroyNotify)venture_money_free);
		run_succeeded = run_failed = run_prs = 0;
		tokens = 0;

		for (i = 0; i < runs->len; i++)
		{
			VentureEntity *run;
			VentureMoney *cost = NULL;
			VentureForgeRunState state;
			gint64 pr = 0;
			gint64 in_tokens = 0;
			gint64 out_tokens = 0;

			run = g_ptr_array_index(runs, i);
			g_object_get(run, "state", &state, "pull-request-number", &pr,
			             "input-tokens", &in_tokens,
			             "output-tokens", &out_tokens, "cost", &cost, NULL);

			if (VENTURE_FORGE_RUN_STATE_SUCCEEDED == state)
				run_succeeded++;
			else if ((VENTURE_FORGE_RUN_STATE_FAILED == state) ||
			         (VENTURE_FORGE_RUN_STATE_INTERRUPTED == state))
				run_failed++;

			if (pr > 0)
				run_prs++;

			tokens += in_tokens + out_tokens;

			if (NULL != cost)
				g_ptr_array_add(costs, cost);
		}

		cost_total = venture_money_sum(costs, NULL, NULL);

		venture_report_result_add_metric(result,
			venture_metric_new_count("runs", "Coding runs", (gint64)runs->len));
		venture_report_result_add_metric(result,
			venture_metric_new_count("runs_succeeded", "Runs succeeded",
			                         (gint64)run_succeeded));
		venture_report_result_add_metric(result,
			venture_metric_new_count("runs_failed", "Runs failed",
			                         (gint64)run_failed));
		venture_report_result_add_metric(result,
			venture_metric_new_count("pull_requests", "Pull requests drafted",
			                         (gint64)run_prs));
		venture_report_result_add_metric(result,
			venture_metric_new_count("tokens", "Tokens", tokens));

		if (NULL != cost_total)
		{
			venture_report_result_add_metric(result,
				venture_metric_new_money("agent_cost", "Agent cost",
				                         cost_total));

			if (run_prs > 0)
			{
				g_autoptr(GPtrArray) shares = NULL;

				shares = venture_money_allocate_evenly(cost_total, run_prs,
				                                       NULL);

				if ((NULL != shares) && (shares->len > 0))
					venture_report_result_add_metric(result,
						venture_metric_new_money("cost_per_pull_request",
						                         "Cost per pull request",
						                         g_ptr_array_index(shares, 0)));
			}
		}
	}

	if (0 == g_hash_table_size(production))
		venture_report_result_set_note(result,
			"No environment is marked production, so the four keys have "
			"nothing to count. Set an environment's kind to production.");

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * Support: how the desk is doing
 *
 * The four questions a helpdesk is judged on -- how fast somebody
 * answers, how fast it is finished, how often a promise is missed, and
 * what the people on the other end made of it -- one row per person
 * holding tickets, so it is also the answer to "who is carrying what".
 * ========================================================================== */

static VentureReportResult *
venture_report_support(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	g_autoptr(GHashTable) rows = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gdouble response_hours;
	gdouble resolution_hours;
	guint responded;
	guint resolved;
	guint breached;
	guint rated;
	guint good;
	guint open;
	guint i;

	/*
	 * By when the ticket was raised, not when it was resolved: "the
	 * tickets of March" is what somebody means, and measuring the ones
	 * resolved in March would count January's slowest and miss March's.
	 */
	query = venture_report_scoped_query(context, VENTURE_TYPE_TICKET, NULL,
	                                    NULL, options, error);

	if (NULL == query)
		return NULL;

	if ((NULL != period) &&
	    !venture_query_set_date_range(query, "created-at", period, error))
		return NULL;

	tickets = venture_report_fetch_all(context, query, options, error);

	if (NULL == tickets)
		return NULL;

	result = venture_report_result_new("Support", period);
	rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	response_hours = resolution_hours = 0.0;
	responded = resolved = breached = rated = good = open = 0;

	venture_report_result_add_column(result, "assignee", "Assignee",
	                                 VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "tickets", "Tickets",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "open", "Still open",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "breached", "Breached",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "response", "Avg reply hours",
	                                 VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "resolution",
	                                 "Avg resolve hours",
	                                 VENTURE_REPORT_COLUMN_NUMBER);

	/*
	 * One accumulator per assignee, kept in a table keyed by name. Six
	 * doubles in one allocation rather than six tables: the figures move
	 * together and a row is meaningless without all of them.
	 */
	for (i = 0; i < tickets->len; i++)
	{
		VentureEntity *ticket;
		g_autofree gchar *assignee = NULL;
		g_autoptr(GDateTime) responded_at = NULL;
		g_autoptr(GDateTime) resolved_at = NULL;
		GDateTime *created;
		VentureTicketStatus status;
		VentureSatisfaction satisfaction;
		gdouble *row;
		gboolean is_breached = FALSE;
		const gchar *name;

		ticket = g_ptr_array_index(tickets, i);
		g_object_get(ticket, "assignee", &assignee, "status", &status,
		             "first-responded-at", &responded_at,
		             "resolved-at", &resolved_at, "sla-breached", &is_breached,
		             "satisfaction", &satisfaction, NULL);
		created = venture_entity_get_created_at(ticket);
		name = venture_string_is_empty(assignee) ? "(unassigned)" : assignee;

		row = g_hash_table_lookup(rows, name);

		if (NULL == row)
		{
			row = g_new0(gdouble, 6);
			g_hash_table_insert(rows, g_strdup(name), row);
		}

		row[0] += 1.0;

		if ((VENTURE_TICKET_STATUS_DONE != status) &&
		    (VENTURE_TICKET_STATUS_CANCELLED != status))
		{
			row[1] += 1.0;
			open++;
		}

		if (is_breached)
		{
			row[2] += 1.0;
			breached++;
		}

		if ((NULL != responded_at) && (NULL != created))
		{
			gdouble hours;

			hours = (gdouble)g_date_time_difference(responded_at, created)
			        / (gdouble)G_TIME_SPAN_HOUR;
			hours = MAX(hours, 0.0);
			row[3] += hours;
			row[4] += 1.0;
			response_hours += hours;
			responded++;
		}

		if ((NULL != resolved_at) && (NULL != created))
		{
			gdouble hours;

			hours = (gdouble)g_date_time_difference(resolved_at, created)
			        / (gdouble)G_TIME_SPAN_HOUR;
			resolution_hours += MAX(hours, 0.0);
			resolved++;
			row[5] += MAX(hours, 0.0);
		}

		if (VENTURE_SATISFACTION_UNRATED != satisfaction)
		{
			rated++;

			if (VENTURE_SATISFACTION_GOOD == satisfaction)
				good++;
		}
	}

	g_hash_table_iter_init(&iter, rows);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		const gdouble *row = value;

		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "assignee", key);
		venture_report_result_set_number(result, "tickets", row[0]);
		venture_report_result_set_number(result, "open", row[1]);
		venture_report_result_set_number(result, "breached", row[2]);
		venture_report_result_set_number(result, "response",
		                                 (row[4] > 0.0) ? row[3] / row[4] : 0.0);
		venture_report_result_set_number(result, "resolution",
			((row[0] - row[1]) > 0.0) ? row[5] / (row[0] - row[1]) : 0.0);
	}

	venture_report_result_add_metric(result,
		venture_metric_new_count("tickets", "Tickets raised",
		                         (gint64)tickets->len));
	venture_report_result_add_metric(result,
		venture_metric_new_count("open", "Still open", (gint64)open));
	venture_report_result_add_metric(result,
		venture_metric_new_number("response", "Mean hours to first reply",
		                          (responded > 0) ? response_hours / responded
		                                          : 0.0));
	venture_report_result_add_metric(result,
		venture_metric_new_number("resolution", "Mean hours to resolve",
		                          (resolved > 0) ? resolution_hours / resolved
		                                         : 0.0));
	venture_report_result_add_metric(result,
		venture_metric_new_ratio("breach_rate", "Service level missed",
		                         (tickets->len > 0)
		                         	? (gdouble)breached / (gdouble)tickets->len
		                         	: 0.0));

	/*
	 * CSAT the way everybody computes it: the share of ratings that were
	 * good, over the ratings given rather than over the tickets. A
	 * denominator of every ticket would read as a satisfaction score
	 * falling every time somebody did not answer a survey.
	 *
	 * And left out entirely when nobody has rated anything, rather than
	 * shown as zero. A satisfaction score of 0% is a claim -- everyone
	 * who answered was unhappy -- and "nobody has said" is not that
	 * claim. The note says so in its place.
	 */
	if (rated > 0)
	{
		venture_report_result_add_metric(result,
			venture_metric_new_ratio("csat", "Satisfaction",
			                         (gdouble)good / (gdouble)rated));
		venture_report_result_add_metric(result,
			venture_metric_new_count("rated", "Tickets rated",
			                         (gint64)rated));
	}
	else
	{
		venture_report_result_append_note(result,
			"No ticket in the period carries a satisfaction rating, so "
			"there is no score to show. Record one on a ticket's Desk "
			"card.");
	}

	return g_steal_pointer(&result);
}


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
		  venture_report_ideas },

		{ "releases", "Releases",
		  "Releases that went out in the period: the tickets each carried, "
		  "how its builds went, and where it was deployed",
		  venture_report_releases },
		{ "lead_time", "Lead time",
		  "Days from a ticket being raised to the release that carried it "
		  "going out, per release, with the median across the period",
		  venture_report_lead_time },
		{ "incidents", "Incidents",
		  "Incidents that started in the period, with hours to resolve and "
		  "the mean across those resolved",
		  venture_report_incidents },
		{ "support", "Support",
		  "How the desk is doing: tickets per assignee, how many are still "
		  "open, how fast the first reply and the resolution came, how "
		  "often a service level was missed, and what people made of it",
		  venture_report_support },
		{ "delivery", "Delivery",
		  "The four keys -- deployment frequency, lead time for changes, "
		  "change failure rate, time to restore -- from the factory's own "
		  "records, beside what the coding runs did and cost",
		  venture_report_delivery }
	};
	gsize i;

	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(self));

	venture_ledger_register_report(self);

	for (i = 0; i < G_N_ELEMENTS(builtins); i++)
	{
		venture_report_registry_add(self,
			VENTURE_REPORT(venture_func_report_new(builtins[i].name,
			                                       builtins[i].title,
			                                       builtins[i].description,
			                                       builtins[i].func)));
	}

	venture_receivables_register_reports(self);
	venture_period_reports_register(self);
	venture_autojournal_register_reports(self);

}
