/*
 * venture-forms-reports.c - The survey summary
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * form_summary counts answers per question: every choice, including the
 * ones nobody picked (a zero is a finding), yes and no for a box, the
 * spread and the average of a scale, and how many answered a text
 * question. Answers are read by stable key and choice id, so relabelling
 * a choice relabels its row rather than splitting it in two.
 */

#include "venture.h"

#include <string.h>

/* Beyond this many responses in the period the summary asks for a
 * narrower one rather than holding them all in memory. */
#define FORMS_SUMMARY_MAX 50000

static void
summary_row(VentureReportResult *result, const gchar *question, const gchar *answer,
	gint64 count, gint64 total, gboolean with_average, gdouble average)
{
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "question", question);
	venture_report_result_set_text(result, "answer", answer);
	venture_report_result_set_number(result, "count", (gdouble)count);
	venture_report_result_set_number(result, "share", total > 0 ? 100.0 * (gdouble)count / (gdouble)total : 0);
	if (with_average)
		venture_report_result_set_number(result, "average", average);
}

/* A response's answers, parsed from their stored text; NULL when absent. */
static JsonNode *
summary_answers(VentureEntity *response)
{
	g_autofree gchar *text = NULL;
	JsonNode *node;

	g_object_get(response, "answers", &text, NULL);
	if (venture_string_is_empty(text))
		return NULL;
	node = json_from_string(text, NULL);
	if (NULL != node && !JSON_NODE_HOLDS_OBJECT(node))
		g_clear_pointer(&node, json_node_unref);
	return node;
}

/* How many responses gave @key the answer @id: equal to it, or containing
 * it for a multiple choice. */
static gint64
summary_count(GPtrArray *responses, const gchar *key, const gchar *id)
{
	gint64 count = 0;
	guint i;

	for (i = 0; i < responses->len; i++)
	{
		g_autoptr(JsonNode) answers = NULL;
		JsonObject *object;
		JsonNode *node;

		answers = summary_answers(g_ptr_array_index(responses, i));
		if (NULL == answers)
			continue;
		object = json_node_get_object(answers);
		if (!json_object_has_member(object, key))
			continue;
		node = json_object_get_member(object, key);
		if (NULL == id)
		{
			count++;
			continue;
		}
		if (JSON_NODE_HOLDS_ARRAY(node))
		{
			JsonArray *array = json_node_get_array(node);
			guint j;

			for (j = 0; j < json_array_get_length(array); j++)
				if (0 == g_strcmp0(json_array_get_string_element(array, j), id))
					count++;
		}
		else if (JSON_NODE_HOLDS_VALUE(node))
		{
			GType type = json_node_get_value_type(node);

			if (G_TYPE_STRING == type && 0 == g_strcmp0(json_node_get_string(node), id))
				count++;
			else if (G_TYPE_BOOLEAN == type && (json_node_get_boolean(node) ? "yes" : "no")[0] == id[0])
				count++;
			else if (G_TYPE_INT64 == type && json_node_get_int(node) == g_ascii_strtoll(id, NULL, 10))
				count++;
		}
	}
	return count;
}

/* The average of a numeric answer, over those who gave one. */
static gboolean
summary_average(GPtrArray *responses, const gchar *key, gdouble *average, gint64 *answered)
{
	gdouble total = 0;
	guint i;

	*answered = 0;
	for (i = 0; i < responses->len; i++)
	{
		g_autoptr(JsonNode) answers = NULL;
		JsonNode *node;

		answers = summary_answers(g_ptr_array_index(responses, i));
		if (NULL == answers || !json_object_has_member(json_node_get_object(answers), key))
			continue;
		node = json_object_get_member(json_node_get_object(answers), key);
		if (!JSON_NODE_HOLDS_VALUE(node))
			continue;
		if (G_TYPE_INT64 != json_node_get_value_type(node) && G_TYPE_DOUBLE != json_node_get_value_type(node))
			continue;
		total += json_node_get_double(node);
		(*answered)++;
	}
	*average = *answered > 0 ? total / (gdouble)*answered : 0;
	return *answered > 0;
}

static VentureReportResult *
summary_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(GPtrArray) responses = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *title = NULL;
	gint64 form_id = options != NULL ? venture_json_object_get_int(options, "form_id", 0) : 0;
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	gint64 total;
	guint i;

	if (0 == org)
		org = venture_context_get_default_organization_id(context);
	if (form_id <= 0)
	{
		venture_set_error_validation(error, "form_id",
			"name the form to summarise: form_id=<id>");
		return NULL;
	}
	form = venture_database_get(database, VENTURE_TYPE_FORM, form_id, NULL);
	if (NULL == form || venture_entity_get_organization_id(form) != org)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"No form #%" G_GINT64_FORMAT " in this organization", form_id);
		return NULL;
	}
	fields = venture_forms_fields(database, form, error);
	if (NULL == fields)
		return NULL;

	query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, FORMS_SUMMARY_MAX + 1);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
	if (NULL != period)
	{
		GDateTime *start = venture_date_range_get_start(period);
		GDateTime *end = venture_date_range_get_end(period);

		if (NULL != start)
		{
			g_autofree gchar *text = venture_time_to_string(start);

			if (!venture_query_add_filter_string(query, "submitted-at", VENTURE_FILTER_OP_GTE, text, error))
				return NULL;
		}
		if (NULL != end)
		{
			g_autofree gchar *text = venture_time_to_string(end);

			if (!venture_query_add_filter_string(query, "submitted-at", VENTURE_FILTER_OP_LT, text, error))
				return NULL;
		}
	}
	responses = venture_database_find(database, query, error);
	if (NULL == responses)
		return NULL;
	if (responses->len > FORMS_SUMMARY_MAX)
	{
		venture_set_error_validation(error, "period",
			"more than %d responses; choose a shorter period", FORMS_SUMMARY_MAX);
		return NULL;
	}
	total = responses->len;

	g_object_get(form, "title", &title, NULL);
	if (venture_string_is_empty(title))
	{
		g_free(title);
		g_object_get(form, "name", &title, NULL);
	}
	{
		g_autofree gchar *heading = g_strdup_printf("Summary of %s", title);

		result = venture_report_result_new(heading, period);
	}
	venture_report_result_add_column(result, "question", "Question", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "answer", "Answer", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "count", "Responses", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "share", "Share", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "average", "Average", VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *key = NULL, *label = NULL, *choices_text = NULL;
		VentureFormFieldKind kind = VENTURE_FORM_FIELD_SHORT_TEXT;

		g_object_get(field, "key", &key, "label", &label, "kind", &kind, "choices", &choices_text, NULL);
		switch (kind)
		{
		case VENTURE_FORM_FIELD_SINGLE_CHOICE:
		case VENTURE_FORM_FIELD_MULTIPLE_CHOICE:
			{
				g_auto(GStrv) lines = g_strsplit(choices_text != NULL ? choices_text : "", "\n", -1);
				guint j;

				for (j = 0; NULL != lines[j]; j++)
				{
					gchar *bar = strchr(lines[j], '|');
					g_autofree gchar *id = NULL, *shown = NULL;

					if (NULL == bar)
						continue;
					id = g_strstrip(g_strndup(lines[j], (gsize)(bar - lines[j])));
					shown = g_strstrip(g_strdup(bar + 1));
					summary_row(result, label, shown, summary_count(responses, key, id), total, FALSE, 0);
				}
			}
			break;
		case VENTURE_FORM_FIELD_CHECKBOX:
			summary_row(result, label, "Yes", summary_count(responses, key, "yes"), total, FALSE, 0);
			summary_row(result, label, "No", summary_count(responses, key, "no"), total, FALSE, 0);
			break;
		case VENTURE_FORM_FIELD_RATING:
		case VENTURE_FORM_FIELD_NUMBER:
			{
				gdouble average = 0;
				gint64 answered = 0;

				if (VENTURE_FORM_FIELD_RATING == kind)
				{
					gdouble min = 0, max = 0;
					gint64 low = 1, high = 5, step;

					g_object_get(field, "min-value", &min, "max-value", &max, NULL);
					if (min != 0 || max != 0)
					{
						low = (gint64)min;
						high = (gint64)max;
					}
					for (step = low; step <= high && step - low <= 100; step++)
					{
						g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, step);

						summary_row(result, label, id, summary_count(responses, key, id), total, FALSE, 0);
					}
				}
				summary_average(responses, key, &average, &answered);
				summary_row(result, label, "Average", answered, total, answered > 0, average);
			}
			break;
		case VENTURE_FORM_FIELD_HIDDEN:
			break;
		case VENTURE_FORM_FIELD_SHORT_TEXT:
		case VENTURE_FORM_FIELD_LONG_TEXT:
		case VENTURE_FORM_FIELD_EMAIL:
		case VENTURE_FORM_FIELD_PHONE:
		case VENTURE_FORM_FIELD_URL:
		case VENTURE_FORM_FIELD_DATE:
		default:
			summary_row(result, label, "Answered", summary_count(responses, key, NULL), total, FALSE, 0);
			break;
		}
	}

	venture_report_result_add_metric(result, venture_metric_new_count("responses", "Responses", total));
	return g_steal_pointer(&result);
}

void
venture_forms_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(
		VENTURE_DATA_CLASS_TENANT, "form_summary", "Form summary",
		"Answers to one form in the period: every choice counted, scales averaged "
		"(form_id=<id>)", summary_report)));
}
