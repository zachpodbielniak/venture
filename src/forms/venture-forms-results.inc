/* SPDX-License-Identifier: AGPL-3.0-or-later */

static void results_row(JsonArray *rows, const gchar *key, const gchar *choice,
	const gchar *label, gint64 first, gint64 last, gint64 count, gboolean has_value, gdouble value)
{
	JsonObject *row = json_object_new();
	json_object_set_string_member(row, "key", key);
	json_object_set_string_member(row, "choice_id", choice != NULL ? choice : "");
	json_object_set_string_member(row, "label", label);
	json_object_set_int_member(row, "first_version", first);
	json_object_set_int_member(row, "last_version", last);
	json_object_set_int_member(row, "count", count);
	if (has_value) json_object_set_double_member(row, "value", value);
	json_array_add_object_element(rows, row);
}

static gboolean results_period(VentureQuery *query, const gchar *column,
	VentureDateRange *period, GError **error)
{
	GDateTime *start = period != NULL ? venture_date_range_get_start(period) : NULL;
	GDateTime *end = period != NULL ? venture_date_range_get_end(period) : NULL;
	g_autofree gchar *from = start != NULL ? venture_time_to_string(start) : NULL;
	g_autofree gchar *until = end != NULL ? venture_time_to_string(end) : NULL;
	return (from == NULL || venture_query_add_filter_string(query, column, VENTURE_FILTER_OP_GTE, from, error)) &&
		(until == NULL || venture_query_add_filter_string(query, column, VENTURE_FILTER_OP_LT, until, error));
}

/* These buckets contain only counts and structural IDs. Never return an answer
 * string, a draft payload, or even a sensitive question's participation count. */
JsonNode *
venture_forms_results(VentureContext *context, gint64 organization, gint64 form_id,
	const gchar *kind, const gchar *key, gint64 version_number, VentureDateRange *period, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) versions = NULL, responses = NULL;
	g_autoptr(JsonObject) result = json_object_new();
	g_autoptr(JsonArray) rows = json_array_new();
	g_autoptr(JsonNode) node = NULL;
	gboolean choices = g_strcmp0(kind, "form_choices") == 0;
	gboolean rating = g_strcmp0(kind, "form_rating") == 0;
	gboolean nps = g_strcmp0(kind, "form_nps") == 0;
	gboolean counts = g_strcmp0(kind, "form_responses") == 0;
	gboolean scores = g_strcmp0(kind, "form_scores") == 0;
	gboolean dropoff = g_strcmp0(kind, "form_dropoff") == 0;
	gboolean found = FALSE;
	guint i, j;
	gint bound = venture_aggregate_get_max_rows();
	if (!venture_context_module_enabled(context, "forms"))
	{ g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Forms module is off"); return NULL; }
	if ((!choices && !rating && !nps && !counts && !scores && !dropoff) || version_number < 0)
	{ venture_set_error_validation(error, "Form results", "choose a results kind and a nonnegative version number"); return NULL; }
	if (organization <= 0) organization = venture_context_get_default_organization_id(context);
	form = venture_database_get(database, VENTURE_TYPE_FORM, form_id, NULL);
	if (form == NULL || venture_entity_get_organization_id(form) != organization)
	{ g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found"); return NULL; }
	query = venture_query_new(VENTURE_TYPE_FORM_VERSION);
	venture_query_set_organization(query, organization);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
	if (version_number > 0) venture_query_add_filter_int(query, "number", VENTURE_FILTER_OP_EQ, version_number, NULL);
	venture_query_add_order(query, "number", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, bound + 1);
	versions = venture_database_find(database, query, error); if (versions == NULL) return NULL;
	if (versions->len > (guint)bound) goto too_many;
	g_clear_object(&query);
	query = venture_query_new(dropoff ? VENTURE_TYPE_FORM_DRAFT_RECORD : VENTURE_TYPE_FORM_SUBMISSION);
	venture_query_set_organization(query, organization);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
	if (version_number > 0)
	{
		if (versions->len == 0) { venture_set_error_validation(error, "Version", "no published version has that number"); return NULL; }
		venture_query_add_filter_int(query, dropoff ? "version-id" : "version-number", VENTURE_FILTER_OP_EQ,
			dropoff ? venture_entity_get_id(g_ptr_array_index(versions, 0)) : version_number, NULL);
	}
	if (!results_period(query, dropoff ? "created-at" : "submitted-at", period, error)) return NULL;
	venture_query_set_limit(query, bound + 1);
	responses = venture_database_find(database, query, error); if (responses == NULL) return NULL;
	if (responses->len > (guint)bound) goto too_many;
	json_object_set_int_member(result, "form_id", form_id);
	json_object_set_string_member(result, "kind", kind);
	json_object_set_int_member(result, "responses", dropoff ? 0 : responses->len);
	if (dropoff) json_object_set_string_member(result, "basis", "Retained unfinished drafts by current page; not a historical visitor funnel");
	if (counts || scores || dropoff)
	{
		g_autoptr(GHashTable) buckets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
		g_autoptr(GList) names = NULL;
		GList *item;
		for (i = 0; i < responses->len; i++)
		{
			VentureEntity *response = g_ptr_array_index(responses, i);
			gint64 number = venture_forms_get_int(response, dropoff ? "version-id" : "version-number");
			g_autofree gchar *dimension = NULL, *bucket_key = NULL;
			JsonObject *bucket;
			if (dropoff)
			{
				gint64 id = number; number = 0;
				for (j = 0; j < versions->len; j++)
					if (venture_entity_get_id(g_ptr_array_index(versions, j)) == id) number = venture_forms_get_int(g_ptr_array_index(versions, j), "number");
				dimension = g_strdup_printf("Page %" G_GINT64_FORMAT, venture_forms_get_int(response, "page") + 1);
			}
			else if (scores)
			{
				if (!venture_forms_get_bool(response, "scored")) continue;
				dimension = g_strdup_printf("%" G_GINT64_FORMAT, venture_forms_get_int(response, "score"));
			}
			else
			{
				g_autoptr(GDateTime) at = NULL;
				g_object_get(response, "submitted-at", &at, NULL);
				if (at == NULL) continue;
				dimension = g_date_time_format(at, "%Y-%m-%d");
			}
			bucket_key = g_strdup_printf("%020" G_GINT64_FORMAT ":%s", number, dimension);
			bucket = g_hash_table_lookup(buckets, bucket_key);
			if (bucket == NULL)
			{
				bucket = json_object_new();
				json_object_set_string_member(bucket, "dimension", dimension);
				json_object_set_int_member(bucket, "version", number);
				json_object_set_int_member(bucket, "count", 0);
				g_hash_table_insert(buckets, g_steal_pointer(&bucket_key), bucket);
			}
			json_object_set_int_member(bucket, "count", json_object_get_int_member(bucket, "count") + 1);
		}
		names = g_hash_table_get_keys(buckets); names = g_list_sort(names, (GCompareFunc)g_strcmp0);
		for (item = names; item != NULL; item = item->next)
		{
			JsonObject *bucket = g_hash_table_lookup(buckets, item->data);
			gint64 number = json_object_get_int_member(bucket, "version");
			const gchar *dimension = json_object_get_string_member(bucket, "dimension");
			results_row(rows, "", dimension, dimension, number, number,
				json_object_get_int_member(bucket, "count"), FALSE, 0);
		}
	}
	else
	{
		/* Identical adjacent definitions share a segment, just as the survey
		 * report does. Stable keys and choice IDs outlive presentation labels. */
		g_autoptr(GPtrArray) definitions = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
		g_autoptr(GPtrArray) segments = g_ptr_array_new_with_free_func(summary_segment_free);
		SummarySegment *previous = NULL;
		for (i = 0; i < versions->len; i++)
		{
			VentureEntity *version = g_ptr_array_index(versions, i);
			g_autofree gchar *definition = venture_forms_get_string(version, "definition");
			g_autofree gchar *meaning = NULL;
			GPtrArray *fields = venture_forms_definition_from_json(definition, error);
			const VentureFormsField *field;
			gint64 number = venture_forms_get_int(version, "number");
			if (fields == NULL) return NULL;
			g_ptr_array_add(definitions, fields);
			field = key != NULL ? venture_forms_definition_find(fields, key) : NULL;
			if (field == NULL) { previous = NULL; continue; }
			found = TRUE;
			if (field->sensitive || (choices && field->kind != VENTURE_FORM_FIELD_SINGLE_CHOICE && field->kind != VENTURE_FORM_FIELD_MULTIPLE_CHOICE) ||
			    ((rating || nps) && field->kind != VENTURE_FORM_FIELD_RATING))
			{ venture_set_error_validation(error, "Question", "select a nonsensitive choice question or rating scale for this widget"); return NULL; }
			if (nps)
			{
				gint64 low, high; venture_forms_rating_bounds(field, &low, &high);
				if (low != 0 || high != 10)
				{ venture_set_error_validation(error, "NPS", "requires a 0-10 rating scale in every selected version"); return NULL; }
			}
			meaning = summary_meaning(field);
			if (previous == NULL || g_strcmp0(previous->meaning, meaning) != 0)
			{
				previous = g_new0(SummarySegment, 1); previous->key = g_strdup(key);
				previous->meaning = g_strdup(meaning); previous->first = number;
				previous->choices = g_ptr_array_new_with_free_func(venture_forms_choice_free);
				g_ptr_array_add(segments, previous);
			}
			previous->last = number; previous->field = (VentureFormsField *)field;
			summary_add_choices(previous, field);
		}
		if (!found) { venture_set_error_validation(error, "Question", "select a published question by its stable key"); return NULL; }
		for (i = 0; i < segments->len; i++)
		{
			SummarySegment *segment = g_ptr_array_index(segments, i);
			g_autoptr(GPtrArray) asked = summary_in_range(responses, segment->first, segment->last);
			if (choices)
			{
				for (j = 0; j < segment->choices->len; j++)
				{
					VentureFormsChoice *choice = g_ptr_array_index(segment->choices, j);
					results_row(rows, key, choice->id, choice->label, segment->first, segment->last,
						summary_count(asked, segment->field, choice->id), FALSE, 0);
				}
			}
			else
			{
				gdouble value = 0;
				gint64 answered = 0;
				summary_average(asked, segment->field, &value, &answered);
				if (nps)
				{
					gint64 promoters = summary_count(asked, segment->field, "9") + summary_count(asked, segment->field, "10"), detractors = 0;
					for (j = 0; j <= 6; j++) { g_autofree gchar *id = g_strdup_printf("%u", j); detractors += summary_count(asked, segment->field, id); }
					value = answered > 0 ? 100.0 * (promoters - detractors) / answered : 0;
				}
				results_row(rows, key, NULL, segment->field->label, segment->first, segment->last, answered, answered > 0, value);
			}
		}
	}
	json_object_set_array_member(result, "rows", g_steal_pointer(&rows));
	node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, g_steal_pointer(&result));
	return g_steal_pointer(&node);
too_many:
	venture_set_error_validation(error, "Form results", "more than %d records; narrow the period or version", bound);
	return NULL;
}
