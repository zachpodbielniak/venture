/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../src/forms/venture-forms-private.h"
static void dashboard_forms_save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;
	file_under_default(f, record);
	if (!venture_database_save(f->database, record, NULL, &error)) g_error("Fixture save failed: %s", error->message);
}

/* The dashboard and survey must count the same answers, without combining
 * different scales or exposing a sensitive answer as a small-count statistic. */
static void test_dashboard_forms(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) form = VENTURE_ENTITY(venture_form_new());
	g_autoptr(VentureEntity) choice = VENTURE_ENTITY(venture_form_field_new());
	g_autoptr(VentureEntity) rating = VENTURE_ENTITY(venture_form_field_new());
	g_autoptr(VentureEntity) version = NULL, draft = VENTURE_ENTITY(venture_form_draft_new());
	g_autoptr(VentureDashboard) dashboard = create_dashboard(f, "Form results");
	g_autoptr(GDateTime) now = venture_time_now(), earlier = g_date_time_add_seconds(now, -10);
	g_autoptr(GError) error = NULL;
	gint64 id, org = venture_context_get_default_organization_id(f->context);
	const gchar *kinds[] = { "form_responses", "form_choices", "form_nps", "form_rating", "form_scores", "form_dropoff", NULL };
	guint i;
	(void)unused;
	g_object_set(form, "name", "Widget survey", "state", VENTURE_FORM_LIVE, "quiz-enabled", TRUE, NULL); dashboard_forms_save(f, form);
	id = venture_entity_get_id(form);
	g_object_set(choice, "form-id", id, "key", "choice", "label", "Choice", "kind", VENTURE_FORM_FIELD_SINGLE_CHOICE,
		"choices", "a | First\nb | Second", "scoring", "{\"choices\":{\"a\":2,\"b\":1}}", "position", (gint64)1, NULL); dashboard_forms_save(f, choice);
	g_object_set(rating, "form-id", id, "key", "rating", "label", "Recommendation", "kind", VENTURE_FORM_FIELD_RATING,
		"min-value", 0.0, "max-value", 10.0, "position", (gint64)2, NULL); dashboard_forms_save(f, rating);
	version = venture_forms_publish(f->database, form, NULL, &error); g_assert_no_error(error); g_assert_nonnull(version);
	g_clear_object(&form); form = venture_database_get(f->database, VENTURE_TYPE_FORM, id, &error); g_assert_no_error(error);
	for (i = 0; i < 3; i++)
	{
		g_autofree gchar *ticket = venture_forms_ticket_new_for_version(form, 1, earlier);
		g_autofree gchar *body = g_strdup_printf("choice=%s&rating=%d&%s=%s", i == 1 ? "b" : "a", i == 0 ? 0 : (i == 1 ? 7 : 10), VENTURE_FORMS_TICKET, ticket);
		g_autoptr(GHashTable) answers = venture_forms_answers_from_urlencoded(body, strlen(body), &error);
		g_autoptr(VentureEntity) response = NULL;
		g_autoptr(JsonObject) errors = NULL;
		VentureFormsOutcome outcome;
		g_assert_no_error(error);
		g_assert_true(venture_forms_submit(f->database, form, answers, NULL, now, &outcome, &response, &errors, &error));
		g_assert_no_error(error); g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
	}
	g_object_set_data(G_OBJECT(draft), VENTURE_FORMS_DRAFT_WRITE, GINT_TO_POINTER(1));
	g_object_set(draft, "name", "Unfinished response", "form-id", id, "version-id", venture_entity_get_id(version), "page", (gint64)0,
		"answers", "{\"private\":\"NEVER-IN-A-WIDGET\"}", NULL); dashboard_forms_save(f, draft);
	for (i = 0; kinds[i] != NULL; i++)
	{
		g_autoptr(VentureDashboardWidget) widget = create_widget(f, dashboard, kinds[i], "record-id", id, "period", "all",
			"field", i == 1 ? "choice" : ((i == 2 || i == 3) ? "rating" : NULL), NULL);
		g_autoptr(VentureWidgetResult) result = venture_dashboard_render_widget(f->context, widget, NULL);
		JsonArray *rows;
		JsonObject *first;
		g_autofree gchar *wire = NULL;
		g_assert_null(result->error); g_assert_nonnull(result->html);
		wire = json_to_string(result->data, FALSE); g_assert_null(strstr(wire, "NEVER-IN-A-WIDGET"));
		rows = json_object_get_array_member(json_node_get_object(result->data), "rows"); g_assert_cmpuint(json_array_get_length(rows), >, 0);
		first = json_array_get_object_element(rows, 0);
		g_assert_cmpint(json_object_get_int_member(first, "first_version"), ==, 1);
		if (i == 0) g_assert_cmpint(json_object_get_int_member(first, "count"), ==, 3);
		if (i == 1) { g_assert_cmpstr(json_object_get_string_member(first, "choice_id"), ==, "a"); g_assert_cmpint(json_object_get_int_member(first, "count"), ==, 2); }
		if (i == 2) g_assert_cmpfloat(json_object_get_double_member(first, "value"), ==, 0.0);
		if (i == 3) g_assert_cmpfloat_with_epsilon(json_object_get_double_member(first, "value"), 17.0 / 3, 0.00001);
		if (i == 4) { g_assert_cmpuint(json_array_get_length(rows), ==, 2); g_assert_cmpint(json_object_get_int_member(first, "count"), ==, 1); }
		if (i == 5) g_assert_cmpint(json_object_get_int_member(first, "count"), ==, 1);
		{
			gint64 other = org + 1000;
			VentureWidgetScope scope = { &other, 1, 0, NULL, 0 };
			g_autoptr(VentureWidgetResult) denied = venture_dashboard_render_widget(f->context, widget, &scope);
			g_assert_nonnull(denied->error);
		}
	}
	/* Survey aggregation uses the same stable choice ID and known count. */
	{
		VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), "form_summary");
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(VentureReportResult) summary = NULL;
		g_autofree gchar *wire = NULL;
		g_autoptr(JsonNode) node = NULL;
		json_object_set_int_member(options, "form_id", id);
		summary = venture_report_generate(report, f->context, NULL, options, &error); g_assert_no_error(error); g_assert_nonnull(summary);
		node = venture_report_result_to_json(summary); wire = json_to_string(node, FALSE);
		g_assert_nonnull(strstr(wire, "First"));
		{
			guint row; gboolean matched = FALSE;
			for (row = 0; row < venture_report_result_get_row_count(summary); row++)
			{
				const GValue *label = venture_report_result_get_cell(summary, row, "answer");
				if (label != NULL && g_strcmp0(g_value_get_string(label), "First") == 0)
				{
					const GValue *count = venture_report_result_get_cell(summary, row, "count");
					g_assert_cmpfloat(g_value_get_double(count), ==, 2.0); matched = TRUE;
				}
			}
			g_assert_true(matched);
		}
	}
	/* A small row bound catches missing query narrowing without a huge fixture. */
	{
		gint old = venture_aggregate_get_max_rows();
		g_autoptr(JsonNode) result = NULL;
		venture_aggregate_set_max_rows(2);
		result = venture_forms_results(f->context, org, id, "form_responses", NULL, 0, NULL, &error);
		g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		venture_aggregate_set_max_rows(old);
	}
	/* Changed scales are separate segments, and an NPS request must refuse one. */
	g_object_set(rating, "min-value", 1.0, "max-value", 5.0, NULL); dashboard_forms_save(f, rating);
	g_clear_object(&version); version = venture_forms_publish(f->database, form, NULL, &error); g_assert_no_error(error);
	{
		g_autoptr(JsonNode) result = venture_forms_results(f->context, org, id, "form_nps", "rating", 0, NULL, &error);
		g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
		result = venture_forms_results(f->context, org, id, "form_rating", "rating", 0, NULL, &error);
		g_assert_no_error(error); g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result), "rows")), ==, 2);
	}
	{
		g_autoptr(JsonNode) selected = venture_forms_results(f->context, org, id, "form_nps", "rating", 1, NULL, &error);
		g_assert_no_error(error); g_assert_nonnull(selected);
		g_assert_cmpfloat(json_object_get_double_member(json_array_get_object_element(json_object_get_array_member(json_node_get_object(selected), "rows"), 0), "value"), ==, 0.0);
	}
	/* Relabeling keeps the stable choice and its historical counts together. */
	g_object_set(choice, "choices", "a | Renamed <choice>\nb | Second", NULL); dashboard_forms_save(f, choice);
	g_clear_object(&version); version = venture_forms_publish(f->database, form, NULL, &error); g_assert_no_error(error);
	{
		g_autoptr(VentureDashboardWidget) widget = create_widget(f, dashboard, "form_choices", "record-id", id, "field", "choice", "period", "all", NULL);
		g_autoptr(VentureWidgetResult) result = venture_dashboard_render_widget(f->context, widget, NULL);
		JsonObject *row;
		g_assert_null(result->error);
		row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(result->data), "rows"), 0);
		g_assert_cmpint(json_object_get_int_member(row, "count"), ==, 2);
		g_assert_cmpint(json_object_get_int_member(row, "last_version"), ==, 3);
		g_assert_nonnull(strstr(result->html, "Renamed &lt;choice&gt;")); g_assert_null(strstr(result->html, "Renamed <choice>"));
	}
	/* A later sensitive definition must not turn its historical participation
	 * into a widget; the selected version boundary is explicit. */
	g_object_set(rating, "sensitive", TRUE, NULL); dashboard_forms_save(f, rating);
	g_clear_object(&version); version = venture_forms_publish(f->database, form, NULL, &error); g_assert_no_error(error);
	{
		g_autoptr(JsonNode) refused = venture_forms_results(f->context, org, id, "form_rating", "rating", 0, NULL, &error);
		g_assert_null(refused); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	}

	{
		g_autoptr(VentureDashboardWidget) widget = create_widget(f, dashboard, "form_responses", "record-id", id, "period", "all", NULL);
		g_autoptr(VentureWidgetResult) off = NULL;
		venture_config_set_module_enabled(f->config, "forms", FALSE);
		off = venture_dashboard_render_widget(f->context, widget, NULL);
		g_assert_nonnull(off->error); g_assert_nonnull(strstr(off->error, "forms"));
		venture_config_set_module_enabled(f->config, "forms", TRUE);
	}
	{
		g_autofree gchar *text = NULL;
		g_autoptr(JsonNode) definition = NULL;
		g_autoptr(VentureDashboard) imported = NULL;
		g_autoptr(GPtrArray) widgets = NULL;
		g_assert_true(g_file_get_contents("data/examples/form-results-dashboard.json", &text, NULL, &error));
		g_assert_no_error(error); definition = json_from_string(text, &error); g_assert_no_error(error);
		imported = venture_dashboard_import(f->context, definition, 0, NULL, &error); g_assert_no_error(error); g_assert_nonnull(imported);
		widgets = venture_dashboard_list_widgets(f->database, venture_entity_get_id(VENTURE_ENTITY(imported)), &error);
		g_assert_no_error(error); g_assert_cmpuint(widgets->len, ==, 6);
	}

}
