/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_activity(VentureCli *cli, gchar **args, GError **error)
{
	g_autofree gchar *path = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonNode) body = NULL;
	JsonObject *object;
	if (args[1] == NULL || args[2] == NULL)
		goto usage;
	if (g_str_equal(args[1], "list"))
	{
		if (!(g_str_equal(args[2], "mine") || g_str_equal(args[2], "overdue") || g_str_equal(args[2], "today")))
			goto usage;
		path = g_strdup_printf("/api/v1/activities?queue=%s", args[2]);
		node = venture_cli_request(cli, "GET", path, NULL, error);
	}
	else
	{
		gchar *end;
		gint64 id = g_ascii_strtoll(args[2], &end, 10);
		const gchar *value = args[3];
		if (id <= 0 || *end || !g_str_equal(args[1], "complete"))
			goto usage;
		if (value != NULL && !g_str_has_prefix(value, "outcome="))
			goto usage;
		body = json_node_new(JSON_NODE_OBJECT);
		object = json_object_new();
		json_node_take_object(body, object);
		json_object_set_string_member(object, "outcome", value ? value + strlen("outcome=") : "");
		path = g_strdup_printf("/api/v1/activities/%" G_GINT64_FORMAT "/complete", id);
		node = venture_cli_request(cli, "POST", path, body, error);
	}
	if (node == NULL)
		return -1;
	venture_cli_output(cli, node);
	return 0;
usage:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: venturectl activity complete ID outcome=... | activity list mine|overdue|today");
	return -1;
}
