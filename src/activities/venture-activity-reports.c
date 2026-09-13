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

void
venture_activity_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("worklist", "Worklist",
		"Current UTC week, per owner: overdue, today, due this week and completed this week", worklist)));
}
