/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
static VentureReportResult *
unposted(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Unposted documents", period);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i;
	if (org == 0) org = venture_context_get_default_organization_id(context);
	sources = venture_autojournal_service_unposted(venture_database_get_autojournal_service(db), org, error);
	if (sources == NULL) return NULL;
	venture_report_result_add_column(result, "type", "Type", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "id", "ID", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "version", "Version", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "date", "Date", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < sources->len; i++) {
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(source));
		g_autofree gchar *version = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_version(source));
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *date = NULL;
		g_object_get(source, "occurred-at", &when, NULL);
		if (when == NULL) when = g_date_time_ref(venture_entity_get_created_at(source));
		if (period != NULL && (g_date_time_compare(when, venture_date_range_get_start(period)) < 0 ||
			g_date_time_compare(when, venture_date_range_get_end(period)) >= 0)) continue;
		date = g_date_time_format_iso8601(when);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "type", venture_entity_get_entity_name(source));
		venture_report_result_set_text(result, "id", id);
		venture_report_result_set_text(result, "version", version);
		venture_report_result_set_text(result, "date", date);
	}
	return g_steal_pointer(&result);
}
void
venture_autojournal_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("unposted", "Unposted documents",
		"Sales and expenses whose latest version has no posted document journal", unposted)));
}
