/* SPDX-License-Identifier: AGPL-3.0-or-later */

static VentureWidgetResult *
venture_widget_kind_forms(VentureContext *context, VentureDashboardWidget *widget,
	const VentureWidgetScope *scope, gpointer user_data, GError **error)
{
	g_autoptr(VentureWidgetResult) result = venture_widget_result_new();
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonNode) options = venture_widget_get_options(widget, error), statistics = NULL;
	g_autoptr(JsonObject) data = json_object_new();
	g_autoptr(JsonArray) output = json_array_new();
	g_autoptr(GString) html = g_string_new(NULL);
	g_autofree gchar *kind = venture_widget_get_string(widget, "kind");
	g_autofree gchar *key = venture_widget_get_string(widget, "field");
	g_autofree gchar *period_text = venture_widget_get_string(widget, "period");
	gint64 form_id = venture_widget_get_int(widget, "record-id"), organization;
	gint64 form_venture = 0;
	gint64 number = venture_widget_option_int(options, "version", 0);
	JsonObject *summary;
	JsonArray *rows;
	guint i;
	gboolean allowed = scope->organization_ids == NULL || scope->n_organizations == 0;
	(void)user_data;
	if (error != NULL && *error != NULL) return NULL;
	form = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_FORM, form_id, NULL);
	if (form == NULL) goto missing;
	organization = venture_entity_get_organization_id(form);
	g_object_get(form, "venture-id", &form_venture, NULL);
	for (i = 0; scope->organization_ids != NULL && i < scope->n_organizations; i++)
		if (scope->organization_ids[i] == organization) allowed = TRUE;
	if (!allowed || (scope->venture_id > 0 && form_venture != scope->venture_id)) goto missing;
	period = venture_context_parse_period(context, period_text, error); if (period == NULL) return NULL;
	statistics = venture_forms_results(context, organization, form_id, kind, key, number, period, error);
	if (statistics == NULL) return NULL;
	summary = json_node_get_object(statistics); rows = json_object_get_array_member(summary, "rows");
	result->title = venture_widget_get_string(widget, "title");
	if (result->title == NULL) result->title = g_strdup("Form results");
	result->link = g_strdup_printf("/reports/form_summary?form_id=%" G_GINT64_FORMAT "&period=all", form_id);
	result->link_label = g_strdup("Survey summary");
	json_object_set_int_member(data, "form_id", form_id);
	json_object_set_string_member(data, "kind", kind);
	json_object_set_int_member(data, "responses", json_object_get_int_member(summary, "responses"));
	if (json_object_has_member(summary, "basis"))
	{
		const gchar *basis = json_object_get_string_member(summary, "basis");
		g_autofree gchar *escaped = g_markup_escape_text(basis, -1);
		json_object_set_string_member(data, "basis", basis);
		g_string_append_printf(html, "<p>%s</p>", escaped);
	}
	g_string_append(html, "<table><thead><tr><th scope=\"col\">Answer</th><th scope=\"col\">Versions</th><th scope=\"col\">Count</th><th scope=\"col\">Value</th></tr></thead><tbody>");
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonNode *row_node = json_array_get_element(rows, i);
		JsonObject *row = json_node_get_object(row_node);
		g_autofree gchar *label = g_markup_escape_text(json_object_get_string_member(row, "label"), -1);
		gint64 first = json_object_get_int_member(row, "first_version"), last = json_object_get_int_member(row, "last_version");
		g_autofree gchar *span = first == last ? g_strdup_printf("%" G_GINT64_FORMAT, first) : g_strdup_printf("%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, first, last);
		g_autofree gchar *value = json_object_has_member(row, "value") ? g_strdup_printf("%.2f", json_object_get_double_member(row, "value")) : g_strdup("—");
		json_array_add_element(output, json_node_copy(row_node));
		g_string_append_printf(html, "<tr><th scope=\"row\">%s</th><td>%s</td><td>%" G_GINT64_FORMAT "</td><td>%s</td></tr>",
			label, span, json_object_get_int_member(row, "count"), value);
	}
	g_string_append(html, "</tbody></table>");
	json_object_set_array_member(data, "rows", g_steal_pointer(&output));
	result->data = json_node_new(JSON_NODE_OBJECT); json_node_take_object(result->data, g_steal_pointer(&data));
	result->html = g_string_free(g_steal_pointer(&html), FALSE);
	return g_steal_pointer(&result);
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found in this scope");
	return NULL;
}
