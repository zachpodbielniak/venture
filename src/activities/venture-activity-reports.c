/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

typedef struct
{
	gint64 overdue, today, week, done;
} OwnerCounts;

static VentureReportResult *
worklist(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) day = g_date_time_new_utc(g_date_time_get_year(now), g_date_time_get_month(now), g_date_time_get_day_of_month(now), 0, 0, 0);
	g_autoptr(GDateTime) tomorrow = g_date_time_add_days(day, 1);
	g_autoptr(GDateTime) week = g_date_time_add_days(day, 1 - g_date_time_get_day_of_week(day));
	g_autoptr(GDateTime) next_week = g_date_time_add_days(week, 7);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Worklist", period);
	g_autoptr(GList) names = NULL;
	GList *item;
	gint64 overdue = 0, today = 0;
	guint i;
	gint64 organization = options ? venture_json_object_get_int(options, "organization_id", venture_context_get_default_organization_id(context)) : venture_context_get_default_organization_id(context);
	rows = venture_activity_service_list(venture_database_get_activity_service(venture_context_get_database(context)), organization, NULL, "all", now, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *owner = NULL;
		g_autoptr(GDateTime) due = NULL;
		g_autoptr(GDateTime) completed = NULL;
		gint status;
		OwnerCounts *counts;
		g_object_get(row, "owner", &owner, "due-at", &due, "completed-at", &completed, "status", &status, NULL);
		if (owner == NULL)
			owner = g_strdup("Unassigned");
		counts = g_hash_table_lookup(owners, owner);
		if (counts == NULL)
		{
			counts = g_new0(OwnerCounts, 1);
			g_hash_table_insert(owners, g_strdup(owner), counts);
		}
		if (status == VENTURE_ACTIVITY_STATUS_PLANNED && due != NULL)
		{
			if (g_date_time_compare(due, day) < 0) { counts->overdue++; overdue++; }
			if (g_date_time_compare(due, day) >= 0 && g_date_time_compare(due, tomorrow) < 0) { counts->today++; today++; }
			if (g_date_time_compare(due, week) >= 0 && g_date_time_compare(due, next_week) < 0) counts->week++;
		}
		if (status == VENTURE_ACTIVITY_STATUS_DONE && completed != NULL &&
			g_date_time_compare(completed, week) >= 0 && g_date_time_compare(completed, next_week) < 0)
			counts->done++;
	}
	venture_report_result_add_metric(result, venture_metric_new_count("activities_overdue", "Overdue activities", overdue));
	venture_report_result_add_metric(result, venture_metric_new_count("activities_today", "Activities today", today));
	venture_report_result_add_column(result, "owner", "Owner", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "overdue", "Overdue", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "today", "Due today", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "week", "Due this week", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "done", "Done this week", VENTURE_REPORT_COLUMN_NUMBER);
	names = g_list_sort(g_hash_table_get_keys(owners), (GCompareFunc)g_strcmp0);
	for (item = names; item != NULL; item = item->next)
	{
		OwnerCounts *counts = g_hash_table_lookup(owners, item->data);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "owner", item->data);
		venture_report_result_set_number(result, "overdue", counts->overdue);
		venture_report_result_set_number(result, "today", counts->today);
		venture_report_result_set_number(result, "week", counts->week);
		venture_report_result_set_number(result, "done", counts->done);
	}
	return g_steal_pointer(&result);
}

typedef struct
{
	gint64 calls, inbound, outbound, seconds;
	gint64 outcomes[5];
} CallCounts;

static VentureReportResult *
calls(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	static const gchar *const outcomes[] = { "unknown", "reached", "voicemail", "no_answer", "callback" };
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) owners = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Calls", period);
	g_autoptr(GList) names = NULL;
	GDateTime *start = period != NULL ? venture_date_range_get_start(period) : NULL;
	GDateTime *end = period != NULL ? venture_date_range_get_end(period) : NULL;
	GList *item;
	gint64 organization = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	gint64 total = 0;
	guint i;
	if (organization == 0) organization = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "kind", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_KIND_CALL, error) ||
		!venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_STATUS_DONE, error))
		return NULL;
	rows = venture_database_find(venture_context_get_database(context), query, error);
	if (rows == NULL) return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) occurred = NULL;
		g_autofree gchar *owner = NULL;
		gint direction, outcome;
		gint64 duration;
		CallCounts *counts;
		g_object_get(row, "call-occurred-at", &occurred, "owner", &owner,
			"call-direction", &direction, "call-outcome", &outcome, "call-duration", &duration, NULL);
		/* Historical completed calls predate actual occurrence metadata. */
		if (occurred == NULL) g_object_get(row, "completed-at", &occurred, NULL);
		if (occurred == NULL || (start != NULL && g_date_time_compare(occurred, start) < 0) ||
			(end != NULL && g_date_time_compare(occurred, end) >= 0)) continue;
		if (venture_string_is_empty(owner))
		{
			g_free(owner);
			owner = g_strdup("Unassigned");
		}
		counts = g_hash_table_lookup(owners, owner);
		if (counts == NULL)
		{
			counts = g_new0(CallCounts, 1);
			g_hash_table_insert(owners, g_strdup(owner), counts);
		}
		counts->calls++;
		counts->seconds += duration;
		if (direction == VENTURE_CALL_DIRECTION_INBOUND) counts->inbound++;
		else counts->outbound++;
		if (outcome >= 0 && outcome < (gint)G_N_ELEMENTS(outcomes)) counts->outcomes[outcome]++;
		total++;
	}
	venture_report_result_add_column(result, "owner", "Owner at logging", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "calls", "Calls", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "inbound", "Inbound", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "outbound", "Outbound", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "seconds", "Duration in seconds", VENTURE_REPORT_COLUMN_NUMBER);
	for (i = 0; i < G_N_ELEMENTS(outcomes); i++)
		venture_report_result_add_column(result, outcomes[i], outcomes[i], VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_metric(result, venture_metric_new_count("calls", "Calls", total));
	names = g_list_sort(g_hash_table_get_keys(owners), (GCompareFunc)g_strcmp0);
	for (item = names; item != NULL; item = item->next)
	{
		CallCounts *counts = g_hash_table_lookup(owners, item->data);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "owner", item->data);
		venture_report_result_set_number(result, "calls", counts->calls);
		venture_report_result_set_number(result, "inbound", counts->inbound);
		venture_report_result_set_number(result, "outbound", counts->outbound);
		venture_report_result_set_number(result, "seconds", counts->seconds);
		for (i = 0; i < G_N_ELEMENTS(outcomes); i++)
			venture_report_result_set_number(result, outcomes[i], counts->outcomes[i]);
	}
	return g_steal_pointer(&result);
}

void
venture_activity_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("worklist", "Worklist",
		"Current UTC week, per owner: overdue, today, due this week and completed this week", worklist)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("calls", "Calls",
		"One completed call per actual occurrence, grouped by owner; duration in seconds", calls)));
}
