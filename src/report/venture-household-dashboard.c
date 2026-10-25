/*
 * venture-household-dashboard.c - the figures on the household dashboard
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two household reports the dashboard template charts beside the existing
 * monthly series and net worth:
 *
 * - household_spending: spending by category for the period, biggest
 *   first, per currency, with each category's share.
 * - household_cash_forecast: today's spendable balance (checking, savings
 *   and cash accounts) carried forward week by week through everything the
 *   money calendar says is due.
 *
 * Privacy is not decided here: every read runs under the caller's access
 * scope, so another member's unshared personal account, its spending and
 * its balance stay out of the figures until the owner shares it.
 */

#include "venture.h"

#include <string.h>

#define FORECAST_WEEKS 13

static gint64
org_option(VentureContext *context, JsonObject *options)
{
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	return org > 0 ? org : venture_context_get_default_organization_id(context);
}

/* Adds @amount into the per-currency total in @totals. */
static gboolean
accumulate(GHashTable *totals, const gchar *key, const VentureMoney *amount, GError **error)
{
	VentureMoney *current = g_hash_table_lookup(totals, key);
	VentureMoney *next;
	if (current == NULL)
	{
		g_hash_table_insert(totals, g_strdup(key), venture_money_copy(amount));
		return TRUE;
	}
	next = venture_money_add(current, amount, error);
	if (next == NULL)
		return FALSE;
	g_hash_table_insert(totals, g_strdup(key), next);
	return TRUE;
}

typedef struct
{
	gchar *category;
	VentureMoney *spent;
	guint items;
} Bucket;

static void
bucket_free(gpointer data)
{
	Bucket *b = data;
	g_free(b->category);
	venture_money_free(b->spent);
	g_free(b);
}

static gint
bucket_compare(gconstpointer a, gconstpointer b)
{
	const Bucket *x = *(Bucket *const *)a, *y = *(Bucket *const *)b;
	gint c = g_strcmp0(venture_money_get_currency(x->spent), venture_money_get_currency(y->spent));
	if (c != 0) return c;
	if (venture_money_get_amount(x->spent) != venture_money_get_amount(y->spent))
		return venture_money_get_amount(x->spent) > venture_money_get_amount(y->spent) ? -1 : 1;
	return g_strcmp0(x->category, y->category);
}

static VentureReportResult *
household_spending(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = venture_report_result_new("Spending by category", period);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EXPENSE);
	g_autoptr(GPtrArray) expenses = NULL;
	g_autoptr(GHashTable) buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(bucket_free);
	const VentureMoney *headline = NULL;
	guint i;

	venture_query_set_organization(query, org_option(context, options));
	if (period != NULL && !venture_query_set_date_range(query, "occurred-at", period, error))
		return NULL;
	venture_query_set_limit(query, 0);
	expenses = venture_database_find(venture_context_get_database(context), query, error);
	if (expenses == NULL)
		return NULL;
	for (i = 0; i < expenses->len; i++)
	{
		g_autofree gchar *category = NULL, *key = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		Bucket *b;
		VentureMoney *sum;
		g_object_get(g_ptr_array_index(expenses, i), "category", &category, "amount", &amount, NULL);
		if (amount == NULL)
			continue;
		if (venture_string_is_empty(category))
		{
			g_free(category);
			category = g_strdup("(no category)");
		}
		key = g_strdup_printf("%s|%s", category, venture_money_get_currency(amount));
		b = g_hash_table_lookup(buckets, key);
		if (b == NULL)
		{
			b = g_new0(Bucket, 1);
			b->category = g_strdup(category);
			b->spent = venture_money_new_zero(venture_money_get_currency(amount));
			g_ptr_array_add(rows, b);
			g_hash_table_insert(buckets, g_strdup(key), b);
		}
		sum = venture_money_add(b->spent, amount, error);
		if (sum == NULL)
			return NULL;
		venture_money_free(b->spent);
		b->spent = sum;
		b->items++;
		if (!accumulate(totals, venture_money_get_currency(amount), amount, error))
			return NULL;
	}
	g_ptr_array_sort(rows, bucket_compare);

	venture_report_result_add_column(result, "category", "Category", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "spent", "Spent", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "share", "Share", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "items", "Purchases", VENTURE_REPORT_COLUMN_NUMBER);
	for (i = 0; i < rows->len; i++)
	{
		Bucket *b = g_ptr_array_index(rows, i);
		const VentureMoney *total = g_hash_table_lookup(totals, venture_money_get_currency(b->spent));
		gdouble share = venture_money_get_amount(total) != 0
			? (gdouble)venture_money_get_amount(b->spent) / (gdouble)venture_money_get_amount(total) : 0.0;
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "category", b->category);
		venture_report_result_set_text(result, "currency", venture_money_get_currency(b->spent));
		venture_report_result_set_money(result, "spent", b->spent);
		venture_report_result_set_number(result, "share", share);
		venture_report_result_set_number(result, "items", (gdouble)b->items);
	}
	/* The headline is the largest currency; others stay in the table. */
	if (rows->len > 0)
		headline = g_hash_table_lookup(totals, venture_money_get_currency(((Bucket *)g_ptr_array_index(rows, 0))->spent));
	if (headline != NULL)
		venture_report_result_add_metric(result, venture_metric_new_money("total", "Spent", headline));
	venture_report_result_set_note(result,
		"Spending in the period by category, biggest first, separately in each currency. Moving money "
		"between your own accounts is not spending. Another member's personal account counts only once "
		"they share it with the household.");
	return g_steal_pointer(&result);
}

/* Monday of @when's week, at midnight UTC. */
static GDateTime *
week_start(GDateTime *when)
{
	g_autoptr(GDateTime) day = g_date_time_new_utc(g_date_time_get_year(when), g_date_time_get_month(when),
		g_date_time_get_day_of_month(when), 0, 0, 0);
	return g_date_time_add_days(day, 1 - g_date_time_get_day_of_week(day));
}

static GDateTime *
as_of_option(JsonObject *options, GError **error)
{
	const gchar *text = options != NULL ? venture_json_object_get_string(options, "as_of", NULL) : NULL;
	g_autofree gchar *iso = NULL;
	GDateTime *parsed;
	if (venture_string_is_empty(text))
	{
		g_autoptr(GDateTime) now = venture_time_now();
		return g_date_time_new_utc(g_date_time_get_year(now), g_date_time_get_month(now),
			g_date_time_get_day_of_month(now), 0, 0, 0);
	}
	iso = strlen(text) == 10 ? g_strdup_printf("%sT00:00:00Z", text) : g_strdup(text);
	parsed = g_date_time_new_from_iso8601(iso, NULL);
	if (parsed == NULL)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "as_of is not a date: %s", text);
	return parsed;
}

/* Spendable balances by currency: checking, savings and cash accounts the
 * caller can see, at the end of @as_of's day. */
static GHashTable *
spendable_balances(VentureContext *context, gint64 org, GDateTime *as_of, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(GHashTable) totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GHashTable) codes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_ACCOUNT);
	g_autoptr(GPtrArray) banks = NULL;
	g_autoptr(VentureLedgerBalances) ledger = NULL;
	g_autoptr(VentureReportResult) balances = NULL;
	g_autoptr(GDateTime) start = g_date_time_new_utc(1970, 1, 1, 0, 0, 0);
	g_autoptr(GDateTime) end = g_date_time_add_days(as_of, 1);
	g_autoptr(VentureDateRange) span = venture_date_range_new(start, end);
	guint i;

	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	banks = venture_database_find(db, query, error);
	if (banks == NULL)
		return NULL;
	for (i = 0; i < banks->len; i++)
	{
		VentureEntity *bank = g_ptr_array_index(banks, i);
		g_autoptr(VentureEntity) account = NULL;
		g_autofree gchar *code = NULL;
		gint kind = 0;
		gint64 account_id = 0;
		g_object_get(bank, "kind", &kind, "account-id", &account_id, NULL);
		if (kind != VENTURE_BANK_ACCOUNT_CHECKING && kind != VENTURE_BANK_ACCOUNT_SAVINGS &&
			kind != VENTURE_BANK_ACCOUNT_CASH)
			continue;
		if (account_id > 0)
			account = venture_database_get(db, VENTURE_TYPE_ACCOUNT, account_id, NULL);
		if (account == NULL)
			continue;
		g_object_get(account, "code", &code, NULL);
		if (code != NULL)
			g_hash_table_add(codes, g_steal_pointer(&code));
	}
	if (g_hash_table_size(codes) == 0)
		return g_steal_pointer(&totals);
	ledger = venture_ledger_balances_new(db);
	balances = venture_ledger_balances_query(ledger, org, NULL, span, NULL, FALSE, error);
	if (balances == NULL)
		return NULL;
	for (i = 0; i < venture_report_result_get_row_count(balances); i++)
	{
		const GValue *key = venture_report_result_get_cell(balances, i, "key");
		const GValue *closing = venture_report_result_get_cell(balances, i, "closing");
		const VentureMoney *amount;
		if (key == NULL || closing == NULL || !g_hash_table_contains(codes, g_value_get_string(key)))
			continue;
		amount = g_value_get_boxed(closing);
		if (amount != NULL && !accumulate(totals, venture_money_get_currency(amount), amount, error))
			return NULL;
	}
	return g_steal_pointer(&totals);
}

static VentureReportResult *
household_cash_forecast(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = venture_report_result_new("Cash forecast", period);
	g_autoptr(GDateTime) as_of = NULL, first = NULL, until = NULL;
	g_autoptr(GHashTable) balances = NULL;
	g_autoptr(GHashTable) flows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GList) currencies = NULL;
	gint64 org = org_option(context, options);
	gint64 weeks = options != NULL ? venture_json_object_get_int(options, "weeks", FORECAST_WEEKS) : FORECAST_WEEKS;
	GList *c;
	guint i;
	gint w;

	if (weeks < 1 || weeks > 53)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "weeks must be 1 to 53");
		return NULL;
	}
	as_of = as_of_option(options, error);
	if (as_of == NULL)
		return NULL;
	first = week_start(as_of);
	until = g_date_time_add_weeks(first, (gint)weeks);
	balances = spendable_balances(context, org, as_of, error);
	if (balances == NULL)
		return NULL;
	/* What is due from today on; an overdue bill is carried to today. */
	if (venture_context_module_enabled(context, "money_calendar"))
	{
		g_autoptr(VentureDateRange) range = venture_date_range_new(as_of, until);
		events = venture_money_calendar_events(context, org, range, as_of, error);
		if (events == NULL)
			return NULL;
		for (i = 0; i < events->len; i++)
		{
			VentureMoneyCalendarEvent *e = g_ptr_array_index(events, i);
			g_autoptr(GDateTime) monday = NULL;
			g_autofree gchar *day = NULL, *key = NULL;
			const VentureMoney *amount = venture_money_calendar_event_get_amount(e);
			if (!venture_money_calendar_event_get_counts_in_net(e) || amount == NULL)
				continue;
			monday = week_start(venture_money_calendar_event_get_date(e));
			day = g_date_time_format(monday, "%Y-%m-%d");
			key = g_strdup_printf("%s|%s|%s", day, venture_money_get_currency(amount),
				venture_money_calendar_event_get_direction(e));
			if (!accumulate(flows, key, amount, error))
				return NULL;
			if (!g_hash_table_contains(balances, venture_money_get_currency(amount)))
				g_hash_table_insert(balances, g_strdup(venture_money_get_currency(amount)),
					venture_money_new_zero(venture_money_get_currency(amount)));
		}
	}

	venture_report_result_add_column(result, "week", "Week of", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "start", "Start", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "in", "Coming in", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "out", "Going out", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "end", "End", VENTURE_REPORT_COLUMN_MONEY);

	currencies = g_list_sort(g_hash_table_get_keys(balances), (GCompareFunc)g_strcmp0);
	for (c = currencies; c != NULL; c = c->next)
	{
		const gchar *currency = c->data;
		g_autoptr(VentureMoney) running = venture_money_copy(g_hash_table_lookup(balances, currency));
		for (w = 0; w < weeks; w++)
		{
			g_autoptr(GDateTime) monday = g_date_time_add_weeks(first, w);
			g_autofree gchar *day = g_date_time_format(monday, "%Y-%m-%d");
			g_autofree gchar *in_key = g_strdup_printf("%s|%s|in", day, currency);
			g_autofree gchar *out_key = g_strdup_printf("%s|%s|out", day, currency);
			g_autoptr(VentureMoney) in = venture_money_new_zero(currency);
			g_autoptr(VentureMoney) out = venture_money_new_zero(currency);
			g_autoptr(VentureMoney) gained = NULL, end = NULL;
			const VentureMoney *found;
			if ((found = g_hash_table_lookup(flows, in_key)) != NULL)
			{
				venture_money_free(in);
				in = venture_money_copy(found);
			}
			if ((found = g_hash_table_lookup(flows, out_key)) != NULL)
			{
				venture_money_free(out);
				out = venture_money_copy(found);
			}
			gained = venture_money_add(running, in, error);
			if (gained == NULL)
				return NULL;
			end = venture_money_subtract(gained, out, error);
			if (end == NULL)
				return NULL;
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "week", day);
			venture_report_result_set_text(result, "currency", currency);
			venture_report_result_set_money(result, "start", running);
			venture_report_result_set_money(result, "in", in);
			venture_report_result_set_money(result, "out", out);
			venture_report_result_set_money(result, "end", end);
			venture_money_free(running);
			running = g_steal_pointer(&end);
		}
	}
	venture_report_result_set_note(result,
		"Starts from today's balance in checking, savings and cash accounts and adds what the money "
		"calendar says comes in and goes out each week: bills, repeating entries and invoices. A guide, "
		"not a promise. Another member's personal account counts only once they share it.");
	return g_steal_pointer(&result);
}

static void
copy_money(VentureReportResult *from, guint row, const gchar *from_key, VentureReportResult *to, const gchar *to_key)
{
	const GValue *cell = venture_report_result_get_cell(from, row, from_key);
	if (cell != NULL && G_VALUE_HOLDS(cell, VENTURE_TYPE_MONEY))
		venture_report_result_set_money(to, to_key, g_value_get_boxed(cell));
}

/* The monthly series in household words: money in, spent, left over. */
static VentureReportResult *
household_months(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureReport *monthly = venture_report_registry_lookup(venture_context_get_report_registry(context), "monthly");
	g_autoptr(VentureReportResult) series = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Money in and out by month", period);
	guint i;
	if (monthly == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The monthly report is not available");
		return NULL;
	}
	series = venture_report_generate(monthly, context, period, options, error);
	if (series == NULL)
		return NULL;
	venture_report_result_add_column(result, "month", "Month", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "money_in", "Money in", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "spent", "Spent", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "left_over", "Left over", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < venture_report_result_get_row_count(series); i++)
	{
		const GValue *month = venture_report_result_get_cell(series, i, "month");
		const GValue *currency = venture_report_result_get_cell(series, i, "currency");
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "month", month != NULL ? g_value_get_string(month) : "");
		venture_report_result_set_text(result, "currency", currency != NULL ? g_value_get_string(currency) : "");
		copy_money(series, i, "revenue", result, "money_in");
		copy_money(series, i, "expenses", result, "spent");
		copy_money(series, i, "profit", result, "left_over");
	}
	venture_report_result_set_note(result,
		"Money in is income recorded under Income; spent is spending; left over is the difference. Moving "
		"money between your own accounts is neither. Another member's personal account counts only once "
		"they share it.");
	return g_steal_pointer(&result);
}

void
venture_household_dashboard_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"household_spending", "Spending by category",
		"Spending in the period by category, biggest first, per currency, with each category's share. "
		"Options: organization_id",
		household_spending)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"household_cash_forecast", "Cash forecast",
		"Today's checking, savings and cash balance carried forward week by week through what is due. "
		"Options: organization_id, as_of (YYYY-MM-DD), weeks (1 to 53, default 13)",
		household_cash_forecast)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"household_months", "Money in and out by month",
		"Money in, spent and left over month by month, in household words. Options: organization_id",
		household_months)));
}
