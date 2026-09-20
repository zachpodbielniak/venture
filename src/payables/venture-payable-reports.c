/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gint64
option_id(JsonObject *options, const gchar *name)
{
	return options != NULL ? venture_json_object_get_int(options, name, 0) : 0;
}

static gint64
organization(VentureContext *context, JsonObject *options)
{
	gint64 id;

	id = option_id(options, "organization_id");
	return id != 0 ? id : venture_context_get_default_organization_id(context);
}

static const gchar *
report_currency(JsonObject *options)
{
	return options != NULL ? venture_json_object_get_string(options, "currency", venture_money_get_default_currency()) : venture_money_get_default_currency();
}

static GDateTime *
cutoff(VentureDateRange *period, JsonObject *options, GError **error)
{
	GDateTime *end;
	g_autoptr(GDateTime) now = NULL;

	end = period != NULL ? venture_date_range_get_end(period) : NULL;
	if (options != NULL && json_object_has_member(options, "as_of"))
	{
		g_autoptr(GDateTime) as_of = venture_period_report_as_of(options, error);
		if (as_of == NULL)
			return NULL;
		now = g_date_time_add(as_of, 1);
	}
	else
		now = venture_time_now();
	return end != NULL && g_date_time_compare(end, now) < 0 ? g_date_time_ref(end) : g_steal_pointer(&now);
}

static GPtrArray *
report_events(VentureContext *context, GType type, JsonObject *options, GDateTime *end, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *text = NULL;
	gint64 vendor;

	query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, organization(context, options));
	vendor = option_id(options, "vendor_id");
	if (vendor != 0 && !venture_query_add_filter_int(query, "vendor-id", VENTURE_FILTER_OP_EQ, vendor, error))
		return NULL;
	text = g_date_time_format_iso8601(end);
	if (!venture_query_add_filter_string(query, "date", VENTURE_FILTER_OP_LT, text, error))
		return NULL;
	if (type == VENTURE_TYPE_VENDOR_BILL_EVENT && option_id(options, "venture_id") != 0 &&
		!venture_query_add_filter_int(query, "venture-id", VENTURE_FILTER_OP_EQ, option_id(options, "venture_id"), error))
		return NULL;
	return venture_database_find(venture_context_get_database(context), query, error);
}

static gboolean
add(VentureMoney **total, const VentureMoney *amount, GError **error)
{
	VentureMoney *next;

	next = venture_money_add(*total, amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*total);
	*total = next;
	return TRUE;
}

static VentureReportResult *
payables_aging(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	static const gchar *const labels[] = {
		"Current", "1-30 days", "31-60 days", "61-90 days", "Over 90 days", "No due date"
	};
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) amounts = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GDateTime) aged_at = NULL;
	g_autoptr(VentureMoney) outstanding = NULL;
	g_autoptr(VentureMoney) overdue = NULL;
	VenturePayablesService *service;
	const gchar *currency;
	guint counts[G_N_ELEMENTS(labels)] = { 0 };
	guint open;
	guint excluded;
	guint i;

	end = cutoff(period, options, error);
	if (end == NULL)
		return NULL;
	/* The exclusive endpoint belongs to the next day/month. Age at the
	 * final included instant, while filtering events with the endpoint. */
	aged_at = g_date_time_add(end, -1);
	events = report_events(context, VENTURE_TYPE_VENDOR_BILL_EVENT, options, end, error);
	if (events == NULL)
		return NULL;
	service = venture_payables_service_get(venture_context_get_database(context));
	currency = report_currency(options);
	outstanding = venture_money_new_zero(currency);
	overdue = venture_money_new_zero(currency);
	amounts = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	for (i = 0; i < G_N_ELEMENTS(labels); i++)
		g_ptr_array_add(amounts, venture_money_new_zero(currency));
	open = 0;
	excluded = 0;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event;
		g_autofree gchar *kind = NULL;
		g_autoptr(GDateTime) due = NULL;
		g_autoptr(VentureMoney) balance = NULL;
		VentureMoney *next;
		gint64 bill_id;
		guint bucket;

		event = g_ptr_array_index(events, i);
		g_object_get(event, "kind", &kind, "bill-id", &bill_id, "due-date", &due, NULL);
		if (g_strcmp0(kind, "issue") != 0)
			continue;
		balance = venture_payables_service_bill_balance(service, bill_id, end, error);
		if (balance == NULL)
			return NULL;
		if (venture_money_is_zero(balance))
			continue;
		if (g_strcmp0(venture_money_get_currency(balance), currency) != 0)
		{
			excluded++;
			continue;
		}
		bucket = 5;
		if (due != NULL)
		{
			gint64 days;

			days = g_date_time_difference(aged_at, due) / G_TIME_SPAN_DAY;
			bucket = days <= 0 ? 0 : (days <= 30 ? 1 : (days <= 60 ? 2 : (days <= 90 ? 3 : 4)));
		}
		next = venture_money_add(g_ptr_array_index(amounts, bucket), balance, error);
		if (next == NULL)
			return NULL;
		venture_money_free(g_ptr_array_index(amounts, bucket));
		g_ptr_array_index(amounts, bucket) = next;
		if (!add(&outstanding, balance, error) || (bucket > 0 && bucket < 5 && !add(&overdue, balance, error)))
			return NULL;
		counts[bucket]++;
		open++;
	}
	result = venture_report_result_new("Payables aging", period);
	venture_report_result_add_column(result, "age", "Age", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "count", "Bills", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "amount", "Amount", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < G_N_ELEMENTS(labels); i++)
	{
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "age", labels[i]);
		venture_report_result_set_number(result, "count", counts[i]);
		venture_report_result_set_money(result, "amount", g_ptr_array_index(amounts, i));
	}
	venture_report_result_add_metric(result, venture_metric_new_money("outstanding", "Outstanding", outstanding));
	venture_report_result_add_metric(result, venture_metric_new_money("overdue", "Overdue", overdue));
	venture_report_result_add_metric(result, venture_metric_new_count("bills", "Open bills", open));
	if (excluded > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%u bills in other currencies are excluded; select their currency to report them.", excluded);
		venture_report_result_append_note(result, note);
	}
	return g_steal_pointer(&result);
}

typedef struct
{
	VentureEntity *record;
	VentureMoney *amount;
	GDateTime *date;
	gboolean subtract;
} StatementRow;

static void
statement_row_free(gpointer data)
{
	StatementRow *row;

	row = data;
	g_clear_object(&row->record);
	g_clear_pointer(&row->amount, venture_money_free);
	g_clear_pointer(&row->date, g_date_time_unref);
	g_free(row);
}

static gint
statement_row_compare(gconstpointer a, gconstpointer b)
{
	const StatementRow *left;
	const StatementRow *right;
	gint order;

	left = *(StatementRow *const *)a;
	right = *(StatementRow *const *)b;
	order = g_date_time_compare(left->date, right->date);
	if (order == 0)
		order = g_strcmp0(venture_entity_get_entity_name(left->record), venture_entity_get_entity_name(right->record));
	if (order == 0)
		order = venture_entity_get_id(left->record) < venture_entity_get_id(right->record) ? -1 : 1;
	return order;
}

static VentureReportResult *
vendor_statement(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	GType types[4];
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureMoney) balance = NULL;
	g_autoptr(VentureMoney) opening = NULL;
	g_autoptr(VentureMoney) zero = NULL;
	GDateTime *start;
	const gchar *currency;
	guint t;
	guint i;

	if (option_id(options, "vendor_id") <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A vendor statement requires vendor_id");
		return NULL;
	}
	if (option_id(options, "venture_id") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A vendor balance includes all ventures within one organization");
		return NULL;
	}
	end = cutoff(period, options, error);
	if (end == NULL)
		return NULL;
	start = period != NULL ? venture_date_range_get_start(period) : NULL;
	currency = report_currency(options);
	zero = venture_money_new_zero(currency);
	opening = venture_money_new_zero(currency);
	balance = venture_money_new_zero(currency);
	rows = g_ptr_array_new_with_free_func(statement_row_free);
	types[0] = VENTURE_TYPE_VENDOR_BILL_EVENT;
	types[1] = VENTURE_TYPE_BILL_PAYMENT;
	types[2] = VENTURE_TYPE_VENDOR_CREDIT;
	types[3] = VENTURE_TYPE_BILL_REFUND;
	for (t = 0; t < G_N_ELEMENTS(types); t++)
	{
		g_autoptr(GPtrArray) events = NULL;

		events = report_events(context, types[t], options, end, error);
		if (events == NULL)
			return NULL;
		for (i = 0; i < events->len; i++)
		{
			VentureEntity *event;
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *kind = NULL;
			StatementRow *row;
			gboolean subtract;
			gint64 payment_id;

			event = g_ptr_array_index(events, i);
			subtract = t == 1 || t == 2;
			if (t == 0)
			{
				g_object_get(event, "kind", &kind, NULL);
				if (g_strcmp0(kind, "issue") != 0 && g_strcmp0(kind, "void") != 0)
					continue;
				subtract = g_strcmp0(kind, "void") == 0;
			}
			if (t == 2)
			{
				g_object_get(event, "payment-id", &payment_id, NULL);
				if (payment_id != 0)
					continue;
			}
			g_object_get(event, "amount", &amount, NULL);
			if (amount == NULL || g_strcmp0(venture_money_get_currency(amount), currency) != 0)
				continue;
			row = g_new0(StatementRow, 1);
			row->record = g_object_ref(event);
			row->amount = g_steal_pointer(&amount);
			g_object_get(event, "date", &row->date, NULL);
			row->subtract = subtract;
			g_ptr_array_add(rows, row);
		}
	}
	g_ptr_array_sort(rows, statement_row_compare);
	result = venture_report_result_new("Vendor statement", period);
	venture_report_result_add_column(result, "date", "Date", VENTURE_REPORT_COLUMN_DATE);
	venture_report_result_add_column(result, "source", "Source", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "debit", "Debit", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "credit", "Credit", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "balance", "Balance", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < rows->len; i++)
	{
		StatementRow *row;
		VentureMoney *next;
		g_autofree gchar *date = NULL;
		g_autofree gchar *source = NULL;

		row = g_ptr_array_index(rows, i);
		next = row->subtract ? venture_money_subtract(balance, row->amount, error) : venture_money_add(balance, row->amount, error);
		if (next == NULL)
			return NULL;
		venture_money_free(balance);
		balance = next;
		if (start != NULL && g_date_time_compare(row->date, start) < 0)
		{
			g_clear_pointer(&opening, venture_money_free);
			opening = venture_money_copy(balance);
			continue;
		}
		date = g_date_time_format_iso8601(row->date);
		source = g_strdup_printf("%s #%" G_GINT64_FORMAT, venture_entity_get_entity_name(row->record), venture_entity_get_id(row->record));
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "date", date);
		venture_report_result_set_text(result, "source", source);
		venture_report_result_set_money(result, "debit", row->subtract ? row->amount : zero);
		venture_report_result_set_money(result, "credit", row->subtract ? zero : row->amount);
		venture_report_result_set_money(result, "balance", balance);
	}
	venture_report_result_add_metric(result, venture_metric_new_money("opening", "Opening balance", opening));
	venture_report_result_add_metric(result, venture_metric_new_money("balance", "Balance", balance));
	return g_steal_pointer(&result);
}

void
venture_payables_register_reports(VentureReportRegistry *registry)
{
	VentureFuncReport *aging = venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "payables", "Payables aging",
		"Approved amounts less dated allocations plus refunds, as of the period end.", payables_aging);
	g_object_set(aging, "financial", TRUE, NULL);
	venture_report_registry_add(registry, VENTURE_REPORT(aging));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"vendor_statement", "Vendor statement", "Dated vendor movements and balance in one organization and currency; requires vendor_id.", vendor_statement)));
}
