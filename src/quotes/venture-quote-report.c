/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static VentureReportResult *
quote_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	static const gchar *const names[] = { "sent", "accepted", "declined", "expired" };
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_QUOTE);
	g_autoptr(GPtrArray) quotes = NULL;
	g_autoptr(GPtrArray) totals = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Quotes", period);
	const gchar *currency = options != NULL ? venture_json_object_get_string(options, "currency", "USD") : "USD";
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	gint64 counts[4] = { 0, 0, 0, 0 };
	gint64 duration = 0;
	gint64 accepted = 0;
	gint64 denominator = 0;
	guint i;
	if (org == 0) org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (period != NULL)
	{
		/* All-time and one-sided ranges deliberately have absent bounds. */
		GDateTime *start = venture_date_range_get_start(period);
		GDateTime *end = venture_date_range_get_end(period);
		if (start != NULL)
		{
			g_autofree gchar *text = g_date_time_format_iso8601(start);
			if (!venture_query_add_filter_string(query, "issued-at", VENTURE_FILTER_OP_GTE, text, error)) return NULL;
		}
		if (end != NULL)
		{
			g_autofree gchar *text = g_date_time_format_iso8601(end);
			if (!venture_query_add_filter_string(query, "issued-at", VENTURE_FILTER_OP_LT, text, error)) return NULL;
		}
	}
	quotes = venture_database_find(venture_context_get_database(context), query, error);
	if (quotes == NULL) return NULL;
	for (i = 0; i < 4; i++) g_ptr_array_add(totals, venture_money_new_zero(currency));
	for (i = 0; i < quotes->len; i++)
	{
		VentureEntity *q = g_ptr_array_index(quotes, i);
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) issued = NULL;
		g_autoptr(VentureQuery) eq = venture_query_new(VENTURE_TYPE_QUOTE_EVENT);
		g_autoptr(GPtrArray) events = NULL;
		gint status;
		guint bucket;
		guint j;
		VentureMoney *next;
		g_object_get(q, "status", &status, "total", &amount, "issued-at", &issued, NULL);
		if (status < VENTURE_QUOTE_SENT || status > VENTURE_QUOTE_EXPIRED || issued == NULL || amount == NULL) continue;
		if (g_strcmp0(venture_money_get_currency(amount), currency) != 0) continue;
		bucket = (guint)(status - VENTURE_QUOTE_SENT);
		next = venture_money_add(g_ptr_array_index(totals, bucket), amount, error);
		if (next == NULL) return NULL;
		venture_money_free(g_ptr_array_index(totals, bucket));
		g_ptr_array_index(totals, bucket) = next;
		counts[bucket]++;
		denominator++;
		if (status != VENTURE_QUOTE_ACCEPTED) continue;
		venture_query_set_organization(eq, org);
		venture_query_set_limit(eq, 0);
		venture_query_add_filter_int(eq, "quote-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(q), NULL);
		events = venture_database_find(venture_context_get_database(context), eq, error);
		if (events == NULL) return NULL;
		for (j = 0; j < events->len; j++)
		{
			g_autoptr(GDateTime) at = NULL;
			g_object_get(g_ptr_array_index(events, j), "accepted-at", &at, NULL);
			if (at == NULL) continue;
			duration += g_date_time_difference(at, issued) / G_TIME_SPAN_SECOND;
			accepted++;
		}
	}
	for (i = 0; i < 4; i++)
	{
		g_autofree gchar *key = g_strconcat(names[i], "_count", NULL);
		g_autofree gchar *value_key = g_strconcat(names[i], "_value", NULL);
		venture_report_result_add_metric(result, venture_metric_new_count(key, names[i], counts[i]));
		venture_report_result_add_metric(result, venture_metric_new_money(value_key, names[i], g_ptr_array_index(totals, i)));
	}
	venture_report_result_add_metric(result, venture_metric_new_ratio("acceptance_rate", "Acceptance rate",
		denominator > 0 ? (gdouble)counts[1] / denominator : 0));
	venture_report_result_add_metric(result, venture_metric_new_number("average_days_to_accept", "Average days to accept",
		accepted > 0 ? (gdouble)duration / accepted / 86400 : 0));
	venture_report_result_set_note(result, "Current outcomes for quotes issued in the selected period and currency; drafts, superseded revisions and other currencies are excluded.");
	return g_steal_pointer(&result);
}

void
venture_quotes_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("quotes", "Quotes",
		"Proposal outcomes, value and acceptance speed for the issued cohort, in one organization and currency.", quote_report)));
}
