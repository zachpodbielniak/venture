/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* venturectl money calendar --from DATE --to DATE [--kind K] [--as-of DATE]:
 * the money_calendar report over the API, printed as an agenda. -f json
 * prints the report as the server sent it. */
static gint
venture_cli_command_money(VentureCli *cli, gchar **args, const gchar *from, const gchar *to,
	const gchar *kind, const gchar *as_of, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) path = NULL;
	JsonArray *rows;
	const gchar *current_day = NULL;
	guint i;
	if (g_strv_length(args) < 2 || g_strcmp0(args[1], "calendar") != 0 ||
		venture_string_is_empty(from) || venture_string_is_empty(to))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: money calendar --from DATE --to DATE [--kind recurring|bill|invoice|dunning|payroll|tax] [--as-of DATE]");
		return -1;
	}
	path = g_string_new("/api/v1/reports/money_calendar?period=");
	g_string_append_uri_escaped(path, from, NULL, FALSE);
	g_string_append(path, "..");
	g_string_append_uri_escaped(path, to, NULL, FALSE);
	if (!venture_string_is_empty(kind))
	{
		g_string_append(path, "&kind=");
		g_string_append_uri_escaped(path, kind, NULL, FALSE);
	}
	if (!venture_string_is_empty(as_of))
	{
		g_string_append(path, "&as_of=");
		g_string_append_uri_escaped(path, as_of, NULL, FALSE);
	}
	node = venture_cli_request(cli, "GET", path->str, NULL, error);
	if (node == NULL)
		return -1;
	if (cli->format == VENTURE_OUTPUT_FORMAT_JSON || cli->format == VENTURE_OUTPUT_FORMAT_YAML)
	{
		venture_cli_output(cli, node);
		return 0;
	}
	rows = json_object_get_array_member(json_node_get_object(node), "rows");
	if (rows == NULL || json_array_get_length(rows) == 0)
	{
		g_print("Nothing dated between %s and %s.\n", from, to);
		return 0;
	}
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		const gchar *day = venture_json_object_get_string(row, "date", "");
		const gchar *status = venture_json_object_get_string(row, "status", "");
		if (g_strcmp0(day, current_day) != 0)
		{
			g_print("%s%s  %s  net %s\n", current_day != NULL ? "\n" : "", day,
				venture_json_object_get_string(row, "week", ""), venture_json_object_get_string(row, "day_net", ""));
			current_day = day;
		}
		g_print("  %-10s %-3s %14s  %s%s%s%s\n", venture_json_object_get_string(row, "kind", ""),
			venture_json_object_get_string(row, "direction", ""),
			venture_json_object_get_string(row, "amount_formatted", venture_json_object_get_string(row, "amount", "")),
			venture_json_object_get_string(row, "title", ""),
			g_strcmp0(status, "due") == 0 ? "" : "  [", g_strcmp0(status, "due") == 0 ? "" : status,
			g_strcmp0(status, "due") == 0 ? "" : "]");
	}
	return 0;
}
