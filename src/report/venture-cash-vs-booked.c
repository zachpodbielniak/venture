/*
 * venture-cash-vs-booked.c - Revenue booked beside cash received
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Booked is what was invoiced: issue events less voids, credit notes and
 * write-offs, each in the bucket of its own date. Received is what arrived:
 * receipts less refunds, each in the bucket of its own date. Every receipt
 * -- manual, Stripe, or a matched bank credit -- is a payment record made
 * by the settlement service, so counting payments counts them all once.
 *
 * A receipt belongs to a customer, not a venture, so with venture_id the
 * report reaches cash through the invoices: receipts and cash credits
 * applied to the venture's invoices count as received in the bucket of the
 * application; credit notes and write-offs applied to them count against
 * booked. Without venture_id the receipt itself is the evidence and an
 * unapplied deposit is still money that came in.
 *
 * Currencies are never folded. Each bucket has one row per currency seen
 * anywhere in the period; the metrics are the period totals in the report
 * currency and say which currencies they left to the rows.
 */

#include "venture.h"

#include <string.h>

/* The options every report surface may send, and this report's own. */
static const gchar *const cash_vs_booked_options[] = {
	"organization_id", "currency", "as_of", "bucket", "customer_id",
	"venture_id", NULL
};

/* One dated amount and which side of the comparison it lands on. */
typedef enum
{
	SIDE_BOOKED,
	SIDE_RECEIVED
} Side;

typedef struct
{
	GDateTime	*date;
	VentureMoney	*amount;
	Side		 side;
	gboolean	 subtract;
} Evidence;

static void
evidence_free(Evidence *evidence)
{
	g_clear_pointer(&evidence->date, g_date_time_unref);
	g_clear_pointer(&evidence->amount, venture_money_free);
	g_free(evidence);
}

/* Per currency: booked and received per bucket, in bucket order, and the
 * gap accumulated so far while the rows are written. */
typedef struct
{
	gchar		*currency;
	GPtrArray	*booked;
	GPtrArray	*received;
	VentureMoney	*running;
} Series;

static void
series_free(Series *series)
{
	g_free(series->currency);
	g_ptr_array_unref(series->booked);
	g_ptr_array_unref(series->received);
	venture_money_free(series->running);
	g_free(series);
}

static Series *
series_new(const gchar *currency, guint n_buckets)
{
	Series *series;
	guint i;

	series = g_new0(Series, 1);
	series->currency = g_strdup(currency);
	series->booked = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	series->received = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	series->running = venture_money_new_zero(currency);

	for (i = 0; i < n_buckets; i++)
	{
		g_ptr_array_add(series->booked, venture_money_new_zero(currency));
		g_ptr_array_add(series->received, venture_money_new_zero(currency));
	}

	return series;
}

static gint
series_compare(gconstpointer a, gconstpointer b)
{
	return g_strcmp0((*(Series *const *)a)->currency, (*(Series *const *)b)->currency);
}

/* Everything the report reads, gathered once. */
typedef struct
{
	VentureDatabase	*database;
	gint64		 organization_id;
	gint64		 customer_id;
	gint64		 venture_id;
	GDateTime	*start;
	GDateTime	*end;		/* exclusive; the as_of cutoff when earlier */
	GPtrArray	*evidence;	/* Evidence */
	GHashTable	*opening;	/* invoice id -> GINT_TO_POINTER(stamped) */
	GHashTable	*venture_invoices; /* invoice id -> itself, with venture_id */
} Gather;

static gboolean
cash_vs_booked_check_options(JsonObject *options, GError **error)
{
	GList *members;
	GList *l;
	const gchar *bucket;

	if (NULL == options)
		return TRUE;

	members = json_object_get_members(options);

	for (l = members; NULL != l; l = l->next)
	{
		if (!g_strv_contains(cash_vs_booked_options, l->data))
		{
			venture_set_error_validation(error, l->data,
				"The cash_vs_booked report does not take %s; its options "
				"are bucket, customer_id, venture_id, currency, "
				"organization_id and as_of", (const gchar *)l->data);
			g_list_free(members);
			return FALSE;
		}
	}

	g_list_free(members);
	bucket = venture_json_object_get_string(options, "bucket", "month");

	if ((0 != g_strcmp0(bucket, "month")) && (0 != g_strcmp0(bucket, "week")))
	{
		venture_set_error_validation(error, "bucket",
			"bucket must be month or week, not \"%s\"", bucket);
		return FALSE;
	}

	return TRUE;
}

/* A query over @type for the organization, the customer when asked, and
 * the report's date window on @date_field. */
static VentureQuery *
gather_query(
	Gather		 *gather,
	GType		  type,
	const gchar	 *date_field,
	gboolean	  by_customer,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *from = NULL;
	g_autofree gchar *until = NULL;

	query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, gather->organization_id);

	if (by_customer && (0 != gather->customer_id) &&
	    !venture_query_add_filter_int(query, "customer-id", VENTURE_FILTER_OP_EQ,
	                                  gather->customer_id, error))
		return NULL;

	if (NULL != gather->start)
	{
		from = g_date_time_format_iso8601(gather->start);

		if (!venture_query_add_filter_string(query, date_field, VENTURE_FILTER_OP_GTE,
		                                     from, error))
			return NULL;
	}

	if (NULL != gather->end)
	{
		until = g_date_time_format_iso8601(gather->end);

		if (!venture_query_add_filter_string(query, date_field, VENTURE_FILTER_OP_LT,
		                                     until, error))
			return NULL;
	}

	return g_steal_pointer(&query);
}

/* Whether @invoice_id carries the cutover stamp: a migrated invoice is not
 * revenue booked here. Looked up once per invoice. */
static gboolean
gather_invoice_is_opening(Gather *gather, gint64 invoice_id, gboolean *stamped, GError **error)
{
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GDateTime) opening = NULL;
	gpointer known;

	if (g_hash_table_lookup_extended(gather->opening, &invoice_id, NULL, &known))
	{
		*stamped = GPOINTER_TO_INT(known);
		return TRUE;
	}

	invoice = venture_database_get(gather->database, VENTURE_TYPE_INVOICE, invoice_id, error);

	if (NULL == invoice)
		return FALSE;

	g_object_get(invoice, "opening-at", &opening, NULL);
	*stamped = (NULL != opening);
	g_hash_table_insert(gather->opening, g_memdup2(&invoice_id, sizeof(invoice_id)),
	                    GINT_TO_POINTER(*stamped));

	return TRUE;
}

static void
gather_add(Gather *gather, GDateTime *date, VentureMoney *amount, Side side, gboolean subtract)
{
	Evidence *evidence;

	if ((NULL == date) || (NULL == amount))
	{
		g_clear_pointer(&date, g_date_time_unref);
		g_clear_pointer(&amount, venture_money_free);
		return;
	}

	evidence = g_new0(Evidence, 1);
	evidence->date = date;
	evidence->amount = amount;
	evidence->side = side;
	evidence->subtract = subtract;
	g_ptr_array_add(gather->evidence, evidence);
}

/* Booked: issue events add, void events subtract, in their own bucket. */
static gboolean
gather_events(Gather *gather, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) events = NULL;
	guint i;

	query = gather_query(gather, VENTURE_TYPE_INVOICE_EVENT, "date", TRUE, error);

	if (NULL == query)
		return FALSE;

	if ((0 != gather->venture_id) &&
	    !venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                  gather->venture_id, error))
		return FALSE;

	events = venture_database_find(gather->database, query, error);

	if (NULL == events)
		return FALSE;

	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autofree gchar *kind = NULL;
		GDateTime *date = NULL;
		VentureMoney *amount = NULL;
		gint64 invoice_id = 0;
		gboolean stamped = FALSE;

		g_object_get(event, "kind", &kind, "invoice-id", &invoice_id, NULL);

		if ((0 != g_strcmp0(kind, "issue")) && (0 != g_strcmp0(kind, "void")))
			continue;

		if (!gather_invoice_is_opening(gather, invoice_id, &stamped, error))
			return FALSE;

		if (stamped)
			continue;

		g_object_get(event, "date", &date, "amount", &amount, NULL);
		gather_add(gather, date, amount, SIDE_BOOKED, 0 == g_strcmp0(kind, "void"));
	}

	return TRUE;
}

/* Without a venture: credit notes and write-offs reduce booked on their
 * own date; receipts add to received and refunds take from it. */
static gboolean
gather_by_customer(Gather *gather, GError **error)
{
	g_autoptr(VentureQuery) credit_query = NULL;
	g_autoptr(VentureQuery) payment_query = NULL;
	g_autoptr(VentureQuery) refund_query = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	guint i;

	credit_query = gather_query(gather, VENTURE_TYPE_CUSTOMER_CREDIT, "date", TRUE, error);
	payment_query = gather_query(gather, VENTURE_TYPE_PAYMENT, "date", TRUE, error);
	refund_query = gather_query(gather, VENTURE_TYPE_REFUND, "date", TRUE, error);

	if ((NULL == credit_query) || (NULL == payment_query) || (NULL == refund_query))
		return FALSE;

	credits = venture_database_find(gather->database, credit_query, error);
	payments = venture_database_find(gather->database, payment_query, error);
	refunds = venture_database_find(gather->database, refund_query, error);

	if ((NULL == credits) || (NULL == payments) || (NULL == refunds))
		return FALSE;

	for (i = 0; i < credits->len; i++)
	{
		VentureEntity *credit = g_ptr_array_index(credits, i);
		g_autofree gchar *kind = NULL;
		g_autoptr(GDateTime) opening = NULL;
		GDateTime *date = NULL;
		VentureMoney *amount = NULL;

		g_object_get(credit, "kind", &kind, "opening-at", &opening, NULL);

		/* Deposits and overpayments are the receipt's own money, already
		 * counted as received; a migrated credit was booked elsewhere. */
		if ((NULL != opening) ||
		    ((0 != g_strcmp0(kind, "credit_note")) && (0 != g_strcmp0(kind, "write_off"))))
			continue;

		g_object_get(credit, "date", &date, "amount", &amount, NULL);
		gather_add(gather, date, amount, SIDE_BOOKED, TRUE);
	}

	for (i = 0; i < payments->len; i++)
	{
		GDateTime *date = NULL;
		VentureMoney *amount = NULL;

		g_object_get(g_ptr_array_index(payments, i), "date", &date, "amount", &amount, NULL);
		gather_add(gather, date, amount, SIDE_RECEIVED, FALSE);
	}

	for (i = 0; i < refunds->len; i++)
	{
		GDateTime *date = NULL;
		VentureMoney *amount = NULL;

		g_object_get(g_ptr_array_index(refunds, i), "date", &date, "amount", &amount, NULL);
		gather_add(gather, date, amount, SIDE_RECEIVED, TRUE);
	}

	return TRUE;
}

/* Whether @allocation_id applies to one of the venture's invoices, and if
 * so from what: cash (a receipt, deposit or overpayment) or a credit note
 * or write-off. */
static gboolean
gather_allocation_side(
	Gather		 *gather,
	VentureEntity	 *allocation,
	gboolean	 *counts,
	Side		 *side,
	GError		**error
){
	g_autoptr(VentureEntity) credit = NULL;
	g_autofree gchar *kind = NULL;
	gint64 invoice_id = 0;
	gint64 credit_id = 0;

	g_object_get(allocation, "invoice-id", &invoice_id, "credit-id", &credit_id, NULL);
	*counts = g_hash_table_contains(gather->venture_invoices, &invoice_id);

	if (!*counts)
		return TRUE;

	if (0 == credit_id)
	{
		*side = SIDE_RECEIVED;
		return TRUE;
	}

	credit = venture_database_get(gather->database, VENTURE_TYPE_CUSTOMER_CREDIT, credit_id, error);

	if (NULL == credit)
		return FALSE;

	g_object_get(credit, "kind", &kind, NULL);
	*side = ((0 == g_strcmp0(kind, "credit_note")) || (0 == g_strcmp0(kind, "write_off")))
		? SIDE_BOOKED : SIDE_RECEIVED;

	return TRUE;
}

/* With a venture: applications to the venture's invoices are the cash and
 * credit evidence, and a refund of such an application reverses it. */
static gboolean
gather_by_venture(Gather *gather, GError **error)
{
	g_autoptr(VentureQuery) invoice_query = NULL;
	g_autoptr(VentureQuery) allocation_query = NULL;
	g_autoptr(VentureQuery) refund_query = NULL;
	g_autoptr(GPtrArray) invoices = NULL;
	g_autoptr(GPtrArray) allocations = NULL;
	g_autoptr(GPtrArray) refunds = NULL;
	guint i;

	invoice_query = venture_query_new(VENTURE_TYPE_INVOICE);
	venture_query_set_limit(invoice_query, 0);
	venture_query_set_include_deleted(invoice_query, TRUE);
	venture_query_set_organization(invoice_query, gather->organization_id);

	if (!venture_query_add_filter_int(invoice_query, "venture-id", VENTURE_FILTER_OP_EQ,
	                                  gather->venture_id, error))
		return FALSE;

	if ((0 != gather->customer_id) &&
	    !venture_query_add_filter_int(invoice_query, "company-id", VENTURE_FILTER_OP_EQ,
	                                  gather->customer_id, error))
		return FALSE;

	invoices = venture_database_find(gather->database, invoice_query, error);

	if (NULL == invoices)
		return FALSE;

	for (i = 0; i < invoices->len; i++)
	{
		gint64 id = venture_entity_get_id(g_ptr_array_index(invoices, i));

		g_hash_table_add(gather->venture_invoices, g_memdup2(&id, sizeof(id)));
	}

	allocation_query = gather_query(gather, VENTURE_TYPE_PAYMENT_ALLOCATION, "date", FALSE, error);
	refund_query = gather_query(gather, VENTURE_TYPE_REFUND, "date", TRUE, error);

	if ((NULL == allocation_query) || (NULL == refund_query))
		return FALSE;

	allocations = venture_database_find(gather->database, allocation_query, error);
	refunds = venture_database_find(gather->database, refund_query, error);

	if ((NULL == allocations) || (NULL == refunds))
		return FALSE;

	for (i = 0; i < allocations->len; i++)
	{
		VentureEntity *allocation = g_ptr_array_index(allocations, i);
		GDateTime *date = NULL;
		VentureMoney *amount = NULL;
		gboolean counts = FALSE;
		Side side = SIDE_RECEIVED;

		if (!gather_allocation_side(gather, allocation, &counts, &side, error))
			return FALSE;

		if (!counts)
			continue;

		g_object_get(allocation, "date", &date, "amount", &amount, NULL);
		gather_add(gather, date, amount, side, SIDE_BOOKED == side);
	}

	for (i = 0; i < refunds->len; i++)
	{
		VentureEntity *refund = g_ptr_array_index(refunds, i);
		g_autoptr(VentureEntity) allocation = NULL;
		GDateTime *date = NULL;
		VentureMoney *amount = NULL;
		gint64 allocation_id = 0;
		gboolean counts = FALSE;
		Side side = SIDE_RECEIVED;

		g_object_get(refund, "allocation-id", &allocation_id, NULL);

		/* A refund of unused credit never reached an invoice, so it is
		 * no venture's cash. */
		if (0 == allocation_id)
			continue;

		allocation = venture_database_get(gather->database, VENTURE_TYPE_PAYMENT_ALLOCATION,
		                                  allocation_id, error);

		if (NULL == allocation)
			return FALSE;

		if (!gather_allocation_side(gather, allocation, &counts, &side, error))
			return FALSE;

		if (!counts || (SIDE_RECEIVED != side))
			continue;

		g_object_get(refund, "date", &date, "amount", &amount, NULL);
		gather_add(gather, date, amount, SIDE_RECEIVED, TRUE);
	}

	return TRUE;
}

static Series *
series_for(GPtrArray *all, const gchar *currency, guint n_buckets)
{
	Series *series;
	guint i;

	for (i = 0; i < all->len; i++)
	{
		series = g_ptr_array_index(all, i);

		if (0 == g_strcmp0(series->currency, currency))
			return series;
	}

	series = series_new(currency, n_buckets);
	g_ptr_array_add(all, series);

	return series;
}

/* Folds one piece of evidence into its bucket's running total. */
static void
series_fold(GPtrArray *all, GPtrArray *buckets, const Evidence *evidence)
{
	Series *series;
	GPtrArray *column;
	VentureMoney *next;
	guint i;

	for (i = 0; i < buckets->len; i++)
	{
		if (venture_date_range_contains(g_ptr_array_index(buckets, i), evidence->date))
			break;
	}

	if (i == buckets->len)
		return;

	series = series_for(all, venture_money_get_currency(evidence->amount), buckets->len);
	column = (SIDE_BOOKED == evidence->side) ? series->booked : series->received;
	next = evidence->subtract
		? venture_money_subtract(g_ptr_array_index(column, i), evidence->amount, NULL)
		: venture_money_add(g_ptr_array_index(column, i), evidence->amount, NULL);
	venture_money_free(g_ptr_array_index(column, i));
	g_ptr_array_index(column, i) = next;
}

static void
money_accumulate(VentureMoney **total, const VentureMoney *amount)
{
	VentureMoney *next;

	next = venture_money_add(*total, amount, NULL);
	venture_money_free(*total);
	*total = next;
}

static void
gather_clear(Gather *gather)
{
	g_clear_pointer(&gather->start, g_date_time_unref);
	g_clear_pointer(&gather->end, g_date_time_unref);
	g_clear_pointer(&gather->evidence, g_ptr_array_unref);
	g_clear_pointer(&gather->opening, g_hash_table_unref);
	g_clear_pointer(&gather->venture_invoices, g_hash_table_unref);
}

VentureReportResult *
venture_cash_vs_booked_report(
	VentureContext		 *context,
	VentureDateRange	 *period,
	JsonObject		 *options,
	GError			**error
){
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) buckets = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GError) as_of_error = NULL;
	g_autoptr(VentureMoney) total_booked = NULL;
	g_autoptr(VentureMoney) total_received = NULL;
	g_autoptr(VentureMoney) total_gap = NULL;
	g_autoptr(GString) others = NULL;
	g_autofree gchar *booked_text = NULL;
	g_autofree gchar *received_text = NULL;
	g_autofree gchar *line = NULL;
	VentureMetric *metric;
	Gather gather;
	const gchar *currency;
	const gchar *bucket_name;
	gboolean ok;
	guint i;
	guint c;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != period, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (!cash_vs_booked_check_options(options, error))
		return NULL;

	as_of = venture_period_report_as_of(options, &as_of_error);

	if (NULL != as_of_error)
	{
		g_propagate_error(error, g_steal_pointer(&as_of_error));
		return NULL;
	}

	currency = (NULL != options)
		? venture_json_object_get_string(options, "currency", venture_money_get_default_currency())
		: venture_money_get_default_currency();
	bucket_name = (NULL != options)
		? venture_json_object_get_string(options, "bucket", "month") : "month";

	memset(&gather, 0, sizeof(gather));
	gather.database = venture_context_get_database(context);
	gather.organization_id = (NULL != options)
		? venture_json_object_get_int(options, "organization_id", 0) : 0;

	if (0 == gather.organization_id)
		gather.organization_id = venture_context_get_default_organization_id(context);

	gather.customer_id = (NULL != options) ? venture_json_object_get_int(options, "customer_id", 0) : 0;
	gather.venture_id = (NULL != options) ? venture_json_object_get_int(options, "venture_id", 0) : 0;
	gather.start = (NULL != venture_date_range_get_start(period))
		? g_date_time_ref(venture_date_range_get_start(period)) : NULL;
	gather.end = (NULL != venture_date_range_get_end(period))
		? g_date_time_ref(venture_date_range_get_end(period)) : NULL;

	/* as_of is an inclusive instant; the window's end is exclusive. */
	if (NULL != as_of)
	{
		g_autoptr(GDateTime) cutoff = g_date_time_add(as_of, 1);

		if ((NULL == gather.end) || (g_date_time_compare(cutoff, gather.end) < 0))
		{
			g_clear_pointer(&gather.end, g_date_time_unref);
			gather.end = g_steal_pointer(&cutoff);
		}
	}

	gather.evidence = g_ptr_array_new_with_free_func((GDestroyNotify)evidence_free);
	gather.opening = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	gather.venture_invoices = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	ok = gather_events(&gather, error) &&
		((0 != gather.venture_id) ? gather_by_venture(&gather, error)
		                          : gather_by_customer(&gather, error));

	if (!ok)
	{
		gather_clear(&gather);
		return NULL;
	}

	buckets = (0 == g_strcmp0(bucket_name, "week"))
		? venture_date_range_split_by_week(period)
		: venture_date_range_split_by_month(period);

	/* An open-ended period has no calendar to cut: it is its own bucket. */
	if (0 == buckets->len)
		g_ptr_array_add(buckets, venture_date_range_copy(period));

	all = g_ptr_array_new_with_free_func((GDestroyNotify)series_free);

	for (i = 0; i < gather.evidence->len; i++)
		series_fold(all, buckets, g_ptr_array_index(gather.evidence, i));

	gather_clear(&gather);

	/* Nothing at all is still a table of the months asked about. */
	if (0 == all->len)
		series_for(all, currency, buckets->len);

	g_ptr_array_sort(all, series_compare);

	result = venture_report_result_new("Cash vs booked", period);
	venture_report_result_add_column(result, "bucket",
		(0 == g_strcmp0(bucket_name, "week")) ? "Week" : "Month",
		VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "booked", "Booked", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "received", "Received", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "gap", "Gap", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "cumulative_gap", "Cumulative gap", VENTURE_REPORT_COLUMN_MONEY);

	total_booked = venture_money_new_zero(currency);
	total_received = venture_money_new_zero(currency);
	others = g_string_new(NULL);

	for (c = 0; c < all->len; c++)
	{
		Series *series = g_ptr_array_index(all, c);

		if (0 != g_strcmp0(series->currency, currency))
			g_string_append_printf(others, "%s%s", (others->len > 0) ? ", " : "", series->currency);
	}

	for (i = 0; i < buckets->len; i++)
	{
		VentureDateRange *bucket = g_ptr_array_index(buckets, i);

		for (c = 0; c < all->len; c++)
		{
			Series *series = g_ptr_array_index(all, c);
			g_autoptr(VentureMoney) gap = NULL;
			VentureMoney *booked = g_ptr_array_index(series->booked, i);
			VentureMoney *received = g_ptr_array_index(series->received, i);

			gap = venture_money_subtract(booked, received, NULL);
			money_accumulate(&series->running, gap);

			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "bucket", venture_date_range_get_label(bucket));
			venture_report_result_set_text(result, "currency", series->currency);
			venture_report_result_set_money(result, "booked", booked);
			venture_report_result_set_money(result, "received", received);
			venture_report_result_set_money(result, "gap", gap);
			venture_report_result_set_money(result, "cumulative_gap", series->running);

			if (0 == g_strcmp0(series->currency, currency))
			{
				money_accumulate(&total_booked, booked);
				money_accumulate(&total_received, received);
			}
		}
	}

	total_gap = venture_money_subtract(total_booked, total_received, NULL);
	booked_text = venture_money_to_display_string(total_booked, TRUE);
	received_text = venture_money_to_display_string(total_received, TRUE);
	line = g_strdup_printf("%s booked, %s received", booked_text, received_text);

	metric = venture_metric_new_money("booked", "Booked", total_booked);

	if (others->len > 0)
	{
		g_autofree gchar *note = g_strdup_printf("%s only; the rows also carry %s",
		                                         currency, others->str);

		venture_metric_set_note(metric, note);
	}

	venture_report_result_add_metric(result, metric);
	venture_report_result_add_metric(result,
		venture_metric_new_money("received", "Received", total_received));
	metric = venture_metric_new_money("gap", "Gap", total_gap);
	venture_metric_set_higher_is_better(metric, FALSE);
	venture_report_result_add_metric(result, metric);
	venture_report_result_add_metric(result,
		venture_metric_new_text("booked_vs_received", "Booked vs received", line));

	if (others->len > 0)
	{
		g_autofree gchar *note = g_strdup_printf(
			"Totals are in %s; %s rows are shown but never converted or added in.",
			currency, others->str);

		venture_report_result_append_note(result, note);
	}

	if (NULL != as_of)
	{
		g_autofree gchar *stamp = g_date_time_format_iso8601(as_of);
		g_autofree gchar *note = g_strdup_printf("Evidence dated after %s is not counted.", stamp);

		venture_report_result_append_note(result, note);
	}

	return g_steal_pointer(&result);
}

void
venture_cash_vs_booked_register_report(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));

	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"cash_vs_booked", "Cash vs booked",
		"Revenue booked (issued invoices less voids and credits) beside cash "
		"received (receipts less refunds) per month or week and per currency, "
		"with the gap and the gap accumulated across the period",
		venture_cash_vs_booked_report)));
}
