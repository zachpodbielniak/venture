/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

GDateTime *
venture_period_report_as_of(JsonObject *options, GError **error)
{
	JsonNode *node;
	const gchar *text;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autofree gchar *timestamp = NULL;
	if ((NULL == options) || !json_object_has_member(options, "as_of"))
		return NULL;
	node = json_object_get_member(options, "as_of");
	if (!JSON_NODE_HOLDS_VALUE(node) || (G_TYPE_STRING != json_node_get_value_type(node)))
	{
		venture_set_error_validation(error, "as_of", "Expected an ISO date or timestamp");
		return NULL;
	}
	text = json_node_get_string(node);
	/* Report cutoffs are persisted evidence. The permissive import parser
	 * also accepts relative words and trailing text, which is unsuitable
	 * for a cutoff that must mean the same instant on every rerun. */
	if (10 == strlen(text))
		timestamp = g_strconcat(text, "T00:00:00Z", NULL);
	date = g_date_time_new_from_iso8601((NULL != timestamp) ? timestamp : text, utc);
	if (NULL == date)
	{
		venture_set_error_validation(error, "as_of", "Expected a valid ISO date or timestamp");
		return NULL;
	}
	if (10 == strlen(text))
	{
		g_autoptr(GDateTime) next = g_date_time_add_days(date, 1);
		return g_date_time_add(next, -1);
	}
	return g_steal_pointer(&date);
}

gboolean
venture_period_report_scope(VentureQuery *query, JsonObject *options, GError **error)
{
	g_autoptr(GDateTime) as_of = NULL;
	if ((NULL == options) || !json_object_has_member(options, "as_of"))
		return TRUE;
	as_of = venture_period_report_as_of(options, error);
	if (NULL == as_of)
		return FALSE;
	venture_query_set_as_of(query, as_of);
	return TRUE;
}

gboolean
venture_period_report_invoice_outstanding(VentureDatabase *database, VentureEntity *invoice,
	GDateTime *as_of, gboolean *outstanding, GError **error)
{
	g_autoptr(GDateTime) issued = NULL;
	g_autoptr(GDateTime) paid = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autofree gchar *cutoff = NULL;
	gint status;
	guint i;

	*outstanding = FALSE;
	g_object_get(invoice, "status", &status, "issued-at", &issued, "paid-at", &paid, NULL);
	if (NULL == as_of)
	{
		*outstanding = VENTURE_INVOICE_STATUS_SENT == status;
		return TRUE;
	}
	if ((NULL == issued) || (g_date_time_compare(issued, as_of) > 0) ||
		((NULL != paid) && (g_date_time_compare(paid, as_of) <= 0)))
		return TRUE;
	/* paid-at also covers imported invoices predating the audit trail. */
	if ((VENTURE_INVOICE_STATUS_PAID == status) && (NULL != paid))
		status = VENTURE_INVOICE_STATUS_SENT;
	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ, "invoice", NULL);
	venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
	cutoff = venture_time_to_string(as_of);
	venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_GT, cutoff, NULL);
	venture_query_add_order(query, "occurred-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	events = venture_database_find(database, query, error);
	if (NULL == events)
		return FALSE;
	for (i = 0; i < events->len; i++)
	{
		g_autofree gchar *text = NULL;
		g_autoptr(JsonNode) diff = NULL;
		JsonObject *object;
		JsonNode *change;
		JsonNode *from;
		g_object_get(g_ptr_array_index(events, i), "diff", &text, NULL);
		if (venture_string_is_empty(text))
			continue;
		diff = venture_json_parse(text, error);
		if (NULL == diff)
			return FALSE;
		object = json_node_get_object(diff);
		change = json_object_get_member(object, "status");
		if ((NULL == change) || !JSON_NODE_HOLDS_OBJECT(change))
			continue;
		from = json_object_get_member(json_node_get_object(change), "from");
		if ((NULL != from) && JSON_NODE_HOLDS_VALUE(from) &&
			(G_TYPE_STRING == json_node_get_value_type(from)))
			venture_enum_from_nick(VENTURE_TYPE_INVOICE_STATUS, json_node_get_string(from), &status);
	}
	*outstanding = VENTURE_INVOICE_STATUS_SENT == status;
	return TRUE;
}

static VentureDateRange *
period_range(VentureEntity *period)
{
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_object_get(period, "start-at", &start, "end-at", &end, NULL);
	return venture_date_range_new(start, end);
}

GPtrArray *
venture_period_report_snapshots(VentureContext *context, VentureEntity *period, GError **error)
{
	g_autoptr(GPtrArray) reports = NULL;
	g_autoptr(GPtrArray) snapshots = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureDateRange) range = period_range(period);
	g_autoptr(GDateTime) closed = NULL;
	g_autoptr(GDateTime) period_cutoff = NULL;
	g_autofree gchar *closer = NULL;
	g_autofree gchar *date = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	GDateTime *as_of;
	guint i;
	g_object_get(period, "closed-at", &closed, "closed-by", &closer, NULL);
	/* Period ends are exclusive; an invoice issued at the next midnight
	 * must not enter the previous period's receivables snapshot. */
	period_cutoff = g_date_time_add(venture_date_range_get_end(range), -1);
	as_of = period_cutoff;
	if (g_date_time_compare(as_of, closed) > 0)
		as_of = closed;
	date = venture_time_to_string(as_of);
	json_object_set_string_member(options, "as_of", date);
	json_object_set_int_member(options, "organization_id", venture_entity_get_organization_id(period));
	reports = venture_report_registry_list(venture_context_get_report_registry(context));
	for (i = 0; i < reports->len; i++)
	{
		VentureReport *report = g_ptr_array_index(reports, i);
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(JsonNode) node = NULL;
		g_autofree gchar *totals = NULL;
		gboolean financial;
		g_object_get(report, "financial", &financial, NULL);
		if (!financial)
			continue;
		result = venture_report_generate(report, context, range, options, error);
		if (NULL == result)
			return NULL;
		node = venture_report_result_to_json(result);
		totals = venture_json_to_string(node, FALSE);
		g_ptr_array_add(snapshots, g_object_new(VENTURE_TYPE_REPORT_SNAPSHOT,
			"organization-id", venture_entity_get_organization_id(period),
			"fiscal-period-id", venture_entity_get_id(period), "as-of", as_of,
			"closed-by", closer, "report", venture_report_get_name(report), "totals", totals, NULL));
	}
	return g_steal_pointer(&snapshots);
}

static VentureReportResult *
snapshot_vs_live(VentureContext *context, VentureDateRange *range, JsonObject *options, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_REPORT_SNAPSHOT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Snapshot vs live", range);
	VentureDatabase *database = venture_context_get_database(context);
	gint64 organization_id = (NULL != options) ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i;
	if (0 == organization_id)
		organization_id = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, organization_id);
	rows = venture_database_find(database, query, error);
	if (NULL == rows)
		return NULL;
	venture_report_result_add_column(result, "period", "Period", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "report", "Report", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "snapshot", "Snapshot totals", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "live", "Live totals", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *snapshot = g_ptr_array_index(rows, i);
		g_autoptr(VentureEntity) period = NULL;
		g_autoptr(VentureDateRange) period_dates = NULL;
		g_autoptr(VentureReportResult) live = NULL;
		g_autoptr(JsonObject) live_options = json_object_new();
		g_autoptr(JsonNode) saved = NULL;
		g_autoptr(JsonNode) current = NULL;
		g_autofree gchar *report_name = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *totals = NULL;
		g_autofree gchar *live_totals = NULL;
		VentureReport *report;
		gint64 period_id;
		g_object_get(snapshot, "fiscal-period-id", &period_id, "report", &report_name, "totals", &totals, NULL);
		period = venture_database_get(database, VENTURE_TYPE_FISCAL_PERIOD, period_id, error);
		if (NULL == period)
			return NULL;
		period_dates = period_range(period);
		if ((NULL != range) &&
			((g_date_time_compare(venture_date_range_get_start(period_dates), venture_date_range_get_end(range)) >= 0) ||
			 (g_date_time_compare(venture_date_range_get_end(period_dates), venture_date_range_get_start(range)) <= 0)))
			continue;
		report = venture_report_registry_lookup(venture_context_get_report_registry(context), report_name);
		if (NULL == report)
		{
			venture_report_result_append_note(result, "A snapshotted report is currently disabled or unavailable.");
			continue;
		}
		json_object_set_int_member(live_options, "organization_id", venture_entity_get_organization_id(snapshot));
		live = venture_report_generate(report, context, period_dates, live_options, error);
		if (NULL == live)
			return NULL;
		saved = venture_json_parse(totals, error);
		if (NULL == saved)
			return NULL;
		current = venture_report_result_to_json(live);
		if (json_node_equal(saved, current))
			continue;
		g_object_get(period, "name", &name, NULL);
		live_totals = venture_json_to_string(current, FALSE);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "period", name);
		venture_report_result_set_text(result, "report", report_name);
		venture_report_result_set_text(result, "snapshot", totals);
		venture_report_result_set_text(result, "live", live_totals);
	}
	return g_steal_pointer(&result);
}

void
venture_period_reports_register(VentureReportRegistry *registry)
{
	static const gchar *const financial[] = {
		"pnl", "ventures", "categories", "inventory", "tax", "campaigns", "pipeline", "monthly", "receivables", "trial_balance"
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(financial); i++)
	{
		VentureReport *report = venture_report_registry_lookup(registry, financial[i]);
		if (NULL != report)
			g_object_set(report, "financial", TRUE, NULL);
	}
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "snapshot_vs_live",
		"Snapshot vs live", "Compare the totals preserved at period close with current reports", snapshot_vs_live)));
}
