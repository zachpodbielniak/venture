/*
 * venture-money-calendar-report.c - the money_calendar report
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One row per event with its day, ISO week, kind, amount, direction and
 * record, plus the net of that day and that week in the event's currency.
 * Nets are never folded across currencies: a day with a USD invoice and a
 * EUR bill has a USD net and a EUR net, both shown, neither converted.
 */

#include "venture.h"
#include "money-calendar/venture-money-calendar-private.h"

#include <string.h>

static gboolean
known_kind(const gchar *kind)
{
	g_autoptr(GPtrArray) kinds = venture_money_calendar_expander_kinds(venture_money_calendar_expander_get_default());
	guint i;
	for (i = 0; i < kinds->len; i++)
		if (g_strcmp0(g_ptr_array_index(kinds, i), kind) == 0)
			return TRUE;
	return FALSE;
}

/* The report's period, unless from/to name their own inclusive span. */
static VentureDateRange *
report_range(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	const gchar *from = options ? venture_json_object_get_string(options, "from", NULL) : NULL;
	const gchar *to = options ? venture_json_object_get_string(options, "to", NULL) : NULL;
	g_autofree gchar *span = NULL;
	if (venture_string_is_empty(from) && venture_string_is_empty(to))
		return venture_date_range_copy(period);
	if (venture_string_is_empty(from) || venture_string_is_empty(to))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"from and to go together: both dates, or a period");
		return NULL;
	}
	span = g_strdup_printf("%s..%s", from, to);
	return venture_context_parse_period(context, span, error);
}

static VentureReportResult *
venture_report_money_calendar(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(GDateTime) today = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GHashTable) day_nets = NULL;
	g_autoptr(GHashTable) week_nets = NULL;
	g_autoptr(GHashTable) totals = NULL;
	const gchar *kind = options ? venture_json_object_get_string(options, "kind", NULL) : NULL;
	const gchar *as_of = options ? venture_json_object_get_string(options, "as_of", NULL) : NULL;
	const gchar *currency = options ? venture_json_object_get_string(options, "currency", NULL) : NULL;
	gint64 organization_id = options ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	gint64 company_id = options ? venture_json_object_get_int(options, "customer_id", 0) : 0;
	gint64 venture_id = options ? venture_json_object_get_int(options, "venture_id", 0) : 0;
	guint i;

	if (company_id <= 0 && options != NULL)
		company_id = venture_json_object_get_int(options, "vendor_id", 0);
	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(context);
	if (!venture_string_is_empty(kind) && !known_kind(kind))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"\"%s\" is not a money calendar kind; try recurring, bill, invoice, dunning, payroll or tax", kind);
		return NULL;
	}
	range = report_range(context, period, options, error);
	if (range == NULL)
		return NULL;
	if (!venture_string_is_empty(as_of))
	{
		today = venture_time_from_string(as_of, error);
		if (today == NULL)
			return NULL;
	}
	all = venture_money_calendar_events(context, organization_id, range, today, error);
	if (all == NULL)
		return NULL;
	events = venture_money_calendar_filter(all, kind, company_id, venture_id, currency);
	day_nets = venture_money_calendar_nets(events, FALSE);
	week_nets = venture_money_calendar_nets(events, TRUE);

	result = venture_report_result_new("Money calendar", range);
	venture_report_result_add_column(result, "date", "Date", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "week", "ISO week", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "kind", "Kind", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "title", "Event", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "counterparty", "Vendor / customer", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "direction", "Direction", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "amount", "Amount", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "record", "Record", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "day_net", "Day net", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "week_net", "Week net", VENTURE_REPORT_COLUMN_TEXT);

	/* Totals over the span, one metric per currency, in and out and net. */
	totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	for (i = 0; i < events->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(events, i);
		g_autofree gchar *record = g_strdup_printf("%s:%" G_GINT64_FORMAT,
			venture_money_calendar_event_get_record_type(event), venture_money_calendar_event_get_record_id(event));
		g_autofree gchar *day_net = venture_money_calendar_nets_format(day_nets, venture_money_calendar_event_get_day(event));
		g_autofree gchar *week_net = venture_money_calendar_nets_format(week_nets, venture_money_calendar_event_get_week(event));
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "date", venture_money_calendar_event_get_day(event));
		venture_report_result_set_text(result, "week", venture_money_calendar_event_get_week(event));
		venture_report_result_set_text(result, "kind", venture_money_calendar_event_get_kind(event));
		venture_report_result_set_text(result, "title", venture_money_calendar_event_get_title(event));
		venture_report_result_set_text(result, "counterparty", venture_money_calendar_event_get_counterparty(event));
		venture_report_result_set_text(result, "direction", venture_money_calendar_event_get_direction(event));
		venture_report_result_set_money(result, "amount", venture_money_calendar_event_get_amount(event));
		venture_report_result_set_text(result, "currency", venture_money_calendar_event_get_currency(event));
		venture_report_result_set_text(result, "status", venture_money_calendar_event_get_overdue(event) ? "overdue"
			: venture_money_calendar_event_get_counts_in_net(event) ? "due" : "reminder");
		venture_report_result_set_text(result, "record", record);
		venture_report_result_set_text(result, "day_net", day_net);
		venture_report_result_set_text(result, "week_net", week_net);
		if (venture_money_calendar_event_get_counts_in_net(event))
		{
			const gchar *cur = venture_money_calendar_event_get_currency(event);
			g_autofree gchar *key = g_strdup_printf("%s|%s", venture_money_calendar_event_get_direction(event), cur);
			g_autofree gchar *net_key = g_strdup_printf("net|%s", cur);
			g_autoptr(VentureMoney) signed_amount = venture_money_calendar_event_signed_amount(event);
			VentureMoney *running = g_hash_table_lookup(totals, key);
			VentureMoney *net = g_hash_table_lookup(totals, net_key);
			g_hash_table_insert(totals, g_strdup(key), running == NULL
				? venture_money_copy(venture_money_calendar_event_get_amount(event))
				: venture_money_add(running, venture_money_calendar_event_get_amount(event), NULL));
			g_hash_table_insert(totals, g_strdup(net_key), net == NULL
				? venture_money_copy(signed_amount) : venture_money_add(net, signed_amount, NULL));
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_count("events", "Events", events->len));
	{
		g_autoptr(GList) keys = g_hash_table_get_keys(totals);
		GList *l;
		keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
		for (l = keys; l != NULL; l = l->next)
		{
			const gchar *key = l->data;
			const gchar *bar = strchr(key, '|');
			g_autofree gchar *metric_key = g_strdup_printf("%.*s_%s", (int)(bar - key), key, bar + 1);
			g_autofree gchar *label = g_strdup_printf("%s %s", g_str_has_prefix(key, "in|") ? "Money in"
				: g_str_has_prefix(key, "out|") ? "Money out" : "Net", bar + 1);
			VentureMetric *metric = venture_metric_new_money(metric_key, label, g_hash_table_lookup(totals, key));
			if (g_str_has_prefix(key, "out|"))
				venture_metric_set_higher_is_better(metric, FALSE);
			venture_report_result_add_metric(result, metric);
		}
	}
	venture_report_result_set_note(result, "Nets are per currency and never converted; reminders show the invoice balance but do not move cash. "
		"Overdue bills and invoices are carried to today and flagged.");
	return g_steal_pointer(&result);
}

void
venture_money_calendar_register_reports(VentureReportRegistry *registry)
{
	g_return_if_fail(VENTURE_IS_REPORT_REGISTRY(registry));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"money_calendar", "Money calendar",
		"Every dated money event in the period: recurring schedules expanded forward, "
		"bills and invoices on their due dates (overdue carried to today), dunning steps, "
		"payroll runs and tax packs, with per-day and per-ISO-week nets per currency. "
		"Options: from, to, as_of, kind, customer_id or vendor_id, venture_id, currency",
		venture_report_money_calendar)));
}
