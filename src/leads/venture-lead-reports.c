/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static GPtrArray *
leads(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LEAD);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (org == 0) org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	if (period != NULL)
	{
		/* All-time and one-sided ranges deliberately have absent bounds. */
		GDateTime *start = venture_date_range_get_start(period);
		GDateTime *end = venture_date_range_get_end(period);
		if (start != NULL)
		{
			g_autofree gchar *text = g_date_time_format_iso8601(start);
			if (!venture_query_add_filter_string(query, "created-at", VENTURE_FILTER_OP_GTE, text, error)) return NULL;
		}
		if (end != NULL)
		{
			g_autofree gchar *text = g_date_time_format_iso8601(end);
			if (!venture_query_add_filter_string(query, "created-at", VENTURE_FILTER_OP_LT, text, error)) return NULL;
		}
	}
	return venture_database_find(venture_context_get_database(context), query, error);
}

typedef struct { gchar *source; gint64 campaign; gint64 count; gint64 converted; } Source;

static void
source_free(gpointer data)
{
	Source *source = data;
	g_free(source->source);
	g_free(source);
}

static VentureReportResult *
source_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GPtrArray) found = leads(context, period, options, error);
	g_autoptr(GPtrArray) groups = g_ptr_array_new_with_free_func(source_free);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Lead sources", period);
	guint i;
	if (found == NULL) return NULL;
	venture_report_result_add_column(result, "source", "Source", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "campaign_id", "Campaign", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "count", "Leads", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "converted", "Converted", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "conversion_rate", "Conversion rate", VENTURE_REPORT_COLUMN_PERCENT);
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *lead = g_ptr_array_index(found, i);
		g_autofree gchar *name = NULL;
		gint64 campaign = 0;
		VentureLeadStatus state;
		Source *group = NULL;
		guint j;
		g_object_get(lead, "source", &name, "campaign-id", &campaign, "status", &state, NULL);
		for (j = 0; j < groups->len; j++)
		{
			Source *candidate = g_ptr_array_index(groups, j);
			if (candidate->campaign == campaign && g_strcmp0(candidate->source, name) == 0) { group = candidate; break; }
		}
		if (group == NULL)
		{
			group = g_new0(Source, 1);
			group->source = g_strdup(name);
			group->campaign = campaign;
			g_ptr_array_add(groups, group);
		}
		group->count++;
		if (state == VENTURE_LEAD_CONVERTED) group->converted++;
	}
	for (i = 0; i < groups->len; i++)
	{
		Source *group = g_ptr_array_index(groups, i);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "source", group->source);
		venture_report_result_set_number(result, "campaign_id", group->campaign);
		venture_report_result_set_number(result, "count", group->count);
		venture_report_result_set_number(result, "converted", group->converted);
		venture_report_result_set_number(result, "conversion_rate", 100.0 * group->converted / group->count);
	}
	venture_report_result_add_metric(result, venture_metric_new_count("leads", "Leads", found->len));
	return g_steal_pointer(&result);
}

static VentureReportResult *
response_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GPtrArray) found = leads(context, period, options, error);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Lead response time", period);
	guint i;
	gint64 responded = 0;
	gdouble seconds = 0;
	if (found == NULL) return NULL;
	venture_report_result_add_column(result, "lead_id", "Lead", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "seconds", "Seconds to first outbound", VENTURE_REPORT_COLUMN_NUMBER);
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *lead = g_ptr_array_index(found, i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTERACTION);
		g_autoptr(GPtrArray) events = NULL;
		g_autoptr(GDateTime) created = NULL;
		g_autoptr(GDateTime) first = NULL;
		guint j;
		g_object_get(lead, "created-at", &created, NULL);
		venture_query_set_organization(query, venture_entity_get_organization_id(lead));
		venture_query_set_limit(query, 0);
		venture_query_add_filter_int(query, "lead-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(lead), NULL);
		venture_query_add_filter_string(query, "outbound", VENTURE_FILTER_OP_EQ, "true", NULL);
		events = venture_database_find(venture_context_get_database(context), query, error);
		if (events == NULL) return NULL;
		for (j = 0; j < events->len; j++)
		{
			g_autoptr(GDateTime) date = NULL;
			g_object_get(g_ptr_array_index(events, j), "occurred-at", &date, NULL);
			if (date != NULL && g_date_time_compare(date, created) >= 0 &&
				(first == NULL || g_date_time_compare(date, first) < 0))
			{
				g_clear_pointer(&first, g_date_time_unref);
				first = g_date_time_ref(date);
			}
		}
		if (first != NULL)
		{
			gdouble delay = g_date_time_difference(first, created) / (gdouble)G_TIME_SPAN_SECOND;
			seconds += delay;
			responded++;
			venture_report_result_begin_row(result);
			venture_report_result_set_number(result, "lead_id", venture_entity_get_id(lead));
			venture_report_result_set_number(result, "seconds", delay);
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_count("responded", "Responded", responded));
	venture_report_result_add_metric(result, venture_metric_new_count("unanswered", "Unanswered", found->len - responded));
	venture_report_result_add_metric(result, venture_metric_new_number("average_seconds", "Average seconds", responded != 0 ? seconds / responded : 0));
	return g_steal_pointer(&result);
}

static VentureReportResult *
recycled_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GPtrArray) found = leads(context, NULL, options, error);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureReportResult) result = venture_report_result_new("Recycled leads due", period);
	gint64 due = 0;
	guint i;
	if (found == NULL) return NULL;
	for (i = 0; i < found->len; i++)
	{
		g_autoptr(GDateTime) until = NULL;
		VentureLeadStatus state;
		g_object_get(g_ptr_array_index(found, i), "status", &state, "recycle-until", &until, NULL);
		if (state == VENTURE_LEAD_RECYCLED && until != NULL && g_date_time_compare(until, now) <= 0) due++;
	}
	venture_report_result_add_metric(result, venture_metric_new_count("leads_recycled_due", "Recycled leads due", due));
	return g_steal_pointer(&result);
}

void
venture_leads_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("lead_sources", "Lead sources",
		"Conversion by source and campaign for leads created in the period", source_report)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("lead_response_time", "Lead response time",
		"Creation to the first outbound interaction; unanswered leads are counted separately", response_report)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("leads_recycled_due", "Recycled leads due",
		"Recycled leads whose return date has arrived", recycled_report)));
}
