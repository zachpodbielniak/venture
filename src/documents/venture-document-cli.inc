/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_compose(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	const gchar *kind;
	const gchar *json;
	g_autofree gchar *path = NULL;
	if (args[1] == NULL || args[2] == NULL || (g_strcmp0(args[1], "invoice") != 0 && g_strcmp0(args[1], "quote") != 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: compose invoice|quote JSON");
		return -1;
	}
	kind = args[1];
	json = args[2];
	if (!json_parser_load_from_data(parser, json, -1, error))
		return -1;
	body = json_node_copy(json_parser_get_root(parser));
	path = g_strdup_printf("/api/v1/%ss/compose", kind);
	result = venture_cli_request(cli, "POST", path, body, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}
