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

/* Per-step engagement for deliveries scheduled in the period: a delivery
 * counts as sent when the adapter marked it sent, opened or clicked when
 * at least one event exists, and replied when it is the last delivery its
 * enrollment sent before exiting with a reply. Rates are whole percentages
 * of sent. */
static gint
by_position(gconstpointer a, gconstpointer b)
{
	gint64 left, right;
	g_object_get(*(VentureEntity *const *)a, "position", &left, NULL);
	g_object_get(*(VentureEntity *const *)b, "position", &right, NULL);
	return left < right ? -1 : (left > right ? 1 : 0);
}

static VentureReportResult *
engagement(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	gint64 org = report_org(context, options);
	g_autoptr(GPtrArray) sequences = report_rows(context, VENTURE_TYPE_SEQUENCE, org, NULL, NULL, error);
	g_autoptr(GPtrArray) steps = NULL;
	g_autoptr(GPtrArray) deliveries = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) enrollments = NULL;
	g_autoptr(GHashTable) opened = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) clicked = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) by_enrollment = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GHashTable) last_sent = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	g_autoptr(VentureReportResult) result = venture_report_result_new("Sequence engagement", period);
	guint i, j, k;
	if (sequences == NULL)
		return NULL;
	steps = report_rows(context, VENTURE_TYPE_SEQUENCE_STEP, org, NULL, NULL, error);
	if (steps == NULL)
		return NULL;
	g_ptr_array_sort(steps, by_position);
	deliveries = report_rows(context, VENTURE_TYPE_SEQUENCE_DELIVERY, org, "scheduled-at", period, error);
	if (deliveries == NULL)
		return NULL;
	events = report_rows(context, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT, org, NULL, NULL, error);
	if (events == NULL)
		return NULL;
	enrollments = report_rows(context, VENTURE_TYPE_SEQUENCE_ENROLLMENT, org, NULL, NULL, error);
	if (enrollments == NULL)
		return NULL;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		gint64 *delivery_id = g_new(gint64, 1);
		gint kind;
		g_object_get(event, "kind", &kind, "delivery-id", delivery_id, NULL);
		g_hash_table_add(kind == 0 ? opened : clicked, delivery_id);
	}
	for (i = 0; i < enrollments->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(enrollments, i);
		gint64 *enrollment_id = g_new(gint64, 1);
		*enrollment_id = venture_entity_get_id(row);
		g_hash_table_insert(by_enrollment, enrollment_id, row);
	}
	/* The reply is answered to the newest sent delivery of its enrollment;
	 * the enrollment's current step already points at the one scheduled next. */
	for (i = 0; i < deliveries->len; i++)
	{
		VentureEntity *delivery = g_ptr_array_index(deliveries, i);
		gint64 *enrollment_id = g_new(gint64, 1);
		gint64 *delivery_id;
		gint state;
		g_object_get(delivery, "state", &state, "enrollment-id", enrollment_id, NULL);
		delivery_id = g_hash_table_lookup(last_sent, enrollment_id);
		if (state != 1 || (delivery_id != NULL && *delivery_id > venture_entity_get_id(delivery)))
		{
			g_free(enrollment_id);
			continue;
		}
		delivery_id = g_new(gint64, 1);
		*delivery_id = venture_entity_get_id(delivery);
		g_hash_table_insert(last_sent, enrollment_id, delivery_id);
	}
	venture_report_result_add_column(result, "sequence", "Sequence", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "step", "Step", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "subject", "Subject", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "sent", "Sent", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "opened", "Opened", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "clicked", "Clicked", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "replies", "Replies", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "open_rate", "Open rate", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "click_rate", "Click rate", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "reply_rate", "Reply rate", VENTURE_REPORT_COLUMN_PERCENT);
	for (i = 0; i < sequences->len; i++)
	{
		VentureEntity *sequence = g_ptr_array_index(sequences, i);
		g_autofree gchar *name = NULL;
		g_object_get(sequence, "name", &name, NULL);
		for (j = 0; j < steps->len; j++)
		{
			VentureEntity *step = g_ptr_array_index(steps, j);
			g_autofree gchar *subject = NULL;
			gint64 sequence_id, position, sent = 0, opens = 0, clicks = 0, replies = 0;
			gint channel;
			g_object_get(step, "sequence-id", &sequence_id, "position", &position, "subject", &subject, "channel", &channel, NULL);
			if (sequence_id != venture_entity_get_id(sequence) || channel != 0)
				continue;
			for (k = 0; k < deliveries->len; k++)
			{
				VentureEntity *delivery = g_ptr_array_index(deliveries, k);
				VentureEntity *enrollment;
				gint64 step_id, delivery_id, enrollment_id;
				gint state;
				g_object_get(delivery, "step-id", &step_id, "state", &state, "enrollment-id", &enrollment_id, NULL);
				delivery_id = venture_entity_get_id(delivery);
				if (step_id != venture_entity_get_id(step) || state != 1)
					continue;
				sent++;
				opens += g_hash_table_contains(opened, &delivery_id);
				clicks += g_hash_table_contains(clicked, &delivery_id);
				enrollment = g_hash_table_lookup(by_enrollment, &enrollment_id);
				if (enrollment != NULL)
				{
					g_autofree gchar *reason = NULL;
					const gint64 *last = g_hash_table_lookup(last_sent, &enrollment_id);
					g_object_get(enrollment, "exit-reason", &reason, NULL);
					replies += g_strcmp0(reason, "reply") == 0 && last != NULL && *last == delivery_id;
				}
			}
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "sequence", name);
			venture_report_result_set_number(result, "step", position);
			venture_report_result_set_text(result, "subject", subject);
			venture_report_result_set_number(result, "sent", sent);
			venture_report_result_set_number(result, "opened", opens);
			venture_report_result_set_number(result, "clicked", clicks);
			venture_report_result_set_number(result, "replies", replies);
			venture_report_result_set_number(result, "open_rate", sent > 0 ? 100 * opens / sent : 0);
			venture_report_result_set_number(result, "click_rate", sent > 0 ? 100 * clicks / sent : 0);
			venture_report_result_set_number(result, "reply_rate", sent > 0 ? 100 * replies / sent : 0);
		}
	}
	return g_steal_pointer(&result);
}

void
venture_sequences_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "sequence_performance",
		"Sequence performance", "Current outcomes of enrollments started during the selected period", performance)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "sequence_failures",
		"Sequence failures", "Failed deliveries scheduled during the selected period", failures)));
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "sequence_engagement",
		"Sequence engagement", "Sent, opened, clicked and replied counts and rates per step for deliveries scheduled in the period", engagement)));
}
