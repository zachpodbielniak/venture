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
	gint64 customer;

	query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, organization(context, options));
	customer = option_id(options, "customer_id");
	if (customer != 0 && !venture_query_add_filter_int(query, "customer-id", VENTURE_FILTER_OP_EQ, customer, error))
		return NULL;
	text = g_date_time_format_iso8601(end);
	if (!venture_query_add_filter_string(query, "date", VENTURE_FILTER_OP_LT, text, error))
		return NULL;
	if (type == VENTURE_TYPE_INVOICE_EVENT && option_id(options, "venture_id") != 0 &&
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
receivables_aging(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
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
	VentureSettlementService *service;
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
	events = report_events(context, VENTURE_TYPE_INVOICE_EVENT, options, end, error);
	if (events == NULL)
		return NULL;
	service = venture_settlement_service_get(venture_context_get_database(context));
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
		gint64 invoice_id;
		guint bucket;

		event = g_ptr_array_index(events, i);
		g_object_get(event, "kind", &kind, "invoice-id", &invoice_id, "due-at", &due, NULL);
		if (g_strcmp0(kind, "issue") != 0)
			continue;
		balance = venture_settlement_service_invoice_balance(service, invoice_id, end, error);
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
	result = venture_report_result_new("Receivables aging", period);
	venture_report_result_add_column(result, "age", "Age", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "count", "Invoices", VENTURE_REPORT_COLUMN_NUMBER);
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
	venture_report_result_add_metric(result, venture_metric_new_count("invoices", "Open invoices", open));
	if (excluded > 0)
	{
		g_autofree gchar *note = NULL;

		note = g_strdup_printf("%u invoices in other currencies are excluded; select their currency to report them.", excluded);
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
customer_statement(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
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

	if (option_id(options, "customer_id") <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A customer statement requires customer_id");
		return NULL;
	}
	if (option_id(options, "venture_id") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A customer balance includes all ventures within one organization");
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
	types[0] = VENTURE_TYPE_INVOICE_EVENT;
	types[1] = VENTURE_TYPE_PAYMENT;
	types[2] = VENTURE_TYPE_CUSTOMER_CREDIT;
	types[3] = VENTURE_TYPE_REFUND;
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
	result = venture_report_result_new("Customer statement", period);
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
		venture_report_result_set_money(result, "debit", row->subtract ? zero : row->amount);
		venture_report_result_set_money(result, "credit", row->subtract ? row->amount : zero);
		venture_report_result_set_money(result, "balance", balance);
	}
	venture_report_result_add_metric(result, venture_metric_new_money("opening", "Opening balance", opening));
	venture_report_result_add_metric(result, venture_metric_new_money("balance", "Balance", balance));
	return g_steal_pointer(&result);
}

typedef struct
{
	gchar *code;
	gchar *jurisdiction;
	gboolean recoverable;
	VentureMoney *taxable;
	VentureMoney *tax;
} TaxRow;

static void
tax_row_free(gpointer data)
{
	TaxRow *row = data;
	if (row == NULL)
		return;
	g_free(row->code);
	g_free(row->jurisdiction);
	g_clear_pointer(&row->taxable, venture_money_free);
	g_clear_pointer(&row->tax, venture_money_free);
	g_free(row);
}

static gboolean
add_tax_row(GHashTable *rows, VentureDatabase *database, gint64 tax_code_id,
	const VentureMoney *taxable, const VentureMoney *tax, const gchar *currency, GError **error)
{
	TaxRow *row;
	gpointer key;

	if (tax == NULL || g_strcmp0(venture_money_get_currency(tax), currency) != 0)
		return TRUE;
	key = GINT_TO_POINTER(tax_code_id);
	row = g_hash_table_lookup(rows, key);
	if (row == NULL)
	{
		row = g_new0(TaxRow, 1);
		row->taxable = venture_money_new_zero(currency);
		row->tax = venture_money_new_zero(currency);
		if (tax_code_id != 0)
		{
			g_autoptr(VentureEntity) code = venture_database_get(database, VENTURE_TYPE_TAX_CODE, tax_code_id, error);
			if (code == NULL)
				return FALSE;
			g_object_get(code, "code", &row->code, "jurisdiction", &row->jurisdiction,
				"recoverable", &row->recoverable, NULL);
		}
		else
			row->code = g_strdup("percent");
		g_hash_table_insert(rows, key, row);
	}
	if (taxable != NULL && !add(&row->taxable, taxable, error))
		return FALSE;
	return add(&row->tax, tax, error);
}

static VentureReportResult *
tax_liability(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) grouped = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, tax_row_free);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GDateTime) end = NULL;
	const gchar *currency;
	GHashTableIter iter;
	TaxRow *row;
	VentureDatabase *database;

	end = cutoff(period, options, error);
	if (end == NULL)
		return NULL;
	currency = report_currency(options);
	database = venture_context_get_database(context);
	events = report_events(context, VENTURE_TYPE_INVOICE_EVENT, options, end, error);
	if (events == NULL)
		return NULL;
	{
		guint i;
		for (i = 0; i < events->len; i++)
		{
			VentureEntity *event = g_ptr_array_index(events, i);
			g_autoptr(GPtrArray) lines = NULL;
			g_autofree gchar *kind = NULL;
			g_autoptr(VentureQuery) query = NULL;
			guint j;
			gint64 invoice_id = 0;
			g_object_get(event, "kind", &kind, "invoice-id", &invoice_id, NULL);
			if (g_strcmp0(kind, "issue") != 0)
				continue;
			query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
			venture_query_set_limit(query, 0);
			if (!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ,
				invoice_id, error))
				return NULL;
			lines = venture_database_find(database, query, error);
			if (lines == NULL)
				return NULL;
			for (j = 0; j < lines->len; j++)
			{
				VentureEntity *line = g_ptr_array_index(lines, j);
				g_autoptr(VentureMoney) taxable = NULL;
				g_autoptr(VentureMoney) tax = NULL;
				gint64 tax_code_id = 0;
				if (venture_entity_is_deleted(line))
					continue;
				g_object_get(line, "income-amount", &taxable, "tax-amount", &tax,
					"tax-code-id", &tax_code_id, NULL);
				if (!add_tax_row(grouped, database, tax_code_id, taxable, tax, currency, error))
					return NULL;
			}
		}
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "vendor_bill_line") != G_TYPE_INVALID)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENDOR_BILL_LINE);
		g_autoptr(GPtrArray) lines = NULL;
		guint i;
		venture_query_set_limit(query, 0);
		venture_query_set_organization(query, organization(context, options));
		lines = venture_database_find(database, query, error);
		if (lines == NULL)
			return NULL;
		for (i = 0; i < lines->len; i++)
		{
			VentureEntity *line = g_ptr_array_index(lines, i);
			g_autoptr(VentureMoney) tax = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autoptr(VentureMoney) taxable = NULL;
			gint64 tax_code_id = 0;
			if (venture_entity_is_deleted(line))
				continue;
			g_object_get(line, "tax-amount", &tax, "tax-code-id", &tax_code_id, NULL);
			amount = venture_vendor_bill_line_get_amount(VENTURE_VENDOR_BILL_LINE(line), error);
			if (amount == NULL)
				return NULL;
			if (tax != NULL)
				taxable = venture_money_subtract(amount, tax, error);
			else
				taxable = venture_money_copy(amount);
			if (taxable == NULL)
				return NULL;
			if (!add_tax_row(grouped, database, tax_code_id, taxable, tax, currency, error))
				return NULL;
		}
	}
	result = venture_report_result_new("Tax liability by code", period);
	venture_report_result_add_column(result, "code", "Tax code", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "jurisdiction", "Jurisdiction", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "taxable", "Taxable", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "tax", "Tax", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "recoverable", "Recoverable", VENTURE_REPORT_COLUMN_TEXT);
	g_hash_table_iter_init(&iter, grouped);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&row))
	{
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "code", row->code);
		venture_report_result_set_text(result, "jurisdiction", row->jurisdiction);
		venture_report_result_set_money(result, "taxable", row->taxable);
		venture_report_result_set_money(result, "tax", row->tax);
		venture_report_result_set_text(result, "recoverable", row->recoverable ? "yes" : "no");
	}
	return g_steal_pointer(&result);
}

void
venture_receivables_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"receivables", "Receivables aging", "Outstanding issued amounts less dated allocations, as of the period end.", receivables_aging)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"customer_statement", "Customer statement", "Dated customer movements and balance in one organization and currency; requires customer_id.", customer_statement)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"tax_liability", "Tax liability by code", "Frozen invoice and bill tax grouped by tax code and jurisdiction.", tax_liability)));
}
