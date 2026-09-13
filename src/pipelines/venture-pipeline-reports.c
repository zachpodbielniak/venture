/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

typedef struct
{
	gchar *pipeline;
	gchar *label;
	gchar *owner;
	gint64 count;
	gint64 converted;
	GArray *days;
	VentureMoney *open;
	VentureMoney *committed;
	VentureMoney *won;
} Group;
static void
free_group(gpointer data)
{
	Group *group = data;
	g_free(group->pipeline);
	g_free(group->label);
	g_free(group->owner);
	g_clear_pointer(&group->days, g_array_unref);
	g_clear_pointer(&group->open, venture_money_free);
	g_clear_pointer(&group->committed, venture_money_free);
	g_clear_pointer(&group->won, venture_money_free);
	g_free(group);
}
static gint64
number(GObject *object, const gchar *field)
{
	gint64 value;
	g_object_get(object, field, &value, NULL);
	return value;
}
static gboolean
within(GDateTime *date, VentureDateRange *period)
{
	return NULL != date && (NULL == venture_date_range_get_start(period) || g_date_time_compare(date, venture_date_range_get_start(period)) >= 0) &&
		(NULL == venture_date_range_get_end(period) || g_date_time_compare(date, venture_date_range_get_end(period)) < 0);
}
static gboolean
add_money(VentureMoney **total, const VentureMoney *value, GError **error)
{
	VentureMoney *sum;
	if (NULL == value)
		return TRUE;
	if (NULL == *total)
	{
		*total = venture_money_copy(value);
		return TRUE;
	}
	sum = venture_money_add(*total, value, error);
	if (NULL == sum)
		return FALSE;
	venture_money_free(*total);
	*total = sum;
	return TRUE;
}
static gint
compare_days(gconstpointer a, gconstpointer b)
{
	gdouble aa = *(const gdouble *)a, bb = *(const gdouble *)b;
	return aa < bb ? -1 : (aa > bb ? 1 : 0);
}
static GPtrArray *
fetch(VentureDatabase *db, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	return venture_database_find(db, query, error);
}
static VentureEntity *
lookup(GPtrArray *array, gint64 id)
{
	guint i;
	for (i = 0; i < array->len; i++)
	{
		VentureEntity *entity = g_ptr_array_index(array, i);
		if (venture_entity_get_id(entity) == id)
			return entity;
	}
	return NULL;
}
static Group *
group_for(GHashTable *groups, const gchar *key, VentureEntity *pipeline,
	const gchar *label, const gchar *owner)
{
	Group *group = g_hash_table_lookup(groups, key);
	if (NULL == group)
	{
		group = g_new0(Group, 1);
		g_object_get(pipeline, "name", &group->pipeline, NULL);
		group->label = g_strdup(label);
		group->owner = g_strdup(owner);
		group->days = g_array_new(FALSE, FALSE, sizeof(gdouble));
		g_hash_table_insert(groups, g_strdup(key), group);
	}
	return group;
}

/* All money remains in currency-specific rows; no portfolio currency is invented. */
static VentureReportResult *
generate(VentureContext *context, VentureDateRange *period, JsonObject *options,
	guint mode, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	gint64 org = venture_context_get_default_organization_id(context);
	gint64 pipeline_filter = 0, overdue = 0;
	const gchar *owner_filter = NULL;
	g_autoptr(GPtrArray) deals = NULL;
	g_autoptr(GPtrArray) stages = NULL;
	g_autoptr(GPtrArray) pipelines = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GPtrArray) reasons = NULL;
	g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_group);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Sales pipeline analysis", period);
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	GDateTime *end = venture_date_range_get_end(period);
	guint i;
	GHashTableIter iter;
	gpointer group_value;

	if (NULL != options)
	{
		if (json_object_has_member(options, "organization_id"))
			org = venture_json_object_get_int(options, "organization_id", org);
		if (json_object_has_member(options, "pipeline_id"))
			pipeline_filter = venture_json_object_get_int(options, "pipeline_id", 0);
		if (json_object_has_member(options, "owner"))
			owner_filter = json_object_get_string_member(options, "owner");
	}
	deals = fetch(db, VENTURE_TYPE_DEAL, org, error);
	stages = fetch(db, VENTURE_TYPE_PIPELINE_STAGE, org, error);
	pipelines = fetch(db, VENTURE_TYPE_PIPELINE, org, error);
	entries = fetch(db, VENTURE_TYPE_DEAL_STAGE_ENTRY, org, error);
	reasons = fetch(db, VENTURE_TYPE_LOSS_REASON, org, error);
	if (NULL == deals || NULL == stages || NULL == pipelines || NULL == entries || NULL == reasons)
		return NULL;
	if (NULL == end || g_date_time_compare(end, now) > 0)
		end = now;
	for (i = 0; i < deals->len; i++)
	{
		VentureEntity *deal = g_ptr_array_index(deals, i);
		gint64 pipeline_id = number(G_OBJECT(deal), "pipeline-id");
		VentureEntity *pipeline = lookup(pipelines, pipeline_id);
		VentureEntity *stage = lookup(stages, number(G_OBJECT(deal), "stage-id"));
		g_autofree gchar *owner = NULL;
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(GDateTime) closed = NULL;
		g_autoptr(GDateTime) latest = NULL;
		gint kind;
		guint j;
		if (NULL == pipeline || NULL == stage || (0 != pipeline_filter && pipeline_filter != pipeline_id))
			continue;
		g_object_get(deal, "owner", &owner, "value", &value, "closed-at", &closed, NULL);
		if (NULL != owner_filter && 0 != g_strcmp0(owner_filter, owner))
			continue;
		g_object_get(stage, "kind", &kind, NULL);
		for (j = 0; j < entries->len; j++)
		{
			VentureEntity *entry = g_ptr_array_index(entries, j);
			g_autoptr(GDateTime) entered = NULL;
			if (number(G_OBJECT(entry), "deal-id") != venture_entity_get_id(deal))
				continue;
			g_object_get(entry, "entered-at", &entered, NULL);
			if (NULL != entered && (NULL == latest || g_date_time_compare(entered, latest) > 0))
			{
				g_clear_pointer(&latest, g_date_time_unref);
				latest = g_date_time_ref(entered);
			}
			if (mode < 2 && NULL != entered && g_date_time_compare(entered, end) < 0)
			{
				VentureEntity *visited = lookup(stages, number(G_OBJECT(entry), "to-stage"));
				g_autoptr(GDateTime) exited = NULL;
				g_autofree gchar *key = NULL;
				g_autofree gchar *label = NULL;
				GDateTime *start;
				gdouble days;
				guint k;
				Group *group;
				gboolean converted = FALSE;
				if (NULL == visited)
					continue;
				for (k = 0; k < entries->len; k++)
				{
					VentureEntity *next = g_ptr_array_index(entries, k);
					g_autoptr(GDateTime) date = NULL;
					VentureEntity *next_stage;
					gint next_kind;
					if (number(G_OBJECT(next), "deal-id") != venture_entity_get_id(deal) ||
						venture_entity_get_id(next) <= venture_entity_get_id(entry))
						continue;
					g_object_get(next, "entered-at", &date, NULL);
					if (NULL == date || g_date_time_compare(date, entered) < 0 || g_date_time_compare(date, end) >= 0)
						continue;
					if (NULL != exited && g_date_time_compare(date, exited) >= 0)
						continue;
					g_clear_pointer(&exited, g_date_time_unref);
					exited = g_date_time_ref(date);
					next_stage = lookup(stages, number(G_OBJECT(next), "to-stage"));
					if (NULL != next_stage)
					{
						g_object_get(next_stage, "kind", &next_kind, NULL);
						converted = 1 == next_kind || (0 == next_kind && number(G_OBJECT(next_stage), "position") > number(G_OBJECT(visited), "position"));
					}
				}
				start = venture_date_range_get_start(period);
				if (NULL == start)
					start = entered;
				if (NULL != exited && g_date_time_compare(exited, start) <= 0)
					continue;
				if (g_date_time_compare(entered, start) > 0)
					start = entered;
				days = (gdouble)g_date_time_difference(NULL != exited ? exited : end, start) / G_TIME_SPAN_DAY;
				key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, pipeline_id, venture_entity_get_id(visited));
				g_object_get(visited, "name", &label, NULL);
				group = group_for(groups, key, pipeline, label, "");
				g_array_append_val(group->days, days);
				if (within(entered, period))
				{
					group->count++;
					if (converted)
						group->converted++;
				}
			}
		}
		if (0 == kind && NULL != latest && number(G_OBJECT(stage), "rotting-days") > 0 &&
			(gdouble)g_date_time_difference(now, latest) / G_TIME_SPAN_DAY > number(G_OBJECT(stage), "rotting-days"))
			overdue++;
		if (mode == 2 || mode == 3)
		{
			g_autofree gchar *key = NULL;
			g_autofree gchar *label = NULL;
			const gchar *currency = NULL != value ? venture_money_get_currency(value) : "USD";
			Group *group;
			if (mode == 3)
			{
				VentureEntity *reason;
				if (2 != kind || !within(closed, period))
					continue;
				reason = lookup(reasons, number(G_OBJECT(deal), "loss-reason-id"));
				if (NULL != reason)
					g_object_get(reason, "name", &label, NULL);
				else
					label = g_strdup("Unrecorded");
				key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", pipeline_id, number(G_OBJECT(deal), "loss-reason-id"), currency);
			}
			else
			{
				label = g_strdup(currency);
				key = g_strdup_printf("%" G_GINT64_FORMAT ":%s:%s", pipeline_id, NULL != owner ? owner : "", currency);
			}
			group = group_for(groups, key, pipeline, label, mode == 2 ? owner : "");
			if (mode == 3)
			{
				group->count++;
				if (!add_money(&group->won, value, error))
					return NULL;
			}
			else if (0 == kind)
			{
				g_autoptr(VentureMoney) weighted = venture_deal_get_weighted_value(VENTURE_DEAL(deal), error);
				gboolean committed;
				g_object_get(deal, "committed", &committed, NULL);
				if (NULL == weighted || !add_money(&group->open, weighted, error))
					return NULL;
				if ((committed || number(G_OBJECT(deal), "probability") >= 90) && !add_money(&group->committed, value, error))
					return NULL;
			}
			else if (1 == kind && within(closed, period) && !add_money(&group->won, value, error))
				return NULL;
		}
	}
	venture_report_result_add_metric(result, venture_metric_new_number("overdue_deals", "Overdue deals", overdue));
	venture_report_result_add_column(result, "pipeline", "Pipeline", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "label", mode < 2 ? "Stage" : (mode == 2 ? "Currency" : "Loss reason"), VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "owner", "Owner", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "entered", "Entered", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "converted", "Converted", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "average_days", "Average days", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "median_days", "Median days", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "open_weighted", "Open weighted", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "committed", "Committed", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "closed_value", mode == 3 ? "Lost value" : "Closed won", VENTURE_REPORT_COLUMN_MONEY);
	g_hash_table_iter_init(&iter, groups);
	while (g_hash_table_iter_next(&iter, NULL, &group_value))
	{
		Group *group = group_value;
		gdouble total = 0, median = 0;
		guint j, n = group->days->len;
		g_array_sort(group->days, compare_days);
		for (j = 0; j < n; j++)
			total += g_array_index(group->days, gdouble, j);
		if (n > 0)
			median = (g_array_index(group->days, gdouble, (n - 1) / 2) + g_array_index(group->days, gdouble, n / 2)) / 2;
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "pipeline", group->pipeline);
		venture_report_result_set_text(result, "label", group->label);
		venture_report_result_set_text(result, "owner", NULL != group->owner ? group->owner : "");
		venture_report_result_set_number(result, "entered", group->count);
		venture_report_result_set_number(result, "converted", group->converted);
		venture_report_result_set_number(result, "average_days", n > 0 ? total / n : 0);
		venture_report_result_set_number(result, "median_days", median);
		if (NULL != group->open)
			venture_report_result_set_money(result, "open_weighted", group->open);
		if (NULL != group->committed)
			venture_report_result_set_money(result, "committed", group->committed);
		if (NULL != group->won)
			venture_report_result_set_money(result, "closed_value", group->won);
	}
	return g_steal_pointer(&result);
}
#define REPORT(name, mode) \
static VentureReportResult *name(VentureContext *c, VentureDateRange *p, JsonObject *o, GError **e) \
{ return generate(c, p, o, mode, e); }
REPORT(stage_duration, 0)
REPORT(funnel, 1)
REPORT(forecast, 2)
REPORT(loss_reasons, 3)
REPORT(overdue_deals, 4)
#undef REPORT
void
venture_pipeline_reports_register(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("stage_duration", "Stage duration", "Stage occupancy clipped to the period", stage_duration)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("funnel", "Stage funnel", "Stage visits entered and advanced in the period", funnel)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("forecast", "Forecast", "Current open weighted and committed; won in the period", forecast)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("loss_reasons", "Loss reasons", "Count and value lost in the period", loss_reasons)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("overdue_deals", "Overdue deals", "Open deals beyond their stage's rotting threshold", overdue_deals)));
}
