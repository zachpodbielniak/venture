/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static gint64
report_org(VentureContext *context, JsonObject *options)
{
	return options != NULL && json_object_has_member(options, "organization_id") ?
		json_object_get_int_member(options, "organization_id") : venture_context_get_default_organization_id(context);
}

static GPtrArray *
report_rows(VentureContext *context, GType type, gint64 org, const gchar *date,
	VentureDateRange *period, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, org, error))
		return NULL;
	if (date != NULL && period != NULL && !venture_query_set_date_range(query, date, period, error))
		return NULL;
	return venture_database_find(venture_context_get_database(context), query, error);
}

static VentureReportResult *
performance(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	gint64 org = report_org(context, options);
	g_autoptr(GPtrArray) sequences = report_rows(context, VENTURE_TYPE_SEQUENCE, org, NULL, NULL, error);
	g_autoptr(GPtrArray) enrollments = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Sequence performance", period);
	guint i, j;
	if (sequences == NULL)
		return NULL;
	enrollments = report_rows(context, VENTURE_TYPE_SEQUENCE_ENROLLMENT, org, "enrolled-at", period, error);
	if (enrollments == NULL)
		return NULL;
	venture_report_result_add_column(result, "sequence", "Sequence", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "enrolled", "Enrolled", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "completed", "Completed", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "exited", "Exited", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "replies", "Replies", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "goal_met", "Goal met", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "exit_reasons", "Exited by reason", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < sequences->len; i++)
	{
		VentureEntity *sequence = g_ptr_array_index(sequences, i);
		g_autofree gchar *name = NULL;
		g_autoptr(JsonNode) reasons = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *reason_text = NULL;
		guint enrolled = 0, completed = 0, exited = 0, replies = 0, goals = 0;
		json_node_take_object(reasons, json_object_new());
		g_object_get(sequence, "name", &name, NULL);
		for (j = 0; j < enrollments->len; j++)
		{
			VentureEntity *row = g_ptr_array_index(enrollments, j);
			gint64 sequence_id;
			gint status;
			g_autofree gchar *reason = NULL;
			g_autoptr(GDateTime) goal = NULL;
			g_object_get(row, "sequence-id", &sequence_id, "status", &status,
				"exit-reason", &reason, "goal-met-at", &goal, NULL);
			if (sequence_id != venture_entity_get_id(sequence))
				continue;
			enrolled++;
			completed += status == 2;
			exited += status == 3;
			replies += g_strcmp0(reason, "reply") == 0;
			goals += goal != NULL;
			if (status == 3)
			{
				JsonObject *map = json_node_get_object(reasons);
				const gchar *key = venture_string_is_empty(reason) ? "unspecified" : reason;
				gint64 count = json_object_has_member(map, key) ? json_object_get_int_member(map, key) : 0;
				json_object_set_int_member(map, key, count + 1);
			}
		}
		reason_text = venture_json_to_string(reasons, FALSE);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "sequence", name);
		venture_report_result_set_number(result, "enrolled", enrolled);
		venture_report_result_set_number(result, "completed", completed);
		venture_report_result_set_number(result, "exited", exited);
		venture_report_result_set_number(result, "replies", replies);
		venture_report_result_set_number(result, "goal_met", goals);
		venture_report_result_set_text(result, "exit_reasons", reason_text);
	}
	return g_steal_pointer(&result);
}

static VentureReportResult *
failures(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GPtrArray) deliveries = report_rows(context, VENTURE_TYPE_SEQUENCE_DELIVERY,
		report_org(context, options), "scheduled-at", period, error);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Sequence failures", period);
	guint i;
	if (deliveries == NULL)
		return NULL;
	venture_report_result_add_column(result, "delivery", "Delivery", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "enrollment", "Enrollment", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "step", "Step", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "error", "Error", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < deliveries->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(deliveries, i);
		g_autofree gchar *message = NULL;
		gint state;
		gint64 enrollment, step;
		g_object_get(row, "state", &state, "error", &message, "enrollment-id", &enrollment, "step-id", &step, NULL);
		if (state != 2)
			continue;
		venture_report_result_begin_row(result);
		venture_report_result_set_number(result, "delivery", venture_entity_get_id(row));
		venture_report_result_set_number(result, "enrollment", enrollment);
		venture_report_result_set_number(result, "step", step);
		venture_report_result_set_text(result, "error", message);
	}
	return g_steal_pointer(&result);
}

void
venture_sequences_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("sequence_performance",
		"Sequence performance", "Current outcomes of enrollments started during the selected period", performance)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new("sequence_failures",
		"Sequence failures", "Failed deliveries scheduled during the selected period", failures)));
}
