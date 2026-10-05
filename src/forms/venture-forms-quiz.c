/*
 * venture-forms-quiz.c - Published scoring declarations and final results
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture-forms-private.h"
#include <math.h>
#include <string.h>

#define FORMS_SCORE_POINTS_MAX 1000000
#define FORMS_SCORE_RULES_MAX 128

static gboolean
quiz_integer(JsonNode *node, gint64 *value)
{
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
	*value = json_node_get_int(node);
	return TRUE;
}

static gboolean
quiz_number(JsonNode *node, gdouble *value)
{
	GType type;
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
	type = json_node_get_value_type(node);
	if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE) return FALSE;
	*value = json_node_get_double(node);
	return isfinite(*value);
}

static gboolean
quiz_choice(const VentureFormsField *field, const gchar *id)
{
	guint i;
	for (i = 0; field->choices != NULL && i < field->choices->len; i++)
		if (g_strcmp0(((VentureFormsChoice *)g_ptr_array_index(field->choices, i))->id, id) == 0) return TRUE;
	return FALSE;
}

static gboolean
quiz_keys(JsonObject *object, const gchar *const *keys)
{
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *value;
	json_object_iter_init(&iter, object);
	while (json_object_iter_next(&iter, &name, &value))
	{
		(void)value;
		if (!g_strv_contains(keys, name)) return FALSE;
	}
	return TRUE;
}

gboolean
venture_forms_quiz_field_valid(const VentureFormsField *field, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object;
	JsonNode *choices, *ranges, *correct;
	guint i, j;
	if (venture_string_is_empty(field->scoring)) return TRUE;
	if (strlen(field->scoring) > 16384) goto invalid;
	node = json_from_string(field->scoring, NULL);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
	object = json_node_get_object(node);
	if (json_object_get_size(object) == 0) return TRUE;
	if (field->sensitive || !quiz_keys(object, (const gchar *const[]) { "choices", "ranges", "correct", NULL })) goto invalid;
	choices = json_object_get_member(object, "choices"); ranges = json_object_get_member(object, "ranges"); correct = json_object_get_member(object, "correct");
	if (choices != NULL)
	{
		JsonObjectIter iter;
		const gchar *id;
		JsonNode *points;
		gint64 value;
		if (!venture_forms_kind_has_choices(field->kind) || !JSON_NODE_HOLDS_OBJECT(choices) || json_object_get_size(json_node_get_object(choices)) > FORMS_SCORE_RULES_MAX) goto invalid;
		json_object_iter_init(&iter, json_node_get_object(choices));
		while (json_object_iter_next(&iter, &id, &points))
			if (!quiz_choice(field, id) || !quiz_integer(points, &value) || value < -FORMS_SCORE_POINTS_MAX || value > FORMS_SCORE_POINTS_MAX) goto invalid;
	}
	if (ranges != NULL)
	{
		JsonArray *array;
		if ((field->kind != VENTURE_FORM_FIELD_NUMBER && field->kind != VENTURE_FORM_FIELD_RATING) || !JSON_NODE_HOLDS_ARRAY(ranges)) goto invalid;
		array = json_node_get_array(ranges);
		if (json_array_get_length(array) > FORMS_SCORE_RULES_MAX) goto invalid;
		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *entry = json_array_get_element(array, i);
			JsonObject *range;
			gdouble low, high;
			gint64 points;
			if (!JSON_NODE_HOLDS_OBJECT(entry)) goto invalid;
			range = json_node_get_object(entry);
			if (!quiz_keys(range, (const gchar *const[]) { "min", "max", "points", NULL }) ||
			    !quiz_number(json_object_get_member(range, "min"), &low) || !quiz_number(json_object_get_member(range, "max"), &high) || low > high ||
			    !quiz_integer(json_object_get_member(range, "points"), &points) || points < -FORMS_SCORE_POINTS_MAX || points > FORMS_SCORE_POINTS_MAX) goto invalid;
			for (j = 0; j < i; j++)
			{
				JsonObject *prior = json_array_get_object_element(array, j);
				if (low <= json_object_get_double_member(prior, "max") && high >= json_object_get_double_member(prior, "min")) goto invalid;
			}
		}
	}
	if (correct != NULL)
	{
		JsonArray *array;
		if (!venture_forms_kind_has_choices(field->kind) || !JSON_NODE_HOLDS_ARRAY(correct)) goto invalid;
		array = json_node_get_array(correct);
		if (json_array_get_length(array) > FORMS_SCORE_RULES_MAX || (field->kind == VENTURE_FORM_FIELD_SINGLE_CHOICE && json_array_get_length(array) > 1)) goto invalid;
		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *id = json_array_get_element(array, i);
			if (!JSON_NODE_HOLDS_VALUE(id) || json_node_get_value_type(id) != G_TYPE_STRING || !quiz_choice(field, json_node_get_string(id))) goto invalid;
			for (j = 0; j < i; j++) if (g_strcmp0(json_array_get_string_element(array, j), json_node_get_string(id)) == 0) goto invalid;
		}
	}
	return TRUE;
invalid:
	venture_set_error_validation(error, "Scoring", "use bounded integer choice points or non-overlapping numeric ranges, and valid correct choice ids; sensitive questions cannot be scored");
	return FALSE;
}

static gboolean
quiz_destination(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	if (venture_string_is_empty(url)) return TRUE;
	if (strlen(url) > 4096 || strpbrk(url, "\r\n") != NULL) return FALSE;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	return uri != NULL && g_strcmp0(g_uri_get_scheme(uri), "https") == 0 && !venture_string_is_empty(g_uri_get_host(uri)) && g_uri_get_userinfo(uri) == NULL;
}

gboolean
venture_forms_quiz_validate_band(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_RESULT_BAND);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *key = venture_forms_get_string(entity, "key"), *url = venture_forms_get_string(entity, "redirect-url"), *message = venture_forms_get_string(entity, "message");
	gint64 low = venture_forms_get_int(entity, "minimum"), high = venture_forms_get_int(entity, "maximum");
	guint i;
	gboolean present = FALSE;
	(void)data;
	if (!venture_forms_key_valid(key) || low > high || !quiz_destination(url) ||
	    (message != NULL && (strlen(message) > 32768 || !g_utf8_validate(message, -1, NULL)))) goto invalid;
	if (previous != NULL)
	{
		g_autofree gchar *old = venture_forms_get_string(previous, "key");
		if (g_strcmp0(old, key) != 0 || venture_forms_get_int(previous, "form-id") != venture_forms_get_int(entity, "form-id")) goto invalid;
	}
	form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(entity, "form-id"), NULL);
	if (form == NULL || venture_entity_get_organization_id(form) != venture_entity_get_organization_id(entity)) goto invalid;
	venture_query_set_organization(query, venture_entity_get_organization_id(entity));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_set_limit(query, FORMS_SCORE_RULES_MAX + 1);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *other = venture_forms_get_string(row, "key");
		if (venture_entity_get_id(row) == venture_entity_get_id(entity)) { present = TRUE; continue; }
		if (g_strcmp0(key, other) == 0 || (low <= venture_forms_get_int(row, "maximum") && high >= venture_forms_get_int(row, "minimum"))) goto invalid;
	}
	if (!venture_entity_is_deleted(entity) && rows->len - (present ? 1 : 0) >= FORMS_SCORE_RULES_MAX) goto invalid;
	return TRUE;
invalid:
	venture_set_error_validation(error, "Result band", "use a stable unique key, ordered non-overlapping score bounds and an optional HTTPS result page in this form (at most 128 bands)");
	return FALSE;
}

JsonObject *
venture_forms_quiz(GPtrArray *fields)
{
	return fields != NULL && fields->len > 0 ? ((VentureFormsField *)g_ptr_array_index(fields, 0))->quiz : NULL;
}

gboolean
venture_forms_quiz_restore(GPtrArray *fields, JsonObject *quiz, GError **error)
{
	const gchar *flags[] = { "enabled", "show_key", "lead_input" };
	JsonNode *node;
	JsonArray *bands;
	guint i, j;
	if (!quiz_keys(quiz, (const gchar *const[]) { "enabled", "show_key", "lead_input", "bands", NULL })) goto invalid;
	for (i = 0; i < G_N_ELEMENTS(flags); i++)
	{
		node = json_object_get_member(quiz, flags[i]);
		if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN) goto invalid;
	}
	node = json_object_get_member(quiz, "bands");
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node)) goto invalid;
	bands = json_node_get_array(node);
	if (json_array_get_length(bands) > FORMS_SCORE_RULES_MAX) goto invalid;
	for (i = 0; i < json_array_get_length(bands); i++)
	{
		JsonObject *band;
		gint64 low, high;
		const gchar *strings[] = { "key", "message", "url" };
		node = json_array_get_element(bands, i);
		if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
		band = json_node_get_object(node);
		if (!quiz_keys(band, (const gchar *const[]) { "key", "message", "url", "min", "max", NULL })) goto invalid;
		for (j = 0; j < G_N_ELEMENTS(strings); j++)
		{
			node = json_object_get_member(band, strings[j]);
			if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING || strlen(json_node_get_string(node)) > 32768) goto invalid;
		}
		if (!venture_forms_key_valid(json_object_get_string_member(band, "key")) || !quiz_destination(json_object_get_string_member(band, "url")) ||
		    !quiz_integer(json_object_get_member(band, "min"), &low) || !quiz_integer(json_object_get_member(band, "max"), &high) || low > high) goto invalid;
		for (j = 0; j < i; j++)
		{
			JsonObject *previous = json_array_get_object_element(bands, j);
			if (g_strcmp0(json_object_get_string_member(previous, "key"), json_object_get_string_member(band, "key")) == 0 ||
			    (low <= json_object_get_int_member(previous, "max") && high >= json_object_get_int_member(previous, "min"))) goto invalid;
		}
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		g_clear_pointer(&field->quiz, json_object_unref);
		field->quiz = json_object_ref(quiz);
	}
	return TRUE;
invalid:
	venture_set_error_validation(error, "Quiz", "the published scoring definition is invalid");
	return FALSE;
}

gboolean
venture_forms_quiz_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	g_autoptr(JsonObject) quiz = json_object_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_RESULT_BAND);
	g_autoptr(GPtrArray) rows = NULL;
	JsonArray *bands = json_array_new();
	guint i;
	json_object_set_boolean_member(quiz, "enabled", venture_forms_get_bool(form, "quiz-enabled"));
	json_object_set_boolean_member(quiz, "show_key", venture_forms_get_bool(form, "show-answer-key"));
	json_object_set_boolean_member(quiz, "lead_input", venture_forms_get_bool(form, "score-to-lead"));
	json_object_set_array_member(quiz, "bands", bands);
	for (i = 0; i < fields->len; i++) if (!venture_forms_quiz_field_valid(g_ptr_array_index(fields, i), error)) return FALSE;
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_order(query, "minimum", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, FORMS_SCORE_RULES_MAX + 1);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	if (rows->len > FORMS_SCORE_RULES_MAX) { venture_set_error_validation(error, "Result bands", "at most 128 bands per form"); return FALSE; }
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *key = venture_forms_get_string(row, "key"), *message = venture_forms_get_string(row, "message"), *url = venture_forms_get_string(row, "redirect-url");
		JsonObject *band = json_object_new();
		json_object_set_string_member(band, "key", key);
		json_object_set_string_member(band, "message", message != NULL ? message : "");
		json_object_set_string_member(band, "url", url != NULL ? url : "");
		json_object_set_int_member(band, "min", venture_forms_get_int(row, "minimum"));
		json_object_set_int_member(band, "max", venture_forms_get_int(row, "maximum"));
		json_array_add_object_element(bands, band);
	}
	return venture_forms_quiz_restore(fields, quiz, error);
}

static gboolean
quiz_selected(JsonNode *answer, const gchar *id)
{
	guint i;
	if (answer == NULL) return FALSE;
	if (JSON_NODE_HOLDS_VALUE(answer) && json_node_get_value_type(answer) == G_TYPE_STRING) return g_strcmp0(json_node_get_string(answer), id) == 0;
	if (JSON_NODE_HOLDS_ARRAY(answer))
		for (i = 0; i < json_array_get_length(json_node_get_array(answer)); i++)
			if (quiz_selected(json_array_get_element(json_node_get_array(answer), i), id)) return TRUE;
	return FALSE;
}

static gboolean
quiz_add(gint64 *total, gint64 points, GError **error)
{
	if ((points > 0 && *total > G_MAXINT64 - points) || (points < 0 && *total < G_MININT64 - points))
	{ venture_set_error_validation(error, "Score", "the assessment exceeds the supported score range"); return FALSE; }
	*total += points;
	return TRUE;
}

gboolean
venture_forms_quiz_apply(GPtrArray *fields, JsonObject *answers, VentureEntity *response, GError **error)
{
	JsonObject *quiz = venture_forms_quiz(fields);
	gint64 total = 0;
	guint i, j;
	if (quiz == NULL || !json_object_get_boolean_member_with_default(quiz, "enabled", FALSE)) return TRUE;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		JsonNode *answer = json_object_get_member(answers, field->key);
		g_autoptr(JsonNode) root = NULL;
		JsonObject *scoring;
		if (answer == NULL || field->sensitive || field->group_boundary != VENTURE_FORMS_GROUP_NONE || venture_string_is_empty(field->scoring)) continue;
		if (!venture_forms_quiz_field_valid(field, error)) return FALSE;
		root = json_from_string(field->scoring, NULL); scoring = json_node_get_object(root);
		if (json_object_has_member(scoring, "choices"))
		{
			JsonObjectIter iter;
			const gchar *id;
			JsonNode *points;
			json_object_iter_init(&iter, json_object_get_object_member(scoring, "choices"));
			while (json_object_iter_next(&iter, &id, &points))
				if (quiz_selected(answer, id) && !quiz_add(&total, json_node_get_int(points), error)) return FALSE;
		}
		if (json_object_has_member(scoring, "ranges"))
		{
			gdouble value;
			JsonArray *ranges = json_object_get_array_member(scoring, "ranges");
			if (!quiz_number(answer, &value)) continue;
			for (j = 0; j < json_array_get_length(ranges); j++)
			{
				JsonObject *range = json_array_get_object_element(ranges, j);
				if (value >= json_object_get_double_member(range, "min") && value <= json_object_get_double_member(range, "max") &&
				    !quiz_add(&total, json_object_get_int_member(range, "points"), error)) return FALSE;
			}
		}
	}
	g_object_set(response, "scored", TRUE, "score", total, NULL);
	{
		JsonArray *bands = json_object_get_array_member(quiz, "bands");
		for (i = 0; i < json_array_get_length(bands); i++)
		{
			JsonObject *band = json_array_get_object_element(bands, i);
			if (total >= json_object_get_int_member(band, "min") && total <= json_object_get_int_member(band, "max"))
			{
				g_autofree gchar *key = g_strdup_printf("result.%s.message", json_object_get_string_member(band, "key"));
				g_object_set(response, "result-key", json_object_get_string_member(band, "key"),
					"result-message", venture_forms_text(fields, key, json_object_get_string_member(band, "message")),
					"result-url", json_object_get_string_member(band, "url"), NULL);
				break;
			}
		}
	}
	return TRUE;
}

static JsonObject *
quiz_public_values(GPtrArray *fields, JsonObject *answers)
{
	JsonObject *values = json_object_new();
	guint i, row;
	if (answers == NULL) return values;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->sensitive) continue;
		if (field->group_key == NULL)
		{
			JsonNode *value = json_object_get_member(answers, field->key);
			if (value != NULL) json_object_set_member(values, field->key, json_node_copy(value));
		}
		else
		{
			JsonNode *group = json_object_get_member(answers, field->group_key);
			if (group == NULL || !JSON_NODE_HOLDS_ARRAY(group)) continue;
			for (row = 0; row < json_array_get_length(json_node_get_array(group)); row++)
			{
				JsonNode *item = json_array_get_element(json_node_get_array(group), row), *value;
				g_autofree gchar *key = NULL;
				if (!JSON_NODE_HOLDS_OBJECT(item)) continue;
				value = json_object_get_member(json_node_get_object(item), field->key);
				if (value == NULL) continue;
				key = g_strdup_printf("%s[%u][%s]", field->group_key, row, field->key);
				json_object_set_member(values, key, json_node_copy(value));
			}
		}
	}
	return values;
}

gchar *
venture_forms_quiz_message(GPtrArray *fields, JsonObject *answers, VentureEntity *response, const gchar *success)
{
	JsonObject *quiz = venture_forms_quiz(fields);
	g_autoptr(GString) message = NULL;
	g_autofree gchar *result = NULL;
	guint i, j;
	if (response == NULL || !venture_forms_get_bool(response, "scored")) return g_strdup(success);
	message = g_string_new(success);
	g_string_append_printf(message, "\n\n%s: %" G_GINT64_FORMAT, venture_forms_text(fields, "message.score", "Score"), venture_forms_get_int(response, "score"));
	result = venture_forms_get_string(response, "result-message");
	if (!venture_string_is_empty(result)) g_string_append_printf(message, "\n%s", result);
	if (quiz != NULL && json_object_get_boolean_member_with_default(quiz, "show_key", FALSE))
	{
		g_autoptr(JsonObject) errors = json_object_new(), values = quiz_public_values(fields, answers);
		g_autoptr(JsonObject) pipe_values = answers != NULL ? venture_forms_pipe_stored_values(fields, answers) : json_object_new();
		g_autoptr(GPtrArray) expanded = venture_forms_expand_groups(fields, values, FALSE, errors);
		venture_forms_piping_apply(expanded, pipe_values);
		for (i = 0; i < expanded->len; i++)
		{
			VentureFormsField *field = g_ptr_array_index(expanded, i);
			g_autoptr(JsonNode) scoring = NULL;
			JsonArray *correct;
			if (field->sensitive || field->group_boundary != VENTURE_FORMS_GROUP_NONE || !json_object_has_member(values, field->key) || venture_string_is_empty(field->scoring)) continue;
			scoring = json_from_string(field->scoring, NULL);
			if (!json_object_has_member(json_node_get_object(scoring), "correct")) continue;
			correct = json_object_get_array_member(json_node_get_object(scoring), "correct");
			if (json_array_get_length(correct) == 0) continue;
			g_string_append_printf(message, "\n%s — %s: ", field->label, venture_forms_text(fields, "message.correct_answers", "Correct answers"));
			for (j = 0; j < json_array_get_length(correct); j++)
			{
				const gchar *id = json_array_get_string_element(correct, j);
				guint k;
				for (k = 0; k < field->choices->len; k++)
				{
					VentureFormsChoice *choice = g_ptr_array_index(field->choices, k);
					if (g_strcmp0(choice->id, id) == 0) g_string_append_printf(message, "%s%s", j > 0 ? ", " : "", choice->label);
				}
			}
		}
	}
	return g_string_free(g_steal_pointer(&message), FALSE);
}

static gint
quiz_bin_compare(gconstpointer left, gconstpointer right)
{
	const gchar *a = left, *b = right;
	gint64 av = g_ascii_strtoll(a, NULL, 10), bv = g_ascii_strtoll(b, NULL, 10);
	gint64 as = g_ascii_strtoll(strchr(a, ':') + 1, NULL, 10), bs = g_ascii_strtoll(strchr(b, ':') + 1, NULL, 10);
	if (av != bv) return av < bv ? -1 : 1;
	return as < bs ? -1 : (as > bs ? 1 : 0);
}

/* Separate version distributions avoid pretending that two different grading
 * schemes have comparable scores merely because the question keys survived. */
void
venture_forms_quiz_report(VentureReportResult *result, GPtrArray *responses)
{
	g_autoptr(GHashTable) bins = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GHashTable) totals = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	GList *keys, *item;
	guint i;
	for (i = 0; i < responses->len; i++)
	{
		VentureEntity *response = g_ptr_array_index(responses, i);
		gint64 version = venture_forms_get_int(response, "version-number");
		gint64 *count, *total;
		g_autofree gchar *key = NULL;
		if (!venture_forms_get_bool(response, "scored")) continue;
		key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT, version, venture_forms_get_int(response, "score"));
		count = g_hash_table_lookup(bins, key);
		if (count == NULL) { count = g_new0(gint64, 1); g_hash_table_insert(bins, g_strdup(key), count); }
		(*count)++;
		total = g_hash_table_lookup(totals, &version);
		if (total == NULL) { gint64 *id = g_new(gint64, 1); *id = version; total = g_new0(gint64, 1); g_hash_table_insert(totals, id, total); }
		(*total)++;
	}
	keys = g_list_sort(g_hash_table_get_keys(bins), quiz_bin_compare);
	for (item = keys; item != NULL; item = item->next)
	{
		g_auto(GStrv) parts = g_strsplit(item->data, ":", 2);
		gint64 version = g_ascii_strtoll(parts[0], NULL, 10);
		gint64 *count = g_hash_table_lookup(bins, item->data), *total = g_hash_table_lookup(totals, &version);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "question", "Assessment score");
		venture_report_result_set_text(result, "versions", parts[0]);
		venture_report_result_set_text(result, "answer", parts[1]);
		venture_report_result_set_number(result, "count", (gdouble)*count);
		venture_report_result_set_number(result, "share", (gdouble)*count / (gdouble)*total);
	}
	g_list_free(keys);
}
