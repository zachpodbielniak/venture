/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_reconcile(VentureCli *cli, gchar **args, const gchar *matcher, gint threshold, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	gchar *end = NULL;
	gint64 id;
	JsonArray *suggestions;
	guint i;
	if (g_strv_length(args) != 4 || g_strcmp0(args[1], "suggest") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Usage: venturectl reconcile suggest TYPE ID [--matcher NAME] [--threshold N]"); return -1;
	}
	id = g_ascii_strtoll(args[3], &end, 10);
	if (id <= 0 || end == args[3] || *end != '\0' || threshold < 0 || threshold > 100)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Positive ID and threshold from 0 through 100 required"); return -1;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type"); json_builder_add_string_value(builder, args[2]);
	json_builder_set_member_name(builder, "id"); json_builder_add_int_value(builder, id);
	json_builder_set_member_name(builder, "threshold"); json_builder_add_int_value(builder, threshold);
	if (matcher != NULL) { json_builder_set_member_name(builder, "matcher"); json_builder_add_string_value(builder, matcher); }
	json_builder_end_object(builder);
	body = json_builder_get_root(builder);
	result = venture_cli_request(cli, "POST", "/api/v1/reconciliation/suggest", body, error);
	if (result == NULL) return -1;
	if (cli->format != VENTURE_OUTPUT_FORMAT_TABLE)
	{
		venture_cli_output(cli, result); return 0;
	}
	suggestions = json_object_get_array_member(json_node_get_object(result), "suggestions");
	g_print("%-5s %-20s %-8s %-10s %s\n", "RANK", "TYPE", "ID", "CONFIDENCE", "RATIONALE");
	for (i = 0; i < json_array_get_length(suggestions); i++)
	{
		JsonObject *row = json_array_get_object_element(suggestions, i);
		g_print("%-5u %-20s %-8" G_GINT64_FORMAT " %-10" G_GINT64_FORMAT " %s\n", i + 1,
			venture_json_object_get_string(row, "type", ""), venture_json_object_get_int(row, "id", 0),
			venture_json_object_get_int(row, "confidence", 0), venture_json_object_get_string(row, "rationale", ""));
	}
	g_print("%s\n", venture_json_object_get_string(json_node_get_object(result), "note", ""));
	return 0;
}
