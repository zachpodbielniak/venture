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

#include "venture-forms-private.h"

#include <string.h>

/* Beyond this many responses in the period the summary asks for a
 * narrower one rather than holding them all in memory. */
#define FORMS_SUMMARY_MAX 50000

static void
summary_row(VentureReportResult *result, const gchar *question, const gchar *versions,
	const gchar *answer, gint64 count, gint64 total, gboolean with_average, gdouble average)
{
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "question", question);
	venture_report_result_set_text(result, "versions", versions);
	venture_report_result_set_text(result, "answer", answer);
	venture_report_result_set_number(result, "count", (gdouble)count);
	venture_report_result_set_number(result, "share", total > 0 ? (gdouble)count / (gdouble)total : 0);
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

/* Borrow only one response's row objects at a time; a bounded response
 * query must not become millions of synthetic GObjects for repeated rows. */
static GPtrArray *
summary_objects(JsonNode *answers, const VentureFormsField *field)
{
	GPtrArray *objects = g_ptr_array_new();
	JsonNode *rows;
	guint i;
	if (answers == NULL) return objects;
	if (field->group_key == NULL)
	{
		g_ptr_array_add(objects, json_node_get_object(answers));
		return objects;
	}
	rows = json_object_get_member(json_node_get_object(answers), field->group_key);
	if (rows == NULL || !JSON_NODE_HOLDS_ARRAY(rows)) return objects;
	for (i = 0; i < json_array_get_length(json_node_get_array(rows)); i++)
	{
		JsonNode *row = json_array_get_element(json_node_get_array(rows), i);
		if (JSON_NODE_HOLDS_OBJECT(row)) g_ptr_array_add(objects, json_node_get_object(row));
	}
	return objects;
}

static gboolean
summary_matches(JsonNode *node, const gchar *id)
{
	if (node == NULL) return FALSE;
	if (id == NULL) return TRUE;
	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		guint i;
		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *item = json_array_get_element(array, i);
			if (JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING &&
			    g_strcmp0(json_node_get_string(item), id) == 0) return TRUE;
		}
	}
	else if (JSON_NODE_HOLDS_VALUE(node))
	{
		GType type = json_node_get_value_type(node);
		if (type == G_TYPE_STRING) return g_strcmp0(json_node_get_string(node), id) == 0;
		if (type == G_TYPE_BOOLEAN) return (json_node_get_boolean(node) ? "yes" : "no")[0] == id[0];
		if (type == G_TYPE_INT64) return json_node_get_int(node) == g_ascii_strtoll(id, NULL, 10);
	}
	return FALSE;
}

static gint64
summary_count(GPtrArray *responses, const VentureFormsField *field, const gchar *id)
{
	gint64 count = 0;
	guint i, j;
	for (i = 0; i < responses->len; i++)
	{
		g_autoptr(JsonNode) answers = summary_answers(g_ptr_array_index(responses, i));
		g_autoptr(GPtrArray) objects = summary_objects(answers, field);
		for (j = 0; j < objects->len; j++)
			if (summary_matches(json_object_get_member(g_ptr_array_index(objects, j), field->key), id)) count++;
	}
	return count;
}

static gboolean
summary_average(GPtrArray *responses, const VentureFormsField *field, gdouble *average, gint64 *answered)
{
	gdouble total = 0;
	guint i, j;
	*answered = 0;
	for (i = 0; i < responses->len; i++)
	{
		g_autoptr(JsonNode) answers = summary_answers(g_ptr_array_index(responses, i));
		g_autoptr(GPtrArray) objects = summary_objects(answers, field);
		for (j = 0; j < objects->len; j++)
		{
			JsonNode *node = json_object_get_member(g_ptr_array_index(objects, j), field->key);
			if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) continue;
			if (json_node_get_value_type(node) != G_TYPE_INT64 && json_node_get_value_type(node) != G_TYPE_DOUBLE) continue;
			total += json_node_get_double(node);
			(*answered)++;
		}
	}
	*average = *answered > 0 ? total / (gdouble)*answered : 0;
	return *answered > 0;
}

/*
 * One question as the summary counts it: a key across the run of versions
 * in which it meant the same thing. A relabelled question or choice keeps
 * its segment -- the ids are what the answers store -- but a new kind or a
 * new scale is a new question with an old key, and counting its answers
 * together with the old ones would average a 1-5 score with a 0-10 one.
 */
typedef struct
{
	gchar			*key;
	gchar			*meaning;
	gint64			 first;
	gint64			 last;
	VentureFormsField	*field;	/* as the segment's latest version asked it */
	GPtrArray		*choices;	/* every choice id seen, latest label */
} SummarySegment;

static void
summary_segment_free(gpointer data)
{
	SummarySegment *segment = data;

	g_free(segment->key);
	g_free(segment->meaning);
	g_clear_pointer(&segment->choices, g_ptr_array_unref);
	g_free(segment);
}

/* What a question's answers mean: its kind, and its scale where it has one. */
static gchar *
summary_meaning(const VentureFormsField *field)
{
	if (VENTURE_FORM_FIELD_RATING == field->kind)
	{
		gint64 low, high;

		venture_forms_rating_bounds(field, &low, &high);
		return g_strdup_printf("%s:rating:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, field->group_key ? field->group_key : "", low, high);
	}
	return g_strdup_printf("%s:%s", field->group_key ? field->group_key : "", venture_forms_kind_nick(field->kind));
}

static void
summary_add_choices(SummarySegment *segment, const VentureFormsField *field)
{
	guint i, j;

	for (i = 0; i < field->choices->len; i++)
	{
		VentureFormsChoice *choice = g_ptr_array_index(field->choices, i);
		gboolean found = FALSE;

		for (j = 0; j < segment->choices->len; j++)
		{
			VentureFormsChoice *seen = g_ptr_array_index(segment->choices, j);

			if (0 == g_strcmp0(seen->id, choice->id))
			{
				/* The latest label wins; the id is what counts. */
				g_free(seen->label);
				seen->label = g_strdup(choice->label);
				found = TRUE;
			}
		}
		if (!found)
		{
			VentureFormsChoice *copy = g_new0(VentureFormsChoice, 1);

			copy->id = g_strdup(choice->id);
			copy->label = g_strdup(choice->label);
			g_ptr_array_add(segment->choices, copy);
		}
	}
}

/* Responses answered on a version in [first, last]. A response with no
 * version predates versions and counts with the first. */
static GPtrArray *
summary_in_range(GPtrArray *responses, gint64 first, gint64 last)
{
	GPtrArray *subset = g_ptr_array_new();
	guint i;

	for (i = 0; i < responses->len; i++)
	{
		VentureEntity *response = g_ptr_array_index(responses, i);
		gint64 number = venture_forms_get_int(response, "version-number");

		if (number <= 0)
			number = first <= 1 ? first : 0;
		if (number >= first && number <= last)
			g_ptr_array_add(subset, response);
	}
	return subset;
}

static VentureReportResult *
summary_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(GPtrArray) responses = NULL;
	g_autoptr(GPtrArray) versions = NULL;
	g_autoptr(GPtrArray) definitions = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	g_autoptr(GPtrArray) segments = g_ptr_array_new_with_free_func(summary_segment_free);
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *title = NULL;
	gint64 form_id = options != NULL ? venture_json_object_get_int(options, "form_id", 0) : 0;
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i, j;

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

	/* --- The versions, oldest first; the draft if there are none --- */

	query = venture_query_new(VENTURE_TYPE_FORM_VERSION);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
	venture_query_add_order(query, "number", VENTURE_SORT_ASCENDING, NULL);
	versions = venture_database_find(database, query, error);
	if (NULL == versions)
		return NULL;
	g_clear_object(&query);
	if (0 == versions->len)
	{
		GPtrArray *draft = venture_forms_definition_from_records(database, form, error);

		if (NULL == draft)
			return NULL;
		g_ptr_array_add(definitions, draft);
	}
	for (i = 0; i < versions->len; i++)
	{
		g_autofree gchar *text = venture_forms_get_string(g_ptr_array_index(versions, i), "definition");
		GPtrArray *fields = venture_forms_definition_from_json(text, error);

		if (NULL == fields)
			return NULL;
		g_ptr_array_add(definitions, fields);
	}

	/* --- Segments: a key while it keeps its meaning --- */

	for (i = 0; i < definitions->len; i++)
	{
		GPtrArray *fields = g_ptr_array_index(definitions, i);
		gint64 number = versions->len > 0 ? venture_forms_get_int(g_ptr_array_index(versions, i), "number") : 1;

		venture_forms_localize(fields, options != NULL ? json_object_get_string_member_with_default(options, "language", NULL) : NULL);
		for (j = 0; j < fields->len; j++)
		{
			VentureFormsField *field = g_ptr_array_index(fields, j);
			g_autofree gchar *meaning = summary_meaning(field);
			SummarySegment *segment = NULL;
			guint k;

			if (VENTURE_FORM_FIELD_HIDDEN == field->kind || VENTURE_FORM_FIELD_PAGE_BREAK == field->kind)
				continue;
			for (k = segments->len; k > 0; k--)
			{
				SummarySegment *candidate = g_ptr_array_index(segments, k - 1);

				if (0 == g_strcmp0(candidate->key, field->key))
				{
					/* Only a run: a key absent from a version and back
					 * again is still the same question if it means the
					 * same thing. */
					if (0 == g_strcmp0(candidate->meaning, meaning))
						segment = candidate;
					break;
				}
			}
			if (NULL == segment)
			{
				segment = g_new0(SummarySegment, 1);
				segment->key = g_strdup(field->key);
				segment->meaning = g_steal_pointer(&meaning);
				segment->first = number;
				segment->choices = g_ptr_array_new_with_free_func(venture_forms_choice_free);
				g_ptr_array_add(segments, segment);
			}
			segment->last = number;
			segment->field = field;
			summary_add_choices(segment, field);
		}
	}


	/* --- The responses in the period --- */

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

	title = venture_forms_get_string(form, "title");
	if (venture_string_is_empty(title))
	{
		g_free(title);
		title = venture_forms_get_string(form, "name");
	}
	{
		g_autofree gchar *heading = g_strdup_printf("Summary of %s", title);

		result = venture_report_result_new(heading, period);
	}
	venture_report_result_add_column(result, "question", "Question", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "versions", "Versions", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "answer", "Answer", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "count", "Responses", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "share", "Share", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(result, "average", "Average", VENTURE_REPORT_COLUMN_NUMBER);

	for (i = 0; i < segments->len; i++)
	{
		SummarySegment *segment = g_ptr_array_index(segments, i);
		const VentureFormsField *field = segment->field;
		g_autoptr(GPtrArray) asked = summary_in_range(responses, segment->first, segment->last);
		g_autofree gchar *span = segment->first == segment->last ?
			g_strdup_printf("%" G_GINT64_FORMAT, segment->first) :
			g_strdup_printf("%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, segment->first, segment->last);
		gint64 total = 0;
		gint64 not_shown = 0;
		for (j = 0; j < asked->len; j++)
		{
			VentureEntity *response = g_ptr_array_index(asked, j);
			g_autofree gchar *text = venture_forms_get_string(response, "not-shown");
			g_autoptr(JsonNode) omitted = text != NULL ? json_from_string(text, NULL) : NULL;
			g_autoptr(JsonNode) answers = summary_answers(response);
			g_autoptr(GPtrArray) objects = summary_objects(answers, field);
			guint k, length = field->group_key != NULL ? objects->len : 1;
			total += length;
			for (k = 0; k < length; k++)
			{
				g_autofree gchar *key = field->group_key != NULL ?
					g_strdup_printf("%s[%u][%s]", field->group_key, k, field->key) : g_strdup(field->key);
				if (omitted != NULL && JSON_NODE_HOLDS_OBJECT(omitted) &&
				    json_object_has_member(json_node_get_object(omitted), key)) not_shown++;
			}
		}
		if (field->group_key != NULL && !field->sensitive)
			summary_row(result, field->label, span, "Rows", total, total, FALSE, 0);

		if (!field->sensitive)
		{
			summary_row(result, field->label, span, "Not shown", not_shown, total, FALSE, 0);
			summary_row(result, field->label, span, "Not answered", MAX((gint64)0, total - not_shown - summary_count(asked, field, NULL)), total, FALSE, 0);
		}


		switch (field->kind)
		{
		case VENTURE_FORM_FIELD_SINGLE_CHOICE:
		case VENTURE_FORM_FIELD_MULTIPLE_CHOICE:
			for (j = 0; j < segment->choices->len; j++)
			{
				VentureFormsChoice *choice = g_ptr_array_index(segment->choices, j);

				summary_row(result, field->label, span, choice->label,
				            summary_count(asked, field, choice->id), total, FALSE, 0);
			}
			break;
		case VENTURE_FORM_FIELD_CHECKBOX:
			summary_row(result, field->label, span, "Yes", summary_count(asked, field, "yes"), total, FALSE, 0);
			summary_row(result, field->label, span, "No", summary_count(asked, field, "no"), total, FALSE, 0);
			break;
		case VENTURE_FORM_FIELD_RATING:
		case VENTURE_FORM_FIELD_NUMBER:
			{
				gdouble average = 0;
				gint64 answered = 0;

				if (VENTURE_FORM_FIELD_RATING == field->kind)
				{
					gint64 low, high, step;

					venture_forms_rating_bounds(field, &low, &high);
					for (step = low; step <= high && step - low <= 100; step++)
					{
						g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, step);

						summary_row(result, field->label, span, id,
						            summary_count(asked, field, id), total, FALSE, 0);
					}
				}
				summary_average(asked, field, &average, &answered);
				summary_row(result, field->label, span, "Average", answered, total, answered > 0, average);
			}
			break;
		case VENTURE_FORM_FIELD_SHORT_TEXT:
		case VENTURE_FORM_FIELD_LONG_TEXT:
		case VENTURE_FORM_FIELD_EMAIL:
		case VENTURE_FORM_FIELD_PHONE:
		case VENTURE_FORM_FIELD_URL:
		case VENTURE_FORM_FIELD_DATE:
		case VENTURE_FORM_FIELD_HIDDEN:
		default:
			summary_row(result, field->label, span, "Answered",
			            summary_count(asked, field, NULL), total, FALSE, 0);
			break;
		}
	}

	venture_report_result_add_metric(result, venture_metric_new_count("responses", "Responses", responses->len));
	return g_steal_pointer(&result);
}

void
venture_forms_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(
		VENTURE_DATA_CLASS_TENANT, "form_summary", "Form summary",
		"Answers to one form in the period: every choice counted, scales averaged, a question "
		"split where a new version changed what it asked (form_id=<id>)", summary_report)));
}
